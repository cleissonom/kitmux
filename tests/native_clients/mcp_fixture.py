"""Credential-free native-client MCP fixture with an observable request log."""
import json
import os
from pathlib import Path
import sys

log = Path(sys.argv[1])


def record(packet):
    with log.open("a") as stream:
        stream.write(json.dumps({"pid":os.getpid(),"packet":packet}) + "\n")


for line in sys.stdin:
    packet = json.loads(line)
    record(packet)
    if "id" not in packet:
        continue
    method = packet.get("method")
    if method == "initialize":
        result = {"protocolVersion":packet["params"]["protocolVersion"],"capabilities":{"tools":{}},
                  "serverInfo":{"name":"kitmux-native-fixture","version":"1"}}
    elif method == "tools/list":
        result = {"tools":[{"name":"identity","description":"Return fixture process and native request metadata",
                            "inputSchema":{"type":"object","properties":{"note":{"type":"string"}},"additionalProperties":False},
                            "annotations":{"readOnlyHint":True,"destructiveHint":False}}]}
    elif method == "tools/call":
        meta = packet.get("params", {}).get("_meta", {})
        result = {"content":[{"type":"text","text":json.dumps({"pid":os.getpid(),"meta":meta})}],"_meta":meta}
    elif method == "ping":
        result = {}
    else:
        print(json.dumps({"jsonrpc":"2.0","id":packet["id"],"error":{"code":-32601,"message":"Unsupported fixture method"}}), flush=True)
        continue
    print(json.dumps({"jsonrpc":"2.0","id":packet["id"],"result":result}), flush=True)
