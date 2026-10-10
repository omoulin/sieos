"""The AArch64 calling convention, checked against the standard rather than
against sicc: random function types (integers, __int128, floats, complex
numbers, HFAs, structs and unions of every size, many arguments: some on the
stack, struct results, variadic functions). A model of AAPCS64's rules
(below, written from the standard) says where each argument and result must
be; abi_tramp.S (hand-written assembly) records or supplies exactly those
registers and stack bytes, and the generated C file compares, both ways:
  sicc calls abi_rec: are the arguments where the model says? does sicc read
    the result from where the model says (x0/x1, v0-v3, or the x8 buffer)?
  abi_call calls a sicc function: does it find its arguments there (also
    through va_arg), and put its result there? Are x19-x28, d8-d15 and sp kept?
Unused register bits are filled with garbage: values narrower than a register
must not be trusted to be extended.
Usage: abi_a64.py SICC QEMU [cases] [seed]  (OUT: the work directory)
Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only"""
import os, random, subprocess, sys

sicc, qemu = sys.argv[1], sys.argv[2]
ncase = int(sys.argv[3]) if len(sys.argv) > 3 else 60
random.seed(int(sys.argv[4]) if len(sys.argv) > 4 else 11)
out = os.environ.get("OUT", "/tmp/sicc-abi-a64")
here = os.path.dirname(os.path.abspath(__file__))
os.makedirs(out, exist_ok=True)

# ---- types: scalars by name, ("struct"/"union", index) for aggregates
SCAL = {  # name: (size, align, float base or None)
    "char": (1, 1, None), "signed char": (1, 1, None), "short": (2, 2, None), "unsigned short": (2, 2, None),
    "int": (4, 4, None), "unsigned": (4, 4, None), "long": (8, 8, None), "unsigned long": (8, 8, None),
    "_Bool": (1, 1, None), "void *": (8, 8, None), "__int128": (16, 16, None), "unsigned __int128": (16, 16, None),
    "float": (4, 4, "float"), "double": (8, 8, "double"), "long double": (8, 8, "double"),   # long double = double on SIEOS
    "float _Complex": (8, 4, "float"), "double _Complex": (16, 8, "double"),
}
MEMBER = ["char", "short", "int", "long", "unsigned", "float", "double", "long double", "__int128"]
aggs = []          # (kind, [(type, array length or 0)])

def up(x, a): return (x + a - 1) // a * a

def size_align(t):
    if isinstance(t, str): return SCAL[t][:2]
    kind, mem = aggs[t[1]]
    size = al = 0
    for m, n in mem:
        s, a = size_align(m)
        s *= n or 1
        al = max(al, a)
        size = max(size, s) if kind == "union" else up(size, a) + s
    return up(size, al), al

def leaves(t):
    """the float type of every fundamental member, or None if any is not floating point"""
    if isinstance(t, str):
        b = SCAL[t][2]
        if not b: return None
        return [b] * (2 if "Complex" in t else 1)
    kind, mem = aggs[t[1]]
    if kind == "union":     # every member made of one float type; the count by size (B.5)
        bases = set()
        for m, n in mem:
            l = leaves(m)
            if l is None: return None
            bases |= set(l)
        if len(bases) != 1: return None
        b = bases.pop()
        return [b] * (size_align(t)[0] // SCAL[b][0])
    r = []
    for m, n in mem:
        l = leaves(m)
        if l is None: return None
        r += l * (n or 1)
    return r

def hfa(t):
    """(element size, count) for a floating-point value or homogeneous floating-point aggregate"""
    l = leaves(t)
    if not l or len(set(l)) != 1 or len(l) > 4: return None
    es = SCAL[l[0]][0]
    return (es, len(l)) if es * len(l) == size_align(t)[0] else None

def place(params):
    """AAPCS64 stage C: where each argument goes"""
    ngrn = nsrn = nsaa = 0
    locs = []
    for t in params:
        size, al = size_align(t)
        h = hfa(t)
        if h:                                            # C.1-C.6: floats, HFAs
            if nsrn + h[1] <= 8: locs.append(("v", nsrn, h)); nsrn += h[1]; continue
            nsrn = 8
            nsaa = up(nsaa, max(8, min(16, al))); locs.append(("s", nsaa)); nsaa += up(size, 8); continue
        if size > 16:                                    # B.4: a copy, passed by address (as an integer)
            if ngrn < 8: locs.append(("rx", ngrn)); ngrn += 1
            else: locs.append(("rs", nsaa)); nsaa += 8
            continue
        if al == 16: ngrn = up(ngrn, 2)                  # C.9, C.12: 16-byte alignment: an even register
        n = up(size, 8) // 8
        if ngrn + n <= 8: locs.append(("x", ngrn)); ngrn += n; continue
        ngrn = 8                                         # C.13-C.16: the stack, 8-byte slots at least
        nsaa = up(nsaa, max(8, min(16, al))); locs.append(("s", nsaa)); nsaa += up(size, 8)
    return locs, nsaa

def ret_place(t):
    if t is None: return None
    h = hfa(t)
    if h: return ("v", 0, h)
    return ("mem",) if size_align(t)[0] > 16 else ("x", 0)

# ---- random aggregates (HFAs on purpose too)
def mkagg():
    r = random.random()
    if r < 0.35:
        b = random.choice(["float", "double"])
        k = random.randint(1, 4)
        mem = [(b, 0)] * k if random.random() < 0.6 else [(b, k)]
        if aggs and random.random() < 0.3:              # an HFA nested in an HFA
            inner = [i for i, a in enumerate(aggs) if hfa(("struct", i)) and leaves(("struct", i))[0] == b and a[0] == "struct"]
            if inner:
                i = random.choice(inner); n = len(leaves(("struct", i)))
                if n < 4: mem = [(("struct", i), 0)] + [(b, 0)] * random.randint(0, 4 - n - 1)
        kind = "union" if random.random() < 0.15 else "struct"
        if kind == "union": mem = [(b, 0), (b, k)]
        aggs.append((kind, mem))
        return
    mem = []
    for _ in range(random.randint(1, 5)):
        if aggs and random.random() < 0.15: mem.append((("struct", random.randrange(len(aggs))), 0))
        else: mem.append((random.choice(MEMBER[:-1] if random.random() < 0.9 else MEMBER), random.choice([0, 0, 0, 2, 3])))
    aggs.append(("union" if random.random() < 0.15 else "struct", mem))

for _ in range(24): mkagg()
aggs = [(k, m) for k, m in aggs]
TYPES = list(SCAL) + [("struct", i) for i in range(len(aggs))]
VARIADIC = ["int", "long", "unsigned long", "double", "void *", "__int128", "float _Complex", "double _Complex"] + [("struct", i) for i in range(len(aggs))]

def cname(t):
    if isinstance(t, str): return t
    return f"{aggs[t[1]][0]} A{t[1]}"

# ---- the C file
L = ["/* generated by abi_a64.py */", "#include <stdarg.h>",
     "int printf(const char *, ...); int memcmp(const void *, const void *, unsigned long);",
     "void *memcpy(void *, const void *, unsigned long); void *memset(void *, int, unsigned long);",
     "struct Regs { unsigned long x[8], x8, pad; unsigned char v[8][16]; unsigned char stack[256];",
     "              unsigned long mem, saved[18], sp_after, sp_before; };",
     "struct Regs abi_in, abi_out, abi_ret;",
     "void abi_rec(void); void abi_call(void *fn);",
     "static int fails;",
     'static void bad(int k, const char *what, int i) { printf("case %d: %s %d\\n", k, what, i); fails++; }']
for i, (kind, mem) in enumerate(aggs):
    L.append(f"{kind} A{i} {{ " + " ".join(f"{cname(m)} m{j}" + (f"[{n}]" if n else "") + ";" for j, (m, n) in enumerate(mem)) + " };")

def value(t, seed):
    """an expression of type t (its bytes depend on seed only)"""
    if not isinstance(t, str) or t in ("__int128", "unsigned __int128", "float _Complex", "double _Complex", "void *"):
        return f"mk_{tag(t)}({seed})"
    if t == "_Bool": return str(seed & 1)
    if SCAL[t][2]: return f"({t})({seed % 997} * 0.25 - 50)"
    return f"({t}){seed * 2654435761 % 4294967291}UL"

def tag(t): return "A%d" % t[1] if not isinstance(t, str) else t.replace(" *", "_ptr").replace(" ", "_")

for t in [t for t in TYPES if not isinstance(t, str)] + ["__int128", "unsigned __int128", "float _Complex", "double _Complex", "void *"]:
    L.append(f"static {cname(t)} mk_{tag(t)}(int seed) {{ {cname(t)} v; unsigned char *p = (unsigned char *)&v;")
    if not isinstance(t, str) and hfa(t) or "Complex" in str(t):     # floats: whole numbers, not NaN patterns
        es = hfa(t)[0]
        L.append(f"    for (unsigned i = 0; i < sizeof v / {es}; i++) {{ {'float' if es == 4 else 'double'} f = seed * 3 + i * 0.5; memcpy(p + i * {es}, &f, {es}); }}")
    else:
        L.append("    for (unsigned i = 0; i < sizeof v; i++) p[i] = (unsigned char)(seed * 7 + i * 13 + 1);")
    L.append("    return v; }")

def where(loc, rec, var, t, k, i, store):
    """C statements: compare var with (or, if store, put it at) location loc of record rec"""
    size = size_align(t)[0]
    kind = loc[0]
    if kind == "x": dst = f"&{rec}.x[{loc[1]}]"
    elif kind == "s": dst = f"{rec}.stack + {loc[1]}"
    elif kind == "v":
        es, n = loc[2]
        if store: return " ".join(f"memcpy({rec}.v[{loc[1] + j}], (char *)&{var} + {j * es}, {es});" for j in range(n))
        return " ".join(f'if (memcmp({rec}.v[{loc[1] + j}], (char *)&{var} + {j * es}, {es})) bad({k}, "{i}", {j});' for j in range(n))
    elif kind == "mem": dst = f"(void *){rec}.x8"
    else:                                                # by reference: the address in x[n] or in a stack slot
        p = f"{rec}.x[{loc[1]}]" if kind == "rx" else f"*(unsigned long *)({rec}.stack + {loc[1]})"
        if store: return f"{{ static {cname(t)} copy; copy = {var}; {p} = (unsigned long)&copy; }}"
        return f'if (memcmp((void *){p}, &{var}, {size})) bad({k}, "{i} (by reference)", 0);'
    if store: return f"memcpy({dst}, &{var}, {size});"
    return f'if (memcmp({dst}, &{var}, {size})) bad({k}, "{i}", 0);'

def is_int(t): return isinstance(t, str) and not SCAL[t][2] and "128" not in t and t != "void *"

calls = []
k = 0
while k < ncase:
    n = random.randint(0, 14)
    params = [random.choice(TYPES) for _ in range(n)]
    fixed = n
    if n and random.random() < 0.25:                     # variadic: the rest of the arguments unnamed
        fixed = random.randint(1, n)
        params = params[:fixed] + [random.choice(VARIADIC) for _ in range(n - fixed)]
    rt = None if random.random() < 0.15 else random.choice(TYPES)
    locs, stack = place(params)
    if stack > 256: continue
    rloc = ret_place(rt)
    proto = ", ".join(cname(t) for t in params[:fixed]) + (", ..." if fixed < n else "") if n else "void"
    rn = cname(rt) if rt else "void"
    args = [value(t, k * 50 + i + 1) for i, t in enumerate(params)]
    rv = value(rt, k * 50 + 49) if rt else None
    # sicc calls abi_rec
    L.append(f"static void caller{k}(void) {{")
    for i, t in enumerate(params): L.append(f"    {cname(t)} a{i} = {args[i]};")
    L.append("    memset(&abi_in, 0xa5, sizeof abi_in); memset(&abi_out, 0xa5, sizeof abi_out); abi_out.mem = 0;")
    if rt:
        L.append(f"    {rn} want = {rv};")
        L.append("    " + (f"abi_out.mem = sizeof want; memcpy(abi_out.stack, &want, sizeof want);" if rloc[0] == "mem" else where(rloc, "abi_out", "want", rt, k, -1, True)))
    call = f"(({rn} (*)({proto}))abi_rec)(" + ", ".join(f"a{i}" for i in range(n)) + ")"
    L.append(f"    {rn + ' got = ' if rt else ''}{call};")
    for i, t in enumerate(params): L.append("    " + where(locs[i], "abi_in", f"a{i}", t, k, i, False))
    if rt:
        L.append(f'    if (memcmp(&got, &want, sizeof got)) bad({k}, "result", 0);')
        if is_int(rt): L.append(f'    if ((long)got != (long)want) bad({k}, "result (extension)", 0);')
    L.append("}")
    # abi_call calls a sicc function
    pnames = [f"p{i}" for i in range(n)]
    L.append(f"static {rn} callee{k}(" + (", ".join(f"{cname(t)} {pnames[i]}" for i, t in enumerate(params[:fixed])) + (", ..." if fixed < n else "") if n else "void") + ") {")
    if fixed < n:
        L.append(f"    va_list ap; va_start(ap, p{fixed - 1});")
        for i in range(fixed, n): L.append(f"    {cname(params[i])} p{i} = va_arg(ap, {cname(params[i])});")
        L.append("    va_end(ap);")
    for i, t in enumerate(params):
        L.append(f"    {{ {cname(t)} e = {args[i]}; if (memcmp(&e, &p{i}, sizeof e)) bad({k}, \"parameter\", {i});" +
                 (f" if ((long)e != (long)p{i}) bad({k}, \"parameter (extension)\", {i});" if is_int(t) else "") + " }")
    if rt: L.append(f"    return {rv};")
    L.append("}")
    L.append(f"static void call{k}(void) {{")
    L.append("    memset(&abi_in, 0xa5, sizeof abi_in);")
    for i, t in enumerate(params): L.append(f"    {{ {cname(t)} a = {args[i]}; " + where(locs[i], "abi_in", "a", t, k, i, True) + " }")
    if rt and rloc[0] == "mem": L.append(f"    static {rn} buf; memset(&buf, 0, sizeof buf); abi_in.x8 = (unsigned long)&buf;")
    L.append(f"    abi_call((void *)callee{k});")
    if rt:
        L.append(f"    {rn} want = {rv};")
        if rloc[0] == "mem": L.append(f'    if (memcmp(&buf, &want, sizeof want)) bad({k}, "result (x8)", 0);')
        else: L.append("    " + where(rloc, "abi_ret", "want", rt, k, -1, False))
    L.append(f'    for (int i = 0; i < 10; i++) if (abi_ret.saved[i] != 0x19190000UL + 0x101 * i) bad({k}, "x19+", i);')
    L.append(f'    for (int i = 0; i < 8; i++) if (abi_ret.saved[10 + i] != 0x19190000UL + 0x101 * i) bad({k}, "d8+", i);')
    L.append(f'    if (abi_ret.sp_after != abi_ret.sp_before) bad({k}, "sp", 0);')
    L.append("}")
    calls.append(k)
    k += 1

L.append("int main(void) {")
for k in calls: L.append(f"    caller{k}(); call{k}();")
L.append(f'    printf("abi: %d cases, %d failures\\n", {ncase}, fails);')
L.append("    return fails != 0;")
L.append("}")
src = os.path.join(out, "abi.c")
open(src, "w").write("\n".join(L) + "\n")

# ---- build at each optimization level, run under the emulator
A = [sicc, "--target=aarch64"]
def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: sys.exit("failed: " + " ".join(cmd) + "\n" + r.stdout + r.stderr)
    return r.stdout
rt_ = os.path.join(out, "rt"); os.makedirs(rt_, exist_ok=True)
run(A + ["-c", "-o", f"{rt_}/crt0.o", f"{here}/crt0.S"])
run(A + ["-c", "-o", f"{rt_}/tramp.o", f"{here}/abi_tramp.S"])
for f in ("libc", "fmt"): run(A + ["-O2", "-c", "-o", f"{rt_}/{f}.o", f"{here}/{f}.c"])
run(A + ["-O2", "-c", "-o", f"{rt_}/rt.o", f"{here}/../../../cc/rt/rt.c"])
ok = True
for o in ("-O0", "-O1", "-O2"):
    obj, exe = os.path.join(out, f"abi{o}.o"), os.path.join(out, f"abi{o}")
    run(A + [o, "-c", "-o", obj, src])
    run(A + ["-nostdlib", "-T", f"{here}/prog.ld", "-o", exe, f"{rt_}/crt0.o", obj, f"{rt_}/tramp.o", f"{rt_}/libc.o", f"{rt_}/fmt.o", f"{rt_}/rt.o"])
    os.chmod(exe, 0o755)
    r = subprocess.run([qemu, exe], capture_output=True, text=True)
    print(f"{o}: " + r.stdout.strip().replace("\n", "\n    "))
    ok &= r.returncode == 0
sys.exit(0 if ok else 1)
