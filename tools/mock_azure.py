#!/usr/bin/env python3
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
"""
mock_azure.py - A stand-in for an Azure AI Foundry chat-completions endpoint,
for testing sia without real credentials.

    mock_azure.py CERT KEY PORT LOG [API_KEY] [MODEL] [VISION] [THINK]

Serves /openai/deployments/<name>/chat/completions, /openai/v1/chat/completions,
/models/chat/completions and /chat/completions over HTTPS.  Requires the
"api-key" header (like Azure OpenAI), so a client sending "Authorization:
Bearer" first gets 401.  Its "model" is scripted:
  - the ping prompt gets "ready";
  - a message whose first word is a tool name becomes a call of that tool;
  - "biggest file in DIR" becomes ls -l DIR, then an answer from the output;
  - "make a note" becomes write_file (a changing command, so sia asks first);
  - after tool results, it summarises them;
  - images: with VISION "yes" it reads the vision test's digits (it decodes
    the PNG), otherwise it refuses images (HTTP 400, as models without
    vision do);
  - MiR (its instructions start "You are MiR"): a request makes a window
    application, a counter: app_create, a main.c with a mistake, app_build
    (fails), the fixed main.c, app_build, app_run, app_screenshot (with
    vision), a click on its button with app_input, app_screenshot again,
    app_stop and a report (whether the window changed after the click);
    "install" calls app_install.  THINK (seconds): MiR's first answer comes
    that late, without a byte before (a model that reasons).
Every request is appended to LOG as JSON (headers without the key value).
"""
import base64, hashlib, json, re, ssl, struct, sys, zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CERT, KEY, PORT, LOG = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
API_KEY = sys.argv[5] if len(sys.argv) > 5 else "test-key"
MODEL = sys.argv[6] if len(sys.argv) > 6 else "gpt-test"
VISION = len(sys.argv) > 7 and sys.argv[7] == "yes"
THINK = int(sys.argv[8]) if len(sys.argv) > 8 else 0
calls = 0
shots = []


def png_pixels(data):
    """(width, height, rows of (r, g, b)) of an 8-bit RGB PNG."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw, stride, rows, prev = zlib.decompress(idat), w * 3, [], bytearray(w * 3)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 3] if i >= 3 else 0
            b, c = prev[i], (prev[i - 3] if i >= 3 else 0)
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * 3:x * 3 + 3]) for x in range(w)])
        prev = line
    return w, h, rows


DIGITS = [[0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E], [0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E],
          [0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F], [0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E],
          [0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02], [0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E],
          [0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E], [0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08],
          [0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E], [0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C]]


def read_number(png):
    """The vision test's four digits (5x7 cells of 10 pixels from (20, 20), 70 apart)."""
    w, h, rows = png_pixels(png)
    out = ""
    for d in range(4):
        bits = [sum((1 << (4 - cx)) for cx in range(5)
                    if sum(rows[20 + cy * 10 + 5][20 + d * 70 + cx * 10 + 5]) < 384) for cy in range(7)]
        out += str(DIGITS.index(bits)) if bits in DIGITS else "?"
    return out


def image_of(msg):
    for part in msg.get("content") or []:
        if isinstance(part, dict) and part.get("type") == "image_url":
            return base64.b64decode(part["image_url"]["url"].split(",", 1)[1])
    return None


COUNTER = r"""/* Counter - made with MiR */
#include <facet/facet.h>
#include <stdio.h>

static int count%s

static struct rect plus_rect(struct rect c) { return rect_make(c.w / 2 - 40, c.h - 60, 80, 30); }

static void draw(struct fct_view *v, struct surface *s, struct rect c)
{
    (void)v;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_CONTENT);
    char t[32];
    snprintf(t, sizeof(t), "%%d", count);
    gfx_text_scaled(s, (c.w - text_width_scaled(t, 3)) / 2, 30, t, 3, C_ACCENT);
    ui_button(s, plus_rect(c), "+1", false);
}

static void mouse(struct fct_view *v, int x, int y, int kind, int buttons)
{
    (void)buttons;
    if (kind == FCT_MOUSE_DOWN && rect_contains(plus_rect(fct_view_content(v)), x, y)) {
        count++;
        fct_view_invalidate(v);
    }
}

int main(void)
{
    if (fct_app_init() < 0)
        return 1;
    struct fct_view *v = fct_view_new("Counter", 240, 180);
    if (!v)
        return 1;
    v->draw = draw;
    v->mouse = mouse;
    return fct_main();
}
"""


WORDCOUNT = r"""// Word count - made with MiR
#include <iostream>
#include <sstream>
#include <string>

int main()
{
    std::string line, word;
    long lines = 0, words = 0;
    while (std::getline(std::cin, line)) {
        lines++;
        std::istringstream in(line);
        while (in >> word)
            words++;
    }
    std::cout << lines << " lines, " << words << " words" << std::endl;
    return 0;
}
"""

WC_TEST = """#!/bin/sh
# the word count's tests
fail=0
check() { got=$(printf "$1" | ./wordcount); [ "$got" = "$2" ] && echo "PASS: $2" || { echo "FAIL: got '$got', expected '$2'"; fail=1; }; }
check 'one two three\\nfour\\n' '2 lines, 4 words'
check '' '0 lines, 0 words'
check '  spaced   out  \\n' '1 lines, 2 words'
exit $fail
"""


def mir_tool(msgs, start, done, result):
    """A terminal program in C++: the word count."""
    if not done:
        return tool_call("app_create", {"name": "wordcount", "title": "Word count", "kind": "terminal",
                                        "language": "c++", "summary": "counts the lines and words of its input"})
    step = done[-1]
    if step == "app_create":
        return tool_call("app_write", {"path": "main.cpp", "content": WORDCOUNT})
    if step == "app_write" and done.count("app_write") == 1:
        return tool_call("app_build", {})
    if step == "app_build":
        return tool_call("app_run", {"stdin": "one two three\nfour\n"}) if "succeeded" in result else \
            reply({"role": "assistant", "content": "It does not build: " + result[:300]})
    if step == "app_run":
        return tool_call("app_write", {"path": "test.sh", "content": WC_TEST})
    if step == "app_write":
        return tool_call("app_test", {})
    if step == "app_test":
        ok = "FAIL" not in result and "exit status: 0" in result
        return reply({"role": "assistant", "content": "Word count is ready: it counts the lines and words of what it "
                      "reads. " + ("Its three tests pass." if ok else "A test fails: " + result[:200])})
    return reply({"role": "assistant", "content": "Done."})


def mir(body):
    msgs = body["messages"]
    tools = {t["function"]["name"] for t in body.get("tools", [])}
    # the request: the last user text that is not a screenshot's caption
    start = max(i for i, m in enumerate(msgs) if m["role"] == "user" and isinstance(m["content"], str)
                and not m["content"].startswith("Screenshot of"))
    request = msgs[start]["content"].lower()
    done = [c["function"]["name"] for m in msgs[start:] if m["role"] == "assistant" for c in m.get("tool_calls") or []]
    last = msgs[-1]
    result = last["content"] if last["role"] == "tool" else ""
    if "install" in request:
        return tool_call("app_install", {}) if "app_install" not in done else \
            reply({"role": "assistant", "content": "Installed: " + result.splitlines()[-1]})
    if "tool" in request:
        return mir_tool(msgs, start, done, result)
    img = image_of(last) if last["role"] == "user" else None
    if img:
        shots.append(hashlib.sha256(png_pixels(img)[2].__repr__().encode()).hexdigest())
    if not done:
        return tool_call("app_create", {"name": "counter", "title": "Counter", "kind": "window", "language": "c",
                                        "summary": "counts clicks on a button"})
    step = done[-1]
    if step == "app_create":
        return tool_call("app_write", {"path": "main.c", "content": COUNTER % ""})        # (the mistake: no ';')
    if step == "app_write":
        return tool_call("app_build", {})
    if step == "app_build":
        if "FAILED" in result:
            return tool_call("app_write", {"path": "main.c", "content": COUNTER % ";"})
        return tool_call("app_run", {"seconds": 2})
    if step == "app_run":
        if "it is running" not in result:
            return reply({"role": "assistant", "content": "It does not start: " + result[:200]})
        return tool_call("app_screenshot" if "app_screenshot" in tools else "app_input", {} if "app_screenshot" in tools else {"x": 120, "y": 135})
    if step == "app_screenshot" and done.count("app_screenshot") == 1:
        return tool_call("app_input", {"x": 120, "y": 135})
    if step == "app_input":
        return tool_call("app_screenshot", {}) if "app_screenshot" in tools else tool_call("app_stop", {})
    if step == "app_screenshot":
        return tool_call("app_stop", {})
    if step == "app_stop":
        seen = ("I looked at it before and after a click on +1: the window " +
                ("changed" if len(shots) >= 2 and shots[-1] != shots[-2] else "did NOT change") + ".") \
            if "app_screenshot" in tools else "I cannot see images: look at the window to check the count."
        return reply({"role": "assistant", "content": "Counter is ready: a window with a number and a +1 button "
                      "that counts the clicks. It builds and runs; " + seen})
    return reply({"role": "assistant", "content": "Done."})


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
    if image_of(msgs[0]) and len(msgs) == 1:          # the vision test
        return reply({"role": "assistant", "content": read_number(image_of(msgs[0]))})
    if msgs[0]["role"] == "system" and msgs[0]["content"].startswith("You are MiR"):
        global THINK
        if THINK:
            import time
            time.sleep(THINK)
            THINK = 0
        return mir(body)
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
    if "open_app" in tools and re.search(r"\bmake\b.*\bapp", text):    # an application to make: MiR
        return tool_call("open_app", {"app": "mir", "path": text})
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
        if not VISION and any(image_of(m) for m in body.get("messages", []) if m["role"] == "user"):
            return self.send(400, {"error": {"code": "BadRequest", "message": "Invalid content type. image_url is only supported by certain models."}})
        self.send(200, think(body))


srv = ThreadingHTTPServer(("0.0.0.0", PORT), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(CERT, KEY)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print(f"mock Azure endpoint on https://0.0.0.0:{PORT}", flush=True)
srv.serve_forever()
