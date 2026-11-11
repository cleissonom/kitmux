import json
import os
from pathlib import Path
import selectors
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("KITMUX_BINARY", ROOT / "build/kitmux")).resolve()
STUB = ROOT / "tests/native_clients/mcp_fixture.py"


class Sandbox:
    def __init__(self, agent):
        self.temporary = tempfile.TemporaryDirectory(prefix="km-native-", dir="/tmp")
        self.home = Path(self.temporary.name).resolve()
        self.agent = agent
        self.marker = self.home / "upstream-starts"
        for relative in ("tmp", ".codex", ".claude", ".config", ".local/share", ".local/state", ".cache"):
            (self.home / relative).mkdir(parents=True, exist_ok=True)
        self.env = {
            "PATH": os.environ["PATH"], "HOME": str(self.home), "TMPDIR": str(self.home / "tmp"),
            "LANG": "en_US.UTF-8", "TERM": "dumb", "NO_COLOR": "1",
            "CODEX_HOME": str(self.home / ".codex"), "CLAUDE_CONFIG_DIR": str(self.home / ".claude"),
            "XDG_CONFIG_HOME": str(self.home / ".config"),
            "XDG_DATA_HOME": str(self.home / ".local/share"),
            "XDG_STATE_HOME": str(self.home / ".local/state"),
            "XDG_CACHE_HOME": str(self.home / ".cache"),
            "DISABLE_AUTOUPDATER": "1", "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1",
        }
        self.catalog_path = self.home / ".config/kitmux/catalog.json"
        self.catalog_path.parent.mkdir()
        self.catalog_path.write_text(json.dumps({"version": 1, "revision": 1, "groups": {}, "skills": {}, "mcps": {
            "stub": {"transport": "stdio", "mode": "managed", "enabled": True,
                     "agents": {agent: "on"}, "command": [sys.executable, str(STUB), str(self.marker)]}}}))

    def kitmux(self, *args):
        result = subprocess.run([str(BINARY), "--home", str(self.home), *args], env=self.env,
                                cwd=self.home, capture_output=True, text=True, timeout=15)
        if result.returncode:
            raise AssertionError(f"Kitmux {args}: {result.stderr or result.stdout}")
        return result

    def command(self, executable, *args, **kwargs):
        return subprocess.run([shutil.which(executable), *args], cwd=self.home, env=self.env,
                              capture_output=True, text=True, timeout=30, **kwargs)

    def calls(self):
        if not self.marker.exists():
            return []
        return [json.loads(line) for line in self.marker.read_text().splitlines()
                if json.loads(line)["packet"].get("method") == "tools/call"]

    def close(self):
        self.temporary.cleanup()


class JsonLinesProcess:
    def __init__(self, sandbox, executable, args):
        self.sandbox = sandbox
        self.log = (sandbox.home / f"{executable}.stderr").open("w+")
        self.process = subprocess.Popen([shutil.which(executable), *args], cwd=sandbox.home, env=sandbox.env,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
                                        start_new_session=True)
        self.buffer = b""
        self.number = 0
        self.notifications = []

    def send(self, packet):
        self.process.stdin.write(json.dumps(packet).encode() + b"\n")
        self.process.stdin.flush()

    def packet(self, timeout=20):
        deadline = time.monotonic() + timeout
        selector = selectors.DefaultSelector()
        selector.register(self.process.stdout, selectors.EVENT_READ)
        try:
            while b"\n" not in self.buffer:
                events = selector.select(max(0, deadline-time.monotonic()))
                if not events:
                    raise AssertionError("Native client response timed out: " + self.errors())
                chunk = os.read(self.process.stdout.fileno(), 65536)
                if not chunk:
                    raise AssertionError("Native client exited before replying: " + self.errors())
                self.buffer += chunk
            line, self.buffer = self.buffer.split(b"\n", 1)
            return json.loads(line)
        finally:
            selector.close()

    def request(self, method, params=None):
        self.number += 1
        number = self.number
        self.send({"id": number, "method": method, "params": params or {}})
        for _ in range(100):
            packet = self.packet()
            if packet.get("id") == number and "method" not in packet:
                return packet
            if "id" in packet and "method" in packet:
                self.send({"id":packet["id"], "error":{"code":-32601,"message":"Qualification client has no interactive callbacks"}})
            else:
                self.notifications.append(packet)
        raise AssertionError("Response missing among native client notifications")

    def errors(self):
        self.log.flush()
        return (self.sandbox.home / self.log.name).read_text()[-4000:]

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(self.process.pid, signal.SIGTERM)
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=3)
        self.process.stdout.close()
        self.log.close()
