import csv
import io
import json
import os
import shlex
import shutil
import socket
import ssl
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
USER = "network-test-admin"
PASSWORD = "test-only-password-123456"


class NetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="opendata-network-build-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.tls = os.environ.get("OPENDATA_TEST_TLS") == "1"
        if cls.tls and os.name == "nt":
            raise unittest.SkipTest(
                "Windows TLS runtime tests are disabled until they can use isolated certificate stores"
            )
        supplied = os.environ.get("OPENDATA_TEST_BINARY")
        if supplied:
            cls.binary = Path(supplied).resolve()
        else:
            cls.binary = Path(cls.build.name) / ("opendata.exe" if os.name == "nt" else "opendata")
            configured = os.environ.get("CC")
            compiler = ([configured] if configured and Path(configured).is_file() else shlex.split(configured)) \
                if configured else [shutil.which("clang") or "cc"]
            flags = ["-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror"]
            if os.name != "nt":
                flags.append("-pthread")
            flags += shlex.split(os.environ.get("OPENDATA_TEST_CFLAGS", ""))
            modules = (ROOT / "src" / "modules.txt").read_text().splitlines()
            sources = [str(ROOT / "src" / name) for name in modules if name.strip()]
            libraries = ["-lws2_32", "-lsecur32", "-lcrypt32", "-lbcrypt"] if os.name == "nt" else ["-lcrypto", "-lm"]
            libraries += shlex.split(os.environ.get("OPENDATA_TEST_LDFLAGS", ""))
            subprocess.run(compiler + flags + sources + libraries + ["-o", str(cls.binary)],
                           check=True, capture_output=True, text=True, timeout=120)
        if cls.tls:
            cls.store_certificate = None
            if os.environ.get("OPENDATA_TEST_CERT") and os.environ.get("OPENDATA_TEST_KEY"):
                cls.cert = Path(os.environ["OPENDATA_TEST_CERT"])
                cls.key = Path(os.environ["OPENDATA_TEST_KEY"])
            else:
                cls.cert = Path(cls.build.name) / "localhost-cert.pem"
                cls.key = Path(cls.build.name) / "localhost-key.pem"
                subprocess.run(
                    ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                     "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                     "-out", str(cls.cert), "-keyout", str(cls.key)],
                    check=True, capture_output=True, timeout=30,
                )
            cls.context = ssl.create_default_context(cafile=str(cls.cert))

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="opendata-network-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = dict(os.environ, OPENDATA_SERVER_USER=USER, OPENDATA_SERVER_PASSWORD=PASSWORD)

    def args(self, *extra):
        return [str(self.binary), "--database", str(self.root / "test.pcod"), *extra]

    def start_server(self, tls=None):
        if tls is None:
            tls = self.tls
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        self.port = probe.getsockname()[1]
        probe.close()
        args = ["--tcps" if tls else "--server", "--port", str(self.port)]
        if tls:
            if self.store_certificate:
                args += ["--cert", self.store_certificate]
            else:
                args += ["--cert", str(self.cert), "--key", str(self.key)]
        errors = (self.root / "server.err").open("w+")
        output = (self.root / "server.out").open("w+")
        child = subprocess.Popen(self.args(*args), env=self.env, stdin=subprocess.DEVNULL,
                                 stdout=output, stderr=errors)
        self.server_child = child

        def cleanup():
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=10)
            errors.close()
            output.close()

        self.addCleanup(cleanup)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if child.poll() is not None:
                errors.seek(0)
                self.fail(f"server exited {child.returncode}: {errors.read()}")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.1):
                    return child
            except OSError:
                time.sleep(0.02)
        self.fail("server did not become responsive")

    def connect(self, tls=None):
        if tls is None:
            tls = self.tls
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        self.addCleanup(sock.close)
        if tls:
            sock = self.context.wrap_socket(sock, server_hostname="localhost")
            self.addCleanup(sock.close)
        return sock

    @staticmethod
    def receive_until(sock, marker=b"> "):
        data = bytearray()
        while marker not in data:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data.extend(chunk)
        return bytes(data)

    def auth(self, sock):
        self.auth_as(sock, USER, PASSWORD)

    def auth_as(self, sock, user, password):
        sock.sendall(f"AUTH {user} {password}\n".encode())
        response = self.receive_until(sock)
        if b"Authentication required:" in response and b"Authenticated" not in response:
            response += self.receive_until(sock)
        self.assertIn(b"Authenticated", response)

    @staticmethod
    def read_exact(sock, length):
        data = bytearray()
        while len(data) < length:
            chunk = sock.recv(length - len(data))
            if not chunk:
                raise EOFError("connection closed mid-packet")
            data.extend(chunk)
        return bytes(data)

    def pg_message(self, sock):
        kind = self.read_exact(sock, 1)
        length = struct.unpack("!I", self.read_exact(sock, 4))[0]
        self.assertGreaterEqual(length, 4)
        self.assertLess(length, 32 * 1024 * 1024)
        return kind, self.read_exact(sock, length - 4)

    def pg_startup(self, sock, user=USER):
        body = struct.pack("!I", 196608) + b"user\0" + user.encode() + b"\0\0"
        packet = struct.pack("!I", len(body) + 4) + body
        for piece in (packet[:1], packet[1:3], packet[3:7], packet[7:]):
            sock.sendall(piece)
            time.sleep(0.01)
        self.assertEqual(self.pg_message(sock), (b"R", struct.pack("!I", 3)))

    def pg_auth(self, sock, user=USER, password=PASSWORD):
        self.pg_startup(sock, user)
        password = password.encode() + b"\0"
        sock.sendall(b"p" + struct.pack("!I", len(password) + 4) + password)
        messages = []
        while not messages or messages[-1][0] != b"Z":
            messages.append(self.pg_message(sock))
        self.assertEqual(messages[0], (b"R", struct.pack("!I", 0)))

    def test_missing_credentials_fail_closed(self):
        self.env.pop("OPENDATA_SERVER_PASSWORD")
        result = subprocess.run(self.args("--server"), env=self.env, capture_output=True,
                                text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OPENDATA_SERVER_PASSWORD", result.stderr)

    def test_invalid_cli_and_remote_plaintext(self):
        for args in (("--port", "-1"), ("--port", "65536"), ("--port", "123abc"),
                     ("--port", "99999999999999999999999"),
                     ("--server", "--bind", "0.0.0.0"),
                     ("--server", "--bind", "not-an-address")):
            with self.subTest(args=args):
                result = subprocess.run(self.args(*args), env=self.env, capture_output=True,
                                        text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("ERROR", result.stderr)

    def test_tls_configuration_fails_closed(self):
        result = subprocess.run(self.args("--tcps"), env=self.env, capture_output=True,
                                text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TLS", result.stderr)
        if os.name == "nt" and self.tls:
            for certificate in ("store:" + "z" * 40, "store:" + "0" * 40, "server.pem"):
                with self.subTest(certificate=certificate):
                    result = subprocess.run(self.args("--tcps", "--cert", certificate), env=self.env,
                                            capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("ERROR", result.stderr)

    def test_text_auth_and_sql_authorization(self):
        self.start_server()
        sock = self.connect()
        sock.sendall(b"SELECT * FROM DUAL;\n")
        self.assertIn(b"authentication required", self.receive_until(sock, b"\n"))
        sock = self.connect()
        sock.sendall(f"AUTH {USER} wrong-password\n".encode())
        self.assertIn(b"authentication failed", self.receive_until(sock, b"\n"))
        sock = self.connect()
        self.auth(sock)
        sock.sendall(b"SELECT * FROM DUAL;\n")
        self.assertIn(b"DUMMY\r\nX\r\n", self.receive_until(sock))
        for command in (b"SOURCE 'secret.sql';\n", b"CREATE DATABASE 'other.pcod';\n",
                        b"SPOOL 'server-file';\n", b"IRIS;\n"):
            sock.sendall(command)
            self.assertIn(b"not permitted", self.receive_until(sock))
        self.assertFalse((self.root / "other.pcod").exists())

    def test_authentication_throttling_is_shared_by_protocols_and_source_scoped(self):
        self.start_server(tls=False)
        existing = self.connect(tls=False)
        self.auth(existing)
        for _ in range(4):
            sock = self.connect(tls=False)
            sock.sendall(f"AUTH {USER} incorrect-password-123456\n".encode())
            self.assertIn(b"authentication failed",
                          self.receive_until(sock, b"authentication failed"))
            sock.close()

        pg = self.connect(tls=False)
        self.pg_startup(pg)
        password = PASSWORD.encode() + b"\0"
        pg.sendall(b"p" + struct.pack("!I", len(password) + 4) + password)
        kind, body = self.pg_message(pg)
        self.assertEqual(kind, b"E")
        self.assertIn(b"authentication failed", body)

        other = socket.create_connection(("127.0.0.1", self.port), timeout=5,
                                         source_address=("127.0.0.2", 0))
        self.addCleanup(other.close)
        self.auth(other)
        existing.sendall(b"SELECT * FROM DUAL;\n")
        self.assertIn(b"DUMMY", self.receive_until(existing))

        unknown = self.connect(tls=False)
        unknown.sendall(b"AUTH unknown-account incorrect-password-123456\n")
        self.assertIn(b"authentication failed",
                      self.receive_until(unknown, b"authentication failed"))
        summaries = [line for line in (self.root / "server.err").read_text().splitlines()
                     if line.startswith("AUTH_STATS ")]
        self.assertTrue(summaries)
        for line in summaries:
            for label in ("admitted=", "credential_failures=", "source_throttled=",
                          "pair_throttled=", "global_throttled=", "source_saturated=",
                          "pair_saturated="):
                self.assertIn(label, line)
            for secret in (USER, PASSWORD, "unknown-account", "127.0.0.1"):
                self.assertNotIn(secret, line)

    def test_multi_user_roles_and_persistence(self):
        self.start_server()
        admin = self.connect()
        self.auth(admin)
        reader_password = "reader-password-123456"
        admin.sendall(
            f"CREATE USER 'reader' IDENTIFIED BY '{reader_password}' ROLE READ_ONLY;\n".encode()
        )
        self.assertIn(b"OK", self.receive_until(admin))
        admin.sendall(b"COMMIT;\n")
        self.assertIn(b"OK", self.receive_until(admin))
        admin.sendall(b"SHOW USERS;\n")
        listing = self.receive_until(admin)
        self.assertIn(b"reader\tREAD_ONLY\tENABLED", listing)
        self.assertNotIn(reader_password.encode(), listing)
        audit_log = (self.root / "server.err").read_text()
        self.assertIn("actor=network-test-admin operation=CREATE USER target=reader", audit_log)
        self.assertNotIn(reader_password, audit_log)

        self.server_child.terminate()
        self.server_child.wait(timeout=10)
        self.env.pop("OPENDATA_SERVER_USER")
        self.env.pop("OPENDATA_SERVER_PASSWORD")
        self.start_server()
        reader = self.connect()
        self.auth_as(reader, "reader", reader_password)
        reader.sendall(b"CONNECTION STATUS;\n")
        status = self.receive_until(reader)
        self.assertIn(b"Session\treader", status)
        self.assertIn(
            b"Transport security\t" +
            (b"encrypted TLS" if self.tls else b"unencrypted loopback TCP"),
            status,
        )
        reader.sendall(b"SELECT * FROM DUAL;\n")
        self.assertIn(b"DUMMY\r\nX\r\n", self.receive_until(reader))
        for statement in (b"CREATE TABLE denied (n NUMBER);\n",
                          b"SHOW USERS;\n"):
            reader.sendall(statement)
            self.assertIn(b"permission denied", self.receive_until(reader))
        reader.sendall(b"INSERT INTO missing VALUES (1);\n")
        self.assertIn(b"ERROR", self.receive_until(reader))
        reader_pg = self.connect()
        self.pg_auth(reader_pg, "reader", reader_password)
        query = b"DELETE FROM missing;\0"
        reader_pg.sendall(b"Q" + struct.pack("!I", len(query) + 4) + query)
        messages = []
        while not messages or messages[-1][0] != b"Z":
            messages.append(self.pg_message(reader_pg))
        error = next(body for kind, body in messages if kind == b"E")
        self.assertIn(b"ERROR", error)

        admin = self.connect()
        self.auth(admin)
        admin.sendall(b"ALTER USER 'reader' DISABLE;\n")
        self.assertIn(b"OK", self.receive_until(admin))
        admin.sendall(b"COMMIT;\n")
        self.assertIn(b"OK", self.receive_until(admin))
        reader.sendall(b"SELECT * FROM DUAL;\n")
        self.assertIn(b"disabled or no longer exists", self.receive_until(reader))

    def test_per_table_grants_for_read_only_users(self):
        self.start_server()
        admin = self.connect()
        self.auth(admin)
        for sql in (b"CREATE TABLE secrets (id NUMBER);\n",
                    b"INSERT INTO secrets VALUES (7);\n",
                    b"CREATE USER 'analyst' IDENTIFIED BY 'analyst-password-123' ROLE READ_ONLY;\n",
                    b"COMMIT;\n"):
            admin.sendall(sql)
            self.assertIn(b"OK", self.receive_until(admin))

        reader = self.connect()
        self.auth_as(reader, "analyst", "analyst-password-123")
        reader.sendall(b"SELECT * FROM secrets;\n")
        self.assertIn(b"permission denied", self.receive_until(reader))
        reader.sendall(b"INSERT INTO secrets VALUES (8);\n")
        self.assertIn(b"permission denied", self.receive_until(reader))

        admin.sendall(b"GRANT SELECT ON secrets TO 'analyst';\n")
        self.assertIn(b"OK", self.receive_until(admin))
        admin.sendall(b"COMMIT;\n")
        self.assertIn(b"OK", self.receive_until(admin))
        reader.sendall(b"SELECT * FROM secrets;\n")
        self.assertIn(b"ID\r\n7\r\n", self.receive_until(reader))
        reader.sendall(b"INSERT INTO secrets VALUES (8);\n")
        self.assertIn(b"permission denied", self.receive_until(reader))

        admin.sendall(b"GRANT INSERT ON secrets TO 'analyst';\n")
        self.assertIn(b"OK", self.receive_until(admin))
        admin.sendall(b"COMMIT;\n")
        self.assertIn(b"OK", self.receive_until(admin))
        reader.sendall(b"INSERT INTO secrets VALUES (8);\n")
        self.assertIn(b"OK", self.receive_until(reader))
        reader.sendall(b"COMMIT;\n")
        self.assertIn(b"OK", self.receive_until(reader))

        admin.sendall(b"SHOW GRANTS;\n")
        grants = self.receive_until(admin)
        self.assertIn(b"analyst", grants)
        self.assertIn(b"SECRETS", grants)
        self.assertIn(b"SELECT", grants)
        self.assertIn(b"INSERT", grants)

        admin.sendall(b"REVOKE ON secrets FROM 'analyst';\n")
        self.assertIn(b"OK", self.receive_until(admin))
        admin.sendall(b"COMMIT;\n")
        self.assertIn(b"OK", self.receive_until(admin))
        reader.sendall(b"SELECT * FROM secrets;\n")
        self.assertIn(b"permission denied", self.receive_until(reader))

        reader.sendall(b"GRANT SELECT ON secrets TO 'analyst';\n")
        self.assertIn(b"permission denied", self.receive_until(reader))

    def test_quoted_delimiters_and_output_escaping(self):
        self.start_server()
        sock = self.connect()
        self.auth(sock)
        for sql in (b"CREATE TABLE strings (v VARCHAR2(100));\n",
                    b"INSERT INTO strings VALUES ('a;\"b,c''d');\n"):
            sock.sendall(sql)
            self.assertIn(b"OK", self.receive_until(sock))
        sock.sendall(b".mode json\n")
        self.receive_until(sock)
        sock.sendall(b"SELECT v FROM strings;\n")
        payload = self.receive_until(sock).removesuffix(b"> ").decode()
        self.assertEqual(json.loads(payload), [{"V": "a;\"b,c'd"}])
        sock.sendall(b".mode csv\n")
        self.receive_until(sock)
        sock.sendall(b"SELECT v FROM strings;\n")
        payload = self.receive_until(sock).removesuffix(b"> ").decode()
        self.assertEqual(list(csv.reader(io.StringIO(payload))), [["V"], ["a;\"b,c'd"]])

    def test_oversized_and_nul_input_do_not_execute_prefix(self):
        self.start_server()
        for bad in (b"SELECT * FROM DUAL" + b"x" * 70000 + b";\n",
                    b"CREATE TABLE bad (n NUMBER)\0;\n"):
            sock = self.connect()
            self.auth(sock)
            sock.sendall(bad)
            try:
                response = self.receive_until(sock, b"\n")
            except ConnectionResetError:
                response = b""
            self.assertTrue(b"ERROR" in response or not response)
        sock = self.connect()
        self.auth(sock)
        sock.sendall(b"SELECT * FROM bad;\n")
        self.assertIn(b"table does not exist", self.receive_until(sock))
        sock.sendall(b"SELECT * FROM DUAL;\n")
        self.assertIn(b"DUMMY", self.receive_until(sock))

    def test_postgres_requires_auth_before_query(self):
        self.start_server()
        sock = self.connect()
        self.pg_startup(sock)
        query = b"SELECT * FROM DUAL;\0"
        sock.sendall(b"Q" + struct.pack("!I", len(query) + 4) + query)
        kind, body = self.pg_message(sock)
        self.assertEqual(kind, b"E")
        self.assertIn(b"authentication required", body)

    def test_postgres_fragmentation_and_wide_rows(self):
        self.start_server()
        sock = self.connect()
        self.pg_auth(sock)
        columns = ",".join(f"c{i} VARCHAR2(255)" for i in range(32))
        values = ",".join("'" + "a" * 255 + "'" for _ in range(32))
        for sql in (f"CREATE TABLE wide ({columns});",
                    f"INSERT INTO wide VALUES ({values});", "SELECT * FROM wide;"):
            query = sql.encode() + b"\0"
            sock.sendall(b"Q" + struct.pack("!I", len(query) + 4) + query)
            messages = []
            while not messages or messages[-1][0] != b"Z":
                messages.append(self.pg_message(sock))
            self.assertNotIn(b"E", [kind for kind, _ in messages])
        row = next(body for kind, body in messages if kind == b"D")
        self.assertEqual(struct.unpack_from("!H", row)[0], 32)
        offset = 2
        for _ in range(32):
            size = struct.unpack_from("!I", row, offset)[0]
            offset += 4
            self.assertEqual(size, 255)
            self.assertEqual(row[offset:offset + size], b"a" * 255)
            offset += size
        self.assertEqual(offset, len(row))

    def test_transaction_owner_and_disconnect_rollback(self):
        self.start_server()
        first, second = self.connect(), self.connect()
        self.auth(first)
        self.auth(second)
        first.sendall(b"CREATE TABLE rolled_back (n NUMBER);\n")
        self.assertIn(b"OK", self.receive_until(first))
        second.sendall(b"COMMIT;\n")
        self.assertIn(b"another session", self.receive_until(second))
        first.close()
        time.sleep(0.2)
        second.sendall(b"CREATE TABLE rolled_back (n NUMBER);\n")
        self.assertIn(b"OK", self.receive_until(second))

    def test_multi_record_requests_and_responses(self):
        self.start_server()
        sock = self.connect()
        self.pg_auth(sock)
        values = ",".join("('" + "x" * 255 + "')" for _ in range(128))
        for sql in ("CREATE TABLE records (v VARCHAR2(255));",
                    f"INSERT INTO records VALUES {values};", "SELECT v FROM records;"):
            query = sql.encode() + b"\0"
            sock.sendall(b"Q" + struct.pack("!I", len(query) + 4) + query)
            messages = []
            while not messages or messages[-1][0] != b"Z":
                messages.append(self.pg_message(sock))
            self.assertNotIn(b"E", [kind for kind, _ in messages])
        rows = [body for kind, body in messages if kind == b"D"]
        self.assertEqual(len(rows), 128)
        for row in rows:
            self.assertEqual(row, struct.pack("!HI", 1, 255) + b"x" * 255)

    def test_tls_plaintext_rejected_and_postgres_upgrade(self):
        if not self.tls:
            self.skipTest("build with OPENDATA_TLS and supply a test certificate")
        self.start_server()
        plain = self.connect(tls=False)
        plain.sendall(f"AUTH {USER} {PASSWORD}\n".encode())
        try:
            self.assertEqual(plain.recv(1024), b"")
        except ConnectionResetError:
            pass
        sock = self.connect(tls=False)
        request = struct.pack("!II", 8, 80877103)
        sock.sendall(request[:3])
        time.sleep(0.02)
        sock.sendall(request[3:])
        self.assertEqual(self.read_exact(sock, 1), b"S")
        sock = self.context.wrap_socket(sock, server_hostname="localhost")
        self.addCleanup(sock.close)
        self.assertIn(sock.version(), ("TLSv1.2", "TLSv1.3"))
        self.pg_auth(sock)


if __name__ == "__main__":
    unittest.main()
