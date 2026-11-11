import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class SyncCheckTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.home = Path(temporary.name)

    def cli(self, *args, ok=True):
        result = subprocess.run([BINARY, "--home", str(self.home), *args],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0 if ok else 1, result.stdout + result.stderr)
        return result

    def snapshot(self):
        return {str(path.relative_to(self.home)): os.readlink(path) if path.is_symlink() else path.read_bytes()
                for path in self.home.rglob("*") if path.is_file() or path.is_symlink()}

    def test_empty_current_configuration_check_creates_nothing(self):
        self.cli("sync", "--check")
        self.assertEqual(list(self.home.iterdir()), [])

    def test_planned_config_changes_fail_read_only_until_applied(self):
        self.cli("mcp", "add", "docs", "--url", "https://example.test/mcp")
        self.cli("mcp", "enable", "docs", "--agent", "codex")
        before = self.snapshot()
        self.cli("sync", "--check", "--agent", "codex", ok=False)
        self.assertEqual(self.snapshot(), before)
        self.cli("sync", "--apply", "--agent", "codex")
        before = self.snapshot()
        self.cli("sync", "--check", "--agent", "codex")
        self.assertEqual(self.snapshot(), before)

    def test_pending_journal_fails_check_even_when_native_bytes_match(self):
        self.cli("sync", "--apply", "--agent", "claude")
        journal = self.home / ".local/state/kitmux/adapters.json"
        document = json.loads(journal.read_text())
        document["pending"] = True
        journal.write_text(json.dumps(document))
        before = self.snapshot()
        self.cli("sync", "--check", "--agent", "claude", ok=False)
        self.assertEqual(self.snapshot(), before)
        self.cli("sync", "--apply", "--agent", "claude")
        self.cli("sync", "--check", "--agent", "claude")

    def test_projection_changes_are_reported_before_adoption(self):
        package = self.home / "review"
        package.mkdir()
        (package / "SKILL.md").write_text("---\nname: review\ndescription: Review code\n---\nReview.\n")
        source = self.home / "reviewer.md"
        source.write_text("Review code.\n")
        self.cli("skill", "add", str(package))
        self.cli("skill", "enable", "review", "--agent", "claude")
        self.cli("subagent", "add", "reviewer", "--from", str(source), "--description", "Review code")
        self.cli("subagent", "enable", "reviewer", "--agent", "claude")
        before = self.snapshot()
        self.cli("sync", "--check", "--agent", "claude", ok=False)
        self.assertEqual(self.snapshot(), before)
        self.cli("sync", "--apply", "--agent", "claude")
        self.cli("sync", "--check", "--agent", "claude")

    def test_check_rejects_apply_and_other_adapter_commands(self):
        for command in ("import", "restore", "doctor"):
            self.cli(command, "--check", ok=False)
        self.cli("sync", "--check", "--apply", ok=False)
        self.assertFalse((self.home / ".local/state/kitmux/adapters.json").exists())
