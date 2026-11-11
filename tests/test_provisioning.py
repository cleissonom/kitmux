import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class ProvisioningTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.home = Path(temporary.name)
        self.catalog = self.home / ".config/kitmux/catalog.json"

    def cli(self, *args, ok=True):
        result = subprocess.run([BINARY, "--home", str(self.home), *args],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0 if ok else 1, result.stdout + result.stderr)
        return result

    def write(self, relative, text):
        path = self.home / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def define(self, client, definition, editing=False):
        source = self.write("definition.json", json.dumps(definition))
        return self.cli("mcp", "edit" if editing else "add", "docs", "--from", str(source),
                        "--format", client)

    def test_native_definitions_preserve_per_client_options_and_begin_unassigned(self):
        definitions = {
            "codex": {"url": "https://codex.example.test/mcp", "enabled_tools": ["read"]},
            "claude": {"type": "stdio", "command": "/fixture/claude", "args": ["--read-only"]},
            "opencode": {"type": "local", "command": ["/fixture/opencode"], "timeout": 9000},
        }
        for client, definition in definitions.items():
            self.define(client, definition, editing=self.catalog.exists())
        resource = json.loads(self.catalog.read_text())["mcps"]["docs"]
        self.assertEqual(resource["native"], definitions)
        self.assertEqual(resource["agents"], {})
        self.cli("mcp", "enable", "docs", "--agent", "claude")
        self.cli("sync", "--agent", "claude", "--apply")
        self.assertEqual(json.loads((self.home / ".claude.json").read_text())["mcpServers"]["docs"],
                         definitions["claude"])

    def test_endpoint_matched_update_preserves_private_fields_policy_and_is_idempotent(self):
        path = self.write(".codex/config.toml", '# keep parent\nmodel = "owner-model"\n'
                          '[mcp_servers.docs]\nurl = "https://docs.example.test/mcp" # URL comment\n'
                          'enabled_tools = ["old"] # tools comment\n'
                          '[mcp_servers.docs.http_headers]\nAuthorization = "fixture-secret" # auth comment\n')
        self.cli("import", "--from", "codex", "--apply")
        self.cli("mcp", "disable", "docs", "--agent", "codex")
        self.cli("mcp", "disable", "docs")
        before = json.loads(self.catalog.read_text())["mcps"]["docs"]
        definition = {"url": "https://docs.example.test/mcp", "enabled_tools": ["read"]}
        result = self.define("codex", definition, editing=True)
        self.assertNotIn("fixture-secret", result.stdout + result.stderr)
        after = json.loads(self.catalog.read_text())["mcps"]["docs"]
        self.assertEqual(after["enabled"], before["enabled"])
        self.assertEqual(after["agents"], before["agents"])
        native = tomllib.loads(path.read_text())["mcp_servers"]["docs"]
        self.assertEqual(native["enabled_tools"], ["read"])
        self.assertEqual(native["http_headers"]["Authorization"], "fixture-secret")
        self.assertFalse(native["enabled"])
        self.assertIn("# tools comment", path.read_text())
        self.assertIn("# auth comment", path.read_text())
        snapshot = (self.catalog.read_bytes(), path.read_bytes())
        self.define("codex", definition, editing=True)
        self.assertEqual((self.catalog.read_bytes(), path.read_bytes()), snapshot)

    def test_endpoint_change_replaces_only_selected_client_without_carrying_credentials(self):
        self.define("codex", {"url": "https://old.example.test", "http_headers": {"Authorization": "old-secret"}})
        self.define("claude", {"type": "http", "url": "https://claude.example.test"}, editing=True)
        self.cli("mcp", "enable", "docs", "--agent", "codex")
        self.cli("sync", "--agent", "codex", "--apply")
        claude = copy.deepcopy(json.loads(self.catalog.read_text())["mcps"]["docs"]["native"]["claude"])
        self.define("codex", {"url": "https://new.example.test"}, editing=True)
        resource = json.loads(self.catalog.read_text())["mcps"]["docs"]
        self.assertEqual(resource["native"]["codex"], {"url": "https://new.example.test"})
        self.assertEqual(resource["native"]["claude"], claude)
        native = tomllib.loads((self.home / ".codex/config.toml").read_text())["mcp_servers"]["docs"]
        self.assertEqual(native["url"], "https://new.example.test")
        self.assertNotIn("old-secret", json.dumps(native))

    def test_stdio_native_update_merges_environment_and_keeps_nested_table_comments(self):
        path = self.write(".codex/config.toml", '[mcp_servers.docs]\ncommand = "/fixture/server"\n'
                          'args = ["old"] # arguments\n[mcp_servers.docs.env] # environment\n'
                          'TOKEN = "fixture-secret" # credential\nSETTING = "old"\n')
        self.cli("import", "--from", "codex", "--apply")
        self.define("codex", {"command": "/fixture/server", "args": ["old"],
                              "env": {"SETTING": "new", "ADDED": "yes"}}, editing=True)
        native = tomllib.loads(path.read_text())["mcp_servers"]["docs"]
        self.assertEqual(native["env"], {"TOKEN": "fixture-secret", "SETTING": "new", "ADDED": "yes"})
        self.assertIn("# credential", path.read_text())
        self.assertIn("# environment", path.read_text())

    def test_changed_stdio_working_directory_does_not_carry_native_credentials(self):
        self.define("codex", {"command": "./server", "cwd": "/old/project",
                              "env": {"TOKEN": "fixture-old-secret"}})
        self.cli("mcp", "enable", "docs", "--agent", "codex")
        self.cli("sync", "--agent", "codex", "--apply")
        self.define("codex", {"command": "./server", "cwd": "/new/project"}, editing=True)
        resource = json.loads(self.catalog.read_text())["mcps"]["docs"]
        self.assertNotIn("env", resource["native"]["codex"])
        native = tomllib.loads((self.home / ".codex/config.toml").read_text())["mcp_servers"]["docs"]
        self.assertEqual(native["cwd"], "/new/project")
        self.assertNotIn("fixture-old-secret", json.dumps(native))

    def test_shared_working_directory_edit_cannot_move_imported_relative_executable(self):
        self.write(".codex/config.toml", '[mcp_servers.docs]\ncommand = "./server"\ncwd = "/old/project"\n'
                   'env = { TOKEN = "fixture-old-secret" }\n')
        self.cli("import", "--from", "codex", "--apply")
        before = self.snapshot_policy_files()
        self.cli("mcp", "edit", "docs", "--cwd", "/new/project", ok=False)
        self.assertEqual(self.snapshot_policy_files(), before)

    def snapshot_policy_files(self):
        return {str(path.relative_to(self.home)): path.read_bytes()
                for path in self.home.rglob("*") if path.is_file() and not path.is_symlink()}

    def test_invalid_native_source_preserves_all_state(self):
        self.define("claude", {"url": "https://example.test/mcp"})
        before = self.catalog.read_bytes()
        for definition in ({"command": []}, {"url": 9}, {}, {"command": "x", "env": {"TOKEN": 3}}):
            source = self.write("invalid.json", json.dumps(definition))
            self.cli("mcp", "edit", "docs", "--from", str(source), "--format", "claude", ok=False)
            self.assertEqual(self.catalog.read_bytes(), before)
        source = self.write("invalid.json", '{"command":"/fixture/server","url":"https://ambiguous.test"}')
        self.cli("mcp", "edit", "docs", "--from", str(source), "--format", "claude", ok=False)
        self.assertEqual(self.catalog.read_bytes(), before)

    def test_unsupported_codex_source_is_rejected_before_catalog_commit(self):
        self.define("codex", {"url": "https://example.test/mcp"})
        before = self.catalog.read_bytes()
        source = self.write("unsupported.json", '{"url":"https://example.test/mcp","unsupported":null}')
        self.cli("mcp", "edit", "docs", "--from", str(source), "--format", "codex", ok=False)
        self.assertEqual(self.catalog.read_bytes(), before)

    def test_editing_unchanged_new_native_definition_is_a_noop(self):
        definition = {"url": "https://example.test/mcp"}
        self.define("codex", definition)
        before = self.catalog.read_bytes()
        self.define("codex", definition, editing=True)
        self.assertEqual(self.catalog.read_bytes(), before)

    def test_endpoint_change_removes_old_nested_credentials_and_preserves_comments(self):
        path = self.write(".codex/config.toml", '[mcp_servers.docs]\ncommand = "/fixture/old" # command\n'
                          '[mcp_servers.docs.env] # credential group\nTOKEN = "old-secret" # credential note\n')
        self.cli("import", "--from", "codex", "--apply")
        self.define("codex", {"command": "/fixture/new"}, editing=True)
        native = tomllib.loads(path.read_text())["mcp_servers"]["docs"]
        self.assertEqual(native["command"], "/fixture/new")
        self.assertNotIn("env", native)
        self.assertIn("# credential note", path.read_text())
        self.assertIn("# credential group", path.read_text())
        self.assertNotIn("old-secret", path.read_text())

    def test_skill_update_can_rebind_same_content_source_and_preserves_history_policy(self):
        text = "---\nname: review\ndescription: Review code\n---\nReview.\n"
        old = self.write("old/review/SKILL.md", text).parent
        new = self.write("new/review/SKILL.md", text).parent
        self.cli("skill", "add", str(old))
        self.cli("skill", "disable", "review", "--agent", "opencode")
        before = self.catalog.read_bytes()
        self.cli("skill", "update", "review", "--source", str(new), "--preview")
        self.assertEqual(self.catalog.read_bytes(), before)
        self.cli("skill", "update", "review", "--source", str(new), "--apply")
        resource = json.loads(self.catalog.read_text())["skills"]["review"]
        self.assertEqual(resource["source"], str(new.resolve()))
        self.assertEqual(resource["agents"], {"opencode": "off"})
        self.assertEqual(resource["previous"]["source"], str(old.resolve()))
        snapshot = self.catalog.read_bytes()
        self.cli("skill", "update", "review", "--source", str(new), "--apply")
        self.assertEqual(self.catalog.read_bytes(), snapshot)

    def test_skill_source_rebinding_does_not_reuse_previous_source_subdirectory(self):
        text = "---\nname: review\ndescription: Review code\n---\nReview.\n"
        old = self.write("old/skills/review/SKILL.md", text).parents[2]
        new = self.write("new/review/SKILL.md", text).parent
        self.cli("skill", "add", str(old), "--path", "skills/review")
        self.cli("skill", "update", "review", "--source", str(new), "--apply")
        resource = json.loads(self.catalog.read_text())["skills"]["review"]
        self.assertEqual(resource["source"], str(new.resolve()))
        self.assertEqual(resource["subdir"], "")

    def test_identical_native_subagent_edit_does_not_rewrite_catalog_or_projection(self):
        source = self.write("reviewer.md", "---\nname: reviewer\ndescription: Review\nmodel: inherit\n---\nReview.\n")
        self.cli("subagent", "add", "reviewer", "--from", str(source), "--format", "claude")
        self.cli("subagent", "enable", "reviewer", "--agent", "claude")
        self.cli("sync", "--agent", "claude", "--apply")
        projection = self.home / ".claude/agents/reviewer.md"
        before = (self.catalog.read_bytes(), projection.read_bytes())
        self.cli("subagent", "edit", "reviewer", "--from", str(source), "--format", "claude")
        self.assertEqual((self.catalog.read_bytes(), projection.read_bytes()), before)

    def test_status_exposes_raw_policy_without_native_options_or_credentials(self):
        self.write(".codex/config.toml", '[mcp_servers.docs]\nurl = "https://example.test"\n'
                   'http_headers = { Authorization = "fixture-secret" }\n')
        self.cli("import", "--from", "codex", "--apply")
        self.cli("mcp", "enable", "docs", "--agent", "codex")
        self.cli("mcp", "disable", "docs", "--agent", "claude")
        self.cli("mcp", "disable", "docs")
        result = self.cli("status", "--json")
        row = json.loads(result.stdout)["mcps"]["docs"]
        self.assertFalse(row["enabled"])
        self.assertEqual(row["assignments"], {"codex": "on", "claude": "off"})
        self.assertFalse(row["agents"]["codex"]["enabled"])
        self.assertNotIn("fixture-secret", result.stdout)
