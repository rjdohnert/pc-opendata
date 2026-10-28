import json
import os
import shlex
import shutil
import socket
import ssl
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == "nt", "isolated Schannel runtime coverage")
class WindowsWalletTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.environ.get("OPENDATA_TEST_WALLET_CASES"):
            raise RuntimeError("Run through Invoke-ClusterTlsTest.ps1 -Wallet")
        cls.cases = json.loads(os.environ["OPENDATA_TEST_WALLET_CASES"])
        cls.build = tempfile.TemporaryDirectory(
            prefix="windows-wallet-", dir=ROOT / "tests")
        cls.addClassCleanup(cls.build.cleanup)
        cls.work = Path(cls.build.name)
        supplied = os.environ.get("OPENDATA_TEST_BINARY")
        cls.binary = Path(supplied).resolve() if supplied else cls.work / "opendata.exe"
        if not supplied:
            configured = os.environ.get("CC")
            compiler = ([configured] if configured and Path(configured).is_file()
                        else shlex.split(configured)) if configured else [shutil.which("clang") or "cc"]
            modules = (ROOT / "src" / "modules.txt").read_text().splitlines()
            sources = [str(ROOT / "src" / name) for name in modules if name.strip()]
            result = subprocess.run(
                compiler + ["-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-DOPENDATA_TLS", "-DOPENDATA_TEST_TLS_FIXTURES"]
                + sources + ["-lws2_32", "-lsecur32", "-lcrypt32", "-lbcrypt", "-lncrypt",
                             "-ladvapi32",
                             "-o", str(cls.binary)],
                capture_output=True, text=True, timeout=180,
            )
            if result.returncode:
                raise RuntimeError(
                    f"Windows fixture build failed ({result.returncode}):\n"
                    f"{result.stdout}\n{result.stderr}")
        cls.wallet = cls.work / "wallet"
        cls.wallet.mkdir()

    def setUp(self):
        self.use_case("node1")

    def use_case(self, name, thumbprint=None, cluster=False):
        case = self.cases[name]
        self.env = dict(os.environ, OPENDATA_TEST_WALLET_PFX=case["pfx"],
                        OPENDATA_SERVER_USER="wallet-test-admin",
                        OPENDATA_SERVER_PASSWORD="wallet-test-password-123456")
        self.thumbprint = thumbprint or case["thumbprint"]
        purpose = "cluster" if cluster else "server"
        manifest = (f"version=1\n{purpose}.store=CurrentUser\\My\n"
                    f"{purpose}.thumbprint={self.thumbprint}\n")
        if cluster:
            manifest += "cluster.trust_store=CurrentUser\\Root\n"
        (self.wallet / "opendata-wallet.conf").write_text(manifest, encoding="ascii")

    def inspect(self):
        return subprocess.run([str(self.binary), "--inspect-wallet", str(self.wallet)],
                              env=self.env, capture_output=True, text=True, timeout=15)

    def test_inspects_native_server_and_cluster_identities(self):
        for cluster in (False, True):
            with self.subTest(cluster=cluster):
                self.use_case("node1", cluster=cluster)
                result = self.inspect()
                self.assertEqual(result.returncode, 0, result.stderr)
                for label in ("Subject:", "Issuer:", "Validity:", "SANs:", "EKUs:"):
                    self.assertIn(label, result.stdout)
                self.assertIn("127.0.0.1", result.stdout)
                self.assertNotIn("PRIVATE KEY", result.stdout)

    def test_rejects_invalid_wallet_identities(self):
        for name in ("expired", "future", "client-only", "public-only"):
            with self.subTest(name=name):
                self.use_case(name)
                result = self.inspect()
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("expired, lacks required EKU, or has no associated private key",
                              result.stderr)

    def test_rejects_missing_thumbprint_without_store_fallback(self):
        self.use_case("node1", thumbprint="0" * 40)
        result = self.inspect()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("thumbprint was not found", result.stderr)

    def test_rejects_invalid_direct_server_identity(self):
        cases = (("node1", "store:" + "0" * 40, "not found"),
                 ("expired", None, "expired or not yet valid"),
                 ("future", None, "expired or not yet valid"),
                 ("public-only", None, "cannot acquire Schannel"))
        for name, reference, diagnostic in cases:
            with self.subTest(name=name):
                self.use_case(name)
                result = subprocess.run(
                    [str(self.binary), "--database", str(self.work / f"{name}.pcod"),
                     "--tcps", "--port", "24961", "--cert",
                     reference or "store:" + self.thumbprint],
                    env=self.env, capture_output=True, text=True, timeout=15,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic, result.stderr)

    @staticmethod
    def receive_until(connection, marker=b"> "):
        data = bytearray()
        while marker not in data:
            chunk = connection.recv(65536)
            if not chunk:
                break
            data.extend(chunk)
        return bytes(data)

    def test_wallet_server_tls_authentication_and_query(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        with (self.work / "server.err").open("w+") as errors:
            child = subprocess.Popen(
                [str(self.binary), "--database", str(self.work / "positive.pcod"),
                 "--tcps", "--port", str(port), "--wallet", str(self.wallet)],
                env=self.env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=errors,
            )
            try:
                context = ssl.create_default_context(
                    cadata=Path(os.environ["OPENDATA_TEST_CLUSTER_CA"]).read_bytes())
                deadline = time.monotonic() + 15
                while True:
                    if child.poll() is not None:
                        errors.seek(0)
                        self.fail(f"server exited {child.returncode}: {errors.read()}")
                    try:
                        raw = socket.create_connection(("127.0.0.1", port), timeout=1)
                        break
                    except OSError:
                        if time.monotonic() >= deadline:
                            self.fail("TLS server did not become ready")
                        time.sleep(0.03)
                raw.settimeout(5)
                with raw, context.wrap_socket(raw, server_hostname="127.0.0.1") as connection:
                    connection.settimeout(5)
                    self.assertIsNotNone(connection.getpeercert())
                    connection.sendall(
                        b"AUTH wallet-test-admin wallet-test-password-123456\n")
                    response = self.receive_until(connection)
                    if b"Authenticated" not in response:
                        response += self.receive_until(connection)
                    self.assertIn(b"Authenticated", response)
                    connection.sendall(b"SELECT * FROM DUAL;\n")
                    self.assertIn(b"DUMMY", self.receive_until(connection))
            finally:
                if child.poll() is None:
                    child.terminate()
                child.wait(timeout=10)


if __name__ == "__main__":
    unittest.main()
