#!/usr/bin/env python3
"""Test other programs' windows and the UI toolkit on the desktop (make
ui-test), through QEMU's control socket (QMP): screenshots, keys and the
mouse, checked against atlas's log lines and against the pixels.

  first start (accounts) -> alice's terminal -> uipatch (tests/ui/uipatch.c):
  a window of exact colour patches: every patch pixel is checked to be
  exactly the program's colour while the window's frame and tab have the
  look's accent; atlas's full-frame times are collected. Then uidemo: every
  widget, its canvas exact before and after the theme changes (the zoom).

Usage: uitest.py <cpus> <screenshots dir> <disk image> <qemu command line...>
(UI_MEASURE=1: only the frame times, nothing checked.) The test works in a
copy of the image. Screenshots (PNG) are kept.

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qdesk import Desk, find, count, pixel

cpus, shots, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
MEASURE = os.environ.get("UI_MEASURE") == "1"
d = Desk(shots, image, cmdline)
start = time.time()
d.login(cpus)
print(f"ok: alice logged in on the desktop ({time.time() - start:.1f} s)")

PATCH = ["ff0000", "00ff00", "0000ff", "ffffff", "000000", "808080", "123456", "fedcba", "4be3c1", "0a0f1c"]
ACCENT = "ffc93c"                                 # the look's accent (glass: a focused window's tab)


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


# ---- another program's window: exact colours
d.type_text("uipatch\n")
d.wait("atlas: window 17 opened by pid", 30)
d.wait("atlas: window 17 shown")
time.sleep(1.5)
d.type_text(" ")                                  # (a key to it: atlas logs places as it settles)
time.sleep(0.6)
x0, y0, x1, y1 = d.window(17)
d.mouse_to(1900, 1060)                            # (the pointer is atlas's: away from the patches)
time.sleep(0.3)
img = d.shot("00-patches")
frames["Glass"] = frame_times(8)
if not MEASURE:
    at, why = patches(img, (x0, y0, x1, y1))
    if not at:
        d.fail(f"uipatch's pixels are not as it drew them: {why}")
    tab = (x0 - 6, y0 - 40, x1 + 6, at[1] - 1)      # its tab, the strip above its insides, the frame's top
    if count(img, ACCENT, tab, 1) < 20:
        d.fail(f"the window's tab does not have the look's accent ({ACCENT})")
    print(f"ok: uipatch's 10 patches exact (64000 pixels), its tab in the look's accent ({ACCENT})")

for name, t in frames.items():
    t = sorted(t)
    print(f"frame: {name}: full frame {t[len(t) // 2]} us (best {t[0]}, median of {t})")
d.click_pic(*d.latest("wclose", 17))              # its close box: the program is told, and ends
d.wait("atlas: window 17 closed", 10)
time.sleep(0.5)
if MEASURE:
    print(f"PASS ({time.time() - start:.0f} s)")
    sys.exit(0)

# ---- the toolkit: uidemo, every widget
d.type_text("uidemo -v\n")
d.wait("atlas: window 17 opened by pid", 30)
d.wait("uidemo: ready", 10)
time.sleep(1.5)
d.type_text(" ")                                 # (atlas logs where the window is)
time.sleep(0.8)


def at(name):
    """Where uidemo said a widget is (its window's pixels) -> the screen's."""
    v = d.last(f"uidemo: at {name} ")
    if v is None:
        d.fail(f"uidemo did not say where its {name} is")
    x0, y0, x1, y1 = (int(a) for a in d.last("atlas: dbg window 17 at ").split()[:4])
    ox, oy = round((x0 + 2) * 0.75), round((y0 + 34) * 0.75)    # (its insides: 2 and 34 in, client.c)
    x, y = (int(a) for a in v.split()[:2])
    return ox + x, oy + y


img = d.shot("10-uidemo")
box = d.window(17)
for rgb, what in ((ACCENT, "the accent (the focused tab, a check box)"), ("d9e0f2", "the buttons' face"),
                  ("fbfcff", "the ground"), ("1a2238", "the text")):
    if count(img, rgb, box) < 30:
        d.fail(f"uidemo: no {what} ({rgb})")
print("ok: uidemo drawn in the desktop's colours (its accent, buttons, ground, text)")

# a click on a button
d.click(*at("button"))
d.wait("uidemo: clicked Button", 10)
print("ok: uidemo: a click on a button")

# typing in the text field, Enter
d.click(*at("field"))
time.sleep(0.3)
d.type_text("hello toolkit\n")
d.wait("uidemo: field hello toolkit", 10)
d.keys("ctrl", "a")
d.type_text("again\n")
d.wait("uidemo: field again", 10)
img = d.shot("11-uidemo-typed")
print("ok: uidemo: typed in the text field (Enter; Ctrl+A then typing replaced it)")

# a menu opened by the keyboard: F10 (File), Right (Edit), Down twice, Enter: "Pick me"
d.keys("f10")
time.sleep(0.4)
d.keys("right")
time.sleep(0.3)
d.keys("down")
time.sleep(0.3)
img = d.shot("12-uidemo-menu")
d.keys("ret")
d.wait("uidemo: menu Pick me", 10)
d.keys("ctrl", "k")                               # a shortcut: Edit, Clear the field
d.wait("uidemo: menu Clear", 10)
print("ok: uidemo: a menu opened and used from the keyboard (F10, arrows, Enter), a shortcut (Ctrl+K)")

# a dialog: the file dialog from the menu, Escape closes it
d.keys("ctrl", "o")
d.wait("uidemo: menu Open", 10)
time.sleep(0.6)
img = d.shot("13-uidemo-open")
d.keys("esc")
d.wait("uidemo: no file", 10)
print("ok: uidemo: the file dialog (Ctrl+O), cancelled with Escape")

# the list (View, The list: Ctrl+E): a row clicked, keys, its right-click menu
d.keys("ctrl", "e")
d.wait("uidemo: menu List", 10)
time.sleep(0.6)
lx, ly = at("list")
d.click(lx, ly)
d.wait("uidemo: row ", 10)
d.keys("down"); d.keys("down")
d.wait("uidemo: row ", 10)
d.click(lx, ly + 40, "right")
time.sleep(0.5)
d.shot("16-uidemo-list-menu")
d.click(lx + 40, ly + 40 + 12)                   # its first item
d.wait("uidemo: menu First row", 10)
print("ok: uidemo: the list (a click, arrow keys), its right-click menu")
d.click(*at("button") if False else (lx, ly))
d.keys("ctrl", "o")
d.wait("uidemo: menu Open", 10)
d.keys("esc")
d.wait("uidemo: no file", 10)

# the canvas (View, The canvas: Ctrl+G): the program's pixels exact, before and after the theme changed live (the zoom)
def canvas_exact(name):
    img = d.shot(name)
    x0, y0, x1, y1 = d.window(17)
    at_ = find(img, "ff0000", (x0, y0, x1, y1), 1)
    if not at_:
        d.fail(f"{name}: no pure red in uidemo's canvas")
    w, h, px = img
    for k, rgb in enumerate(PATCH):
        want = bytes.fromhex(rgb)
        cx, cy = at_[0] + k % 5 * 70, at_[1] + k // 5 * 70
        for y in range(cy, cy + 60):
            if px[(y * w + cx) * 3:(y * w + cx + 60) * 3] != want * 60:
                d.fail(f"{name}: the canvas's patch {k} ({rgb}) is not exact at row {y}")
    return img


d.keys("ctrl", "g")
d.wait("uidemo: menu Canvas", 10)
time.sleep(0.6)
d.mouse_to(1900, 1060)
time.sleep(0.3)
canvas_exact("14-uidemo-canvas")
d.click_pic(*d.latest("dbapp", "Settings"))      # everything at 200%, then 100% again, from Settings
d.wait("atlas: settings open", 10)
time.sleep(0.4)
d.click_pic(*d.latest("set", 181))                # (its Look section)
time.sleep(0.4)
d.click_pic(*d.latest("set", 115))
d.wait("atlas: zoom 200%", 10)
d.wait("uidemo: theme 0", 10)
time.sleep(1.0)
d.click(*(v * 1.5 for v in d.latest("set", 112)))  # (the picture at 200%: 1.5 screen pixels each)
d.wait("atlas: zoom 100%", 10)
d.wait("uidemo: theme 0", 10)
d.keys("ctrl", "w")
d.wait("atlas: settings closed", 10)
time.sleep(0.8)
d.mouse_to(1900, 1060)
time.sleep(0.3)
img = canvas_exact("15-uidemo-canvas-again")
box = d.window(17)
if count(img, ACCENT, box) < 30 or count(img, "fbfcff", box) < 30:
    d.fail("uidemo lost the desktop's colours after the theme changed")
print("ok: uidemo: the canvas's 6 x 6000 pixels exact, before and after the theme changed live (200% and back)")
d.keys("ctrl", "q")
d.wait("uidemo: menu Quit", 10)
d.wait("atlas: dbg frame", 10)                    # (the desk settled without it: its window is gone)
if b"dbg window 17 at" in d.full[d.full.rfind(b"uidemo: menu Quit"):]:
    d.fail("uidemo's window stayed after Quit")
print("ok: uidemo: Quit (Ctrl+Q) closed its window")
print(f"PASS ({time.time() - start:.0f} s)")
