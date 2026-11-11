"""Opt-in Claude profile discovery with OS-enforced credential isolation."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import unittest

from native_clients.common import Sandbox
from native_clients.provider import LocalProvider

ENABLED = os.environ.get("KITMUX_NATIVE_TESTS") == "1"
SUPPORTED = sys.platform == "darwin" and Path("/usr/bin/sandbox-exec").is_file()


@unittest.skipUnless(ENABLED and SUPPORTED and shutil.which("claude"),
                     "opt-in macOS native Claude profile qualification")
class ClaudeSubagentNativeTests(unittest.TestCase):
    def setUp(self):
        self.sandbox = Sandbox("claude")
        self.addCleanup(self.sandbox.close)
        self.provider = LocalProvider()
        self.addCleanup(self.provider.close)
        self.sandbox.catalog_path.write_text(json.dumps({
            "version": 1, "revision": 0, "groups": {}, "skills": {}, "mcps": {}}))
        self.sandbox.env.update({
            "ANTHROPIC_BASE_URL": self.provider.url,
            "ANTHROPIC_API_KEY": "synthetic-fixture-key",
            "ENABLE_TOOL_SEARCH": "false",
        })
        self.executable = str(Path(shutil.which("claude")).resolve())

    def sandbox_profile(self):
        # --bare suppresses automatic user-agent discovery. The native loader
        # therefore runs normally, with account/config reads and remote traffic
        # blocked by macOS rather than relying on an authentication preference.
        return "\n".join([
            "(version 1)", "(allow default)", "(deny mach-lookup)",
            f"(deny file-read* (subpath {json.dumps(str(Path.home()))}))",
            f"(allow file-read* (literal {json.dumps(self.executable)}))",
            '(deny file-read* (subpath "/Library/Keychains"))',
            '(deny file-read* (subpath "/Library/Application Support/ClaudeCode"))',
            '(deny process-exec (literal "/usr/bin/security"))',
            "(deny network-outbound)",
            f'(allow network-outbound (remote ip "localhost:{self.provider.server.server_port}"))',
        ])

    def native(self):
        args = ["/usr/bin/sandbox-exec", "-p", self.sandbox_profile(), self.executable,
                "--print", "--output-format", "stream-json", "--verbose",
                "--strict-mcp-config", "--setting-sources", "user", "--tools", "",
                "--no-session-persistence", "--model", "claude-sonnet-4-5",
                "--agent", "kitmux-native-reviewer", "Reply with fixture-complete."]
        process = subprocess.Popen(args, cwd=self.sandbox.home, env=self.sandbox.env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=3)
            self.fail("Claude profile qualification timed out")
        packets = [json.loads(line) for line in stdout.splitlines() if line.strip()]
        return process.returncode, packets, stderr

    def assert_native_profile(self):
        before = len(self.provider.requests)
        code, packets, stderr = self.native()
        self.assertEqual(code, 0, stderr)
        init = next(packet for packet in packets
                    if packet.get("type") == "system" and packet.get("subtype") == "init")
        self.assertIn("kitmux-native-reviewer", init["agents"])
        result = next(packet for packet in packets if packet.get("type") == "result")
        self.assertFalse(result.get("is_error"), result)
        self.assertEqual(result["result"], "fixture-complete")
        requests = self.provider.requests[before:]
        self.assertTrue(requests)
        self.assertTrue(any("unique Kitmux subagent fixture instructions" in
                            json.dumps(request["body"].get("system")) for request in requests))

    def test_native_profile_discovery_disable_and_reenable(self):
        source = self.sandbox.home / "review.md"
        source.write_text("Use these unique Kitmux subagent fixture instructions.\n")
        self.sandbox.kitmux("subagent", "add", "kitmux-native-reviewer", "--from", str(source),
                            "--description", "Native profile qualification")
        self.sandbox.kitmux("subagent", "enable", "kitmux-native-reviewer", "--agent", "claude")
        self.sandbox.kitmux("sync", "--apply", "--agent", "claude")
        self.assert_native_profile()

        self.sandbox.kitmux("subagent", "disable", "kitmux-native-reviewer", "--agent", "claude")
        before = len(self.provider.requests)
        code, packets, stderr = self.native()
        self.assertNotEqual(code, 0)
        self.assertIn("--agent 'kitmux-native-reviewer' not found", stderr)
        self.assertEqual(self.provider.requests[before:], [])
        self.assertFalse(any(packet.get("type") == "assistant" for packet in packets))

        self.sandbox.kitmux("subagent", "enable", "kitmux-native-reviewer", "--agent", "claude")
        self.assert_native_profile()


if __name__ == "__main__":
    unittest.main()
