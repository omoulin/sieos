#!/usr/bin/env python3
"""Test SIEOS on a USB key, through QEMU (an xHCI controller, a USB disk as
SIEOS's only disk, a USB keyboard) and its control socket (QMP):
  - boot with the disk on USB only (no virtio disk): vblk takes the key;
  - the first start's root password typed on the USB keyboard;
  - bench on the USB disk (MB/s);
  - fsloop while the USB server is killed: it comes back, the disk with it;
  - a second USB disk plugged, unplugged and plugged again (same port name);
  - power off, fsck.siefs on the host, a second boot: the files are there.
Usage: usbdevtest.py <cpus> <host tools dir> <disk image> <qemu command...>
(the command without disk; see make usb-dev-test).

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import json, os, re, select, shutil, socket, subprocess, sys, time

cpus, host, image, base = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
SLOW = 1 if "kvm" in " ".join(base) else 8          # emulated processors: give them time
copy, extra = image + ".usbtest", image + ".usb2"
shutil.copyfile(image, copy)
subprocess.run([f"{host}/mkfs.siefs", "-s", "16M", "-L", "DATA", extra], check=True, stdout=subprocess.DEVNULL)
sock = image + ".qmp"
cmdline = base + ["-device", "qemu-xhci,id=x", "-device", "usb-kbd,id=kbd",
                  "-drive", f"file={copy},if=none,id=u0,format=raw", "-device", "usb-storage,drive=u0,id=ud0,serial=SIEOSKEY0",
                  "-drive", f"file={extra},if=none,id=u1,format=raw",
                  "-qmp", f"unix:{sock},server=on,wait=off"]
qemu, out, seen, qmp = None, b"", b"", None

def fail(msg):
    if qemu: qemu.kill()
    sys.exit("FAIL: " + msg + "\n--- output ---\n" + (seen + out).decode(errors="replace")[-3000:])

def boot():
    global qemu, out, seen, qmp
    out, seen, qmp = b"", b"", None
    if os.path.exists(sock): os.unlink(sock)
    qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

def expect(text, timeout=20, must=True):
    global out, seen
    end = time.time() + timeout * SLOW
    while text.encode() not in out:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            if must: fail(f"waiting for {text!r}")
            return None
        if select.select([qemu.stdout], [], [], left)[0]:
            out += qemu.stdout.read1(4096)
    i = out.index(text.encode()) + len(text)
    before, seen, out = out[:i], seen + out[:i], out[i:]
    return before.decode(errors="replace")

def send(line):
    qemu.stdin.write(line.encode() + b"\r")
    qemu.stdin.flush()

def q(cmd, **args):
    global qmp
    if not qmp:
        for _ in range(50):
            try:
                qmp = socket.socket(socket.AF_UNIX); qmp.connect(sock); break
            except OSError:
                time.sleep(0.1)
        qmp.recv(4096)
        q("qmp_capabilities")
    qmp.send(json.dumps({"execute": cmd, "arguments": args}).encode())
    while True:
        r = json.loads(qmp.recv(65536).decode().splitlines()[0])
        if "return" in r or "error" in r: return r

def usb_type(text):
    """Type on the USB keyboard (QMP send-key: the key goes down, then up)."""
    for ch in text:
        key = "ret" if ch == "\n" else ch
        q("send-key", keys=[{"type": "qcode", "data": key}])
        time.sleep(0.05)

def run(cmd, prompt="# ", timeout=30):
    send(cmd)
    return expect(prompt, timeout)

def svc_pid(name):
    m = re.search(rf"\n{name}\s+running\s+(\d+)", run("svc"))
    return int(m.group(1)) if m else None

# ---- first boot: SIEOS's disk is the USB key
start = time.time()
boot()
expect(f"mk: {cpus} CPU")
expect("vblk: USB disk usbdisk0")
print(f"ok: vblk took the USB key as SIEOS's disk, {time.time() - start:.2f} s after start")
expect("New password")
usb_type("rootpass1\n")                       # on the USB keyboard
expect("Again: ")
usb_type("rootpass1\n")
print("ok: root's password typed on the USB keyboard")
expect("user name"); send("alice")
expect("full name"); send("Alice")
for p in ("New password", "Again: "):
    expect(p); send("alicepass1")
expect("login: "); send("root")
expect("Password: "); send("rootpass1")
expect("root@sieos:"); expect("# ")
print("ok: first start done, root logged in")

text = run("bench 16", timeout=120)
speeds = re.findall(r"(write \+ commit|read \(from the disk\))\s+([\d.]+) MiB/s", text)
print("ok: bench on the USB disk: " + ", ".join(f"{k} {v} MiB/s" for k, v in speeds))
run("echo kept on the key > /root/usbnote.txt")
run("sleep 1.2")                               # (older than 1 s: a kill is not a "quick" failure)

# ---- the USB server killed while fsloop works on the USB disk
send("fsloop /tmp/loop 200 &")
expect("# ")
time.sleep(0.5 * SLOW)
pid = svc_pid("usb")
if not pid: fail("usb is not running")
send(f"kill {pid}")
expect("usb: usbdisk0:", 30)                   # the restarted server found the key again
end = time.time() + 180 * SLOW
while not re.search(rb"fsloop: 200 rounds.*\n", seen + out) and time.time() < end:
    if select.select([qemu.stdout], [], [], 0.5)[0]:
        out += qemu.stdout.read1(4096)
m = re.search(rb"fsloop: 200 rounds.*", seen + out)
if not m or b"OK" not in m.group(0):
    fail(m.group(0).decode() if m else "fsloop did not finish")
print(f"ok: usb (pid {pid}) killed during fsloop, back with the disk: " + m.group(0).decode().strip())

# ---- a second USB disk: plugged, unplugged, plugged again
r = q("device_add", driver="usb-storage", drive="u1", id="ud1", serial="SIEOSKEY1")
if "error" in r: fail(f"device_add: {r}")
expect("usb: usbdisk1:", 20)
print("ok: a second USB disk plugged in: usbdisk1")
q("device_del", id="ud1")
expect("usbdisk1 unplugged", 20)
print("ok: unplugged")
# (QEMU finishes removing the device, and maybe its drive, a moment later)
for _ in range(50):
    r = q("device_add", driver="usb-storage", drive="u1", id="ud1", serial="SIEOSKEY1")   # the same key: same serial
    if "error" not in r: break
    if "find value" in r["error"]["desc"] or "not found" in r["error"]["desc"]:
        q("human-monitor-command", **{"command-line": f"drive_add 0 file={extra},if=none,id=u1,format=raw"})
    time.sleep(0.1)
if "error" in r: fail(f"device_add: {r}")
expect("usbdisk1 is back", 20)
print("ok: plugged in again: the same disk, the same port")

run("sync")
send("poweroff")
qemu.wait(30)
r = subprocess.run([f"{host}/fsck.siefs", copy], capture_output=True, text=True)
if r.returncode: fail("fsck.siefs: " + r.stdout + r.stderr)
r = subprocess.run([f"{host}/siefs", copy, "cat", "/root/usbnote.txt"], capture_output=True, text=True)
if "kept on the key" not in r.stdout: fail("the host does not see the file on the key")
print("ok: fsck.siefs clean; the file is on the key")

# ---- second boot from the key
boot()
expect("vblk: USB disk usbdisk0")
expect("login: "); send("root")
expect("Password: "); send("rootpass1")
expect("# ")
if "kept on the key" not in run("cat /root/usbnote.txt"): fail("the file did not survive the reboot")
print("ok: second boot from the key: the file is still there")
send("poweroff")
qemu.wait(30)
for f in (copy, extra, sock):
    if os.path.exists(f): os.unlink(f)
print("PASS")
