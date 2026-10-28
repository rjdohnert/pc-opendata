import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class TlsWalletTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="opendata-tls-wallet-")
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        cls.binary = work / ("opendata.exe" if os.name == "nt" else "opendata")
        configured = os.environ.get("CC")
        compiler = shlex.split(configured) if configured else [shutil.which("clang") or "cc"]
        modules = (ROOT / "src" / "modules.txt").read_text(encoding="utf-8").splitlines()
        sources = [str(ROOT / "src" / name) for name in modules if name.strip()]
        libraries = (["-lws2_32", "-lsecur32", "-lcrypt32", "-lbcrypt"]
                     if os.name == "nt" else ["-lssl", "-lcrypto", "-lm"])
        flags = ["-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror", "-DOPENDATA_TLS"]
        if os.name != "nt":
            flags.append("-pthread")
        command = compiler + flags + sources + libraries + ["-o", str(cls.binary)]
        subprocess.run(
            command,
            check=True, capture_output=True, text=True, timeout=120,
        )
        cls.wallet = work / "wallet"
        cls.wallet.mkdir()
        if os.name != "nt":
            cls.create_certificate_fixtures(cls.wallet)
        cls.valid_manifest = (
            "version=1\n"
            "server.cert=server.pem\n"
            "server.key=server.key\n"
            "cluster.cert=cluster.pem\n"
            "cluster.key=cluster.key\n"
            "cluster.ca=ca.pem\n"
        )

    @classmethod
    def create_certificate_fixtures(cls, directory):
        openssl = shutil.which("openssl")
        if not openssl:
            raise unittest.SkipTest("OpenSSL command-line tool unavailable")
        ca_key = directory / "ca.key"
        ca_cert = directory / "ca.pem"
        subprocess.run(
            [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3",
             "-subj", "/CN=OpenData Wallet Test CA", "-addext", "basicConstraints=critical,CA:TRUE",
             "-addext", "keyUsage=critical,keyCertSign,cRLSign", "-keyout", str(ca_key),
             "-out", str(ca_cert)],
            check=True, capture_output=True, timeout=30,
        )
        for purpose, common_name, extended_usage, san in (
            ("server", "localhost", "serverAuth", "DNS:localhost,IP:127.0.0.1"),
            ("cluster", "opendata-node-1", "serverAuth,clientAuth", "IP:127.0.0.1"),
        ):
            key = directory / f"{purpose}.key"
            request = directory / f"{purpose}.csr"
            cert = directory / f"{purpose}.pem"
            extension = directory / f"{purpose}.ext"
            extension.write_text(
                "basicConstraints=critical,CA:FALSE\n"
                "keyUsage=critical,digitalSignature,keyEncipherment\n"
                f"extendedKeyUsage={extended_usage}\n"
                f"subjectAltName={san}\n",
                encoding="ascii",
            )
            subprocess.run(
                [openssl, "req", "-new", "-newkey", "rsa:2048", "-nodes",
                 "-subj", f"/CN={common_name}", "-keyout", str(key), "-out", str(request)],
                check=True, capture_output=True, timeout=30,
            )
            subprocess.run(
                [openssl, "x509", "-req", "-in", str(request), "-CA", str(ca_cert),
                 "-CAkey", str(ca_key), "-CAcreateserial", "-out", str(cert), "-days", "2",
                 "-sha256", "-extfile", str(extension)],
                check=True, capture_output=True, timeout=30,
            )
            key.chmod(0o600)

        (directory / "index.txt").write_text("", encoding="ascii")
        (directory / "serial").write_text("1000\n", encoding="ascii")
        config = directory / "ca.conf"
        config.write_text(
            "[ca]\ndefault_ca=issuer\n[issuer]\n"
            f"database={directory / 'index.txt'}\nserial={directory / 'serial'}\n"
            f"new_certs_dir={directory}\ncertificate={ca_cert}\nprivate_key={ca_key}\n"
            "default_md=sha256\ndefault_days=2\npolicy=policy\nunique_subject=no\n"
            "[policy]\ncommonName=supplied\n",
            encoding="ascii",
        )
        for label, start, end in (
            ("expired", "20000101000000Z", "20010101000000Z"),
            ("future", "20900101000000Z", "20910101000000Z"),
        ):
            subprocess.run(
                [openssl, "ca", "-batch", "-config", str(config),
                 "-in", str(directory / "server.csr"), "-out", str(directory / f"{label}.pem"),
                 "-startdate", start, "-enddate", end, "-notext",
                 "-extfile", str(directory / "server.ext")],
                check=True, capture_output=True, timeout=30,
            )
        for label, usage in (("wrong-eku", "clientAuth"), ("malformed-eku", "DER:01:01:FF")):
            extensions = directory / f"{label}.ext"
            extensions.write_text(f"extendedKeyUsage={usage}\n", encoding="ascii")
            subprocess.run(
                [openssl, "x509", "-req", "-in", str(directory / "server.csr"),
                 "-CA", str(ca_cert), "-CAkey", str(ca_key), "-CAcreateserial",
                 "-out", str(directory / f"{label}.pem"), "-days", "2",
                 "-extfile", str(extensions)],
                check=True, capture_output=True, timeout=30,
            )
        subprocess.run(
            [openssl, "pkey", "-in", str(directory / "server.key"), "-aes-256-cbc",
             "-passout", "stdin", "-out", str(directory / "encrypted.key")],
            input=b"fixture-only-secret\n", check=True, capture_output=True, timeout=30,
        )
        (directory / "encrypted.key").chmod(0o600)

    def write_manifest(self, content):
        path = self.wallet / "opendata-wallet.conf"
        path.write_text(content, encoding="ascii")
        return path

    def inspect(self):
        return subprocess.run(
            [str(self.binary), "--inspect-wallet", str(self.wallet)],
            capture_output=True, text=True, timeout=15,
        )

    @unittest.skipIf(os.name == "nt", "Windows wallet tests use read-only native store references")
    def test_valid_relative_paths_and_separate_identities(self):
        (self.wallet / "server..pem").write_bytes((self.wallet / "server.pem").read_bytes())
        self.write_manifest(self.valid_manifest)
        result = self.inspect()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Format version: 1", result.stdout)
        self.assertIn(str((self.wallet / "server.pem").resolve()), result.stdout)
        self.assertIn(str((self.wallet / "cluster.pem").resolve()), result.stdout)
        self.assertIn("Subject:", result.stdout)
        self.assertIn("Issuer:", result.stdout)
        self.assertIn("Not before:", result.stdout)
        self.assertIn("SANs: localhost", result.stdout)
        self.assertIn("EKUs:", result.stdout)
        self.assertNotIn("PRIVATE KEY-----", result.stdout)

        self.write_manifest(self.valid_manifest.replace("server.pem", "server..pem"))
        result = self.inspect()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_unknown_duplicate_and_malformed_fields(self):
        cases = (
            self.valid_manifest + "unexpected=value\n",
            self.valid_manifest + "version=1\n",
            self.valid_manifest.replace("version=1\n", "version=1\nbroken\n"),
            self.valid_manifest + "bad=\x00value\n",
            self.valid_manifest.replace("server.cert=server.pem", "server.cert=../server.pem"),
            self.valid_manifest.replace("server.cert=server.pem", "server.cert=/tmp/server.pem"),
        )
        for manifest in cases:
            with self.subTest(manifest=manifest[-40:]):
                self.write_manifest(manifest)
                result = self.inspect()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("invalid wallet manifest", result.stderr)

    @unittest.skipIf(os.name == "nt", "PEM wallet identities are POSIX-only")
    def test_rejects_missing_files_mismatched_key_and_unsafe_permissions(self):
        self.write_manifest(self.valid_manifest.replace("server.key=server.key", "server.key=missing.key"))
        self.assertNotEqual(self.inspect().returncode, 0)

        self.write_manifest(self.valid_manifest.replace("server.key=server.key", "server.key=cluster.key"))
        self.assertNotEqual(self.inspect().returncode, 0)

        key = self.wallet / "server.key"
        key.chmod(0o644)
        try:
            self.write_manifest(self.valid_manifest)
            result = self.inspect()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("owner-only", result.stderr)
        finally:
            key.chmod(0o600)

    @unittest.skipIf(os.name == "nt", "POSIX certificate metadata policy")
    def test_rejects_invalid_validity_and_eku(self):
        for cert in ("expired.pem", "future.pem", "wrong-eku.pem", "malformed-eku.pem"):
            with self.subTest(cert=cert):
                self.write_manifest(self.valid_manifest.replace("server.cert=server.pem",
                                                               f"server.cert={cert}"))
                result = self.inspect()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("certificate is invalid", result.stderr)

    @unittest.skipIf(os.name == "nt", "POSIX private key policy")
    def test_rejects_encrypted_key_without_interactive_prompt(self):
        self.write_manifest(self.valid_manifest.replace("server.key=server.key",
                                                       "server.key=encrypted.key"))
        result = subprocess.run(
            [str(self.binary), "--inspect-wallet", str(self.wallet)],
            input="must-not-be-read\n", capture_output=True, text=True, timeout=5,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("Enter PEM pass phrase", result.stderr)
        self.assertNotIn("fixture-only-secret", result.stderr + result.stdout)

    @unittest.skipIf(os.name == "nt", "POSIX trust bundle policy")
    def test_rejects_malformed_ca_bundle(self):
        bad_ca = self.wallet / "bad-ca.pem"
        bad_ca.write_text("not a certificate\n", encoding="ascii")
        self.write_manifest(self.valid_manifest.replace("cluster.ca=ca.pem",
                                                       "cluster.ca=bad-ca.pem"))
        result = self.inspect()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CA bundle", result.stderr)

    @unittest.skipIf(os.name == "nt", "POSIX trust chain policy")
    def test_rejects_identity_from_untrusted_ca(self):
        self.write_manifest(self.valid_manifest.replace("cluster.ca=ca.pem",
                                                       "cluster.ca=server.pem"))
        result = self.inspect()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not chain to its CA bundle", result.stderr)

    @unittest.skipIf(os.name == "nt", "POSIX wallet filesystem policy")
    def test_rejects_writable_wallet_inputs_and_directories(self):
        manifest = self.write_manifest(self.valid_manifest)
        paths = (self.wallet.parent, self.wallet, manifest,
                 self.wallet / "server.pem", self.wallet / "ca.pem")
        for path in paths:
            if not path.exists():
                continue
            previous = path.stat().st_mode & 0o7777
            try:
                path.chmod(previous | 0o020)
                result = self.inspect()
                self.assertNotEqual(result.returncode, 0, str(path))
                self.assertIn("unsafe wallet path", result.stderr)
            finally:
                path.chmod(previous)

    @unittest.skipIf(os.name == "nt", "POSIX wallet filesystem policy")
    def test_rejects_symlinks_and_special_files(self):
        self.write_manifest(self.valid_manifest)
        manifest = self.wallet / "opendata-wallet.conf"
        for path in (manifest, self.wallet / "server.pem", self.wallet / "server.key",
                     self.wallet / "ca.pem"):
            saved = path.with_name(path.name + ".saved")
            path.rename(saved)
            try:
                path.symlink_to(saved.name)
                result = self.inspect()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("unsafe wallet path", result.stderr)
            finally:
                path.unlink()
                saved.rename(path)

        link = self.wallet.parent / "wallet-link"
        link.symlink_to(self.wallet, target_is_directory=True)
        self.addCleanup(link.unlink)
        result = subprocess.run(
            [str(self.binary), "--inspect-wallet", str(link)],
            capture_output=True, text=True, timeout=15,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsafe wallet path", result.stderr)

        manifest = self.wallet / "opendata-wallet.conf"
        manifest.unlink()
        try:
            os.mkfifo(manifest, 0o600)
            result = self.inspect()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unsafe wallet path", result.stderr)
        finally:
            manifest.unlink()
            self.write_manifest(self.valid_manifest)

        fifo = self.wallet / "fifo"
        os.mkfifo(fifo, 0o600)
        self.addCleanup(fifo.unlink)
        self.write_manifest(self.valid_manifest.replace("server.key=server.key", "server.key=fifo"))
        result = self.inspect()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsafe wallet path", result.stderr)

    @unittest.skipIf(os.name == "nt", "POSIX wallet filesystem policy")
    def test_rejects_symlinked_or_writable_path_components(self):
        self.write_manifest(self.valid_manifest)
        nested = self.wallet / "nested"
        nested.mkdir()
        self.addCleanup(nested.rmdir)
        cert = nested / "cert.pem"
        cert.write_bytes((self.wallet / "server.pem").read_bytes())
        self.addCleanup(cert.unlink)
        self.write_manifest(self.valid_manifest.replace("server.cert=server.pem",
                                                       "server.cert=nested/cert.pem"))
        self.assertEqual(self.inspect().returncode, 0)
        nested.chmod(0o777)
        self.assertNotEqual(self.inspect().returncode, 0)
        nested.chmod(0o755)
        alias = self.wallet / "alias"
        alias.symlink_to(nested.name, target_is_directory=True)
        self.addCleanup(alias.unlink)
        self.write_manifest(self.valid_manifest.replace("server.cert=server.pem",
                                                       "server.cert=alias/cert.pem"))
        self.assertNotEqual(self.inspect().returncode, 0)

    @unittest.skipIf(os.name == "nt", "POSIX wallet filesystem policy")
    def test_rejects_untrusted_file_owner(self):
        if os.geteuid() != 0:
            self.skipTest("ownership rejection requires permission to change fixture owner")
        path = self.wallet / "server.key"
        original = path.stat()
        self.write_manifest(self.valid_manifest)
        try:
            os.chown(path, 65534, original.st_gid)
            result = self.inspect()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unsafe wallet path", result.stderr)
        finally:
            os.chown(path, original.st_uid, original.st_gid)


if __name__ == "__main__":
    unittest.main()
