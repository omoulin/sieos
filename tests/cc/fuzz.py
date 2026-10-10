#!/usr/bin/env python3
"""Random integer expressions over every integer type, compiled by the host
compiler (-fwrapv: wrapping overflow, as sicc's code does) and by sicc; the
printed results must agree. Usage: fuzz.py SICC [programs] [seed]
(QEMU=the emulator: for AArch64, see a64/target.py).
Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only"""
import random, subprocess, sys, os
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "a64"))
from target import Target
sicc = sys.argv[1]
nprog = int(sys.argv[2]) if len(sys.argv) > 2 else 20
random.seed(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
out = os.environ.get("OUT", "/tmp/sicc-fuzz")
os.makedirs(out, exist_ok=True)
T = Target(sicc, out)
TYPES = ["signed char", "unsigned char", "short", "unsigned short", "int", "unsigned", "long", "unsigned long", "_Bool"]

def expr(d, vars_):
    if d <= 0 or random.random() < 0.2:
        if random.random() < 0.7:
            return random.choice(vars_)
        return random.choice(["0", "1", "-1", "7", "255", "-128", "0x7fffffff", "0x80000000u", "0xffffffffffffffffUL", "1000003L", "3", "-5"])
    a, b = expr(d - 1, vars_), expr(d - 1, vars_)
    op = random.choice(["+", "-", "*", "&", "|", "^", "<<", ">>", "/", "%", "==", "!=", "<", "<=", ">", ">=", "&&", "||", "cast", "neg", "not", "cond", "tilde"])
    if op in ("<<", ">>"):
        return f"(({a}) {op} (({b}) & 31))"
    if op in ("/", "%"):
        return f"(({a}) {op} (int)((({b}) & 0x7f) + 1))"
    if op == "cast":
        return f"(({random.choice(TYPES)})({a}))"
    if op == "neg":
        return f"(-({a}))"
    if op == "not":
        return f"(!({a}))"
    if op == "tilde":
        return f"(~({a}))"
    if op == "cond":
        return f"(({a}) ? ({b}) : ({expr(d - 1, vars_)}))"
    return f"(({a}) {op} ({b}))"

fails = 0
for p in range(nprog):
    names = [f"v{i}" for i in range(len(TYPES))]
    src = ["int printf(const char *, ...);"]
    src += [f"{t} {n} = ({t}){random.randint(-2**40, 2**40)}L;" for t, n in zip(TYPES, names)]
    src.append("int main(void) {")
    for i in range(150):
        e = expr(random.randint(1, 5), names)
        src.append(f'    printf("%d %lld\\n", {i}, (long long)({e}));')
        if random.random() < 0.2:
            t = random.randrange(len(TYPES))
            src.append(f"    {names[t]} = ({TYPES[t]})({expr(2, names)});")
            if random.random() < 0.5:
                src.append(f"    {names[t]} {random.choice(['+=', '-=', '*=', '^=', '|=', '&='])} {expr(2, names)};")
    src.append("    return 0;\n}")
    c = f"{out}/f{p}.c"
    open(c, "w").write("\n".join(src) + "\n")
    T.ref(c, f"{out}/f{p}.ref", ["-O0", "-fwrapv"])
    ok, err = T.build(c, f"{out}/f{p}.bin", ["-O2"])
    if not ok:
        print(f"FAIL (build) {c}: {err[:300]}"); fails += 1; continue
    want = T.run(f"{out}/f{p}.ref", False)
    got = T.run(f"{out}/f{p}.bin")
    if want != got:
        fails += 1
        w, g = want.splitlines(), got.splitlines()
        for a, b in zip(w, g):
            if a != b: print(f"FAIL {c}: want '{a}', got '{b}'"); break
        else: print(f"FAIL {c}: output length differs")
print(f"sicc fuzz: {nprog - fails} of {nprog} programs agree ({nprog * 150} expressions)")
sys.exit(1 if fails else 0)
