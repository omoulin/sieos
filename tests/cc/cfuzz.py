"""Random statements and control flow: loops of every kind (bounded), if/else,
switch with fall-through, break, continue, goto, early returns, local arrays,
calls between the generated functions (a bounded depth); unsigned arithmetic,
so nothing is undefined. Built by the host compiler and by sicc (-O0, -O2);
the printed results must agree. Usage: cfuzz.py SICC [programs] [seed]
(QEMU=the emulator: for AArch64, see a64/target.py).
Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only"""
import random, subprocess, sys, os
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "a64"))
from target import Target
sicc = sys.argv[1]
nprog = int(sys.argv[2]) if len(sys.argv) > 2 else 20
random.seed(int(sys.argv[3]) if len(sys.argv) > 3 else 3)
out = os.environ.get("OUT", "/tmp/sicc-cfuzz")
os.makedirs(out, exist_ok=True)
T = Target(sicc, out)

class Gen:
    def __init__(self, nf):
        self.nf, self.lab = nf, 0
    def expr(self, vs, d=0):
        if d > 2 or random.random() < 0.3:
            return random.choice(vs + [str(random.randint(0, 99)), "arr[%s & 7]" % random.choice(vs)])
        a, b = self.expr(vs, d + 1), self.expr(vs, d + 1)
        op = random.choice(["+", "-", "*", "&", "|", "^", "<<", ">>", "/", "%", "<", "==", "!=", "&&", "||", "?:"])
        if op in ("<<", ">>"): return f"({a} {op} ({b} & 31))"
        if op in ("/", "%"): return f"({b} ? {a} {op} {b} : {a})"
        if op == "?:": return f"({a} > {b} ? {a} : {b} + 1)"
        return f"({a} {op} {b})"
    def stmt(self, vs, d, fn, inloop, labels):
        r = random.random()
        v = random.choice([x for x in vs if not x.startswith("i")])     # (loop counters: read only)
        if d > 3 or r < 0.3: return [f"{v} = {self.expr(vs)};"]
        if r < 0.4: return [f"arr[{self.expr(vs)} & 7] += {self.expr(vs)};"]
        if r < 0.5:
            return [f"if ({self.expr(vs)}) {{"] + self.block(vs, d + 1, fn, inloop, labels) + ["} else {"] + self.block(vs, d + 1, fn, inloop, labels) + ["}"]
        if r < 0.58:
            i = f"i{d}"
            return [f"for (unsigned {i} = 0; {i} < {random.randint(1, 6)}; {i}++) {{"] + self.block(vs + [i], d + 1, fn, True, labels) + ["}"]
        if r < 0.64:
            c = f"c{d}"
            return [f"{{ unsigned {c} = {random.randint(1, 5)}; while ({c}--) {{"] + self.block(vs, d + 1, fn, True, labels) + ["} }"]
        if r < 0.69:
            c = f"c{d}"
            return [f"{{ unsigned {c} = {random.randint(1, 4)}; do {{"] + self.block(vs, d + 1, fn, True, labels) + [f"}} while (--{c}); }}"]
        if r < 0.77:
            out = [f"switch ({self.expr(vs)} % 5) {{"]
            for k in random.sample(range(5), random.randint(1, 4)):
                out.append(f"case {k}:")
                out += self.block(vs, d + 1, fn, inloop, labels)
                if random.random() < 0.6: out.append("break;")
            if random.random() < 0.5: out += ["default:"] + self.block(vs, d + 1, fn, inloop, labels)
            return out + ["}"]
        if r < 0.82 and inloop: return [random.choice(["break;", "continue;"]) if random.random() < 0.5 else f"if ({self.expr(vs)}) {random.choice(['break', 'continue'])};"]
        if r < 0.86: return [f"if ({self.expr(vs)}) return {self.expr(vs)};"]
        if r < 0.9 and fn > 0 and not inloop: return [f"{v} += f{random.randrange(fn)}({self.expr(vs)}, {self.expr(vs)}, depth + 1);"]
        if r < 0.95:
            self.lab += 1
            l = f"L{self.lab}"
            labels.append(l)
            return [f"if ({self.expr(vs)}) goto {l};"] + self.block(vs, d + 1, fn, inloop, labels) + [f"{l}: ;"]
        return [f"{{ unsigned t = {self.expr(vs)}; {v} = t * 3 + {v}; }}"]
    def block(self, vs, d, fn, inloop, labels):
        out = []
        for _ in range(random.randint(1, 3)): out += self.stmt(vs, d, fn, inloop, labels)
        return out
    def program(self):
        lines = ["int printf(const char *, ...);"]
        for f in range(self.nf):
            body = [f"static unsigned f{f}(unsigned a, unsigned b, int depth) {{", "    unsigned x = a ^ 5, y = b + 1, z = a * b;", "    unsigned arr[8] = { a, b, 3, 4, 5, 6, 7, 8 };",
                    "    if (depth > 2) return a + b;"]
            body += ["    " + l for l in self.block(["a", "b", "x", "y", "z"], 0, f, False, [])]
            body += ["    return x + y * 3 + z * 7 + arr[1] + arr[5];", "}"]
            lines += body
        lines.append("int main(void) {")
        lines.append("    unsigned long h = 0;")
        lines.append(f"    for (unsigned i = 0; i < 40; i++) for (unsigned j = 0; j < 5; j++) h = h * 1000003 + f{self.nf - 1}(i * 2654435761u, j * 40503u + i, 0);")
        lines.append('    printf("%lx\\n", h);')
        lines.append("    return 0;\n}")
        return "\n".join(lines) + "\n"

bad = 0
for p in range(nprog):
    src = f"{out}/c{p}.c"
    open(src, "w").write(Gen(random.randint(2, 5)).program())
    T.ref(src, f"{out}/c{p}.ref", ["-O0"])
    want = T.run(f"{out}/c{p}.ref", False)
    for o in ("-O0", "-O2"):
        ok, err = T.build(src, f"{out}/c{p}.bin", [o])
        got = T.run(f"{out}/c{p}.bin") if ok else ""
        if got != want:
            print(f"cfuzz: {src} {o}: want {want.strip()}, got {got.strip()} {err[:300]}")
            bad += 1
print(f"sicc cfuzz: {nprog - bad if bad <= nprog else 0} of {nprog} programs agree (-O0 and -O2)")
sys.exit(1 if bad else 0)
