import json
import os
import shutil
import time
import unittest

from native_clients.common import JsonLinesProcess, Sandbox
from native_clients.provider import LocalProvider
from native_clients.opencode import OpenCodeServer

ENABLED = os.environ.get("KITMUX_NATIVE_TESTS") == "1"


@unittest.skipUnless(ENABLED and shutil.which("codex"), "opt-in native Codex qualification")
class CodexNativeTests(unittest.TestCase):
    def test_native_discovery_calls_toggles_and_reconnect(self):
        sandbox = Sandbox("codex")
        self.addCleanup(sandbox.close)
        sandbox.kitmux("sync", "--apply", "--agent", "codex")
        client = JsonLinesProcess(sandbox, "codex", ["app-server", "--stdio"])
        self.addCleanup(client.close)
        response = client.request("initialize", {"clientInfo":{"name":"kitmux-qualification", "version":"1"},
                                                   "capabilities":{"experimentalApi":True}})
        self.assertIn("result", response, response)
        client.send({"method":"initialized"})
        response = client.request("thread/start", {"cwd":str(sandbox.home), "ephemeral":True,
                                                    "approvalPolicy":"never", "sandbox":"danger-full-access"})
        self.assertIn("result", response, response)
        thread = response["result"]["thread"]["id"]
        listed = client.request("mcpServerStatus/list", {"threadId":thread,"serverName":"stub", "detail":"toolsAndAuthOnly"})
        self.assertIn("identity", json.dumps(listed), listed)
        metadata = {"qualification":{"client":"codex"}, "opaque":"preserve"}

        def call():
            return client.request("mcpServer/tool/call", {"threadId":thread,"server":"stub", "tool":"identity",
                                                          "arguments":{}, "_meta":metadata})

        first = call()
        self.assertIn("result", first, first)
        for key, value in metadata.items():
            self.assertEqual(first["result"]["_meta"][key], value)
        self.assertEqual(first["result"]["_meta"]["threadId"], thread)
        sandbox.kitmux("mcp", "disable", "stub", "--agent", "codex")
        stale = call()
        self.assertTrue("error" in stale or stale.get("result", {}).get("isError"), stale)
        sandbox.kitmux("mcp", "enable", "stub", "--agent", "codex")
        second = call()
        self.assertIn("result", second, second)
        sandbox.kitmux("mcp", "reconnect", "stub", "--agent", "codex", "--json")
        third = call()
        self.assertIn("result", third, third)
        self.assertNotEqual(second["result"]["content"][0]["text"], third["result"]["content"][0]["text"])
        for key, value in metadata.items():
            self.assertEqual(third["result"]["_meta"][key], value)
        self.assertEqual(third["result"]["_meta"]["threadId"], thread)


@unittest.skipUnless(ENABLED and shutil.which("claude"), "opt-in native Claude Code qualification")
class ClaudeNativeTests(unittest.TestCase):
    def test_native_tool_use_toggles_and_reconnect_with_local_provider(self):
        sandbox = Sandbox("claude")
        self.addCleanup(sandbox.close)
        provider = LocalProvider()
        self.addCleanup(provider.close)
        sandbox.env.update({"ANTHROPIC_BASE_URL":provider.url, "ANTHROPIC_API_KEY":"synthetic-fixture-key",
                            "ENABLE_TOOL_SEARCH":"false"})
        sandbox.kitmux("sync", "--apply", "--agent", "claude")
        client = JsonLinesProcess(sandbox, "claude", ["--bare", "--print", "--input-format", "stream-json",
            "--output-format", "stream-json", "--verbose", "--strict-mcp-config", "--mcp-config",
            str(sandbox.home / ".claude.json"), "--model", "claude-sonnet-4-5", "--allowedTools",
            "mcp__stub__identity", "--tools", "", "--no-session-persistence", "--max-turns", "20"])
        self.addCleanup(client.close)

        def turn(text):
            client.send({"type":"user", "message":{"role":"user", "content":text}})
            packets = []
            for _ in range(100):
                packet = client.packet()
                packets.append(packet)
                if packet.get("type") == "result":
                    self.assertFalse(packet.get("is_error"), (packet, client.errors()))
                    return packets
            self.fail("Claude did not complete the fixture turn")

        first = turn("Call stub identity for the enabled phase.")
        self.assertEqual(len(sandbox.calls()), 1, (first, provider.requests))
        sandbox.kitmux("mcp", "disable", "stub", "--agent", "claude")
        stale = turn("Call stub identity again using the cached tool name.")
        self.assertEqual(len(sandbox.calls()), 1, stale)
        self.assertTrue(any(item.get("is_error") for packet in stale for item in
                            packet.get("message", {}).get("content", []) if isinstance(item, dict)), stale)
        sandbox.kitmux("mcp", "enable", "stub", "--agent", "claude")
        deadline = time.monotonic()+5
        while time.monotonic()<deadline:
            client.send({"type":"control_request", "request_id":"qualification-status", "request":{"subtype":"mcp_status"}})
            for _ in range(50):
                status_packet = client.packet()
                if status_packet.get("type") == "control_response":
                    break
            tools = status_packet.get("response", {}).get("response", {}).get("mcpServers", [{}])[0].get("tools", [])
            if any(tool.get("name") == "identity" for tool in tools):
                break
            time.sleep(.05)
        self.assertTrue(any(tool.get("name") == "identity" for tool in tools), status_packet)
        enabled_packets = turn("Call stub identity after re-enabling it.")
        self.assertEqual(len(sandbox.calls()), 2, (status_packet, enabled_packets, client.errors(),
            [{"path":r["path"], "tools":[t.get("name") for t in r["body"].get("tools",[])]} for r in provider.requests]))
        before = sandbox.calls()[-1]
        sandbox.kitmux("mcp", "reconnect", "stub", "--agent", "claude", "--json")
        turn("Call stub identity after reconnecting it.")
        after = sandbox.calls()[-1]
        self.assertEqual(len(sandbox.calls()), 3)
        self.assertNotEqual(before["pid"], after["pid"])


@unittest.skipUnless(ENABLED and shutil.which("opencode"), "opt-in native OpenCode qualification")
class OpenCodeNativeTests(unittest.TestCase):
    def test_native_tool_use_metadata_toggles_and_reconnect_with_local_provider(self):
        sandbox = Sandbox("opencode")
        self.addCleanup(sandbox.close)
        provider = LocalProvider()
        provider.use_code_mode = True
        provider.code_path = "tools.stub.identity"
        self.addCleanup(provider.close)
        sandbox.env["KITMUX_FIXTURE_KEY"] = "synthetic-fixture-key"
        sandbox.kitmux("sync", "--apply", "--agent", "opencode")
        path = sandbox.home / ".config/opencode/opencode.json"
        config = json.loads(path.read_text())
        config.update({"model":"fixture/fixture", "providers":{"fixture":{"name":"Kitmux protocol fixture",
            "env":["KITMUX_FIXTURE_KEY"], "package":"@opencode/ai/providers/openai-compatible",
            "settings":{"baseURL":provider.url+"/v1"}, "models":{"fixture":{"name":"Fixture"}}}},
            "permissions":[{"action":"*", "resource":"*", "effect":"allow"}]})
        path.write_text(json.dumps(config))
        server = OpenCodeServer(sandbox)
        self.addCleanup(server.close)
        deadline = time.monotonic()+15
        while time.monotonic()<deadline:
            native_status = server.request("GET", "/api/mcp")
            stub = next((row for row in native_status["data"] if row["name"] == "stub"), None)
            if stub and stub["status"]["status"] in ("connected", "failed"):
                break
            time.sleep(.05)
        self.assertEqual(stub["status"]["status"], "connected", (native_status,server.log_path.read_text()[-2000:]))
        deadline = time.monotonic()+5
        while time.monotonic()<deadline:
            packets = [json.loads(line)["packet"] for line in sandbox.marker.read_text().splitlines()] if sandbox.marker.exists() else []
            if any(packet.get("method") == "tools/list" for packet in packets):
                break
            time.sleep(.05)
        self.assertTrue(any(packet.get("method") == "tools/list" for packet in packets), native_status)
        session = server.request("POST", "/api/session", {"location":{"directory":str(sandbox.home)},
            "agent":"build",
            "model":{"providerID":"fixture", "id":"fixture"},
            "permissions":[{"action":"*", "resource":"*", "effect":"allow"}]})["data"]["id"]

        def turn(text):
            return server.prompt(session, text)

        def identity_turn(text, expected):
            deadline = time.monotonic()+8
            while time.monotonic()<deadline:
                turn(text)
                if len(sandbox.calls()) >= expected:
                    return
                # OpenCode's connected state precedes its model tool-registry
                # update. Retrying this fixture read is safe and bounded.
                time.sleep(.05)
            self.fail("OpenCode did not publish the fixture tool after connecting")

        identity_turn("Call stub identity for the enabled phase.", 1)
        calls = sandbox.calls()
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["packet"]["params"]["_meta"]["ai.opencode/sessionID"], session)
        sandbox.kitmux("mcp", "disable", "stub", "--agent", "opencode")
        turn("Call stub identity again using the cached tool name.")
        self.assertEqual(len(sandbox.calls()), 1)
        sandbox.kitmux("mcp", "enable", "stub", "--agent", "opencode")
        identity_turn("Call stub identity after re-enabling it.", 2)
        before = sandbox.calls()[-1]
        sandbox.kitmux("mcp", "reconnect", "stub", "--agent", "opencode", "--json")
        identity_turn("Call stub identity after reconnecting it.", 3)
        after = sandbox.calls()[-1]
        self.assertEqual(len(sandbox.calls()), 3)
        self.assertNotEqual(before["pid"], after["pid"])
        self.assertEqual(after["packet"]["params"]["_meta"]["ai.opencode/sessionID"], session)


if __name__ == "__main__":
    unittest.main()
