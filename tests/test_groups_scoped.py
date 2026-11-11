"""Scoped grouping and lossless migration contracts."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class ScopedGroupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.path = self.home / ".config/kitmux/catalog.json"

    def cli(self, *args, ok=True):
        result = subprocess.run([BINARY, "--home", str(self.home), *args],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def status(self):
        return json.loads(self.cli("status", "--json").stdout)

    def legacy(self, empty=False):
        self.path.parent.mkdir(parents=True)
        document = {"version": 1, "revision": 9, "mcps": {
            "docs": {"transport": "http", "url": "https://example.invalid/mcp",
                     "native": {"codex": {"http_headers": {"Authorization": "private-fixture"}}},
                     "agents": {"claude": "off"}}}, "skills": {}, "groups": {
            "shared": {"enabled": True, "agents": ["codex", "claude"],
                       "members": [] if empty else ["mcp:docs"]}}}
        self.path.write_text(json.dumps(document, indent=1) + "\n")
        return self.path.read_bytes()

    def test_default_scoped_group_list_creates_no_state(self):
        self.cli("mcp", "group")
        self.assertEqual(list(self.home.iterdir()), [])

    def test_same_named_groups_have_independent_kind_and_members(self):
        self.cli("mcp", "add", "docs", "--url", "https://example.invalid/mcp")
        for kind in ("mcp", "skill", "subagent"):
            self.cli(kind, "group", "add", "work")
        self.cli("mcp", "group", "include", "work", "docs")
        state = self.status()["groups"]
        self.assertEqual(set(state), {"mcp:work", "skill:work", "subagent:work"})
        self.assertEqual(state["mcp:work"]["members"], ["mcp:docs"])
        self.assertEqual(state["skill:work"]["members"], [])
        self.assertEqual(state["subagent:work"]["kind"], "subagent")
        self.assertEqual(json.loads(self.path.read_text())["version"], 3)
        groups = json.loads(self.cli("skill", "group", "list", "--json").stdout)
        self.assertEqual(set(groups), {"work"})

    def test_cross_kind_and_global_mutations_fail_without_writes(self):
        self.cli("mcp", "add", "docs", "--url", "https://example.invalid/mcp")
        self.cli("skill", "group", "add", "work")
        before = self.path.read_bytes()
        self.cli("skill", "group", "include", "work", "mcp:docs", ok=False)
        self.cli("group", "disable", "work", ok=False)
        self.assertEqual(self.path.read_bytes(), before)

    def test_multiple_memberships_union_and_removal_preserve_resource(self):
        self.cli("mcp", "add", "docs", "--url", "https://example.invalid/mcp")
        for name in ("development", "review"):
            self.cli("mcp", "group", "add", name)
            self.cli("mcp", "group", "include", name, "mcp:docs")
            self.cli("mcp", "group", "assign", name, "--agents", "codex")
        self.cli("mcp", "group", "disable", "development")
        self.assertTrue(self.status()["mcps"]["docs"]["agents"]["codex"]["enabled"])
        self.cli("mcp", "disable", "docs", "--agent", "codex")
        self.assertFalse(self.status()["mcps"]["docs"]["agents"]["codex"]["enabled"])
        self.cli("mcp", "group", "remove", "review")
        self.assertIn("docs", self.status()["mcps"])

    def test_migration_preview_is_read_only_and_apply_preserves_native_values(self):
        before = self.legacy()
        original = self.status()["mcps"]
        preview = self.cli("group", "migrate", "--preview")
        self.assertNotIn("private-fixture", preview.stdout + preview.stderr)
        self.assertEqual(self.path.read_bytes(), before)
        self.assertFalse((self.home / ".local").exists())
        self.cli("mcp", "group", "add", "new", ok=False)
        self.assertEqual(self.path.read_bytes(), before)
        applied = self.cli("group", "migrate", "--apply")
        self.assertIn(str(self.home / ".local/state/kitmux/migrations"), applied.stdout)
        current = self.status()["mcps"]
        for client in ("codex", "claude", "opencode"):
            self.assertEqual(current["docs"]["agents"][client]["enabled"],
                             original["docs"]["agents"][client]["enabled"])
        self.assertEqual(json.loads(before)["mcps"], json.loads(self.path.read_text())["mcps"])
        migrated = json.loads(self.path.read_text())
        self.assertEqual(migrated["version"], 3)
        self.assertEqual(set(migrated["groups"]), {"mcp:shared"})
        self.assertIn("private-fixture", self.path.read_text())
        backups = list((self.home / ".local/state/kitmux/migrations").glob("*.json"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), before)
        self.assertEqual(backups[0].stat().st_mode & 0o777, 0o600)
        after = self.path.read_bytes()
        self.cli("group", "migrate", "--apply")
        self.assertEqual(self.path.read_bytes(), after)

    def test_empty_legacy_group_preserved_in_mcp_scope(self):
        self.legacy(empty=True)
        preview = self.cli("group", "migrate", "--preview")
        self.assertIn("mcp", preview.stdout)
        self.cli("group", "migrate", "--apply")
        self.assertEqual(self.status()["groups"]["mcp:shared"]["members"], [])

    def test_scoped_group_validation_rejects_key_and_member_mismatch(self):
        self.cli("mcp", "add", "docs", "--url", "https://example.invalid/mcp")
        self.cli("skill", "group", "add", "work")
        data = json.loads(self.path.read_text())
        data["groups"]["skill:work"]["members"] = ["mcp:docs"]
        self.path.write_text(json.dumps(data))
        before = self.path.read_bytes()
        self.cli("status", "--json", ok=False)
        self.assertEqual(self.path.read_bytes(), before)

    def test_mixed_migration_preserves_all_kinds_and_adopted_native_files(self):
        self.legacy()
        data = json.loads(self.path.read_text())
        data["mcps"]["docs"].pop("native")
        data["skills"]["guide"] = {"enabled": False, "agents": {"opencode": "on"}}
        data["subagents"] = {"reviewer": {"description": "Review", "instructions": "Review safely.",
                                           "portable": True, "agents": {}}}
        data["version"] = 2
        data["groups"]["shared"]["members"] += ["skill:guide", "subagent:reviewer"]
        self.path.write_text(json.dumps(data))
        self.cli("sync", "--apply", "--agent", "codex")
        native = self.home / ".codex/config.toml"
        profile = self.home / ".codex/agents/reviewer.toml"
        before = [(p.read_bytes(), p.stat().st_mtime_ns) for p in (native, profile)]
        original = self.status()
        self.cli("group", "migrate", "--apply")
        after = self.status()
        self.assertEqual(set(after["groups"]), {"mcp:shared", "skill:shared", "subagent:shared"})
        for kind in ("mcps", "skills", "subagents"):
            for name in original[kind]:
                for client in ("codex", "claude", "opencode"):
                    self.assertEqual(original[kind][name]["agents"][client]["enabled"],
                                     after[kind][name]["agents"][client]["enabled"])
        self.assertEqual(before, [(p.read_bytes(), p.stat().st_mtime_ns) for p in (native, profile)])
        self.cli("sync", "--check", "--agent", "codex")

    def test_migration_backup_conflict_preserves_catalog_and_existing_backup(self):
        before = self.legacy()
        backup = self.home / ".local/state/kitmux/migrations/groups-v3-r9-catalog.json"
        backup.parent.mkdir(parents=True)
        backup.write_text("preserved previous backup")
        self.cli("group", "migrate", "--apply", ok=False)
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(backup.read_text(), "preserved previous backup")

    def test_migration_backup_symlink_is_not_followed(self):
        before = self.legacy()
        external = self.home / "private-backup"
        external.write_text("preserved private bytes")
        backup = self.home / ".local/state/kitmux/migrations/groups-v3-r9-catalog.json"
        backup.parent.mkdir(parents=True)
        backup.symlink_to(external)
        self.cli("group", "migrate", "--apply", ok=False)
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(external.read_text(), "preserved private bytes")


if __name__ == "__main__":
    unittest.main()
