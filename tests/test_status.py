import fcntl
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class StatusTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="km-status-", dir="/tmp")
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        config = self.home / ".config/kitmux"
        config.mkdir(parents=True)
        self.catalog = config / "catalog.json"
        self.document = {"version": 1, "revision": 1, "groups": {}, "skills": {}, "mcps": {
            "local": {"enabled": True, "agents": {"codex": "on"}, "transport": "stdio",
                      "mode": "managed", "command": ["/not/run", "private-argument"],
                      "env": {"SECRET": "private-value"}},
            "remote": {"enabled": True, "agents": {}, "transport": "http",
                       "mode": "direct", "url": "https://example.test/mcp?token=private-token"}}}
        self.catalog.write_text(json.dumps(self.document))

    def run_cli(self, *args):
        return subprocess.run([BINARY, "--home", str(self.home), *args],
                              capture_output=True, text=True, timeout=10)

    def test_local_status_without_bridge_is_inactive_and_read_only(self):
        before = self.catalog.read_bytes()
        result = self.run_cli("mcp", "status", "local", "--agent", "codex", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        status = json.loads(result.stdout)
        self.assertEqual(status["state"], "inactive")
        self.assertEqual(status["connections"], [])
        self.assertTrue(status["enabled"])
        self.assertEqual(before, self.catalog.read_bytes())

    def test_status_queries_do_not_require_the_catalog_writer_lock(self):
        with (self.catalog.parent / "catalog.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_cli("mcp", "status", "remote", "--agent", "claude", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        status = json.loads(result.stdout)
        self.assertEqual(status["state"], "unverified")
        self.assertFalse(status["enabled"])

    def test_general_status_reports_evidence_without_secrets(self):
        result = self.run_cli("status", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        row = json.loads(result.stdout)["mcps"]["local"]["agents"]["codex"]
        self.assertEqual(row["runtime"], "inactive")
        self.assertEqual(row["runtime_status"]["reachable"], 0)
        self.assertEqual(row["refresh"], "unverified")
        for secret in ("private-argument", "private-value", "private-token"):
            self.assertNotIn(secret, result.stdout + result.stderr)

    def test_status_arguments_are_checked(self):
        cases = [("missing", "--agent", "codex"), ("local",),
                 ("local", "--agent", "unknown"),
                 ("local", "--agent", "codex", "--agent", "claude"),
                 ("local", "--agent", "codex", "--unexpected")]
        for args in cases:
            with self.subTest(args=args):
                self.assertNotEqual(self.run_cli("mcp", "status", *args).returncode, 0)


if __name__ == "__main__":
    unittest.main()
