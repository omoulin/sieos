#!/usr/bin/env python3
"""Print a flattened device tree (.dtb): nodes and properties, for looking at
what a board's firmware describes. Usage: fdtdump.py file.dtb [path-filter]

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
"""
import struct, sys

data = open(sys.argv[1], "rb").read()
want = sys.argv[2] if len(sys.argv) > 2 else ""
magic, size, off_st, off_str = struct.unpack(">IIII", data[:16])
assert magic == 0xD00DFEED, "not a device tree"
strings = data[off_str:]
pos, path, show = off_st, [], []

def text(v):
    if v and v[-1] == 0 and all(32 <= c < 127 or c == 0 for c in v[:-1]):
        return " | ".join(x.decode() for x in v[:-1].split(b"\0"))
    if len(v) % 4 == 0:
        return " ".join("%#x" % w for w in struct.unpack(">%dI" % (len(v) // 4), v))
    return v.hex()

while True:
    tok = struct.unpack(">I", data[pos:pos + 4])[0]; pos += 4
    if tok == 1:
        end = data.index(b"\0", pos); name = data[pos:end].decode(); pos = (end + 4) & ~3
        path.append(name)
        p = "/" + "/".join(path[1:])
        if p.startswith(want) or not want: print(p)
    elif tok == 2:
        path.pop()
    elif tok == 3:
        ln, no = struct.unpack(">II", data[pos:pos + 8]); pos += 8
        v = data[pos:pos + ln]; pos = (pos + ln + 3) & ~3
        name = strings[no:strings.index(b"\0", no)].decode()
        p = "/" + "/".join(path[1:])
        if p.startswith(want) or not want: print("    %s = %s" % (name, text(v)))
    elif tok == 4:
        continue
    else:
        break
