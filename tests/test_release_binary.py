"""Smoke-test a prebuilt executable without compiling a substitute."""

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


class ReleaseBinaryTests(unittest.TestCase):
    def setUp(self):
        supplied = os.environ.get("OPENDATA_TEST_BINARY")
        if not supplied:
            self.skipTest("OPENDATA_TEST_BINARY must identify the executable to validate")
        self.binary = Path(supplied).resolve(strict=True)
        self.work = tempfile.TemporaryDirectory(prefix="opendata-release-")
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        (self.root / "backups").mkdir()

    def run_sql(self, statement, expected=0, commit=False):
        command = [str(self.binary), "--database", str(self.root / "release.pcod"),
                   "--backup-dir", str(self.root / "backups"), "--format", "json"]
        if not commit:
            command += ["--execute", statement]
        result = subprocess.run(
            command, input=statement + "\nCOMMIT;\nEXIT\n" if commit else "",
            cwd=self.root, capture_output=True,
            text=True, timeout=30,
        )
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def test_version(self):
        result = subprocess.run(
            [str(self.binary), "--version"], cwd=self.root, capture_output=True,
            text=True, timeout=10, check=True,
        )
        self.assertIn("PC/OpenData", result.stdout)

    def test_sql_survives_restart(self):
        self.run_sql("CREATE TABLE release_check (id NUMBER PRIMARY KEY, label VARCHAR2(40));",
                     commit=True)
        self.run_sql("INSERT INTO release_check VALUES (17, 'release-persisted');", commit=True)
        result = self.run_sql("SELECT id, label FROM release_check;")
        rows = json.loads(result.stdout)
        self.assertEqual(len(rows), 1)
        self.assertEqual(str(rows[0]["ID"]), "17")
        self.assertEqual(rows[0]["LABEL"], "release-persisted")


if __name__ == "__main__":
    unittest.main()
