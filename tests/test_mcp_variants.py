import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import test_runtime as runtime


SCRIPT = r'''
import json, os, sys
for line in sys.stdin:
    request = json.loads(line)
    method = request.get("method")
    if method == "initialize":
        result = {"protocolVersion": request["params"]["protocolVersion"],
                  "capabilities": {"tools": {}}, "serverInfo": {"name": "variant-fixture", "version": "1"}}
    elif method == "tools/list":
        result = {"tools": [{"name": "identity", "inputSchema": {"type": "object"}}]}
    elif method == "tools/call":
        if request["params"].get("name") == "hang":
            print(json.dumps({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}), flush=True)
            continue
        observation = {"variant": sys.argv[1], "pid": os.getpid(), "cwd": os.getcwd(),
                       "base": os.environ.get("KITMUX_VARIANT_BASE"),
                       "agent": os.environ.get("KITMUX_VARIANT_AGENT"),
                       "shared": os.environ.get("KITMUX_VARIANT_SHARED")}
        result = {"content": [{"type": "text", "text": json.dumps(observation)}],
                  "_meta": request["params"].get("_meta", {})}
    else:
        continue
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
'''


class McpVariantTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="kmv-", dir="/tmp")
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.catalog = self.home / ".config/kitmux/catalog.json"
        self.catalog.parent.mkdir(parents=True)
        self.claude_cwd = self.home / "claude-work"
        self.claude_cwd.mkdir()
        self.resource = {"enabled": True, "agents": {agent: "on" for agent in ("codex", "claude", "opencode")},
                         "transport": "stdio", "mode": "managed", "command": self.command_for("base"),
                         "env": {"KITMUX_VARIANT_BASE": "base-only", "KITMUX_VARIANT_SHARED": "base-value"},
                         "variants": {
                             "claude": {"transport": "stdio", "command": self.command_for("claude"),
                                        "env": {"KITMUX_VARIANT_AGENT": "claude-only"}, "cwd": str(self.claude_cwd)},
                             "opencode": {"transport": "stdio", "command": self.command_for("opencode")}}}
        self.config = {"version": 1, "revision": 1, "groups": {}, "skills": {}, "mcps": {"stub": self.resource}}
        self.save()

    @staticmethod
    def command_for(label):
        return [sys.executable, "-u", "-c", SCRIPT, label]

    def save(self):
        temporary = self.catalog.with_suffix(".tmp")
        temporary.write_text(json.dumps(self.config))
        temporary.replace(self.catalog)

    def command(self, *args):
        return subprocess.run([runtime.BINARY, "--home", str(self.home), *args],
                              text=True, capture_output=True, timeout=10)

    def bridge(self, agent):
        harness = runtime.RuntimeTests("test_disabled_bridge_stays_alive_without_upstream")
        harness.home = self.home
        environment = dict(os.environ)
        for name in ("KITMUX_VARIANT_BASE", "KITMUX_VARIANT_AGENT", "KITMUX_VARIANT_SHARED"):
            environment.pop(name, None)
        harness.process = subprocess.Popen(
            [runtime.BINARY, "--home", str(self.home), "bridge", "stub", "--agent", agent],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)
        harness.buffer = b""
        harness.addCleanup(harness.close_bridge)
        self.addCleanup(harness.doCleanups)
        harness.send({"id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "variant-test", "version": "1"}}})
        self.assertIn("result", harness.reply(1))
        harness.send({"method": "notifications/initialized"})
        return harness

    def observe(self, bridge, request_id=3, meta=None):
        result = bridge.call(request_id, meta=meta)
        self.assertIn("result", result, result)
        return json.loads(result["result"]["content"][0]["text"]), result["result"]["_meta"]

    def test_each_agent_launches_its_full_execution_without_optional_field_leakage(self):
        bridges = {agent: self.bridge(agent) for agent in ("codex", "claude", "opencode")}
        observations = {}
        for agent, bridge in bridges.items():
            meta = {"ai.opencode/sessionID": f"session-{agent}", "custom": {"agent": agent}}
            observations[agent], returned_meta = self.observe(bridge, meta=meta)
            self.assertEqual(returned_meta, meta)
        self.assertEqual(observations["codex"]["variant"], "base")
        self.assertEqual(observations["claude"]["variant"], "claude")
        self.assertEqual(observations["opencode"]["variant"], "opencode")
        self.assertEqual(observations["codex"]["base"], "base-only")
        self.assertEqual(observations["claude"]["agent"], "claude-only")
        self.assertIsNone(observations["claude"]["base"])
        self.assertIsNone(observations["claude"]["shared"])
        self.assertIsNone(observations["opencode"]["base"])
        self.assertEqual(observations["claude"]["cwd"], str(self.claude_cwd.resolve()))
        self.assertEqual(observations["opencode"]["cwd"], str(Path.cwd().resolve()))
        self.assertEqual(len({row["pid"] for row in observations.values()}), 3)

    def test_other_agent_execution_change_preserves_this_upstream_and_its_pending_call(self):
        bridge = self.bridge("codex")
        before, _ = self.observe(bridge)
        bridge.send({"id": "pending", "method": "tools/call", "params": {"name": "hang", "arguments": {}}})
        self.assertEqual(bridge.packet()["method"], "notifications/tools/list_changed")
        self.resource["variants"]["claude"]["command"] = self.command_for("new-claude")
        self.save()
        after, _ = self.observe(bridge, 4)
        self.assertEqual(before["pid"], after["pid"])
        status = self.command("mcp", "status", "stub", "--agent", "codex", "--json")
        self.assertEqual(status.returncode, 0, status.stderr)
        self.assertEqual(json.loads(status.stdout)["connections"][0]["pending"], 1)

    def test_base_execution_change_does_not_restart_a_complete_agent_variant(self):
        bridge = self.bridge("claude")
        before, _ = self.observe(bridge)
        self.resource["command"] = self.command_for("updated-base")
        self.resource["env"]["KITMUX_VARIANT_SHARED"] = "updated-base-environment"
        self.save()
        after, _ = self.observe(bridge, 4)
        self.assertEqual(before["pid"], after["pid"])
        self.assertEqual(after["variant"], "claude")
        self.assertIsNone(after["shared"])

    def test_selected_execution_change_restarts_only_the_affected_agent(self):
        codex, claude = self.bridge("codex"), self.bridge("claude")
        old_codex, _ = self.observe(codex)
        old_claude, _ = self.observe(claude)
        self.resource["variants"]["claude"]["command"] = self.command_for("updated-claude")
        self.save()
        new_claude, _ = self.observe(claude, 4)
        new_codex, _ = self.observe(codex, 4)
        self.assertEqual(new_claude["variant"], "updated-claude")
        self.assertNotEqual(old_claude["pid"], new_claude["pid"])
        self.assertEqual(old_codex["pid"], new_codex["pid"])

    def test_shared_env_and_cwd_edits_update_all_variants_without_erasing_specific_values(self):
        shared_cwd = self.home / "shared-work"
        shared_cwd.mkdir()
        self.resource["native"] = {"codex": {"command": sys.executable, "auth": "preserve-native-setting"}}
        self.save()
        command = self.command("mcp", "edit", "stub", "--env", "KITMUX_VARIANT_SHARED=explicit", "--cwd", str(shared_cwd))
        self.assertEqual(command.returncode, 0, command.stderr)
        saved = json.loads(self.catalog.read_text())["mcps"]["stub"]
        self.assertEqual(saved["native"], self.resource["native"])
        self.assertEqual(saved["variants"]["claude"]["env"]["KITMUX_VARIANT_AGENT"], "claude-only")
        for agent in ("codex", "claude", "opencode"):
            result, _ = self.observe(self.bridge(agent))
            self.assertEqual(result["shared"], "explicit")
            self.assertEqual(result["cwd"], str(shared_cwd.resolve()))
        self.assertNotIn("KITMUX_VARIANT_BASE", saved["variants"]["opencode"]["env"])

    def test_malformed_variants_are_rejected_without_rewriting_catalog(self):
        invalid = [None, [], {"unknown": {"transport": "stdio", "command": ["true"]}},
                   {"claude": {"command": ["true"]}}, {"claude": {"transport": "stdio"}},
                   {"claude": {"transport": "stdio", "command": "true"}},
                   {"claude": {"transport": "stdio", "command": ["true"], "mode": "managed"}},
                   {"claude": {"transport": "stdio", "command": ["true"], "env": {"BAD": 9}}},
                   {"claude": {"transport": "stdio", "command": ["true"], "cwd": 9}},
                   {"claude": {"transport": "stdio", "command": ["true"], "url": "https://example.test"}}]
        original = copy.deepcopy(self.resource)
        for variants in invalid:
            with self.subTest(variants=variants):
                self.config["mcps"]["stub"] = copy.deepcopy(original)
                self.config["mcps"]["stub"]["variants"] = variants
                self.save()
                before = self.catalog.read_bytes()
                result = self.command("status", "--json")
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.catalog.read_bytes(), before)

    def test_managed_mode_rejects_http_in_any_variant(self):
        self.resource["mode"] = "direct"
        self.resource["variants"]["opencode"] = {"transport": "http", "url": "https://example.test/mcp"}
        self.save()
        before = self.catalog.read_bytes()
        result = self.command("mcp", "edit", "stub", "--mode", "managed")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.catalog.read_bytes(), before)

    def test_imported_endpoint_edits_remain_rejected(self):
        self.resource["native"] = {"codex": {"command": sys.executable}}
        self.save()
        before = self.catalog.read_bytes()
        for options in (("--command", "/usr/bin/false"), ("--url", "https://changed.test/mcp")):
            result = self.command("mcp", "edit", "stub", *options)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(self.catalog.read_bytes(), before)

    def test_shared_endpoint_edit_cannot_silently_replace_an_imported_variant(self):
        self.resource["native"] = {"codex": {"command": sys.executable}, "claude": {"command": "different"}}
        self.save()
        before = self.catalog.read_bytes()
        command = self.command_for("base")
        result = self.command("mcp", "edit", "stub", "--command", command[0], "--", *command[1:])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.catalog.read_bytes(), before)

    def test_ai_memory_id_cannot_be_switched_to_managed_mode(self):
        self.resource["mode"] = "direct"
        self.config["mcps"] = {"ai-memory": self.resource}
        self.save()
        result = self.command("mcp", "edit", "ai-memory", "--mode", "managed")
        self.assertNotEqual(result.returncode, 0)
