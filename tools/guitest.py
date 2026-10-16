#!/usr/bin/env python3
"""Test the desktop (Stage, /bin/atlas) through QEMU's control socket
(QMP): screenshots, key presses and absolute mouse events, checked against
the desktop's log lines on the serial port and against the pixels.

  first start on the screen (accounts) -> alice's first terminal -> a
  file from the shell -> windows: a second terminal moved by its tab,
  resized by its grip, zoomed, minimized (double-click) and back from the
  Deskbar, sent to another workspace, closed -> history, copy and paste ->
  Files and the editor -> the sia strip (a file, an app, a question to sia,
  its buttons, Stop) -> Settings (mouse, screen size, sia's model) -> kill
  atlas: init restarts it -> kill the console server -> memory and frame
  times.

With /tmp/atlas.debug (made here), atlas logs where it drew things.

Usage: guitest.py <cpus> <screenshots dir> <disk image> <qemu command line...>
The test works in a copy of the image. Screenshots (PNG) are kept.

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import os, atexit, json, os, select, shutil, socket, subprocess, sys, time
import qserial                                      # (tools/qserial.py: typing on the console)

cpus, shots, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
os.makedirs(shots, exist_ok=True)
copy = image + ".gui"
shutil.copyfile(image, copy)
sock_path = os.path.join(shots, "qmp.sock")
if os.path.exists(sock_path):
    os.unlink(sock_path)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
cmdline += ["-qmp", f"unix:{sock_path},server=on,wait=off"]
qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
atexit.register(lambda: qemu.poll() is None and qemu.kill())
full = b""                      # everything from the serial port
pos = {"con": 0, "log": 0}      # two readers: the text console, and atlas's log lines
                                # (they share the port, so their order may vary)

TAB = 34                                    # a window tab's height (atlas.h TAB_H)

def fail(msg):
    qemu.kill()
    sys.exit(f"FAIL: {msg}\n--- serial (last part) ---\n{full[-3000:].decode(errors='replace')}")

def wait(text, who="log", timeout=20):
    """Wait for `text` after reader `who`'s position; returns the rest of that line."""
    global full
    end = time.time() + timeout
    while text.encode() not in full[pos[who]:]:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            fail(f"waiting for {text!r}")
        if select.select([qemu.stdout], [], [], left)[0]:
            full += qemu.stdout.read1(4096)
    i = full.index(text.encode(), pos[who]) + len(text)
    while b"\n" not in full[i:] and time.time() < end and qemu.poll() is None:   # the whole line
        if select.select([qemu.stdout], [], [], 0.2)[0]:
            full += qemu.stdout.read1(4096)
    e = full.find(b"\n", i)
    pos[who] = i
    return full[i:e if e >= 0 else len(full)].decode(errors="replace")

def wait_any(texts, who="log", timeout=60):
    """Wait for whichever of `texts` comes first after reader `who`'s position; returns (text, rest of its line)."""
    global full
    end = time.time() + timeout
    while True:
        hits = [(full.find(t.encode(), pos[who]), t) for t in texts]
        hits = [h for h in hits if h[0] >= 0]
        if hits:
            return min(hits)[1], wait(min(hits)[1], who, max(1, end - time.time()))
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            fail(f"waiting for one of {texts!r}")
        if select.select([qemu.stdout], [], [], left)[0]:
            full += qemu.stdout.read1(4096)

def expect(text, timeout=20):
    return wait(text, "con", timeout)

def send(line):
    qserial.type_line(qemu, line)

# ---- QMP
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

KEYS = {" ": "spc", "\n": "ret", "\t": "tab", "\b": "backspace", ".": "dot", "/": "slash", "-": "minus",
        "=": "equal", ",": "comma", ";": "semicolon", "\x1b": "esc"}
SHIFTED = {"?": "slash", "_": "minus", "+": "equal", ">": "dot", "<": "comma", ":": "semicolon", '"': "apostrophe"}

def type_text(s):
    for c in s:
        if c.isalpha() and c.isupper():
            keys = ["shift", c.lower()]
        elif c in SHIFTED:
            keys = ["shift", SHIFTED[c]]
        else:
            keys = [KEYS.get(c, c)]
        q("send-key", keys=[{"type": "qcode", "data": k} for k in keys])
        time.sleep(0.02)

SCREEN = [1920, 1080]           # the screen's size in pixels (Settings may change it)
ZOOM = [0.75]                   # screen pixels per pixel of the desktop's picture (100%: 0.75)

def mouse_to(x, y):
    """Move the pointer to (x, y) of the desktop's picture (where atlas logs things)."""
    w, h = SCREEN
    z = ZOOM[0]
    q("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": int((x * z + 0.5) * 32768 / w)}},
                                  {"type": "abs", "data": {"axis": "y", "value": int((y * z + 0.5) * 32768 / h)}}])
    time.sleep(0.05)

def button(name, down):
    q("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": name}}])
    time.sleep(0.05)

def click(x, y, name="left"):
    mouse_to(x, y)
    button(name, True)
    button(name, False)

def wheel(x, y, n):
    mouse_to(x, y)
    for _ in range(abs(n)):
        b = "wheel-up" if n > 0 else "wheel-down"
        button(b, True)
        button(b, False)

def menu_pick(label, timeout=10):
    """Click the entry `label` of the menu just opened (atlas logs where each entry is)."""
    got = wait(f"atlas: menu entry ", "log", timeout)
    while f" {label} at " not in " " + got:
        got = wait("atlas: menu entry ", "log", timeout)
    x, y = got.split(" at ")[-1].split()[:2]
    click(int(x), int(y))

def drag(x0, y0, x1, y1):
    mouse_to(x0, y0); button("left", True)
    mouse_to((x0 + x1) // 2, (y0 + y1) // 2); mouse_to(x1, y1); button("left", False)

def shot(name):
    """A screenshot: kept as PNG; returns its pixels (from a PPM copy)."""
    ppm = os.path.join(shots, name + ".ppm")
    q("screendump", filename=ppm)
    q("screendump", filename=os.path.join(shots, name + ".png"), format="png")
    with open(ppm, "rb") as f:
        data = f.read()
    os.unlink(ppm)
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

def colours(img, box=None):
    """How many pixels of each colour (in the box, sampled every 4th pixel)."""
    w, h, px = img
    x0, y0, x1, y1 = box or (0, 0, w, h)
    seen = {}
    for y in range(y0, y1, 4):
        row = y * w * 3
        for x in range(x0, x1, 4):
            c = px[row + x * 3: row + x * 3 + 3]
            seen[c] = seen.get(c, 0) + 1
    return seen

def has_colour(img, rgb, box=None, at_least=10):
    return colours(img, box).get(bytes.fromhex(rgb), 0) >= at_least

def keys(*names):
    q("send-key", keys=[{"type": "qcode", "data": n} for n in names])
    time.sleep(0.05)

def drain(t=0.4):
    global full
    time.sleep(t)
    while select.select([qemu.stdout], [], [], 0.1)[0]:
        full += qemu.stdout.read1(4096)

def latest(prefix, name=None):
    """The place last logged by "atlas: dbg PREFIX NAME at X Y" (with /tmp/atlas.debug) -> (x, y)."""
    drain()
    text = full.decode(errors="replace")
    k = text.rfind(f"atlas: dbg {prefix} {name} at " if name else f"atlas: dbg {prefix} at ")
    if k < 0:
        fail(f"no place logged for {prefix} {name}")
    x, y = text[k:].split(" at ", 1)[1].split()[:2]
    return int(x), int(y)

def shown(w=None):
    """Wait for the next window shown (or window w) -> its id."""
    while True:
        got = wait("atlas: window ", "log", 20)
        if got.split()[1:2] == ["shown"] and (w is None or int(got.split()[0]) == w):
            return int(got.split()[0])

def window(w):
    """The last place logged for window w -> (x0, y0, x1, y1)."""
    drain()
    text = full.decode(errors="replace")
    k = text.rfind(f"atlas: dbg window {w} at ")
    if k < 0:
        fail(f"no place logged for window {w}")
    return tuple(int(v) for v in text[k:].split(" at ", 1)[1].split()[:4])

def term_geo(chan):
    """Where terminal chan's text starts, and its cell size (logged when settled)."""
    drain()
    k = full.rfind(f"atlas: terminal {chan} at ".encode())
    if k < 0:
        fail(f"no place logged for terminal {chan}")
    v = full[k:].split(b"\n")[0].decode().split()
    return int(v[4]), int(v[5]), int(v[7]), int(v[8])

def settle_log():
    """Make atlas log where things are (a command in the focused terminal: it settles)."""
    type_text("\n")
    time.sleep(0.6)

def ps_uid(pid):
    """The owner (uid) of process `pid`, from ps on the serial console."""
    send("ps")
    expect("NAME")
    drain(0.8)
    for l in full[pos["con"]:].decode(errors="replace").splitlines():
        if l.split()[:1] == [str(pid)]:
            return l.split()[2]
    fail(f"process {pid} not in ps")

def opened():
    """Wait for the next terminal to open -> its number."""
    got = wait("atlas: terminal ")
    while "opened" not in got:
        got = wait("atlas: terminal ")
    return int(got.split()[0])

def term_pid(chan):
    k = full.rfind(f"atlas: terminal {chan} opened (pid ".encode())
    return int(full[k:].split(b"(pid ")[1].split(b")")[0])

def look(rgb, kind=0, scan=False):
    """A colour atlas draws -> what the screen shows in look `kind` (0 Phosphor,
    1 Amber 1984, 2 Glass; draw.c's draw_look), on a row or a scanline row."""
    if kind == 2:
        return rgb
    r, g, b = (int(rgb[i:i + 2], 16) for i in (0, 2, 4))
    v = min(255, ((r * 77 + g * 150 + b * 29) >> 8) * 5 // 4)
    if scan:
        v = v * (140 + 116 * v // 255) // 256 if kind == 0 else v * (180 + 76 * v // 255) // 256   # (bloom)
    t = (0x46, 0xff, 0x7a) if kind == 0 else (0xff, 0xb0, 0x38)
    return "".join(f"{c * v // 255:02x}" for c in t)

RAW = dict(NAVY="0a0f1c", ACC="4be3c1", TERM="070b14", TEXT="e6ecf5", TERMFG="9fe8c8", GLASS="141a28", GRID="111829")
NAVY, ACC, TERM, TEXT, TERMFG, GLASS, GRID = (look(RAW[k]) for k in ("NAVY", "ACC", "TERM", "TEXT", "TERMFG", "GLASS", "GRID"))   # (Phosphor: the default)

# ---- first start: the accounts are made on the screen
start = time.time()
wait(f"mk: {cpus} CPU")
wait("atlas: first start: accounts needed (setup on the screen)")
expect("First start: on the screen once it has a keyboard")    # the serial console only says so
got = wait("atlas: greeter (1920x1080, full frame ")
greeter_us = int(got.split()[0])
print(f"ok: first start on the screen ({time.time() - start:.2f} s after start, full frame {greeter_us} us)")

img = shot("01-setup-root")
if not (has_colour(img, NAVY, at_least=10000) and has_colour(img, ACC, (500, 200, 1420, 880), 50)):
    fail("the welcome screen does not look right")
type_text("short\nshort\n")                       # too short: refused, says why
wait("atlas: setup: not yet: The password needs at least 8 characters.")
keys("shift", "tab")                               # back to the first field
for _ in range(5): type_text("\b")
type_text("rootpass1\t")
for _ in range(5): type_text("\b")
type_text("rootpass2\n")                          # they differ
wait("atlas: setup: not yet: The two passwords differ.")
type_text("\b1\n")
wait("atlas: setup: step 2")
type_text("Alice\tAlice Example\talicepass1\talicepass1\n")    # upper case: refused
wait("atlas: setup: not yet: Login name:")
keys("shift", "tab"); keys("shift", "tab"); keys("shift", "tab")
for _ in range(5): type_text("\b")
type_text("alice")
time.sleep(0.5)
shot("02-setup-user")
type_text("\t\t\t\n")
wait("atlas: setup: step 3")
click(SCREEN[0] / ZOOM[0] / 2 + 20, SCREEN[1] / ZOOM[0] / 2 + 271)   # "Add another user" (the dialog is centred)
wait("atlas: setup: another user")
type_text("bob\tBob Example\tbobpass11\tbobpass11\n")
wait("atlas: setup: step 3")
time.sleep(0.3)
shot("03-setup-ready")
type_text("\n")                                    # "Create accounts"
wait("atlas: setup: accounts created (root and 2 user(s))", "log", 20)
wait("atlas: login alice: ok", "log", 20)
t1 = 1
w1 = shown()
expect("login: ")                                  # the serial console's prompt, now that accounts exist
time.sleep(0.8)
img = shot("04-logged-in")
if not has_colour(img, TERM, at_least=20000) or not has_colour(img, TERMFG, at_least=20) or not has_colour(img, ACC, at_least=50):
    fail("no terminal window after the setup")
print("ok: accounts made on the screen (root, alice, bob; bad ones refused); alice's first terminal window")

# ---- root on the serial console: places logged (tests only), the stand-in assistant
send("root")
expect("Password: "); send("rootpass1")
expect("root@sieos:")
send("echo 1 > /tmp/atlas.debug")
REAL = os.environ.get("SIA_REAL") == "1"     # gui-test-real: the real sia with a model on the disk
if REAL:
    stub = False
else:
    send("svc stop sia")                    # (no model on the test disk: the stand-in answers the
    time.sleep(0.5)                         #  same way, word by word)
    send("siastub &")
    stub = "ready" in wait("siastub: ", "con", 10)

# ---- a file from alice's shell
type_text("echo hello from the desktop > note.txt\n")
wait("atlas: files: ")
type_text("tag note.txt topic=notes\n")
time.sleep(0.5)
img = shot("05-desktop")
if not has_colour(img, GRID, at_least=2000) or not has_colour(img, GLASS, (1600, 16, 1904, 400), 200):
    fail("no desk grid or no Deskbar")
print("ok: note.txt made by alice's shell; the desk and the Deskbar are drawn")

# ---- windows: a second terminal, moved, resized, zoomed, minimized, sent away, closed
keys("ctrl", "t")
c2 = opened()
w2 = shown()
if ps_uid(term_pid(c2)) != "1000":
    fail(f"terminal {c2} does not run as alice")
time.sleep(0.4)
x0, y0, x1, y1 = window(w2)
tx, ty = latest("wtab", f"Terminal {c2}")
drag(tx, ty, tx + 150, ty + 80)
got = wait(f"atlas: window {w2} at ")
a = [int(v) for v in got.split()[:4]]
if (a[0] - x0, a[1] - y0) != (150, 80):
    fail(f"moved by {(a[0] - x0, a[1] - y0)}, not (150, 80)")
x0, y0, x1, y1 = a
drag(x1 - 10, y1 - 10, x1 - 210, y1 - 110)         # the grip, bottom right
got = wait(f"atlas: window {w2} at ")
b = [int(v) for v in got.split()[:4]]
if (b[2] - b[0], b[3] - b[1]) != (x1 - x0 - 200, y1 - y0 - 100):
    fail(f"resized to {b}, from {(x0, y0, x1, y1)}")
print(f"ok: terminal {c2} (as alice) moved by its tab (+150, +80), resized by its grip (-200, -100)")
# past the screen's sides: the window may go, its tab stays wholly on the screen
x0, y0, x1, y1 = b
settle_log()
tx, ty = latest("wtab", f"Terminal {c2}")
LW, LH = round(SCREEN[0] / ZOOM[0]), round(SCREEN[1] / ZOOM[0])   # (the desktop's picture)
drag(tx, ty, LW - 5, LH - 5)
got = [int(v) for v in wait(f"atlas: window {w2} at ").split()[:4]]
if got[2] <= LW or got[3] <= LH or got[1] > LH - 8:
    fail(f"not past the bottom right: {got}")
settle_log()
tx, ty = latest("wtab", f"Terminal {c2}")
if not (0 < tx < LW and TAB // 2 <= ty <= LH - TAB // 2):
    fail(f"the tab left the screen: at {tx} {ty}")
drag(tx, ty, 5, ty)
got = [int(v) for v in wait(f"atlas: window {w2} at ").split()[:4]]
settle_log()
tx2, ty2 = latest("wtab", f"Terminal {c2}")
if got[0] >= 0 or tx2 - tx < -LW or tx2 < 0:
    fail(f"not past the left side, or the tab off it: {got}, tab at {tx2}")
drag(tx2, ty2, tx2 + (x0 - got[0]), ty2 + (y0 - got[1]))       # back where it was
got = [int(v) for v in wait(f"atlas: window {w2} at ").split()[:4]]
if got != b:
    fail(f"not back: {got}, not {b}")
print("ok: past the bottom right and the left side, its tab always on the screen (slid along at the left)")
settle_log()
click(*latest("wzoom", str(w2)))
wait(f"atlas: window {w2} zoomed")
time.sleep(0.4)
img = shot("06-zoomed")
click(*latest("wzoom", str(w2)))
wait(f"atlas: window {w2} unzoomed")
time.sleep(0.3)
tx, ty = latest("wtab", f"Terminal {c2}")
click(tx, ty); time.sleep(0.1); click(tx, ty)      # double-click the tab: minimized
wait(f"atlas: window {w2} minimized")
click(*latest("dbapp", "Terminal"))                 # the Deskbar brings it back
shown(w2)
print("ok: zoom and back; a double-click on the tab minimized it; the Deskbar's Terminal brought it back")
time.sleep(0.4)
tx, ty = latest("wtab", f"Terminal {c2}")
click(tx, ty, "right")
wait(f"atlas: menu Terminal {c2}")
menu_pick("Send to workspace 2")
wait(f"atlas: window {w2} to workspace 2")
click(*latest("ws", "2"))
wait("atlas: workspace 2")
time.sleep(0.4)
img = shot("07-workspace-2")
settle_log()
click(*latest("wclose", str(w2)))                  # close: its shell ends
got = wait("atlas: terminal ")
while "closed" not in got:
    got = wait("atlas: terminal ")
if int(got.split()[0]) != c2:
    fail(f"closed the wrong terminal: {got}")
time.sleep(0.4)
shot("07b-empty-workspace")                        # (workspace 2, empty: the resting clock)
click(*latest("ws", "1"))
wait("atlas: workspace 1")
keys("ctrl", "tab")
time.sleep(0.4)
print(f"ok: right-click the tab -> Send to workspace 2, workspace 2 shown, the close box ended terminal {c2}")

# ---- a terminal's history and copy / paste
settle_log()
tx, ty, cw, ch = term_geo(t1)
type_text("help\n")
type_text("ls -l /bin\n")
time.sleep(0.8)
keys("pgup")
got = wait(f"atlas: terminal {t1} back ")
if int(got.split()[0]) <= 0:
    fail(f"no history to scroll back to: {got}")
keys("pgdn")
back = got.split()[0]
wait(f"atlas: terminal {t1} back 0")
type_text("clear\n")
time.sleep(0.4)
settle_log()
tx, ty, cw, ch = term_geo(t1)
cmd = "ln note.txt pasted.txt"
type_text(f"clear\n")
time.sleep(0.4)
type_text(f"echo {cmd}\n")                        # its output, on the second line, is a command
time.sleep(0.6)
y = ty + ch + ch // 2
mouse_to(tx + cw // 2, y); button("left", True)
mouse_to(tx + 10 * cw, y); mouse_to(tx + (len(cmd) - 1) * cw + cw // 2, y); button("left", False)
got = wait("atlas: copied ")
if not got.startswith(f"{len(cmd)} bytes"):
    fail(f"the selection: {got}")
shot("08-selection")
drain()
k = full.rfind(b"atlas: files: ")
before = int(full[k + 14:].split()[0])
keys("ctrl", "v")
wait(f"atlas: pasted {len(cmd)} bytes")
type_text("\n")
got = wait("atlas: files: ")
while int(got.split()[0]) == before:
    got = wait("atlas: files: ")
if int(got.split()[0]) != before + 1:
    fail(f"the pasted command did not make pasted.txt: {before} -> {got}")
print(f"ok: history (PgUp went back {back} lines), selected {len(cmd)} bytes, pasted with Ctrl+V, it ran")

# ---- Ctrl+C in a terminal: the program it runs ends, the shell takes the next command
type_text("ktest hang spin\n")
time.sleep(0.8)
drain()
k = full.rfind(b"atlas: files: ")
before = int(full[k + 14:].split()[0])
keys("ctrl", "c")
time.sleep(0.3)
type_text("touch ctrlc.txt\n")
got = wait("atlas: files: ")
while int(got.split()[0]) == before:
    got = wait("atlas: files: ")
send("ps")
expect("NAME")
drain(0.8)
if " ktest" in full[pos["con"]:].decode(errors="replace"):
    fail("ktest still runs after Ctrl+C")
print("ok: Ctrl+C in a terminal ended its program; the shell ran the next command")

# ---- Files and the editor
click(*latest("dbapp", "Files"))
wait("atlas: files /home/alice: ")
wf = shown()
time.sleep(0.4)
shot("09-files")
for k, line in enumerate(("/* a demo */", "int main(void)", "{", "    return 42;", "}")):   # (a C file, as root)
    send(f'echo "{line}" {">" if k == 0 else ">>"} /home/alice/demo.c')
    time.sleep(0.3)
time.sleep(0.3)
fx, fy = latest("file", "note.txt")
click(fx, fy); time.sleep(0.1); click(fx, fy)      # double-click: the editor
wait("atlas: editing note.txt")
type_text("edited on the desktop\n")
keys("ctrl", "s")
got = wait("atlas: saved note.txt ")
keys("end"); time.sleep(0.6)                      # (a redraw: the places logged; no change)
click(*latest("etree", "demo.c"))                  # the tree: a second file, in a second tab
wait("atlas: editing demo.c")
time.sleep(0.3)
keys("end"); time.sleep(0.6)                      # (a redraw: the places logged; no change)
click(*latest("etab", "note.txt"))                 # the first tab again ...
time.sleep(0.2)
click(*latest("etab", "demo.c"))                   # ... and the second
time.sleep(0.4)
shot("10-editor")
keys("end"); time.sleep(0.6)                      # (a redraw: the places logged; no change)
click(*latest("etabx", "demo.c"))                  # its x closes it
wait("atlas: editor closed demo.c")
keys("esc")                                        # Esc: the last tab, and the window
wait("atlas: editor closed note.txt")
send("cat /home/alice/note.txt")
expect("edited on the desktop")
click(*latest("dbapp", "Files"))                   # (in front again: its places logged)
time.sleep(0.4)
click(*latest("fileup"))
wait("atlas: files /home: ")
time.sleep(0.4)
click(*latest("crumb", "-1"))                      # the path's first part: the top folder
wait("atlas: files /: ")
# snapshots: one of alice's home (a volume of its own) taken, a file changed,
# the old version still in the snapshot; Files opens the volume's .snapshots
send("snap take before /home/alice")
expect("snapshot before taken")
send("echo changed after the snapshot > /home/alice/note.txt")
time.sleep(0.3)
click(*latest("filesnap"))                          # Files, at "/": that volume's hidden .snapshots
wait("atlas: files /.snapshots: ")
send("cat /home/alice/.snapshots/before/note.txt")
expect("edited on the desktop")
send("cat /home/alice/note.txt")
expect("changed after the snapshot")
send("snap list /home/alice")
expect("before")
send("snap list /")                                 # "/": the hourly one, taken when the file server started
expect("auto-")
send("snap delete before /home/alice")
expect("snapshot before deleted")
send("echo edited on the desktop > /home/alice/note.txt")
print(f"ok: Files listed the home folder; note.txt opened by a double-click, edited, saved ({got.strip()}); demo.c from the tree in a second tab; tabs shown and closed; Up; the path's / part; a snapshot taken, browsed in Files, read, deleted")

# ---- the editor on its own: an untitled tab, a name asked only when saved,
# in the folder last clicked in its tree
send("mkdir /home/alice/docs")                     # (as root: then given to alice)
time.sleep(0.3)
send("chown 1000 /home/alice/docs")
time.sleep(0.5)
click(*latest("dbapp", "Text editor"))
wait("atlas: editing untitled")
time.sleep(0.5)
type_text("written before it had a name")
click(*latest("etree", "docs"))
wait("atlas: editor folder /home/alice/docs")
time.sleep(0.3)
keys("ctrl", "s")
wait("atlas: prompt Save in /home/alice/docs as:")
type_text("later.txt\n")
wait("atlas: save as later.txt (0)")
wait("atlas: saved later.txt 28 bytes")
send("cat /home/alice/docs/later.txt")
expect("written before it had a name")
keys("ctrl", "w")                                  # (saved: closes at once, and the window with it)
wait("atlas: editor closed later.txt")
print("ok: the editor alone: an untitled tab; the docs folder clicked; Ctrl+S asked for a name, saved docs/later.txt (28 bytes)")

# ---- Pins: the GPIO pins as sequencer tracks (simulated pins under QEMU)
click(*latest("dbapp", "Pins"))
got = wait("atlas: pins open: ")
time.sleep(0.5)
click(*latest("pins", "1004"))                     # track 1 (BCM 17), step 5: off (the demo had it on)
wait("atlas: pins: BCM 17 step 5 off")
time.sleep(0.3)
click(*latest("pins", "128"))                      # track 3 (BCM 22): an input (pull-up)
wait("atlas: pins: BCM 22 in")
time.sleep(0.3)
click(*latest("pins", "1"))                        # Play: the server plays the outputs, records the inputs
wait("atlas: pins: playing 16 steps of 100 ms on ")
time.sleep(2.0)
wait("atlas: pins: recorded BCM 22 1111111111111111")   # (simulated pins: an input reads its pull at every step)
wait("atlas: pins: recorded BCM 27 1111111111111111")
img = shot("31-pins")
send("gpio")                                       # (the server, from the console: BCM 17 is an output)
expect(" 17  out")
click(*latest("pins", "1"))                        # Stop
wait("atlas: pins: stopped")
keys("ctrl", "w")
wait("atlas: pins closed")
print(f"ok: Pins: {got.split(',')[0]}; a step set, a track made an input; played by the GPIO server, the inputs recorded at each step; stopped; closed")

# ---- Git: a repository made in alice's terminal, chosen in Files; the Git app
# shows its changes and a diff, stages, commits (/bin/git runs as alice)
click(*latest("dbapp", "Terminal"))
time.sleep(0.5)
for c in ("mkdir proj", "cd proj", "git init", "echo first line > a.txt", "echo kept > b.txt", "git add -A",
          "git commit -m First commit", "echo second line >> a.txt", "echo new > c.txt", "echo staged >> b.txt",
          "git add b.txt", "cd"):
    type_text(c + "\n")
    time.sleep(0.5)
send("git -C /home/alice/proj status -s")
expect("?? c.txt")
click(*latest("dbapp", "Files"))
time.sleep(0.4)
click(*latest("crumb", "-1"))                      # Files at the top, then home, alice, proj
wait("atlas: files /: ")
for d, path in (("home", "/home"), ("alice", "/home/alice"), ("proj", "/home/alice/proj")):
    time.sleep(0.4)
    fx, fy = latest("file", d)
    click(fx, fy); time.sleep(0.1); click(fx, fy)
    wait(f"atlas: files {path}: ")
keys("ctrl", "spc")
type_text("git")
time.sleep(0.3)
type_text("\n")
wait("atlas: git: open /home/alice/proj")
got = wait("atlas: git: /home/alice/proj: branch ")
if "main, 3 changes" not in got:
    fail(f"the Git app's status: {got}")
wait("atlas: git: log 1 commits")
time.sleep(0.4)
click(*latest("gitfile", "a.txt"))
got = wait("atlas: git: diff a.txt (")
time.sleep(0.4)
img = shot("32-git-changes")
click(*latest("git", "Stage"))
wait("atlas: git: add -> 0")
wait("atlas: git: /home/alice/proj: branch main, 3 changes")
time.sleep(0.4)
click(*latest("gitfile", "c.txt"))
time.sleep(0.4)
click(*latest("git", "Stage"))
wait("atlas: git: add -> 0")
time.sleep(0.4)
click(*latest("git", "field"))
type_text("Second from the Git app")
time.sleep(0.3)
click(*latest("git", "Commit"))
wait("atlas: git: commit -> 0")
wait("atlas: git: /home/alice/proj: branch main, 0 changes")
wait("atlas: git: log 2 commits")
time.sleep(0.5)
shot("33-git-log")
click(*latest("git", "Push"))                        # (no remote here: git says so, the window shows it)
wait("atlas: git: push -> 1")
send("git -C /home/alice/proj log --oneline")
expect("Second from the Git app")
keys("ctrl", "w")
wait("atlas: git closed")
print(f"ok: Git: a repository from Files; 3 changes, a.txt's diff ({got.strip()}); a.txt and c.txt staged; committed as alice; Push without a remote refused")

# ---- the sia strip: a file, an app, a question
keys("ctrl", "spc")
wait("atlas: strip")
type_text("note")
time.sleep(0.4)
shot("11-strip")
type_text("\n")
wait("atlas: editing note.txt")
keys("esc")
wait("atlas: editor closed note.txt")
keys("ctrl", "spc")
type_text("hello")
time.sleep(0.3)
type_text("\n")
got = wait("atlas: app Hello started (terminal ")
ch_app = int(got.split(")")[0])
if ps_uid(term_pid(ch_app)) != "1000":
    fail("the app does not run as alice")
keys("ctrl", "spc")
type_text("zzz")
keys("esc")                                        # Esc: the results close, nothing opens
time.sleep(0.3)
print(f"ok: strip: 'note' opened the file, 'hello' started the Hello app (terminal {ch_app}, as alice), Esc closed it")

keys("ctrl", "spc")
type_text("show me my note")
type_text("\t")                                    # Tab: ask sia
wait("atlas: ask from the strip")
wait("atlas: assistant open")
wait("atlas: sia: ask")
which, got = wait_any(["atlas: sia: done ", "atlas: sia: error "], "log", 120)
if stub and "done" not in which:
    fail(f"the stand-in did not answer: {got}")
time.sleep(0.4)
img = shot("12-assistant")
print(f"ok: strip -> Ask sia: {which.split(': ')[-1].strip()} {got.strip()}")
if stub:
    if "2 actions" not in got:
        fail(f"the answer's actions were not found: {got}")
    b0 = wait("atlas: sia: button 0 at ").split()
    click(int(b0[0]), int(b0[1]))
    if not wait("atlas: sia: action fly ").strip().endswith("-> 0"):
        fail("the 'Open note.txt' button did not find the file")
    time.sleep(0.4)
    shot("13-asked-open")
    keys("esc")
    wait("atlas: editor closed note.txt")
    drain()
    k = full.rfind(b"atlas: sia: button 1 at ")
    b1 = full[k + 24:].split()[:2]
    click(int(b1[0]), int(b1[1]))
    got = wait("atlas: sia: action find ")
    if not got.strip().endswith("-> 1"):
        fail(f"the 'topic=notes' button: {got}")
    print("ok: the answer's buttons: 'Open note.txt' (the editor), the files with topic=notes (1, under the strip)")
    keys("esc")
    keys("ctrl", "spc")
    type_text("please count slowly\t")
    wait("atlas: sia: first piece", "log", 30)
    keys("esc")                                    # Esc in the assistant: stop the answer
    got = wait("atlas: sia: stopped after ", "log", 20)
    print(f"ok: Esc stopped a long answer after {got.split()[0]} pieces")

# ---- Settings: the mouse, the screen's size, the assistant's model
click(*latest("dbapp", "Settings"))
rest = wait("atlas: settings open")
if "wifi none" not in rest:
    fail(f"Settings did not report no Wi-Fi (QEMU has none): {rest!r}")
print("ok: Settings: Network shows 'No Wi-Fi on this machine.' (none in QEMU)")
time.sleep(0.5)
click(*latest("set", "32"))                        # pointer speed: Fast
wait("atlas: mouse speed 150 swap 0 reverse 0")
click(*latest("set", "41"))                        # Left-handed: the buttons swap ...
wait("atlas: mouse speed 150 swap 1 reverse 0")
click(*latest("set", "40"), "right")               # ... so the right button now clicks
wait("atlas: mouse speed 150 swap 0 reverse 0")
send("cat /home/alice/.mouse")
expect("speed=150"); expect("swap=0")
print("ok: Settings: pointer speed Fast, left-handed (then back with the other button), kept in ~/.mouse")

click(*latest("set", "101"))                       # the look: Amber 1984 ...
wait("atlas: look 1")
time.sleep(0.5)
img = shot("27-look-amber")
if not has_colour(img, look(RAW["ACC"], 1), at_least=50) or has_colour(img, ACC, at_least=50):
    fail("Amber 1984 is not on the screen")
settle_log()
click(*latest("set", "102"))                       # ... Glass: atlas's own colours ...
wait("atlas: look 2")
time.sleep(0.5)
img = shot("28-look-glass")
if not has_colour(img, RAW["ACC"], at_least=50) or not has_colour(img, RAW["GLASS"], at_least=2000):
    fail("Glass is not on the screen")
settle_log()
click(*latest("set", "103"))                       # ... BeOS: its own colours ...
wait("atlas: look 3")
time.sleep(0.4)
img = shot("28b-look-beos")
if not has_colour(img, "336698", at_least=2000) or not has_colour(img, "ffcb00", at_least=50) or has_colour(img, RAW["GLASS"], at_least=50):
    fail("BeOS (its blue desk, its yellow tab) is not on the screen")
settle_log()
click(*latest("set", "100"))                       # ... and Phosphor again
wait("atlas: look 0")
send("cat /home/alice/.look")
expect("look=0")
time.sleep(0.4)
img = shot("29-look-phosphor")
if not has_colour(img, ACC, at_least=50) or not has_colour(img, look(RAW["NAVY"], 0, True), at_least=2000):
    fail("Phosphor (and its scanlines) is not on the screen")
print("ok: Settings: the look: Amber 1984, Glass, BeOS, then Phosphor again, kept in ~/.look")

settle_log()
click(*latest("set", "115"))                       # everything at 200% ...
wait("atlas: zoom 200% (1280 x 720)")
ZOOM[0] = 1.5
time.sleep(0.6)
img = shot("30-zoom-200")
settle_log()
click(*latest("set", "112"))                       # ... and 100% again (a click at 200% lands)
wait("atlas: zoom 100% (2560 x 1440)")
ZOOM[0] = 0.75
send("cat /home/alice/.look")
expect("zoom=100")
time.sleep(0.4)
settle_log()
print("ok: Settings: everything at 200%, then 100% again (clicks land at both), kept in ~/.look")

click(*latest("set", "12"))                        # 1600 x 900
wait("atlas: screen size 1600 x 900 (back to 1920 x 1080 in 15 s unless kept)")
SCREEN[:] = [1600, 900]
time.sleep(0.6)
img = shot("24-settings-1600")
if img[:2] != (1600, 900):
    fail(f"the screen is {img[:2]}, not 1600 x 900")
click(*latest("set", "90"))                        # Keep
wait("atlas: screen size 1600 x 900 kept")
send("cat /etc/display.conf")
expect("size=1600x900")
click(*latest("set", "11"))                        # 1366 x 768, then "Go back"
wait("atlas: screen size 1366 x 768")
SCREEN[:] = [1366, 768]
time.sleep(0.4)
click(*latest("set", "91"))
wait("atlas: screen size back to 1600 x 900")
SCREEN[:] = [1600, 900]
time.sleep(0.4)
click(*latest("set", "13"))                        # 1920 x 1080 again, kept
wait("atlas: screen size 1920 x 1080")
SCREEN[:] = [1920, 1080]
time.sleep(0.4)
click(*latest("set", "90"))
wait("atlas: screen size 1920 x 1080 kept")
if shot("25-settings-1920")[:2] != (1920, 1080):
    fail("the screen did not come back to 1920 x 1080")
print("ok: Settings: screen 1600 x 900 kept (/etc/display.conf), 1366 x 768 then 'Go back', 1920 x 1080 again")

click(*latest("dbapp", "Settings"))                         # (shown again: the rows moved up without "Keep")
time.sleep(0.5)
click(*latest("wzoom", "10"))                      # Settings zoomed: room for the online model's fields
wait("atlas: window 10 zoomed")
time.sleep(0.4)
click(*latest("set", "61"))                        # the assistant: online
time.sleep(0.3)
click(*latest("dbapp", "Settings"))                         # (shown again: where the new fields are)
time.sleep(0.5)
click(*latest("set", "70")); type_text("http://10.0.2.2:9/v1")
click(*latest("set", "71")); type_text("test-model")
click(*latest("set", "72")); type_text("sk-test")
click(*latest("set", "73")); type_text("wrongpass")
click(*latest("set", "80"))
wait("atlas: settings: root's password refused")
click(*latest("set", "73")); type_text("rootpass1\n")
wait("atlas: settings: sia online")
send("cat /etc/sia.conf")
expect("backend=remote"); expect("url=http://10.0.2.2:9/v1"); expect("api_model=test-model")
send("cat /etc/sia.key")
expect("sk-test")
time.sleep(0.4)
shot("26-settings-online")
click(*latest("set", "60"))                        # back to the local model
click(*latest("dbapp", "Settings"))
time.sleep(0.5)
click(*latest("set", "73")); type_text("rootpass1\n")
wait("atlas: settings: sia local")
send("cat /etc/sia.conf")
expect("backend=local")
print("ok: Settings: sia online (URL, model, key; a wrong root password refused), then local again")


# ---- kill the desktop: init restarts it; the login screen
send("svc")
expect("UPTIME")
got = expect("atlas ")
send(f"kill {[w for w in got.split() if w.isdigit()][0]}")
wait("atlas: greeter", "log", 10)
time.sleep(1.0)
type_text("alice\twrong\n")
wait("atlas: login alice: refused", "log", 10)
img = shot("16-login")
if not has_colour(img, ACC, (560, 260, 1360, 820), 50):
    fail("no login card")
send("ps")
expect("NAME")
drain(0.6)
listing = full[pos["con"]:].decode(errors="replace")
if any(l.split()[2:3] == ["1000"] for l in listing.splitlines() if len(l.split()) > 3):
    fail("alice's shells still run after the desktop restarted:\n" + listing)
type_text("alicepass1\n")
wait("atlas: login alice: ok")
print("ok: atlas killed -> init restarted it; old shells ended; wrong password refused; logged in again")
time.sleep(0.8)

# ---- kill the console server (keyboard and mouse): the desktop gets them back
send("svc")
expect("UPTIME")
got = expect("con ")
send(f"kill {[w for w in got.split() if w.isdigit()][0]}")
wait("init: con")
time.sleep(1.0)
drain()
k = full.rfind(b"atlas: files: ")
before = int(full[k + 14:].split()[0])
type_text("echo still typing > after-con.txt\n")
got = wait("atlas: files: ")
while int(got.split()[0]) == before:
    got = wait("atlas: files: ")
print("ok: console server killed and restarted; the keyboard still reaches the desktop")

# ---- measurements: frame times (logged with each settled picture), memory per process
frames = [int(l.split(b"frame ")[1].split()[0]) for l in full.split(b"\n") if b"atlas: dbg frame " in l]
if frames:
    print(f"    frames: {len(frames)} full pictures, median {sorted(frames)[len(frames) // 2]} us, max {max(frames)} us")
send("ps")
expect("NAME")
expect(" siad" if REAL else " siastub")
drain(0.3)
for line in full[pos["con"] - 4000:].decode(errors="replace").splitlines():
    if line.split()[-1:] in (["atlas"], ["con"], ["sh"], ["fs"], ["init"], ["auth"], ["siastub"], ["siad"]):
        print("   ", line.strip())
send("mem")
print("   ", expect("memory:").strip())
send("poweroff")
try:
    qemu.wait(10)
except subprocess.TimeoutExpired:
    fail("no power off")
os.unlink(copy)
print("PASS")
