#!/usr/bin/env python3
"""Serial console of a Raspberry Pi over a USB serial cable, usable from
scripts (and by an agent) rather than an interactive terminal.

  piconsole.py listen [--dev D] [--log F]   read the port forever, append to the log
  piconsole.py send TEXT [--no-cr]          type TEXT (and Enter) on the console
  piconsole.py tail [-n N]                  print the log's last N lines
  piconsole.py wait REGEX [--timeout S]     wait until the log shows REGEX

Run `listen` once in the background; `send`, `tail` and `wait` work beside it.
The port is 115200 8N1, raw. Only the standard library is used."""

import argparse
import os
import re
import sys
import termios
import time

DEV = os.environ.get("PICONSOLE_DEV", "/dev/ttyUSB0")
LOG = os.environ.get("PICONSOLE_LOG", "/tmp/piconsole.log")


def open_port(dev, flags):
    fd = os.open(dev, flags | os.O_NOCTTY)
    a = termios.tcgetattr(fd)
    a[0] = 0                                    # iflag: no translation
    a[1] = 0                                    # oflag: raw output
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0                                    # lflag: no echo, no canon
    a[4] = a[5] = termios.B115200
    a[6][termios.VMIN] = 1
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def listen(args):
    fd = open_port(args.dev, os.O_RDONLY)
    with open(args.log, "ab", buffering=0) as log:
        log.write(b"\n--- piconsole: listening on %s at %s ---\n"
                  % (args.dev.encode(), time.strftime("%H:%M:%S").encode()))
        while True:
            data = os.read(fd, 4096)
            if not data:
                time.sleep(0.05)
                continue
            log.write(data.replace(b"\r\n", b"\n").replace(b"\r", b""))


def send(args):
    fd = open_port(args.dev, os.O_WRONLY)
    text = args.text.encode().decode("unicode_escape").encode("latin-1")
    if not args.no_cr:
        text += b"\r"
    for b in text:                              # slowly: the Pi has no flow control
        os.write(fd, bytes([b]))
        time.sleep(0.002)
    termios.tcdrain(fd)
    os.close(fd)


def tail(args):
    try:
        with open(args.log, "rb") as f:
            lines = f.read().decode("utf-8", "replace").splitlines()
    except FileNotFoundError:
        sys.exit("no log yet: start `piconsole.py listen` first")
    print("\n".join(lines[-args.n:]))


def wait(args):
    pat = re.compile(args.regex)
    start = os.path.getsize(args.log) if args.new else 0
    end = time.time() + args.timeout
    while time.time() < end:
        with open(args.log, "rb") as f:
            f.seek(start)
            text = f.read().decode("utf-8", "replace")
        m = pat.search(text)
        if m:
            print(text[max(0, m.start() - 2000):m.end()])
            return
        time.sleep(0.2)
    sys.exit("timeout: %r not seen" % args.regex)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dev", default=DEV)
    p.add_argument("--log", default=LOG)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("listen").set_defaults(fn=listen)
    s = sub.add_parser("send")
    s.add_argument("text")
    s.add_argument("--no-cr", action="store_true", help="do not press Enter")
    s.set_defaults(fn=send)
    t = sub.add_parser("tail")
    t.add_argument("-n", type=int, default=40)
    t.set_defaults(fn=tail)
    w = sub.add_parser("wait")
    w.add_argument("regex")
    w.add_argument("--timeout", type=float, default=30)
    w.add_argument("--new", action="store_true", help="only text after now")
    w.set_defaults(fn=wait)
    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
