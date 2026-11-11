import json
import os
from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)

    def cli(self, *args, ok=True):
        result = subprocess.run([BINARY, "--home", str(self.home), *args],
                                capture_output=True, text=True, timeout=15)
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

    def registered(self, agents="codex,claude,opencode"):
        self.cli("mcp", "add", "example", "--command", "/usr/bin/true")
        self.cli("mcp", "group", "add", "work")
        self.cli("mcp", "group", "include", "work", "mcp:example")
        self.cli("mcp", "group", "assign", "work", "--agents", agents)

    def test_preview_does_not_write_native_configs(self):
        self.registered()
        result = self.cli("sync", "--preview")
        self.assertIn("preview", result.stdout.lower())
        self.assertFalse((self.home / ".codex/config.toml").exists())
        self.assertFalse((self.home / ".claude.json").exists())
        self.assertFalse((self.home / ".config/opencode/opencode.json").exists())

    def test_sync_three_clients_and_idempotence(self):
        self.registered()
        self.cli("sync", "--apply")
        codex = self.home / ".codex/config.toml"
        claude = self.home / ".claude.json"
        opencode = self.home / ".config/opencode/opencode.json"
        self.assertEqual(tomllib.loads(codex.read_text())["mcp_servers"]["example"]["command"], "/usr/bin/true")
        self.assertEqual(json.loads(claude.read_text())["mcpServers"]["example"]["command"], "/usr/bin/true")
        self.assertEqual(json.loads(opencode.read_text())["mcp"]["servers"]["example"]["command"], ["/usr/bin/true"])
        before = [p.read_bytes() for p in (codex, claude, opencode)]
        self.cli("sync", "--apply")
        self.assertEqual([p.read_bytes() for p in (codex, claude, opencode)], before)
        self.cli("mcp", "disable", "example", "--agent", "claude")
        self.cli("sync", "--apply")
        self.assertNotIn("example", json.loads(claude.read_text())["mcpServers"])

    def test_jsonc_comments_and_unowned_values_survive(self):
        original = '{\n // keep this comment\n "theme": "custom",\n "mcp": {"servers": {"unowned":{"type":"remote","url":"https://example.test"}}},\n}\n'
        path = self.write(".config/opencode/opencode.jsonc", original)
        self.registered("opencode")
        self.cli("sync", "--apply", "--agent", "opencode")
        text = path.read_text()
        self.assertIn("// keep this comment", text)
        self.assertIn('"theme": "custom"', text)
        self.assertIn('"unowned":{"type":"remote","url":"https://example.test"}', text)

    def test_jsonc_trailing_comment_after_comma_remains_valid(self):
        path = self.write(".config/opencode/opencode.jsonc", '{"theme":"custom", // keep trailing comment\n}\n')
        self.registered("opencode")
        self.cli("sync", "--apply", "--agent", "opencode")
        self.assertIn("// keep trailing comment", path.read_text())
        self.cli("doctor", "--agent", "opencode")

    def test_unowned_collision_requires_import(self):
        original = '{"mcpServers":{"example":{"command":"/usr/bin/false","env":{"TOKEN":"private"}}},"theme":"dark"}'
        path = self.write(".claude.json", original)
        self.registered("claude")
        result = self.cli("sync", "--apply", "--agent", "claude", ok=False)
        self.assertNotIn("private", result.stderr + result.stdout)
        self.assertEqual(path.read_text(), original)

    def test_import_adopts_and_restore_preserves_unowned_settings(self):
        path = self.write(".claude.json", '{"mcpServers":{"existing":{"command":"/usr/bin/true","args":["--flag"]}},"theme":"dark"}')
        self.cli("import", "--from", "claude", "--apply")
        self.cli("mcp", "disable", "existing", "--agent", "claude")
        self.cli("sync", "--apply", "--agent", "claude")
        doc = json.loads(path.read_text())
        self.assertNotIn("existing", doc["mcpServers"])
        doc["new-setting"] = "preserve"
        path.write_text(json.dumps(doc))
        self.cli("restore", "--agent", "claude", "--apply")
        restored = json.loads(path.read_text())
        self.assertEqual(restored["mcpServers"]["existing"]["args"], ["--flag"])
        self.assertEqual(restored["new-setting"], "preserve")

    def test_edited_owned_server_is_a_conflict(self):
        self.registered("claude")
        self.cli("sync", "--apply", "--agent", "claude")
        path = self.home / ".claude.json"
        doc = json.loads(path.read_text())
        doc["mcpServers"]["example"]["command"] = "/usr/bin/false"
        path.write_text(json.dumps(doc))
        before = path.read_bytes()
        self.cli("sync", "--apply", "--agent", "claude", ok=False)
        self.assertEqual(path.read_bytes(), before)

    def test_skill_only_for_claude_is_denied_in_opencode(self):
        self.cli("mcp", "group", "add", "skills")
        package = self.home / "package"
        package.mkdir()
        (package / "SKILL.md").write_text("---\nname: review\ndescription: Review code\n---\nReview.")
        catalog_path = self.home / ".config/kitmux/catalog.json"
        catalog = json.loads(catalog_path.read_text())
        catalog["skills"]["review"] = {"name":"review", "path":str(package), "enabled":True,
                                          "agents":{"claude":"on"}}
        catalog_path.write_text(json.dumps(catalog))
        self.cli("sync", "--apply")
        self.assertTrue((self.home / ".claude/skills/review").is_symlink())
        self.assertFalse((self.home / ".agents/skills/review").exists())
        doc = json.loads((self.home / ".config/opencode/opencode.json").read_text())
        self.assertIn({"action":"skill", "resource":"review", "effect":"deny"}, doc["permissions"])

    def test_jsonc_invalid_input_is_not_rewritten(self):
        path = self.write(".config/opencode/opencode.jsonc", '{"mcp": nope}')
        self.registered("opencode")
        self.cli("sync", "--apply", "--agent", "opencode", ok=False)
        self.assertEqual(path.read_text(), '{"mcp": nope}')

    def test_corrupt_ownership_is_rejected_without_signals(self):
        self.registered("claude")
        path = self.home / ".local/state/kitmux/adapters.json"
        cases = [{"files": []}, {"files": {str(self.home / ".claude.json"): {"agent": None, "entries": {}}}},
                 {"files": {str(self.home / "../escape"): {"agent":"claude", "toml":False, "entries":{}}}},
                 {"files": {str(self.home / ".claude.json"): {"agent":"claude", "toml":False, "entries":{"mcpServers/example": 7}}}}]
        for doc in cases:
            self.write(".local/state/kitmux/adapters.json", json.dumps(doc))
            before = path.read_bytes()
            result = self.cli("sync", "--apply", ok=False)
            self.assertGreater(result.returncode, 0)
            self.assertEqual(path.read_bytes(), before)

    def test_managed_disabled_bridges_remain_registered(self):
        self.cli("mcp", "add", "managed", "--command", "/usr/bin/true", "--mode", "managed")
        self.cli("sync", "--apply")
        codex = tomllib.loads((self.home / ".codex/config.toml").read_text())["mcp_servers"]["managed"]
        claude = json.loads((self.home / ".claude.json").read_text())["mcpServers"]["managed"]
        opencode = json.loads((self.home / ".config/opencode/opencode.json").read_text())["mcp"]["servers"]["managed"]
        self.assertTrue(codex["enabled"])
        self.assertIn("bridge", codex["args"])
        self.assertIn("--home", claude["args"])
        self.assertFalse(opencode["disabled"])

    def test_codex_import_preserves_comments_and_multiline_instructions(self):
        original = '# keep\ndeveloper_instructions = """\n[mcp_servers.fake]\nkeep these instructions\n"""\n\n[mcp_servers.real]\ncommand = "/usr/bin/true" # preserve command comment\n[mcp_servers.real.env]\nTOKEN = "private" # preserve env comment\n'
        path = self.write(".codex/config.toml", original)
        self.cli("import", "--from", "codex", "--apply")
        self.cli("mcp", "disable", "real", "--agent", "codex")
        self.cli("sync", "--apply", "--agent", "codex")
        text = path.read_text()
        self.assertIn('# preserve command comment', text)
        self.assertIn('# preserve env comment', text)
        doc = tomllib.loads(text)
        self.assertFalse(doc["mcp_servers"]["real"]["enabled"])
        self.assertNotIn("fake", doc["mcp_servers"])
        self.assertEqual(doc["mcp_servers"]["real"]["env"]["TOKEN"], "private")

    def test_opencode_disabled_skill_deny_is_last_matching_rule(self):
        self.write(".config/opencode/opencode.json", json.dumps({"permissions":[
            {"action":"skill", "resource":"review", "effect":"deny"},
            {"action":"skill", "resource":"*", "effect":"allow"}]}))
        self.cli("mcp", "group", "add", "seed")
        path = self.home / ".config/kitmux/catalog.json"
        catalog = json.loads(path.read_text())
        catalog["skills"]["review"] = {"name":"review", "path":str(self.home / "unused"), "enabled":True, "agents":{}}
        path.write_text(json.dumps(catalog))
        self.cli("sync", "--apply", "--agent", "opencode")
        rules = json.loads((self.home / ".config/opencode/opencode.json").read_text())["permissions"]
        self.assertEqual(rules[-1], {"action":"skill", "resource":"review", "effect":"deny"})

    def test_removed_mcp_is_removed_from_all_owned_configs(self):
        self.registered()
        self.cli("sync", "--apply")
        self.cli("mcp", "remove", "example")
        self.cli("sync", "--apply")
        self.assertNotIn("example", tomllib.loads((self.home / ".codex/config.toml").read_text()).get("mcp_servers", {}))
        self.assertNotIn("example", json.loads((self.home / ".claude.json").read_text())["mcpServers"])
        self.assertNotIn("example", json.loads((self.home / ".config/opencode/opencode.json").read_text())["mcp"]["servers"])

    def test_native_duplicate_json_keys_are_rejected(self):
        path = self.write(".claude.json", '{"mcpServers":{},"mcpServers":{"example":{"command":"/usr/bin/false"}}}')
        self.registered("claude")
        before = path.read_bytes()
        self.cli("sync", "--apply", "--agent", "claude", ok=False)
        self.assertEqual(path.read_bytes(), before)

    def test_invalid_native_shapes_do_not_crash_or_write(self):
        cases = [("claude", ".claude.json", {"mcpServers": []}),
                 ("claude", ".claude.json", {"mcpServers": {"bad": "invalid"}}),
                 ("claude", ".claude.json", {"mcpServers": {"bad": {"command":"/usr/bin/true", "args":"invalid"}}}),
                 ("opencode", ".config/opencode/opencode.json", {"mcp": []}),
                 ("opencode", ".config/opencode/opencode.json", {"mcp":{"servers":[]}})]
        for agent, relative, document in cases:
            path = self.write(relative, json.dumps(document))
            before = path.read_bytes()
            result = self.cli("import", "--from", agent, "--apply", ok=False)
            self.assertGreater(result.returncode, 0)
            self.assertEqual(path.read_bytes(), before)

    def test_open_code_agent_skill_override_conflict_is_explicit(self):
        self.write(".config/opencode/opencode.json", json.dumps({"agents":{"build":{"permissions":[
            {"action":"skill", "resource":"*", "effect":"allow"}]}}}))
        self.cli("mcp", "group", "add", "seed")
        path = self.home / ".config/kitmux/catalog.json"
        catalog = json.loads(path.read_text())
        catalog["skills"]["review"] = {"name":"review", "path":str(self.home / "unused"), "enabled":True, "agents":{}}
        path.write_text(json.dumps(catalog))
        result = self.cli("sync", "--apply", "--agent", "opencode", ok=False)
        self.assertIn("permission", result.stderr.lower())

    def test_skill_remove_and_restore_only_detach_owned_links(self):
        self.cli("mcp", "group", "add", "seed")
        package = self.home / "package"
        package.mkdir()
        (package / "SKILL.md").write_text("---\nname: review\ndescription: Review code\n---\nReview.")
        path = self.home / ".config/kitmux/catalog.json"
        catalog = json.loads(path.read_text())
        catalog["skills"]["review"] = {"name":"review", "path":str(package), "enabled":True,
                                           "agents":{"codex":"on", "claude":"on", "opencode":"on"}}
        path.write_text(json.dumps(catalog))
        self.cli("sync", "--apply")
        self.cli("restore", "--apply")
        for relative in (".agents/skills/review", ".claude/skills/review", ".config/opencode/skills/review"):
            self.assertFalse((self.home / relative).is_symlink())
        self.assertTrue((package / "SKILL.md").exists())

    def test_restore_releases_client_until_explicit_sync(self):
        self.registered("claude")
        self.cli("sync", "--apply", "--agent", "claude")
        self.cli("restore", "--apply", "--agent", "claude")
        path = self.home / ".claude.json"
        before = path.read_bytes()
        self.cli("mcp", "enable", "example", "--agent", "claude")
        self.assertEqual(path.read_bytes(), before)
        self.cli("sync", "--apply", "--agent", "claude")
        self.assertIn("example", json.loads(path.read_text())["mcpServers"])

    def test_all_eight_skill_assignments_have_independent_projections(self):
        self.cli("mcp", "group", "add", "seed")
        package = self.home / "package"
        package.mkdir()
        (package / "SKILL.md").write_text("---\nname: review\ndescription: Review code\n---\nReview.")
        path = self.home / ".config/kitmux/catalog.json"
        catalog = json.loads(path.read_text())
        roots = {"codex":".agents/skills", "claude":".claude/skills", "opencode":".config/opencode/skills"}
        for mask in [*range(8), 0]:
            selected = {agent: "on" if mask & (1 << bit) else "off" for bit, agent in enumerate(roots)}
            catalog["skills"]["review"] = {"name":"review", "path":str(package), "enabled":True, "agents":selected}
            path.write_text(json.dumps(catalog))
            self.cli("sync", "--apply")
            for agent, root in roots.items():
                self.assertEqual((self.home / root / "review").is_symlink(), selected[agent]=="on", (mask, agent))
            rules = json.loads((self.home / ".config/opencode/opencode.json").read_text()).get("permissions", [])
            deny = {"action":"skill", "resource":"review", "effect":"deny"}
            self.assertEqual(deny in rules, selected["opencode"]=="off", mask)


if __name__ == "__main__":
    unittest.main()
