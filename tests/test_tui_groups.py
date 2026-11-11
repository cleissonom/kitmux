import json
import unittest

from test_tui import TerminalTestCase


class GroupedTerminalTests(TerminalTestCase):
    def group(self, kind, name, *members):
        self.run_cli(kind, "group", "add", name)
        if members:
            self.run_cli(kind, "group", "include", name, *members)

    def catalog(self):
        return json.loads((self.home / ".config/kitmux/catalog.json").read_text())

    def test_resource_tabs_replace_global_groups_tab(self):
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        self.assertNotIn(b"1 Groups", terminal.output)
        terminal.send(b"3")
        terminal.wait_text("Subagents")
        terminal.send(b"4")
        terminal.wait_text("Separate configuration")
        terminal.send(b"5")
        terminal.wait_text("Runtime snapshot")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)

    def test_collapsed_group_search_finds_child_and_escape_restores_collapse(self):
        self.run_cli("mcp", "add", "teams", "--command", "/usr/bin/true")
        self.group("mcp", "communication", "teams")
        terminal = self.start()
        terminal.wait_text("communication (1)")
        terminal.send(b"\n")
        terminal.wait_text("communication collapsed.")
        terminal.send(b"/teams\n")
        terminal.wait_text("Filter: teams")
        terminal.send(b"\x1b")
        terminal.wait_text("Filter cleared.")
        terminal.send(b"je")
        terminal.wait_text("Saved")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        self.assertTrue(self.catalog()["mcps"]["teams"]["enabled"])
        self.assertFalse(self.catalog()["groups"]["mcp:communication"]["enabled"])

    def test_selected_resource_in_two_groups_toggles_once(self):
        self.run_cli("mcp", "add", "teams", "--command", "/usr/bin/true")
        self.group("mcp", "communication", "teams")
        self.group("mcp", "work", "teams")
        terminal = self.start()
        terminal.wait_text("communication (1)")
        terminal.send(b"j e")
        terminal.wait_text("Saved")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        self.assertFalse(self.catalog()["mcps"]["teams"]["enabled"])

    def test_group_remove_keeps_member_and_moves_it_to_ungrouped(self):
        self.run_cli("mcp", "add", "teams", "--command", "/usr/bin/true")
        self.group("mcp", "communication", "teams")
        terminal = self.start()
        terminal.wait_text("communication (1)")
        terminal.send(b"d")
        terminal.wait_text("Members remain installed")
        terminal.send(b"y")
        terminal.wait_text("Ungrouped (1)")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        self.assertIn("teams", self.catalog()["mcps"])
        self.assertNotIn("mcp:communication", self.catalog()["groups"])

    def test_same_group_name_in_other_kind_is_unaffected(self):
        self.group("mcp", "review")
        self.group("subagent", "review")
        terminal = self.start()
        terminal.wait_text("review (0)")
        terminal.send(b"e")
        terminal.wait_text("Saved")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        self.assertFalse(self.catalog()["groups"]["mcp:review"]["enabled"])
        self.assertTrue(self.catalog()["groups"]["subagent:review"]["enabled"])

    def test_item_membership_selector_can_choose_multiple_current_kind_groups(self):
        self.run_cli("mcp", "add", "teams", "--command", "/usr/bin/true")
        self.group("mcp", "communication")
        self.group("mcp", "work")
        self.group("subagent", "unrelated")
        terminal = self.start()
        terminal.wait_text("Ungrouped (1)")
        terminal.send(b"jjjm")
        terminal.wait_text("Item groups")
        self.assertNotIn(b"unrelated", terminal.output)
        terminal.send(b" j \n")
        terminal.wait_text("communication (1)")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        for group in ("mcp:communication", "mcp:work"):
            self.assertEqual(self.catalog()["groups"][group]["members"], ["mcp:teams"])

    def test_ungrouped_header_does_not_remove_members(self):
        self.run_cli("mcp", "add", "teams", "--command", "/usr/bin/true")
        terminal = self.start()
        terminal.wait_text("Ungrouped (1)")
        terminal.send(b"dea")
        terminal.read_for(.2)
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        self.assertIn("teams", self.catalog()["mcps"])

    def test_add_mcp_in_collapsed_group_selects_the_new_resource(self):
        self.group("mcp", "communication")
        terminal = self.start()
        terminal.wait_text("communication (0)")
        terminal.send(b"\n")
        terminal.wait_text("communication collapsed.")
        terminal.send(b"n")
        terminal.wait_text("MCP name")
        terminal.send(b"sample\n")
        terminal.wait_text("stdio or http")
        terminal.send(b"\n")
        terminal.wait_text("Connection mode")
        terminal.send(b"\n")
        self.finish_mcp_form(terminal)
        terminal.send(b"r")
        terminal.wait_text("Reconnect agent")
        terminal.send(b"\x1bq")
        self.assertEqual(terminal.wait_exit(), 0)
        self.assertEqual(self.catalog()["groups"]["mcp:communication"]["members"], ["mcp:sample"])

    def legacy_catalog(self):
        self.run_cli("mcp", "add", "teams", "--command", "/usr/bin/true")
        path = self.home / ".config/kitmux/catalog.json"
        catalog = self.catalog()
        catalog.update(version=2, groups={"communication": {
            "enabled": True, "agents": ["codex"], "members": ["mcp:teams"]}})
        path.write_text(json.dumps(catalog))
        return path

    def test_migration_dialog_cancel_leaves_legacy_catalog_unchanged(self):
        path = self.legacy_catalog()
        before = path.read_bytes()
        terminal = self.start()
        terminal.wait_text("Migrate groups into resource tabs")
        terminal.send(b"\x1b")
        self.assertEqual(terminal.wait_exit(), 0)
        self.assertEqual(path.read_bytes(), before)

    def test_migration_dialog_apply_opens_the_typed_group(self):
        self.legacy_catalog()
        terminal = self.start()
        terminal.wait_text("Migrate groups into resource tabs")
        terminal.send(b"\n")
        terminal.wait_text("communication (1)")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        self.assertEqual(self.catalog()["groups"]["mcp:communication"]["agents"], ["codex"])

    def test_failed_migration_reports_failure_and_preserves_original(self):
        path = self.legacy_catalog()
        before = path.read_bytes()
        revision = self.catalog()["revision"]
        backups = self.home / ".local/state/kitmux/migrations"
        backups.mkdir(parents=True)
        (backups / f"groups-v3-r{revision}-catalog.json").write_text("different backup")
        terminal = self.start()
        terminal.wait_text("Migrate groups into resource tabs")
        terminal.send(b"\n")
        terminal.wait_text("Group migration failed")
        terminal.send(b"\n")
        self.assertEqual(terminal.wait_exit(), 1)
        self.assertEqual(path.read_bytes(), before)

    def test_group_membership_selector_does_not_show_other_resource_kind(self):
        self.run_cli("mcp", "add", "teams", "--command", "/usr/bin/true")
        source = self.home / "review.md"
        source.write_text("Review code.\n")
        self.run_cli("subagent", "add", "reviewer", "--from", str(source),
                     "--description", "Review code")
        self.group("mcp", "communication")
        terminal = self.start()
        terminal.wait_text("communication (0)")
        terminal.send(b"m")
        terminal.wait_text("Group members")
        screen = terminal.read_for(.1)
        self.assertNotIn(b"reviewer", screen)
        terminal.send(b" \n")
        terminal.wait_text("communication (1)")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        self.assertEqual(self.catalog()["groups"]["mcp:communication"]["members"], ["mcp:teams"])


if __name__ == "__main__":
    unittest.main()
