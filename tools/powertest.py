#!/usr/bin/env python3
"""ACPI power in QEMU (x86-64, user/acpid, docs/power.md), as root on the
serial console, through QEMU's monitor for the buttons:
  - `battery` (QEMU has none: "no battery");
  - sleep: a file written, `suspend` (S3), woken by the monitor's
    system_wakeup; then the shell, the file, a new file, the network;
    again while fsloop works; `suspend 3`: woken by the clock's alarm;
  - `reboot`: the FADT's reset register (QEMU stops: -no-reboot);
  - booted again: the power button (system_powerdown): a clean power off.
Usage: powertest.py <cpus> <disk image> <qemu command line...>
The image must be a fresh one (first-start setup); the test works in a copy.

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import os, re, select, shutil, socket, subprocess, sys, time
import qserial

cpus, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
copy = image + ".p"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
mon = copy + ".mon"
fails = 0
qemu = None
import atexit
atexit.register(lambda: qemu and qemu.poll() is None and qemu.kill())

def boot():
    global qemu, out, seen
    if os.path.exists(mon): os.unlink(mon)
    qemu = subprocess.Popen(cmdline + ["-monitor", f"unix:{mon},server,nowait"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = seen = b""

def expect(text, timeout=30):
    global out, seen
    end = time.time() + timeout
    while text.encode() not in out:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            if qemu.poll() is None: qemu.kill()
            sys.exit(f"FAIL: waiting for {text!r}\n--- output ---\n{seen.decode(errors='replace')[-6000:]}")
        if select.select([qemu.stdout], [], [], left)[0]:
            d = qemu.stdout.read1(65536)
            out += d
            seen += d
    i = out.index(text.encode())
    before, out = out[:i], out[i + len(text):]
    return before.decode(errors="replace")

def send(line): qserial.type_line(qemu, line)
def run(cmd, timeout=60): send(cmd); return expect("# ", timeout)
def monitor(cmd):
    s = socket.socket(socket.AF_UNIX)
    s.connect(mon)
    s.sendall(cmd.encode() + b"\n")
    time.sleep(0.3)
    s.close()
def line(r):                                        # a command's last line (before the prompt)
    l = r.strip().splitlines()
    return l[-2].strip() if len(l) > 1 else ""
def ok(name, cond, detail=""):
    global fails
    print(f"{'ok' if cond else 'FAIL'}: {name}{(' (' + detail + ')') if detail else ''}", flush=True)
    fails += not cond
def stopped(timeout=20):
    try: qemu.wait(timeout)
    except subprocess.TimeoutExpired: return False
    seen_rest = qemu.stdout.read()
    global seen
    seen += seen_rest
    return True

boot()
expect(f"mk: {cpus} CPU")
expect("first start")
for p in ("New password", "Again: "): expect(p); send("rootpass1")
expect("user name"); send("alice")
expect("full name"); send("Alice")
for p in ("New password", "Again: "): expect(p); send("alicepass1")
expect("login: "); send("root")
expect("Password: "); send("rootpass1")
expect("root@sieos:"); expect("# ")
r = run("battery")
ok("battery", "no battery" in r and "AC" in r, line(r))
ok("acpid: sleep types", b"\\_S5: sleep type" in seen and b"\\_S3: sleep type" in seen)

# ---- sleep (S3) and waking up
if not os.environ.get("NOSLEEP"):                   # (NOSLEEP=1: without this part)
  run("echo before > /root/before.txt")
  t0 = time.time()
  send("suspend")
  expect("mk: ACPI S3", 20)
  time.sleep(1.0)
  asleep = qemu.poll() is None
  monitor("system_wakeup")
  expect("mk: awake", 20)
  expect("# ", 30)
  ok("suspend, then woken by the monitor", asleep, f"{time.time() - t0:.1f} s")
  r = run("cat /root/before.txt")
  ok("a file written before", "before" in r)
  run("echo after > /root/after.txt")
  ok("a file written after", "after" in run("cat /root/after.txt"))
  r = run("fetch -p 10.0.2.2 3", 60)
  if "reply in" not in r: print(seen.decode(errors="replace")[-5000:])
  ok("the network after waking", "reply in" in r, line(r))
  r = run("battery")
  ok("woken once", "woken 1 times" in r, line(r))
  r = run("ps")
  ok("the drivers restarted", "vblk" in r and "con" in r)
  # again, while fsloop writes, commits and checks files (the disk busy, the CPUs too)
  send("mkdir /tmp/loop"); expect("# ")
  send("fsloop /tmp/loop 300 &"); expect("# ")
  time.sleep(0.5)
  send("suspend")
  expect("mk: ACPI S3", 20)
  time.sleep(0.5)
  monitor("system_wakeup")
  expect("mk: awake", 20)
  end = time.time() + 120
  while not re.search(rb"fsloop: 300 rounds.*\n", seen) and time.time() < end and qemu.poll() is None:
      if select.select([qemu.stdout], [], [], 1)[0]: d = qemu.stdout.read1(65536); out += d; seen += d
  m = re.search(rb"fsloop: 300 rounds.*", seen)
  ok("suspended while fsloop worked", m is not None and b"OK" in m.group(0), m.group(0).decode().strip() if m else "fsloop did not finish")
  out = b""
  send(""); expect("# ")
  r = run("battery")
  ok("woken twice", "woken 2 times" in r, line(r))
  # woken by the clock: `suspend 3`, the CMOS alarm (PM1 RTC_EN); no monitor command
  t0 = time.time()
  send("suspend 3")
  expect("mk: ACPI S3", 20)
  w = int(expect(")", 30).split("acpid: awake (PM1 status ")[-1].split(",")[0], 16)
  dt = time.time() - t0
  expect("# ", 30)
  ok("suspend 3: woken by the clock", w & 0x400 and 2 <= dt < 20, f"PM1 status {w:x}, {dt:.1f} s")
  r = run("battery")
  ok("woken 3 times", "woken 3 times" in r, line(r))

# ---- restart: the FADT's reset register (QEMU stops)
send("reboot")
ok("QEMU stopped (-no-reboot)", stopped())
ok("reboot", b"mk: restarting" in seen, "reset register" if b"mk: reset register" in seen else "keyboard controller")

# ---- booted again: the power button
boot()
expect("login: ", 60); send("root")
expect("Password: "); send("rootpass1")
expect("# ")
if not os.environ.get("NOSLEEP"): ok("the file written after waking is on the disk", "after" in run("cat /root/after.txt"))
monitor("system_powerdown")
ok("power button: power off", stopped(30))
ok("power button seen by acpid", b"acpid: power button" in seen and b"mk: power off" in seen)
if fails: print(seen.decode(errors="replace")[-6000:])
print("PASS" if not fails else f"FAIL ({fails})")
sys.exit(1 if fails else 0)
