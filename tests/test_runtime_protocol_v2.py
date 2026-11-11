"""Tools-only legacy bridge contracts, using no live MCP or agent clients."""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

import test_runtime as runtime_base

SERVER = r'''
import json, os, pathlib, sys
marker = pathlib.Path(sys.argv[1])
with marker.open("a") as stream: stream.write(str(os.getpid()) + "\n")
held = {}
cancelled = []
version = None
waiting_ping = None
def send(packet):
    print(json.dumps({"jsonrpc": "2.0", **packet}), flush=True)
def reply(request, result):
    send({"id": request["id"], "result": result})
for line in sys.stdin:
    request = json.loads(line)
    method = request.get("method")
    if method == "initialize":
        version = os.environ.get("KM_TEST_REVISION", request["params"]["protocolVersion"])
        reply(request, {"protocolVersion": version,
              "capabilities": json.loads(os.environ.get("KM_TEST_CAPS", '{"tools":{}}')),
              "serverInfo": {"name": "protocol-fixture", "version": "1"}})
    elif method == "tools/list":
        reply(request, {"tools": [{"name": "echo", "inputSchema": {"type":"object"}}]})
    elif method == "tools/call":
        params = request.get("params", {})
        name = params.get("name") if isinstance(params, dict) else None
        if name == "hold":
            held[json.dumps(request["id"])] = request
            send({"method":"notifications/tools/list_changed"})
        elif name == "progress":
            token = params.get("_meta", {}).get("progressToken")
            send({"method":"notifications/progress", "params":{"progressToken":"unsolicited", "progress":1}})
            send({"method":"notifications/progress", "params":{"progressToken":token, "progress":1}})
            send({"method":"notifications/progress", "params":{"progressToken":token, "progress":1}})
            send({"method":"notifications/progress", "params":{"progressToken":token, "progress":2, "total":4, "message":"Working"}})
            reply(request, {"content":[], "_meta":params.get("_meta", {})})
        elif name == "cancel-events":
            reply(request, {"content":[{"type":"text", "text":json.dumps(cancelled)}]})
        elif name == "bad-error":
            send({"id":request["id"], "error":"not-an-error-object"})
        elif name == "server-ping":
            waiting_ping = request
            send({"id":"server-ping", "method":"ping"})
        else:
            reply(request, {"content":[{"type":"text", "text":version}], "_meta":params.get("_meta", {}) if isinstance(params, dict) else {}})
    elif method == "notifications/cancelled":
        target = request.get("params", {}).get("requestId")
        cancelled.append(target)
        pending = held.pop(json.dumps(target), None)
        if pending:
            token = pending.get("params", {}).get("_meta", {}).get("progressToken")
            send({"method":"notifications/progress", "params":{"progressToken":token, "progress":1}})
            reply(pending, {"content":[]})
    elif request.get("id") == "server-ping" and waiting_ping:
        reply(waiting_ping, {"content":[{"type":"text", "text":"pong" if request.get("result") == {} else "ping-rejected"}]})
        waiting_ping = None
'''


class ProtocolV2Tests(unittest.TestCase):
    save = runtime_base.RuntimeTests.save
    command = runtime_base.RuntimeTests.command
    close_bridge = runtime_base.RuntimeTests.close_bridge
    send = runtime_base.RuntimeTests.send
    packet = runtime_base.RuntimeTests.packet
    reply = runtime_base.RuntimeTests.reply
    call = runtime_base.RuntimeTests.call

    def setUp(self):
        runtime_base.RuntimeTests.setUp(self)
        script = self.home / "server.py"
        script.write_text(SERVER)
        self.config["mcps"]["stub"]["command"] = [sys.executable, str(script), str(self.marker)]
        self.save()

    def bridge(self):
        self.process = subprocess.Popen([runtime_base.BINARY, "--home", str(self.home),
            "bridge", "stub", "--agent", "codex"], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buffer = b""
        self.addCleanup(self.close_bridge)
        self.send({"id":1, "method":"initialize", "params": {
            "protocolVersion":"2025-11-25", "capabilities":{},
            "clientInfo":{"name":"protocol-test", "version":"1"}}})
        self.assertIn("result", self.reply(1))
        self.send({"method":"notifications/initialized"})

    def test_upstream_can_select_another_supported_legacy_revision(self):
        self.config["mcps"]["stub"]["env"] = {"KM_TEST_REVISION":"2025-03-26"}
        self.save()
        self.bridge()
        response = self.call()
        self.assertIn("result", response, response)
        self.assertEqual(response["result"]["content"][0]["text"], "2025-03-26")

    def test_unknown_upstream_revision_is_rejected(self):
        self.config["mcps"]["stub"]["env"] = {"KM_TEST_REVISION":"2026-07-28"}
        self.save()
        self.bridge()
        self.assertIn("error", self.call())

    def test_malformed_and_unsupported_capabilities_are_rejected(self):
        self.config["mcps"]["stub"]["env"] = {"KM_TEST_CAPS":'{"tools":true}'}
        self.save()
        self.bridge()
        self.assertIn("error", self.call())

    def test_unsupported_server_capability_stays_direct_only(self):
        self.config["mcps"]["stub"]["env"] = {"KM_TEST_CAPS":'{"tools":{},"resources":{}}'}
        self.save()
        self.bridge()
        self.assertIn("error", self.call())

    def test_progress_only_for_active_token_and_increases(self):
        self.bridge()
        meta = {"progressToken":7, "ai.opencode/sessionID":"session-preserved"}
        self.send({"id":"work", "method":"tools/call", "params": {
            "name":"progress", "arguments":{}, "_meta":meta}})
        first, second, result = self.packet(), self.packet(), self.packet()
        self.assertEqual(first.get("method"), "notifications/progress", first)
        self.assertEqual(first["params"]["progress"], 1)
        self.assertEqual(first["params"]["progressToken"], 7)
        self.assertEqual(second["params"]["progress"], 2)
        self.assertEqual(result["id"], "work")
        self.assertEqual(result["result"]["_meta"], meta)

    def test_duplicate_active_progress_token_is_not_forwarded(self):
        self.bridge()
        self.send({"id":"held", "method":"tools/call", "params": {
            "name":"hold", "arguments":{}, "_meta":{"progressToken":"same"}}})
        self.assertEqual(self.packet()["method"], "notifications/tools/list_changed")
        response = self.call(4, meta={"progressToken":"same"})
        self.assertIn("error", response)
        self.assertIn("progress", response["error"]["message"].lower())

    def test_cancellation_is_scoped_and_suppresses_late_completion(self):
        self.bridge()
        self.send({"id":"held", "method":"tools/call", "params": {
            "name":"hold", "arguments":{}, "_meta":{"progressToken":"progress-held"}}})
        self.assertEqual(self.packet()["method"], "notifications/tools/list_changed")
        self.send({"method":"notifications/cancelled", "params":{"requestId":"kitmux/initialize"}})
        self.send({"method":"notifications/cancelled", "params":{"requestId":"unknown"}})
        self.send({"method":"notifications/cancelled", "params":{"requestId":"held"}})
        self.send({"id":4, "method":"tools/call", "params":{"name":"cancel-events", "arguments":{}}})
        response = self.packet()
        self.assertEqual(response.get("id"), 4, response)
        self.assertEqual(json.loads(response["result"]["content"][0]["text"]), ["held"])

    def test_invalid_call_params_are_rejected_before_starting_upstream(self):
        self.bridge()
        self.send({"id":3, "method":"tools/call", "params":{"name":"echo", "arguments":[]}})
        self.assertEqual(self.reply(3)["error"]["code"], -32602)
        self.assertFalse(self.marker.exists())

    def test_malformed_upstream_error_is_not_forwarded(self):
        self.bridge()
        response = self.call(name="bad-error")
        self.assertIsInstance(response.get("error"), dict, response)
        self.assertIn("not replayed", response["error"]["message"])

    def test_null_request_id_is_not_forwarded(self):
        self.bridge()
        self.send({"id":None, "method":"tools/list"})
        response = self.packet()
        self.assertIn("error", response, response)
        self.assertFalse(self.marker.exists())

    def test_server_ping_works_without_optional_callback_capabilities(self):
        self.bridge()
        response = self.call(name="server-ping")
        self.assertEqual(response["result"]["content"][0]["text"], "pong")

    def test_numeric_and_string_request_ids_remain_distinct(self):
        self.bridge()
        self.send({"id":7, "method":"tools/call", "params":{"name":"hold", "arguments":{}}})
        self.assertEqual(self.packet()["method"], "notifications/tools/list_changed")
        self.assertIn("result", self.call("7"))

    def test_explicit_null_progress_token_is_rejected(self):
        self.bridge()
        self.assertEqual(self.call(meta={"progressToken":None})["error"]["code"], -32602)
        self.assertFalse(self.marker.exists())


if __name__ == "__main__":
    unittest.main()
