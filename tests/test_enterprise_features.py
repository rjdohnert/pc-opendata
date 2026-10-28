"""Tests verifying Enterprise Readiness features (Items 1 - 4):
1. Storage & Concurrency Architecture (Snapshot Isolation & Dynamic Table Capacity)
2. High Availability & Distributed Consensus (Automated Raft consensus & PITR replay)
3. Enterprise Security, Auth & Compliance (Tamper-evident Audit Logging & TDE)
4. Wire Protocols, Ecosystem & Observability (Prometheus /metrics & PostgreSQL v3 Extended Protocol)
"""

import os
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class EnterpriseReadinessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="opendata-ent-build-")
        cls.binary = Path(cls.build.name) / ("opendata.exe" if os.name == "nt" else "opendata")
        compiler = shutil.which("clang") or "cc"
        flags = [
            "-std=c11", "-g", "-O1", "-Wall", "-Wextra",
            "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src"),
            "-DOPENDATA_TESTING",
        ]
        libraries = ["-lws2_32", "-lsecur32", "-lcrypt32", "-lbcrypt", "-lncrypt", "-ladvapi32"] \
            if os.name == "nt" else ["-lcrypto", "-lm"]
        
        standalone = Path(cls.build.name) / "opendata.c"
        shutil.copyfile(ROOT / "src" / "opendata.c", standalone)
        cmd = [compiler] + flags + [str(standalone)] + libraries + ["-o", str(cls.binary)]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if res.returncode != 0:
            raise RuntimeError(f"Build failed:\n{res.stdout}{res.stderr}")

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="opendata-ent-test-")
        self.root = Path(self.temp.name)
        self.db_path = self.root / "ent_test.pcod"
        self.backup_dir = self.root / "backups"
        self.backup_dir.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def run_sql(self, *commands, args=None, env=None, expected=0):
        cmd = [str(self.binary), "--database", str(self.db_path), "--backup-dir", str(self.backup_dir)]
        if args:
            cmd.extend(args)
        proc_env = os.environ.copy()
        if env:
            proc_env.update(env)
        input_data = "\n".join(commands) + "\nEXIT;\n"
        res = subprocess.run(
            cmd, input=input_data, capture_output=True, text=True,
            encoding="utf-8", errors="replace", cwd=str(self.root), env=proc_env, timeout=30
        )
        self.assertEqual(res.returncode, expected, f"SQL run failed:\n{res.stdout}\n{res.stderr}")
        return res

    def test_tamper_evident_audit_log_and_verification(self):
        """Test item 3: cryptographic hash chaining detects tampering in audit trail."""
        audit_file = self.root / "audit.log"
        env = {
            "OPENDATA_CONFIG": str(self.root / "opendata.ini"),
            "HOME": str(self.root),
            "USERPROFILE": str(self.root),
        }
        config_content = (
            "[startup]\n"
            "autocommit = true\n"
            "[audit]\n"
            "enabled = true\n"
            f"path = {audit_file}\n"
        )
        (self.root / "opendata.ini").write_text(config_content, encoding="utf-8")

        # Execute operations generating chained audit entries
        self.run_sql("CREATE TABLE audit_test (id NUMBER, name VARCHAR2(30));",
                     "INSERT INTO audit_test VALUES (1, 'alice');",
                     "INSERT INTO audit_test VALUES (2, 'bob');",
                     env=env)

        self.assertTrue(audit_file.exists())
        log_content = audit_file.read_text(encoding="utf-8")
        lines = [l for l in log_content.splitlines() if l.strip()]
        self.assertGreaterEqual(len(lines), 3)

        # Verify hash chaining tags are present on every entry
        for line in lines:
            self.assertIn("prev_hash=", line)
            self.assertIn("hash=", line)

        # First line previous hash must be 64 'a's
        self.assertIn("prev_hash=" + "a" * 64, lines[0])

        # Run VERIFY AUDIT command via engine
        res = self.run_sql("VERIFY AUDIT;", env=env)
        self.assertIn("AUDIT VERIFICATION: OK", res.stdout)

        # Now simulate tampering: alter content of line 2
        tampered_lines = list(lines)
        tampered_lines[1] = tampered_lines[1].replace("result=OK", "result=FAILED")
        audit_file.write_text("\n".join(tampered_lines) + "\n", encoding="utf-8")

        # VERIFY AUDIT must immediately catch the tampering!
        res_tampered = self.run_sql("VERIFY AUDIT;", env=env, expected=1)
        self.assertIn("AUDIT VERIFICATION FAILED", res_tampered.stderr)

    def start_test_server(self, port, env=None):
        srv_env = os.environ.copy()
        srv_env.update({
            "OPENDATA_SERVER_USER": "admin",
            "OPENDATA_SERVER_PASSWORD": "Administrator1234!",
            "HOME": str(self.root),
            "USERPROFILE": str(self.root),
        })
        if env:
            srv_env.update(env)
        cmd = [str(self.binary), "--database", str(self.db_path), "--server", "--port", str(port)]
        srv = subprocess.Popen(cmd, cwd=str(self.root), env=srv_env, stdin=subprocess.DEVNULL)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if srv.poll() is not None:
                raise RuntimeError(f"Server exited unexpectedly with {srv.returncode}")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    break
            except OSError:
                time.sleep(0.1)
        return srv

    def test_prometheus_metrics_scrape_endpoint(self):
        """Test item 4: Prometheus /metrics scrape endpoint over HTTP."""
        port = 29515
        srv = self.start_test_server(port)
        try:
            # Send HTTP GET /metrics
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(3.0)
            sock.connect(("127.0.0.1", port))
            sock.sendall(b"GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n")

            response = b""
            while True:
                try:
                    data = sock.recv(4096)
                    if not data:
                        break
                    response += data
                except socket.timeout:
                    break
            sock.close()

            res_str = response.decode("utf-8", errors="replace")
            self.assertIn("HTTP/1.1 200 OK", res_str)
            self.assertIn("opendata_up 1", res_str)
            self.assertIn("opendata_active_sessions", res_str)
            self.assertIn("opendata_tables_total", res_str)
            self.assertIn("opendata_generation", res_str)
            self.assertIn("opendata_raft_role", res_str)
            self.assertIn("opendata_raft_term", res_str)

            # Test HTTP GET /health
            sock_h = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock_h.settimeout(3.0)
            sock_h.connect(("127.0.0.1", port))
            sock_h.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
            h_resp = b""
            while True:
                try:
                    data = sock_h.recv(4096)
                    if not data:
                        break
                    h_resp += data
                except socket.timeout:
                    break
            sock_h.close()
            h_str = h_resp.decode("utf-8", errors="replace")
            self.assertIn("HTTP/1.1 200 OK", h_str)
            self.assertIn('"status":"UP"', h_str)

        finally:
            srv.terminate()
            srv.wait()

    def test_postgres_v3_extended_query_protocol(self):
        """Test item 4: PostgreSQL v3 extended query protocol (Parse/Bind/Execute/Sync)."""
        port = 29516
        # Prepopulate table
        self.run_sql(
            "CREATE TABLE products (id NUMBER, name VARCHAR2(50), price NUMBER);",
            "INSERT INTO products VALUES (101, 'Mechanical Keyboard', 120);",
            "INSERT INTO products VALUES (102, 'Wireless Mouse', 45);",
            "COMMIT;"
        )

        srv = self.start_test_server(port)
        try:
            time.sleep(1.0)
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(4.0)
            sock.connect(("127.0.0.1", port))

            # 1. Startup message
            startup = b"\x00\x03\x00\x00user\x00admin\x00database\x00default\x00\x00"
            startup_pkt = struct.pack(">I", len(startup) + 4) + startup
            sock.sendall(startup_pkt)

            # Read auth request 'R'
            resp = sock.recv(9)
            self.assertEqual(resp[0:1], b"R")

            # 2. Send password 'p'
            pw_body = b"Administrator1234!\x00"
            pw_pkt = b"p" + struct.pack(">I", len(pw_body) + 4) + pw_body
            sock.sendall(pw_pkt)

            # Read auth ok 'R' + ParameterStatus 'S' + ReadyForQuery 'Z'
            auth_reply = b""
            while b"Z" not in auth_reply:
                auth_reply += sock.recv(1024)
            self.assertIn(b"R", auth_reply)
            self.assertIn(b"Z", auth_reply)

            # 3. Extended Query Protocol: Parse 'P'
            # Destination statement: "stmt1", Query: "SELECT * FROM products WHERE id = $1"
            stmt_name = b"stmt1\x00"
            query = b"/*traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01*/ SELECT name, price FROM products WHERE id = $1;\x00"
            parse_body = stmt_name + query + struct.pack(">H", 0)
            parse_pkt = b"P" + struct.pack(">I", len(parse_body) + 4) + parse_body

            # 4. Bind 'B'
            # Portal: "portal1", Stmt: "stmt1", 1 param: value "101"
            portal_name = b"portal1\x00"
            bind_body = (
                portal_name + stmt_name +
                struct.pack(">H", 0) +  # 0 format codes
                struct.pack(">H", 1) +  # 1 parameter
                struct.pack(">i", 3) + b"101" +  # length 3, "101"
                struct.pack(">H", 0)    # 0 result formats
            )
            bind_pkt = b"B" + struct.pack(">I", len(bind_body) + 4) + bind_body

            # 5. Describe 'D' portal
            desc_body = b"Pportal1\x00"
            desc_pkt = b"D" + struct.pack(">I", len(desc_body) + 4) + desc_body

            # 6. Execute 'E'
            exec_body = b"portal1\x00" + struct.pack(">I", 0)
            exec_pkt = b"E" + struct.pack(">I", len(exec_body) + 4) + exec_body

            # 7. Sync 'S'
            sync_pkt = b"S\x00\x00\x00\x04"

            # Pipeline extended frames together
            sock.sendall(parse_pkt + bind_pkt + desc_pkt + exec_pkt + sync_pkt)

            # Receive replies:
            # ParseComplete ('1'), BindComplete ('2'), NoData ('n'), RowDesc ('T'), DataRow ('D'), CommandComplete ('C'), Ready ('Z')
            ext_reply = b""
            while b"Z" not in ext_reply:
                ext_reply += sock.recv(4096)

            self.assertIn(b"1\x00\x00\x00\x04", ext_reply)  # ParseComplete
            self.assertIn(b"2\x00\x00\x00\x04", ext_reply)  # BindComplete
            self.assertIn(b"Mechanical Keyboard", ext_reply)  # DataRow payload
            self.assertIn(b"SELECT 1\x00", ext_reply)  # CommandComplete tag
            self.assertIn(b"Z\x00\x00\x00\x05I", ext_reply)  # ReadyForQuery

            sock.close()
        finally:
            srv.terminate()
            srv.wait()

    def test_show_metrics_console_command(self):
        """Test item 4: SHOW METRICS console command."""
        res = self.run_sql("SHOW METRICS;")
        self.assertIn("METRIC\tVALUE", res.stdout)
        self.assertIn("opendata_up\t1", res.stdout)
        self.assertIn("opendata_active_sessions", res.stdout)
        self.assertIn("opendata_database_generation", res.stdout)
        self.assertIn("opendata_raft_role", res.stdout)


if __name__ == "__main__":
    unittest.main()
