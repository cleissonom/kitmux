"""A loopback protocol fixture, not a language model or external provider."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import re
import threading
import time


class LocalProvider:
    def __init__(self):
        self.requests = []
        self.tool_name = None
        self.use_code_mode = False
        self.code_path = None
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), self.handler())
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server.server_port}"

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)

    @staticmethod
    def completion(body):
        for message in reversed(body.get("messages", [])):
            if message.get("role") != "user":
                continue
            phase = re.search(r"\[kitmux-phase:([0-9a-f]{32})\]", json.dumps(message.get("content")))
            if phase:
                return "fixture-complete:" + phase.group(1)
        return "fixture-complete"

    def handler(self):
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                self.reply({"object":"list", "data":[{"id":"fixture", "object":"model"}]})

            def reply(self, value):
                body = json.dumps(value).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))) or "{}")
                fixture.requests.append({"path":self.path,"body":body})
                if "count_tokens" in self.path:
                    self.reply({"input_tokens":16})
                    return
                anthropic = "messages" in self.path
                tools = body.get("tools", [])
                names = [tool.get("name") or tool.get("function", {}).get("name") for tool in tools]
                name = next((name for name in names if name and "stub" in name and "identity" in name), None)
                if fixture.use_code_mode and "execute" in names:
                    name = "execute"
                if name:
                    fixture.tool_name = name
                messages = body.get("messages", [])
                last = messages[-1] if messages else {}
                content = last.get("content", [])
                finished = last.get("role") == "tool" or (isinstance(content, list) and any(
                    item.get("type") == "tool_result" for item in content if isinstance(item, dict)))
                call = fixture.tool_name if not finished else None
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "close")
                self.end_headers()
                if anthropic:
                    self.anthropic(body, call)
                else:
                    self.openai(body, call)

            def event(self, value, event=None):
                if event:
                    self.wfile.write(f"event: {event}\n".encode())
                self.wfile.write(b"data: " + json.dumps(value).encode() + b"\n\n")
                self.wfile.flush()

            def anthropic(self, body, call):
                number = len(fixture.requests)
                self.event({"type":"message_start", "message":{"id":f"msg_fixture_{number}","type":"message",
                    "role":"assistant","model":body.get("model","fixture"),"content":[],"stop_reason":None,
                    "stop_sequence":None,"usage":{"input_tokens":16,"output_tokens":0}}},"message_start")
                block = {"type":"tool_use","id":f"toolu_fixture_{number}","name":call,"input":{}} if call else {"type":"text","text":""}
                self.event({"type":"content_block_start","index":0,"content_block":block},"content_block_start")
                delta = {"type":"input_json_delta","partial_json":"{}"} if call else {"type":"text_delta","text":fixture.completion(body)}
                self.event({"type":"content_block_delta","index":0,"delta":delta},"content_block_delta")
                self.event({"type":"content_block_stop","index":0},"content_block_stop")
                self.event({"type":"message_delta","delta":{"stop_reason":"tool_use" if call else "end_turn","stop_sequence":None},
                            "usage":{"output_tokens":16}},"message_delta")
                self.event({"type":"message_stop"},"message_stop")

            def openai(self, body, call):
                number = len(fixture.requests)
                envelope = {"id":f"chatcmpl_fixture_{number}","object":"chat.completion.chunk",
                            "created":int(time.time()),"model":body.get("model","fixture")}
                code = f"return await {fixture.code_path}({{}});"
                arguments = json.dumps({"code":code}) if call == "execute" else "{}"
                delta = {"role":"assistant","tool_calls":[{"index":0,"id":f"call_fixture_{number}","type":"function",
                         "function":{"name":call,"arguments":arguments}}]} if call else {"role":"assistant","content":fixture.completion(body)}
                self.event({**envelope,"choices":[{"index":0,"delta":delta,"finish_reason":None}]})
                self.event({**envelope,"choices":[{"index":0,"delta":{},"finish_reason":"tool_calls" if call else "stop"}],
                            "usage":{"prompt_tokens":16,"completion_tokens":16,"total_tokens":32}})
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()

        return Handler
