#!/usr/bin/env python3
"""usbtest.py - A USB key image (tools/mkusb.py) booted by UEFI firmware in
QEMU (make usb-test): SIEOS's loader starts the kernel, the desktop shows the
first start on the firmware's screen, accounts are created there, a file is
written; a second boot finds the accounts and the file; then the key's SieFS
partition is checked on the host (fsck.siefs, and the file read back).

Usage: usbtest.py CPUS HOSTTOOLS IMAGE SHOTS QEMU-COMMAND...

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
"""
import atexit, json, os, select, socket, struct, subprocess, sys, tempfile, time

cpus, host, image, shots, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:]
os.makedirs(shots, exist_ok=True)
sock_path = os.path.join(shots, "usb-qmp.sock")
qemu = qf = None
full, pos = b"", 0


def fail(msg):
    if qemu: qemu.kill()
    sys.exit(f"FAIL: {msg}\n--- serial (last part) ---\n{full[-3000:].decode(errors='replace')}")


def boot():
    """Start QEMU (with a control socket); returns once QMP answers."""
    global qemu, qf, full, pos
    if os.path.exists(sock_path):
        os.unlink(sock_path)
    full, pos = b"", 0
    qemu = subprocess.Popen(cmdline + ["-qmp", f"unix:{sock_path},server=on,wait=off"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    atexit.register(lambda p=qemu: p.poll() is None and p.kill())
    for _ in range(100):
        try:
            s = socket.socket(socket.AF_UNIX); s.connect(sock_path); break
        except OSError:
            time.sleep(0.05)
    qf = s.makefile("rw")
    json.loads(qf.readline())
    q("qmp_capabilities")


def wait(text, timeout=30):
    """Wait for `text` on the serial port (after the last match); returns the rest of its line."""
    global full, pos
    end = time.time() + timeout
    while text.encode() not in full[pos:]:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            fail(f"waiting for {text!r}")
        if select.select([qemu.stdout], [], [], left)[0]:
            full += qemu.stdout.read1(4096)
    i = full.index(text.encode(), pos) + len(text)
    time.sleep(0.05)
    while select.select([qemu.stdout], [], [], 0.05)[0]:
        full += qemu.stdout.read1(4096)
    e = full.find(b"\n", i)
    pos = i
    return full[i:e if e >= 0 else len(full)].decode(errors="replace").strip()


def q(cmd, **args):
    qf.write(json.dumps({"execute": cmd, "arguments": args}) + "\n"); qf.flush()
    while True:
        r = json.loads(qf.readline())
        if "return" in r: return r["return"]
        if "error" in r: fail(f"QMP {cmd}: {r['error']}")


KEYS = {" ": "spc", "\n": "ret", "\t": "tab", "\b": "backspace", ".": "dot", "-": "minus"}
def type_text(s):
    for c in s:
        k = ["shift", c.lower()] if c.isupper() else ["shift", "dot"] if c == ">" else [KEYS.get(c, c)]
        q("send-key", keys=[{"type": "qcode", "data": x} for x in k])
        time.sleep(0.02)


def send(line):
    qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()


def shot(name):
    ppm = os.path.join(shots, name + ".ppm")
    q("screendump", filename=ppm)
    q("screendump", filename=os.path.join(shots, name + ".png"), format="png")
    data = open(ppm, "rb").read(); os.unlink(ppm)
    return data.split(b"\n", 3)[3]


def power_off():
    """Log in as root on the serial console and power off (it commits the disk)."""
    send(""); wait("login: ")
    send("root"); wait("Password: ")
    send("rootpass1"); wait("root@")
    send("poweroff")
    try:
        qemu.wait(20)
    except subprocess.TimeoutExpired:
        fail("did not power off")


# ---- 1: first start on the firmware's screen
t0 = time.time()
boot()
wait("SIEOS loader: starting the kernel", 60)
t_kernel = time.time() - t0
wait("mk: started by the UEFI loader")
wait(f"mk: {cpus} CPUs running")
got = wait("vblk: ")                       # (a virtio disk, or the key through the USB driver)
if "SieFS partition" not in got:
    fail(f"the disk driver did not take the key's SieFS partition: {got}")
disk = got
got = wait("atlas: screen: ")
if not got.startswith("firmware screen (UEFI)"):
    fail(f"atlas does not use the firmware's screen: {got}")
wait("atlas: first start: accounts needed (setup on the screen)")
wait("atlas: greeter")
t_screen = time.time() - t0
px = shot("usb-01-welcome")
if px.count(bytes.fromhex("0a0f1c")) < 100000:
    fail("the welcome screen does not look right")
print(f"ok: UEFI boot from the key ({disk}): kernel after {t_kernel:.2f} s, welcome screen after {t_screen:.2f} s ({got})")
type_text("rootpass1\trootpass1\n"); wait("atlas: setup: step 2")
type_text("alice\tAlice Example\talicepass1\talicepass1\n"); wait("atlas: setup: step 3")
type_text("\n"); wait("atlas: setup: accounts created")
wait("atlas: login alice: ok")
wait("atlas: stage ")
time.sleep(0.5)
type_text("echo written on the key > keep.txt\n")
wait("atlas: projects: ")
shot("usb-02-session")
print("ok: accounts created on the screen, alice's file written")
power_off()

# ---- 2: a second boot finds them (the greeter, not the first start)
boot()
wait("atlas: greeter", 60)
if b"accounts needed" in full:
    fail("the second boot asked for the first start again: the accounts were not kept")
type_text("alice\talicepass1\n")
wait("atlas: login alice: ok")
print("ok: second boot: the accounts were kept (alice logged in)")
power_off()

# ---- 3: the key on the host: its SieFS partition checked, the file read back
with open(image, "rb") as f:
    f.seek(512); h = f.read(92)
    lba, n, size = struct.unpack_from("<QII", h, 72)
    f.seek(lba * 512); ents = f.read(n * size)
    first = last = None
    for i in range(n):
        e = ents[i * size:(i + 1) * size]
        if e[:16] == bytes.fromhex("00051e5eef5153419e05534945465331"):     # SieFS's partition type
            first, last = struct.unpack_from("<QQ", e, 32)
    if first is None:
        fail("no SieFS partition on the key")
    with tempfile.NamedTemporaryFile(dir=os.path.dirname(image), delete=True) as part:
        f.seek(first * 512)
        left = (last + 1 - first) * 512
        while left:                                   # (a sparse copy: zeros stay holes)
            b = f.read(min(1 << 20, left))
            if b.strip(b"\0"): part.write(b)
            else: part.seek(len(b), 1)
            left -= len(b)
        part.truncate((last + 1 - first) * 512); part.flush()
        r = subprocess.run([os.path.join(host, "fsck.siefs"), part.name], capture_output=True, text=True)
        if r.returncode:
            fail(f"fsck.siefs: {r.stdout}{r.stderr}")
        c = subprocess.run([os.path.join(host, "siefs"), part.name, "cat", "/home/alice/keep.txt"], capture_output=True, text=True)
        if "written on the key" not in c.stdout:
            fail(f"alice's file is not on the key: {c.stdout}{c.stderr}")
print(f"ok: the key's SieFS partition checks clean ({r.stdout.strip().splitlines()[-1].split(': ')[-1]}); alice's file is there")
print("PASS")
