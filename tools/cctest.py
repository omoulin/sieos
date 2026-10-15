#!/usr/bin/env python3
"""sicc on SIEOS (/bin/cc): boot SIEOS without a window, then as alice on the
serial console: compile the programs of tests/cc/sieos (put in /tmp/t by
the Makefile) with cc, run them, check their output; a
compile error is reported with its line; -S writes assembly; the current
folder reaches the programs a shell starts.
Usage: cctest.py <cpus> <disk image> <qemu command line...> (make cc-sieos-test)

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import select, shutil, subprocess, sys, time
import qserial                                      # (tools/qserial.py: typing on the console)
SLOW = float(__import__("os").environ.get("SIEOS_SLOW", "1"))   # emulated processors: everything takes longer

cpus, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
copy = image + ".cc"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
import atexit
atexit.register(lambda: qemu.poll() is None and qemu.kill())
out = b""
fails = 0

def expect(text, timeout=30):
    """Wait for text; return what came before it."""
    timeout *= SLOW
    global out
    end = time.time() + timeout
    while text.encode() not in out:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            qemu.kill()
            sys.exit(f"FAIL: waiting for {text!r}\n--- output ---\n{out.decode(errors='replace')[-3000:]}")
        if select.select([qemu.stdout], [], [], left)[0]:
            out += qemu.stdout.read1(65536)
    i = out.index(text.encode())
    before, out = out[:i], out[i + len(text):]
    return before.decode(errors="replace")

def send(line):
    qserial.type_line(qemu, line)

def run(cmd, timeout=120):
    """A command at alice's prompt -> its output."""
    send(cmd)
    return expect("$ ", timeout), 0

def ok(name, cond, detail=""):
    global fails
    print(f"{'ok' if cond else 'FAIL'}: {name}{(' (' + detail + ')') if detail else ''}")
    fails += not cond

expect(f"mk: {cpus} CPU")
expect("first start")
for p in ("New password", "Again: "): expect(p); send("rootpass1")
expect("user name"); send("alice")
expect("full name"); send("Alice")
for p in ("New password", "Again: "): expect(p); send("alicepass1")
expect("login: "); send("alice")
expect("Password: "); send("alicepass1")
expect("alice@sieos:"); expect("$ ")

r, _ = run("cd /tmp/t")
r, _ = run("pwd")
ok("the shell is in /tmp/t", "/tmp/t" in r, r.strip())
r, _ = run("cc hello.c -o hello", 300)
ok("cc hello.c -o hello", "error" not in r and "cannot" not in r, r.strip()[-200:])
r, _ = run("./hello world")
ok("./hello world: it runs", "hello from sicc on SIEOS: 2 argument(s), first world" in r, r.strip()[-120:])
r, _ = run("cc -O2 calc.c -o calc", 300)
ok("cc -O2 calc.c -o calc", "error" not in r and "cannot" not in r, r.strip()[-200:])
r, _ = run("./calc")
ok("./calc: every result right", "calc: fib 6765, cubes 250500250000, area 12, harmonic*1e6 2928968, 128-bit ok" in r, r.strip()[-160:])
r, _ = run("cc -S hello.c -o hello.s")
r2, _ = run("cat hello.s")
ok("cc -S: assembly", "main:" in r2, r2.strip()[:60])
r, _ = run("cc broken.c -o broken")
ok("a compile error names its line", "broken.c:4" in r, r.strip()[-160:])
r, _ = run("ls")
ok("the outputs are in /tmp/t", "hello" in r and "calc" in r and "hello.s" in r, " ".join(r.split())[-120:])
send("exit")
time.sleep(0.5)
qemu.kill()
print("PASS" if not fails else f"FAIL: {fails} check(s)")
sys.exit(1 if fails else 0)
