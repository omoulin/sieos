#!/usr/bin/env python3
"""sia's memory (docs/sia.md), on the serial console, headless:
  - a conversation is saved, listed, and continued after `svc restart sia`,
    after an idle stop, and after a reboot (the same disk, booted again);
  - a long conversation is compacted (a summary replaces older turns);
  - facts (`sia remember`) are kept, given to the model, and forgotten;
  - another user sees none of it, and cannot read /var/sia.
Usage: siamemtest.py <cpus> <disk image> <qemu command line...>
The image must be fresh (first start); the test works in a copy.

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
"""
import os, re, select, shutil, subprocess, sys, time
SLOW = float(os.environ.get("SIEOS_SLOW", "1"))   # emulated processors: everything takes longer

cpus, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
copy = image + ".m"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
qemu, out, seen, fails = None, b"", b"", 0
import atexit
atexit.register(lambda: qemu and qemu.poll() is None and qemu.kill())

def boot():
    global qemu, out
    qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = b""

def expect(text, timeout=30):
    """Wait for text; return what came before it."""
    global out, seen
    end = time.time() + timeout * SLOW
    while text.encode() not in out:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            qemu.kill()
            sys.exit(f"FAIL: waiting for {text!r}\n--- output ---\n{out.decode(errors='replace')[-3000:]}")
        if select.select([qemu.stdout], [], [], left)[0]:
            d = qemu.stdout.read1(65536)
            out += d
            seen += d
    i = out.index(text.encode())
    before, out = out[:i], out[i + len(text):]
    return before.decode(errors="replace")

def send(line):
    qemu.stdin.write(line.encode() + b"\r")
    qemu.stdin.flush()

def run(cmd, timeout=60, prompt="# "):
    send(cmd)
    return expect(prompt, timeout)

def ok(name, cond, detail=""):
    global fails
    print(f"{'ok' if cond else 'FAIL'}: {name}{(' (' + detail + ')') if detail else ''}", flush=True)
    fails += not cond

def login(user, pw, prompt):
    expect("login: "); send(user)
    expect("Password: "); send(pw)
    expect(prompt)

def log_since(mark):
    return seen[mark:].decode(errors="replace")

# ---- first boot: accounts, then root
boot()
expect(f"mk: {cpus} CPU")
expect("first start")
for p in ("New password", "Again: "): expect(p); send("rootpass1")
expect("user name"); send("alice")
expect("full name"); send("Alice")
for p in ("New password", "Again: "): expect(p); send("alicepass1")
login("root", "rootpass1", "# ")
# small context and early compaction, so a few questions are enough
run("sia config ctx=512", 60); time.sleep(SLOW * 1)
run("sia config compact_at=40", 60); time.sleep(SLOW * 1)
run("sia config keep_turns=2", 60); time.sleep(SLOW * 1)

r = run('sia new "My name is Robert and I work on the SIEOS file system. Reply with OK."', 300)
ok("a new conversation answers", len(r.strip()) > 0, r.strip()[-80:])
r = run("sia list", 60)
ok("it is saved (sia list)", re.search(r"^\s*1\s+\d+", r, re.M) is not None, r.strip().splitlines()[-1] if r.strip() else "")

r = run('sia remember "The user is called Robert"', 60)
r = run("sia memory", 60)
ok("a fact is kept (sia remember, sia memory)", "Robert" in r, r.strip()[-60:])

# ---- continued after svc restart sia: the history is read back into the model
mark = len(seen)
run("svc restart sia", 60); time.sleep(SLOW * 2)
r = run('sia "What is my name? Answer with the name only."', 300)
log = log_since(mark)
ok("continued after svc restart sia", re.search(r"conversation 1 resumed, [1-9]", log) is not None,
   (re.search(r"siad: uid 0: conversation.*", log) or [""])[0][:90])
ok("the history is read into the model again", "resumed 1 exchanges" in log or "resumed" in log,
   (re.search(r"siad: resumed.*", log) or [""])[0][:90])
ok("the facts are in the system text", re.search(r"system text (\d+) bytes", log) is not None and
   int(re.search(r"system text (\d+) bytes", log).group(1)) > 400)
print("    answer:", r.strip().replace("\r", "")[-100:])

# ---- a long conversation is compacted
mark = len(seen)
for q in ("Name three planets.", "Name three rivers.", "Name three mountains.", "Name three trees.", "Name three birds."):
    run(f'sia "{q} Be very brief."', 300)
    if "compacted conversation" in log_since(mark): break
log = log_since(mark)
m = re.search(r"compacted conversation (\d+): (\d+) -> (\d+) tokens \((\d+) turns summarized\) in (\d+) ms", log)
ok("a long conversation is compacted", m is not None, m.group(0) if m else log[-200:])
r = run('sia "What is my name? Answer with the name only."', 300)
print("    after compaction:", r.strip().replace("\r", "")[-100:])
r = run("sia list", 60)
print("    " + "\n    ".join(l.strip() for l in r.strip().splitlines()[-3:]))

# ---- an idle stop: the next question continues the conversation
run("svc idle sia 1", 30)
time.sleep(SLOW * 4)
mark = len(seen)
r = run("svc", 30)
idle = re.search(r"\nsia\s+(idle|stopped)", r) is not None
r = run('sia "Say yes."', 300)
log = log_since(mark)
ok("continued after an idle stop", idle and "resumed" in log, "sia stopped" if idle else "sia did not stop")
run("svc idle sia 900", 30)

# ---- another user sees none of it
send("logout")
login("alice", "alicepass1", "$ ")
r = run("sia list", 60, "$ ")
ok("alice sees no conversation of root's", "no saved conversation" in r, r.strip()[-60:])
r = run("sia memory", 60, "$ ")
ok("alice sees no fact of root's", "Robert" not in r and "nothing" in r, r.strip()[-60:])
r = run("ls /var/sia", 30, "$ ")
ok("alice cannot read /var/sia", "Robert" not in r and ("denied" in r.lower() or "error" in r.lower() or "not allowed" in r.lower()), r.strip()[-60:])
r = run("cat /var/sia/0/facts", 30, "$ ")
ok("alice cannot read root's facts file", "Robert" not in r, r.strip()[-60:])
send("logout")
login("root", "rootpass1", "# ")
run("sync", 30)
send("poweroff")
qemu.wait(SLOW * 20)

# ---- after a reboot (same disk)
boot()
login("root", "rootpass1", "# ")
r = run("sia list", 60)
ok("the conversations survive a reboot", re.search(r"^\s*1\s+\d+", r, re.M) is not None, r.strip().splitlines()[-1][:60] if r.strip() else "")
r = run("sia memory", 60)
ok("the facts survive a reboot", "Robert" in r)
mark = len(seen)
r = run('sia "Thank you."', 300)
ok("the conversation continues after a reboot", "conversation 1 resumed" in log_since(mark))
r = run("sia memory forget 1", 60)
r = run("sia memory", 60)
ok("sia memory forget", "Robert" not in r, r.strip()[-60:])
r = run("sia forget all", 60)
r = run("sia list", 60)
ok("sia forget all", "no saved conversation" in r, r.strip()[-60:])

for l in seen.decode(errors="replace").splitlines():
    if l.startswith("siad:") and ("compacted" in l or "resumed" in l or "loaded" in l): print(l.strip())
send("poweroff")
try:
    qemu.wait(SLOW * 15)
except subprocess.TimeoutExpired:
    qemu.kill()
print("PASS" if not fails else f"FAIL: {fails} checks")
sys.exit(1 if fails else 0)
