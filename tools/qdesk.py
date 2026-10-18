"""The desktop in QEMU for the test scripts (tools/uitest.py): QEMU started
with its control socket (QMP), the serial port read, keys, the mouse,
screenshots, the first start (accounts) and alice's desktop.

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import atexit, json, os, select, shutil, socket, subprocess, sys, time
import qserial

SCREEN, ZOOM = (1920, 1080), 0.75          # the screen; screen pixels per picture pixel (atlas logs picture places)
KEYS = {" ": "spc", "\n": "ret", "\t": "tab", "\b": "backspace", ".": "dot", "/": "slash", "-": "minus", "=": "equal",
        ",": "comma", ";": "semicolon", "\x1b": "esc"}
SHIFTED = {"?": "slash", "_": "minus", "+": "equal", ">": "dot", "<": "comma", ":": "semicolon", '"': "apostrophe"}


class Desk:
    def __init__(self, shots, image, cmdline, tag="ui"):
        self.shots = shots
        os.makedirs(shots, exist_ok=True)
        copy = image + "." + tag
        shutil.copyfile(image, copy)
        sock = os.path.join(shots, "qmp.sock")
        if os.path.exists(sock):
            os.unlink(sock)
        cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
        cmdline += ["-qmp", f"unix:{sock},server=on,wait=off"]
        self.qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        atexit.register(lambda: self.qemu.poll() is None and self.qemu.kill())
        self.full, self.pos = b"", 0
        for _ in range(100):
            try:
                self.qmp = socket.socket(socket.AF_UNIX)
                self.qmp.connect(sock)
                break
            except OSError:
                time.sleep(0.05)
        self.qf = self.qmp.makefile("rw")
        json.loads(self.qf.readline())
        self.q("qmp_capabilities")

    def fail(self, msg):
        self.qemu.kill()
        self.pump(0.2)
        open(os.path.join(self.shots, "serial.log"), "wb").write(self.full)
        sys.exit(f"FAIL: {msg}\n--- serial (last part) ---\n{self.full[-3000:].decode(errors='replace')}")

    def pump(self, t):
        if select.select([self.qemu.stdout], [], [], t)[0]:
            self.full += self.qemu.stdout.read1(65536)

    def wait(self, text, timeout=30):
        """Wait for text after the last wait -> the rest of its line."""
        end = time.time() + timeout
        while text.encode() not in self.full[self.pos:]:
            if time.time() > end or self.qemu.poll() is not None:
                self.fail(f"waiting for {text!r}")
            self.pump(0.2)
        i = self.full.index(text.encode(), self.pos) + len(text)
        while b"\n" not in self.full[i:] and time.time() < end:
            self.pump(0.1)
        e = self.full.find(b"\n", i)
        self.pos = e + 1
        return self.full[i:e].decode(errors="replace").strip()

    def last(self, text):
        """The rest of the line of the last `text` seen (or None)."""
        self.pump(0.3)
        k = self.full.rfind(text.encode())
        if k < 0:
            return None
        return self.full[k + len(text):].split(b"\n", 1)[0].decode(errors="replace").strip()

    def q(self, cmd, **args):
        self.qf.write(json.dumps({"execute": cmd, "arguments": args}) + "\n")
        self.qf.flush()
        while True:
            r = json.loads(self.qf.readline())
            if "return" in r:
                return r["return"]
            if "error" in r:
                self.fail(f"QMP {cmd}: {r['error']}")

    def type_text(self, s):
        for c in s:
            k = ["shift", c.lower()] if c.isalpha() and c.isupper() else ["shift", SHIFTED[c]] if c in SHIFTED else [KEYS.get(c, c)]
            self.q("send-key", keys=[{"type": "qcode", "data": x} for x in k])
            time.sleep(0.03)

    def keys(self, *names):
        self.q("send-key", keys=[{"type": "qcode", "data": n} for n in names])
        time.sleep(0.08)

    def mouse_to(self, x, y):
        """(x, y): screen pixels."""
        self.q("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": int((x + 0.5) * 32768 / SCREEN[0])}},
                                           {"type": "abs", "data": {"axis": "y", "value": int((y + 0.5) * 32768 / SCREEN[1])}}])
        time.sleep(0.05)

    def click(self, x, y, name="left"):
        """(x, y): screen pixels."""
        self.mouse_to(x, y)
        for d in (True, False):
            self.q("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": name}}])
            time.sleep(0.05)

    def click_pic(self, x, y):
        """(x, y): the desktop's picture (where atlas logs things)."""
        self.click(x * ZOOM, y * ZOOM)

    def shot(self, name):
        """A screenshot: kept as PNG -> (w, h, RGB bytes)."""
        ppm = os.path.join(self.shots, name + ".ppm")
        self.q("screendump", filename=ppm)
        self.q("screendump", filename=os.path.join(self.shots, name + ".png"), format="png")
        data = open(ppm, "rb").read()
        os.unlink(ppm)
        parts = data.split(b"\n", 3)
        w, h = map(int, parts[1].split())
        return w, h, parts[3]

    def window(self, w):
        """The last place logged for window w (picture) -> its box in screen pixels."""
        v = self.last(f"atlas: dbg window {w} at ")
        if v is None:
            self.fail(f"no place logged for window {w}")
        return tuple(int(int(a) * ZOOM) for a in v.split()[:4])

    def latest(self, prefix, name):
        """"atlas: dbg PREFIX NAME at X Y" -> (x, y) in the picture."""
        v = self.last(f"atlas: dbg {prefix} {name} at ")
        if v is None:
            self.fail(f"no place logged for {prefix} {name}")
        x, y = v.split()[:2]
        return int(x), int(y)

    def login(self, cpus):
        """The first start (accounts root and alice), alice's desktop; root
        on the serial console makes atlas log where it draws things."""
        self.wait(f"mk: {cpus} CPU")
        self.wait("atlas: first start: accounts needed")
        self.wait("atlas: greeter")
        time.sleep(0.5)
        self.type_text("rootpass1\trootpass1\n")
        self.wait("atlas: setup: step 2")
        self.type_text("alice\tAlice Example\talicepass1\talicepass1\n")
        self.wait("atlas: setup: step 3")
        self.type_text("\n")
        self.wait("atlas: login alice: ok", 30)
        self.wait("shown")
        qserial.type_line(self.qemu, "root")
        time.sleep(0.5)
        for _ in range(20):
            self.pump(0.2)
            if b"Password:" in self.full[-200:]:
                break
        qserial.type_line(self.qemu, "rootpass1")
        time.sleep(0.5)
        qserial.type_line(self.qemu, "echo 1 > /tmp/atlas.debug")
        time.sleep(1)

    def send(self, line):
        qserial.type_line(self.qemu, line)


def pixel(img, x, y):
    w, h, px = img
    return px[(y * w + x) * 3:(y * w + x) * 3 + 3].hex()


def find(img, rgb, box, step=2):
    """The first pixel of colour rgb in box (row by row) -> (x, y), or None."""
    w, h, px = img
    want = bytes.fromhex(rgb)
    x0, y0, x1, y1 = (max(0, int(v)) for v in box)
    for y in range(y0, min(y1, h), step):
        row = px[y * w * 3:(y + 1) * w * 3]
        i = row.find(want, x0 * 3, min(x1, w) * 3)
        while i >= 0 and i % 3:
            i = row.find(want, i + 1, min(x1, w) * 3)
        if i >= 0:
            return i // 3, y
    return None


def count(img, rgb, box, step=2):
    """Pixels of colour rgb in box, every step-th one."""
    w, h, px = img
    want = bytes.fromhex(rgb)
    x0, y0, x1, y1 = (max(0, int(v)) for v in box)
    n = 0
    for y in range(y0, min(y1, h), step):
        row = px[y * w * 3:(y + 1) * w * 3]
        for x in range(x0, min(x1, w), step):
            if row[x * 3:x * 3 + 3] == want:
                n += 1
    return n


def look(rgb, kind=0, scan=False):
    """A colour atlas draws -> what the screen shows in look `kind` (draw.c)."""
    if kind in (2, 3):
        return rgb
    r, g, b = (int(rgb[i:i + 2], 16) for i in (0, 2, 4))
    v = min(255, ((r * 77 + g * 150 + b * 29) >> 8) * 5 // 4)
    if scan:
        v = v * (140 + 116 * v // 255) // 256 if kind == 0 else v * (180 + 76 * v // 255) // 256
    t = (0x46, 0xff, 0x7a) if kind == 0 else (0xff, 0xb0, 0x38)
    return "".join(f"{c * v // 255:02x}" for c in t)
