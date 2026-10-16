# deep.py - recursion and the garbage collector (tools/pythontest.py): a deep
# call chain, one too deep (an exception, not a crash), and a heap that grows
# well past its first 1 MiB while collections run.
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
import gc

def f(n):
    return f(n - 1) + 1 if n else 0

print("deep", f(500))
try:
    f(10 ** 6)
except RuntimeError as e:
    print("limit", type(e).__name__)
d = {i: str(i) for i in range(100000)}
junk = sum(len(bytearray(1000)) for i in range(20000))
gc.collect()
print("heap", len(d), all(d[i] == str(i) for i in range(100000)), junk, gc.mem_alloc() > 4000000)
