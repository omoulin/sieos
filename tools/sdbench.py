#!/usr/bin/env python3
"""Measure the SD card path on QEMU's Raspberry Pi 4 (raspi4b): boot a
fresh card, do the first start on the serial console, log in as root, run
`bench`, check the files, power off, check the card with the host's fsck.
Prints the disk server's lines (controller, mode, clock, DMA, interrupts)
and bench's figures. Used by `make ARCH=arm64 pi4-sdbench`.

Usage: sdbench.py <host tools dir> <card image> <MiB> <qemu command line...>

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
"""
import select, subprocess, sys, time

tools, image, mib, cmdline = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
out, log = b"", b""

qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

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
    qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()

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
send("echo sd-ok > /root/sd.txt"); expect("# ")
send("cat /root/sd.txt"); expect("sd-ok"); expect("# ")
send("poweroff")
try:
    qemu.wait(120)
except subprocess.TimeoutExpired:
    qemu.kill()
text = log.decode(errors="replace").replace("\r", "")
for line in text.splitlines():
    if line.startswith(("vblk:", "bench:", "  write", "  read", "  create", "  stat", "  delete")):
        print(line)
r = subprocess.run([f"{tools}/fsck.siefs", image], capture_output=True, text=True)
print(r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr.strip())
sys.exit(0 if r.returncode == 0 else 1)
