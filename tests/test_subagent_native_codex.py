"""Native Codex profile discovery through a synthetic local Responses endpoint."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import shutil
import threading
import unittest

from native_clients.common import JsonLinesProcess, Sandbox


class ResponsesFixture:
    def __init__(self):
        self.requests = []
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                fixture.requests.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
                events = [
                    {"type": "response.created", "response": {"id": "resp_fixture"}},
                    {"type": "response.output_item.done", "output_index": 0, "item": {
                        "id": "msg_fixture", "type": "message", "role": "assistant", "status": "completed",
                        "content": [{"type": "output_text", "text": "fixture-complete", "annotations": []}]}},
                    {"type": "response.completed", "response": {"id": "resp_fixture", "status": "completed",
                        "output": [], "usage": {"input_tokens": 10, "output_tokens": 2, "total_tokens": 12}}},
                ]
                body = "".join("event: " + event["type"] + "\ndata: " + json.dumps(event) + "\n\n"
                               for event in events).encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)


@unittest.skipUnless(os.environ.get("KITMUX_NATIVE_TESTS") == "1" and shutil.which("codex"),
                     "opt-in native Codex subagent qualification")
class CodexSubagentNativeTests(unittest.TestCase):
    def test_profile_discovery_disable_and_reenable_after_reload(self):
        sandbox = Sandbox("codex")
        self.addCleanup(sandbox.close)
        sandbox.catalog_path.write_text(json.dumps({"version": 1, "revision": 0,
                                                   "groups": {}, "mcps": {}, "skills": {}}))
        source = sandbox.home / "review.md"
        source.write_text("Review code changes without editing files.\n")
        marker = "kitmux-native-discovery-marker-0123"
        sandbox.kitmux("subagent", "add", "kitmux-probe", "--from", str(source), "--description", marker)
        sandbox.kitmux("subagent", "enable", "kitmux-probe", "--agent", "codex")
        sandbox.kitmux("sync", "--apply", "--agent", "codex")
        provider = ResponsesFixture()
        self.addCleanup(provider.close)
        sandbox.env["KITMUX_FIXTURE_KEY"] = "synthetic-fixture-key"
        config = {
            "model_provider": "fixture", "model_providers.fixture.name": "Fixture",
            "model_providers.fixture.base_url": f"http://127.0.0.1:{provider.server.server_port}/v1",
            "model_providers.fixture.env_key": "KITMUX_FIXTURE_KEY",
            "model_providers.fixture.wire_api": "responses",
            "model_providers.fixture.requires_openai_auth": False,
            "model_providers.fixture.supports_websockets": False, "agents.enabled": True,
        }
        args = ["app-server", "--stdio"]
        for key, value in config.items():
            args += ["-c", key + "=" + json.dumps(value)]

        def discovered():
            before = len(provider.requests)
            client = JsonLinesProcess(sandbox, "codex", args)
            try:
                self.assertIn("result", client.request("initialize", {
                    "clientInfo": {"name": "kitmux-subagent-qualification", "version": "1"},
                    "capabilities": {"experimentalApi": True}}))
                client.send({"method": "initialized"})
                started = client.request("thread/start", {"cwd": str(sandbox.home), "ephemeral": True,
                    "approvalPolicy": "never", "sandbox": "read-only", "model": "gpt-6.1-sol",
                    "modelProvider": "fixture"})
                self.assertIn("result", started, started)
                thread = started["result"]["thread"]["id"]
                self.assertIn("result", client.request("turn/start", {"threadId": thread,
                    "input": [{"type": "text", "text": "Reply fixture-complete."}]}))
                for _ in range(100):
                    packet = client.packet(timeout=15)
                    if packet.get("method") == "turn/completed":
                        self.assertEqual(packet["params"]["turn"]["status"], "completed", packet)
                        break
                else:
                    self.fail("Codex did not complete the synthetic fixture turn")
                self.assertEqual(len(provider.requests), before + 1)
                return marker in json.dumps(provider.requests[-1]["input"])
            finally:
                client.close()

        self.assertTrue(discovered(), "Codex did not expose the enabled custom profile")
        sandbox.kitmux("subagent", "disable", "kitmux-probe", "--agent", "codex")
        self.assertFalse(discovered(), "Codex still exposes the disabled custom profile")
        sandbox.kitmux("subagent", "enable", "kitmux-probe", "--agent", "codex")
        self.assertTrue(discovered(), "Codex did not rediscover the reenabled custom profile")
