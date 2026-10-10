#!/usr/bin/env python3
"""Boot SIEOS without a window and check, as root on the serial console:
  kernel  floating-point and AVX registers per thread, thread-local
          variables (programs built by gcc and by sicc), memory quotas
  sia     the assistant: a question, two sessions at once, sia killed in
          the middle of an answer (init restarts it), speed and memory
Usage: ktest.py <kernel|sia> <cpus> <disk image> <qemu command line...>
The image must be a fresh one (first-start setup); the test works in a copy.

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
"""
import re, select, shutil, subprocess, sys, time
SLOW = float(__import__("os").environ.get("SIEOS_SLOW", "1"))   # emulated processors (arm64 on a PC): everything takes longer

what, cpus, image, cmdline = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4:]
A64 = "aarch64" in " ".join(cmdline)           # an arm64 system: no AVX test
copy = image + ".k"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
import atexit
atexit.register(lambda: qemu.poll() is None and qemu.kill())     # never leave a QEMU behind
out = b""
seen = b""                                  # everything, for the server's log lines

def expect(text, timeout=30):
    """Wait for text (a string, or a compiled regex); return what came before it."""
    timeout *= SLOW
    global out, seen
    end = time.time() + timeout
    while True:
        m = text.search(out) if hasattr(text, "search") else None
        if m or (not hasattr(text, "search") and text.encode() in out):
            break
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            qemu.kill()
            sys.exit(f"FAIL: waiting for {text!r}\n--- output ---\n{out.decode(errors='replace')[-3000:]}")
        if select.select([qemu.stdout], [], [], left)[0]:
            d = qemu.stdout.read1(65536)
            out += d
            seen += d
    if m:
        before, out = out[:m.start()], out[m.end():]
        return before.decode(errors="replace"), m
    i = out.index(text.encode())
    before, out = out[:i], out[i + len(text):]
    return before.decode(errors="replace")

def send(line):
    qemu.stdin.write(line.encode() + b"\r")
    qemu.stdin.flush()

def run(cmd, timeout=60):
    """A command at the root prompt; returns its output."""
    timeout *= SLOW
    send(cmd)
    return expect("# ", timeout)

expect(f"mk: {cpus} CPU")
expect("first start")
for p in ("New password", "Again: "): expect(p); send("rootpass1")
expect("user name"); send("alice")
expect("full name"); send("Alice")
for p in ("New password", "Again: "): expect(p); send("alicepass1")
expect("login: "); send("root")
expect("Password: "); send("rootpass1")
expect("root@sieos:"); expect("# ")
fails = 0

def ok(name, cond, detail=""):
    global fails
    print(f"{'ok' if cond else 'FAIL'}: {name}{(' (' + detail + ')') if detail else ''}")
    fails += not cond

if what == "kernel":
    for prog in ("ktest", "ktest-sicc"):
        for t in (["fpu", "24"], ["avx", "24"], ["tls", "16"], ["quota"], ["ipc"]):
            if t[0] == "avx" and (prog == "ktest-sicc" or A64):   # (AVX: x86-64, built by gcc)
                continue
            r = run(f"{prog} {' '.join(t)}", 120)
            lines = [l.strip() for l in r.splitlines() if l.strip().startswith("ktest ") and ":" in l]
            for l in lines[:-1]: print("   ", l)
            ok(f"{prog} {t[0]}", f"ktest {t[0]}: ok" in r, lines[0] if lines else r.strip()[-120:])
    # two processes computing at once (each with its threads)
    send("ktest fpu 12 &")
    send("ktest fpu 12")
    res = []
    for _ in range(2):
        res.append(expect(re.compile(rb"ktest fpu: (ok|FAIL)"), 120)[1].group(1))
    ok("two processes computing at once", res == [b"ok", b"ok"])
    run("", 10)

elif what == "sia":
    t = time.time()
    r = run('sia "What is the capital of France? Answer in one word."', 300)
    ok("sia answers", "Paris" in r, r.strip().replace("\r", "")[-200:])
    r = run("sia info", 30)
    print(r.strip())
    m = re.search(r"([\d.]+) tok/s", r)
    ok("sia info shows the speed", m is not None, m.group(0) if m else "")
    # two conversations at once: one in the background, one in the foreground
    send('sia Count from 1 to 5, digits only. &')
    expect("# ", 30)                      # the prompt comes back at once
    text = run('sia Name three colours of the rainbow.', 300) + run('sleep 10', 60)
    ok("two conversations at once", len(text) > 120 and any(c in text.lower() for c in ("red", "blue", "green", "yellow")),
       " / ".join(l.strip() for l in text.splitlines() if l.strip())[:160])
    # kill sia during an answer: the client gets an error, init restarts it
    send('sia "Write a long story about a microkernel." &')
    expect("# ", 30)
    time.sleep(SLOW * 3)
    r = run("svc", 30)
    m = re.search(r"\nsia\s+\S+\s+(\d+)", r)
    pid = m.group(1) if m else "0"
    run(f"kill {pid}", 30)
    time.sleep(SLOW * 4)
    r = run("svc", 30)
    ok("sia restarted by init", re.search(r"\nsia\s+running", r) is not None, r.strip().splitlines()[-1][:80] if r.strip() else "")
    r = run('sia "Say hello."', 300)
    ok("sia answers after its restart", len(r.strip()) > 0)
    r = run("ps", 30)
    print(r.strip())

for l in seen.decode(errors="replace").splitlines():
    if l.startswith("siad:"): print(l.strip())
send("poweroff")
try:
    qemu.wait(SLOW * 15)
except subprocess.TimeoutExpired:
    qemu.kill()
print("PASS" if not fails else f"FAIL: {fails} checks")
sys.exit(1 if fails else 0)
