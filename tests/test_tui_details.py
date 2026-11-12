import json
from pathlib import Path
import signal
import termios
import unittest

from test_tui import TerminalTestCase


class ResourceDetailsTests(TerminalTestCase):
    def catalog_path(self):
        return self.home / ".config/kitmux/catalog.json"

    def describe_mcp(self, description):
        path = self.catalog_path()
        catalog = json.loads(path.read_text())
        catalog["mcps"]["docs"]["description"] = description
        path.write_text(json.dumps(catalog))

    def snapshot(self):
        return {str(path.relative_to(self.home)): (path.read_bytes(), path.stat().st_mtime_ns)
                for path in self.home.rglob("*") if path.is_file() and not path.is_symlink()}

    def close_view(self, terminal):
        terminal.send(b"\x1b")
        terminal.wait_text("1 MCPs")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        pending = getattr(termios, "PENDIN", 0)
        self.assertEqual(termios.tcgetattr(terminal.slave)[3] & ~pending,
                         terminal.original[3] & ~pending)

    def test_mcp_fallback_view_is_read_only_and_does_not_start_managed_process(self):
        marker = self.home / "must-not-start"
        self.run_cli("mcp", "add", "docs", "--mode", "managed", "--command",
                     "/usr/bin/touch", "--", str(marker))
        before = self.snapshot()
        terminal = self.start()
        terminal.wait_text("Ungrouped (1)")
        terminal.send(b"j\n")
        terminal.wait_text("No description supplied")
        self.assertIn(b"Transport: stdio", terminal.output)
        self.assertIn(b"Runtime mode: managed", terminal.output)
        self.close_view(terminal)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse(marker.exists())

    def test_mcp_markdown_omits_execution_and_authentication_secrets(self):
        self.run_cli("mcp", "add", "docs", "--url",
                     "https://user:url-secret@example.test/private?token=query-secret",
                     "--env", "TOKEN=env-secret")
        self.describe_mcp("# Search documentation\n\n- Find APIs\n\n```sh\nexample command\n```\n")
        terminal = self.start()
        terminal.wait_text("Ungrouped (1)")
        terminal.send(b"jv")
        terminal.wait_text("Search documentation")
        if b"example command" not in terminal.output:
            terminal.wait_text("example command")
        self.assertIn(b"example command", terminal.output)
        terminal.send(b"G")
        terminal.wait_text("opencode: OFF")
        self.close_view(terminal)
        for secret in (b"url-secret", b"query-secret", b"env-secret", b"/private"):
            self.assertNotIn(secret, terminal.output)

    def test_installed_skill_document_beyond_32k_can_be_read_to_end(self):
        package = self.home / "guide"
        package.mkdir()
        source = package / "SKILL.md"
        source.write_text("---\nname: guide\ndescription: Read the guide\n---\n"
                          "# Installed guide\n\n" + "A useful instruction with context.\n" * 1600 +
                          "# Final installed instruction\n")
        self.run_cli("skill", "add", str(package))
        source.write_text("Source changed after installation.\n")
        before = self.snapshot()
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"2j\n")
        terminal.wait_text("Installed guide")
        self.assertIn(b"Revision:", terminal.output)
        terminal.send(b"G")
        terminal.wait_text("Final installed instruction")
        terminal.send(b"g")
        terminal.wait_text("Installed guide")
        terminal.send(b"\x1b[6~")
        terminal.wait_text("A useful instruction")
        self.assertNotIn(b"Source changed", terminal.output)
        self.close_view(terminal)
        self.assertEqual(self.snapshot(), before)

    def test_missing_installed_skill_reports_read_error_without_opening_source(self):
        package = self.home / "guide"
        package.mkdir()
        (package / "SKILL.md").write_text("---\nname: guide\ndescription: Guide\n---\nOriginal source instructions\n")
        self.run_cli("skill", "add", str(package))
        catalog = json.loads(self.catalog_path().read_text())
        installed = Path(catalog["skills"]["guide"]["path"]) / "SKILL.md"
        installed.unlink()
        before = self.snapshot()
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"2j\n")
        terminal.wait_text("Cannot read")
        self.assertNotIn(b"Original source instructions", terminal.output)
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        self.assertEqual(self.snapshot(), before)

    def test_subagent_variants_show_effective_instructions_without_native_secrets(self):
        source = self.home / "reviewer.toml"
        source.write_text('name = "reviewer"\ndescription = "Review changes"\n'
                          'developer_instructions = "Codex-only instructions"\n')
        self.run_cli("subagent", "add", "reviewer", "--from", str(source), "--format", "codex")
        source = self.home / "reviewer.md"
        source.write_text("---\nname: reviewer\ndescription: Claude purpose\n"
                          "env:\n  TOKEN: native-secret\n---\nClaude-only instructions\n")
        self.run_cli("subagent", "edit", "reviewer", "--from", str(source), "--format", "claude")
        before = self.snapshot()
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"3j\n")
        terminal.wait_text("Codex-only instructions")
        terminal.send(b"G")
        terminal.wait_text("Claude-only instructions")
        self.assertIn(b"claude variant", terminal.output)
        self.assertNotIn(b"native-secret", terminal.output)
        self.close_view(terminal)
        self.assertEqual(self.snapshot(), before)

    def test_unicode_and_control_characters_are_safe_across_resize(self):
        source = self.home / "review.md"
        source.write_text("# Revisão 安全\n\nSafe\x1b]52;c;clipboard\x07 instructions.\n" +
                          "Wide characters 安全 and accents ação.\n" * 30 + "Resize end marker\n")
        self.run_cli("subagent", "add", "reviewer", "--from", str(source),
                     "--description", "Review safely")
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"3jv")
        terminal.wait_text("Revisão 安全")
        terminal.resize(20, 74)
        terminal.process.send_signal(signal.SIGWINCH)
        terminal.send(b"G")
        terminal.wait_text("Resize end marker")
        terminal.send(b"g")
        terminal.send(b"jjjjjj")
        terminal.wait_text("Revisão 安全")
        self.close_view(terminal)
        self.assertNotIn(b"\x1b]52", terminal.output)

    def test_escape_restores_resource_selection_and_group_expansion(self):
        self.run_cli("mcp", "add", "docs", "--command", "/usr/bin/true")
        self.run_cli("mcp", "group", "add", "tools")
        self.run_cli("mcp", "group", "include", "tools", "docs")
        terminal = self.start()
        terminal.wait_text("tools (1)")
        terminal.send(b"\n")
        terminal.wait_text("tools collapsed.")
        terminal.send(b"\n")
        terminal.wait_text("expanded.")
        terminal.send(b"jv")
        terminal.wait_text("No description supplied")
        terminal.send(b"\x1b")
        terminal.wait_text("1 MCPs")
        terminal.send(b"e")
        terminal.wait_text("Saved")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        catalog = json.loads(self.catalog_path().read_text())
        self.assertFalse(catalog["mcps"]["docs"]["enabled"])
        self.assertTrue(catalog["groups"]["mcp:tools"]["enabled"])


if __name__ == "__main__":
    unittest.main()
