#!/usr/bin/env python3
"""Test other programs' windows and the UI toolkit on the desktop (make
ui-test), through QEMU's control socket (QMP): screenshots, keys and the
mouse, checked against atlas's log lines and against the pixels.

  first start (accounts) -> alice's terminal -> uipatch (tests/ui/uipatch.c):
  a window of exact colour patches; in each look (Phosphor, Amber 1984,
  Glass, BeOS) every patch pixel is checked to be exactly the program's
  colour while the window's frame and tab have the look's colours; atlas's
  full-frame times are collected in each look.

Usage: uitest.py <cpus> <screenshots dir> <disk image> <qemu command line...>
(UI_MEASURE=1: only the frame times, nothing checked.) The test works in a
copy of the image. Screenshots (PNG) are kept.

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qdesk import Desk, find, count, pixel, look

cpus, shots, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
MEASURE = os.environ.get("UI_MEASURE") == "1"
d = Desk(shots, image, cmdline)
start = time.time()
d.login(cpus)
print(f"ok: alice logged in on the desktop ({time.time() - start:.1f} s)")

PATCH = ["ff0000", "00ff00", "0000ff", "ffffff", "000000", "808080", "123456", "fedcba", "4be3c1", "0a0f1c"]
LOOKS = ["Phosphor", "Amber 1984", "Glass", "BeOS"]
ACCENT = [look("4be3c1", 0), look("4be3c1", 1), "4be3c1", "ffcb00"]   # each look's accent (a focused frame, its tab)


def patches(img, box):
    """Every pixel of uipatch's ten patches exact? -> (origin of its insides, None) or (None, why)."""
    at = find(img, "ff0000", box, 1)
    if not at:
        return None, "no pure red (ff0000) in the window"
    ox, oy = at[0] - 20, at[1] - 20
    w, h, px = img
    for k, rgb in enumerate(PATCH):
        want = bytes.fromhex(rgb)
        x0, y0 = ox + 20 + k % 5 * 100, oy + 20 + k // 5 * 100
        for y in range(y0, y0 + 80):
            row = px[(y * w + x0) * 3:(y * w + x0 + 80) * 3]
            if row != want * 80:
                bad = next(i for i in range(80) if row[i * 3:i * 3 + 3] != want)
                return None, f"patch {k} ({rgb}) at {x0 + bad},{y}: {row[bad * 3:bad * 3 + 3].hex()}"
    ground = pixel(img, ox + 10, oy + 10)
    if ground != "404040":
        return None, f"the ground (404040) is {ground}"
    return (ox, oy), None


frames = {}


def frame_times(n):
    """n full frames timed by atlas (a window shown again settles the desk) -> microseconds."""
    got = []
    for _ in range(n):
        d.click_pic(*d.latest("dbapp", "Settings"))
        d.wait("atlas: settings open", 10)
        d.keys("ctrl", "w")
        d.wait("atlas: settings closed", 10)
        time.sleep(0.4)
        v = d.last("atlas: dbg frame ")
        got.append(int(v.split()[0]))
    return got


# ---- another program's window: exact colours in every look
d.type_text("uipatch\n")
d.wait("atlas: window 17 opened by pid", 30)
d.wait("atlas: window 17 shown")
time.sleep(1.5)
d.type_text(" ")                                  # (a key to it: atlas logs places as it settles)
time.sleep(0.6)
for k in range(4):
    if k:                                         # the look, from Settings (then closed: the window in front again)
        d.click_pic(*d.latest("dbapp", "Settings"))
        d.wait("atlas: settings open", 10)
        time.sleep(0.4)
        d.click_pic(*d.latest("set", 100 + k))
        d.wait(f"atlas: look {k}", 10)
        d.keys("ctrl", "w")
        d.wait("atlas: settings closed", 10)
        time.sleep(0.6)
    x0, y0, x1, y1 = d.window(17)
    d.mouse_to(1900, 1060)                        # (the pointer is atlas's: in the look's colours, away from the patches)
    time.sleep(0.3)
    img = d.shot(f"0{k}-patches-{LOOKS[k].split()[0].lower()}")
    frames[LOOKS[k]] = frame_times(8)
    if MEASURE:
        continue
    at, why = patches(img, (x0, y0, x1, y1))
    if not at:
        d.fail(f"{LOOKS[k]}: uipatch's pixels are not as it drew them: {why}")
    tab = (x0 - 6, y0 - 40, x1 + 6, at[1] - 1)      # its tab, the strip above its insides, the frame's top
    if count(img, ACCENT[k], tab, 1) < 20:
        d.fail(f"{LOOKS[k]}: the window's frame or tab does not have the look ({ACCENT[k]})")
    if k < 2 and count(img, "4be3c1", tab, 1):
        d.fail(f"{LOOKS[k]}: the frame shows atlas's own colours untinted")
    print(f"ok: {LOOKS[k]}: uipatch's 10 patches exact (64000 pixels), its frame in the look's colours ({ACCENT[k]})")

for name, t in frames.items():
    t = sorted(t)
    print(f"frame: {name}: full frame {t[len(t) // 2]} us (median of {t})")
d.type_text("q")
print(f"PASS ({time.time() - start:.0f} s)")
