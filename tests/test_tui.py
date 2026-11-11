import errno
import fcntl
import json
import os
from pathlib import Path
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class TerminalSession:
    def __init__(self, home, term="xterm-256color"):
        self.master, self.slave = os.openpty()
        self.original = termios.tcgetattr(self.slave)
        self.resize(24, 80)
        env = dict(os.environ, TERM=term, NO_COLOR="1")
        self.process = subprocess.Popen(
            [BINARY, "--home", str(home)], stdin=self.slave, stdout=self.slave,
            stderr=self.slave, env=env, close_fds=True)
        self.output = b""

    def resize(self, rows, columns):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))

    def send(self, value):
        os.write(self.master, value)

    def wait_exit(self, timeout=5):
        deadline = time.monotonic() + timeout
        while self.process.poll() is None:
            if time.monotonic() >= deadline:
                raise AssertionError(f"terminal did not exit: {self.output[-1000:]!r}")
            ready, _, _ = select.select([self.master], [], [], 0.05)
            if ready:
                try:
                    self.output += os.read(self.master, 65536)
                except OSError as exc:
                    if exc.errno != errno.EIO:
                        raise
        return self.process.returncode

    def read_for(self, duration):
        start = len(self.output)
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.master], [], [], max(0, deadline - time.monotonic()))
            if not ready:
                break
            data = os.read(self.master, 65536)
            if not data:
                break
            self.output += data
        return self.output[start:]

    def wait_text(self, text, timeout=5):
        deadline = time.monotonic() + timeout
        start = len(self.output)
        while time.monotonic() < deadline:
            if text.encode() in self.output[start:]:
                return
            ready, _, _ = select.select([self.master], [], [], max(0, deadline - time.monotonic()))
            if not ready:
                break
            try:
                data = os.read(self.master, 65536)
            except OSError as exc:
                if exc.errno == errno.EIO:
                    break
                raise
            self.output += data
            if text.encode() in self.output[start:]:
                return
            if not data or self.process.poll() is not None:
                break
        raise AssertionError(f"terminal did not show {text!r}: {self.output[-3000:]!r}")

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        os.close(self.master)
        os.close(self.slave)


class TerminalTestCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="km-ui-", dir="/tmp")
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)

    def start(self, term="xterm-256color"):
        terminal = TerminalSession(self.home, term)
        self.addCleanup(terminal.close)
        return terminal

    def run_cli(self, *args):
        result = subprocess.run([BINARY, "--home", str(self.home), *args],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def finish_mcp_form(self, terminal):
        terminal.wait_text("Executable path or command")
        terminal.send(b"/usr/bin/true\n")
        terminal.wait_text("Arguments (quotes supported")
        terminal.send(b"\n")
        terminal.wait_text("Saved")


class TerminalTests(TerminalTestCase):
    def test_choose_managed_mode_when_adding_and_direct_when_editing(self):
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"1n")
        terminal.wait_text("MCP name")
        terminal.send(b"sample\n")
        terminal.wait_text("stdio or http")
        terminal.send(b"\n")
        terminal.wait_text("Connection mode")
        terminal.send(b"j\n")
        self.finish_mcp_form(terminal)
        path = self.home / ".config/kitmux/catalog.json"
        self.assertEqual(json.loads(path.read_text())["mcps"]["sample"]["mode"], "managed")
        terminal.send(b"x")
        terminal.wait_text("stdio or http")
        terminal.send(b"\n")
        terminal.wait_text("Connection mode")
        terminal.send(b"k\n")
        terminal.wait_text("Executable path or command")
        terminal.send(b"\n")
        terminal.wait_text("Arguments (quotes supported")
        terminal.send(b"\n")
        terminal.wait_text("Saved")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        self.assertEqual(json.loads(path.read_text())["mcps"]["sample"]["mode"], "direct")

    def test_reconnect_chooses_opencode_and_shows_native_refresh_advice(self):
        self.run_cli("mcp", "add", "sample", "--command", "/usr/bin/true")
        self.run_cli("mcp", "enable", "sample", "--agent", "opencode")
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"1jr")
        terminal.wait_text("Reconnect agent")
        terminal.send(b"jj\n")
        terminal.wait_text("/mcps")
        terminal.send(b"\nq")
        self.assertEqual(terminal.wait_exit(), 0)

    def test_status_refresh_captures_new_observation(self):
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"5")
        terminal.wait_text("Runtime snapshot")
        self.run_cli("mcp", "add", "late", "--command", "/usr/bin/true")
        self.assertNotIn(b"mcp:late", terminal.read_for(.25), "snapshot refreshed without an explicit request")
        terminal.send(b"f")
        terminal.wait_text("mcp:late")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)

    def test_status_observes_owned_upstream_without_restarting_it(self):
        import test_runtime as runtime

        marker = self.home / "upstream-starts"
        self.run_cli("mcp", "add", "stub", "--mode", "managed", "--command",
                     sys.executable, "--", str(runtime.FIXTURE), str(marker))
        self.run_cli("mcp", "enable", "stub", "--agent", "codex")
        harness = runtime.RuntimeTests("test_discovery_call_metadata_and_reconnect")
        harness.home, harness.marker = self.home, marker
        self.addCleanup(harness.doCleanups)
        harness.bridge()
        self.assertIn("result", harness.call())
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"5")
        terminal.wait_text("upstream running")
        harness.close_bridge()
        terminal.send(b"f")
        terminal.wait_text("inactive")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        self.assertEqual(len(marker.read_text().splitlines()), 1)

    def test_add_group_and_assign_two_agents(self):
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"g")
        terminal.wait_text("Group name")
        terminal.send(b"communication\n")
        terminal.wait_text("communication")
        terminal.send(b"a")
        terminal.wait_text("Assign agents")
        terminal.send(b" j \n")
        terminal.wait_text("Assignments saved")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        catalog = json.loads((self.home / ".config/kitmux/catalog.json").read_text())
        self.assertEqual(catalog["groups"]["mcp:communication"]["agents"], ["codex", "claude"])

    def test_resize_navigation_and_terminal_restore(self):
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.resize(30, 100)
        terminal.process.send_signal(signal.SIGWINCH)
        terminal.send(b"5")
        terminal.wait_text("Catalog")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        restored = termios.tcgetattr(terminal.slave)
        pending = getattr(termios, "PENDIN", 0)
        self.assertEqual(restored[3] & ~pending, terminal.original[3] & ~pending)
        self.assertNotRegex(terminal.output, rb"\x1b\[(?:3[0-7]|4[0-7])m")

    def test_subagent_tab_and_client_status_navigation(self):
        terminal = self.start()
        terminal.wait_text("Subagents")
        terminal.send(b"3n")
        terminal.wait_text("Subagent name")
        terminal.send(b"\x1b4")
        terminal.wait_text("Separate configuration")
        terminal.send(b"5")
        terminal.wait_text("Runtime snapshot")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)

    def test_create_duplicate_edit_and_assign_subagent(self):
        source = self.home / "review.md"
        source.write_text("Review code for defects.\n")
        updated = self.home / "updated.md"
        updated.write_text("Review tests and failure handling.\n")
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"3n")
        terminal.wait_text("Subagent name")
        terminal.send(b"reviewer\n")
        terminal.wait_text("Definition format")
        terminal.send(b"\n")
        terminal.wait_text("Definition file")
        terminal.send(str(source).encode() + b"\n")
        terminal.wait_text("purpose")
        terminal.send(b"Review changes\n")
        terminal.wait_text("Added subagent reviewer")
        terminal.send(b"a")
        terminal.wait_text("Assign agents")
        terminal.send(b"j \n")
        terminal.wait_text("Assignments saved")
        terminal.send(b"p")
        terminal.wait_text("New subagent name")
        terminal.send(b"reviewer-copy\n")
        terminal.wait_text("Duplicated reviewer")
        terminal.send(b"x")
        terminal.wait_text("Definition format")
        terminal.send(b"\n")
        terminal.wait_text("Definition file")
        terminal.send(str(updated).encode() + b"\n")
        terminal.wait_text("purpose")
        terminal.send(b"Review changes\n")
        terminal.wait_text("Updated subagent reviewer")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        catalog = json.loads((self.home / ".config/kitmux/catalog.json").read_text())
        self.assertEqual(catalog["subagents"]["reviewer"]["agents"],
                         {"codex": "off", "claude": "on", "opencode": "off"})
        self.assertEqual(catalog["subagents"]["reviewer-copy"]["agents"], {})
        self.assertIn("failure handling", catalog["subagents"]["reviewer"]["instructions"])

    def test_native_only_assignment_keeps_other_clients_disabled(self):
        source = self.home / "reviewer.md"
        source.write_text("---\nname: reviewer\ndescription: Review code\n---\nReview code.\n")
        self.run_cli("subagent", "add", "reviewer", "--from", str(source), "--format", "claude")
        self.run_cli("sync", "--apply")
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"3ja")
        terminal.wait_text("native variant required")
        terminal.send(b"j \n")
        terminal.wait_text("Assignments saved")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        catalog = json.loads((self.home / ".config/kitmux/catalog.json").read_text())
        self.assertEqual(catalog["subagents"]["reviewer"]["agents"],
                         {"codex": "off", "claude": "on", "opencode": "off"})
        self.assertTrue((self.home / ".claude/agents/reviewer.md").is_file())
        self.assertFalse((self.home / ".codex/agents/reviewer.toml").exists())
        self.assertFalse((self.home / ".config/opencode/agents/reviewer.md").exists())

    def test_subagent_details_show_compatibility_at_minimum_terminal_size(self):
        source = self.home / "reviewer.md"
        source.write_text("---\nname: reviewer\ndescription: Review code\n---\nReview code.\n")
        self.run_cli("subagent", "add", "reviewer", "--from", str(source), "--format", "claude")
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.resize(20, 74)
        terminal.process.send_signal(signal.SIGWINCH)
        terminal.send(b"3j\n")
        terminal.wait_text("opencode")
        self.assertIn(b"N/A", terminal.output)
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)

    def test_subagent_tab_import_adopts_only_profiles(self):
        directory = self.home / ".claude/agents"
        directory.mkdir(parents=True)
        (directory / "reviewer.md").write_text(
            "---\nname: reviewer\ndescription: Review code\n---\nReview code.\n")
        (self.home / ".claude.json").write_text(
            '{"mcpServers":{"existing":{"command":"/usr/bin/true"}}}')
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"3i")
        terminal.wait_text("Review subagent import")
        terminal.send(b"\n")
        terminal.wait_text("reviewer")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        catalog = json.loads((self.home / ".config/kitmux/catalog.json").read_text())
        self.assertIn("reviewer", catalog["subagents"])
        self.assertEqual(catalog["mcps"], {})

    def test_group_membership_includes_subagents(self):
        source = self.home / "review.md"
        source.write_text("Review code.\n")
        self.run_cli("subagent", "add", "reviewer", "--from", str(source),
                     "--format", "common", "--description", "Review code")
        self.run_cli("subagent", "group", "add", "engineering")
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"3m")
        terminal.wait_text("reviewer")
        terminal.send(b" \n")
        terminal.wait_text("Saved")
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)
        catalog = json.loads((self.home / ".config/kitmux/catalog.json").read_text())
        self.assertEqual(catalog["groups"]["subagent:engineering"]["members"], ["subagent:reviewer"])

    def test_interrupt_restores_terminal(self):
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.process.send_signal(signal.SIGINT)
        terminal.wait_exit()
        pending = getattr(termios, "PENDIN", 0)
        self.assertEqual(termios.tcgetattr(terminal.slave)[3] & ~pending, terminal.original[3] & ~pending)

    def test_dumb_terminal_prints_help(self):
        terminal = self.start("dumb")
        terminal.wait_exit()
        self.assertIn(b"kitmux", terminal.output)
        self.assertEqual(terminal.process.returncode, 0)
        self.assertNotIn(b"\x1b[", terminal.output)


if __name__ == "__main__":
    unittest.main()
