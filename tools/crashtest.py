#!/usr/bin/env python3
"""Crash recovery: boot SIEOS on a fresh disk, then kill its servers while
they are in use and check that everything carries on (init restarts them,
clients reconnect by themselves):
  - fs and vblk killed while fsloop writes, commits and checks files;
  - con killed while the shell waits for input;
  - auth killed with a session open; then su still works;
  - login killed with a session open: no second prompt until it ends;
  - a service that always fails: init backs off, then gives up;
  - a user may not kill someone else's process.
Then the disk is checked on the host (fsck.siefs, and fsloop's files).

Usage: crashtest.py <cpus> <host tools dir> <disk image> <qemu command line...>

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
"""
import os, re, select, shutil, subprocess, sys, time
SLOW = float(__import__("os").environ.get("SIEOS_SLOW", "1"))   # emulated processors (arm64 on a PC): everything takes longer

cpus, tools, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
copy = image + ".crash"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
out, seen, mark = b"", b"", 0

def fail(msg):
    qemu.kill()
    sys.exit("FAIL: " + msg)

def expect(text, timeout=30, must=True):
    """Wait for `text` (after what was already matched); returns what came
    before it (None if not `must` and it did not come in time)."""
    timeout *= SLOW
    global out, seen
    end = time.time() + timeout
    while text.encode() not in out:
        left = end - time.time()
        if not must and left <= 0:
            return None
        if left <= 0 or qemu.poll() is not None:
            qemu.kill()
            fail(f"waiting for {text!r}\n--- output ---\n{(seen + out).decode(errors='replace')[-6000:]}")
        if select.select([qemu.stdout], [], [], left)[0]:
            out += qemu.stdout.read1(4096)
    i = out.index(text.encode()) + len(text)
    before, out = out[:i], out[i:]
    seen += before
    return before.decode(errors="replace")

def send(line):
    qemu.stdin.write(line.encode() + b"\r")
    qemu.stdin.flush()

def sync(prompt="# "):
    """Kernel log lines share the serial port with the console, so their
    characters may cut into the shell's output: resynchronise on a marker
    (again, if a log line garbled it)."""
    global mark
    for attempt in range(5):
        mark += 1
        send(f"echo SYNC{mark}Z")
        if expect(f"SYNC{mark}Z\r\n", 5, must=False) is not None and expect(prompt, 5, must=False) is not None:
            return
    fail("cannot resynchronise with the shell")

def run(cmd, prompt="# ", timeout=30):
    timeout *= SLOW
    send(cmd)
    text = expect(prompt, timeout)
    return text

def svc():
    sync()
    text = run("svc")
    return {m[0]: (m[1], int(m[2]), int(m[3])) for m in re.findall(r"\n(\w+)\s+(running|waiting|GIVEN UP|stopped)\s+(\d+)\s+(\d+)", text)}

def restarted(name, old, timeout=10):
    """Poll svc until the service runs again with a new pid. (Kernel log
    lines and the console share the serial port, so their characters may
    interleave: the test never relies on reading a log line intact.)"""
    timeout *= SLOW
    end = time.time() + timeout
    s = None
    while time.time() < end:
        s = svc().get(name)
        if s and s[0] == "running" and s[1] != old:
            return s[1]
        time.sleep(SLOW * 0.2)
    fail(f"{name} did not come back (last seen: {s})\n--- output ---\n" + (seen + out).decode(errors="replace")[-3000:])

def kill(name):
    pid = svc()[name][1]
    send(f"kill {pid}")
    new = restarted(name, pid)
    print(f"ok: {name} (pid {pid}) killed and restarted (pid {new})")

def login(name, pw, prompt, restarted=False):
    if restarted:                       # its prompt and init's log line may interleave
        time.sleep(SLOW * 0.5)
    else:
        expect("login: ")
    send(name)
    expect("Password: ")
    send(pw)
    expect(f"{name}@sieos:")
    expect(prompt)

# ---- first start, then root
expect(f"mk: {cpus} CPU")
expect("first start")
for p in ("New password", "Again: "):
    expect(p); send("rootpass1")
expect("user name"); send("alice")
expect("full name"); send("Alice")
for p in ("New password", "Again: "):
    expect(p); send("alicepass1")
login("root", "rootpass1", "# ")
print("ok: first start, root logged in")
send("sleep 1.2")                       # services older than 1 s: a kill is not a "quick" failure
expect("# ")

# ---- the file server and the disk driver, killed while fsloop works
send("fsloop /tmp/loop 400 &")          # about 6-10 s: long enough for the three kills
time.sleep(SLOW * 0.5); kill("fs")
time.sleep(SLOW * 0.5); kill("vblk")
time.sleep(SLOW * 0.5); kill("fs")
end = time.time() + 120                 # its result line, wherever it came in the output
while not re.search(rb"fsloop: 400 rounds.*\n", seen + out) and time.time() < end:
    if select.select([qemu.stdout], [], [], 0.5)[0]:
        out += qemu.stdout.read1(4096)
m = re.search(rb"fsloop: 400 rounds.*", seen + out)
if not m or b"OK" not in m.group(0):
    fail((m.group(0).decode() if m else "fsloop did not finish") + "\n--- output ---\n" + (seen + out).decode(errors="replace")[-3000:])
print("ok: fsloop through 3 restarts: " + m.group(0).decode().strip())
sync()

# ---- the console, killed while the shell waits for a line
pid = svc()["con"][1]
send(f"kill {pid}")
time.sleep(SLOW * 0.3)
restarted("con", pid)
print("ok: con killed and restarted; the shell carries on")

# ---- auth killed with a session open; su works after
kill("auth")
sync()
send("su alice")                        # (root: no password asked)
expect("alice@sieos:"); expect("$ ")
send("whoami"); expect("alice"); expect("$ ")
send("exit"); expect("# ")
print("ok: auth restarted; this session went on; su alice works")

# ---- login killed with this session open: no new prompt until it ends
pid = svc()["login"][1]
send(f"kill {pid}")
time.sleep(SLOW * 0.5)
state = svc()["login"][0]
if state != "waiting":
    fail(f"login is {state!r} while the session is open")
print("ok: login killed; it waits for the open session")

# ---- a service that always fails: backoff, then given up
sync()
send("svc add crasher /bin/crash")      # (its kernel log lines may cut into the prompt)
time.sleep(SLOW * 0.6)                         # 10 + 20 + 40 + 80 ms of backoff, and 5 starts
s = svc()["crasher"]
if s[0] != "GIVEN UP":
    fail(f"crasher is {s[0]!r}")
print(f"ok: crasher given up after {s[2]} restarts (backoff 10, 20, 40, 80 ms)")

# ---- a user may not kill root's processes
fs_pid = svc()["fs"][1]
send("logout")                          # the session ends: now login comes back
login("alice", "alicepass1", "$ ", restarted=True)
sync("$ ")
send(f"kill {fs_pid}")
expect("not allowed"); expect("$ ")
print("ok: alice may not kill fs")
send("logout")
login("root", "rootpass1", "# ")
sync()
lat = re.findall(r"(\w+) restarted in (\d+) us", seen.decode(errors="replace"))
print("restart times (us): " + ", ".join(f"{n} {t}" for n, t in lat))
send("poweroff")
try:
    qemu.wait(SLOW * 10)
except subprocess.TimeoutExpired:
    qemu.kill()
    sys.exit("FAIL: QEMU did not power off")

# ---- the disk, on the host
r = subprocess.run([os.path.join(tools, "fsck.siefs"), copy], capture_output=True, text=True)
if r.returncode:
    sys.exit("FAIL: fsck.siefs:\n" + r.stdout + r.stderr)
r = subprocess.run([os.path.join(tools, "siefs"), copy, "ls", "/tmp/loop"], capture_output=True, text=True)
n = len([l for l in r.stdout.split() if l.startswith("file-")])
if n != 400:
    fail(f"{n} of fsloop's 400 files on the disk")
print("ok: fsck.siefs clean; fsloop's 400 files are on the disk")
os.remove(copy)
print("PASS")
