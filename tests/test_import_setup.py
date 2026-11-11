import json
import os
from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class ImportSetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name)

    def cli(self, *args):
        result = subprocess.run([BINARY, "--home", str(self.home), *args],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def test_import_merges_same_native_name_with_per_client_execution_variants(self):
        codex_text = '''# Keep the Codex comment and unrelated setting.
approval_policy = "on-request"

[mcp_servers.shared]
command = "/fixture/codex-server" # Codex executable
args = ["--stdio"]
enabled = true
env = { CODEX_ONLY = "fixture-value" }
'''
        claude_text = json.dumps({"theme": "dark", "mcpServers": {
            "shared": {"command": "/fixture/claude-server", "args": ["--stdio"]}}}, indent=2) + "\n"
        opencode_text = json.dumps({"theme": "dark", "mcp": {"servers": {
            "shared": {"type": "local", "command": ["/fixture/opencode-server", "--stdio"]}}}}, indent=2) + "\n"

        codex = self.home / ".codex/config.toml"
        claude = self.home / ".claude.json"
        opencode = self.home / ".config/opencode/opencode.json"
        codex.parent.mkdir(parents=True, exist_ok=True)
        claude.parent.mkdir(parents=True, exist_ok=True)
        opencode.parent.mkdir(parents=True, exist_ok=True)
        codex.write_text(codex_text)
        claude.write_text(claude_text)
        opencode.write_text(opencode_text)
        originals = {codex: codex.read_bytes(), claude: claude.read_bytes(), opencode: opencode.read_bytes()}

        self.cli("import", "--preview")
        self.assertFalse((self.home / ".config/kitmux/catalog.json").exists())
        self.assertEqual({path: path.read_bytes() for path in originals}, originals)

        self.cli("import", "--apply")
        catalog_path = self.home / ".config/kitmux/catalog.json"
        catalog = json.loads(catalog_path.read_text())
        self.assertEqual(set(catalog["mcps"]), {"shared"})
        server = catalog["mcps"]["shared"]
        self.assertEqual(set(server["native"]), {"codex", "claude", "opencode"})
        self.assertEqual(server["native"]["codex"]["command"], "/fixture/codex-server")
        self.assertEqual(server["native"]["claude"]["command"], "/fixture/claude-server")
        self.assertEqual(server["native"]["opencode"]["command"], ["/fixture/opencode-server", "--stdio"])
        self.assertEqual({path: path.read_bytes() for path in originals}, originals)

        self.cli("sync", "--apply")
        self.assertEqual({path: path.read_bytes() for path in originals}, originals)
        self.assertIn("# Keep the Codex comment and unrelated setting.", codex.read_text())
        self.assertEqual(tomllib.loads(codex.read_text())["mcp_servers"]["shared"]["command"],
                         "/fixture/codex-server")

    def test_reimport_preserves_managed_upstreams_and_does_not_adopt_bridge_command(self):
        path = self.home / ".codex/config.toml"
        path.parent.mkdir(parents=True)
        original = '[mcp_servers.local]\ncommand = "/usr/bin/true"\n'
        path.write_text(original)
        self.cli("import", "--from", "codex", "--apply")
        self.cli("mcp", "edit", "local", "--mode", "managed")
        self.cli("mcp", "disable", "local", "--agent", "codex")
        catalog_path = self.home / ".config/kitmux/catalog.json"
        before = json.loads(catalog_path.read_text())["mcps"]["local"]
        self.cli("import", "--from", "codex", "--apply")
        after = json.loads(catalog_path.read_text())["mcps"]["local"]
        self.assertEqual(before, after)
        self.assertEqual(after["agents"]["codex"], "off")
        self.cli("mcp", "enable", "local", "--agent", "codex")
        self.cli("mcp", "edit", "local", "--mode", "direct")
        restored = tomllib.loads(path.read_text())["mcp_servers"]["local"]
        self.assertEqual(restored["command"], "/usr/bin/true")
        self.assertNotIn("args", restored)
        self.cli("restore", "--agent", "codex", "--apply")
        self.assertEqual(path.read_text(), original)

    def test_malformed_native_snapshots_fail_before_projection(self):
        catalog = self.home / ".config/kitmux/catalog.json"
        catalog.parent.mkdir(parents=True)
        malformed = [[], {"codex": 1}, {"claude": None}, {"unknown": {}}, {"opencode": "private-snapshot"}]
        for native in malformed:
            with self.subTest(native=native):
                document = {"version": 1, "revision": 1, "groups": {}, "skills": {}, "mcps": {
                    "fixture": {"enabled": True, "agents": {}, "transport": "stdio",
                                "command": ["/usr/bin/true"], "native": native}}}
                catalog.write_text(json.dumps(document))
                before = catalog.read_bytes()
                result = subprocess.run([BINARY, "--home", str(self.home), "status", "--json"],
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 1)
                self.assertIn("native", result.stderr.lower())
                self.assertNotIn("private-snapshot", result.stdout + result.stderr)
                self.assertEqual(catalog.read_bytes(), before)
                self.assertFalse((self.home / ".codex/config.toml").exists())

    def test_failed_catalog_commit_does_not_adopt_native_ownership(self):
        catalog = self.home / ".config/kitmux/catalog.json"
        catalog.parent.mkdir(parents=True)
        document = {"version": 1, "revision": 1, "groups": {}, "skills": {}, "mcps": {},
                    "notes": "x" * (8 * 1024 * 1024 - 400)}
        catalog.write_text(json.dumps(document, separators=(",", ":")))
        before = catalog.read_bytes()
        native = self.home / ".codex/config.toml"
        native.parent.mkdir()
        original = '[mcp_servers.fixture]\ncommand = "/usr/bin/true"\nenv = { X = "' + 'y' * 1024 + '" }\n'
        native.write_text(original)
        result = subprocess.run([BINARY, "--home", str(self.home), "import", "--from", "codex", "--apply"],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(catalog.read_bytes(), before)
        self.assertEqual(native.read_text(), original)
        state = self.home / ".local/state/kitmux/adapters.json"
        self.assertFalse(state.exists(), "Failed import must not claim native ownership without its catalog")

    def test_imported_opencode_default_flag_returns_to_original_after_toggle(self):
        path = self.home / ".config/opencode/opencode.jsonc"
        path.parent.mkdir(parents=True)
        original = '{ // preserve comment\n "mcp":{"servers":{"local":{"type":"local","command":["/usr/bin/true"]}}}\n}\n'
        path.write_text(original)
        self.cli("import", "--from", "opencode", "--apply")
        self.cli("mcp", "disable", "local", "--agent", "opencode")
        self.cli("mcp", "enable", "local", "--agent", "opencode")
        self.assertIn('"disabled": false', path.read_text())
        self.assertIn("// preserve comment", path.read_text())
        self.cli("restore", "--agent", "opencode", "--apply")
        self.assertEqual(path.read_text(), original)


if __name__ == "__main__":
    unittest.main()
