import json
import os
from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class SubagentFormatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)

    def cli(self, *args, ok=True):
        result = subprocess.run([BINARY, "--home", str(self.home), *args],
                                text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode == 0, ok, result.stderr)
        return result

    def source(self, name, text):
        path = self.home / name
        path.write_text(text)
        return str(path)

    def add(self, text, format):
        self.cli("subagent", "add", "reviewer", "--from", self.source("source", text), "--format", format)

    def sync(self, client):
        self.cli("subagent", "enable", "reviewer", "--agent", client)
        self.cli("sync", "--apply", "--agent", client)
        relative = ".codex/agents/reviewer.toml" if client == "codex" else ".claude/agents/reviewer.md" if client == "claude" else ".config/opencode/agents/reviewer.md"
        return self.home / relative

    def test_codex_native_round_trip_and_shared_edit_preserve_options(self):
        original = '# preserve policy\nname = "reviewer"\ndescription = "Code review"\ndeveloper_instructions = """Review carefully.\nReport risks."""\nmodel = "fixture-model"\nmodel_reasoning_effort = "high"\n[features]\njs_repl = false\n'
        self.add(original, "codex")
        path = self.sync("codex")
        self.assertEqual(path.read_text(), original)
        self.cli("subagent", "edit", "reviewer", "--description", "Review changes")
        parsed = tomllib.loads(path.read_text())
        self.assertEqual(parsed["description"], "Review changes")
        self.assertEqual(parsed["developer_instructions"], "Review carefully.\nReport risks.")
        self.assertEqual(parsed["model"], "fixture-model")
        self.assertFalse(parsed["features"]["js_repl"])
        self.assertIn("# preserve policy", path.read_text())

    def test_claude_native_edit_preserves_tools_and_unknown_frontmatter(self):
        original = '---\nname: reviewer\ndescription: >-\n  Review code\n  for defects.\ntools: Read, Grep\npermissionMode: plan\ncustom: {keep: true}\n---\nInspect changes.\n'
        self.add(original, "claude")
        path = self.sync("claude")
        self.assertEqual(path.read_text(), original)
        source = self.source("updated-prompt.md", "New instructions.\n")
        self.cli("subagent", "edit", "reviewer", "--from", source)
        self.assertIn("tools: Read, Grep", path.read_text())
        self.assertIn("permissionMode: plan", path.read_text())
        self.assertIn("custom: {keep: true}", path.read_text())
        self.assertTrue(path.read_text().endswith("New instructions.\n"))

    def test_native_profile_cannot_silently_drop_restrictions_for_other_client(self):
        self.add('---\nname: reviewer\ndescription: Review\ntools: Read\n---\nReview.\n', "claude")
        self.cli("subagent", "enable", "reviewer", "--agent", "codex")
        self.cli("sync", "--apply", "--agent", "codex")
        self.assertFalse((self.home / ".codex/agents/reviewer.toml").exists())
        status = json.loads(self.cli("status", "--json").stdout)
        self.assertEqual(status["subagents"]["reviewer"]["agents"]["codex"]["availability"], "incompatible")

    def test_malformed_frontmatter_and_codex_identity_are_rejected(self):
        cases = [("claude", '---\nname: reviewer\ndescription: Review\ndescription: Other\n---\nReview.'),
                 ("claude", '---\nname: different\ndescription: Review\n---\nReview.'),
                 ("claude", '---\nname: reviewer\ndescription: [not, a, string]\n---\nReview.'),
                 ("codex", 'name="different"\ndescription="Review"\ndeveloper_instructions="Review."\n'),
                 ("codex", 'name="reviewer"\ndescription="Review"\ndeveloper_instructions="Review.\\u0000Hidden"\n'),
                 ("opencode-json", '{"description":"Review","prompt":"Review.","mode":"subagent\\u0000primary"}'),
                 ("opencode", '---\ndescription: Review\nmode: primary\n---\nReview.')]
        for index, (format, text) in enumerate(cases):
            result = self.cli("subagent", "add", "reviewer", "--from", self.source(f"bad{index}", text), "--format", format, ok=False)
            self.assertNotIn("Traceback", result.stderr)

    def test_duplicate_changes_identity_without_dropping_native_policy(self):
        self.add('---\nname: reviewer\ndescription: Review\ntools: Read\n---\nReview.\n', "claude")
        self.cli("subagent", "duplicate", "reviewer", "auditor")
        self.cli("subagent", "enable", "auditor", "--agent", "claude")
        self.cli("sync", "--apply", "--agent", "claude")
        text = (self.home / ".claude/agents/auditor.md").read_text()
        self.assertIn('name: "auditor"', text)
        self.assertIn("tools: Read", text)

    def test_opencode_subagent_preserves_hidden_and_permissions(self):
        original = '---\ndescription: Review\nmode: subagent\nhidden: true\npermissions:\n  - action: bash\n    effect: deny\n---\nReview.\n'
        self.add(original, "opencode")
        path = self.sync("opencode")
        self.assertEqual(path.read_text(), original)
        self.cli("subagent", "edit", "reviewer", "--description", "Review changes")
        self.assertIn("hidden: true", path.read_text())
        self.assertIn("effect: deny", path.read_text())

    def test_invalid_utf8_source_cannot_poison_the_catalog(self):
        self.cli("group", "add", "existing")
        catalog = self.home / ".config/kitmux/catalog.json"
        before = catalog.read_bytes()
        cases = [("common", b"Review.\xff"),
                 ("codex", b'name="reviewer"\ndescription="Review"\ndeveloper_instructions="Review.\xff"\n')]
        for format, content in cases:
            source = self.home / "invalid-encoding"
            source.write_bytes(content)
            args = ["subagent", "add", "reviewer", "--from", str(source), "--format", format]
            if format == "common":
                args += ["--description", "Review"]
            self.cli(*args, ok=False)
            self.assertEqual(catalog.read_bytes(), before)
            self.cli("status", "--json")

    def test_unicode_before_frontmatter_edits_preserves_native_policy(self):
        original = '---\n# revisão 🧪\nname: reviewer\ndescription: "Revisão cuidadosa 🧪"\ntools: Read\npermissionMode: plan\n---\nInspecione alterações.\n'
        self.add(original, "claude")
        path = self.sync("claude")
        self.cli("subagent", "edit", "reviewer", "--description", "Revisão final")
        self.assertIn("# revisão 🧪", path.read_text())
        self.assertIn('description: "Revisão final"\n', path.read_text())
        self.assertIn("tools: Read\npermissionMode: plan\n", path.read_text())
        self.assertTrue(path.read_text().endswith("Inspecione alterações.\n"))
        self.cli("subagent", "duplicate", "reviewer", "auditor")
        self.cli("subagent", "enable", "auditor", "--agent", "claude")
        text = (self.home / ".claude/agents/auditor.md").read_text()
        self.assertIn('name: "auditor"\n', text)
        self.assertIn("tools: Read\npermissionMode: plan\n", text)

    def test_enabling_alias_disabled_flag_cannot_change_an_unrelated_native_setting(self):
        self.add('---\ndescription: Review\nmode: subagent\nhidden: &visibility true\ndisabled: *visibility\n---\nReview.\n', "opencode")
        path = self.sync("opencode")
        self.assertIn("hidden: &visibility true\n", path.read_text())
        self.assertIn("disabled: false\n", path.read_text())


if __name__ == "__main__":
    unittest.main()
