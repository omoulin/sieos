#!/usr/bin/env python3
"""Compare SIEOS's BLAKE2b (keyed, every digest length used, inputs across
block boundaries) with the independent implementation of the host's Python.
Usage: check.py <crypto-test program>

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import hashlib, subprocess, sys

data = bytes((i * 7 + 1) & 255 for i in range(256))
key = bytes(range(64))
lines = subprocess.run([sys.argv[1], "-x"], capture_output=True, text=True, check=True).stdout.split("\n")
n = 0
for line in filter(None, lines):
    size, outlen, keylen, digest = line.split()
    want = hashlib.blake2b(data[:int(size)], digest_size=int(outlen), key=key[:int(keylen)]).hexdigest()
    if digest != want:
        sys.exit(f"FAIL: BLAKE2b of {size} bytes, digest {outlen}, key {keylen}")
    n += 1
print(f"  BLAKE2b vs an independent implementation: {n} digests ok")

# zlib.c against the host's zlib: our streams inflated by it, its streams
# (every level: stored, fixed and dynamic blocks) inflated by ours.
import glob, os, random, zlib
rnd = random.Random(1)
srcs = b"".join(open(f, "rb").read() for f in sorted(glob.glob(os.path.join(os.path.dirname(__file__), "../../lib/*.c"))))
corpus = {"empty": b"", "one byte": b"x", "zeros": bytes(100000), "random": bytes(rnd.randrange(256) for _ in range(70000)),
          "lib/*.c": srcs, "pattern": bytes(i * i % 251 for i in range(200000)), "short text": srcs[:300]}
ours = theirs = 0
for name, data in corpus.items():
    z = subprocess.run([sys.argv[1], "-z"], input=data, capture_output=True, check=True).stdout
    if zlib.decompress(z) != data:
        sys.exit(f"FAIL: our deflate of {name}, inflated by zlib")
    for level in (0, 1, 6, 9):
        u = subprocess.run([sys.argv[1], "-u"], input=zlib.compress(data, level), capture_output=True).stdout
        if u != data:
            sys.exit(f"FAIL: zlib -{level} of {name}, inflated by ours")
    if name == "lib/*.c":
        ours, theirs = len(z), len(zlib.compress(data, 9))
print(f"  zlib vs the host's zlib: {len(corpus)} inputs both ways ok;"
      f" lib/*.c {len(srcs)} bytes -> {ours} (ours), {theirs} (zlib -9)")
