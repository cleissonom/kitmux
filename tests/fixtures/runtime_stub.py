"""Local tools-only MCP fixture; never contacts an external service."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

marker = Path(sys.argv[1])
client_capabilities = {}
protocol_version = None
with marker.open("a") as stream:
    stream.write(str(os.getpid()) + "\n")


def send(request, result):
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)


for line in sys.stdin:
    request = json.loads(line)
    method = request.get("method")
    if method == "initialize":
        client_capabilities = request["params"].get("capabilities", {})
        protocol_version = request["params"]["protocolVersion"]
        send(request, {"protocolVersion": request["params"]["protocolVersion"],
                       "capabilities": {"tools": {"listChanged": True}},
                       "serverInfo": {"name": "runtime-fixture", "version": "1"}})
    elif method == "tools/list":
        send(request, {"tools": [{"name": "identity", "inputSchema": {"type": "object"}}]})
    elif method == "tools/call":
        name = request["params"]["name"]
        if name == "client-capabilities":
            send(request, {"content": [{"type": "text", "text": json.dumps(client_capabilities)}]})
            continue
        if name == "protocol-version":
            send(request, {"content": [{"type": "text", "text": protocol_version}]})
            continue
        if name == "exit":
            sys.exit(0)
        if name == "hang":
            with marker.with_suffix(".calls").open("a") as stream:
                stream.write("accepted\n")
            print(json.dumps({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}), flush=True)
            continue
        if name == "descendant":
            ready = marker.with_suffix(".ready")
            child = subprocess.Popen([sys.executable, "-c",
                "import signal,time,pathlib,sys; signal.signal(signal.SIGTERM,signal.SIG_IGN); pathlib.Path(sys.argv[1]).touch(); time.sleep(60)", str(ready)],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            while not ready.exists():
                time.sleep(.005)
            send(request, {"content": [{"type": "text", "text": str(child.pid)}]})
            continue
        send(request, {"content": [{"type": "text", "text": str(os.getpid())}],
                       "_meta": request["params"].get("_meta", {})})
