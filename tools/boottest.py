#!/usr/bin/env python3
"""Boot SIEOS in QEMU without a window and drive its serial console: the
first-start setup, logins, what users may and may not do, the files; then
check the disk with the host tools, and boot a second time to see that the
accounts and files stayed.

Usage: boottest.py <cpus> <host tools dir> <disk image> <qemu command line...>
(see make test: the image is a fresh one, with no accounts yet). The test
works in a copy of it.

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
"""
import os, select, shutil, subprocess, sys, time
SLOW = float(__import__("os").environ.get("SIEOS_SLOW", "1"))   # emulated processors (arm64 on a PC): everything takes longer

cpus, tools, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
copy = image + ".test"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
qemu, out, prompt = None, b"", "# "

def boot():
    global qemu, out
    out = b""
    qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

def expect(text, timeout=20):
    """Wait until `text` appears in the output (after what was already matched)."""
    timeout *= SLOW
    global out
    end = time.time() + timeout
    while text.encode() not in out:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            qemu.kill()
            sys.exit(f"FAIL: waiting for {text!r}\n--- output ---\n{out.decode(errors='replace')}")
        if select.select([qemu.stdout], [], [], left)[0]:
            out += qemu.stdout.read1(4096)
    out = out[out.index(text.encode()) + len(text):]

def send(line):
    qemu.stdin.write(line.encode() + b"\r")
    qemu.stdin.flush()

def check(cmd, *answers, timeout=20):
    timeout *= SLOW
    send(cmd)
    for a in answers:
        expect(a, timeout)
    expect(prompt, timeout)
    print(f"ok: {cmd}")

def login(name, pw, ok=True):
    """Log in at the login prompt; returns the time from password to prompt."""
    global prompt
    expect("login: ")
    send(name)
    expect("Password: ")
    t = time.time()
    send(pw)
    if not ok:
        expect("Login incorrect.")
        print(f"ok: wrong password for {name} refused")
        return 0
    prompt = "# " if name == "root" else "$ "
    expect(f"{name}@sieos:")
    expect(prompt)
    t = time.time() - t
    print(f"ok: logged in as {name} ({t:.2f} s)")
    return t

def logout():
    send("logout")
    expect("login: ")
    send("")                            # an empty name: the prompt comes again
    print("ok: logout")

def new_password(pw):
    for p in ("New password", "Again: "):
        expect(p)
        send(pw)

def poweroff():
    send("poweroff")
    try:
        qemu.wait(SLOW * 5)
        print("ok: poweroff")
    except subprocess.TimeoutExpired:
        qemu.kill()
        sys.exit("FAIL: QEMU did not power off")

def host(*args):
    r = subprocess.run([os.path.join(tools, args[0]), *args[1:]], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr

# ---- first boot: the first-start setup
start = time.time()
boot()
expect(f"mk: {cpus} CPU")
print(f"ok: {cpus} CPUs running")
expect(": no desktop")    # no display card (or no screen driver: arm64): the desktop leaves,
expect("no screen, the terminal does it")             # and auth gives the first start to login
expect("first start, and there is no screen")
print(f"ok: first start on the terminal (no screen), after {time.time() - start:.2f} s")
expect("New password"); send("short")                 # the rule: at least 8 characters
expect("At least 8 characters")
new_password("rootpass1")
expect("user name"); send("alice")
expect("full name"); send("Alice Example")
new_password("alicepass1")
expect("You can log in now")
print("ok: root and alice created")

# ---- alice: what a user may and may not do
times = [login("alice", "alicepass1")]
check("whoami", "alice")
check("id", "uid=1000(alice)", "100(users)")
check("cat /etc/secrets", "permission denied")
check("echo x > /etc/evil", "permission denied")
check("ls /root", "permission denied")
check("echo hello alice > note.txt")
check("cat /home/alice/note.txt", "hello alice")
check("echo mine > /tmp/alice.txt")
check("hello", "uid 1000")                         # programs run as the user
check("poweroff", "not allowed")
send("passwd")
expect("Current password: "); send("alicepass1")
new_password("alicepass2")
expect("Password changed."); expect(prompt)
print("ok: passwd")
logout()
login("alice", "alicepass1", ok=False)
times.append(login("alice", "alicepass2"))
logout()

# ---- root: the system, the files, a second user, su
times.append(login("root", "rootpass1"))
check("svc", "atlas", "stopped")                 # no display: the desktop stopped once, cleanly
print("ok: no screen: the desktop stopped cleanly (not restarted)")
for cmd, answer in [("echo hello world", "hello world"), ("ps", "auth"), ("mem", f"processors: {cpus}"),
                    ("uptime", f"{cpus} CPUs"), ("nonsense", "unknown command")]:
    check(cmd, answer)
t = time.time()
send("sleep 0.5")                       # the tickless timer must wake it, not too early
expect(prompt)
t = time.time() - t
if not 0.5 <= t < 1.0:
    sys.exit(f"FAIL: sleep 0.5 took {t:.3f} s")
print(f"ok: sleep 0.5 ({t:.3f} s)")
check("mkdir /root/test")
check("cd /root/test")
check("echo first line > notes.txt")
check("echo second line >> notes.txt")
check("mkdir sub")
check("mv notes.txt sub/notes.txt")
check("ln -s sub/notes.txt link")
check("cat link", "first line", "second line")
check("tag sub/notes.txt project=SIEOS")
check("find project=SIEOS", "/root/test/sub/notes.txt")
check("echo for the group > /tmp/grp.txt")
check("chown 0:100 /tmp/grp.txt")
check("chmod 640 /tmp/grp.txt")
send('useradd bob "Bob Builder"')
new_password("bobpass11")
expect("uid 1001"); expect(prompt)
print("ok: useradd bob")
send("useradd carl")
new_password("carlpass1")
expect("uid 1002"); expect(prompt)
print("ok: useradd carl")
check("userdel carl", "removed")
check("ls -l /home", "bob", "alice")

send("su bob")                          # root needs no password
prompt = "$ "
expect("bob@sieos:"); expect(prompt)
print("ok: su bob")
check("whoami", "bob")
check("cat /tmp/grp.txt", "for the group")         # through his group "users"
check("cat /home/alice/note.txt", "permission denied")
check("rm /tmp/alice.txt", "not allowed")          # /tmp is sticky
send("su alice")
expect("Password: "); send("wrong")
expect("wrong password"); expect(prompt)
print("ok: su with a wrong password refused")
send("exit")
prompt = "# "
expect("root@sieos:"); expect(prompt)
print("ok: back to root")
check("sync")
poweroff()

# ---- the disk, seen from the host
code, text = host("fsck.siefs", copy)
if code:
    sys.exit(f"FAIL: fsck.siefs found problems:\n{text}")
print("ok: fsck.siefs: clean")
code, text = host("siefs", copy, "cat", "/home/alice/note.txt")
if code or "hello alice" not in text:
    sys.exit(f"FAIL: the host does not see alice's file:\n{text}")
code, text = host("siefs", copy, "cat", "/etc/secrets")
if code or text.count("argon2id") != 3 or "carl" in text:
    sys.exit(f"FAIL: /etc/secrets is not as expected:\n{text}")
print("ok: the host sees the files; /etc/secrets holds root, alice, bob")

# ---- second boot: the accounts and files are still there
boot()
login("alice", "alicepass2")
check("cat note.txt", "hello alice")
logout()
login("bob", "bobpass11")
check("groups", "bob users")
logout()
login("root", "rootpass1")
check("cat /root/test/sub/notes.txt", "first line", "second line")
check("find project=SIEOS", "/root/test/sub/notes.txt")
check("ps", "login", "auth")          # (auth starts on demand: after login)
poweroff()
code, text = host("fsck.siefs", copy)
if code:
    sys.exit(f"FAIL: fsck.siefs after the second boot:\n{text}")
print("ok: fsck.siefs after the second boot: clean")
print(f"login (password check + starting the shell): {min(times):.2f}-{max(times):.2f} s")
os.remove(copy)
print("PASS")
