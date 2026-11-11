import json
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))
FIXTURE = Path(__file__).parent / "fixtures/runtime_stub.py"


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="km-", dir="/tmp")
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.marker = self.home / "upstream-starts"
        self.catalog = self.home / ".config/kitmux/catalog.json"
        self.catalog.parent.mkdir(parents=True)
        self.config = {"version": 1, "revision": 1, "groups": {}, "skills": {}, "mcps": {
            "stub": {"enabled": True, "agents": {"codex": "on"}, "transport": "stdio",
                     "mode": "managed", "command": [sys.executable, str(FIXTURE), str(self.marker)]}}}
        self.save()

    def save(self):
        tmp = self.catalog.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.config))
        tmp.replace(self.catalog)

    def command(self, *args):
        return subprocess.run([BINARY, "--home", str(self.home), *args],
                              text=True, capture_output=True, timeout=10)

    def bridge(self, capabilities=None):
        self.process = subprocess.Popen([BINARY, "--home", str(self.home), "bridge", "stub",
                                         "--agent", "codex"], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buffer = b""
        self.addCleanup(self.close_bridge)
        self.send({"id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-11-25", "capabilities": capabilities or {},
            "clientInfo": {"name": "runtime-test", "version": "1"}}})
        result = self.reply(1)
        self.assertIn("result", result, result)
        self.send({"method": "notifications/initialized"})

    def close_bridge(self):
        p = getattr(self, "process", None)
        if not p:
            return
        if p.poll() is None:
            p.stdin.close()
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(timeout=3)
        for stream in (p.stdin, p.stdout, p.stderr):
            if not stream.closed:
                stream.close()

    def send(self, message):
        message = {"jsonrpc": "2.0", **message}
        self.process.stdin.write(json.dumps(message).encode() + b"\n")
        self.process.stdin.flush()

    def packet(self, timeout=6):
        deadline = time.monotonic() + timeout
        selector = selectors.DefaultSelector()
        selector.register(self.process.stdout, selectors.EVENT_READ)
        try:
            while b"\n" not in self.buffer:
                self.assertGreater(deadline - time.monotonic(), 0, "MCP response timed out")
                events = selector.select(max(0, deadline - time.monotonic()))
                self.assertTrue(events, "MCP response timed out")
                chunk = os.read(self.process.stdout.fileno(), 65536)
                self.assertTrue(chunk, "bridge exited before responding")
                self.buffer += chunk
            line, self.buffer = self.buffer.split(b"\n", 1)
            return json.loads(line)
        finally:
            selector.close()

    def reply(self, request_id):
        for _ in range(20):
            response = self.packet()
            if response.get("id") == request_id:
                return response
        self.fail("response missing among notifications")

    def call(self, request_id=3, name="identity", meta=None):
        self.send({"id": request_id, "method": "tools/call", "params": {
            "name": name, "arguments": {}, "_meta": meta or {}}})
        return self.reply(request_id)

    def test_disabled_bridge_stays_alive_without_upstream(self):
        self.config["mcps"]["stub"]["enabled"] = False
        self.save()
        self.bridge()
        self.send({"id": 2, "method": "tools/list"})
        self.assertEqual(self.reply(2)["result"]["tools"], [])
        self.assertIn("error", self.call())
        self.assertIsNone(self.process.poll())
        self.assertFalse(self.marker.exists())

    def test_discovery_call_metadata_and_reconnect(self):
        self.bridge()
        self.assertFalse(self.marker.exists(), "initialize should not launch upstream")
        self.send({"id": 2, "method": "tools/list"})
        self.assertEqual(self.reply(2)["result"]["tools"][0]["name"], "identity")
        meta = {"ai.opencode/sessionID": "session-one", "custom": {"key": 4}}
        first = self.call(meta=meta)["result"]
        self.assertEqual(first["_meta"], meta)
        p = self.command("mcp", "reconnect", "stub", "--agent", "codex", "--json")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(json.loads(p.stdout)["status"], "reconnected")
        second = self.call(4)["result"]
        self.assertNotEqual(first["content"][0]["text"], second["content"][0]["text"])
        self.assertEqual(len(self.marker.read_text().splitlines()), 2)

    def test_policy_disable_rejects_cached_call_and_reenable(self):
        self.bridge()
        self.assertIn("result", self.call())
        self.config["mcps"]["stub"]["enabled"] = False
        self.save()
        self.assertIn("error", self.call(4))
        self.config["mcps"]["stub"]["enabled"] = True
        self.save()
        self.send({"id": 5, "method": "tools/list"})
        self.assertEqual(len(self.reply(5)["result"]["tools"]), 1)

    def test_other_agent_override_does_not_restart_this_connection(self):
        self.bridge()
        first = self.call()["result"]["content"][0]["text"]
        self.config["mcps"]["stub"]["agents"]["claude"] = "on"
        self.config["mcps"]["stub"]["description"] = "Updated catalog notes"
        self.save()
        second = self.call(4)["result"]["content"][0]["text"]
        self.assertEqual(first, second)
        self.assertEqual(len(self.marker.read_text().splitlines()), 1)

    def test_upstream_does_not_receive_unsupported_client_capabilities(self):
        self.bridge({"roots": {"listChanged": True}, "sampling": {}, "elicitation": {}})
        result = self.call(name="client-capabilities")["result"]
        self.assertEqual(json.loads(result["content"][0]["text"]), {})

    def test_upstream_exit_fails_pending_call_without_replay(self):
        self.bridge()
        response = self.call(name="exit")
        self.assertIn("error", response)
        self.assertIn("not replayed", response["error"]["message"])
        self.assertEqual(len(self.marker.read_text().splitlines()), 1)

    def test_reconnect_fails_inflight_call_without_replaying(self):
        self.bridge()
        self.send({"id": "pending", "method": "tools/call", "params": {"name": "hang", "arguments": {}}})
        self.assertEqual(self.packet()["method"], "notifications/tools/list_changed")
        p = self.command("mcp", "reconnect", "stub", "--agent", "codex", "--json")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("not replayed", self.reply("pending")["error"]["message"])
        self.assertEqual(self.marker.with_suffix(".calls").read_text().splitlines(), ["accepted"])

    def test_shutdown_kills_term_resistant_owned_descendant(self):
        self.bridge()
        child = int(self.call(name="descendant")["result"]["content"][0]["text"])
        def cleanup():
            try:
                os.kill(child, 9)
            except ProcessLookupError:
                pass
        self.addCleanup(cleanup)
        self.close_bridge()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                os.kill(child, 0)
            except ProcessLookupError:
                return
            time.sleep(.01)
        self.fail("owned descendant survived bridge shutdown")

    def test_direct_reconnect_returns_action_required(self):
        self.config["mcps"]["stub"]["mode"] = "direct"
        self.save()
        p = self.command("mcp", "reconnect", "stub", "--agent", "codex", "--json")
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(json.loads(p.stdout)["status"], "action_required")

    def test_disabled_managed_reconnect_does_not_start_a_process(self):
        self.config["mcps"]["stub"]["enabled"] = False
        self.save()
        p = self.command("mcp", "reconnect", "stub", "--agent", "codex", "--json")
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(json.loads(p.stdout)["status"], "disabled")
        self.assertFalse(self.marker.exists())

    def test_legacy_initialize_negotiates_a_supported_version(self):
        self.process = subprocess.Popen([BINARY, "--home", str(self.home), "bridge", "stub",
                                         "--agent", "codex"], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buffer = b""
        self.addCleanup(self.close_bridge)
        self.send({"id": 1, "method": "initialize", "params": {
            "protocolVersion": "2026-07-28", "clientInfo": {"name": "test", "version": "1"}}})
        response = self.reply(1)
        self.assertEqual(response["result"]["protocolVersion"], "2025-11-25")
        self.assertFalse(self.marker.exists())
        self.send({"method": "notifications/initialized"})
        result = self.call(name="protocol-version")["result"]
        self.assertEqual(result["content"][0]["text"], "2025-11-25")

    def test_preinit_modern_probe_allows_legacy_fallback(self):
        self.process = subprocess.Popen([BINARY, "--home", str(self.home), "bridge", "stub",
                                         "--agent", "codex"], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buffer = b""
        self.addCleanup(self.close_bridge)
        self.send({"id": "probe", "method": "server/discover", "params": {
            "_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"}}})
        self.assertEqual(self.reply("probe")["error"]["code"], -32601)
        self.assertFalse(self.marker.exists())
        self.send({"id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "fallback-test", "version": "1"}}})
        self.assertEqual(self.reply(1)["result"]["protocolVersion"], "2025-06-18")
        self.send({"method": "notifications/initialized"})
        self.send({"id": 2, "method": "tools/list"})
        self.assertEqual(self.reply(2)["result"]["tools"][0]["name"], "identity")

    def test_stateless_modern_request_is_not_forwarded(self):
        self.process = subprocess.Popen([BINARY, "--home", str(self.home), "bridge", "stub",
                                         "--agent", "codex"], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buffer = b""
        self.addCleanup(self.close_bridge)
        self.send({"id": 1, "method": "tools/list", "params": {
            "_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"}}})
        self.assertEqual(self.reply(1)["error"]["code"], -32002)
        self.assertFalse(self.marker.exists())


if __name__ == "__main__":
    unittest.main()
