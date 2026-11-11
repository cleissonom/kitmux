import json
import base64
import os
import signal
import socket
import subprocess
import shutil
import time
import uuid
from urllib import error, request


class OpenCodeServer:
    def __init__(self, sandbox):
        self.sandbox = sandbox
        sandbox.env["OPENCODE_PASSWORD"] = "synthetic-fixture-password"
        self.authorization = "Basic " + base64.b64encode(b"opencode:synthetic-fixture-password").decode()
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        self.url = f"http://127.0.0.1:{port}"
        self.log_path = sandbox.home / "opencode-server.log"
        self.log = self.log_path.open("w+")
        self.process = subprocess.Popen([shutil.which("opencode"), "serve", "--hostname", "127.0.0.1",
            "--port", str(port)], cwd=sandbox.home, env=sandbox.env, stdout=self.log, stderr=self.log,
            start_new_session=True)
        deadline = time.monotonic()+25
        while time.monotonic()<deadline:
            try:
                self.schema = self.request("GET", "/openapi.json", timeout=.5)
                return
            except (OSError, error.URLError):
                if self.process.poll() is not None:
                    break
                time.sleep(.05)
            except AssertionError as exc:
                if "503" in str(exc) and "service_starting" in str(exc):
                    time.sleep(.05)
                    continue
                self.close()
                raise
        self.close()
        raise AssertionError("OpenCode private server failed to start: " + self.log_path.read_text()[-2000:])

    def request(self, method, path, body=None, timeout=10):
        encoded = json.dumps(body).encode() if body is not None else None
        query = request.Request(self.url+path, data=encoded, method=method,
                                headers={"Content-Type":"application/json", "Authorization":self.authorization})
        try:
            with request.urlopen(query, timeout=timeout) as response:
                text = response.read()
                return json.loads(text) if text else None
        except error.HTTPError as exc:
            message = exc.read().decode()[:2000]
            exc.close()
            raise AssertionError(f"OpenCode {method} {path}: {exc.code} {message}") from exc

    def prompt(self, session, text, timeout=25):
        endpoint = f"/api/session/{session}/message?order=desc&limit=20"
        baseline = {message["id"] for message in self.request("GET", endpoint)["data"]}
        phase = uuid.uuid4().hex
        marker = "fixture-complete:" + phase
        admitted = self.request("POST", f"/api/session/{session}/prompt",
                                {"text":text + f" [kitmux-phase:{phase}]"})["data"]["id"]
        deadline = time.monotonic()+timeout
        while time.monotonic()<deadline:
            messages = self.request("GET", endpoint)["data"]
            for message in messages:
                if message["id"] == admitted:
                    break
                if message["id"] in baseline or message.get("type") != "assistant":
                    continue
                completed = message.get("time", {}).get("completed")
                text_parts = [part.get("text", "") for part in message.get("content", [])
                              if part.get("type") == "text"]
                if isinstance(completed, (int, float)) and message.get("finish") == "stop" and any(
                        marker in part for part in text_parts):
                    return message
                if message.get("finish") == "error":
                    raise AssertionError(f"OpenCode fixture prompt failed: {message.get('error')}")
            time.sleep(.05)
        raise AssertionError(f"OpenCode fixture prompt {admitted} did not persist its completed assistant reply")

    def close(self):
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGTERM)
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=3)
        self.log.close()
