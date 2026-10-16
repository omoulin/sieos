#!/usr/bin/env python3
"""The SSH test (docs/ssh.md): boot SIEOS with sshd turned on (first-start
setup on a fresh disk), then, against OpenSSH:
  - the host's ssh to SIEOS's sshd (QEMU forwards a host port to its port 22):
    host key accepted and checked again, a wrong password refused, commands
    with a password (the exit status comes back) and with a key, an
    interactive session on a terminal, a password-protected key refused;
  - SIEOS's ssh-keygen and ssh to an sshd run on the host (as this user, its own
    host key, seen from SIEOS at 10.0.2.2): first sight asked, a command run with
    the key, an interactive shell on the console (raw keys), the same host with
    another key refused;
  - SIEOS's ssh to its own sshd (through the host's forwarded port: netd has no
    loopback), interactively, with a password.

Usage: sshtest.py <cpus> <host tools dir> <disk image> <qemu command line...>
(see make ssh-test). The host part needs OpenSSH's ssh, ssh-keygen and sshd.

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import getpass, os, re, select, shutil, socket, subprocess, sys, tempfile, time
import qserial                                      # (tools/qserial.py: typing on the console)
SLOW = float(os.environ.get("SIEOS_SLOW", "1"))     # emulated processors (arm64 on a PC): everything takes longer

cpus, tools, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
copy = image + ".ssh"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
siefs = os.path.join(tools, "siefs")
tmp = tempfile.mkdtemp()
open(os.path.join(tmp, "sshd_config"), "w").write("# sshd is on (docs/ssh.md)\n")
subprocess.run([siefs, copy, "mkdir", "/etc/ssh"], check=True)
subprocess.run([siefs, copy, "put", os.path.join(tmp, "sshd_config"), "/etc/ssh/sshd_config"], check=True)

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
fport = free_port()                                 # host -> SIEOS's port 22
cmdline = [a.replace("user,id=n0", f"user,id=n0,hostfwd=tcp:127.0.0.1:{fport}-:22") for a in cmdline]

qemu, out, log, servers = None, b"", [], []
def expect(text, timeout=30):
    global out
    end = time.time() + timeout * SLOW
    while text.encode() not in out:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            qemu.kill()
            open(copy + ".log", "wb").write(b"".join(log) + out)
            sys.exit(f"FAIL: waiting for {text!r} (all of the console: {copy}.log)\n--- output ---\n{(b"".join(log) + out).decode(errors="replace")[-3000:]}")
        if select.select([qemu.stdout], [], [], left)[0]:
            out += qemu.stdout.read1(4096)
    i = out.index(text.encode())
    seen = out[:i + len(text)]
    log.append(seen)
    out = out[i + len(text):]
    return seen.decode(errors="replace")
def send(line): qserial.type_line(qemu, line)
prompt = "$ "
def check(cmd, *answers, timeout=30):
    send(cmd)
    seen = "".join(expect(a, timeout) for a in answers) + expect(prompt, timeout)
    print(f"ok: {cmd}")
    return seen
def fail(why):
    qemu.kill()
    sys.exit("FAIL: " + why)

# ---- OpenSSH on the host, as a client: a password from SSH_ASKPASS, our own known_hosts
askpass = os.path.join(tmp, "askpass")
open(askpass, "w").write('#!/bin/sh\necho "$SSHTEST_PW"\n')
os.chmod(askpass, 0o755)
kh = os.path.join(tmp, "known_hosts")
def ossh(*args, pw=None, key=None, inp=None, tty=False, timeout=60):
    env = dict(os.environ, SSH_ASKPASS=askpass, SSH_ASKPASS_REQUIRE="force", SSHTEST_PW=pw or "", DISPLAY=":0")
    opts = ["-F", "/dev/null", "-p", str(fport), "-o", f"UserKnownHostsFile={kh}", "-o", "StrictHostKeyChecking=accept-new",
            "-o", "IdentityAgent=none", "-o", "IdentitiesOnly=yes", "-o", "NumberOfPasswordPrompts=1",
            "-o", "ConnectTimeout=30", "-o", "LogLevel=ERROR"]
    opts += ["-i", key, "-o", "PreferredAuthentications=publickey"] if key else ["-o", "PreferredAuthentications=password", "-o", "PubkeyAuthentication=no"]
    if tty: opts.append("-tt")
    return subprocess.run(["ssh", *opts, *args], input=inp, capture_output=True, timeout=timeout * SLOW, env=env,
                          stdin=None if inp is not None else subprocess.DEVNULL)

qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
import atexit; atexit.register(lambda: [p.kill() for p in [qemu] + servers])
expect("first start")
for p in ("New password", "Again: "): expect(p); send("rootpass1")
expect("user name"); send("alice")
expect("full name"); send("Alice Example")
for p in ("New password", "Again: "): expect(p); send("alicepass1")
expect("You can log in now")
expect("login: ")
m = re.search(rb"sshd: listening on port 22, host key (SHA256:\S+)", b"".join(log))   # (any time during the setup)
fp = m.group(1).decode() if m else fail("sshd did not start")
print(f"ok: sshd started, made its host key {fp}")
send("alice"); expect("Password: "); send("alicepass1"); expect(prompt)
print("ok: alice logged in on the console")

# ---- the host's ssh to SIEOS's sshd
r = ossh("alice@127.0.0.1", "echo hello over ssh", pw="alicepass1")
if r.returncode or b"hello over ssh" not in r.stdout: fail(f"ssh with a password: {r}")
line = open(kh).read()
want = subprocess.run(["ssh-keygen", "-l", "-f", kh], capture_output=True, text=True).stdout
if fp not in want: fail(f"known_hosts has another key: {want!r}, sshd said {fp}")
print(f"ok: host ssh -> sshd, password, command (host key {fp} saved, the one sshd made)")
r = ossh("alice@127.0.0.1", "whoami", pw="alicepass1")
if r.returncode or r.stdout.strip() != b"alice": fail(f"second connection (host key checked): {r}")
print("ok: second connection, host key checked against known_hosts")
r = ossh("alice@127.0.0.1", "echo nope", pw="wrongpass9")
if r.returncode != 255 or b"Permission denied" not in r.stderr: fail(f"a wrong password was not refused: {r}")
print("ok: wrong password refused")
r = ossh("root@127.0.0.1", "echo nope", pw="rootpass1")
if r.returncode != 255: fail(f"root logged in with a password: {r}")
print("ok: root refused with a password")
r = ossh("alice@127.0.0.1", "cat /etc/missing", pw="alicepass1")
if b"no such file" not in r.stdout: fail(f"command output: {r}")
r = ossh("alice@127.0.0.1", "/bin/ssh", pw="alicepass1")
if r.returncode != 255 or b"usage: ssh" not in r.stdout: fail(f"exit status: {r}")
print("ok: a command's exit status comes back (255)")

# a key: made on the host, put in SIEOS's ~/.ssh/authorized_keys from the console
key = os.path.join(tmp, "id_host")
subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "host@test", "-f", key], check=True)
check("mkdir .ssh")
check("echo " + open(key + ".pub").read().strip() + " > .ssh/authorized_keys")
r = ossh("alice@127.0.0.1", "pwd", key=key)
if r.returncode or r.stdout.strip() != b"/home/alice": fail(f"ssh with a key: {r}")
print("ok: host ssh -> sshd with an Ed25519 key (runs at home)")
other = os.path.join(tmp, "id_other")
subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", other], check=True)
r = ossh("alice@127.0.0.1", "pwd", key=other)
if r.returncode != 255: fail(f"a key not in authorized_keys was taken: {r}")
print("ok: a key not in authorized_keys refused")

# an interactive session on a terminal: typed lines, echoed, CR LF line ends
r = ossh("alice@127.0.0.1", key=key, tty=True, inp=b"echo typed-$((1))\rls -l /bin/ssh\rexit\r", timeout=60)
o = r.stdout.decode(errors="replace")
if "alice@" not in o or "typed-$((1))\r\n" not in o or "/bin/ssh" not in o or r.returncode: fail(f"interactive session: {r}")
print("ok: interactive session (pty, echo, exit)")

# ---- SIEOS's ssh-keygen, and ssh to an sshd on the host (this user, its own host key)
hport = free_port()
hkey = os.path.join(tmp, "host_key")
subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", hkey], check=True)
auth = os.path.join(tmp, "authorized_keys")
def host_sshd(hk):
    cfg = os.path.join(tmp, "hsshd_config")
    open(cfg, "w").write(f"Port {hport}\nListenAddress 127.0.0.1\nHostKey {hk}\nAuthorizedKeysFile {auth}\nPidFile {tmp}/sshd.pid\n"
                         "UsePAM no\nStrictModes no\nPasswordAuthentication no\nKbdInteractiveAuthentication no\n"
                         # (a plain sh, not this user's own shell and its settings)
                         'ForceCommand exec /bin/sh -c "${SSH_ORIGINAL_COMMAND:-exec /bin/sh -i}"\n')
    servers.append(subprocess.Popen(["/usr/sbin/sshd", "-D", "-e", "-f", cfg], stdout=subprocess.DEVNULL, stderr=open(os.path.join(tmp, "sshd.log"), "a")))
    return servers[-1]
seen = check("ssh-keygen", "Your public key has been saved in /home/alice/.ssh/id_ed25519.pub", "alice@sieos")
pubkey = re.search(r"ssh-ed25519 \S+ alice@sieos", seen).group(0)
open(auth, "w").write(pubkey + "\n")
check("ssh-keygen", "already exists")
r = subprocess.run(["ssh-keygen", "-l", "-f", auth], capture_output=True, text=True)
if r.returncode: fail(f"OpenSSH does not read SIEOS's public key: {r}")
hsshd = host_sshd(hkey)
time.sleep(1)
me = getpass.getuser()
send(f"ssh -p {hport} {me}@10.0.2.2 echo hello from the host")
expect("Are you sure you want to continue connecting (yes/no)? ", 60); send("yes")
expect("Permanently added", 30); expect("hello from the host", 60); expect(prompt)
print("ok: SIEOS ssh -> host sshd: first sight asked, command run with SIEOS's key")
check(f"ssh -p {hport} {me}@10.0.2.2 uname -s", "Linux", timeout=60)
print("ok: second connection, host key checked")
send(f"ssh -p {hport} {me}@10.0.2.2")
expect("$ ", 60)
send("echo inner shell"); expect("\ninner shell", 30)
send("exit"); expect("Connection to 10.0.2.2 closed.", 30); expect(prompt)
check("echo console still works", "console still works")
print("ok: SIEOS ssh -> host sshd, interactive (raw console, a terminal there); the console is back")
hsshd.kill(); hsshd.wait()
hkey2 = os.path.join(tmp, "host_key2")
subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", hkey2], check=True)
hsshd = host_sshd(hkey2)
time.sleep(1)
seen = check(f"ssh -p {hport} {me}@10.0.2.2 echo should-not-run", "HAS CHANGED", timeout=60)
if "should-not-run\n" in seen.replace("echo should-not-run", ""): fail("ran despite a changed host key")
print("ok: a changed host key refused")
hsshd.kill()

# ---- SIEOS's ssh to its own sshd (10.0.2.2:fport is QEMU's forward back to our port 22),
# interactively, with a password (alice's key is not in her authorized_keys)
send(f"ssh -p {fport} alice@10.0.2.2")
expect("(yes/no)? ", 60); send("yes")
expect(f"alice@10.0.2.2's password: ", 60); send("alicepass1")
expect("alice@sieos:/home/alice$ ", 60)
send("echo nested shell"); expect("\nnested shell", 30)
send("exit"); expect("Connection to 10.0.2.2 closed.", 30); expect(prompt)
print("ok: SIEOS ssh -> SIEOS sshd, interactive, password")
check(f"ssh -p {free_port()} 10.0.2.2", "connection refused")

send("exit"); expect("login: ")
qemu.kill()
print("PASS")
