#!/usr/bin/env python3
"""The signals test (docs/kernel.md, "Signals"): boot SIEOS on a fresh disk
(first-start setup), log in as root on the serial console, then:
  - ktest sig (built by gcc and by sicc): caught, ignored and default
    signals, the waits they interrupt (and a call's late reply dropped), a
    process group, registers and IPC through a storm of signals;
  - Ctrl-C on the console ends a busy program, a sleeping one, one waiting
    for a typed line and one waiting on the network, and the prompt comes
    back; it ends a built-in sleep and a typed line too; the shell survives;
  - a program that catches SIGINT gets it in a busy loop, a sleep and a
    console read, and cleans up;
  - kill -INT / -TERM / -KILL between processes, from the shell;
  - Python: Ctrl-C raises KeyboardInterrupt in a running loop, a sleep,
    input() and at the prompt; a script's try/except cleans up.
Usage: sigtest.py <cpus> <disk image> <qemu command line...> (make sig-test)

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import os, re, select, shutil, subprocess, sys, time
import qserial                                      # (tools/qserial.py: typing on the console)
SLOW = float(os.environ.get("SIEOS_SLOW", "1"))     # emulated processors (arm64 on a PC): everything takes longer

cpus, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
copy = image + ".sig"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
import atexit
atexit.register(lambda: qemu.poll() is None and qemu.kill())
out, seen = b"", b""

def expect(text, timeout=30):
    """Wait for text; return what came before it."""
    global out, seen
    end = time.time() + timeout * SLOW
    while text.encode() not in out:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            qemu.kill()
            sys.exit(f"FAIL: waiting for {text!r}\n--- output ---\n{(seen + out).decode(errors='replace')[-3000:]}")
        if select.select([qemu.stdout], [], [], left)[0]:
            out += qemu.stdout.read1(65536)
    i = out.index(text.encode())
    before, seen, out = out[:i], seen + out[:i + len(text)], out[i + len(text):]
    return before.decode(errors="replace")

def send(line): qserial.type_line(qemu, line)
def ctrl_c():
    qemu.stdin.write(b"\x03")
    qemu.stdin.flush()
def pause(s): time.sleep(s * SLOW)
PROMPT = "root@sieos:/root# "
fails = 0
def ok(name, cond, detail=""):
    global fails
    detail = " ".join(detail.split())[-100:]
    print(f"{'ok' if cond else 'FAIL'}: {name}{(' (' + detail + ')') if detail else ''}")
    fails += not cond
def run(cmd, timeout=60):
    send(cmd)
    return expect(PROMPT, timeout)

expect(f"mk: {cpus} CPU")
expect("first start")
for p in ("New password", "Again: "): expect(p); send("rootpass1")
expect("user name"); send("alice")
expect("full name"); send("Alice")
for p in ("New password", "Again: "): expect(p); send("alicepass1")
expect("login: "); send("root")
expect("Password: "); send("rootpass1")
expect(PROMPT)

# ---- the kernel's part, in one program
for prog in ("ktest", "ktest-sicc"):
    r = run(f"{prog} sig", 300)
    for l in r.splitlines():
        if l.startswith("ktest sig: ") and not l.endswith(": ok"): print("   ", l.strip())
    ok(f"{prog} sig", "ktest sig: ok" in r, "" if "ktest sig: ok" in r else r.strip()[-300:])

# ---- Ctrl-C on the console: a program without a handler ends, the prompt comes back
for how in ("spin", "sleep", "read", "net"):
    send(f"ktest hang {how}")
    expect(f"ktest hang: {how}", 30)
    pause(0.5)
    t = time.time()
    ctrl_c()
    r = expect(PROMPT, 30)
    ok(f"Ctrl-C ends a program {'busy' if how == 'spin' else 'waiting: ' + how}", "^C" in r and "not interrupted" not in r,
       f"{(time.time() - t) * 1000:.0f} ms")
r = run("ps", 30)
ok("none of them is left", "ktest" not in r)
send("sleep 60")
pause(0.5)
t = time.time()
ctrl_c()
expect(PROMPT, 30)
ok("Ctrl-C ends the built-in sleep", time.time() - t < 30 * SLOW, f"{(time.time() - t) * 1000:.0f} ms")
qemu.stdin.write(b"echo half-typed")
qemu.stdin.flush()
pause(0.3)
ctrl_c()
r = expect(PROMPT, 30)
r += run("echo still here", 30)
ok("Ctrl-C drops a typed line; the shell goes on", "still here" in r and "half-typed\r\n" not in r.replace("echo half-typed", ""))

# ---- a program that catches SIGINT
send("ktest sigwait")
expect("ktest sigwait: ready", 30)
pause(0.3)
steps = []
for what in ("busy loop", "sleep", "read"):
    ctrl_c()
    steps.append(expect(f"caught in a {what}", 30) + expect("\n", 10))
    pause(0.5)
r = expect(PROMPT, 30)
ok("a handler catches SIGINT (busy, sleeping, reading) and cleans up",
   "not interrupted" not in "".join(steps) and "cleaned up, 3 signals" in r)

# ---- kill between processes
pids = {}
for name in ("int", "term", "kill"):
    send("ktest hang spin &")
    m = re.search(r"\[(\d+)\]", expect(PROMPT, 30))
    pids[name] = m.group(1) if m else "0"
r = run(f"kill -INT {pids['int']}", 30) + run(f"kill {pids['term']}", 30) + run(f"kill -KILL {pids['kill']}", 30)
pause(0.5)
r += run("", 30)            # (the shell tells of ended background programs at its next prompt)
ok("kill -INT, kill (TERM), kill -KILL end background programs",
   f"[{pids['int']}] interrupted" in r and f"[{pids['term']}] terminated" in r and f"[{pids['kill']}] killed" in r)
r = run("kill -HUP 1", 30)
ok("not init", "not allowed" in r)

# ---- Python: KeyboardInterrupt
send("python")
expect(">>> ", 60)
def py(line, want, timeout=30):
    send(line)
    return expect(want, timeout)
py("while True: pass", "... ")
py("", "\n")
pause(1)
ctrl_c()
r = expect(">>> ", 30)
ok("python: Ctrl-C in a loop: KeyboardInterrupt, the prompt stays", "KeyboardInterrupt" in r)
r = py("1 + 1", ">>> ")
ok("python: and it carries on", "2" in r)
py("import time", ">>> ")
send("time.sleep(100)")
pause(1)
t = time.time()
ctrl_c()
r = expect(">>> ", 30)
ok("python: Ctrl-C in time.sleep", "KeyboardInterrupt" in r and time.time() - t < 20 * SLOW)
send("input('? ')")
expect("? ", 30)
pause(0.5)
ctrl_c()
r = expect(">>> ", 30)
ok("python: Ctrl-C in input()", "KeyboardInterrupt" in r)
ctrl_c()
r = expect(">>> ", 30)
ok("python: Ctrl-C at the prompt", "KeyboardInterrupt" in r)
for l in ("try:", "    while True: pass", "except KeyboardInterrupt:", "    print('cleaned', 'up')"):
    py(l, "... ")
py("", "\n")
pause(1)
ctrl_c()
r = expect(">>> ", 30)
ok("python: try/except KeyboardInterrupt cleans up", "cleaned up" in r)
qemu.stdin.write(b"\x04")
qemu.stdin.flush()
expect(PROMPT, 30)
send("python -c while True: pass")
pause(2)
ctrl_c()
r = expect(PROMPT, 30)
ok("python -c: Ctrl-C ends the script", "KeyboardInterrupt" in r)

send("poweroff")
try:
    qemu.wait(15 * SLOW)
except subprocess.TimeoutExpired:
    qemu.kill()
print("PASS" if not fails else f"FAIL: {fails} checks")
sys.exit(1 if fails else 0)
