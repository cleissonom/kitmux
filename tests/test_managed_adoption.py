import json
import os
from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class ManagedAdoptionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name)

    def cli(self, *args):
        result = subprocess.run([BINARY, "--home", str(self.home), *args],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def test_managed_adoption_preserves_native_policy_and_restores_original_toml(self):
        original = '''# Preserve the unrelated top-level comment.
model = "local-model" # unrelated setting

[mcp_servers.native]
command = "/usr/bin/env" # keep command annotation
args = ["python", "-m", "native_mcp"] # keep args annotation
enabled = false # preserve the native availability comment
enabled_tools = ["read"] # preserve allow policy
disabled_tools = ["delete"] # preserve deny policy
default_tools_approval_mode = "prompt" # preserve approval policy

[mcp_servers.native.env]
TOKEN = "fixture-value" # preserve nested auth comment
REGION = "test-region"

[mcp_servers.native.tools.read]
approval_mode = "approve" # preserve tool approval

[unrelated]
value = "leave-me-alone" # preserve unrelated table
'''
        config = self.home / ".codex/config.toml"
        config.parent.mkdir(parents=True)
        config.write_text(original)

        self.cli("import", "--from", "codex", "--apply")
        self.cli("mcp", "edit", "native", "--mode", "managed")

        managed_text = config.read_text()
        managed = tomllib.loads(managed_text)["mcp_servers"]["native"]
        self.assertNotEqual(managed["command"], "/usr/bin/env")
        self.assertIn("bridge", managed["args"])
        self.assertIn("--agent", managed["args"])
        self.assertTrue(managed["enabled"])
        self.assertEqual(managed["enabled_tools"], ["read"])
        self.assertEqual(managed["disabled_tools"], ["delete"])
        self.assertEqual(managed["default_tools_approval_mode"], "prompt")
        self.assertEqual(managed["env"], {"TOKEN": "fixture-value", "REGION": "test-region"})
        self.assertEqual(managed["tools"]["read"]["approval_mode"], "approve")
        expected = original.replace('command = "/usr/bin/env"',
                                    f'command = {json.dumps(managed["command"])}')
        expected = expected.replace('args = ["python", "-m", "native_mcp"]',
                                    f'args = {json.dumps(managed["args"])}')
        expected = expected.replace("enabled = false", "enabled = true")
        self.assertEqual(managed_text, expected)
        for comment in (
            "# Preserve the unrelated top-level comment.", "# unrelated setting",
            "# keep command annotation", "# keep args annotation",
            "# preserve the native availability comment", "# preserve allow policy",
            "# preserve deny policy", "# preserve approval policy",
            "# preserve nested auth comment", "# preserve tool approval",
            "# preserve unrelated table",
        ):
            self.assertIn(comment, managed_text)

        self.cli("mcp", "disable", "native", "--agent", "codex")
        disabled = tomllib.loads(config.read_text())["mcp_servers"]["native"]
        self.assertTrue(disabled["enabled"])
        self.assertEqual(disabled["command"], managed["command"])
        self.assertEqual(disabled["args"], managed["args"])

        self.cli("mcp", "edit", "native", "--mode", "direct")
        direct = tomllib.loads(config.read_text())["mcp_servers"]["native"]
        self.assertEqual(direct["command"], "/usr/bin/env")
        self.assertEqual(direct["args"], ["python", "-m", "native_mcp"])
        self.assertFalse(direct["enabled"])
        self.assertEqual(direct["enabled_tools"], ["read"])
        self.assertEqual(direct["disabled_tools"], ["delete"])
        self.assertEqual(direct["default_tools_approval_mode"], "prompt")
        self.assertEqual(direct["env"], {"TOKEN": "fixture-value", "REGION": "test-region"})
        self.assertEqual(direct["tools"]["read"]["approval_mode"], "approve")
        self.assertEqual(config.read_text(), original)

        self.cli("restore", "--apply", "--agent", "codex")
        self.assertEqual(config.read_text(), original)

    def test_managed_adoption_removes_generated_args_when_native_args_were_absent(self):
        original = '''[mcp_servers.no_args]
command = "/usr/bin/native" # preserve command comment

[mcp_servers.no_args.env]
TOKEN = "fixture-value"

[unrelated]
value = "preserve"
'''
        config = self.home / ".codex/config.toml"
        config.parent.mkdir(parents=True)
        config.write_text(original)

        self.cli("import", "--from", "codex", "--apply")
        self.cli("mcp", "edit", "no_args", "--mode", "managed")
        managed_text = config.read_text()
        managed = tomllib.loads(managed_text)["mcp_servers"]["no_args"]
        self.assertIn("bridge", managed["args"])
        self.assertTrue(managed["enabled"])
        self.assertEqual(managed["env"], {"TOKEN": "fixture-value"})
        self.assertIn("# preserve command comment", managed_text)

        self.cli("mcp", "edit", "no_args", "--mode", "direct")
        direct_text = config.read_text()
        direct = tomllib.loads(direct_text)["mcp_servers"]["no_args"]
        self.assertEqual(direct["command"], "/usr/bin/native")
        self.assertNotIn("args", direct)
        self.assertTrue(direct["enabled"])
        self.assertEqual(direct["env"], {"TOKEN": "fixture-value"})

        self.cli("restore", "--apply", "--agent", "codex")
        self.assertEqual(config.read_text(), original)

    def test_multiline_owned_values_fail_closed_without_rewriting_native_toml(self):
        original = '''[mcp_servers.multiline]
command = "/usr/bin/native"
args = [
  "--first", # keep this value comment
  "--second",
]
'''
        config = self.home / ".codex/config.toml"
        config.parent.mkdir(parents=True)
        config.write_text(original)
        self.cli("import", "--from", "codex", "--apply")

        result = subprocess.run([BINARY, "--home", str(self.home), "mcp", "edit", "multiline",
                                 "--mode", "managed"], capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("multiline adopted args", result.stderr)
        self.assertEqual(config.read_text(), original)


if __name__ == "__main__":
    unittest.main()
