#!/usr/bin/env python3
"""Test Scriptures (/bin/scriptures, docs/scriptures.md) on the desktop
(make scriptures-test), through QEMU's control socket (QMP): screenshots,
keys and the mouse. The app says what it drew on the serial port (its
"debug" file: debug_sieos.c): its state, its clickable zones, where words
are; clicks go there. Its library comes over HTTPS from a stand-in for the
Church's content API on the host (tests/scriptures/server.py, a test CA).

  first start -> alice: ~/.scriptures/site (the test server) and debug ->
  Scriptures from the sia strip (its .app): the window, the empty library
  -> Settings: Library: download the five standard works (tiny) -> the
  library drawn -> a book, a chapter -> a footnote -> search ("light",
  Ctrl+F) -> a result -> a highlight (a double-clicked word, a colour) ->
  a note on another word (typed, saved) -> the notes tab -> the window
  closed (Ctrl+W): the session saved -> Scriptures again: the tab, the
  highlight and the note back; notes.json read in alice's shell.

Usage: scripturestest.py <cpus> <screenshots dir> <disk image> <host tools dir> <qemu command line...>
The test works in a copy of the image. Screenshots (PNG) are kept.

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import atexit, json, os, re, shutil, socket, subprocess, sys, threading, time
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
sys.path.insert(0, os.path.join(here, "..", "tests", "scriptures"))
import qserial, server
SLOW = float(os.environ.get("SIEOS_SLOW", "1"))
T0 = time.time()
def say(s): print(f"{s} [{time.time() - T0:.0f} s]", flush=True)

cpus, shots, image, tools, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:]
os.makedirs(shots, exist_ok=True)
copy = image + ".scr"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
port, seen = server.start(copy, os.path.join(tools, "siefs"))

# the app's zone numbers (app.h's enum of Z_...)
src = open(os.path.join(here, "..", "user", "scriptures", "app.h")).read()
names = re.sub(r"/\*.*?\*/", "", re.search(r"enum \{\s*Z_NONE(.*?)\};", src, re.S).group(0), flags=re.S)
Z = {n: i for i, n in enumerate(re.findall(r"\b(Z_[A-Z_]+)\b", names))}
SEC_LIBRARY, POP_SELECTION, DLG_NOTE = 1, 2, 1

sock_path = os.path.join(shots, "qmp-scr.sock")
if os.path.exists(sock_path): os.unlink(sock_path)
cmdline += ["-qmp", f"unix:{sock_path},server=on,wait=off"]
qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
atexit.register(lambda: qemu.poll() is None and qemu.kill())
full, pos = b"", {"con": 0, "log": 0}

def fail(msg):
    pull(0.2)
    qemu.kill()
    open(os.path.join(shots, "scriptures-serial.log"), "wb").write(full)
    sys.exit(f"FAIL: {msg}\n--- serial (last part) ---\n{full[-3000:].decode(errors='replace')}\n--- requests ---\n{seen[-10:]}")

# the serial port, read all the time by a thread: the app's frames are many lines, and a
# full pipe would stop QEMU (its control socket too) while the test waits for QMP
chunks, lock = [], threading.Lock()
def reader():
    while True:
        d = qemu.stdout.read1(65536)
        if not d: return
        with lock: chunks.append(d)
threading.Thread(target=reader, daemon=True).start()
def pull(t):
    global full
    time.sleep(t)
    with lock:
        if chunks: full += b"".join(chunks); chunks.clear()

def wait(text, who="log", timeout=30, line=True):
    """Wait for text after reader who's position -> the rest of its line."""
    end = time.time() + timeout * SLOW
    while text.encode() not in full[pos[who]:]:
        if time.time() > end or qemu.poll() is not None: fail(f"waiting for {text!r}")
        pull(0.2)
    i = full.index(text.encode(), pos[who]) + len(text)
    while line and b"\n" not in full[i:] and time.time() < end: pull(0.2)
    e = full.find(b"\n", i)
    pos[who] = i
    return full[i:e if e >= 0 else len(full)].decode(errors="replace")

def drain(t=0.4):
    end = time.time() + t
    while time.time() < end: pull(0.05)

def send(line): qserial.type_line(qemu, line)

def sh(cmd, timeout=60):
    """A command in alice's shell on the serial console -> its output."""
    drain(0.3)
    start = pos["con"] = len(full)
    send(cmd)
    wait("alice@sieos", "con", timeout, line=False)
    return full[start:pos["con"]].decode(errors="replace")

for _ in range(600):
    try:
        qmp = socket.socket(socket.AF_UNIX); qmp.connect(sock_path); break
    except OSError:
        time.sleep(0.05)
else:
    fail("no QMP socket")
qf = qmp.makefile("rw")
json.loads(qf.readline())
def q(cmd, **args):
    qf.write(json.dumps({"execute": cmd, "arguments": args}) + "\n"); qf.flush()
    while True:
        r = json.loads(qf.readline())
        if "return" in r: return r["return"]
        if "error" in r: fail(f"QMP {cmd}: {r['error']}")
q("qmp_capabilities")

KEYS = {" ": "spc", "\n": "ret", "\t": "tab", "\b": "backspace", ".": "dot", "/": "slash", "-": "minus",
        "=": "equal", ",": "comma", ";": "semicolon", "\x1b": "esc", "'": "apostrophe"}
SHIFTED = {"?": "slash", "_": "minus", "+": "equal", ">": "dot", "<": "comma", ":": "semicolon", '"': "apostrophe", "@": "2"}
def type_text(s):
    for c in s:
        k = ["shift", c.lower()] if c.isalpha() and c.isupper() else ["shift", SHIFTED[c]] if c in SHIFTED else [KEYS.get(c, c)]
        q("send-key", keys=[{"type": "qcode", "data": x} for x in k])
        time.sleep(0.03 * SLOW)
def keys(*names):
    q("send-key", keys=[{"type": "qcode", "data": n} for n in names]); time.sleep(0.05)
W, H, ZOOM = 1920, 1080, 0.75               # the screen; screen pixels per pixel of the desktop's picture
def move(x, y):                             # (the desktop's picture)
    q("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": int((x * ZOOM + 0.5) * 32768 / W)}},
                                  {"type": "abs", "data": {"axis": "y", "value": int((y * ZOOM + 0.5) * 32768 / H)}}])
    time.sleep(0.05)
def button(down, which="left"):
    q("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": which}}]); time.sleep(0.05)
def shot(name):
    time.sleep(0.6 * SLOW)
    q("screendump", filename=os.path.join(shots, name + ".png"), format="png")

# ---- the app's window and what it drew
origin = None                               # its insides' top left in the desktop's picture
def place():
    global origin
    drain(0.3)
    w = re.findall(rb"atlas: window (\d+) opened by pid", full)[-1].decode()
    k = full.rfind(f"atlas: dbg window {w} at ".encode())
    if k < 0: fail("no place logged for the window")
    x0, y0 = map(int, full[k:].split(b" at ", 1)[1].split()[:2])
    origin = (x0 + 2, y0 + 34)
def to_pic(ax, ay): return origin[0] + ax / ZOOM, origin[1] + ay / ZOOM
def click(ax, ay, n=1, which="left"):
    move(*to_pic(ax, ay))
    for _ in range(n): button(True, which); button(False, which)
    time.sleep(0.2)

frame = {}
def frame_now(timeout=20, want=None):
    """The last frame the app described (after the last wait): its state, zones, words.
    want(state) -> True: wait for such a frame."""
    end = time.time() + timeout * SLOW
    while True:
        drain(0.3)
        text = full[pos["log"]:].decode(errors="replace")
        frames = re.findall(r"scriptures: frame (\d+) (\d+)x(\d+) (.*?)\n(.*?)scriptures: end", text, re.S)
        if frames:
            n, w, h, st, rest = frames[-1]
            head, _, focus = st.partition(" focus=")                 # (the focused field's text: last, may hold spaces)
            state = dict(kv.split("=", 1) for kv in head.split() if "=" in kv)
            state["focus"] = focus.strip()
            zones = [tuple(map(int, z)) for z in re.findall(r"scriptures: zone (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)", rest)]
            words = {(a, int(k)): (int(x), int(y)) for a, k, x, y in re.findall(r"scriptures: word (\S+) (\d+) (-?\d+) (-?\d+)", rest)}
            f = {"n": int(n), "w": int(w), "h": int(h), "state": state, "zones": zones, "words": words}
            if not want or want(f["state"]):
                frame.clear(); frame.update(f)
                return f
        if time.time() > end: fail(f"no frame as wanted (last: {frames[-1][3] if frames else 'none'})")
        time.sleep(0.3)
def zone(name, a=None, f=None):
    """The centre of zone Z_name (argument a) in the last frame."""
    for z in (f or frame)["zones"]:
        if z[0] == Z[name] and (a is None or z[1] == a):
            return (z[2] + z[4]) // 2, (z[3] + z[5]) // 2
    fail(f"no zone {name} {a} (zones: {sorted(set(z[0] for z in (f or frame)['zones']))})")
def press(name, a=None, n=1):
    frame_now()
    pos["log"] = len(full)
    click(*zone(name, a), n)

def count(png, rgb, box):
    """Pixels of colour rgb (hex) in box (screen pixels), from a screenshot, every 2nd one."""
    ppm = png[:-4] + ".ppm"
    q("screendump", filename=ppm)
    data = open(ppm, "rb").read(); os.unlink(ppm)
    p = data.split(b"\n", 3); w, h = map(int, p[1].split()); px = p[3]
    want = bytes.fromhex(rgb)
    x0, y0, x1, y1 = (max(0, int(v)) for v in box)
    return sum(px[(y * w + x) * 3:(y * w + x) * 3 + 3] == want for y in range(y0, min(y1, h), 2) for x in range(x0, min(x1, w), 2))
def light(png):
    """The desktop's light grounds (glass's panels) in the window's insides (screen pixels), every 3rd pixel."""
    x0, y0 = origin[0] * ZOOM, origin[1] * ZOOM
    ppm = os.path.join(shots, "poll.ppm")
    q("screendump", filename=ppm)
    data = open(ppm, "rb").read(); os.unlink(ppm)
    p = data.split(b"\n", 3); w, h = map(int, p[1].split()); px = p[3]
    n = 0
    for y in range(int(y0) + 4, min(int(y0) + frame["h"], h), 3):
        for x in range(int(x0) + 4, min(int(x0) + frame["w"], w), 3):
            r, g, b = px[(y * w + x) * 3:(y * w + x) * 3 + 3]
            n += r > 200 and g > 200 and b > 220
    return n

# ---- first start: accounts, alice on the screen and on the serial console
wait("atlas: first start: accounts needed", timeout=60)
wait("atlas: greeter (")
time.sleep(0.5)
type_text("rootpass1\trootpass1\n"); wait("atlas: setup: step 2")
type_text("alice\tAlice Example\talicepass1\talicepass1\n"); wait("atlas: setup: step 3")
type_text("\n"); wait("atlas: login alice: ok", timeout=60)
wait("login: ", "con", 30, line=False)
send("alice"); wait("Password: ", "con", line=False); send("alicepass1"); wait("alice@sieos", "con", 30, line=False)
sh("echo 1 > /tmp/atlas.debug")
sh("mkdir /home/alice/.scriptures")
sh(f"echo https://10.0.2.2:{port} > /home/alice/.scriptures/site")
sh("echo 1 > /home/alice/.scriptures/debug")
say("ok: first start; alice's ~/.scriptures points at the test server")

# ---- Scriptures from the sia strip
pos["log"] = len(full)
keys("ctrl", "spc"); wait("atlas: strip")
type_text("scriptures"); time.sleep(0.6); type_text("\n")
wait("atlas: window 17 opened by pid", timeout=30)
f = frame_now(30)
time.sleep(1); place()
shot("scr-01-empty")
if light("") < 400: fail("the window shows little of the desktop's light colours")
say(f"ok: the window opened ({f['w']} x {f['h']}), the empty library drawn")

# ---- Settings, Library: the standard works (tiny) from the test server
press("Z_SETTINGS")
press("Z_S_SECTION", SEC_LIBRARY)
frame_now()
shot("scr-02-settings")
press("Z_S_DOWNLOAD")
f = frame_now(120, lambda s: s.get("dl") == "0" and int(s.get("vols", 0)) == 5)
press("Z_S_CLOSE") if any(z[0] == Z["Z_S_CLOSE"] for z in frame["zones"]) else (keys("esc"), time.sleep(0.5))
frame_now(10, lambda s: s.get("dlg") == "0")
shot("scr-03-library")
got = sorted(set(u for p, u, a in seen if p.endswith("/content")))
if len(got) < 6 or not all("SIEOS" in a for p, u, a in seen): fail(f"the requests: {seen}")
say(f"ok: downloaded over HTTPS: 5 volumes, {len(got)} chapters; the library drawn")

# ---- a book, a chapter
press("Z_VOLUME", 0)                        # the Old Testament
f = frame_now()
if any(z[0] == Z["Z_GROUP"] for z in f["zones"]): press("Z_GROUP", 0)
frame_now()
press("Z_CHAPTER")                          # Genesis 1
f = frame_now(20, lambda s: s.get("uri") == "/scriptures/ot/gen/1")
if ("p3", 1) not in f["words"]: fail(f"no verses laid out: {f['words']}")
shot("scr-04-chapter")
say("ok: Genesis 1 open from the library")

# ---- a footnote: "beginning" (verse 1's word 2), its link to John 1
pos["log"] = len(full)
click(*f["words"]["p1", 2])
f = frame_now(10, lambda s: s.get("pop") == "1")
shot("scr-04b-footnote")
press("Z_FNLINK")
f = frame_now(20, lambda s: s.get("uri") == "/scriptures/nt/john/1" and s.get("pop") == "0")
say("ok: a footnote, its link followed (John 1)")

# ---- search: Ctrl+F (now the window's), "light"
pos["log"] = len(full)
keys("ctrl", "f"); time.sleep(0.3)
type_text("light\n")
f = frame_now(20, lambda s: s.get("pages") == "2" and s.get("focus") == "-")
hits = [z for z in f["zones"] if z[0] == Z["Z_HIT"]]
if len(hits) < 3: fail(f"search 'light': {len(hits)} results")
shot("scr-05-search")
press("Z_HIT", hits[0][1])                  # the first: Genesis 1:3 (in the library's order)
f = frame_now(20, lambda s: s.get("uri") == "/scriptures/ot/gen/1")
say(f"ok: search: {len(hits)} verses with 'light'; one opened (Genesis 1)")

# ---- a highlight: a word double-clicked, a colour
f = frame_now()
x, y = f["words"]["p4", 1]                  # "God" of verse 4
pos["log"] = len(full)
click(x, y, 2)
f = frame_now(10, lambda s: s.get("pop") == str(POP_SELECTION))
shot("scr-06-selection")
press("Z_SWATCH", 2)
f = frame_now(10, lambda s: s.get("notes") == "1")
shot("scr-07-highlight")
say("ok: a highlight (a double-clicked word, a colour)")

# ---- a note on another word
x, y = f["words"]["p1", 1]                  # "the" of verse 1
pos["log"] = len(full)
click(x, y, 2)
frame_now(10, lambda s: s.get("pop") == str(POP_SELECTION))
press("Z_POP_NOTE")
f = frame_now(10, lambda s: s.get("dlg") == str(DLG_NOTE))
fields = sorted((z for z in f["zones"] if z[0] == Z["Z_FIELD"]), key=lambda z: z[3])   # title, tags, note
if len(fields) < 3: fail(f"the note's fields: {fields}")
pos["log"] = len(full)
click((fields[0][2] + fields[0][4]) // 2, (fields[0][3] + fields[0][5]) // 2)
type_text("Test note")
click((fields[2][2] + fields[2][4]) // 2, (fields[2][3] + fields[2][5]) // 2)
type_text("Written on SIEOS.")
frame_now(10, lambda s: s.get("focus") == "Written on SIEOS.")
shot("scr-08-note")
press("Z_N_SAVE")
f = frame_now(10, lambda s: s.get("dlg") == "0" and s.get("notes") == "2")
shot("scr-09-saved")
say("ok: a note written and saved")

# ---- the notes tab (Ctrl+Shift+N)
pos["log"] = len(full)
keys("ctrl", "shift", "n")
f = frame_now(10, lambda s: s.get("uri") == "notes:")
shot("scr-10-notes")
say("ok: the notes tab")

# ---- closed (Ctrl+W: the desktop closes it), started again
pos["log"] = len(full)
keys("ctrl", "w")
wait("atlas: window 17 closed", timeout=10)
out = sh("cat /home/alice/.scriptures/notes.json")
if "Written on SIEOS." not in out or "Test note" not in out: fail(f"notes.json: {out[-800:]}")
say("ok: closed; notes.json holds the note")
pos["log"] = len(full)
keys("ctrl", "spc"); wait("atlas: strip")
type_text("scriptures"); time.sleep(0.6); type_text("\n")
wait("atlas: window 17 opened by pid", timeout=30)
f = frame_now(30, lambda s: s.get("notes") == "2" and int(s.get("tabs", 0)) >= 2)
time.sleep(1); place()
shot("scr-11-again")
say(f"ok: started again: {f['state']['tabs']} tabs back, 2 notes")
press("Z_TAB", 0)
f = frame_now(10, lambda s: s.get("uri") == "/scriptures/ot/gen/1")
shot("scr-12-highlight-back")
keys("ctrl", "w")
wait("atlas: window 17 closed", timeout=10)
say(f"PASS ({time.time() - T0:.0f} s)")
