import json
import os
import selectors
import socket
import subprocess
import threading
import time
import unittest

import test_runtime as runtime


class RuntimeStatusTests(unittest.TestCase):
    setUp = runtime.RuntimeTests.setUp
    save = runtime.RuntimeTests.save
    command = runtime.RuntimeTests.command

    def status(self, agent="codex"):
        result = self.command("mcp", "status", "stub", "--agent", agent, "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def connection(self, initialized=True, agent="codex"):
        harness = runtime.RuntimeTests("test_disabled_bridge_stays_alive_without_upstream")
        harness.home, harness.marker = self.home, self.marker
        self.addCleanup(harness.doCleanups)
        harness.process = subprocess.Popen(
            [runtime.BINARY, "--home", str(self.home), "bridge", "stub", "--agent", agent],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        harness.buffer = b""
        harness.addCleanup(harness.close_bridge)
        if initialized:
            harness.send({"id": 1, "method": "initialize", "params": {
                "protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "status-test", "version": "1"}}})
            self.assertIn("result", harness.reply(1))
            harness.send({"method": "notifications/initialized"})
        return harness

    def wait_state(self, expected):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            result = self.status()
            if result["state"] == expected:
                return result
            time.sleep(.01)
        self.fail(f"runtime did not reach {expected}: {result}")

    def test_waiting_and_idle_observation_does_not_spawn_upstream(self):
        harness = self.connection(initialized=False)
        observed = self.wait_state("waiting")
        row = observed["connections"][0]
        self.assertEqual(row["bridge_pid"], harness.process.pid)
        self.assertEqual(row["upstream_pid"], 0)
        self.assertEqual(row["pending"], 0)
        self.assertIsNone(row["protocol"])
        self.assertFalse(self.marker.exists())
        harness.send({"id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-11-25", "capabilities": {},
            "clientInfo": {"name": "status-test", "version": "1"}}})
        self.assertIn("result", harness.reply(1))
        observed = self.wait_state("idle")
        self.assertEqual(observed["connections"][0]["protocol"], "2025-11-25")
        self.assertEqual(observed["tool_refresh"], "unverified")
        self.assertFalse(self.marker.exists())

    def test_running_diagnostics_preserve_pid_and_hide_command_and_environment(self):
        self.config["mcps"]["stub"]["env"] = {"KITMUX_TEST_SECRET": "must-stay-private"}
        self.save()
        harness = self.connection()
        upstream_pid = int(harness.call()["result"]["content"][0]["text"])
        for _ in range(2):
            observed = self.status()
            self.assertEqual(observed["state"], "running")
            self.assertEqual(observed["reachable"], 1)
            self.assertEqual(observed["unreachable"], 0)
            self.assertEqual(observed["connections"][0]["upstream_pid"], upstream_pid)
            self.assertEqual(observed["connections"][0]["bridge_pid"], harness.process.pid)
            self.assertNotIn("must-stay-private", json.dumps(observed))
            self.assertNotIn("KITMUX_TEST_SECRET", json.dumps(observed))
            self.assertNotIn(str(runtime.FIXTURE), json.dumps(observed))
        self.assertEqual(self.marker.read_text().splitlines(), [str(upstream_pid)])

    def test_status_reports_pending_without_cancelling_or_replaying(self):
        harness = self.connection()
        harness.send({"id": "pending", "method": "tools/call", "params": {"name": "hang", "arguments": {}}})
        self.assertEqual(harness.packet()["method"], "notifications/tools/list_changed")
        observed = self.status()
        self.assertEqual(observed["state"], "running")
        self.assertEqual(observed["connections"][0]["pending"], 1)
        self.assertEqual(self.marker.with_suffix(".calls").read_text().splitlines(), ["accepted"])
        selector = selectors.DefaultSelector()
        self.addCleanup(selector.close)
        selector.register(harness.process.stdout, selectors.EVENT_READ)
        self.assertFalse(selector.select(.05), "status changed the outstanding MCP request")

    def test_multiple_connections_report_independent_upstream_pids(self):
        first, second = self.connection(), self.connection()
        first_pid = int(first.call()["result"]["content"][0]["text"])
        second_pid = int(second.call()["result"]["content"][0]["text"])
        observed = self.status()
        self.assertEqual(observed["reachable"], 2)
        self.assertEqual({row["upstream_pid"] for row in observed["connections"]}, {first_pid, second_pid})
        self.assertNotEqual(first_pid, second_pid)

    def test_disabling_or_reconnecting_one_agent_preserves_the_others(self):
        self.config["mcps"]["stub"]["agents"].update({"claude": "on", "opencode": "on"})
        self.save()
        codex, claude, opencode = [self.connection(agent=agent) for agent in ("codex", "claude", "opencode")]
        pids = [bridge.call()["result"]["content"][0]["text"] for bridge in (codex, claude, opencode)]
        self.assertEqual(len(set(pids)), 3)
        self.config["mcps"]["stub"]["agents"]["codex"] = "off"
        self.save()
        self.assertIn("error", codex.call(4))
        self.assertEqual(self.status("codex")["state"], "disabled")
        self.assertEqual(claude.call(4)["result"]["content"][0]["text"], pids[1])
        self.assertEqual(opencode.call(4)["result"]["content"][0]["text"], pids[2])
        self.config["mcps"]["stub"]["agents"]["codex"] = "on"
        self.save()
        resumed = codex.call(5)["result"]["content"][0]["text"]
        self.assertNotEqual(resumed, pids[0])
        reconnect = self.command("mcp", "reconnect", "stub", "--agent", "claude", "--json")
        self.assertEqual(reconnect.returncode, 0, reconnect.stderr)
        self.assertNotEqual(claude.call(5)["result"]["content"][0]["text"], pids[1])
        self.assertEqual(opencode.call(5)["result"]["content"][0]["text"], pids[2])
        self.assertEqual(codex.call(6)["result"]["content"][0]["text"], resumed)

    def test_disabled_connection_has_no_upstream(self):
        self.config["mcps"]["stub"]["enabled"] = False
        self.save()
        self.connection()
        observed = self.status()
        self.assertEqual(observed["state"], "disabled")
        self.assertEqual(observed["connections"][0]["upstream_pid"], 0)
        self.assertFalse(self.marker.exists())

    def stale_socket(self, path):
        endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        endpoint.bind(str(path))
        endpoint.close()
        self.addCleanup(path.unlink)

    def test_stale_socket_is_unreachable_and_never_implies_running(self):
        harness = self.connection()
        path = next((self.home / ".local/state/kitmux/r").iterdir())
        harness.close_bridge()
        self.stale_socket(path)
        observed = self.status()
        self.assertEqual(observed["state"], "unreachable")
        self.assertEqual(observed["reachable"], 0)
        self.assertEqual(observed["unreachable"], 1)
        self.assertNotIn("upstream_pid", observed["connections"][0])
        self.assertFalse(self.marker.exists())

    def test_partial_observation_preserves_reachable_connection(self):
        live = self.connection()
        upstream_pid = int(live.call()["result"]["content"][0]["text"])
        before = set((self.home / ".local/state/kitmux/r").iterdir())
        stale = self.connection()
        path = (set((self.home / ".local/state/kitmux/r").iterdir()) - before).pop()
        stale.close_bridge()
        self.stale_socket(path)
        observed = self.status()
        self.assertEqual(observed["state"], "partial")
        self.assertEqual(observed["reachable"], 1)
        self.assertEqual(observed["unreachable"], 1)
        self.assertEqual(next(row for row in observed["connections"] if row["reachable"])["upstream_pid"], upstream_pid)

    def test_direct_and_inactive_status_do_not_create_runtime_storage(self):
        observed = self.status()
        self.assertEqual(observed["state"], "inactive")
        self.assertEqual(observed["connections"], [])
        self.config["mcps"]["stub"]["mode"] = "direct"
        self.save()
        observed = self.status()
        self.assertEqual(observed["state"], "unverified")
        self.assertEqual(observed["mode"], "direct")
        self.assertFalse((self.home / ".local/state/kitmux/r").exists())

    def fake_endpoint(self, payload=None, path=None):
        if path is None:
            harness = self.connection()
            path = next((self.home / ".local/state/kitmux/r").iterdir())
            harness.close_bridge()
        endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        endpoint.bind(str(path))
        endpoint.listen(8)
        endpoint.settimeout(.1)
        done = threading.Event()
        def serve():
            while not done.is_set():
                try:
                    peer, _ = endpoint.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                with peer:
                    if payload is None:
                        continue
                    peer.settimeout(1)
                    try:
                        peer.recv(32)
                        if callable(payload):
                            payload(peer)
                        else:
                            peer.sendall(json.dumps(payload).encode() + b"\n")
                    except OSError:
                        pass
        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        def cleanup():
            done.set()
            endpoint.close()
            worker.join(timeout=2)
            path.unlink(missing_ok=True)
        self.addCleanup(cleanup)

    def test_budget_exhaustion_marks_unqueried_healthy_peer_as_incomplete(self):
        harness = self.connection()
        directory = self.home / ".local/state/kitmux/r"
        base = next(directory.iterdir())
        harness.close_bridge()
        blocked, requests = set(), []
        released = threading.Event()
        reply = {"control_version": 1, "id": "stub", "agent": "codex",
                 "bridge_pid": os.getpid(), "upstream_pid": 0, "state": "idle", "pending": 0}
        def responder(path):
            def respond(peer):
                requests.append(path)
                if path in blocked:
                    try:
                        peer.recv(1)  # Wait for the querying client to exhaust its deadline.
                    finally:
                        released.set()
                else:
                    peer.sendall(json.dumps(reply).encode() + b"\n")
            return respond
        for suffix in (".first", ".second"):
            path = base.with_name(base.name + suffix)
            self.fake_endpoint(responder(path), path=path)
        # Both directory scans see the same unchanged entries; block the first
        # entry so the otherwise healthy later peer is outside the total budget.
        blocked.add(next(directory.iterdir()))
        start = time.monotonic()
        observed = self.status()
        elapsed = time.monotonic() - start
        self.assertGreater(elapsed, .35)
        self.assertLess(elapsed, 2, "blocked peer escaped the bounded control-query deadline")
        self.assertFalse(observed["scan_complete"])
        self.assertEqual(observed["reachable"], 0)
        self.assertEqual(observed["unreachable"], 1)
        self.assertEqual(len(observed["connections"]), 1)
        self.assertEqual(len(requests), 1)
        self.assertTrue(released.wait(1))
        blocked.clear()
        observed = self.status()
        self.assertTrue(observed["scan_complete"])
        self.assertEqual(observed["reachable"], 2)

    def test_closed_peer_does_not_terminate_status_with_sigpipe(self):
        self.fake_endpoint()
        for _ in range(8):
            observed = self.status()
            self.assertEqual(observed["state"], "unreachable")
            self.assertEqual(observed["reachable"], 0)

    def test_malformed_numeric_fields_cannot_leak_objects_or_secrets(self):
        self.fake_endpoint({"control_version": 1, "id": "stub", "agent": "codex",
                            "bridge_pid": os.getpid(), "upstream_pid": 0, "state": "idle",
                            "pending": {"secret": "never-return-this"}})
        observed = self.status()
        self.assertEqual(observed["state"], "unreachable")
        self.assertNotIn("never-return-this", json.dumps(observed))
        self.assertNotIn("pending", observed["connections"][0])

    def test_unknown_response_fields_are_not_returned_as_diagnostics(self):
        self.fake_endpoint({"control_version": 1, "id": "stub", "agent": "codex",
                            "bridge_pid": os.getpid(), "upstream_pid": 0, "state": "idle",
                            "pending": 0, "env": {"TOKEN": "private-value"},
                            "protocol": {"secret": "also-private"}})
        observed = self.status()
        self.assertEqual(observed["state"], "idle")
        self.assertIsNone(observed["connections"][0]["protocol"])
        self.assertNotIn("private", json.dumps(observed))
