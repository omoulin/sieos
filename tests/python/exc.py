# exc.py - an uncaught exception: its traceback (tools/pythontest.py)
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
def inner(n):
    return 10 // n
def outer():
    return inner(0)
outer()
