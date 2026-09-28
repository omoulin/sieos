#!/usr/bin/env python3
"""
mock_azure.py - A stand-in for an Azure AI Foundry chat-completions endpoint,
for testing sia without real credentials.

    mock_azure.py CERT KEY PORT LOG [API_KEY] [MODEL]

Serves /openai/deployments/<name>/chat/completions, /openai/v1/chat/completions,
/models/chat/completions and /chat/completions over HTTPS.  Requires the
"api-key" header (like Azure OpenAI), so a client sending "Authorization:
Bearer" first gets 401.  Its "model" is scripted:
  - the ping prompt gets "ready";
  - a message whose first word is a tool name becomes a call of that tool;
  - "biggest file in DIR" becomes ls -l DIR, then an answer from the output;
  - "make a note" becomes write_file (a changing command, so sia asks first);
  - after tool results, it summarises them.
Every request is appended to LOG as JSON (headers without the key value).
"""
import json, re, ssl, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CERT, KEY, PORT, LOG = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
API_KEY = sys.argv[5] if len(sys.argv) > 5 else "test-key"
MODEL = sys.argv[6] if len(sys.argv) > 6 else "gpt-test"
calls = 0


def reply(message, finish="stop"):
    return {"id": "chatcmpl-mock", "object": "chat.completion", "model": MODEL,
            "choices": [{"index": 0, "message": message, "finish_reason": finish}],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}}


def tool_call(name, args):
    global calls
    calls += 1
    return reply({"role": "assistant", "content": None,
                  "tool_calls": [{"id": f"call_{calls}", "type": "function",
                                  "function": {"name": name, "arguments": json.dumps(args)}}]}, "tool_calls")


def think(body):
    msgs = body["messages"]
    tools = {t["function"]["name"] for t in body.get("tools", [])}
    last = msgs[-1]
    if last["role"] == "tool":
        # find the user request of this turn
        user = next(m["content"] for m in reversed(msgs) if m["role"] == "user")
        out = last["content"]
        m = re.search(r"biggest file in (\S+)", user)
        if m:
            best = None
            for line in out.splitlines():
                f = line.split()
                if len(f) >= 5 and f[0][0] == "-":
                    try:
                        size = int(f[4])
                    except ValueError:
                        continue
                    if not best or size > best[0]:
                        best = (size, f[-1])
            if best:
                return reply({"role": "assistant",
                              "content": f"The biggest file in {m.group(1)} is {best[1]} ({best[0]} bytes) — done ✓"})
        status = out.splitlines()[0] if out else ""
        ok = "exit status: 0" in status or not status.startswith("exit status")
        return reply({"role": "assistant", "content": None if ok else f"The command failed ({status})."})
    text = last["content"].strip()
    if text.startswith("Reply with the single word"):
        return reply({"role": "assistant", "content": "ready"})
    m = re.search(r"biggest file in (\S+)", text)
    if m:
        return tool_call("ls", {"args": ["-l", m.group(1)]})
    if "make a note" in text:
        return tool_call("write_file", {"path": "note.txt", "content": "remember the milk\n"})
    words = text.split()
    if words and words[0] in tools and words[0] not in ("cd", "sh", "write_file"):
        if any(c in text for c in "|><;&$"):
            return tool_call("sh", {"command": text})
        return tool_call(words[0], {"args": words[1:]})
    if words and words[0] == "cd":
        return tool_call("cd", {"path": words[1] if len(words) > 1 else ""})
    return reply({"role": "assistant", "content": f"You said: {text}"})


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, obj):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        # exercise chunked decoding on every other response
        if calls % 2:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(data), 100):
                piece = data[i:i + 100]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        entry = {"path": self.path, "api_key_ok": self.headers.get("api-key") == API_KEY,
                 "bearer": "Authorization" in self.headers, "model": body.get("model"),
                 "ntools": len(body.get("tools", [])), "messages": body.get("messages")}
        with open(LOG, "a") as f:
            f.write(json.dumps(entry) + "\n")
        if self.headers.get("api-key") != API_KEY:
            return self.send(401, {"error": {"code": "401", "message": "Access denied due to invalid subscription key or wrong API endpoint."}})
        m = re.match(r"/openai/deployments/([^/]+)/chat/completions", self.path)
        if m and m.group(1) != MODEL:
            return self.send(404, {"error": {"code": "DeploymentNotFound", "message": "The API deployment for this resource does not exist."}})
        if not m and body.get("model") != MODEL:
            return self.send(400, {"error": {"code": "unknown_model", "message": f"Unknown model: {body.get('model')}"}})
        self.send(200, think(body))


srv = ThreadingHTTPServer(("0.0.0.0", PORT), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(CERT, KEY)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print(f"mock Azure endpoint on https://0.0.0.0:{PORT}", flush=True)
srv.serve_forever()
