import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)

    def run_cli(self, *args, ok=True):
        p = subprocess.run([BINARY, "--home", str(self.home), *args], text=True,
                           capture_output=True, timeout=10)
        if ok:
            self.assertEqual(p.returncode, 0, p.stderr)
        else:
            self.assertNotEqual(p.returncode, 0)
        return p

    def status(self):
        return json.loads(self.run_cli("status", "--json").stdout)

    def resource(self):
        self.run_cli("mcp", "add", "teams", "--command", "/usr/bin/true")
        self.run_cli("mcp", "group", "add", "communication")
        self.run_cli("mcp", "group", "include", "communication", "mcp:teams")

    def test_all_eight_agent_assignments(self):
        self.resource()
        agents = ["codex", "claude", "opencode"]
        for mask in range(8):
            selected = [a for bit, a in enumerate(agents) if mask & (1 << bit)]
            self.run_cli("mcp", "group", "assign", "communication", "--agents", ",".join(selected))
            row = self.status()["mcps"]["teams"]["agents"]
            for a in agents:
                self.assertEqual(row[a]["enabled"], a in selected, (mask, a))

    def test_union_and_explicit_off(self):
        self.resource()
        self.run_cli("mcp", "group", "assign", "communication", "--agents", "codex,claude")
        self.run_cli("mcp", "group", "add", "engineering")
        self.run_cli("mcp", "group", "include", "engineering", "mcp:teams")
        self.run_cli("mcp", "group", "assign", "engineering", "--agents", "codex")
        self.run_cli("mcp", "group", "disable", "communication")
        row = self.status()["mcps"]["teams"]["agents"]
        self.assertTrue(row["codex"]["enabled"])
        self.assertFalse(row["claude"]["enabled"])
        self.run_cli("mcp", "disable", "teams", "--agent", "codex")
        self.assertFalse(self.status()["mcps"]["teams"]["agents"]["codex"]["enabled"])
        self.run_cli("mcp", "inherit", "teams", "--agent", "codex")
        self.assertTrue(self.status()["mcps"]["teams"]["agents"]["codex"]["enabled"])

    def test_global_off_wins(self):
        self.resource()
        self.run_cli("mcp", "enable", "teams", "--agent", "opencode")
        self.run_cli("mcp", "disable", "teams")
        self.assertFalse(self.status()["mcps"]["teams"]["agents"]["opencode"]["enabled"])

    def test_invalid_change_preserves_catalog(self):
        self.resource()
        path = self.home / ".config/kitmux/catalog.json"
        before = path.read_bytes()
        self.run_cli("mcp", "group", "include", "communication", "mcp:missing", ok=False)
        self.run_cli("mcp", "group", "assign", "communication", "--agents", "unknown", ok=False)
        self.run_cli("mcp", "group", "add", "../escape", ok=False)
        self.assertEqual(path.read_bytes(), before)

    def test_remove_cleans_membership(self):
        self.resource()
        self.run_cli("mcp", "remove", "teams")
        state = self.status()
        self.assertNotIn("teams", state["mcps"])
        self.assertEqual(state["groups"]["mcp:communication"]["members"], [])

    def test_corrupt_catalog_is_not_overwritten(self):
        self.resource()
        path = self.home / ".config/kitmux/catalog.json"
        path.write_text('{"version":1,"groups":')
        self.run_cli("mcp", "group", "add", "other", ok=False)
        self.assertEqual(path.read_text(), '{"version":1,"groups":')

    def test_non_tty_does_not_wait_for_input(self):
        p = self.run_cli()
        self.assertIn("kitmux", p.stdout)

    def test_invalid_catalog_types_and_control_text_are_rejected(self):
        self.resource()
        path = self.home / ".config/kitmux/catalog.json"
        original = json.loads(path.read_text())
        cases = [
            ("command", ["/usr/bin/true", "bad\x00argument"]),
            ("command", [42]),
            ("transport", "unsupported"),
            ("env", {"TOKEN": 42}),
        ]
        for key, value in cases:
            data = json.loads(json.dumps(original))
            data["mcps"]["teams"][key] = value
            path.write_text(json.dumps(data))
            self.run_cli("status", "--json", ok=False)
        original["groups"]["mcp:communication"]["members"] = ["mcp:teams\x1b[2J"]
        path.write_text(json.dumps(original))
        self.run_cli("status", ok=False)

    def test_status_never_exposes_credentials(self):
        self.run_cli("mcp", "add", "remote", "--url", "https://example.test/mcp?token=private-token",
                     "--env", "TOKEN=private-token")
        self.assertNotIn("private-token", self.run_cli("status", "--json").stdout)
        self.assertNotIn("private-token", self.run_cli("mcp", "list").stdout)

    def test_catalog_symlink_is_rejected(self):
        self.resource()
        path = self.home / ".config/kitmux/catalog.json"
        external = self.home / "external.json"
        external.write_bytes(path.read_bytes())
        path.unlink()
        path.symlink_to(external)
        before = external.read_bytes()
        self.run_cli("mcp", "group", "add", "new", ok=False)
        self.assertEqual(external.read_bytes(), before)

    def test_unknown_list_option_is_rejected(self):
        self.run_cli("mcp", "group", "list", "--surprise", ok=False)

    def test_imported_endpoint_edit_cannot_carry_credentials(self):
        self.run_cli("mcp", "add", "remote", "--url", "https://original.test/mcp")
        path = self.home / ".config/kitmux/catalog.json"
        data = json.loads(path.read_text())
        data["mcps"]["remote"]["native"] = {"claude": {
            "type": "http", "url": "https://original.test/mcp",
            "headers": {"Authorization": "Bearer private-token"}}}
        path.write_text(json.dumps(data))
        before = path.read_bytes()
        result = self.run_cli("mcp", "edit", "remote", "--url", "https://different.test/mcp", ok=False)
        self.assertNotIn("private-token", result.stdout + result.stderr)
        self.assertEqual(path.read_bytes(), before)

    def test_save_cannot_exceed_its_own_read_limit(self):
        self.resource()
        path = self.home / ".config/kitmux/catalog.json"
        data = json.loads(path.read_text())
        data["mcps"]["teams"]["native"] = {"codex": {"metadata": [0] * 600000}}
        path.write_text(json.dumps(data, separators=(",", ":")))
        before = path.read_bytes()
        self.run_cli("mcp", "group", "add", "extra", ok=False)
        self.assertEqual(path.read_bytes(), before)
        self.run_cli("status", "--json")


if __name__ == "__main__":
    unittest.main()
