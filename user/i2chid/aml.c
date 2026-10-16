/*
 * aml.c - A small reader of the firmware's ACPI tables: just enough AML to
 * find the I2C HID devices (touchpads, "PNP0C50") and what reaching them
 * takes: address, speed, controller, interrupt pin, HID descriptor register.
 *
 * The DSDT and SSDTs hold byte code ("AML"): a tree of named objects
 * (scopes, devices, names, methods, fields of memory). Laptops rarely
 * write a touchpad's resources as constants: Intel's reference firmware
 * (Lenovo's included) picks the model, its address and the controller in
 * methods (_INI, _CRS, _DSM) from values in memory the firmware filled in.
 * So this is a small interpreter rather than a pattern matcher:
 *   load:  every table is walked once; named objects go in a flat table
 *          (parent, 4-letter name; a hash finds children), statements in
 *          scopes are skipped (parsed without effects: "dry");
 *   run:   for each device whose _HID/_CID says PNP0C50 and whose _STA says
 *          present: its controller's _INI, its own statements and _INI,
 *          then _CRS (I2cSerialBus, GpioInt) and _DSM (descriptor register).
 * What it runs: integers, strings, buffers, packages; names, locals, args;
 * method calls; If/Else/While/Break/Return; arithmetic, logic, Index,
 * DerefOf, SizeOf, buffer fields, ConcatenateResTemplate; fields in memory
 * regions (read through aml_mem32; writes are kept here, never done to
 * memory). Anything else stops that evaluation, with a line in the log.
 * We answer _OSI like Windows, as Linux does: OSYS starts at Windows 2015.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "aml.h"

enum { N_SCOPE, N_DEV, N_METHOD, N_NAME, N_REGION, N_FIELD, N_BFIELD, N_ALIAS, N_EXT, N_OTHER };
enum { V_INT, V_STR, V_BUF, V_PKG, V_NONE };
/* A value. A string or buffer: b, n bytes; a package: its elements' AML
 * from b to e, n of them, names in it seen from node i. */
typedef struct { uint8_t t; uint32_t n; int64_t i; uint8_t *b; const uint8_t *e; } val_t;
typedef struct {
    uint32_t seg; int up, next, val;        /* name, parent, hash chain, its value (vals[], -1: not yet) */
    uint8_t kind, argc, space;              /* a method's arguments; a region's space (0: memory) */
    union { const uint8_t *p; int reg; };   /* its AML (method body, data, definition); a field: its region */
    union { const uint8_t *e; struct { uint32_t off, bits; }; };   /* a method's end; a field's bits */
} node_t;
typedef struct { int scope, ret, brk; val_t a[7], l[8], rv; } frame_t;
typedef struct { int root, ups, n; uint32_t s[8]; } name_t;

static node_t *nd;
static int nn, ncap, hh[4096];
static val_t *vals;
static int nv, vcap;
static int err, dry, depth;
static const uint8_t *base, *cur;           /* the tables (offsets in the log are from here); the term being evaluated */

static void fail(const char *why, const uint8_t *at)
{
    if (!err && !dry) log_line("aml: %s at 0x%lx\n", why, (long)((at ? at : cur) - base));
    err = 1;
}

/* ---- The names. */
static unsigned hash(int up, uint32_t seg) { return (seg * 2654435761u ^ (unsigned)up * 40503u) >> 20; }
static int child(int up, uint32_t seg)
{
    for (int i = hh[hash(up, seg)]; i >= 0; i = nd[i].next) if (nd[i].seg == seg && nd[i].up == up) return i;
    return -1;
}
static int add(int up, uint32_t seg, int kind)
{
    int k = child(up, seg);
    if (k >= 0) {                                       /* known: a placeholder takes its real kind */
        if (kind != N_EXT && (nd[k].kind == N_EXT || nd[k].kind == N_SCOPE)) nd[k].kind = kind;
        return k;
    }
    if (nn == ncap && !(nd = realloc(nd, (ncap = ncap ? 2 * ncap : 8192) * sizeof *nd))) abort();
    unsigned h = hash(up, seg);
    nd[nn] = (node_t){ .seg = seg, .up = up, .next = hh[h], .val = -1, .kind = kind };
    hh[h] = nn;
    return nn++;
}
static void pop(int mark)                           /* a method's own names, at its end */
{
    for (; nn > mark; nn--) hh[hash(nd[nn - 1].up, nd[nn - 1].seg)] = nd[nn - 1].next;
}
static int isname(uint8_t c) { return c == '\\' || c == '^' || c == '_' || (c >= 'A' && c <= 'Z') || c == 0x2E || c == 0x2F; }
static const uint8_t *getname(const uint8_t *p, name_t *m)
{
    int c = 1;
    m->root = *p == '\\';
    p += m->root;
    for (m->ups = 0; *p == '^'; p++) m->ups++;
    if (*p == 0x2E) c = 2, p++;
    else if (*p == 0x2F) c = p[1], p += 2;
    else if (!*p) c = 0, p++;
    m->n = c > 8 ? 8 : c;
    for (int k = 0; k < c; k++, p += 4) if (k < 8) memcpy(&m->s[k], p, 4);
    return p;
}
static int start(int s, const name_t *m) { if (m->root) s = 0; for (int k = 0; k < m->ups; k++) s = nd[s].up; return s; }
static int find(int s, const name_t *m)
{
    s = start(s, m);
    if (m->n == 1 && !m->root && !m->ups)               /* one segment: here, then each scope up */
        for (;; s = nd[s].up) { int c = child(s, m->s[0]); if (c >= 0 || !s) { s = c; break; } }
    else for (int k = 0; k < m->n && s >= 0; k++) s = child(s, m->s[k]);
    if (s >= 0 && nd[s].kind == N_ALIAS && depth < 8) { name_t a; getname(nd[s].p, &a); depth++; s = find(nd[s].up, &a); depth--; }
    return s;
}
static int declare(int s, const name_t *m, int kind)
{
    s = start(s, m);
    if (!m->n) return s;
    for (int k = 0; k < m->n - 1; k++) { int c = child(s, m->s[k]); s = c >= 0 ? c : add(s, m->s[k], N_SCOPE); }
    return add(s, m->s[m->n - 1], kind);
}
static void setval(int k, const val_t *v)
{
    if (nd[k].val < 0) {
        if (nv == vcap && !(vals = realloc(vals, (vcap = vcap ? 2 * vcap : 256) * sizeof *vals))) abort();
        nd[k].val = nv++;
    }
    vals[nd[k].val] = *v;
}

/* PkgLength: the bytes from here to the end of the object (or a field's bits). */
static const uint8_t *plen(const uint8_t *p, uint32_t *n)
{
    int k = *p >> 6;
    uint32_t v = k ? *p & 15 : *p & 63;
    for (int i = 1; i <= k; i++) v |= (uint32_t)p[i] << (8 * i - 4);
    *n = v;
    return p + 1 + k;
}
static const uint8_t *pkg(const uint8_t *p, const uint8_t **e) { uint32_t n; const uint8_t *q = plen(p, &n); *e = p + n; return q; }

/* ---- Values. */
static int64_t num(const val_t *v)
{
    int64_t x = 0;
    if (v->t == V_INT) return v->i;
    if (v->t == V_BUF) for (uint32_t k = 0; k < v->n && k < 8; k++) x |= (int64_t)v->b[k] << 8 * k;
    if (v->t == V_STR) for (const uint8_t *s = v->b + (v->n > 2 && v->b[1] == 'x' ? 2 : 0); *s; s++) {
        int d = *s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10;
        if (d < 0 || d > 15) break;
        x = x << 4 | d;
    }
    return x;
}
static void mkbuf(val_t *o, uint32_t n, const void *a, uint32_t an, const void *b, uint32_t bn)
{
    *o = (val_t){ .t = V_BUF, .n = n };
    if (dry) return;
    if (!(o->b = malloc(n + 1))) abort();               /* (never freed: a few evaluations at start) */
    memset(o->b, 0, n + 1);
    memcpy(o->b, a, an < n ? an : n);
    if (bn && an < n) memcpy(o->b + an, b, bn < n - an ? bn : n - an);
}
static int cmp(const val_t *a, const val_t *b)
{
    if (a->t == V_INT || (b->t != V_STR && b->t != V_BUF)) { uint64_t x = num(a), y = num(b); return x < y ? -1 : x > y; }
    uint32_t n = a->n < b->n ? a->n : b->n;
    int c = memcmp(a->b, b->b, n);
    return c ? c : a->n < b->n ? -1 : a->n > b->n;
}
static uint64_t getbits(const uint8_t *b, int64_t off, int bits)
{
    uint64_t v = 0;
    for (int i = 0; i < bits; i++) v |= (uint64_t)(b[(off + i) / 8] >> (off + i) % 8 & 1) << i;
    return v;
}
static void putbits(uint8_t *b, int64_t off, int bits, uint64_t v)
{
    for (int i = 0; i < bits; i++, v >>= 1) b[(off + i) / 8] = (uint8_t)((b[(off + i) / 8] & ~(1 << (off + i) % 8)) | (v & 1) << (off + i) % 8);
}

static const uint8_t *ev(const uint8_t *p, frame_t *f, val_t *o);
static void exec(const uint8_t *p, const uint8_t *e, frame_t *f);
static void fields(const uint8_t *p, const uint8_t *e, int s, int reg);

/* A method call (one of ours: _OSI, Windows' strings say yes). */
static void call(int m, val_t *a, val_t *o)
{
    if (!nd[m].p) { *o = (val_t){ .t = V_INT, .i = a[0].t == V_STR && !memcmp(a[0].b, "Windows", 7) ? -1 : 0 }; return; }
    if (depth > 24) { fail("calls too deep", nd[m].p); return; }
    frame_t f = { .scope = m };
    for (int k = 0; k < nd[m].argc; k++) f.a[k] = a[k];
    int mark = nn;
    depth++;
    exec(nd[m].p, nd[m].e, &f);
    depth--;
    pop(mark);
    *o = f.ret ? f.rv : (val_t){ .t = V_INT };
}

/* A buffer field (CreateDWordField...): find its buffer once. 0: ready. */
static int bfield(int k, frame_t *f)
{
    if (nd[k].val >= 0) return 0;
    const uint8_t *p = nd[k].p;
    int op = *p++;
    if (op == 0x5B) op = 0x100 | *p++;
    frame_t lf = { .scope = nd[k].up };
    val_t src, ix, nb;
    if (!f) f = &lf;
    p = ev(ev(p, f, &src), f, &ix);
    int bits = op == 0x8A ? 32 : op == 0x8B ? 16 : op == 0x8C ? 8 : op == 0x8D ? 1 : 64;
    int64_t off = num(&ix) * (op == 0x8D || op == 0x113 ? 1 : 8);
    if (op == 0x113) { ev(p, f, &nb); bits = (int)num(&nb); }
    if (err || src.t != V_BUF || bits < 1 || bits > 64 || off < 0 || off + bits > 8 * (int64_t)src.n) { fail("buffer field", nd[k].p); return -1; }
    setval(k, &(val_t){ .t = V_BUF, .n = src.n, .i = off << 8 | bits, .b = src.b });
    return 0;
}

/* A field of a memory region, up to 64 bits (as dwords: device registers want them). */
static uint64_t memfield(uint64_t a, int64_t off, int bits)
{
    if (bits > 32) return memfield(a, off, 32) | memfield(a, off + 32, bits - 32) << 32;
    a += off / 8;
    int sh = (int)(a & 3) * 8 + (int)(off % 8);
    uint64_t v = aml_mem32(a & ~3ULL);
    if (sh + bits > 32) v |= (uint64_t)aml_mem32((a & ~3ULL) + 4) << 32;
    return v >> sh & ((1ULL << bits) - 1);
}

/* A named object's value (a method: called without arguments). */
static void get(int k, val_t *o)
{
    *o = (val_t){ .t = V_NONE };
    if (k < 0) { fail("unknown name", 0); return; }
    node_t *n = &nd[k];
    if (n->val >= 0 && n->kind != N_BFIELD && n->kind != N_REGION) { *o = vals[n->val]; return; }
    frame_t f = { .scope = n->up };
    val_t v;
    switch (n->kind) {
    case N_NAME: ev(n->p, &f, &v); if (!err) setval(k, &v), *o = v; return;
    case N_METHOD: call(k, f.a, o); return;
    case N_BFIELD:
        if (!bfield(k, 0)) *o = (val_t){ .t = V_INT, .i = (int64_t)getbits(vals[nd[k].val].b, vals[nd[k].val].i >> 8, vals[nd[k].val].i & 255) };
        return;
    case N_FIELD:
        if (n->reg < 0 || nd[n->reg].space || n->bits > 64) { fail("field not in memory", 0); return; }
        if (nd[n->reg].val < 0) { f.scope = nd[n->reg].up; ev(nd[n->reg].p, &f, &v); if (err) return; setval(n->reg, &v); }
        *o = (val_t){ .t = V_INT, .i = (int64_t)memfield((uint64_t)num(&vals[nd[nd[k].reg].val]), nd[k].off, (int)nd[k].bits) };
        return;
    case N_EXT: fail("external name not defined", 0); return;
    }
}
static void put(int k, const val_t *v)
{
    if (k < 0) { fail("unknown name", 0); return; }
    if (nd[k].kind == N_NAME) setval(k, v);
    else if (nd[k].kind == N_FIELD) setval(k, &(val_t){ .t = V_INT, .i = num(v) });   /* ours only: memory is not written */
    else if (nd[k].kind == N_BFIELD && !bfield(k, 0)) putbits(vals[nd[k].val].b, vals[nd[k].val].i >> 8, vals[nd[k].val].i & 255, (uint64_t)num(v));
}

/* A package's element i (names in packages are references, not calls). */
static const uint8_t *element(const uint8_t *p, int scope, val_t *o)
{
    name_t m;
    if (!isname(*p)) { frame_t f = { .scope = scope }; return ev(p, &f, o); }
    p = getname(p, &m);
    if (!dry) get(find(scope, &m), o);
    return p;
}
static void index_of(const val_t *a, int64_t i, val_t *o)
{
    *o = (val_t){ .t = V_INT };
    if (a->t == V_BUF || a->t == V_STR) { if (i >= 0 && i < a->n) o->i = a->b[i]; else fail("index", 0); return; }
    if (a->t != V_PKG || i < 0 || i >= a->n) { fail("index", 0); return; }
    const uint8_t *p = a->b;
    int was = dry;
    dry = 1;
    for (int64_t k = 0; k < i && p < a->e; k++) p = element(p, (int)a->i, o);
    dry = was;
    if (p < a->e) element(p, (int)a->i, o); else *o = (val_t){ .t = V_INT };
}

/* Where a result goes (a "target"): nowhere, a local, an argument, a name, a byte of a buffer. */
static const uint8_t *store(const uint8_t *p, frame_t *f, const val_t *v)
{
    uint8_t op = *p;
    name_t m;
    if (!op) return p + 1;
    if (op >= 0x60 && op <= 0x67) { if (!dry) f->l[op - 0x60] = *v; return p + 1; }
    if (op >= 0x68 && op <= 0x6E) { if (!dry) f->a[op - 0x68] = *v; return p + 1; }
    if (op == 0x5B && p[1] == 0x31) return p + 2;                       /* Debug */
    if (op == 0x88) {                                                   /* Index (buffer, i) */
        val_t b, i, z = { .t = V_NONE };
        p = store(ev(ev(p + 1, f, &b), f, &i), f, &z);
        if (!dry && b.t == V_BUF && num(&i) >= 0 && num(&i) < b.n) b.b[num(&i)] = (uint8_t)num(v);
        else if (!dry) fail("store to Index", p);
        return p;
    }
    if (isname(op)) { p = getname(p, &m); if (!dry) put(find(f->scope, &m), v); return p; }
    fail("unsupported target", p);
    return p + 1;
}

/* Evaluate the term at p (a "TermArg") into *o -> after it. */
static const uint8_t *ev(const uint8_t *p, frame_t *f, val_t *o)
{
    const uint8_t *at = p, *e;
    uint8_t op = *p++;
    val_t a, b;
    name_t m;
    int64_t x, y;
    *o = (val_t){ .t = V_INT };
    if (err) return p;
    cur = at;
    switch (op) {
    case 0x00: case 0xA3: return p;                                     /* Zero; Noop */
    case 0x01: o->i = 1; return p;
    case 0xFF: o->i = -1; return p;                                     /* Ones */
    case 0x0A: o->i = p[0]; return p + 1;
    case 0x0B: o->i = p[0] | p[1] << 8; return p + 2;
    case 0x0C: o->i = (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); return p + 4;
    case 0x0E: for (int k = 7; k >= 0; k--) o->i = (int64_t)((uint64_t)o->i << 8 | p[k]); return p + 8;
    case 0x0D: *o = (val_t){ .t = V_STR, .n = (uint32_t)strlen((const char *)p), .b = (uint8_t *)p }; return p + o->n + 1;
    case 0x11:                                                          /* Buffer (size) { bytes } */
        p = ev(pkg(p, &e), f, &a);
        x = num(&a);
        mkbuf(o, (uint32_t)(x > e - p ? x : e - p), p, (uint32_t)(e - p), 0, 0);
        return e;
    case 0x12: case 0x13:                                               /* Package, VarPackage */
        p = pkg(p, &e);
        if (op == 0x12) x = *p++; else p = ev(p, f, &a), x = num(&a);
        *o = (val_t){ .t = V_PKG, .n = (uint32_t)x, .i = f->scope, .b = (uint8_t *)p, .e = e };
        return e;
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: *o = f->l[op - 0x60]; return p;
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: *o = f->a[op - 0x68]; return p;
    case 0x70: p = ev(p, f, o); return store(p, f, o);                  /* Store (value, target) */
    case 0x72: case 0x74: case 0x77: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: case 0x85:
        p = ev(ev(p, f, &a), f, &b);
        x = num(&a); y = num(&b);
        o->i = op == 0x72 ? x + y : op == 0x74 ? x - y : op == 0x77 ? x * y : op == 0x79 ? (y < 64 ? x << y : 0) :
               op == 0x7A ? (y < 64 ? (int64_t)((uint64_t)x >> y) : 0) : op == 0x7B ? x & y : op == 0x7C ? ~(x & y) :
               op == 0x7D ? x | y : op == 0x7E ? ~(x | y) : op == 0x7F ? x ^ y : y ? (int64_t)((uint64_t)x % (uint64_t)y) : 0;
        return store(p, f, o);
    case 0x75: case 0x76:                                               /* Increment, Decrement */
        e = ev(p, f, &a);
        *o = (val_t){ .t = V_INT, .i = num(&a) + (op == 0x75 ? 1 : -1) };
        store(p, f, o);
        return e;
    case 0x80: p = ev(p, f, &a); o->i = ~num(&a); return store(p, f, o);   /* Not */
    case 0x83: return ev(p, f, o);                                      /* DerefOf: Index gives values already */
    case 0x87: p = ev(p, f, &a); o->i = a.t == V_INT ? 8 : a.n; return p;  /* SizeOf */
    case 0x88:                                                          /* Index (source, i, target) */
        p = ev(ev(p, f, &a), f, &b);
        if (!dry) index_of(&a, num(&b), o);
        return store(p, f, o);
    case 0x90: p = ev(ev(p, f, &a), f, &b); o->i = num(&a) && num(&b) ? -1 : 0; return p;   /* LAnd */
    case 0x91: p = ev(ev(p, f, &a), f, &b); o->i = num(&a) || num(&b) ? -1 : 0; return p;   /* LOr */
    case 0x92: p = ev(p, f, &a); o->i = num(&a) ? 0 : -1; return p;    /* LNot (also LNotEqual...) */
    case 0x93: case 0x94: case 0x95:                                    /* LEqual, LGreater, LLess */
        p = ev(ev(p, f, &a), f, &b);
        x = dry ? 0 : cmp(&a, &b);
        o->i = (op == 0x93 ? !x : op == 0x94 ? x > 0 : x < 0) ? -1 : 0;
        return p;
    case 0x99: p = ev(p, f, &a); o->i = num(&a); return store(p, f, o);    /* ToInteger */
    case 0x84:                                                          /* ConcatenateResTemplate */
        p = ev(ev(p, f, &a), f, &b);
        if (!dry && (a.t != V_BUF || b.t != V_BUF || a.n < 2)) { fail("ConcatenateResTemplate", at); return p; }
        mkbuf(o, a.n - 2 + b.n, a.b, a.n - 2, b.b, b.n);         /* (without the first one's end tag) */
        return store(p, f, o);
    case 0x5B:
        switch (*p++) {
        case 0x12:                                                      /* CondRefOf (name, target) */
            if (isname(*p)) { p = getname(p, &m); o->i = !dry && find(f->scope, &m) >= 0 && nd[find(f->scope, &m)].kind != N_EXT ? -1 : 0; }
            else p = ev(p, f, &a), o->i = -1;
            return store(p, f, &(val_t){ .t = V_NONE });
        }
        break;
    default:
        if (!isname(op)) break;
        p = getname(at, &m);                                            /* a name, or a method call */
        int k = find(f->scope, &m), n = k >= 0 && (nd[k].kind == N_METHOD || nd[k].kind == N_EXT) ? nd[k].argc : 0;
        val_t arg[7];
        for (int i = 0; i < n; i++) p = ev(p, f, &arg[i]);
        if (dry) return p;
        if (k >= 0 && nd[k].kind == N_METHOD) call(k, arg, o); else get(k, o);
        return p;
    }
    fail("unsupported opcode", at);
    return p;
}

/* A method's statements. */
static void exec(const uint8_t *p, const uint8_t *end, frame_t *f)
{
    while (p < end && !err && !f->ret && !f->brk) {
        const uint8_t *e, *q;
        val_t v, w;
        name_t m;
        int k;
        switch (*p) {
        case 0xA0:                                                      /* If (...) { } Else { } */
            q = ev(pkg(p + 1, &e), f, &v);
            k = num(&v) != 0;
            if (k && !err) exec(q, e, f);
            if (e < end && *e == 0xA1) { const uint8_t *e2; q = pkg(e + 1, &e2); if (!k && !err && !f->ret && !f->brk) exec(q, e2, f); e = e2; }
            p = e;
            continue;
        case 0xA1: pkg(p + 1, &p); continue;
        case 0xA2:                                                      /* While */
            q = pkg(p + 1, &e);
            for (int n = 0; ; n++) {
                const uint8_t *body = ev(q, f, &v);
                if (err || !num(&v) || f->ret) break;
                if (n > 100000) { fail("endless loop", p); break; }
                exec(body, e, f);
                if (f->brk) { f->brk = 0; break; }
            }
            p = e;
            continue;
        case 0xA4: p = ev(p + 1, f, &f->rv); f->ret = 1; continue;      /* Return */
        case 0xA5: f->brk = 1; return;                                  /* Break */
        case 0xA3: case 0xCC: p++; continue;                            /* Noop, BreakPoint */
        case 0x08:                                                      /* Name (a method's own) */
            q = getname(p + 1, &m);
            k = declare(f->scope, &m, N_NAME);
            p = ev(q, f, &v);
            setval(k, &v);
            continue;
        case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F:          /* CreateDWordField... */
            q = ev(ev(p + 1, f, &v), f, &w);
            q = getname(q, &m);
            k = declare(f->scope, &m, N_BFIELD);
            nd[k].p = p; nd[k].val = -1;
            bfield(k, f);
            p = q;
            continue;
        case 0x5B:
            if (p[1] == 0x80) {                                         /* OperationRegion (its address now) */
                q = getname(p + 2, &m);
                k = declare(f->scope, &m, N_REGION);
                nd[k].space = *q;
                p = ev(ev(q + 1, f, &v), f, &w);
                setval(k, &v);
                continue;
            }
            if (p[1] == 0x81) {                                         /* Field */
                q = getname(pkg(p + 2, &e), &m);
                fields(q + 1, e, f->scope, find(f->scope, &m));
                p = e;
                continue;
            }
            if (p[1] == 0x13) {                                         /* CreateField */
                q = getname(ev(ev(ev(p + 2, f, &v), f, &w), f, &w), &m);
                k = declare(f->scope, &m, N_BFIELD);
                nd[k].p = p; nd[k].val = -1;
                bfield(k, f);
                p = q;
                continue;
            }
            if (p[1] == 0x01) { p = getname(p + 2, &m) + 1; declare(f->scope, &m, N_OTHER); continue; }   /* Mutex */
            /* fall through */
        default: p = ev(p, f, &v);
        }
    }
}

/* A Field's list: named runs of bits of a region, one after the other. */
static void fields(const uint8_t *p, const uint8_t *e, int s, int reg)
{
    uint32_t off = 0, n;
    while (p < e) {
        if (!*p) { p = plen(p + 1, &n); off += n; }                     /* reserved bits */
        else if (*p == 1) p += 3;                                       /* AccessField */
        else if (*p == 3) p += 4;                                       /* ExtendedAccessField */
        else if (*p == 2) { name_t m; const uint8_t *q; p = p[1] == 0x11 ? (pkg(p + 2, &q), q) : getname(p + 1, &m); }   /* Connection */
        else {
            uint32_t seg;
            memcpy(&seg, p, 4);
            p = plen(p + 4, &n);
            int k = add(s, seg, reg >= 0 && nd[reg].kind == N_REGION ? N_FIELD : N_OTHER);
            nd[k].reg = reg; nd[k].off = off; nd[k].bits = n;
            off += n;
        }
    }
}

/* A block of AML (a table, scope, device...): declare what it defines;
 * with run, carry out its statements instead (a device's, before its _INI). */
static void load(const uint8_t *p, const uint8_t *end, int s, int run)
{
    frame_t f = { .scope = s };
    while (p < end) {
        const uint8_t *q, *e = end, *e2;
        val_t v;
        name_t m;
        int k, op = *p == 0x5B ? 0x100 | p[1] : *p;
        dry = !run;
        switch (op) {
        case 0x10: case 0x182: case 0x185:                              /* Scope, Device, ThermalZone */
            q = getname(pkg(p + 1 + (op > 0xFF), &e), &m);
            if (run || e > end) break;
            k = declare(s, &m, op == 0x10 ? N_SCOPE : N_DEV);
            if (op != 0x10) nd[k].p = q, nd[k].e = e;
            load(q, e, k, 0);
            break;
        case 0x183: case 0x184:                                         /* Processor, PowerResource */
            q = getname(pkg(p + 2, &e), &m);
            if (!run && e <= end) load(q + (op == 0x183 ? 6 : 3), e, declare(s, &m, N_DEV), 0);
            break;
        case 0x14:                                                      /* Method */
            q = getname(pkg(p + 1, &e), &m);
            if (run) break;
            k = declare(s, &m, N_METHOD);
            nd[k].kind = N_METHOD; nd[k].argc = *q & 7; nd[k].p = q + 1; nd[k].e = e;
            break;
        case 0x08:                                                      /* Name */
            q = getname(p + 1, &m);
            if (!run) k = declare(s, &m, N_NAME), nd[k].p = q;
            dry = 1;
            e = ev(q, &f, &v);
            break;
        case 0x15:                                                      /* External (name, type, arguments) */
            q = getname(p + 1, &m);
            if (!run && (k = declare(s, &m, N_EXT), nd[k].kind == N_EXT)) nd[k].argc = q[1] & 7;
            e = q + 2;
            break;
        case 0x180:                                                     /* OperationRegion (name, space, address, length) */
            q = getname(p + 2, &m);
            if (!run) { k = declare(s, &m, N_REGION); nd[k].space = *q; nd[k].p = q + 1; }
            dry = 1;
            e = ev(ev(q + 1, &f, &v), &f, &v);
            break;
        case 0x181: case 0x186: case 0x187:                             /* Field, IndexField, BankField */
            q = getname(pkg(p + 2, &e), &m);
            k = find(s, &m);
            if (op != 0x181) q = getname(q, &m), k = -1;
            if (op == 0x187) dry = 1, q = ev(q, &f, &v);
            if (!run && e <= end) fields(q + 1, e, s, k);
            break;
        case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F: case 0x113:   /* CreateDWordField...: made when used */
            dry = 1;
            q = ev(ev(p + 1 + (op > 0xFF), &f, &v), &f, &v);
            if (op == 0x113) q = ev(q, &f, &v);
            e = getname(q, &m);
            if (!run) k = declare(s, &m, N_BFIELD), nd[k].p = p;
            break;
        case 0x06:                                                      /* Alias (source, name) */
            e = getname(getname(p + 1, &m), &m);
            if (!run) k = declare(s, &m, N_ALIAS), nd[k].p = p + 1;
            break;
        case 0x101: case 0x102:                                         /* Mutex, Event */
            e = getname(p + 2, &m) + (op == 0x101);
            if (!run) declare(s, &m, N_OTHER);
            break;
        case 0xA0: case 0xA2:                                           /* If, While: what they hold is declared either way */
            q = ev(pkg(p + 1, &e), &f, &v);
            if (e > end || err) break;
            if (!run) { load(q, e, s, 0); break; }
            k = num(&v) != 0;
            if (k) load(q, e, s, 1);
            if (e < end && *e == 0xA1) { q = pkg(e + 1, &e2); if (!k) load(q, e2, s, 1); e = e2; }
            break;
        case 0xA1: q = pkg(p + 1, &e); if (!run && e <= end) load(q, e, s, 0); break;
        default: e = ev(p, &f, &v);                                     /* a statement */
        }
        dry = 0;
        if (err || e <= p || e > end) {                                 /* lost: the rest of this block is skipped */
            if (!run) log_line("aml: cannot read the AML at 0x%lx (opcode %x)\n", (long)(p - base), op);
            err = 0;
            return;
        }
        p = e;
    }
}

long aml_load(const uint8_t *t, long n)
{
    static const char pre[][5] = { "_SB_", "_GPE", "_PR_", "_TZ_", "_SI_", "_OSI" };
    base = t;
    memset(hh, -1, sizeof hh);
    nn = nv = 0;
    add(0, 0, N_SCOPE);                                                 /* the root, \ */
    for (unsigned i = 0; i < sizeof pre / sizeof *pre; i++) {
        uint32_t s;
        memcpy(&s, pre[i], 4);
        int k = add(0, s, i == 5 ? N_METHOD : N_SCOPE);
        nd[k].argc = i == 5;                                            /* _OSI (string): ours */
    }
    for (long o = 0; o + 36 <= n; ) {
        uint32_t len = t[o + 4] | t[o + 5] << 8 | t[o + 6] << 16 | (uint32_t)t[o + 7] << 24;
        if (len < 36 || o + len > n) break;
        load(t + o + 36, t + o + len, 0, 0);
        o += len;
    }
    name_t m = { .n = 1 };
    memcpy(m.s, "OSYS", 4);
    int k = find(0, &m);                                                /* we are Windows 2015 (_OSI) */
    if (k >= 0 && nd[k].kind == N_FIELD) setval(k, &(val_t){ .t = V_INT, .i = 0x07DF });
    return nn;
}

/* ---- The I2C HID devices. */
static void path(int k, char *s, int n)
{
    int seg[16], d = 0, o = 0;
    for (; k > 0 && d < 16; k = nd[k].up) seg[d++] = k;
    s[o++] = '\\';
    while (d-- > 0 && o + 6 < n) { memcpy(s + o, &nd[seg[d]].seg, 4); o += 4; if (d) s[o++] = '.'; }
    s[o] = 0;
}
static int obj(int dev, const char *seg, val_t *a, val_t *o)    /* a device's object -> 0, or -1 */
{
    uint32_t sg;
    memcpy(&sg, seg, 4);
    int k = child(dev, sg);
    *o = (val_t){ .t = V_NONE };
    if (k < 0) return -1;
    err = 0;
    if (nd[k].kind == N_METHOD) call(k, a, o); else get(k, o);
    if (err) { char p[48]; path(dev, p, sizeof p); log_line("aml: (in %s.%s)\n", p, seg); err = 0; o->t = V_NONE; }
    return o->t == V_NONE ? -1 : 0;
}
static void id(const val_t *v, char *s)                                 /* a string, or a compressed EISA id */
{
    if (v->t == V_STR) { strlcpy(s, (const char *)v->b, 12); return; }
    uint32_t x = (uint32_t)num(v), e = (x & 0xFF) << 8 | (x >> 8 & 0xFF), pr = (x >> 16 & 0xFF) << 8 | x >> 24;
    static const char hex[] = "0123456789ABCDEF";
    s[0] = (char)('@' + (e >> 10 & 31)); s[1] = (char)('@' + (e >> 5 & 31)); s[2] = (char)('@' + (e & 31));
    for (int k = 0; k < 4; k++) s[3 + k] = hex[pr >> (12 - 4 * k) & 15];
    s[7] = 0;
}
static int hid_id(const val_t *v)
{
    char s[12];
    if (v->t == V_PKG) { for (uint32_t i = 0; i < v->n; i++) { val_t e; index_of(v, i, &e); if (hid_id(&e)) return 1; } return 0; }
    if (v->t != V_STR && v->t != V_INT) return 0;
    id(v, s);
    return !strcmp(s, "PNP0C50") || !strcmp(s, "ACPI0C50");
}
static void cstr(char *d, uint32_t dn, const uint8_t *s, uint32_t sn)   /* a string from a descriptor */
{
    uint32_t k = 0;
    for (; k + 1 < dn && k < sn && s[k]; k++) d[k] = (char)s[k];
    d[k] = 0;
}
static void resources(aml_hid_t *d, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0, len, hdr; i < n; i += hdr + len) {
        const uint8_t *r = b + i;
        uint8_t t = r[0];
        if (t & 0x80) { if (i + 3 > n) break; len = r[1] | r[2] << 8; hdr = 3; }
        else { len = t & 7; hdr = 1; t &= 0x78; if (t == 0x78) break; }
        if (i + hdr + len > n) break;
        if (t == 0x8E && r[5] == 1 && d->addr < 0) {                    /* I2cSerialBus */
            uint32_t tl = r[10] | r[11] << 8;
            d->speed = r[12] | r[13] << 8 | r[14] << 16 | (uint32_t)r[15] << 24;
            d->addr = r[16] | r[17] << 8;
            if (12 + tl < hdr + len) cstr(d->bus, sizeof d->bus, r + 12 + tl, hdr + len - 12 - tl);
        }
        if (t == 0x8C && !r[4] && d->pin < 0) {                         /* GpioInt */
            uint32_t pt = r[14] | r[15] << 8, sn = r[17] | r[18] << 8;
            d->pin_edge = r[7] & 1; d->pin_low = (r[7] >> 1 & 3) == 1;
            if (pt + 1 < hdr + len) d->pin = r[pt] | r[pt + 1] << 8;
            if (sn < hdr + len) cstr(d->gpio, sizeof d->gpio, r + sn, hdr + len - sn);
        }
        if (t == 0x89 && d->irq < 0 && r[4]) d->irq = r[5] | r[6] << 8 | r[7] << 16;   /* Interrupt */
    }
}

int aml_hid(aml_hid_t *d, int max)
{
    static const uint8_t dsm[16] = { 0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45, 0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE };
    int found = 0, n0 = nn;
    val_t v, a[4];
    for (int k = 1; k < n0 && found < max; k++) {
        if (nd[k].kind != N_DEV || !((!obj(k, "_CID", a, &v) && hid_id(&v)) || (!obj(k, "_HID", a, &v) && hid_id(&v)))) continue;
        aml_hid_t *h = &d[found];
        memset(h, 0, sizeof *h);
        h->addr = h->pin = h->irq = h->desc = -1;
        path(k, h->path, sizeof h->path);
        obj(nd[k].up, "_INI", a, &v);                                /* its controller's _INI, its statements, */
        load(nd[k].p, nd[k].e, k, 1);
        err = 0;
        if (!obj(k, "_STA", a, &v) && !(num(&v) & 1)) { log_line("aml: %s: not present (_STA %lx)\n", h->path, (long)num(&v)); continue; }
        obj(k, "_INI", a, &v);                                       /* its own _INI */
        if (!obj(k, "_HID", a, &v)) id(&v, h->hid);
        if (!obj(k, "_CRS", a, &v) && v.t == V_BUF) resources(h, v.b, v.n);
        a[0] = (val_t){ .t = V_BUF, .n = 16, .b = (uint8_t *)dsm };                 /* _DSM (HID over I2C, revision 1, function 1) */
        a[1] = a[2] = (val_t){ .t = V_INT, .i = 1 };
        a[3] = (val_t){ .t = V_PKG };
        if (!obj(k, "_DSM", a, &v) && v.t != V_NONE) h->desc = (int)num(&v);
        log_line("aml: %s %s: I2C address 0x%x at %u Hz on %s, GpioInt %d on %s, IRQ %d, HID descriptor at %d\n",
                 h->path, h->hid, h->addr, h->speed, h->bus[0] ? h->bus : "?", h->pin, h->gpio[0] ? h->gpio : "?", h->irq, h->desc);
        found++;
    }
    return found;
}

static int eval(const char *s, int argc, const int64_t *arg, val_t *o)   /* "\_SB.GINF" (args...) */
{
    name_t m = { .root = *s == '\\' };
    val_t a[7];
    for (s += m.root; *s && m.n < 8; m.n++) {
        char seg[4] = { '_', '_', '_', '_' };
        for (int i = 0; *s && *s != '.'; s++) if (i < 4) seg[i++] = *s;
        memcpy(&m.s[m.n], seg, 4);
        if (*s == '.') s++;
    }
    int k = find(0, &m);
    if (k < 0) return -1;
    for (int i = 0; i < argc && i < 7; i++) a[i] = (val_t){ .t = V_INT, .i = arg[i] };
    err = 0;
    if (nd[k].kind == N_METHOD) call(k, a, o); else get(k, o);
    if (err || o->t == V_NONE) { err = 0; return -1; }
    return 0;
}
int aml_int(const char *s, int argc, const int64_t *arg, int64_t *v)
{
    val_t o;
    if (eval(s, argc, arg, &o)) return -1;
    *v = num(&o);
    return 0;
}
int aml_res(const char *s, aml_hid_t *d)
{
    val_t o;
    memset(d, 0, sizeof *d);
    d->addr = d->pin = d->irq = d->desc = -1;
    if (eval(s, 0, 0, &o) || o.t != V_BUF) return -1;
    resources(d, o.b, o.n);
    return 0;
}
