# primes.py - an example for /bin/python: the primes below N (default 100).
#   python /src/python/primes.py 1000
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
import sys, time

n = int(sys.argv[1]) if len(sys.argv) > 1 else 100
t = time.time()
sieve = bytearray(b"\x01" * n)
sieve[0:2] = b"\0\0"
for i in range(2, int(n ** 0.5) + 1):
    if sieve[i]:
        for j in range(i * i, n, i):
            sieve[j] = 0
primes = [i for i in range(n) if sieve[i]]
print(primes if len(primes) <= 30 else primes[:10] + ["..."] + primes[-5:])
print(f"{len(primes)} primes below {n}, in {int((time.time() - t) * 1000)} ms")
