import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class SubagentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.catalog = self.home / ".config/kitmux/catalog.json"

    def cli(self, *args, ok=True):
        result = subprocess.run([BINARY, "--home", str(self.home), *args],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0 if ok else 1, result.stdout + result.stderr)
        return result

    def source(self, text="Review changes carefully.\n", name="review.md"):
        source = self.home / name
        source.write_text(text)
        return source

    def add(self, name="reviewer"):
        self.cli("subagent", "add", name, "--from", str(self.source()),
                 "--description", "Review code changes")

    def status(self):
        return json.loads(self.cli("status", "--json").stdout)

    def test_plain_definition_starts_unassigned_and_can_be_inspected(self):
        self.add()
        resource = json.loads(self.cli("subagent", "show", "reviewer", "--json").stdout)
        self.assertEqual(resource["description"], "Review code changes")
        self.assertEqual(resource["instructions"], "Review changes carefully.\n")
        rows = self.status()["subagents"]["reviewer"]["agents"]
        for agent in ("codex", "claude", "opencode"):
            self.assertFalse(rows[agent]["enabled"])
            self.assertTrue(rows[agent]["compatible"])
            self.assertEqual(rows[agent]["runtime"], "native")
            self.assertEqual(rows[agent]["refresh"], "unverified")
        self.assertIn("reviewer", json.loads(self.cli("subagent", "list", "--json").stdout))
        self.assertEqual(json.loads(self.catalog.read_text())["version"], 2)

    def test_scoped_groups_union_explicit_off_and_inherit(self):
        self.add()
        self.cli("mcp", "add", "search", "--command", "/usr/bin/true")
        self.cli("subagent", "group", "add", "engineering")
        self.cli("subagent", "group", "include", "engineering", "subagent:reviewer")
        self.cli("subagent", "group", "assign", "engineering", "--agents", "codex,claude")
        self.cli("subagent", "group", "add", "review")
        self.cli("subagent", "group", "include", "review", "subagent:reviewer")
        self.cli("subagent", "group", "assign", "review", "--agents", "codex")
        self.cli("subagent", "group", "disable", "engineering")
        rows = self.status()["subagents"]["reviewer"]["agents"]
        self.assertTrue(rows["codex"]["enabled"])
        self.assertFalse(rows["claude"]["enabled"])
        self.cli("subagent", "disable", "reviewer", "--agent", "codex")
        self.assertFalse(self.status()["subagents"]["reviewer"]["agents"]["codex"]["enabled"])
        self.cli("subagent", "inherit", "reviewer", "--agent", "codex")
        self.assertTrue(self.status()["subagents"]["reviewer"]["agents"]["codex"]["enabled"])
        self.cli("subagent", "disable", "reviewer")
        self.assertFalse(self.status()["subagents"]["reviewer"]["agents"]["codex"]["enabled"])

    def test_all_client_assignments_are_independent(self):
        self.add()
        agents = ("codex", "claude", "opencode")
        for mask in range(8):
            for bit, agent in enumerate(agents):
                self.cli("subagent", "enable" if mask & (1 << bit) else "disable", "reviewer", "--agent", agent)
            rows = self.status()["subagents"]["reviewer"]["agents"]
            for bit, agent in enumerate(agents):
                self.assertEqual(rows[agent]["enabled"], bool(mask & (1 << bit)))

    def test_edit_preserves_policy_and_duplicate_starts_unassigned(self):
        self.add()
        self.cli("subagent", "enable", "reviewer", "--agent", "claude")
        self.cli("subagent", "edit", "reviewer", "--from", str(self.source("New instructions.")),
                 "--description", "New purpose")
        self.cli("subagent", "duplicate", "reviewer", "reviewer-copy")
        resource = json.loads(self.cli("subagent", "show", "reviewer").stdout)
        duplicate = json.loads(self.cli("subagent", "show", "reviewer-copy").stdout)
        self.assertEqual(resource["instructions"], "New instructions.")
        self.assertEqual(resource["agents"]["claude"], "on")
        self.assertEqual(duplicate["instructions"], resource["instructions"])
        self.assertEqual(duplicate["agents"], {})
        self.assertTrue(duplicate["enabled"])

    def test_remove_cleans_group_membership(self):
        self.add()
        self.cli("subagent", "group", "add", "engineering")
        self.cli("subagent", "group", "include", "engineering", "subagent:reviewer")
        self.cli("subagent", "remove", "reviewer")
        data = self.status()
        self.assertEqual(data["subagents"], {})
        self.assertEqual(data["groups"]["subagent:engineering"]["members"], [])

    def test_invalid_changes_preserve_catalog(self):
        self.add()
        before = self.catalog.read_bytes()
        empty = self.source("", "empty.md")
        commands = [
            ("subagent", "add", "../escape", "--from", str(self.source()), "--description", "Bad"),
            ("subagent", "add", "missing-description", "--from", str(self.source())),
            ("subagent", "edit", "reviewer", "--from", str(empty)),
            ("subagent", "edit", "reviewer", "--description", ""),
            ("subagent", "duplicate", "reviewer", "reviewer"),
            ("subagent", "enable", "reviewer", "--agent", "unknown"),
            ("subagent", "list", "--unknown"),
        ]
        for command in commands:
            with self.subTest(command=command):
                self.cli(*command, ok=False)
                self.assertEqual(self.catalog.read_bytes(), before)

    def test_v1_read_is_non_mutating_and_first_subagent_upgrades(self):
        self.cli("mcp", "add", "seed", "--command", "/usr/bin/true")
        old = json.loads(self.catalog.read_text())
        old["groups"]["existing"] = {"enabled": True, "agents": [], "members": []}
        old.pop("subagents", None)
        old["version"] = 1
        self.catalog.write_text(json.dumps(old))
        before = self.catalog.read_bytes()
        self.assertEqual(self.status()["subagents"], {})
        self.assertEqual(self.catalog.read_bytes(), before)
        self.add()
        new = json.loads(self.catalog.read_text())
        self.assertEqual(new["version"], 2)
        self.assertIn("existing", new["groups"])
        self.assertEqual(new["revision"], old["revision"] + 1)

    def test_corrupt_subagent_schema_is_rejected_without_writes(self):
        self.add()
        original = json.loads(self.catalog.read_text())
        cases = [None, [], {"bad/id": {}}, {"reviewer": {"description": "x", "instructions": 7}},
                 {"reviewer": {"description": "x", "instructions": "y", "portable": "yes"}}]
        for invalid in cases:
            data = copy.deepcopy(original)
            data["subagents"] = invalid
            self.catalog.write_text(json.dumps(data))
            before = self.catalog.read_bytes()
            self.cli("status", "--json", ok=False)
            self.assertEqual(self.catalog.read_bytes(), before)

    def test_native_definition_cannot_silently_expand_to_another_client(self):
        source = self.source('---\nname: reviewer\ndescription: Review code\ntools: Read\n---\nReview safely.\n')
        self.cli("subagent", "add", "reviewer", "--from", str(source), "--format", "claude")
        self.cli("subagent", "enable", "reviewer", "--agent", "claude")
        self.cli("subagent", "enable", "reviewer", "--agent", "codex")
        rows = self.status()["subagents"]["reviewer"]["agents"]
        self.assertTrue(rows["claude"]["enabled"])
        self.assertFalse(rows["codex"]["enabled"])
        self.assertFalse(rows["codex"]["compatible"])
        self.assertIn("native", rows["codex"]["reason"].lower())

    def test_attach_variant_preserves_shared_baseline_and_duplicate_identity(self):
        claude = self.source('---\nname: reviewer\ndescription: Claude purpose\ntools: Read\n---\nClaude instructions.\n')
        self.cli("subagent", "add", "reviewer", "--from", str(claude), "--format", "claude")
        codex_text = 'name = "reviewer"\ndescription = "Codex purpose"\ndeveloper_instructions = "Codex instructions."\nsandbox_mode = "read-only"\n'
        codex = self.source(codex_text, "reviewer.toml")
        self.cli("subagent", "edit", "reviewer", "--from", str(codex), "--format", "codex")
        resource = json.loads(self.catalog.read_text())["subagents"]["reviewer"]
        self.assertEqual(resource["description"], "Claude purpose")
        self.assertEqual(resource["native"]["codex"]["text"], codex_text)
        self.assertEqual(resource["native"]["codex"]["description"], "Claude purpose")
        self.assertEqual(resource["native"]["codex"]["instructions"], "Claude instructions.\n")
        self.cli("subagent", "duplicate", "reviewer", "copy")
        duplicate = json.loads(self.catalog.read_text())["subagents"]["copy"]
        self.assertIn('name = "copy"', duplicate["native"]["codex"]["text"])
        self.assertIn('sandbox_mode = "read-only"', duplicate["native"]["codex"]["text"])
        self.assertIn("Codex instructions.", duplicate["native"]["codex"]["text"])
        self.assertIn("copy", duplicate["native"]["claude"]["text"])
        self.assertEqual(duplicate["agents"], {})
        self.assertNotIn("origins", duplicate)

    def test_dependency_diagnostics_do_not_grant_managed_resources(self):
        source = self.source('---\nname: reviewer\ndescription: Review code\nskills:\n  - review-guidance\nmcpServers:\n  - search\n---\nReview.\n')
        self.cli("subagent", "add", "reviewer", "--from", str(source), "--format", "claude")
        self.cli("mcp", "add", "search", "--command", "/usr/bin/true")
        self.cli("subagent", "enable", "reviewer", "--agent", "claude")
        status = self.status()
        self.assertFalse(status["mcps"]["search"]["agents"]["claude"]["enabled"])
        row = status["subagents"]["reviewer"]["agents"]["claude"]
        dependencies = {item["resource"]: item["state"] for item in row["dependencies"]}
        self.assertEqual(dependencies, {"mcp:search": "unavailable", "skill:review-guidance": "unresolved_native"})

    def test_subagent_import_does_not_apply_other_pending_resources(self):
        self.cli("mcp", "add", "search", "--command", "/usr/bin/true")
        self.cli("mcp", "enable", "search", "--agent", "claude")
        self.cli("sync", "--apply", "--agent", "claude")
        native = self.home / ".claude.json"
        before = native.read_bytes()
        desired = json.loads(self.catalog.read_text())
        desired["mcps"]["search"]["command"] = ["/usr/bin/false"]
        self.catalog.write_text(json.dumps(desired))
        definition = self.home / ".claude/agents/reviewer.md"
        definition.parent.mkdir(parents=True)
        definition.write_text('---\nname: reviewer\ndescription: Review\n---\nReview.\n')
        self.cli("subagent", "import", "--from", "claude", "--apply")
        self.assertEqual(native.read_bytes(), before)
        self.assertIn("reviewer", json.loads(self.catalog.read_text())["subagents"])

    def test_adopted_opencode_storage_format_cannot_change_during_edit(self):
        definition = self.home / ".config/opencode/agents/reviewer-file.md"
        definition.parent.mkdir(parents=True)
        markdown = '---\ndescription: Review\nmode: subagent\n---\nReview.\n'
        definition.write_text(markdown)
        config = self.home / ".config/opencode/opencode.json"
        native_json = {"mode": "subagent", "description": "Review", "prompt": "Review."}
        config.write_text(json.dumps({"agents": {"reviewer-json": native_json}}))
        self.cli("subagent", "import", "--from", "opencode", "--apply")
        before = self.catalog.read_bytes()
        json_source = self.source(json.dumps(native_json), "reviewer.json")
        md_source = self.source(markdown)
        self.cli("subagent", "edit", "reviewer-file", "--from", str(json_source),
                 "--format", "opencode-json", ok=False)
        self.assertEqual(self.catalog.read_bytes(), before)
        self.cli("subagent", "edit", "reviewer-json", "--from", str(md_source),
                 "--format", "opencode", ok=False)
        self.assertEqual(self.catalog.read_bytes(), before)

    def test_owned_opencode_file_cannot_change_to_json_during_edit(self):
        self.add()
        self.cli("subagent", "enable", "reviewer", "--agent", "opencode")
        self.cli("sync", "--apply", "--agent", "opencode")
        native = self.home / ".config/opencode/agents/reviewer.md"
        config = self.home / ".config/opencode/opencode.json"
        journal = self.home / ".local/state/kitmux/adapters.json"
        journal_before = journal.read_bytes()
        before = self.catalog.read_bytes(), native.read_bytes(), config.read_bytes() if config.exists() else None
        source = self.source(json.dumps({"mode": "subagent", "description": "Review", "prompt": "Review."}), "reviewer.json")
        self.cli("subagent", "edit", "reviewer", "--from", str(source), "--format", "opencode-json", ok=False)
        self.assertEqual((self.catalog.read_bytes(), native.read_bytes(), config.read_bytes() if config.exists() else None), before)
        self.assertEqual(journal.read_bytes(), journal_before)
        self.cli("sync", "--apply", "--agent", "opencode")

    def test_owned_opencode_json_cannot_change_to_file_during_edit(self):
        source = self.source(json.dumps({"mode": "subagent", "description": "Review", "prompt": "Review."}), "reviewer.json")
        self.cli("subagent", "add", "reviewer", "--from", str(source), "--format", "opencode-json")
        self.cli("subagent", "enable", "reviewer", "--agent", "opencode")
        self.cli("sync", "--apply", "--agent", "opencode")
        config = self.home / ".config/opencode/opencode.json"
        native = self.home / ".config/opencode/agents/reviewer.md"
        journal = self.home / ".local/state/kitmux/adapters.json"
        journal_before = journal.read_bytes()
        before = self.catalog.read_bytes(), config.read_bytes()
        source = self.source('---\ndescription: Review\nmode: subagent\n---\nReview.\n')
        self.cli("subagent", "edit", "reviewer", "--from", str(source), "--format", "opencode", ok=False)
        self.assertEqual((self.catalog.read_bytes(), config.read_bytes()), before)
        self.assertEqual(journal.read_bytes(), journal_before)
        self.assertFalse(native.exists())
        self.cli("sync", "--apply", "--agent", "opencode")

    def test_first_opencode_native_variant_is_allowed_before_projection(self):
        self.add()
        source = self.source(json.dumps({"mode": "subagent", "description": "Review", "prompt": "Review."}), "reviewer.json")
        self.cli("subagent", "edit", "reviewer", "--from", str(source), "--format", "opencode-json")
        resource = json.loads(self.catalog.read_text())["subagents"]["reviewer"]
        self.assertEqual(resource["native"]["opencode"]["format"], "opencode-json")

    def test_status_and_list_do_not_dump_instructions_or_native_secrets(self):
        source = self.source('---\nname: reviewer\ndescription: Review code\nenv:\n  PRIVATE_TOKEN: sensitive-token\n---\nInstructions contain private-prompt.\n')
        self.cli("subagent", "add", "reviewer", "--from", str(source), "--format", "claude")
        for command in (("status", "--json"), ("subagent", "list"), ("subagent", "list", "--json"),
                        ("subagent", "show", "reviewer", "--json")):
            output = self.cli(*command).stdout
            self.assertNotIn("sensitive-token", output)
        self.assertNotIn("private-prompt", self.cli("status", "--json").stdout)
        self.assertNotIn("private-prompt", self.cli("subagent", "list", "--json").stdout)


if __name__ == "__main__":
    unittest.main()
