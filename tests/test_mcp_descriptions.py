import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_tui import TerminalTestCase


BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class McpDescriptionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="km-doc-", dir="/tmp")
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.catalog = self.home / ".config/kitmux/catalog.json"

    def cli(self, *args, ok=True):
        result = subprocess.run([BINARY, "--home", str(self.home), *args],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0 if ok else 1, result.stdout + result.stderr)
        return result

    def write(self, relative, text):
        target = self.home / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
        return target

    def resource(self):
        return json.loads(self.catalog.read_text())["mcps"]["docs"]

    def test_add_and_metadata_only_edit_accept_markdown_and_preserve_execution(self):
        markdown = "# Documentation\n\n- Search the local manuals.\n"
        self.cli("mcp", "add", "docs", "--command", "/usr/bin/true", "--description", markdown)
        self.assertEqual(self.resource()["description"], markdown)
        before = self.resource()
        self.cli("mcp", "edit", "docs", "--description", "Updated purpose")
        expected = dict(before, description="Updated purpose")
        self.assertEqual(self.resource(), expected)
        saved = self.catalog.read_bytes()
        self.cli("mcp", "edit", "docs", "--description", "Updated purpose")
        self.assertEqual(self.catalog.read_bytes(), saved)
        self.cli("mcp", "edit", "docs", "--description", "")
        self.assertEqual(self.resource()["description"], "")

    def test_file_description_larger_than_tui_capture_is_loaded_in_full(self):
        markdown = "# Long documentation\n" + "Details.\n" * 6000 + "Last section.\n"
        source = self.write("description.md", markdown)
        self.cli("mcp", "add", "docs", "--url", "https://example.test/mcp",
                 "--description-file", str(source))
        self.assertEqual(self.resource()["description"], markdown)
        before = self.resource()["description"]
        self.cli("mcp", "edit", "docs", "--mode", "direct")
        self.assertEqual(self.resource()["description"], before)

    def test_invalid_description_options_leave_existing_catalog_unchanged(self):
        self.cli("mcp", "add", "docs", "--command", "/usr/bin/true")
        before = self.catalog.read_bytes()
        source = self.write("description.md", "Documentation")
        invalid = self.home / "invalid.md"
        for body in (b"bad\x00description", b"bad\xffdescription", b"x" * (8 * 1024 * 1024 + 1)):
            invalid.write_bytes(body)
            self.cli("mcp", "edit", "docs", "--description-file", str(invalid), ok=False)
            self.assertEqual(self.catalog.read_bytes(), before)
        for options in (("--description", "one", "--description", "two"),
                        ("--description", "one", "--description-file", str(source)),
                        ("--description-file", str(self.home / "missing.md")),
                        ("--description-file", str(self.home)), ("--description",)):
            self.cli("mcp", "edit", "docs", *options, ok=False)
            self.assertEqual(self.catalog.read_bytes(), before)

    def test_native_provisioning_description_never_changes_native_options(self):
        definition = {"url": "https://example.test/mcp", "http_headers": {"Authorization": "fixture-secret"}}
        source = self.write("native.json", json.dumps(definition))
        self.cli("mcp", "add", "docs", "--from", str(source), "--format", "codex",
                 "--description", "# Manuals\nSearch documentation.")
        self.assertEqual(self.resource()["native"]["codex"], definition)
        self.assertEqual(self.resource()["description"], "# Manuals\nSearch documentation.")
        update = self.write("update.md", "Updated Markdown.")
        self.cli("mcp", "edit", "docs", "--from", str(source), "--format", "codex",
                 "--description-file", str(update))
        self.assertEqual(self.resource()["description"], "Updated Markdown.")
        self.assertEqual(self.resource()["native"]["codex"], definition)
        self.assertNotIn("fixture-secret", self.cli("mcp", "show", "docs").stdout)
        self.assertNotIn("description", self.cli("mcp", "show", "docs").stdout)

    def test_import_and_refresh_preserve_user_documentation_and_native_bytes(self):
        source = self.write(".codex/config.toml", '[mcp_servers.docs]\ncommand = "/usr/bin/true"\n')
        self.cli("import", "--from", "codex", "--apply")
        self.cli("mcp", "edit", "docs", "--description", "My user-authored documentation")
        native_before = source.read_bytes()
        self.cli("import", "--from", "codex", "--apply")
        self.assertEqual(self.resource()["description"], "My user-authored documentation")
        self.assertEqual(source.read_bytes(), native_before)
        source.write_text('[mcp_servers.docs]\ncommand = "/usr/bin/false"\n')
        native_before = source.read_bytes()
        self.cli("import", "--refresh", "--from", "codex", "--apply")
        self.assertEqual(self.resource()["description"], "My user-authored documentation")
        self.assertEqual(source.read_bytes(), native_before)

    def test_import_adopts_existing_string_description_but_never_replaces_user_text(self):
        source = self.write(".claude.json", json.dumps({"mcpServers": {"docs": {
            "command": "/usr/bin/true", "description": "Imported purpose"}}}))
        self.cli("import", "--from", "claude", "--apply")
        self.assertEqual(self.resource()["description"], "Imported purpose")
        self.cli("mcp", "edit", "docs", "--description", "My purpose")
        document = json.loads(source.read_text())
        document["mcpServers"]["docs"]["description"] = "Externally edited purpose"
        source.write_text(json.dumps(document))
        self.cli("import", "--refresh", "--from", "claude", "--apply")
        self.assertEqual(self.resource()["description"], "My purpose")

    def test_later_client_can_supply_missing_documentation(self):
        self.write(".codex/config.toml", '[mcp_servers.docs]\ncommand = "/usr/bin/true"\n')
        self.write(".claude.json", json.dumps({"mcpServers": {"docs": {
            "command": "/usr/bin/true", "description": "Client documentation"}}}))
        self.cli("import", "--from", "codex,claude", "--apply")
        self.assertEqual(self.resource()["description"], "Client documentation")
        self.cli("mcp", "edit", "docs", "--description", "")
        self.cli("import", "--from", "codex,claude", "--apply")
        self.assertEqual(self.resource()["description"], "")

    def test_native_update_can_supply_missing_documentation(self):
        self.cli("mcp", "add", "docs", "--command", "/usr/bin/true")
        source = self.write("native.json", json.dumps({"command": "/usr/bin/true",
                                                       "description": "Native documentation"}))
        self.cli("mcp", "edit", "docs", "--from", str(source), "--format", "claude")
        self.assertEqual(self.resource()["description"], "Native documentation")

    def test_description_text_and_upstream_arguments_are_not_native_source_flags(self):
        self.cli("mcp", "add", "docs", "--command", "/usr/bin/true",
                 "--description", "--from", "--", "--from", "upstream-file")
        self.assertEqual(self.resource()["description"], "--from")
        self.assertEqual(self.resource()["command"], ["/usr/bin/true", "--from", "upstream-file"])

    def test_v04_catalogs_load_and_invalid_description_metadata_is_rejected(self):
        self.catalog.parent.mkdir(parents=True)
        base = {"version": 3, "revision": 7, "groups": {}, "skills": {}, "subagents": {},
                "mcps": {"docs": {"enabled": True, "agents": {}, "mode": "direct",
                                  "transport": "stdio", "command": ["/usr/bin/true"]}}}
        self.catalog.write_text(json.dumps(base))
        self.cli("mcp", "show", "docs")
        for invalid in (None, {}, [], 42, "secret\x00bad"):
            with self.subTest(invalid=invalid):
                base["mcps"]["docs"]["description"] = invalid
                self.catalog.write_text(json.dumps(base))
                before = self.catalog.read_bytes()
                result = self.cli("status", "--json", ok=False)
                self.assertIn("description", result.stderr.lower())
                self.assertNotIn("secret", result.stdout + result.stderr)
                self.assertEqual(self.catalog.read_bytes(), before)


class McpDescriptionTerminalTests(TerminalTestCase):
    def test_metadata_edit_preserves_imported_client_execution_variants(self):
        codex = self.home / ".codex/config.toml"
        codex.parent.mkdir()
        codex.write_text('[mcp_servers.docs]\ncommand = "/usr/bin/true"\n'
                         'args = ["--codex"]\nenabled = false\n')
        claude = self.home / ".claude.json"
        claude.write_text(json.dumps({"mcpServers": {"docs": {
            "command": "/usr/bin/false", "args": ["--claude"]}}}))
        self.run_cli("import", "--from", "codex,claude", "--apply")
        catalog = self.home / ".config/kitmux/catalog.json"
        before = json.loads(catalog.read_text())["mcps"]["docs"]
        native_before = {codex: codex.read_bytes(), claude: claude.read_bytes()}
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"jx")
        terminal.wait_text("stdio or http")
        terminal.send(b"\n")
        terminal.wait_text("Connection mode")
        terminal.send(b"\n")
        terminal.wait_text("Executable path or command")
        terminal.send(b"\n")
        terminal.wait_text("Arguments (quotes supported")
        terminal.send(b"\n")
        terminal.wait_text("Read a Markdown file")
        terminal.send(b"j\n")
        terminal.wait_text("Description text")
        terminal.send(b"Client-aware documentation\n")
        terminal.wait_text("Saved")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        expected = dict(before, description="Client-aware documentation")
        self.assertEqual(json.loads(catalog.read_text())["mcps"]["docs"], expected)
        self.assertEqual({target: target.read_bytes() for target in native_before}, native_before)

    def test_add_description_through_mcp_form(self):
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"njournal\n")
        terminal.wait_text("stdio or http")
        terminal.send(b"\n")
        terminal.wait_text("Connection mode")
        terminal.send(b"\n")
        terminal.wait_text("Executable path or command")
        terminal.send(b"/usr/bin/true\n")
        terminal.wait_text("Arguments (quotes supported")
        terminal.send(b"\n")
        terminal.wait_text("Read a Markdown file")
        terminal.send(b"j\n")
        terminal.wait_text("Description text")
        terminal.send(b"Search local journals\n")
        terminal.wait_text("Saved")
        terminal.send(b"x")
        terminal.wait_text("stdio or http")
        terminal.send(b"\n")
        terminal.wait_text("Connection mode")
        terminal.send(b"\n")
        terminal.wait_text("Executable path or command")
        terminal.send(b"\n")
        terminal.wait_text("Arguments (quotes supported")
        terminal.send(b"\n")
        terminal.wait_text("Read a Markdown file")
        terminal.send(b"\n")
        terminal.wait_text("already current")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        catalog = json.loads((self.home / ".config/kitmux/catalog.json").read_text())
        self.assertEqual(catalog["mcps"]["journal"]["description"], "Search local journals")

    def test_edit_can_load_long_documentation_file(self):
        description = self.home / "manual.md"
        text = "# Manual\n" + "Documentation.\n" * 4000
        description.write_text(text)
        self.run_cli("mcp", "add", "docs", "--command", "/usr/bin/true")
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"jx")
        terminal.wait_text("stdio or http")
        terminal.send(b"\n")
        terminal.wait_text("Connection mode")
        terminal.send(b"\n")
        terminal.wait_text("Executable path or command")
        terminal.send(b"\n")
        terminal.wait_text("Arguments (quotes supported")
        terminal.send(b"\n")
        terminal.wait_text("Read a Markdown file")
        terminal.send(b"jj\n")
        terminal.wait_text("Markdown file")
        terminal.send(str(description).encode() + b"\n")
        terminal.wait_text("Saved")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        catalog = json.loads((self.home / ".config/kitmux/catalog.json").read_text())
        self.assertEqual(catalog["mcps"]["docs"]["description"], text)


if __name__ == "__main__":
    unittest.main()
