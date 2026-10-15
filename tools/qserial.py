"""Typing a line on SIEOS's serial console in QEMU (the test scripts).

A few bytes at a time: an emulated UART's FIFO is small, and a guest busy
elsewhere (an emulated arm64 machine, a crash test killing servers) drops
what overflows it; a whole line written at once then arrives cut short.

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import time


def type_line(qemu, line):
    data = line.encode() + b"\r"
    for i in range(0, len(data), 8):
        qemu.stdin.write(data[i:i + 8])
        qemu.stdin.flush()
        time.sleep(0.01)
