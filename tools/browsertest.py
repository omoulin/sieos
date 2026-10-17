#!/usr/bin/env python3
"""Test the web browser on the desktop (make browser-test) through QEMU's
control socket (QMP): screenshots, keys and the mouse, checked against
atlas's log lines on the serial port and against the pixels.

  first start (accounts) -> alice's terminal -> atlaswin: another program's
  window (mk/atlas.h) drawn, a click seen in it, 'q' ends it -> web: a local
  page in the browser's window, its pixels; the address typed (Ctrl-L), a
  second page; back -> a page over HTTPS from the host -> the window closed.

Usage: browsertest.py <cpus> <screenshots dir> <disk image> <qemu command line...>
The test works in a copy of the image. Screenshots (PNG) are kept.

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import atexit, json, os, select, shutil, socket, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "webdeps"))
import qserial, servers

cpus, shots, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
os.makedirs(shots, exist_ok=True)
copy = image + ".web"
shutil.copyfile(image, copy)
ports = servers.start(copy, os.path.join(os.path.dirname(image), "host", "siefs")).split()   # PLAIN TLS H2 SELF
sock_path = os.path.join(shots, "qmp.sock")
if os.path.exists(sock_path):
    os.unlink(sock_path)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
cmdline += ["-qmp", f"unix:{sock_path},server=on,wait=off"]
qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
atexit.register(lambda: qemu.poll() is None and qemu.kill())
full = b""
pos = 0

def fail(msg):
    qemu.kill()
    sys.exit(f"FAIL: {msg}\n--- serial (last part) ---\n{full[-3000:].decode(errors='replace')}")

def pump(t):
    global full
    if select.select([qemu.stdout], [], [], t)[0]:
        full += qemu.stdout.read1(65536)

def wait(text, timeout=30):
    """Wait for text after the last wait -> the rest of its line."""
    global pos
    end = time.time() + timeout
    while text.encode() not in full[pos:]:
        if time.time() > end or qemu.poll() is not None:
            fail(f"waiting for {text!r}")
        pump(0.2)
    i = full.index(text.encode(), pos) + len(text)
    while b"\n" not in full[i:] and time.time() < end:
        pump(0.1)
    e = full.find(b"\n", i)
    pos = e + 1
    return full[i:e].decode(errors="replace").strip()

for _ in range(100):
    try:
        qmp = socket.socket(socket.AF_UNIX)
        qmp.connect(sock_path)
        break
    except OSError:
        time.sleep(0.05)
qf = qmp.makefile("rw")
json.loads(qf.readline())

def q(cmd, **args):
    qf.write(json.dumps({"execute": cmd, "arguments": args}) + "\n")
    qf.flush()
    while True:
        r = json.loads(qf.readline())
        if "return" in r:
            return r["return"]
        if "error" in r:
            fail(f"QMP {cmd}: {r['error']}")
q("qmp_capabilities")

KEYS = {" ": "spc", "\n": "ret", "\t": "tab", "\b": "backspace", ".": "dot", "/": "slash", "-": "minus", "=": "equal",
        ",": "comma", ";": "semicolon", "\x1b": "esc"}
SHIFTED = {"?": "slash", "_": "minus", "+": "equal", ">": "dot", "<": "comma", ":": "semicolon", '"': "apostrophe"}
def type_text(s):
    for c in s:
        k = ["shift", c.lower()] if c.isalpha() and c.isupper() else ["shift", SHIFTED[c]] if c in SHIFTED else [KEYS.get(c, c)]
        q("send-key", keys=[{"type": "qcode", "data": x} for x in k])
        time.sleep(0.02)
def keys(*names):
    q("send-key", keys=[{"type": "qcode", "data": n} for n in names])
    time.sleep(0.05)

SCREEN, ZOOM = (1920, 1080), 0.75          # the screen; screen pixels per picture pixel (atlas logs picture places)
def mouse_to(x, y):
    q("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": int((x * ZOOM + 0.5) * 32768 / SCREEN[0])}},
                                  {"type": "abs", "data": {"axis": "y", "value": int((y * ZOOM + 0.5) * 32768 / SCREEN[1])}}])
    time.sleep(0.05)
def click(x, y):
    mouse_to(x, y)
    for d in (True, False):
        q("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": "left"}}])
        time.sleep(0.05)

def shot(name):
    ppm = os.path.join(shots, name + ".ppm")
    q("screendump", filename=ppm)
    q("screendump", filename=os.path.join(shots, name + ".png"), format="png")
    data = open(ppm, "rb").read()
    os.unlink(ppm)
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

def count(img, rgb, box):
    """Pixels of colour rgb in box (screen pixels), every 2nd one."""
    w, h, px = img
    want = bytes.fromhex(rgb)
    x0, y0, x1, y1 = (max(0, int(v)) for v in box)
    n = 0
    for y in range(y0, min(y1, h), 2):
        row = px[y * w * 3:(y + 1) * w * 3]
        for x in range(x0, min(x1, w), 2):
            if row[x * 3:x * 3 + 3] == want:
                n += 1
    return n

def look(rgb):
    """What Phosphor (the default look) makes of a colour, on a row that is not a scanline (draw.c)."""
    r, g, b = (int(rgb[i:i + 2], 16) for i in (0, 2, 4))
    v = min(255, ((r * 77 + g * 150 + b * 29) >> 8) * 5 // 4)
    return "".join(f"{c * v // 255:02x}" for c in (0x46, 0xff, 0x7a))

def window(w):
    """The last place logged for window w (picture) -> its box in screen pixels."""
    pump(0.3)
    k = full.rfind(f"atlas: dbg window {w} at ".encode())
    if k < 0:
        fail(f"no place logged for window {w}")
    return tuple(int(v) * ZOOM for v in full[k:].split(b" at ", 1)[1].split()[:4])

def until(what, check, timeout=90):
    end = time.time() + timeout
    while time.time() < end:
        img = shot("poll")
        if check(img):
            return img
        time.sleep(1)
    fail(f"the screen never showed {what}")

# ---- first start: accounts (as gui-test does), alice's terminal
start = time.time()
wait(f"mk: {cpus} CPU")
wait("atlas: first start: accounts needed")
wait("atlas: greeter")
time.sleep(0.5)
type_text("rootpass1\trootpass1\n")
wait("atlas: setup: step 2")
type_text("alice\tAlice Example\talicepass1\talicepass1\n")
wait("atlas: setup: step 3")
type_text("\n")
wait("atlas: login alice: ok", 30)
wait("shown")
qserial.type_line(qemu, "root")
time.sleep(0.5)
for _ in range(20):
    pump(0.2)
    if b"Password:" in full[-200:]:
        break
qserial.type_line(qemu, "rootpass1")
time.sleep(0.5)
qserial.type_line(qemu, "echo 1 > /tmp/atlas.debug")
time.sleep(1)
print(f"ok: alice logged in on the desktop ({time.time() - start:.1f} s)")

# ---- another program's window (atlaswin)
type_text("atlaswin\n")
got = wait("atlas: window 16 opened by pid", 30)
wait("atlas: window 16 shown")
time.sleep(1.5)
type_text(" ")                                  # (atlas logs places as it settles)
time.sleep(0.5)
x0, y0, x1, y1 = window(16)
img = shot("10-atlaswin")
inside = (x0 + 10, y0 + 60, x1 - 10, y1 - 10)
red, blue = count(img, look("ff0000"), inside), count(img, look("0000ff"), inside)
if red < 500 or blue < 500:
    fail(f"atlaswin's window does not show its pixels (red {red}, blue {blue})")
cx, cy = (x0 + x1) / 2 / ZOOM - 100, (y0 + y1) / 2 / ZOOM
click(cx, cy)
time.sleep(0.8)
img = shot("11-atlaswin-click")
if count(img, look("ffffff"), (cx * ZOOM - 20, cy * ZOOM - 20, cx * ZOOM + 20, cy * ZOOM + 20)) < 20:
    fail("the click in atlaswin's window was not drawn")
type_text("q")
time.sleep(0.5)
mouse_to(cx, cy)                                 # (atlas notices the program ended at the next input)
wait("atlas: window 16: its program ended", 10)
print("ok: another program's window: drawn, a click and a key received, its end noticed")

# ---- the browser
type_text("web file:///lib/web/basic.html\n")
wait("atlas: window 16 opened by pid", 30)
t0 = time.time()
time.sleep(2)
def page_shown(img, colours, n=200):
    x0, y0, x1, y1 = window(16)
    box = (x0 + 4, y0 + 100, x1 - 4, y1 - 4)
    return all(count(img, look(c), box) >= n for c in colours)
until("the basic page (its red, green and blue squares)", lambda img: page_shown(img, ("ff0000", "00ff00", "0000ff")))
shot("20-web-basic")
print(f"ok: web: a local page shown in its window ({time.time() - t0:.1f} s)")

keys("ctrl", "l")
time.sleep(0.5)
type_text("file:///lib/web/script.html\n")
until("the script page (its canvas)", lambda img: page_shown(img, ("008000", "800080")))
shot("21-web-script")
print("ok: web: an address typed in the toolbar (Ctrl-L), the page shown")

x0, y0, x1, y1 = window(16)
click((x0 + 2 * ZOOM + 24) / ZOOM, (y0 + 34 * ZOOM + 20) / ZOOM)   # the toolbar's back button (its insides: 2 and 34 in; the button 8..40, 6..34)
until("the basic page again", lambda img: page_shown(img, ("ff0000", "00ff00", "0000ff")))
print("ok: web: back")

keys("ctrl", "l")
time.sleep(0.5)
type_text(f"https://10.0.2.2:{ports[1]}/page\n")
until("the page over HTTPS", lambda img: page_shown(img, ("123456",), 2000))
shot("22-web-https")
print("ok: web: a page over HTTPS from the host (tls.c's certificate check)")

keys("ctrl", "w")                                # atlas closes the focused window
wait("atlas: window 16 closed", 10)
print(f"PASS ({time.time() - start:.0f} s)")
