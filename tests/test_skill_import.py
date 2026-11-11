import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))
ROOTS = {"codex": ".codex/skills", "claude": ".claude/skills", "opencode": ".config/opencode/skills"}


class SkillImportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="km-import-", dir="/tmp")
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name).resolve()
        self.names = ("meeting-notes", "code-review", "writing")
        for name in self.names:
            source = self.home / ".agents/codex/skills" / name
            (source / "references").mkdir(parents=True)
            (source / "SKILL.md").write_text(f"---\nname: {name}\ndescription: Imported fixture instructions\n---\nComplete skill package\n")
            (source / "references/guide.txt").write_text("Keep supporting resources\n")
            (source / "LICENSE").write_text("Fixture license\n")
            for relative in ROOTS.values():
                root = self.home / relative
                root.mkdir(parents=True, exist_ok=True)
                (root / name).symlink_to(source)

    def cli(self, *args, ok=True):
        result = subprocess.run([BINARY, "--home", str(self.home), *args],
                                text=True, capture_output=True, timeout=20)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def write(self, relative, text):
        path = self.home / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def catalog(self):
        return json.loads((self.home / ".config/kitmux/catalog.json").read_text())

    def aliases(self):
        return {str(self.home / root / name): os.readlink(self.home / root / name)
                for root in ROOTS.values() for name in self.names}

    def test_preview_discovers_complete_skills_without_writes(self):
        before = self.aliases()
        result = self.cli("import", "--from", "codex,claude,opencode", "--preview")
        self.assertIn("skill meeting-notes", result.stdout)
        self.assertEqual(self.aliases(), before)
        self.assertFalse((self.home / ".config/kitmux").exists())
        self.assertFalse((self.home / ".local/share/kitmux").exists())
        self.assertFalse((self.home / ".local/state/kitmux").exists())

    def test_import_deduplicates_packages_and_adopts_aliases_without_replacing_them(self):
        before = self.aliases()
        self.cli("import", "--from", "codex,claude,opencode", "--apply")
        skills = self.catalog()["skills"]
        self.assertEqual(set(skills), set(self.names))
        self.assertEqual(self.aliases(), before)
        for name, skill in skills.items():
            installed = Path(skill["path"])
            self.assertTrue(installed.is_relative_to(self.home / ".local/share/kitmux/skills"))
            self.assertEqual((installed / "references/guide.txt").read_text(), "Keep supporting resources\n")
            self.assertEqual((installed / "LICENSE").read_text(), "Fixture license\n")
            self.assertEqual(skill["agents"], {agent: "on" for agent in ROOTS})
        state = json.loads((self.home / ".local/state/kitmux/adapters.json").read_text())
        for path, target in before.items():
            self.assertEqual(state["links"][path]["before"], target)
            self.assertEqual(state["links"][path]["after"], target)
        self.assertFalse((self.home / ".agents/skills").exists())

    def test_client_filter_does_not_assign_other_clients(self):
        self.cli("import", "--from", "codex", "--apply")
        self.assertEqual(set(self.catalog()["skills"]), set(self.names))
        for skill in self.catalog()["skills"].values():
            self.assertEqual(skill["agents"], {"codex": "on"})

    def test_import_is_idempotent(self):
        self.cli("import", "--from", "codex,claude,opencode", "--apply")
        skills = self.catalog()["skills"]
        state = json.loads((self.home / ".local/state/kitmux/adapters.json").read_text())
        self.cli("import", "--from", "codex,claude,opencode", "--apply")
        self.assertEqual(self.catalog()["skills"], skills)
        self.assertEqual(json.loads((self.home / ".local/state/kitmux/adapters.json").read_text())["links"], state["links"])

    def test_native_disabled_skills_remain_disabled_in_catalog(self):
        source = self.home / ".codex/skills/meeting-notes/SKILL.md"
        self.write(".codex/config.toml", f"[[skills.config]]\npath = {json.dumps(str(source))}\nenabled = false\n")
        self.write(".claude/settings.json", '{"skillOverrides":{"meeting-notes":"off"}}')
        self.write(".config/opencode/opencode.json", '{"permissions":[{"action":"skill","resource":"meeting-*","effect":"deny"}]}')
        before = self.aliases()
        self.cli("import", "--from", "codex,claude,opencode", "--apply")
        self.assertEqual(self.catalog()["skills"]["meeting-notes"]["agents"], {agent:"off" for agent in ROOTS})
        self.assertEqual(self.aliases(), before)

    def test_disabled_and_reenabled_skills_use_existing_client_aliases(self):
        self.cli("import", "--from", "codex,claude,opencode", "--apply")
        for agent, relative in ROOTS.items():
            with self.subTest(agent=agent):
                self.cli("skill", "disable", "meeting-notes", "--agent", agent)
                alias = self.home / relative / "meeting-notes"
                self.assertFalse(alias.exists())
                self.assertTrue((self.home / ".agents/codex/skills/meeting-notes/SKILL.md").exists())
                self.cli("skill", "enable", "meeting-notes", "--agent", agent)
                self.assertTrue(alias.is_symlink())
                self.assertEqual(alias.resolve(), Path(self.catalog()["skills"]["meeting-notes"]["path"]))
        self.assertFalse((self.home / ".agents/skills").exists())

    def test_restore_returns_original_native_alias_targets(self):
        before = self.aliases()
        self.cli("import", "--from", "codex,claude,opencode", "--apply")
        self.cli("skill", "disable", "meeting-notes")
        self.cli("restore", "--apply")
        self.assertEqual(self.aliases(), before)

    def test_conflicting_client_variants_fail_without_mutating_sources(self):
        variant = self.home / "variant/meeting-notes"
        variant.mkdir(parents=True)
        (variant / "SKILL.md").write_text("---\nname: meeting-notes\ndescription: Other variant\n---\nDifferent instructions\n")
        alias = self.home / ".claude/skills/meeting-notes"
        alias.unlink()
        alias.symlink_to(variant)
        before = self.aliases()
        result = self.cli("import", "--from", "codex,claude,opencode", "--apply", ok=False)
        self.assertIn("conflict", result.stderr.lower())
        self.assertEqual(self.aliases(), before)
        self.assertFalse((self.home / ".config/kitmux/catalog.json").exists())

    def test_real_native_directories_require_explicit_migration(self):
        alias = self.home / ".claude/skills/meeting-notes"
        alias.unlink()
        alias.mkdir()
        (alias / "SKILL.md").write_text("---\nname: meeting-notes\ndescription: Existing real directory\n---\nKeep me\n")
        result = self.cli("import", "--from", "claude", "--apply", ok=False)
        self.assertIn("migration", result.stderr.lower())
        self.assertTrue(alias.is_dir())
        self.assertFalse(alias.is_symlink())

    def test_builtin_and_synced_containers_stay_external(self):
        self.write(".codex/skills/.system/builtin/SKILL.md", "---\nname: builtin\ndescription: Native\n---\nNative\n")
        self.write(".claude/skills/synced/provider/remote/SKILL.md", "---\nname: remote\ndescription: Synced\n---\nSynced\n")
        self.cli("import", "--from", "codex,claude,opencode", "--apply")
        self.assertEqual(set(self.catalog()["skills"]), set(self.names))
        self.assertTrue((self.home / ".codex/skills/.system/builtin/SKILL.md").exists())
        self.assertTrue((self.home / ".claude/skills/synced/provider/remote/SKILL.md").exists())

    def test_discovery_path_outside_native_roots_cannot_modify_files(self):
        self.cli("import", "--from", "codex,claude,opencode", "--apply")
        path = self.home / ".config/kitmux/catalog.json"
        outside = self.write("outside-native-roots", "Preserve this file\n")
        document = self.catalog()
        document["skills"]["meeting-notes"]["discovery"]["claude"] = [{"path":str(outside), "target":"/unused"}]
        path.write_text(json.dumps(document))
        before = self.aliases()
        self.cli("sync", "--agent", "claude", "--apply", ok=False)
        self.assertEqual(outside.read_text(), "Preserve this file\n")
        self.assertEqual(self.aliases(), before)

    def test_unknown_discovery_agents_and_unsafe_origins_are_rejected(self):
        self.cli("import", "--from", "codex,claude,opencode", "--apply")
        path = self.home / ".config/kitmux/catalog.json"
        original = self.catalog()
        for field, agent in (("discovery", "unknown"), ("origins", "claude")):
            document = json.loads(json.dumps(original))
            document["skills"]["meeting-notes"].setdefault(field,{})[agent] = [{"path":str(self.home/"outside"), "target":"/unused"}]
            path.write_text(json.dumps(document))
            self.cli("sync", "--agent", "claude", "--apply", ok=False)

    def test_relative_alias_targets_are_restored_exactly(self):
        for root in ROOTS.values():
            for name in self.names:
                alias = self.home / root / name
                source = alias.resolve()
                alias.unlink()
                alias.symlink_to(os.path.relpath(source, alias.parent))
        before = self.aliases()
        self.cli("import", "--from", "codex,claude,opencode", "--apply")
        self.cli("skill", "disable", "meeting-notes")
        self.cli("restore", "--apply")
        self.assertEqual(self.aliases(), before)

    def test_native_codex_off_cannot_be_silently_overridden(self):
        source = self.home / ".codex/skills/meeting-notes/SKILL.md"
        config = self.write(".codex/config.toml", f"[[skills.config]]\npath = {json.dumps(str(source))}\nenabled = false\n")
        self.cli("import", "--from", "codex", "--apply")
        before = config.read_bytes()
        result = self.cli("skill", "enable", "meeting-notes", "--agent", "codex", ok=False)
        self.assertIn("native Codex skills.config", result.stderr)
        self.assertEqual(config.read_bytes(), before)

    def test_claude_invocation_restrictions_survive_enable_disable(self):
        settings = self.write(".claude/settings.json", '{"skillOverrides":{"meeting-notes":"user-invocable-only"}}')
        self.cli("import", "--from", "claude", "--apply")
        self.cli("skill", "disable", "meeting-notes", "--agent", "claude")
        self.assertEqual(json.loads(settings.read_text())["skillOverrides"]["meeting-notes"], "off")
        self.cli("skill", "enable", "meeting-notes", "--agent", "claude")
        self.assertEqual(json.loads(settings.read_text())["skillOverrides"]["meeting-notes"], "user-invocable-only")

    def test_reimport_rejects_malformed_discovery_without_aborting(self):
        self.cli("import", "--from", "codex,claude,opencode", "--apply")
        catalog = self.home / ".config/kitmux/catalog.json"
        document = self.catalog()
        document["skills"]["meeting-notes"]["discovery"] = []
        catalog.write_text(json.dumps(document))
        before = self.aliases()
        result = self.cli("import", "--from", "claude", "--apply", ok=False)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.aliases(), before)

    def test_codex_native_name_selectors_are_supported_and_preserved(self):
        config = self.write(".codex/config.toml", '[[skills.config]]\nname = "plugin:external"\nenabled = false\n\n[[skills.config]]\nname = "meeting-notes"\nenabled = false\n\n[[skills.config]]\nname = "code-review"\nenabled = true\n')
        before = config.read_bytes()
        self.cli("import", "--from", "codex", "--apply")
        skills = self.catalog()["skills"]
        self.assertEqual(skills["meeting-notes"]["agents"]["codex"], "off")
        self.assertEqual(skills["code-review"]["agents"]["codex"], "on")
        self.assertEqual(skills["writing"]["agents"]["codex"], "on")
        self.assertEqual(config.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
