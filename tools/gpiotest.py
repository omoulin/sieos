#!/usr/bin/env python3
"""The GPIO server, library and command (docs/gpio.md): boot SIEOS without a
window; as alice on the serial console, /bin/gpio (list, out, get, in with a
pull, a device's pin refused, blink and stop, the self-check), then a C program using gpio.h
built with cc on SIEOS (tests/gpio, put in /tmp/t by the Makefile). With no
GPIO here (x86-64, QEMU's virt) the server simulates the pins.
Usage: gpiotest.py <cpus> <disk image> <qemu command line...> (make gpio-test)

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import atexit, os, select, shutil, subprocess, sys, time
SLOW = float(os.environ.get("SIEOS_SLOW", "1"))

cpus, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
copy = image + ".gpio"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
atexit.register(lambda: qemu.poll() is None and qemu.kill())
out = b""
fails = 0

def expect(text, timeout=30):
    global out
    end = time.time() + timeout * SLOW
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
    qemu.stdin.write(line.encode() + b"\r")
    qemu.stdin.flush()

def run(cmd, timeout=60):
    send(cmd)
    return expect("$ ", timeout)

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

r = run("gpio")
ok("gpio: the pins listed", "28 pins" in r and " 14  busy" in r, " ".join(r.split())[:90])
r = run("gpio out 17 1") + run("gpio get 17")
ok("gpio out 17 1, then get: 1", "\n1\r" in r or "\n1\n" in r, r.strip()[-40:])
r = run("gpio out 17 0") + run("gpio get 17")
ok("gpio out 17 0, then get: 0", "\n0\r" in r or "\n0\n" in r, r.strip()[-40:])
r = run("gpio in 27 up") + run("gpio")
ok("gpio in 27 up: an input with its pull", " 27  in   1 pull-up" in r or " 27  in   0 pull-up" in r, "")
r = run("gpio out 14 1")
ok("the serial console's pin refused", "used by a device" in r, r.strip()[-60:])
r = run("gpio blink 22 10")
time.sleep(0.5 * SLOW)
r += run("gpio stop") + run("gpio")
ok("gpio blink 22 10, then stop", "error" not in r and " 22  out" in r, "")
run("gpio out 17 1")
pins = lambda r: [l.split() for l in r.splitlines() if l.strip()[:1].isdigit()]   # the listing's rows
a, t, b = run("gpio"), run("gpio selftest", 120), run("gpio")
print("\n".join("   " + l for l in t.strip().splitlines()[1:]))
ok("gpio selftest: every free pin passes", "selftest: PASS: 22 pins checked, 0 failed, 6 skipped" in t and t.count(" PASS\r") + t.count(" PASS\n") == 22,
   " ".join(t.split())[-90:])
ok("gpio selftest: the console's pins skipped", "  14  skipped" in t and "  15  skipped" in t)
ok("gpio selftest: every pin put back", len(pins(a)) == 28 and pins(a) == pins(b), "")
r = run("cd /tmp/t") + run("cc pins.c -o pins", 300)
ok("cc pins.c (gpio.h, libsieos.a)", "error" not in r and "cannot" not in r, r.strip()[-200:])
r = run("./pins", 60)
print("\n".join("   " + l for l in r.strip().splitlines() if l.strip()))
ok("pins: every check", "gpio test: ok" in r)
send("exit")
time.sleep(0.5)
qemu.kill()
os.unlink(copy)
print("PASS" if not fails else f"FAIL: {fails} check(s)")
sys.exit(1 if fails else 0)
