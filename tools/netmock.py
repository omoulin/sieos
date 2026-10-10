#!/usr/bin/env python3
"""A stand-in for an OpenAI-compatible model server, for testing sia's
remote backend: POST /v1/chat/completions answers "You said: <the last user
message> (N messages)" in small pieces, streamed as Server-Sent Events
("stream": true) or all at once. With a key set, requests without
"Authorization: Bearer <key>" get HTTP 401 and an error message.

Usage: netmock.py [port] [key]   (port 0: any; prints the port)
Also importable: start(port, key) -> (server, port).

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
"""
import http.server, json, sys, threading, time

def start(port=0, key=""):
    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        def log_message(self, *a): pass
        def send_json(self, code, obj):
            b = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            H.requests.append(body)
            if key and self.headers.get("Authorization") != f"Bearer {key}":
                return self.send_json(401, {"error": {"message": "Incorrect API key provided", "type": "auth"}})
            if not self.path.endswith("/chat/completions"):
                return self.send_json(404, {"error": {"message": "no such path " + self.path}})
            msgs = body.get("messages", [])
            last = next((m["content"] for m in reversed(msgs) if m.get("role") == "user"), "")
            text = f"You said: {last} ({len(msgs)} messages, model {body.get('model')}) é✓"
            if not body.get("stream"):
                return self.send_json(200, {"choices": [{"message": {"role": "assistant", "content": text}}]})
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            def chunk(s):
                b = s.encode()
                self.wfile.write(f"{len(b):x}\r\n".encode() + b + b"\r\n")
                self.wfile.flush()
            chunk("data: " + json.dumps({"choices": [{"delta": {"role": "assistant"}}]}) + "\n\n")
            for i in range(0, len(text), 4):
                chunk("data: " + json.dumps({"choices": [{"delta": {"content": text[i:i + 4]}}]}) + "\n\n")
                time.sleep(0.005)
            chunk("data: [DONE]\n\n")
            self.wfile.write(b"0\r\n\r\n")
        def do_GET(self):
            self.send_json(200, {"data": [{"id": "mock-model"}]})
    H.requests = []
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
    srv.requests = H.requests
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]

if __name__ == "__main__":
    srv, port = start(int(sys.argv[1]) if len(sys.argv) > 1 else 0, sys.argv[2] if len(sys.argv) > 2 else "")
    print(f"mock OpenAI-compatible server on 127.0.0.1:{port} (from SIEOS: http://10.0.2.2:{port}/v1)")
    threading.Event().wait()
