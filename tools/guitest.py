#!/usr/bin/env python3
"""Test the desktop (Stage, /bin/atlas) through QEMU's control socket
(QMP): screenshots, key presses and absolute mouse events, checked against
the desktop's log lines on the serial port and against the pixels.

  first start on the screen (accounts) -> alice on the stage -> files and
  projects from the terminal, older island attributes migrated -> the
  stage: two terminals, beside, swap, the shelf, keys -> history, copy and
  paste -> the editor -> the Lens (a file, an app, a question to sia) ->
  projects: new, a file and an app in one, a card dragged onto another,
  rename -> kill atlas: init restarts it, the projects stay -> delete a
  project -> kill the console server -> memory and frame times.

With /tmp/atlas.debug (made here), atlas logs where it drew things.

Usage: guitest.py <cpus> <screenshots dir> <disk image> <qemu command line...>
The test works in a copy of the image. Screenshots (PNG) are kept.

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import os, atexit, json, os, select, shutil, socket, subprocess, sys, time

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
    qemu.stdin.write(line.encode() + b"\r")
    qemu.stdin.flush()

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

def mouse_to(x, y):
    """Move the pointer to screen pixel (x, y) of the 1920 x 1080 screen."""
    q("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": x * 32767 // 1919}},
                                  {"type": "abs", "data": {"axis": "y", "value": y * 32767 // 1079}}])
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

def projects():
    """The last "atlas: projects:" line already printed -> {name: files}."""
    k = full.rfind(b"atlas: projects: ")
    line = full[k + 17: full.find(b"\n", k)].decode()
    return {a: int(b) for a, b in (w.rsplit("=", 1) for w in line.split())}

def stage(text):
    """Wait for the stage line that says `text` ("demo: main=Terminal 3 beside=-")."""
    got = wait("atlas: stage ", "log", 10)
    while not got.startswith(text):
        got = wait("atlas: stage ", "log", 10)

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

NAVY, ACC, TERM, TEXT, TERMFG, GLASS = "0a0f1c", "4be3c1", "070b14", "e6ecf5", "9fe8c8", "141a28"

# ---- first start: the accounts are made on the screen
start = time.time()
wait(f"mk: {cpus} CPU")
wait("atlas: first start: accounts needed (setup on the screen)")
expect("please complete the setup on the screen")    # the serial console only says so
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
click(980, 811)                                    # "Add another user"
wait("atlas: setup: another user")
type_text("bob\tBob Example\tbobpass11\tbobpass11\n")
wait("atlas: setup: step 3")
time.sleep(0.3)
shot("03-setup-ready")
type_text("\n")                                    # "Create accounts"
wait("atlas: setup: accounts created (root and 2 user(s))", "log", 20)
wait("atlas: login alice: ok", "log", 20)
stage("Inbox: main=Terminal 1 beside=-")
expect("login: ")                                  # the serial console's prompt, now that accounts exist
time.sleep(0.8)
img = shot("04-logged-in")
if not has_colour(img, TERM, at_least=20000) or not has_colour(img, TERMFG, at_least=20) or not has_colour(img, ACC, at_least=50):
    fail("no terminal on the stage after the setup")
print("ok: accounts made on the screen (root, alice, bob; bad ones refused); alice on the stage with a terminal")

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

# ---- files and projects from the terminal; older island attributes migrated
type_text("echo hello from stage > note.txt\n")
wait("atlas: projects: Inbox=1 ")
type_text("tag note.txt project=demo\n")
got = wait("atlas: projects: ")
if "demo=1" not in got:
    fail(f"the project 'demo' did not appear: {got}")
type_text("echo old > old.txt\n")
type_text("tag old.txt island.legacy=1\n")
wait("atlas: migrated /home/alice/old.txt: project legacy")
type_text("tag old.txt atlas.zoom=3\n")
wait("atlas: migrated /home/alice/old.txt: project legacy")
type_text("echo drift > drift.txt\n")
type_text("tag drift.txt island.Driftwood=1\n")
wait("atlas: migrated /home/alice/drift.txt: project Inbox")
type_text("echo oldisle > .islands\n")            # the older desktop's empty islands
got = wait("atlas: projects: ")
while "oldisle=0" not in got:
    got = wait("atlas: projects: ")
send("tags /home/alice/old.txt")
expect("project")
drain()
if b"island." in full[pos["con"]:] or b"atlas." in full[pos["con"]:]:
    fail("old.txt kept its island attributes")
print(f"ok: projects from tags ({got.strip()}); island.* -> project, Driftwood -> Inbox, ~/.islands -> empty project")
shot("05-projects")

# ---- the stage: a project, two terminals, beside, swap, the shelf
click(*latest("project", "demo"))
stage("demo: main=- beside=-")
time.sleep(0.3)
shot("06-empty-stage")
keys("ctrl", "t")
c2 = opened()
stage(f"demo: main=Terminal {c2} beside=-")
keys("ctrl", "t")
c3 = opened()
stage(f"demo: main=Terminal {c3} beside=-")
for c in (c2, c3):
    if ps_uid(term_pid(c)) != "1000":
        fail(f"terminal {c} does not run as alice")
print(f"ok: project 'demo', Ctrl+T twice: terminals {c2} and {c3} (both run as alice, uid 1000)")
x, y = latest("card", f"Terminal {c2}")
drag(x, y, 1600, 400)                             # up onto the right half: beside
wait("atlas: card dropped on the stage (beside)")
stage(f"demo: main=Terminal {c3} beside=Terminal {c2}")
time.sleep(0.6)
img = shot("07-main-beside")
print("ok: dragged a terminal's card up to the right: beside")
click(*latest("wbtn", f"Terminal {c3} Swap"))
stage(f"demo: main=Terminal {c2} beside=Terminal {c3}")
keys("ctrl", "w")                                  # the focused one (now main) to the shelf
stage(f"demo: main=Terminal {c3} beside=-")
keys("ctrl", "backslash")                          # the shelf's first window beside
stage(f"demo: main=Terminal {c3} beside=Terminal {c2}")
keys("ctrl", "backslash")
stage(f"demo: main=Terminal {c3} beside=-")
keys("ctrl", "tab")
stage(f"demo: main=Terminal {c2} beside=-")
print("ok: Swap, Ctrl+W (to the shelf), Ctrl+\\ (beside on/off), Ctrl+Tab (next window)")

# ---- a terminal's history and copy / paste
geo = wait(f"atlas: terminal {c2} at ").split()
tx, ty, cw, ch = int(geo[0]), int(geo[1]), int(geo[3]), int(geo[4])
type_text("help\n")
type_text("ls -l /bin\n")
time.sleep(0.8)
keys("pgup")
got = wait(f"atlas: terminal {c2} back ")
if int(got.split()[0]) <= 0:
    fail(f"no history to scroll back to: {got}")
keys("pgdn")
back = got.split()[0]
wait(f"atlas: terminal {c2} back 0")
type_text("clear\n")
time.sleep(0.4)
cmd = "ln drift.txt pasted.txt"
type_text(f"echo {cmd}\n")                        # its output, on the second line, is a command
time.sleep(0.6)
inbox = projects()["Inbox"]
y = ty + ch + ch // 2
mouse_to(tx + cw // 2, y); button("left", True)
mouse_to(tx + 10 * cw, y); mouse_to(tx + (len(cmd) - 1) * cw + cw // 2, y); button("left", False)
got = wait("atlas: copied ")
if not got.startswith(f"{len(cmd)} bytes"):
    fail(f"the selection: {got}")
shot("08-selection")
keys("ctrl", "v")
wait(f"atlas: pasted {len(cmd)} bytes")
type_text("\n")
got = wait("atlas: projects: ")
if f"Inbox={inbox + 1}" not in got:              # pasted.txt (a link to drift.txt: no project either)
    fail(f"the pasted command did not run: {got}")
print(f"ok: history (PgUp went back {back} lines), selected {len(cmd)} bytes, pasted with Ctrl+V, it ran")

# ---- the editor: a card of the shelf, type, Ctrl+S, Esc
click(*latest("card", "note.txt"))
wait("atlas: editing note.txt")
stage("demo: main=note.txt")
type_text("edited in stage\n")
keys("ctrl", "s")
got = wait("atlas: saved note.txt ")
time.sleep(0.3)
shot("09-editor")
keys("esc")
wait("atlas: editor closed note.txt")
send("cat /home/alice/note.txt")
expect("edited in stage")
print(f"ok: note.txt opened from its card, edited, saved ({got.strip()}); the shell reads the new text")

# ---- the Lens: a file, an app, a question
keys("ctrl", "spc")
wait("atlas: lens")
type_text("note")
time.sleep(0.4)
shot("10-lens")
type_text("\n")
wait("atlas: editing note.txt")
keys("esc")
wait("atlas: editor closed note.txt")
keys("ctrl", "spc")
type_text("hello")
time.sleep(0.3)
type_text("\n")
got = wait("atlas: app Hello started in demo (terminal ")
ch_app = int(got.split(")")[0])
if ps_uid(term_pid(ch_app)) != "1000":
    fail("the app does not run as alice")
keys("ctrl", "spc")
type_text("zzz")
keys("esc")                                        # Esc: the results close, nothing opens
time.sleep(0.3)
print(f"ok: Lens: 'note' opened the file, 'hello' started the Hello app (terminal {ch_app}, as alice), Esc closed it")

keys("ctrl", "spc")
type_text("show me my note")
type_text("\t")                                    # Tab: ask sia
wait("atlas: ask from the lens")
wait("atlas: assistant open")
wait("atlas: sia: ask")
which, got = wait_any(["atlas: sia: done ", "atlas: sia: error "], "log", 120)
if stub and "done" not in which:
    fail(f"the stand-in did not answer: {got}")
time.sleep(0.4)
img = shot("11-assistant")
print(f"ok: Lens -> Ask sia: {which.split(': ')[-1].strip()} {got.strip()}")
if stub:
    if "2 actions" not in got:
        fail(f"the answer's actions were not found: {got}")
    b0 = wait("atlas: sia: button 0 at ").split()
    b1 = wait("atlas: sia: button 1 at ").split()
    click(int(b0[0]), int(b0[1]))
    stage("demo: main=note.txt beside=sia")         # (logged before the action's own line)
    if not wait("atlas: sia: action fly ").strip().endswith("-> 0"):
        fail("the 'Open note.txt' button did not find the file")
    time.sleep(0.4)
    shot("12-asked-open")
    keys("esc")
    wait("atlas: editor closed note.txt")
    b1 = None
    drain()
    k = full.rfind(b"atlas: sia: button 1 at ")
    b1 = full[k + 24:].split()[:2]
    click(int(b1[0]), int(b1[1]))
    got = wait("atlas: sia: action project ")
    if not got.strip().endswith("-> 0"):
        fail(f"the 'Project demo' button: {got}")
    print("ok: the answer's buttons: 'Open note.txt' (the editor, sia beside), 'Project demo'")
    keys("ctrl", "spc")
    type_text("please count slowly\t")
    wait("atlas: sia: first piece", "log", 30)
    keys("esc")                                    # Esc in the assistant: stop the answer
    got = wait("atlas: sia: stopped after ", "log", 20)
    print(f"ok: Esc stopped a long answer after {got.split()[0]} pieces")

# ---- projects: new, a new file and an app in it, a file moved in, rename, delete
click(*latest("newproject"))
wait("atlas: prompt Name of the new project")
type_text("work\n")
wait("atlas: new project work")
stage("work: main=- beside=-")
click(*latest("card", "+ Add"))
wait("atlas: menu Add to this project")
time.sleep(0.3)
shot("13-add-menu")
menu_pick("New file")
wait("atlas: prompt New file")
type_text("plan.txt\n")
got = wait("atlas: new file plan.txt in work ")
if not got.startswith("(0)"):
    fail(f"new file: {got}")
wait("atlas: editing plan.txt")
type_text("the plan\n")
keys("ctrl", "s")
wait("atlas: saved plan.txt ")
keys("esc")
wait("atlas: editor closed plan.txt")
click(*latest("card", "+ Add"))
wait("atlas: menu Add to this project")
menu_pick("Open app")
wait("atlas: menu Open app")
menu_pick("Hello")
got = wait("atlas: app Hello started in work (terminal ")
send("find project=work")
expect("/home/alice/apps/hello.app")
print("ok: '+ New project' work; '+' card -> New file plan.txt (edited, saved), Open app Hello: both in 'work'")

x, y = latest("card", "plan.txt")
lx, ly = latest("project", "legacy")
drag(x, y, lx, ly)                                 # a card onto a project of the rail: it moves there
got = wait("atlas: plan.txt moved to legacy ")
if not got.startswith("(0)"):
    fail(f"move: {got}")
got = wait("atlas: projects: ")
if "work=1" not in got or "legacy=2" not in got:
    fail(f"the move: {got}")
print(f"ok: dragged plan.txt's card onto 'legacy' in the rail: it moved ({got.strip()})")

click(*latest("project", "work"), "right")
wait("atlas: menu Project work")
time.sleep(0.3)
shot("14-project-menu")
menu_pick("Rename")
wait("atlas: prompt Rename")
type_text("\b\b\b\bplans\n")
got = wait("atlas: project work renamed to plans ")
if not got.startswith("(1 files, 0 not allowed)"):
    fail(f"rename: {got}")
got = wait("atlas: projects: ")
if "plans=1" not in got or "work" in got:
    fail(f"rename: {got}")
time.sleep(0.4)
shot("15-renamed")
print("ok: right-click 'work' -> Rename: it is 'plans' (its app and its terminal go with it)")

# ---- kill the desktop: init restarts it; the login screen; the projects stay
send("svc")
expect("UPTIME")
got = expect("atlas ")
send(f"kill {[w for w in got.split() if w.isdigit()][0]}")
wait("atlas: greeter", "log", 10)
time.sleep(1.0)
type_text("alice\twrong\n")
wait("atlas: login alice: refused", "log", 10)
img = shot("16-login")
if not has_colour(img, ACC, (560, 290, 1360, 790), 50):
    fail("no login panel")
send("ps")
expect("NAME")
drain(0.6)
listing = full[pos["con"]:].decode(errors="replace")
if any(l.split()[2:3] == ["1000"] for l in listing.splitlines() if len(l.split()) > 3):
    fail("alice's shells still run after the desktop restarted:\n" + listing)
type_text("alicepass1\n")
wait("atlas: login alice: ok")
p = projects()
for name, n in (("plans", 1), ("legacy", 2), ("oldisle", 0), ("demo", 1)):
    if p.get(name) != n:
        fail(f"project {name} after the restart: {p}")
print(f"ok: atlas killed -> init restarted it; old shells ended; wrong password refused; projects kept: {p}")

click(*latest("project", "plans"), "right")
wait("atlas: menu Project plans")
menu_pick("Delete the project")
wait("atlas: menu Delete plans")
menu_pick("Delete the project")
got = wait("atlas: project plans deleted ")
if not got.startswith("(1 files to the Inbox, 0 not allowed)"):
    fail(f"delete: {got}")
got = wait("atlas: projects: ")
if "plans" in got:
    fail(f"delete: {got}")
send("ls /home/alice/apps")
expect("hello.app")
time.sleep(0.4)
shot("17-inbox")
print(f"ok: right-click 'plans' -> Delete (confirmed): its files went to the Inbox ({got.strip()})")
keys("ctrl", "t")
opened()
time.sleep(0.8)

# ---- kill the console server (keyboard and mouse): the desktop gets them back
send("svc")
expect("UPTIME")
got = expect("con ")
send(f"kill {[w for w in got.split() if w.isdigit()][0]}")
wait("init: con")
time.sleep(1.0)
before = projects().get("Inbox", 0)
type_text("echo still typing > after-con.txt\n")
got = wait("atlas: projects: ")
if f"Inbox={before + 1}" not in got:
    fail(f"keyboard lost after the console server restarted: {got}")
print("ok: console server killed and restarted; the keyboard still reaches the desktop")

# ---- measurements: frame times (logged with each settled picture), memory per process
frames = [int(l.split(b"frame ")[1].split()[0]) for l in full.split(b"\n") if b"atlas: dbg stage " in l]
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
