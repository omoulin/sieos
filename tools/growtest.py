#!/usr/bin/env python3
"""growtest.py - A small image on a bigger disk (make grow-test): SIEOS grows
its SieFS partition and file system to the disk's end on the first start.

  1. power cuts: QEMU killed just as the disk server starts (the partition
     table being extended) and just after (the file system growing); each
     time the table must be valid, the SieFS partition check clean, and the
     next start finish the job;
  2. a whole run: the grow logged once, `df` shows the new size, a file
     bigger than the old file system written; a second start does not grow
     again and reads the file back;
  3. on the host: the table (a GPT: both copies valid, the backup at the
     disk's last sector; or an MBR), the partition up to the end (1 MiB
     aligned), the other partitions untouched, fsck clean, the file's data.

Usage: growtest.py CPUS HOSTTOOLS IMAGE SIZE QEMU-COMMAND...
(IMAGE: a GPT or MBR image with a SieFS partition, made small; the QEMU
command uses it as its disk. It is made SIZE bytes first.)

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import os, select, shutil, subprocess, sys, tempfile, time
import qserial, gptcheck
SLOW = float(os.environ.get("SIEOS_SLOW", "1"))

cpus, host, image, size, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:]
size = int(size[:-1]) << {"M": 20, "G": 30}[size[-1]] if size[-1] in "MG" else int(size)
qemu, out = None, b""
small = image + ".small"
shutil.copyfile(image, small)


def fail(msg):
    if qemu and qemu.poll() is None: qemu.kill()
    sys.exit(f"FAIL: {msg}\n--- output (last part) ---\n{out[-3000:].decode(errors='replace')}")


def boot():
    global qemu, out
    out = b""
    qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def expect(text, timeout=30, consume=True):
    global out
    end = time.time() + timeout * SLOW
    while text.encode() not in out:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None: fail(f"waiting for {text!r}")
        if select.select([qemu.stdout], [], [], left)[0]: out += qemu.stdout.read1(4096)
    i = out.index(text.encode()) + len(text)
    while out.find(b"\n", i) < 0 and qemu.poll() is None and select.select([qemu.stdout], [], [], 2)[0]:
        out += qemu.stdout.read1(4096)                  # (the rest of the line)
    line = out[i:out.find(b"\n", i)].decode(errors="replace").strip()
    if consume: out = out[i:]
    return line


def send(line): qserial.type_line(qemu, line)


def run(cmd, want=None, timeout=60):
    send(cmd)
    if want: expect(want, timeout)
    expect("# ", timeout)


def poweroff():
    send("poweroff")
    try: qemu.wait(10 * SLOW)
    except subprocess.TimeoutExpired: fail("did not power off")


def host_check(what, grown=None):
    """The table valid, the SieFS partition checks clean; -> (first, last, fs blocks)."""
    kind, parts, bad = gptcheck.table(image)
    if bad: fail(f"{what}: the partition table: {'; '.join(bad)}")
    for p0, p1 in zip(parts_before, parts):
        if p0[0] not in (gptcheck.SIEFS_GPT, 0x5E) and p0 != p1: fail(f"{what}: another partition changed: {p0} -> {p1}")
    first, last = gptcheck.siefs_part(image)
    with tempfile.NamedTemporaryFile(dir=os.path.dirname(image)) as t:
        gptcheck.extract(image, t.name, first, last)
        r = subprocess.run([os.path.join(host, "fsck.siefs"), "-d", t.name], capture_output=True, text=True)
        if r.returncode: fail(f"{what}: fsck.siefs: {r.stdout}{r.stderr}")
        blocks = int(r.stdout.split(" blocks free")[0].split(" of ")[-1])
        if grown:                                   # the file written beyond the old end, read back
            subprocess.run([os.path.join(host, "siefs"), t.name, "get", "/big.bin", t.name + ".big"], check=True)
            data = open(t.name + ".big", "rb").read(); os.unlink(t.name + ".big")
            if data != b"".join(bytes([i % 251]) * (1 << 20) for i in range(grown)): fail("/big.bin reads back wrong on the host")
    return kind, first, last, blocks


parts_before = gptcheck.table(image)[1]
kind, first, last0 = gptcheck.table(image)[0], *gptcheck.siefs_part(image)
old_mib = (last0 + 1 - first) >> 11
end = (size // 512 - (34 if kind == "gpt" else 0)) // 2048 * 2048 - 1     # where the partition must end

# ---- 1: power cuts during the grow
cuts = 0
for when in ("vblk: ", "vblk: +0.05", "vblk: +0.3", "fs: grew"):
    shutil.copyfile(small, image); os.truncate(image, size)
    boot()
    expect(when.split("+")[0], 60)
    if "+" in when: time.sleep(float(when.split("+")[1]))
    qemu.kill(); qemu.wait()
    k, f0, l0, b0 = host_check(f"cut at {when!r}")
    if l0 not in (last0, end): fail(f"cut at {when!r}: the partition ends at {l0}")
    boot()                                          # the next start finishes it
    expect("New password", 60 * 3)
    qemu.kill(); qemu.wait()
    k, f1, l1, b1 = host_check(f"after the cut at {when!r}")
    if l1 != end or b1 != (l1 + 1 - f1) // 8: fail(f"after the cut at {when!r}: partition to {l1}, {b1} blocks (want {end}, {(end + 1 - f1) // 8})")
    cuts += 1
print(f"ok: {cuts} power cuts during the grow (at the disk server's start, just after, after the file system grew): "
      f"the table valid and the file system clean each time, grown fully on the next start")

# ---- 2: a whole run
shutil.copyfile(small, image); os.truncate(image, size)
boot()
grew = expect("fs: grew the root partition from ", 60)
print(f"ok: first start: fs: grew the root partition from {grew}")
expect("New password", 60); send("rootpass1"); expect("Again: "); send("rootpass1")
expect("user name"); send("alice"); expect("full name"); send("Alice Example")
expect("New password"); send("alicepass1"); expect("Again: "); send("alicepass1")
expect("You can log in now")
expect("login: "); send("root"); expect("Password: "); send("rootpass1"); expect("root@sieos:"); expect("# ")
send("df"); df = expect('"SIEOS": ', 20); expect("# ")
if abs(int(df.split()[0]) * 1024 - (end + 1 - first) * 512) > 1 << 20: fail(f"df does not show the grown size: {df}")
nbig = old_mib + 16                                # bigger than the old file system: it must use the new space
run(f"python -c \"f=open('/big.bin','wb');[f.write(bytes([i%251])*1048576) for i in range({nbig})];f.close();print('wr'+'ote')\"",
    "wrote", 300)
print(f"ok: df after the grow: {df}; a {nbig} MiB file written (the old file system had {old_mib} MiB)")
poweroff()
boot()
expect("login: ", 60)
if b"fs: grew" in out: fail("the second start grew again")
send("root"); expect("Password: "); send("rootpass1"); expect("# ")
run(f"python -c \"f=open('/big.bin','rb');print('big', all(f.read(1048576)==bytes([i%251])*1048576 for i in range({nbig})))\"",
    "big True", 300)
print("ok: second start: no grow again; the file reads back")
poweroff()

# ---- 3: on the host
k, f, l, blocks = host_check("after the runs", nbig)
if l != end: fail(f"the partition ends at sector {l}, not {end}")
if blocks != (l + 1 - f) // 8: fail(f"the file system has {blocks} blocks, the partition {(l + 1 - f) // 8}")
print(f"ok: on the host: {k.upper()} valid{' (both copies, the backup at the disk end)' if k == 'gpt' else ''}, "
      f"SieFS partition {old_mib} -> {(l + 1 - f) >> 11} MiB, other partitions untouched, fsck -d clean, the file's data right")
os.unlink(small)
print("PASS")
