"""Zero-downtime hot-patch restart regression tests.

A running server must hand its listening socket to a successor process so a
patched binary takes over without refusing a single connection, with all
committed data intact and without the RESTART issuer losing their reply.
"""

import os
import re
import shlex
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
USER = "hotpatch-admin"
PASSWORD = "hotpatch-test-password-123456"
READER = "hotpatch-reader"
READER_PASSWORD = "hotpatch-reader-password-123456"


class HotPatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="opendata-hotpatch-build-")
        cls.addClassCleanup(cls.build.cleanup)
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

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="opendata-hotpatch-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = dict(os.environ, OPENDATA_SERVER_USER=USER, OPENDATA_SERVER_PASSWORD=PASSWORD)
        self.successor_pid = None

    def args(self, *extra):
        return [str(self.binary), "--database", str(self.root / "test.pcod"), *extra]

    def start_server(self):
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        self.port = probe.getsockname()[1]
        probe.close()
        self.errors = (self.root / "server.err").open("w+")
        self.output = (self.root / "server.out").open("w+")
        child = subprocess.Popen(self.args("--server", "--port", str(self.port)),
                                 env=self.env, stdin=subprocess.DEVNULL,
                                 stdout=self.output, stderr=self.errors)
        self.server_child = child

        def cleanup():
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=10)
            if self.successor_pid:
                try:
                    os.kill(self.successor_pid, signal.SIGTERM)
                except OSError:
                    pass
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if not self.pid_running(self.successor_pid):
                        break
                    time.sleep(0.05)
            self.errors.close()
            self.output.close()

        self.addCleanup(cleanup)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if child.poll() is not None:
                self.errors.seek(0)
                self.fail(f"server exited {child.returncode}: {self.errors.read()}")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.1):
                    return child
            except OSError:
                time.sleep(0.02)
        self.fail("server did not become responsive")

    @staticmethod
    def pid_running(pid):
        if os.name == "nt":
            result = subprocess.run(
                ["tasklist", "/FI", f"PID eq {pid}", "/NH", "/FO", "CSV"],
                capture_output=True, text=True, timeout=10)
            return f'"{pid}"' in result.stdout
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False

    def connect(self):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        self.addCleanup(sock.close)
        return sock

    @staticmethod
    def receive_until(sock, marker=b"> "):
        # On POSIX the exec-confirmation poll can take up to 5s (WSL2 kernel
        # bug workaround), so allow a generous receive window for RESTART.
        sock.settimeout(15)
        data = bytearray()
        while marker not in data:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data.extend(chunk)
        return bytes(data)

    def auth(self, sock, user=USER, password=PASSWORD):
        sock.sendall(f"AUTH {user} {password}\n".encode())
        response = self.receive_until(sock)
        if b"Authentication required:" in response and b"Authenticated" not in response:
            response += self.receive_until(sock)
        self.assertIn(b"Authenticated", response)

    def query(self, sock, sql):
        sock.sendall(sql.encode() + b"\n")
        return self.receive_until(sock)

    def read_server_errors(self):
        # Read through a separate handle: the running successor shares the
        # write handle's file offset, so seeking self.errors would corrupt it.
        try:
            return (self.root / "server.err").read_text()
        except OSError:
            return ""

    def await_successor_pid(self):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            match = re.search(r"hot-patch successor pid=(\d+)", self.read_server_errors())
            if match:
                self.successor_pid = int(match.group(1))
                return
            time.sleep(0.05)
        self.fail(f"successor never adopted the listener: {self.read_server_errors()}")

    def test_restart_zero_downtime_and_data_survives(self):
        self.start_server()
        admin = self.connect()
        self.auth(admin)
        self.assertIn(b"OK", self.query(admin, "CREATE TABLE hp (id NUMBER PRIMARY KEY, note VARCHAR2(32));"))
        self.assertIn(b"OK", self.query(admin, "INSERT INTO hp VALUES (1, 'before-restart');"))
        self.assertIn(b"OK", self.query(admin, "COMMIT;"))

        refusals = []
        halt = threading.Event()

        def probe():
            while not halt.is_set():
                try:
                    # On POSIX the WSL2 kernel bug stalls the exec-confirmation
                    # poll for up to 5s; use a longer timeout there so the probe
                    # measures refusals, not the stall.
                    timeout = 10 if os.name != "nt" else 2
                    with socket.create_connection(("127.0.0.1", self.port), timeout=timeout):
                        pass
                except OSError as exc:
                    refusals.append(exc)

        thread = threading.Thread(target=probe, daemon=True)
        thread.start()
        try:
            admin.sendall(b"RESTART;\n")
            response = self.receive_until(admin)
            self.assertIn(b"OK", response)
            admin.close()

            self.server_child.wait(timeout=15)
            self.assertEqual(self.server_child.returncode, 0)
            self.await_successor_pid()
            self.assertNotEqual(self.successor_pid, self.server_child.pid)

            deadline = time.monotonic() + 15
            while True:
                try:
                    sock = self.connect()
                    self.auth(sock)
                    reply = self.query(sock, "SELECT note FROM hp WHERE id = 1;")
                    if b"before-restart" in reply:
                        break
                except OSError:
                    pass
                if time.monotonic() > deadline:
                    self.fail("successor did not resume serving committed data")
                time.sleep(0.05)
        finally:
            halt.set()
            thread.join(timeout=5)
        self.assertEqual(refusals, [])

        # The successor is a fully functional server: it accepts new writes.
        sock = self.connect()
        self.auth(sock)
        self.assertIn(b"OK", self.query(sock, "INSERT INTO hp VALUES (2, 'after-restart');"))
        self.assertIn(b"OK", self.query(sock, "COMMIT;"))
        reply = self.query(sock, "SELECT note FROM hp WHERE id = 2;")
        self.assertIn(b"after-restart", reply)

    def test_read_only_role_cannot_restart(self):
        self.start_server()
        admin = self.connect()
        self.auth(admin)
        self.assertIn(b"OK", self.query(
            admin, f"CREATE USER '{READER}' IDENTIFIED BY '{READER_PASSWORD}' ROLE READ_ONLY;"))
        self.assertIn(b"OK", self.query(admin, "COMMIT;"))
        reader = self.connect()
        self.auth(reader, READER, READER_PASSWORD)
        reader.sendall(b"RESTART;\n")
        self.assertIn(b"permission denied", self.receive_until(reader))
        # The restart was rejected and the original server keeps serving.
        self.assertIsNone(self.server_child.poll())
        self.assertIn(b"DUMMY\r\nX\r\n", self.query(admin, "SELECT * FROM DUAL;"))

    def test_restart_requires_server_mode(self):
        result = subprocess.run(
            [str(self.binary), "--database", str(self.root / "local.pcod"),
             "--execute", "RESTART;"],
            env=self.env, capture_output=True, text=True, timeout=30)
        self.assertIn("server mode", result.stderr)

    @unittest.skipIf(os.name == "nt", "SIGUSR2 is POSIX-only")
    def test_sigusr2_triggers_hot_patch(self):
        self.start_server()
        admin = self.connect()
        self.auth(admin)
        self.assertIn(b"OK", self.query(admin, "CREATE TABLE sig (id NUMBER PRIMARY KEY);"))
        self.assertIn(b"OK", self.query(admin, "INSERT INTO sig VALUES (7);"))
        self.assertIn(b"OK", self.query(admin, "COMMIT;"))
        self.server_child.send_signal(signal.SIGUSR2)
        self.server_child.wait(timeout=15)
        self.assertEqual(self.server_child.returncode, 0)
        self.await_successor_pid()
        deadline = time.monotonic() + 15
        while True:
            try:
                sock = self.connect()
                self.auth(sock)
                reply = self.query(sock, "SELECT id FROM sig;")
                if b"7" in reply:
                    break
            except OSError:
                pass
            if time.monotonic() > deadline:
                self.fail("successor did not resume after SIGUSR2")
            time.sleep(0.05)

    @unittest.skipIf(os.name == "nt", "SIGTERM graceful-stop check is POSIX-specific")
    def test_sigterm_stops_gracefully_without_restart(self):
        self.start_server()
        self.server_child.send_signal(signal.SIGTERM)
        self.server_child.wait(timeout=15)
        self.assertEqual(self.server_child.returncode, 0)
        self.assertNotIn("hot-patch successor", self.read_server_errors())

    def test_predecessor_exits_zero_despite_prior_query_errors(self):
        self.start_server()
        admin = self.connect()
        self.auth(admin)
        # A routine client SQL failure increments the lifetime error counter.
        admin.sendall(b"SELECT * FROM missing_table;\n")
        self.assertIn(b"ERROR", self.receive_until(admin))
        admin.sendall(b"RESTART;\n")
        self.assertIn(b"OK", self.receive_until(admin))
        admin.close()
        self.server_child.wait(timeout=15)
        self.assertEqual(self.server_child.returncode, 0)
        self.await_successor_pid()

    def test_chained_restart(self):
        self.start_server()
        admin = self.connect()
        self.auth(admin)
        self.assertIn(b"OK", self.query(admin, "CREATE TABLE chain (id NUMBER PRIMARY KEY);"))
        self.assertIn(b"OK", self.query(admin, "INSERT INTO chain VALUES (1);"))
        self.assertIn(b"OK", self.query(admin, "COMMIT;"))
        # First restart.
        admin.sendall(b"RESTART;\n")
        self.assertIn(b"OK", self.receive_until(admin))
        admin.close()
        self.server_child.wait(timeout=15)
        self.assertEqual(self.server_child.returncode, 0)
        self.await_successor_pid()
        first_pid = self.successor_pid
        # Second restart from the successor.
        deadline = time.monotonic() + 15
        sock = None
        while time.monotonic() < deadline:
            try:
                sock = self.connect()
                self.auth(sock)
                break
            except OSError:
                time.sleep(0.05)
        self.assertIsNotNone(sock)
        sock.sendall(b"RESTART;\n")
        self.assertIn(b"OK", self.receive_until(sock))
        sock.close()
        # Wait for the second successor to adopt the listener.
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            pids = re.findall(r"hot-patch successor pid=(\d+)", self.read_server_errors())
            if len(pids) >= 2:
                self.successor_pid = int(pids[-1])
                break
            time.sleep(0.05)
        self.assertIsNotNone(self.successor_pid)
        self.assertNotEqual(self.successor_pid, first_pid)
        # Data survives both restarts.
        deadline = time.monotonic() + 15
        while True:
            try:
                sock = self.connect()
                self.auth(sock)
                reply = self.query(sock, "SELECT id FROM chain;")
                if b"1" in reply:
                    break
            except OSError:
                pass
            if time.monotonic() > deadline:
                self.fail("second successor did not resume serving committed data")
            time.sleep(0.05)

    def test_spawn_failure_aborts_and_predecessor_keeps_serving(self):
        self.start_server()
        admin = self.connect()
        self.auth(admin)
        self.assertIn(b"OK", self.query(admin, "CREATE TABLE sf (id NUMBER PRIMARY KEY);"))
        self.assertIn(b"OK", self.query(admin, "INSERT INTO sf VALUES (1);"))
        self.assertIn(b"OK", self.query(admin, "COMMIT;"))
        # Replace the binary with a non-executable so the successor cannot launch.
        binary = Path(self.server_child.args[0])
        backup = binary.with_suffix(".bak")
        binary.rename(backup)
        binary.write_text("not a valid executable")
        try:
            admin.sendall(b"RESTART;\n")
            response = self.receive_until(admin)
            # The restart is accepted (OK) but the spawn fails; the predecessor
            # logs the abort and keeps serving.
            self.assertIn(b"OK", response)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if "hot-patch restart aborted" in self.read_server_errors():
                    break
                time.sleep(0.05)
            self.assertIn("hot-patch restart aborted", self.read_server_errors())
            # The predecessor is still alive and serving.
            self.assertIsNone(self.server_child.poll())
            self.assertIn(b"1", self.query(admin, "SELECT id FROM sf;"))
        finally:
            binary.unlink()
            backup.rename(binary)


if __name__ == "__main__":
    unittest.main()
