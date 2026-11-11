import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class ImportRefreshTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.home = Path(temporary.name)
        self.catalog = self.home / ".config/kitmux/catalog.json"
        self.journal = self.home / ".local/state/kitmux/adapters.json"

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

    def test_refresh_explicitly_adopts_codex_drift_without_native_rewrite_and_keeps_restore(self):
        original = '# owner\n[mcp_servers.docs]\ncommand = "/fixture/original"\nrequired = false\n'
        path = self.write(".codex/config.toml", original)
        self.cli("import", "--from", "codex", "--apply")
        changed = '# owner\n[mcp_servers.docs]\ncommand = "/fixture/changed"\nenabled = false\n'
        path.write_text(changed)
        before = (self.catalog.read_bytes(), self.journal.read_bytes())
        for command in ("import", "sync", "restore"):
            self.cli(command, "--agent", "codex", "--apply", ok=False)
            self.assertEqual(path.read_text(), changed)
            self.assertEqual((self.catalog.read_bytes(), self.journal.read_bytes()), before)
        self.cli("import", "--refresh", "--from", "codex", "--preview")
        self.assertEqual((self.catalog.read_bytes(), self.journal.read_bytes()), before)
        self.assertEqual(path.read_text(), changed)
        self.cli("import", "--refresh", "--from", "codex", "--apply")
        self.assertEqual(path.read_text(), changed)
        resource = json.loads(self.catalog.read_text())["mcps"]["docs"]
        self.assertEqual(resource["native"]["codex"]["command"], "/fixture/changed")
        self.assertEqual(resource["agents"]["codex"], "off")
        self.cli("sync", "--agent", "codex", "--apply")
        self.assertEqual(path.read_text(), changed)
        self.cli("restore", "--agent", "codex", "--apply")
        self.assertEqual(path.read_text(), original)

    def test_refresh_preserves_client_specific_subagent_prompts_and_options(self):
        paths = {
            "claude": self.write(".claude/agents/reviewer.md", "---\nname: reviewer\ndescription: Claude review\nmodel: inherit\n---\nOriginal Claude prompt.\n"),
            "opencode": self.write(".config/opencode/agents/reviewer.md", "---\ndescription: OpenCode review\nmode: subagent\n---\nOpenCode prompt.\n"),
        }
        self.cli("subagent", "import", "--from", "claude,opencode", "--apply")
        original = paths["claude"].read_text()
        changed = original.replace("Original Claude prompt.", "Updated private Claude prompt.").replace("model: inherit", "model: custom")
        paths["claude"].write_text(changed)
        self.cli("subagent", "import", "--from", "claude", "--apply", ok=False)
        before = (self.catalog.read_bytes(), self.journal.read_bytes())
        self.cli("subagent", "import", "--refresh", "--from", "claude", "--preview")
        self.assertEqual((self.catalog.read_bytes(), self.journal.read_bytes()), before)
        self.cli("subagent", "import", "--refresh", "--from", "claude", "--apply")
        self.cli("sync", "--apply")
        self.assertEqual(paths["claude"].read_text(), changed)
        self.assertIn("OpenCode prompt.", paths["opencode"].read_text())
        self.cli("restore", "--agent", "claude", "--apply")
        self.assertEqual(paths["claude"].read_text(), original)

    def test_managed_projection_drift_fails_closed_instead_of_importing_its_bridge(self):
        path = self.write(".codex/config.toml", '[mcp_servers.docs]\ncommand = "/fixture/upstream"\n')
        self.cli("import", "--from", "codex", "--apply")
        self.cli("mcp", "edit", "docs", "--mode", "managed")
        path.write_text(path.read_text() + 'required = false\n')
        before = (self.catalog.read_bytes(), self.journal.read_bytes(), path.read_bytes())
        result = self.cli("import", "--refresh", "--from", "codex", "--apply", ok=False)
        self.assertIn("managed", result.stderr.lower())
        self.assertEqual((self.catalog.read_bytes(), self.journal.read_bytes(), path.read_bytes()), before)
        self.assertEqual(json.loads(self.catalog.read_text())["mcps"]["docs"]["command"], ["/fixture/upstream"])

    def test_refresh_keeps_raw_mcp_assignment_when_global_off_hides_it(self):
        path = self.write(".codex/config.toml", '[mcp_servers.docs]\ncommand = "/fixture/server"\nrequired = false\n')
        self.cli("import", "--from", "codex", "--apply")
        self.cli("mcp", "disable", "docs")
        path.write_text(path.read_text().replace("required = false\n", ""))
        self.cli("import", "--refresh", "--from", "codex", "--apply")
        resource = json.loads(self.catalog.read_text())["mcps"]["docs"]
        self.assertFalse(resource["enabled"])
        self.assertEqual(resource["agents"]["codex"], "on")

    def test_refresh_keeps_subagent_inheritance_when_only_prompt_changes(self):
        path = self.write(".claude/agents/reviewer.md", "---\nname: reviewer\ndescription: Review\n---\nOriginal prompt.\n")
        self.cli("subagent", "import", "--from", "claude", "--apply")
        self.cli("group", "add", "engineering")
        self.cli("group", "include", "engineering", "subagent:reviewer")
        self.cli("group", "assign", "engineering", "--agents", "claude")
        self.cli("subagent", "inherit", "reviewer", "--agent", "claude")
        path.write_text(path.read_text().replace("Original prompt.", "Owner edited prompt."))
        self.cli("subagent", "import", "--refresh", "--from", "claude", "--apply")
        resource = json.loads(self.catalog.read_text())["subagents"]["reviewer"]
        self.assertEqual(resource["agents"]["claude"], "inherit")
        self.cli("group", "disable", "engineering")
        self.assertFalse(path.exists())

    def test_refresh_is_rejected_for_sync_restore_and_doctor(self):
        for command in ("sync", "restore", "doctor"):
            self.cli(command, "--refresh", "--apply", ok=False)
        self.assertFalse(self.catalog.exists())
        self.assertFalse(self.journal.exists())
