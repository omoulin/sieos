"""Checks sicc's AArch64 instruction encodings: assembles enc.S and compares
each word of .text with the hex number in that line's comment.
Usage: enc.py SICC  (OUT: the work directory)
Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only"""
import os, struct, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
out = os.environ.get("OUT", "/tmp/sicc-enc")
os.makedirs(out, exist_ok=True)
src, obj = os.path.join(here, "enc.S"), os.path.join(out, "enc.o")
subprocess.run([sys.argv[1], "--target=aarch64", "-c", "-o", obj, src], check=True)

def text(path):
    """the bytes of the ELF object's .text section"""
    b = open(path, "rb").read()
    shoff, = struct.unpack_from("<Q", b, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 0x3a)
    sec = [struct.unpack_from("<IIQQQQ", b, shoff + i * shentsize) for i in range(shnum)]
    names = sec[shstrndx][4]
    for name, _, _, _, off, size in sec:
        if b[names + name:b.index(b"\0", names + name)] == b".text": return b[off:off + size]
    sys.exit("enc.py: no .text")

want = [(l.split("//")[0].strip(), int(l.split("//")[1], 16)) for l in open(src) if "//" in l and not l.startswith("//")]
words = struct.unpack(f"<{len(text(obj)) // 4}I", text(obj))
bad = 0
if len(words) != len(want): print(f"enc: {len(words)} words for {len(want)} instructions"); bad += 1
for (insn, w), got in zip(want, words):
    if w != got: print(f"enc: {insn}: want {w:08x}, got {got:08x}"); bad += 1
print(f"sicc AArch64 encodings: {len(want) - bad} of {len(want)} agree")
sys.exit(1 if bad else 0)
