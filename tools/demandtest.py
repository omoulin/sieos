#!/usr/bin/env python3
"""Boot SIEOS without a window and check servers started on demand, as root
on the serial console:
  - after boot: the network (vnet, netd) and the assistant (siad) are not
    running; memory used
  - first use starts them (a ping starts netd, which starts vnet; a question
    starts siad); time to the first answer
  - unused, they stop (idle times shortened with svc idle), and the next use
    starts them again
  - calls racing a server that is stopping are answered by the next one
    (racey: lingers while stopping), and none fails
  - a stopped service fails its callers at once instead of leaving them waiting
Usage: demandtest.py <cpus> <disk image> <qemu command line...> (make demand-test)

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
"""
import re, select, shutil, subprocess, sys, time
SLOW = float(__import__("os").environ.get("SIEOS_SLOW", "1"))   # emulated processors (arm64 on a PC): everything takes longer

cpus, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
copy = image + ".d"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
import atexit
atexit.register(lambda: qemu.poll() is None and qemu.kill())
out = b""
fails = 0

def expect(text, timeout=30):
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
    qemu.stdin.write(line.encode() + b"\r")
    qemu.stdin.flush()

def run(cmd, timeout=60):
    timeout *= SLOW
    send(cmd)
    return expect("# ", timeout)

def ok(name, cond, detail=""):
    global fails
    print(f"{'ok' if cond else 'FAIL'}: {name}{(' (' + detail + ')') if detail else ''}")
    fails += not cond

def procs():
    """The running process names (ps)."""
    return {l.split()[-1] for l in run("ps").splitlines() if re.match(r"\s*\d+\s+\d+", l)}

def used():
    m = re.search(r"(\d+) KiB used", run("mem"))
    return int(m.group(1)) if m else -1

start = time.time()
expect(f"mk: {cpus} CPU")
expect("first start")
boot = time.time() - start
for p in ("New password", "Again: "): expect(p); send("rootpass1")
expect("user name"); send("alice")
expect("full name"); send("Alice")
for p in ("New password", "Again: "): expect(p); send("alicepass1")
expect("login: "); send("root")
expect("Password: "); send("rootpass1")
expect("root@sieos:"); expect("# ")
print(f"ok: booted to the first-start prompt in {boot:.2f} s")

# 1. after boot: no network, no assistant
p = procs()
ok("after boot, vnet, netd and siad are not running", not p & {"vnet", "netd", "siad"}, " ".join(sorted(p)))
s = run("svc")
ok("svc shows them idle, with their ports", all(re.search(rf"\n{n}\s+idle.*{port}, idle", s) for n, port in
                                               (("vnet", "nic0"), ("netd", "net"), ("sia", "sia"))))
boot_kib = used()
print(f"    memory used after boot, logged in: {boot_kib} KiB")

# 2. the first ping starts netd, which starts vnet
t = time.time()
r = ""
for _ in range(20):                      # (the first replies wait for DHCP)
    r = run("fetch -p 10.0.2.2 1", 30)
    if "reply in" in r: break
net_s = time.time() - t
p = procs()
ok("a ping started netd and vnet", "reply in" in r and {"vnet", "netd"} <= p, f"first reply after {net_s:.2f} s")

# 3. the first question starts siad
t = time.time()
r = run('sia "Say hello in one word."', 300)
sia_s = time.time() - t
ok("a question started siad", "siad" in procs() and len(r.strip()) > 0, f"answered after {sia_s:.2f} s")
loaded_kib = used()
print(f"    memory used with the network and the assistant: {loaded_kib} KiB")

# 4. unused, they stop
for n in ("sia", "netd", "vnet", "auth"):
    run(f"svc idle {n} 1")
time.sleep(SLOW * 5)                            # (stops after 1-2 idle times; vnet after netd)
time.sleep(SLOW * 2)
p = procs()
ok("unused for a while, siad, netd, vnet and auth stopped", not p & {"siad", "netd", "vnet", "auth"}, " ".join(sorted(p)))
s = run("svc")
ok("svc shows them idle again", all(re.search(rf"\n{n}\s+idle", s) for n in ("sia", "netd", "vnet", "auth")))
if fails: print(s)
idle_kib = used()
print(f"    memory used after they stopped: {idle_kib} KiB")

# 5. and the next use starts them again
r = ""
for _ in range(20):
    r = run("fetch -p 10.0.2.2 1", 30)
    if "reply in" in r: break
ok("the next ping starts the network again", "reply in" in r)
r = run('sia "Say yes."', 300)
ok("the next question starts the assistant again", len(r.strip()) > 0 and "error" not in r.lower(), r.strip()[:60])

# 6. calls racing a stopping server: never lost
run("svc add -p racey -i 1 racey /bin/racey always serve racey 1500")
def racey_line(r):
    return next((l.strip() for l in r.splitlines() if l.strip().startswith("racey: ")), r.strip()[-200:])
r = run("racey call racey 200 0", 120)
ok("200 calls in a row: all answered by one server", "0 failed, 1 servers" in r, racey_line(r))
r = run("racey call racey 20 3000", 240)
m = re.search(r"(\d+) failed, (\d+) servers answered, (\d+) calls waited", r)
ok("calls racing the stop: all answered, by new servers", m and m.group(1) == "0" and int(m.group(2)) >= 2 and int(m.group(3)) >= 1,
   racey_line(r))

# 7. a stopped service: its callers get an error at once, not a wait
run("svc stop sia")
t = time.time()
r = run('sia "Hello?"', 30)
ok("a stopped service fails its callers at once", time.time() - t < 5 and "stopped" in r, r.strip()[:80])

send("poweroff")
try:
    qemu.wait(SLOW * 15)
except subprocess.TimeoutExpired:
    qemu.kill()
print(f"memory used (KiB): after boot {boot_kib}, with network and assistant {loaded_kib}, after they stopped {idle_kib}")
print("PASS" if not fails else f"FAIL: {fails} checks")
sys.exit(1 if fails else 0)
