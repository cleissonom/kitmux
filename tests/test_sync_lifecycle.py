import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class SyncLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.cli("mcp", "add", "example", "--command", "/usr/bin/true")
        self.cli("mcp", "enable", "example", "--agent", "claude")

    def cli(self, *args, ok=True):
        p = subprocess.run([BINARY, "--home", str(self.home), *args],
                           capture_output=True, text=True, timeout=15)
        self.assertEqual(p.returncode == 0, ok, p.stderr)
        return p

    def test_toggle_auto_applies_only_previously_adopted_clients(self):
        self.cli("sync", "--apply", "--agent", "claude")
        self.cli("mcp", "disable", "example", "--agent", "claude")
        native = json.loads((self.home / ".claude.json").read_text())
        self.assertNotIn("example", native["mcpServers"])
        self.assertFalse((self.home / ".codex/config.toml").exists())
        self.assertFalse((self.home / ".config/opencode/opencode.json").exists())
        self.cli("mcp", "enable", "example", "--agent", "claude")
        self.assertIn("example", json.loads((self.home / ".claude.json").read_text())["mcpServers"])

    def test_conflict_retains_desired_catalog_without_overwriting_native(self):
        self.cli("sync", "--apply", "--agent", "claude")
        path = self.home / ".claude.json"
        native = json.loads(path.read_text())
        native["mcpServers"]["example"]["command"] = "/usr/bin/false"
        path.write_text(json.dumps(native))
        before = path.read_bytes()
        result = self.cli("mcp", "disable", "example", "--agent", "claude", ok=False)
        self.assertIn("saved", result.stderr.lower())
        self.assertEqual(path.read_bytes(), before)
        state = json.loads((self.home / ".config/kitmux/catalog.json").read_text())
        self.assertEqual(state["mcps"]["example"]["agents"]["claude"], "off")

    def test_removal_detaches_adopted_entry(self):
        self.cli("sync", "--apply", "--agent", "claude")
        self.cli("mcp", "remove", "example")
        self.assertNotIn("example", json.loads((self.home / ".claude.json").read_text())["mcpServers"])


if __name__ == "__main__":
    unittest.main()
