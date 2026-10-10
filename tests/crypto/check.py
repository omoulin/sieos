#!/usr/bin/env python3
"""Compare SIEOS's BLAKE2b (keyed, every digest length used, inputs across
block boundaries) with the independent implementation of the host's Python.
Usage: check.py <crypto-test program>

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
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
