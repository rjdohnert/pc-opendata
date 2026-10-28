"""Exercise durable quorum replication with separate authenticated node processes."""

import json
import os
import shutil
import socket
import ssl
import struct
import subprocess
import tempfile
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from threading import Barrier
from pathlib import Path

from run_c_tests import ROOT, prepare_tls_certificates


class ClusterRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="opendata-cluster-recovery-")
        cls.addClassCleanup(cls.work.cleanup)
        cls.root = Path(cls.work.name)
        cls.environment = dict(os.environ, OPENDATA_SERVER_USER="cluster-test",
                               OPENDATA_SERVER_PASSWORD="test-only-cluster-password")
        if os.name == "nt":
            if not cls.environment.get("OPENDATA_TEST_CLUSTER_NODE1_CERT"):
                raise RuntimeError("Run through Invoke-ClusterTlsTest.ps1 -Recovery")
        else:
            prepare_tls_certificates(cls.root, cls.environment, shutil.which("openssl") or "openssl")
        cls.binary = cls.root / ("cluster-test.exe" if os.name == "nt" else "cluster-test")
        compiler = os.environ.get("CC") or shutil.which("clang") or "clang"
        modules = (ROOT / "src" / "modules.txt").read_text().splitlines()
        command = [compiler, "-std=c11", "-O1", "-g", "-DOPENDATA_TLS", "-DOPENDATA_TESTING",
                   *[str(ROOT / "src" / name) for name in modules if name.strip()]]
        if os.name == "nt":
            command.insert(6, "-DOPENDATA_TEST_TLS_FIXTURES")
        command += (["-lws2_32", "-lsecur32", "-lcrypt32", "-lbcrypt", "-lncrypt", "-ladvapi32"]
                    if os.name == "nt" else
                    ["-pthread", "-lssl", "-lcrypto", "-lm"])
        subprocess.run(command + ["-o", str(cls.binary)], check=True,
                       capture_output=True, text=True, timeout=120)

    def setUp(self):
        self.case = tempfile.TemporaryDirectory(dir=self.root)
        self.addCleanup(self.case.cleanup)
        self.directory = Path(self.case.name)
        probes = [socket.socket() for _ in range(4)]
        try:
            for probe in probes:
                probe.bind(("127.0.0.1", 0))
            self.ports = [probe.getsockname()[1] for probe in probes]
        finally:
            for probe in probes:
                probe.close()
        self.processes = {}
        self.sockets = []
        self.addCleanup(self.stop_all)

    def stop_all(self):
        for connection in self.sockets:
            connection.close()
        for node in list(self.processes):
            self.stop(node)

    def stop(self, node):
        process, output, errors = self.processes.pop(node)
        if process.poll() is None:
            process.kill()
        process.wait(timeout=15)
        output.close()
        errors.close()

    def start(self, node, drop_ack=False, authenticate=True):
        backup = self.directory / f"backup{node}"
        backup.mkdir(exist_ok=True)
        command = [str(self.binary), "--database", str(self.directory / f"node{node}.pcod"),
                   "--backup-dir", str(backup), "--server", "--port", str(self.ports[node - 1]),
                   "--cluster-node", str(node), "--cluster-port", str(self.ports[node + 1]),
                   "--cluster-peers", f"{3 - node}@127.0.0.1:{self.ports[4 - node]}",
                   "--cert", self.environment[f"OPENDATA_TEST_CLUSTER_NODE{node}_CERT"]]
        if node == 1:
            command += ["--sync-replication"]
        else:
            command += ["--cluster-follower"]
        if os.name != "nt":
            command += ["--key", self.environment[f"OPENDATA_TEST_CLUSTER_NODE{node}_KEY"],
                        "--cluster-ca", self.environment["OPENDATA_TEST_CLUSTER_CA"]]
        env = self.environment.copy()
        if node != 1:
            env.pop("OPENDATA_SERVER_USER", None)
            env.pop("OPENDATA_SERVER_PASSWORD", None)
        if drop_ack:
            env["OPENDATA_TEST_CLUSTER_DROP_ACK"] = "1"
        output = (self.directory / f"node{node}.out").open("a")
        errors = (self.directory / f"node{node}.err").open("a")
        process = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL,
                                   stdout=output, stderr=errors, cwd=self.directory)
        self.processes[node] = (process, output, errors)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if process.poll() is not None:
                self.fail((self.directory / f"node{node}.err").read_text())
            try:
                connection = socket.create_connection(("127.0.0.1", self.ports[node - 1]), timeout=1)
            except OSError:
                time.sleep(0.05)
                continue
            if not authenticate:
                connection.close()
                return None
            connection.settimeout(20)
            self.sockets.append(connection)
            return self.authenticate_connection(connection)
        self.fail(f"Node {node} did not become ready")

    def authenticate_connection(self, connection):
        connection.settimeout(20)
        connection.sendall(b"AUTH cluster-test test-only-cluster-password\n")
        response = self.receive(connection)
        if b"Authenticated" not in response:
            response += self.receive(connection)
        self.assertIn(b"Authenticated", response)
        self.query(connection, ".mode json")
        return connection

    def authenticate(self, node):
        connection = socket.create_connection(("127.0.0.1", self.ports[node - 1]), timeout=5)
        self.sockets.append(connection)
        return self.authenticate_connection(connection)

    def prepare_cluster(self):
        leader = self.start(1)
        self.start(2, authenticate=False)
        self.assertNotIn("ERROR", self.query(leader, "CREATE TABLE cluster_seed (id NUMBER);"))
        self.assertEqual("OK", self.query(leader, "COMMIT;"))
        follower = self.authenticate(2)
        return leader, follower

    @staticmethod
    def receive(connection):
        response = bytearray()
        while not response.endswith(b"> "):
            chunk = connection.recv(65536)
            if not chunk:
                raise EOFError("node closed query connection")
            response.extend(chunk)
        return bytes(response)

    def query(self, connection, sql):
        connection.sendall(sql.encode() + b"\n")
        return self.receive(connection).removesuffix(b"> ").decode().strip()

    def create_pending(self, leader):
        self.assertNotIn("ERROR", self.query(leader, "CREATE TABLE durable (id NUMBER);"))
        self.assertNotIn("ERROR", self.query(leader, "INSERT INTO durable VALUES (17);"))

    def assert_rows(self, connection, values):
        result = self.query(connection, "SELECT id FROM durable ORDER BY id;")
        self.assertEqual([str(row["ID"]) for row in json.loads(result)], values)

    def test_acknowledged_commit_survives_both_nodes_killed(self):
        leader, follower = self.prepare_cluster()
        self.create_pending(leader)
        self.assertEqual("OK", self.query(leader, "COMMIT;"))
        self.assert_rows(follower, ["17"])
        self.stop(1)
        self.stop(2)
        follower = self.start(2)
        leader = self.start(1)
        self.assert_rows(leader, ["17"])
        self.assert_rows(follower, ["17"])

    def test_logins_during_replicated_catalog_replacement(self):
        leader, follower = self.prepare_cluster()
        barrier = Barrier(2, timeout=15)

        def login_once():
            barrier.wait()
            connection = socket.create_connection(("127.0.0.1", self.ports[1]), timeout=10)
            try:
                self.authenticate_connection(connection)
                self.assertNotIn("ERROR", self.query(connection, "SELECT id FROM cluster_seed;"))
            finally:
                connection.close()

        with ThreadPoolExecutor(max_workers=1) as executor:
            for _ in range(4):
                self.assertNotIn("ERROR", self.query(
                    leader, "ALTER USER 'cluster-test' ROLE ADMIN;"))
                attempt = executor.submit(login_once)
                barrier.wait()
                self.assertEqual("OK", self.query(leader, "COMMIT;"))
                attempt.result(timeout=30)
                self.assertNotIn("ERROR", self.query(follower, "SELECT id FROM cluster_seed;"))

    def test_unavailable_peer_does_not_acknowledge_commit(self):
        leader = self.start(1)
        self.create_pending(leader)
        self.assertIn("ERROR", self.query(leader, "COMMIT;"))
        self.stop(1)
        leader = self.start(1)
        self.assertIn("ERROR", self.query(leader, "SELECT * FROM durable;"))

    def test_failed_follower_publication_does_not_acknowledge_commit(self):
        leader, follower = self.prepare_cluster()
        self.create_pending(leader)
        obstruction = self.directory / "node2.pcod.pending"
        obstruction.mkdir()
        self.assertIn("ERROR", self.query(leader, "COMMIT;"))
        self.assertIn("ERROR", self.query(follower, "SELECT * FROM durable;"))
        obstruction.rmdir()
        self.assertEqual("OK", self.query(leader, "COMMIT;"))
        self.assert_rows(follower, ["17"])

    def test_lost_ack_is_not_reported_as_success_and_retry_recovers(self):
        leader, follower = self.prepare_cluster()
        self.stop(2)
        follower = self.start(2, drop_ack=True)
        self.create_pending(leader)
        self.assertIn("ERROR", self.query(leader, "COMMIT;"))
        self.assert_rows(follower, ["17"])
        self.stop(2)
        follower = self.start(2)
        self.assertEqual("OK", self.query(leader, "COMMIT;"))
        self.assert_rows(leader, ["17"])
        self.assert_rows(follower, ["17"])

    @unittest.skipIf(os.name == "nt", "Python TLS client requires PEM test keys")
    def test_truncated_authenticated_snapshot_is_not_published(self):
        leader, follower = self.prepare_cluster()
        context = ssl.create_default_context(cafile=self.environment["OPENDATA_TEST_CLUSTER_CA"])
        context.load_cert_chain(self.environment["OPENDATA_TEST_CLUSTER_NODE1_CERT"],
                                self.environment["OPENDATA_TEST_CLUSTER_NODE1_KEY"])
        with socket.create_connection(("127.0.0.1", self.ports[3]), timeout=5) as raw:
            with context.wrap_socket(raw, server_hostname="127.0.0.1") as connection:
                connection.sendall(struct.pack("=IHIII", 0x50434F44, 1, 1, 1, 1024) + b"partial")
        self.assertIn("ERROR", self.query(follower, "SELECT * FROM durable;"))
        self.stop(2)
        follower = self.start(2)
        self.assertIn("ERROR", self.query(follower, "SELECT * FROM durable;"))


if __name__ == "__main__":
    unittest.main()
