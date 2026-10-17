#!/usr/bin/env python3
"""installtest.py - The installer (user/install, the desktop's Install
window) in QEMU with UEFI firmware (OVMF, with persistent variables), on
emulated NVMe drives (make install-test):

  1. The USB key boots (no screen: the first start on the terminal). Four
     drives: A, a fake Debian laptop drive (an EFI partition made by mtools
     with Debian's files, a /boot, a LUKS-like and a swap partition full of
     random data, 3 MiB of unusable space, then 6 GiB free); B, an old MBR
     drive; C, a GPT drive with only 2 GiB free; D, a GPT drive with no free
     space. As root: the list; the refusals (too small, no free space, an
     MBR drive, the model typed wrong); a dry run; then the install into
     A's free space.
     On the host: every pre-existing partition of A and every byte outside
     the new partition and the EFI partition unchanged (hashes), the EFI
     partition's other files byte-identical (mtools), the FAT consistent,
     both GPTs valid with one entry more, the new SieFS clean (fsck.siefs);
     B, C, D untouched.
  2. The key again: its loader adds "SIEOS" to the firmware's boot menu.
  3. Drive A alone: the firmware tries its default first (the fake Debian's
     loader, which fails), then "SIEOS", from A's EFI partition: SIEOS's
     loader, the kernel, vblk takes the partition by its GUID, the first
     start (accounts) runs; the partition is checked after.
  4. With a screen: the key, logged in as alice on the desktop; the Install
     window (root's password, "Erase whole drive" on B, its model typed,
     the summary, Install, the progress, done). B checked on the host.
  5. The key once more (B's boot entry), then B alone: SIEOS starts from the
     EFI partition the installer formatted, and shows the first start on
     the screen.

Usage: installtest.py CPUS HOSTTOOLS USBIMAGE WORKDIR ACCEL...

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import atexit, hashlib, json, os, random, select, shutil, socket, struct, subprocess, sys, time, uuid, zlib
sys.path.insert(0, os.path.dirname(__file__))
import qserial
from mkusb import gpt, ESP_TYPE

cpus, host, usbimg, work, accel = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:]
CODE = os.environ.get("OVMF_CODE", "/usr/share/OVMF/OVMF_CODE_4M.secboot.fd")   # (no UEFI shell in the way)
VARS = os.environ.get("OVMF_VARS", "/usr/share/OVMF/OVMF_VARS_4M.fd")
for f in (CODE, VARS):
    if not os.path.exists(f):
        sys.exit(f"SKIP: no {f} (the ovmf package)")
if not shutil.which("mformat"):
    sys.exit("SKIP: no mtools (mformat, mcopy)")
os.makedirs(work, exist_ok=True)
os.environ["MTOOLS_SKIP_CHECK"] = "1"
S, MIB = 512, 2048                                  # sectors; sectors per MiB
LINUX = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")
LUKS = uuid.UUID("CA7D7CCB-63ED-4C53-861C-1742536059CC")
SWAP = uuid.UUID("0657FD6D-A4AB-43C4-84E5-0933C84B4F4F")
SIEFS = bytes.fromhex("00051e5eef5153419e05534945465331")
rnd = random.Random(1983)
vars_fd = os.path.join(work, "vars.fd")
shutil.copy(VARS, vars_fd)
qemu = qf = None
full, pos = b"", 0


def fail(msg):
    if qemu and qemu.poll() is None: qemu.kill()
    open(os.path.join(work, "serial.log"), "wb").write(full)
    sys.exit(f"FAIL: {msg}\n--- serial (last part) ---\n{full[-4000:].decode(errors='replace')}")


# ---- the drives
def disk(name, size_mib, parts, fill=()):
    """A GPT drive: parts = [(type, first MiB, last MiB + 1, name)]; fill: (first, sectors) of random data."""
    path = os.path.join(work, name)
    total = size_mib * MIB
    pp = [(t, a * MIB, b * MIB - 1, n) for t, a, b, n in parts]
    h1, ent, h2, ent2 = gpt(total, pp, uuid.uuid4())
    with open(path, "wb") as f:
        f.truncate(total * S)
        mbr = bytearray(S)
        struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\0\x02\0", 0xEE, b"\xff\xff\xff", 1, total - 1)
        mbr[510:512] = b"\x55\xAA"
        f.write(mbr); f.write(h1); f.write(ent)
        f.seek(ent2 * S); f.write(ent); f.write(h2)
        for t, a, b, n in pp:                       # random data at both ends of each partition
            if t == ESP_TYPE: continue
            for at in (a, b + 1 - MIB):
                f.seek(at * S); f.write(rnd.randbytes(MIB * S))
            if t == LUKS:
                f.seek(a * S); f.write(b"LUKS\xba\xbe\x00\x02" + rnd.randbytes(504))
    return path


ESP_FILES = {"EFI/debian/shimx64.efi": 960_000, "EFI/debian/grubx64.efi": 2_600_000, "EFI/debian/mmx64.efi": 850_000,
             "EFI/debian/BOOTX64.CSV": 108, "EFI/debian/grub.cfg": 124, "EFI/BOOT/BOOTX64.EFI": 960_000,
             "EFI/BOOT/fbx64.efi": 85_000, "EFI/BOOT/mmx64.efi": 850_000, "System Volume Information.txt": 300}
A = disk("nvme-a.img", 8192, [(ESP_TYPE, 1, 65, "EFI System Partition"), (LINUX, 65, 193, ""), (LUKS, 193, 1729, ""),
                              (SWAP, 1732, 1988, "")])
esp = os.path.join(work, "esp.img")                 # Debian's EFI partition, made by another FAT implementation
with open(esp, "wb") as f: f.truncate(64 << 20)
subprocess.run(["mformat", "-i", esp, "-F", "-c", "1", "-v", "DEBIAN", "::"], check=True)
for d in ("EFI", "EFI/debian", "EFI/BOOT"):
    subprocess.run(["mmd", "-i", esp, "::/" + d], check=True)
esp_hash = {}
for name, size in ESP_FILES.items():
    data = rnd.randbytes(size)
    esp_hash[name] = hashlib.sha256(data).hexdigest()
    src = os.path.join(work, "espfile")
    open(src, "wb").write(data)
    subprocess.run(["mcopy", "-i", esp, src, "::/" + name], check=True)
with open(A, "r+b") as f:
    f.seek(MIB * S); f.write(open(esp, "rb").read())
B = os.path.join(work, "nvme-b.img")                # an old MBR drive: one partition
with open(B, "wb") as f:
    f.truncate(6144 << 20)
    mbr = bytearray(S)
    struct.pack_into("<B3sB3sII", mbr, 446, 0x80, b"\0\x21\0", 0x83, b"\xfe\xff\xff", 2048, 4000 * MIB)
    mbr[510:512] = b"\x55\xAA"
    f.write(mbr); f.seek(MIB * S); f.write(rnd.randbytes(MIB * S))
C = disk("nvme-c.img", 6144, [(LINUX, 1, 4096, "data")])            # 2 GiB free: too small
D = disk("nvme-d.img", 1024, [(LINUX, 1, 1023, "all")])             # no free space


def chunks(f, a, b):
    """The data of bytes [a, b) of file f, as (offset, bytes), skipping its holes (sparse images: fast)."""
    fd = f.fileno()
    while a < b:
        try:
            a = os.lseek(fd, a, os.SEEK_DATA)
        except OSError:
            return
        if a >= b: return
        e = min(b, os.lseek(fd, a, os.SEEK_HOLE))
        while a < e:
            c = os.pread(fd, min(e - a, 8 << 20), a)
            if not c: return
            yield a, c
            a += len(c)


def sha(path, ranges=None):
    """A hash of the sectors in ranges, 1 MiB block by block: the blocks holding anything but zeros, and
    where (holes and zeros alike count as zeros; any other change shows)."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for a, b in ranges or [(0, os.path.getsize(path) // S)]:
            a, b, seen = a * S, b * S, set()
            for at, c in chunks(f, a, b):
                for blk in range(at & ~0xFFFFF, at + len(c), 1 << 20):
                    if blk in seen: continue
                    seen.add(blk)
                    lo, hi = max(a, blk), min(b, blk + (1 << 20))
                    d = os.pread(f.fileno(), hi - lo, lo)
                    if d.strip(b"\0"): h.update(struct.pack("<Q", lo)); h.update(d)
    return h.hexdigest()


def read_gpt(path, lba=1):
    with open(path, "rb") as f:
        total = os.path.getsize(path) // S
        f.seek(lba * S); h = f.read(92)
        sig, rev, hs, hcrc, _, me, alt, fu, lu, g, el, n, es, ecrc = struct.unpack("<8sIIIIQQQQ16sQIII", h)
        ok = sig == b"EFI PART" and zlib.crc32(h[:16] + b"\0\0\0\0" + h[20:92]) == hcrc
        f.seek(el * S); ents = f.read(n * es)
        ok = ok and zlib.crc32(ents) == ecrc and me == lba and alt == (total - 1 if lba == 1 else 1)
        parts = [(e[:16], e[16:32], *struct.unpack_from("<QQ", e, 32)) for e in (ents[i * es:(i + 1) * es] for i in range(n)) if e[:16] != bytes(16)]
        return ok, parts, ents, (el, el + (n * es + S - 1) // S)


def check_gpt(path):
    ok1, p1, e1, r1 = read_gpt(path)
    ok2, p2, e2, r2 = read_gpt(path, os.path.getsize(path) // S - 1)
    if not (ok1 and ok2 and e1 == e2):
        fail(f"{path}: the GPT is not valid (primary {ok1}, backup {ok2}, same entries {e1 == e2})")
    return p1, r1, r2


def guid_text(b):
    return str(uuid.UUID(bytes_le=bytes(b)))


def fat_check(path, offset):
    """FAT32 consistency: both FATs equal, every chain valid and unshared, FSInfo's free count right."""
    with open(path, "rb") as f:
        f.seek(offset); bs = f.read(512)
        bps, spc, rsvd, nf = struct.unpack_from("<HBHB", bs, 11)
        tot, fsz = struct.unpack_from("<II", bs, 32)
        root, fsi = struct.unpack_from("<IH", bs, 44)
        fats = []
        for i in range(nf):
            f.seek(offset + (rsvd + i * fsz) * bps); fats.append(f.read(fsz * bps))
        if any(x != fats[0] for x in fats):
            fail("the FAT copies differ")
        fat = struct.unpack(f"<{fsz * bps // 4}I", fats[0])
        data0 = rsvd + nf * fsz
        nclus = (tot - data0) // spc
        used = {}
        def chain(c, owner):
            out = []
            while 2 <= c < nclus + 2:
                if c in used: fail(f"cluster {c} is in two chains ({used[c]}, {owner})")
                used[c] = owner; out.append(c)
                n = fat[c] & 0x0FFFFFFF
                if n >= 0x0FFFFFF8: break
                c = n
            return out
        def rd(c):
            f.seek(offset + (data0 + (c - 2) * spc) * bps); return f.read(spc * bps)
        files = 0
        def walk(dc, name):
            nonlocal files
            for c in chain(dc, name):
                d = rd(c)
                for i in range(0, len(d), 32):
                    e = d[i:i + 32]
                    if e[0] == 0: return
                    if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 8 or e[:2] in (b". ", b".."): continue
                    first = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
                    if e[11] & 0x10: walk(first, name + "/" + e[:11].decode())
                    else:
                        files += 1
                        cl = chain(first, name + "/" + e[:11].decode()) if first else []
                        size = struct.unpack_from("<I", e, 28)[0]
                        if len(cl) != (size + spc * bps - 1) // (spc * bps): fail(f"{name}/{e[:11]}: its chain does not fit its size")
        walk(root, "")
        free = sum(1 for c in range(2, nclus + 2) if fat[c] & 0x0FFFFFFF == 0)
        lost = sum(1 for c in range(2, nclus + 2) if fat[c] & 0x0FFFFFFF and c not in used)
        f.seek(offset + fsi * bps); fs = f.read(512)
        if fs[:4] == b"RRaA" and struct.unpack_from("<I", fs, 488)[0] not in (free, 0xFFFFFFFF):
            fail(f"FSInfo's free count {struct.unpack_from('<I', fs, 488)[0]} is not {free}")
        if lost: fail(f"{lost} clusters are used by no file")
        return files, free


def fsck_part(path, first, last, cat=None):
    part = os.path.join(work, "part.img")
    with open(path, "rb") as f, open(part, "wb") as o:
        for at, c in chunks(f, first * S, (last + 1) * S):
            o.seek(at - first * S); o.write(c)
        o.truncate((last + 1 - first) * S)
    r = subprocess.run([os.path.join(host, "fsck.siefs"), part], capture_output=True, text=True)
    out = subprocess.run([os.path.join(host, "siefs"), part, "cat", cat], capture_output=True, text=True).stdout if cat else ""
    os.unlink(part)
    if r.returncode: fail(f"fsck.siefs: {r.stdout}{r.stderr}")
    return r.stdout.strip().splitlines()[-1], out


# ---- QEMU
def boot(drives, usb=True, screen=False):
    global qemu, qf, full, pos
    cmd = ["qemu-system-x86_64", "-machine", "q35,smm=on", "-global", "driver=cfi.pflash01,property=secure,value=on",
           "-smp", cpus, "-m", "2G", *accel, "-no-reboot",
           "-drive", f"if=pflash,format=raw,readonly=on,file={CODE}", "-drive", f"if=pflash,format=raw,file={vars_fd}",
           "-device", "qemu-xhci", "-net", "none", "-serial", "stdio", "-display", "none"]
    cmd += ["-vga", "std", "-device", "usb-tablet"] if screen else ["-vga", "none"]
    if usb:
        cmd += ["-drive", f"if=none,id=k,file={usbimg},format=raw", "-device", "usb-storage,drive=k,bootindex=0"]
    for i, (path, serial) in enumerate(drives):
        cmd += ["-drive", f"if=none,id=n{i},file={path},format=raw", "-device", f"nvme,serial={serial},drive=n{i},addr={0x10 + i:#x}"]
    sock = os.path.join(work, "qmp.sock")
    if os.path.exists(sock): os.unlink(sock)
    full, pos = b"", 0
    qemu = subprocess.Popen(cmd + ["-qmp", f"unix:{sock},server=on,wait=off"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    atexit.register(lambda p=qemu: p.poll() is None and p.kill())
    for _ in range(100):
        try:
            s = socket.socket(socket.AF_UNIX); s.connect(sock); break
        except OSError:
            time.sleep(0.05)
    qf = s.makefile("rw")
    json.loads(qf.readline())
    q("qmp_capabilities")


def wait(text, timeout=60):
    global full, pos
    end = time.time() + timeout
    while text.encode() not in full[pos:]:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            fail(f"waiting for {text!r}")
        if select.select([qemu.stdout], [], [], left)[0]:
            full += qemu.stdout.read1(65536)
    i = full.index(text.encode(), pos) + len(text)
    time.sleep(0.05)
    while select.select([qemu.stdout], [], [], 0.05)[0]:
        full += qemu.stdout.read1(65536)
    e = full.find(b"\n", i)
    pos = i
    return full[i:e if e >= 0 else len(full)].decode(errors="replace").strip()


def q(cmd, **args):
    qf.write(json.dumps({"execute": cmd, "arguments": args}) + "\n"); qf.flush()
    while True:
        r = json.loads(qf.readline())
        if "return" in r: return r["return"]
        if "error" in r: fail(f"QMP {cmd}: {r['error']}")


KEYS = {" ": "spc", "\n": "ret", "\t": "tab", ".": "dot", "-": "minus"}
def type_text(s):
    for c in s:
        k = ["shift", c.lower()] if c.isupper() else [KEYS.get(c, c)]
        q("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.02)


SW = SH = 0
def click(x, y):
    """(x, y) of the desktop's picture (where atlas logs things: 0.75 screen pixels each at 100%)."""
    q("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": int((x * 0.75 + 0.5) * 32768 / SW)}},
                                  {"type": "abs", "data": {"axis": "y", "value": int((y * 0.75 + 0.5) * 32768 / SH)}}])
    time.sleep(0.05)
    for down in (True, False):
        q("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}]); time.sleep(0.05)


def latest(what):
    """Where atlas last drew it ("atlas: dbg WHAT at X Y", with /tmp/atlas.debug)."""
    t = full.decode(errors="replace")
    k = t.rfind(f"atlas: dbg {what} at ")
    if k < 0: fail(f"atlas never drew {what}")
    x, y = t[k:].split(" at ", 1)[1].split()[:2]
    return int(x), int(y)


def shot(name):
    q("screendump", filename=os.path.join(work, name + ".png"), format="png")


def send(line):
    qserial.type_line(qemu, line)


def root_shell():
    send(""); wait("login: "); send("root"); wait("Password: "); send("rootpass1"); wait("# ")


def run(cmd, answer=None, timeout=60):
    """A command at the root shell -> its output."""
    global pos
    send(cmd)
    if answer is not None:
        wait("Type "); send(answer)
    start = pos
    wait("# ", timeout)
    return full[start:pos].decode(errors="replace")


def power_off():
    send("poweroff")
    try:
        qemu.wait(60)
    except subprocess.TimeoutExpired:
        fail("did not power off")


# ---- 1: install into A's free space
before = {p: sha(p) for p in (B, C, D)}
pa, ra1, ra2 = check_gpt(A)
a_parts = [(t, g, a, b) for t, g, a, b in pa]
TOTAL = os.path.getsize(A) // S
NLAST = (TOTAL - 33) // MIB * MIB - 1               # (the last usable sector is TOTAL - 34: whole MiB below it)
KEEP = [(0, 1), (34, MIB), (65 * MIB, 1988 * MIB), (NLAST + 1, TOTAL - 33)]   # not the GPT, the EFI partition, the free space
keep_hash = sha(A, KEEP)
t0 = time.time()
boot([(A, "deb0"), (B, "old1"), (C, "small2"), (D, "full3")])
wait("SIEOS loader: starting the kernel", 90)
got = wait("vblk: root ")
wait("vblk: USB disk")
wait("first start, and there is no screen", 60)
for pw in ("rootpass1", "alicepass1"):
    for p in ("New password", "Again: "):
        wait(p); send(pw)
    if pw == "rootpass1":
        wait("user name"); send("alice")
        wait("full name"); send("Alice Example")
wait("You can log in now")
root_shell()
out = run("install list")
print(out.replace("\r", "").strip())
for want in ("drive 0: QEMU NVMe Ctrl (serial deb0)", "part 1: EFI system", "part 3: Linux LUKS", "free 1 small: 3 MiB",
             "free 2 ok: ", "an MBR partition table", "free 1 small: 2.0 GiB", "no free space"):
    if want not in out: fail(f"install list: no {want!r}")
for cmd, answer, want in (("install -n free 2 1", None, "error: that free space is too small"),
                          ("install free 3 1", None, "error: no such free space on that drive"),
                          ("install free 1 1", None, "error: an MBR partition table, not a GPT"),
                          ("install free 0 1", None, "error: that free space is too small"),
                          ("install whole 1", "QEMU NVMe", "error: not confirmed: nothing was written"),
                          ("install free 0 2", "no", "error: not confirmed: nothing was written")):
    o = run(cmd, answer)
    if want not in o: fail(f"{cmd}: expected {want!r}, got:\n{o}")
print("ok: refused: too small (2 GiB, 3 MiB), no free space, an MBR drive, the model typed wrong, not \"yes\"")
o = run("install -n free 0 2")
if "plan: dry run: nothing was written" not in o or "plan: add partition 5" not in o: fail(f"dry run:\n{o}")
print(o.replace("\r", "").strip())
t1 = time.time()
o = run("install free 0 2", "yes", 600)
if "done: SIEOS is installed on drive 0, partition 5" not in o: fail(f"install:\n{o}")
t_inst = time.time() - t1
print("\n".join(l for l in o.replace("\r", "").splitlines() if l.startswith(("step:", "check:", "done:"))))
power_off()
print(f"ok: installed into A's free space in {t_inst:.1f} s")

# ---- on the host
for p in (B, C, D):
    if sha(p) != before[p]: fail(f"{os.path.basename(p)} changed: the refusals and the dry run must write nothing")
pn, r1, r2 = check_gpt(A)
new = [x for x in pn if x not in [(t, g, a, b) for t, g, a, b in a_parts]]
if [x for x in a_parts if x not in pn] or len(new) != 1 or new[0][0] != SIEFS:
    fail(f"A's GPT: the old entries must stay and one SieFS entry be added: {pn}")
_, nguid, nfirst, nlast = new[0]
if (nfirst, nlast) != (1988 * MIB, NLAST):
    fail(f"the new partition is not the free space: {nfirst}-{nlast}, not {1988 * MIB}-{NLAST}")
if sha(A, KEEP) != keep_hash:
    fail("A changed outside the new partition, the EFI partition and the GPT's own sectors")
print(f"ok: A's GPT valid (both copies), its {len(a_parts)} entries unchanged, partition 5 added ({guid_text(nguid)}); "
      f"every other sector of A unchanged (the 3 partitions with data, the gaps: hashed)")
for name, h in esp_hash.items():
    r = subprocess.run(["mcopy", "-n", "-i", f"{A}@@{MIB * S}", "::/" + name, "-"], capture_output=True)
    if hashlib.sha256(r.stdout).hexdigest() != h: fail(f"the EFI partition's {name} changed")
files, freec = fat_check(A, MIB * S)
cfg = subprocess.run(["mtype", "-i", f"{A}@@{MIB * S}", "::/EFI/SIEOS/BOOT.CFG"], capture_output=True, text=True).stdout
if f"root={guid_text(nguid)}" not in cfg: fail(f"BOOT.CFG does not name the new partition: {cfg}")
ls = subprocess.run(["mdir", "-b", "-i", f"{A}@@{MIB * S}", "::/EFI/SIEOS"], capture_output=True, text=True).stdout
for f_ in ("SIEOSX64.EFI", "KERNEL.BIN", "VBLK.ELF", "ADDENTRY"):
    if f_ not in ls: fail(f"no \\EFI\\SIEOS\\{f_}: {ls}")
print(f"ok: the EFI partition's {len(esp_hash)} other files byte-identical; FAT consistent ({files} files, {freec} clusters free); BOOT.CFG has root={guid_text(nguid)}")
sum_, _ = fsck_part(A, nfirst, nlast)
print(f"ok: the new SieFS partition checks clean ({sum_.split(': ')[-1]})")


# ---- 2: the key again: the boot entry
boot([(A, "deb0")])
got = wait("SIEOS loader: SIEOS added to the firmware's boot menu (", 90)
entry = got.split(")")[0]
wait("vblk: USB disk", 60)
wait("login: ", 60)
root_shell(); power_off()
print(f"ok: the key's loader added \"SIEOS\" to the firmware's boot menu ({entry})")



def efi_vars():
    """The firmware's variables in its store (vars.fd): {name: data}, the valid copy of each."""
    v, d = {}, open(vars_fd, "rb").read()
    i = d.find(b"\xaa\x55")
    while i >= 0:
        if d[i + 2] == 0x3F and i + 60 < len(d):          # (State: added, valid; the authenticated header)
            ns_, ds = struct.unpack_from("<II", d, i + 36)
            if 0 < ns_ < 256 and ds < 65536:
                name = d[i + 60:i + 60 + ns_].decode("utf-16-le", "replace").rstrip("\0")
                v[name] = d[i + 60 + ns_:i + 60 + ns_ + ds]
        i = d.find(b"\xaa\x55", i + 2)
    return v


v = efi_vars()
order = ["Boot%04X" % n for n in struct.unpack(f"<{len(v.get('BootOrder', b'')) // 2}H", v.get("BootOrder", b""))]
if not order or order[-1] != entry or len(order) < 2:
    fail(f"BootOrder {order}: {entry} must be last, after the firmware's own entries")
if "SIEOS".encode("utf-16-le") not in v.get(entry, b"") or "\\EFI\\SIEOS\\SIEOSX64.EFI".encode("utf-16-le") not in v[entry]:
    fail(f"{entry} is not SIEOS's entry")
print(f"ok: BootOrder {' '.join(order)}: {entry} \"SIEOS\" last (not the default)")

# ---- 3: A alone: SIEOS from the boot menu's entry
boot([(A, "deb0")], usb=False)
wait(f'BdsDxe: starting {entry} "SIEOS"', 90)
wait("SIEOS loader: starting the kernel", 30)
got = wait("vblk: root ")
if guid_text(nguid) not in got: fail(f"vblk: root {got}")
got = wait("vblk: NVMe disk")
wait("first start, and there is no screen", 60)
for pw in ("rootpass1", "bobpass123"):
    for p in ("New password", "Again: "):
        wait(p); send(pw)
    if pw == "rootpass1":
        wait("user name"); send("bob")
        wait("full name"); send("Bob")
wait("You can log in now")
root_shell(); run("echo installed > /installed.txt"); power_off()
sum_, cat = fsck_part(A, nfirst, nlast, "/installed.txt")
if "installed" not in cat: fail("the file written on the installed system is not there")
print(f"ok: A alone: {entry} \"SIEOS\": SIEOS started from A (root={guid_text(nguid)}), first start done; partition clean ({sum_.split(': ')[-1]})")

# ---- 4: the Install window: erase B
boot([(B, "old1")], screen=True)
wait("atlas: greeter", 90)
SW, SH = 1920, 1080
got = full.decode(errors="replace")
k = got.rfind("atlas: screen: ")
if k >= 0:
    import re
    m = re.search(r"(\d+) x (\d+)", got[k:k + 120])
    if m: SW, SH = int(m.group(1)), int(m.group(2))
root_shell(); run("touch /tmp/atlas.debug")
type_text("alice\talicepass1\n")
wait("atlas: login alice: ok", 30)
click(*latest("dbmenu"))
got = wait("atlas: menu entry ", 10)
while " Install SIEOS at " not in " " + got:
    got = wait("atlas: menu entry ", 10)
x, y = got.split(" at ")[-1].split()[:2]
click(int(x), int(y))
wait("atlas: install: open (step 0)", 10)
type_text("rootpass1\n")
wait("atlas: install: drive 0: QEMU NVMe Ctrl (serial old1)", 30)
wait("atlas: install: program ended (0), step 1", 30)
time.sleep(0.5); shot("install-1-drives")
click(*latest("inst 200"))
wait("atlas: install: step 2", 10)
type_text("QEMU NVMe Ctrl\n")
wait("atlas: install: plan: ERASE THE WHOLE DRIVE", 30)
wait("atlas: install: program ended (0), step 3", 30)
time.sleep(0.5); shot("install-2-summary")
click(*latest("inst 3"))
wait("atlas: install: answered its question", 30)
wait("atlas: install: done: SIEOS is installed on drive 0, partition 2", 600)
wait("step 5", 10)
time.sleep(0.5); shot("install-3-done")
send("poweroff")
try:
    qemu.wait(60)
except subprocess.TimeoutExpired:
    fail("did not power off")
pb, _, _ = check_gpt(B)
if len(pb) != 2 or pb[0][0] != ESP_TYPE.bytes_le or pb[1][0] != SIEFS: fail(f"B's new GPT: {pb}")
files, freec = fat_check(B, pb[0][2] * S)
cfg = subprocess.run(["mtype", "-i", f"{B}@@{pb[0][2] * S}", "::/EFI/SIEOS/BOOT.CFG"], capture_output=True, text=True).stdout
if f"root={guid_text(pb[1][1])}" not in cfg: fail(f"B's BOOT.CFG: {cfg}")
sum_, _ = fsck_part(B, pb[1][2], pb[1][3])
print(f"ok: the Install window erased B and installed: new GPT (EFI + SieFS), FAT consistent ({files} files), SieFS clean ({sum_.split(': ')[-1]})")

# ---- 5: B's boot entry, then B alone, with its screen's first start
boot([(B, "old1")], screen=True)
entry = wait("SIEOS loader: SIEOS added to the firmware's boot menu (", 90).split(")")[0]
wait("atlas: greeter", 90)
q("quit")
time.sleep(1)
boot([(B, "old1")], usb=False, screen=True)
wait(f'BdsDxe: starting {entry} "SIEOS"', 90)
got = wait("vblk: root ", 30)
if guid_text(pb[1][1]) not in got: fail(f"vblk: root {got}")
wait("atlas: first start: accounts needed (setup on the screen)", 60)
time.sleep(0.5); shot("install-4-firststart")
q("quit")
print(f"ok: B alone: {entry} \"SIEOS\" started SIEOS from the EFI partition the installer made; the first start on the screen")
print(f"PASS ({time.time() - t0:.0f} s)")
