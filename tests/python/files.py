# files.py - /bin/python's test (tools/pythontest.py): files and folders,
# a module imported from the script's folder, and the modules SIEOS ships.
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
import os, sys, json, re, struct, binascii, hashlib, random, errno, time, collections, math
print("argv", sys.argv[1:])
with open("out.txt", "w") as f:
    for i in range(3):
        f.write("line %d\n" % i)
with open("out.txt") as f:
    print("lines", f.readlines())
with open("out.txt", "a") as f:
    f.write("more\n")
print("size", os.stat("out.txt")[6])
with open("out.txt", "rb") as f:
    f.seek(5)
    print("seek", f.read(6), f.tell())
os.mkdir("sub")
os.rename("out.txt", "sub/moved.txt")
print("listdir", os.listdir("sub"), "out.txt" in os.listdir())
d = {"name": "SIEOS", "n": [1, 2.5, None, True], "nested": {"k": "v"}}
s = json.dumps(d)
print("json", json.loads(s) == d, json.loads('{"a": [1, "x"]}'))
print("re", re.sub(r"(\d+)", r"<\1>", "a1b22c333"), re.match(r"(\w+)@(\w+)", "alice@sieos").group(2))
print("struct", struct.pack("<HI", 1, 2), struct.unpack(">I", b"\x00\x00\x01\x00"))
print("binascii", binascii.hexlify(b"\x01\xab"), binascii.b2a_base64(b"SIEOS"))
print("sha256", hashlib.sha256(b"abc").hexdigest())
try:
    os.stat("nothing")
except OSError as e:
    print("oserror", e.errno == errno.ENOENT)
random.seed(1)
print("random", 0 <= random.random() < 1, random.randint(5, 5))
q = collections.deque([], 5)
q.append(1); q.append(2)
print("collections", q.popleft(), collections.namedtuple("P", "x y")(1, 2))
t = time.ticks_ms()
time.sleep(0.2)
print("slept", time.ticks_diff(time.ticks_ms(), t) >= 190, time.localtime()[0] >= 2026)
print("math", math.sqrt(2), math.floor(-2.5), math.log2(8), f"{math.pi:.3f}")
import mymod
print("import", mymod.twice(21))
os.remove("sub/moved.txt")
os.rmdir("sub")
print("done", "sub" in os.listdir())
