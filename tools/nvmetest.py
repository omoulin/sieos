#!/usr/bin/env python3
"""The NVMe driver (user/vblk/nvme.c) on QEMU's emulated NVMe drives: a
blank drive with 4 KiB sectors first (listed, skipped: no SieFS), then a
drive with 512-byte sectors, 32 KiB per command at most (MDTS 3: vblk cuts
transfers), and a GPT whose SieFS partition starts at
1 MiB. SIEOS must take that partition: first start on the serial console,
`bench`, a file written and read back, power off; then the partition is
checked on the host (fsck.siefs, the file read back). Used by `make nvme-test`.

Usage: nvmetest.py <host tools dir> <SieFS image> <MiB> <qemu command line...>

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import os, select, subprocess, sys, time, uuid
sys.path.insert(0, os.path.dirname(__file__))
import qserial                                      # typing on the console
from mkusb import gpt, copy_sparse, SIEFS_TYPE      # the key image's GPT writer

tools, siefs, mib, cmdline = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
d = os.path.dirname(siefs)
disk, blank = os.path.join(d, "nvme.img"), os.path.join(d, "nvme-blank.img")
S, FIRST = 512, 2048                                # sectors; the partition at 1 MiB
psize = os.path.getsize(siefs)
total = FIRST + psize // S + 2048
h1, ent, h2, ent2 = gpt(total, [(SIEFS_TYPE, FIRST, FIRST + psize // S - 1, "SIEOS")], uuid.uuid4())
with open(disk, "wb") as f:
    f.truncate(total * S)
    f.seek(S); f.write(h1); f.write(ent)
    f.seek(ent2 * S); f.write(ent); f.write(h2)
    copy_sparse(siefs, f, FIRST * S)
with open(blank, "wb") as f:
    f.truncate(64 << 20)
cmdline += ["-drive", f"file={blank},if=none,id=nb,format=raw",
            "-device", "nvme,serial=sieosblank,drive=nb,logical_block_size=4096,physical_block_size=4096,addr=6",
            "-drive", f"file={disk},if=none,id=nd,format=raw", "-device", "nvme,serial=sieos0,drive=nd,addr=7,mdts=3"]   # (32 KiB per command: vblk cuts transfers)

qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
out, log = b"", b""

def expect(text, timeout=600):
    global out, log
    end = time.time() + timeout
    while text.encode() not in out:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            qemu.kill()
            sys.exit(f"FAIL: waiting for {text!r}\n--- output ---\n{log.decode(errors='replace')[-6000:]}")
        if select.select([qemu.stdout], [], [], left)[0]:
            b = qemu.stdout.read1(4096)
            out += b; log += b
    out = out[out.index(text.encode()) + len(text):]

def send(line):
    qserial.type_line(qemu, line)

expect("at most 32 KiB per command", 120); expect("vblk: NVMe disk", 120); expect("(SieFS partition)")
expect("first start, and there is no screen")
for pw in ("rootpass1", "alicepass1"):
    for p in ("New password", "Again: "):
        expect(p); send(pw)
    if pw == "rootpass1":
        expect("user name"); send("alice")
        expect("full name"); send("Alice Example")
expect("You can log in now")
expect("login: "); send("root"); expect("Password: "); send("rootpass1"); expect("# ")
send(f"bench {mib}")
expect("delete", 3600); expect("# ")
send("echo nvme-ok > /nvme.txt"); expect("# ")
send("cat /nvme.txt"); expect("nvme-ok"); expect("# ")
send("poweroff")
try:
    qemu.wait(120)
except subprocess.TimeoutExpired:
    qemu.kill()
for line in log.decode(errors="replace").replace("\r", "").splitlines():
    if line.startswith(("vblk:", "bench:", "  write", "  read", "  create", "  stat", "  delete")):
        print(line)

part = os.path.join(d, "nvme-part.img")             # the partition back out, checked
with open(disk, "rb") as f, open(part, "wb") as o:
    f.seek(FIRST * S); o.write(f.read(psize))
r = subprocess.run([f"{tools}/fsck.siefs", part], capture_output=True, text=True)
c = subprocess.run([f"{tools}/siefs", part, "cat", "/nvme.txt"], capture_output=True, text=True)
for p in (part, disk, blank): os.remove(p)
print(r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr.strip())
if r.returncode or "nvme-ok" not in c.stdout:
    sys.exit(f"FAIL: fsck {r.returncode}, /nvme.txt: {c.stdout}{c.stderr}")
print("PASS")
