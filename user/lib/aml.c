/*
 * aml.c - A small interpreter of the firmware's ACPI tables, for the drivers
 * that need them: the touchpad (user/i2chid: address, speed, controller,
 * interrupt pin, HID descriptor register) and the ACPI server (user/acpid:
 * sleep types, _PTS/_WAK, the embedded controller's queries, battery, AC,
 * lid, buttons).
 *
 * The DSDT and SSDTs hold byte code ("AML"): a tree of named objects
 * (scopes, devices, names, methods, fields of regions). Firmware rarely
 * writes what we need as constants: Intel's reference code (Lenovo's
 * included) computes it in methods from values in memory, and batteries
 * are read through the embedded controller. So this is an interpreter:
 *   load:  every table is walked once; named objects go in a flat table
 *          (parent, 4-letter name; a hash finds children), statements in
 *          scopes are skipped (parsed without effects: "dry");
 *   run:   a method or object on demand (aml_eval), the I2C HID devices
 *          (aml_hid), the devices of a kind (aml_devs).
 * What it runs: integers, strings, buffers, packages (stored as arrays:
 * their elements can be changed); names, locals, args; method calls;
 * If/Else/While/Break/Return; arithmetic, logic, Index, DerefOf, SizeOf,
 * Divide, Concatenate, ToString, ToBuffer, Mid, buffer fields,
 * ConcatenateResTemplate, Notify (to aml_notify), Acquire/Release, Sleep,
 * events; fields of regions in memory, I/O ports and the embedded
 * controller (read and written through aml_io, in the access widths the
 * fields ask for). Anything else stops that evaluation, with a line in
 * the log. We answer _OSI like Windows, as Linux does: OSYS starts at
 * Windows 2015.
 * Memory: what an evaluation makes lives in an arena, emptied when the next
 * one starts; values stored in the tables' own names are copied out of it.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "aml.h"

enum { N_SCOPE, N_DEV, N_METHOD, N_NAME, N_REGION, N_FIELD, N_BFIELD, N_ALIAS, N_EXT, N_OTHER };
enum { V_INT, V_STR, V_BUF, V_PKG, V_NONE };
/* A value. A string or buffer: b, n bytes (and a 0 after them); a package:
 * n elements (val_t) at b; a buffer field: its buffer, i = offset << 8 | bits. */
typedef struct { uint8_t t; uint32_t n; int64_t i; uint8_t *b; } val_t;
typedef struct {
    uint32_t seg; int up, next, val;        /* name, parent, hash chain, its value (vals[], -1: not yet) */
    uint8_t kind, argc, space;              /* a method's arguments, a field's access type; a region's space */
    union { const uint8_t *p; int reg; };   /* its AML (method body, data, definition); a field: its region */
    union { const uint8_t *e; struct { uint32_t off, bits; }; };   /* a method's end; a field's bits */
} node_t;
typedef struct { int scope, ret, brk; val_t a[7], l[8], rv; } frame_t;
typedef struct { int root, ups, n; uint32_t s[8]; } name_t;

void (*aml_notify)(int dev, int64_t value);
static node_t *nd;
static int nn, ncap, hh[4096], nn0 = 1 << 30;   /* nodes below nn0 outlive the evaluation */
static val_t *vals;
static int nv, vcap;
static int err, dry, depth, quiet;          /* (quiet: failures not logged) */
static const uint8_t *base, *cur;           /* the tables (offsets in the log are from here); the term being evaluated */
static uint8_t arena[32768] __attribute__((aligned(16)));
static uint32_t used;

static void fail(const char *why, const uint8_t *at)
{
    if (!err && !dry && !quiet) log_line("aml: %s at 0x%lx\n", why, (long)((at ? at : cur) - base));
    err = 1;
}

/* ---- Memory: the arena (an evaluation's values), malloc for kept ones. */
static void *alloc(uint32_t n)
{
    n = (n + 15) & ~15u;
    if (used + n <= sizeof arena) { used += n; return arena + used - n; }
    void *p = malloc(n);                    /* (an evaluation that big: rare; kept) */
    if (!p) abort();
    return p;
}
static int temp(const void *p) { return (const uint8_t *)p >= arena && (const uint8_t *)p < arena + sizeof arena; }
static void keep(val_t *v)                  /* a value stored in a name of the tables: out of the arena */
{
    if (!temp(v->b)) return;
    uint32_t n = v->t == V_PKG ? v->n * sizeof(val_t) : v->n + 1;
    uint8_t *b = malloc(n ? n : 1);
    if (!b) abort();
    memcpy(b, v->b, n);
    v->b = b;
    if (v->t == V_PKG) for (uint32_t i = 0; i < v->n; i++) keep((val_t *)b + i);
}
static void top(void) { used = 0; nn0 = nn; err = 0; }    /* a new evaluation */

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
    for (; nn > mark; nn--) {
        node_t *x = &nd[nn - 1];
        hh[hash(x->up, x->seg)] = x->next;
        if (x->val >= 0 && x->val == nv - 1) nv--;
    }
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
static void mkbuf(val_t *o, int t, uint32_t n, const void *a, uint32_t an, const void *b, uint32_t bn)
{
    *o = (val_t){ .t = (uint8_t)t, .n = n };
    if (dry) return;
    o->b = alloc(n + 1);
    memset(o->b, 0, n + 1);
    memcpy(o->b, a, an < n ? an : n);
    if (bn && an < n) memcpy(o->b + an, b, bn < n - an ? bn : n - an);
}
static void bytes(const val_t *v, val_t *o)         /* as a buffer (an integer: its 8 bytes) */
{
    if (v->t == V_BUF || v->t == V_STR) { *o = *v; o->t = V_BUF; return; }
    int64_t x = num(v);
    mkbuf(o, V_BUF, 8, &x, 8, 0, 0);
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

static void get(int k, val_t *o);
static uint32_t sg(const char *s) { uint32_t x; memcpy(&x, s, 4); return x; }
/* The PCI address (bus << 16 | device << 11 | function << 8) of the device
 * holding node k: its _ADR; its bus: the secondary bus of the bridge it is
 * behind (a device with _ADR and no _HID), else the host bridge's _BBN. */
static int64_t pciaddr(int k)
{
    int a, up, b;
    val_t v, u = { .t = V_INT };
    uint64_t x = 0;
    for (; k > 0 && nd[k].kind != N_DEV; k = nd[k].up) ;
    if (k <= 0 || (a = child(k, sg("_ADR"))) < 0 || depth > 16) return -1;
    get(a, &v);
    for (up = nd[k].up; up > 0 && nd[up].kind != N_DEV; up = nd[up].up) ;
    if (up > 0 && child(up, sg("_HID")) < 0 && child(up, sg("_ADR")) >= 0) {
        depth++;
        int64_t p = pciaddr(up);
        depth--;
        if (p < 0 || aml_io(2, (uint64_t)p | 0x18, 4, &x, 0)) return -1;
        u.i = (int64_t)(x >> 8 & 0xFF);
    } else if (up > 0 && (b = child(up, sg("_BBN"))) >= 0) get(b, &u);
    return err ? -1 : num(&u) << 16 | (num(&v) >> 16 & 31) << 11 | (num(&v) & 7) << 8;
}

/* A field of a region: its bits to or from b, in units of the field's
 * access width (read-modify-write for a unit it covers in part) -> 0. */
static int fieldio(int k, uint8_t *b, int write)
{
    node_t *n = &nd[k];
    int r = n->reg, sp = nd[r].space, t = n->argc & 15, w = t >= 1 && t <= 4 ? 1 << (t - 1) : 1;
    if (nd[r].val < 0) {                                /* the region's address, once */
        frame_t f = { .scope = nd[r].up };
        val_t v;
        ev(nd[r].p, &f, &v);
        if (err) return -1;
        setval(r, &v);
    }
    uint64_t a = (uint64_t)num(&vals[nd[r].val]);
    int64_t pci = sp == 2 ? pciaddr(nd[r].up) : 0;      /* PCI: the device's configuration (256 bytes through 0xCF8) */
    if (pci < 0) return -1;
    if (sp == 3) w = 1;                                 /* the embedded controller: bytes */
    else if (!t) {                         /* AnyAcc: the smallest unit holding it all */
        for (w = 1; w < 8 && n->off % (8 * w) + n->bits > 8u * w; w *= 2) ;
        if (n->off % (8 * w) + n->bits > 8u * w) w = sp ? 1 : 4;
    }
    for (uint32_t done = 0; done < n->bits; ) {
        uint32_t at = (n->off + done) / 8 / w * w, sh = n->off + done - at * 8, c = n->bits - done;
        if (c > 8 * w - sh) c = 8 * w - sh;
        uint64_t u = 0, m = c < 64 ? (1ULL << c) - 1 : ~0ULL, ua = a + at;
        if (sp == 2 && (ua + w > 256 || (ua += (uint64_t)pci, 0))) return -1;
        if ((!write || c < 8u * w) && aml_io(sp, ua, w, &u, 0)) return -1;
        if (write) { u = (u & ~(m << sh)) | getbits(b, done, (int)c) << sh; if (aml_io(sp, ua, w, &u, 1)) return -1; }
        else putbits(b, done, (int)c, u >> sh & m);
        done += c;
    }
    return 0;
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
    uint8_t b[256];
    switch (n->kind) {
    case N_NAME: ev(n->p, &f, &v); if (!err) { if (k < nn0) keep(&v); setval(k, &v); *o = v; } return;
    case N_METHOD: call(k, f.a, o); return;
    case N_BFIELD:
        if (!bfield(k, 0)) *o = (val_t){ .t = V_INT, .i = (int64_t)getbits(vals[nd[k].val].b, vals[nd[k].val].i >> 8, vals[nd[k].val].i & 255) };
        return;
    case N_FIELD:
        memset(b, 0, sizeof b);
        if (n->reg < 0 || n->bits > 8 * sizeof b || fieldio(k, b, 0)) { fail("field not readable", 0); return; }
        if (n->bits > 64) mkbuf(o, V_BUF, (n->bits + 7) / 8, b, (n->bits + 7) / 8, 0, 0);
        else *o = (val_t){ .t = V_INT, .i = (int64_t)getbits(b, 0, (int)n->bits) };
        return;
    case N_EXT: fail("external name not defined", 0); return;
    }
}
static void put(int k, const val_t *v)
{
    if (k < 0) { fail("unknown name", 0); return; }
    node_t *n = &nd[k];
    val_t x = *v, y;
    if (n->kind == N_NAME) { if (k < nn0) keep(&x); setval(k, &x); }
    else if (n->kind == N_FIELD && n->bits <= 2048) {
        uint8_t b[256] = { 0 };
        bytes(v, &y);
        memcpy(b, y.b, y.n < sizeof b ? y.n : sizeof b);
        if (fieldio(k, b, 1) && n->bits <= 64) setval(k, &(val_t){ .t = V_INT, .i = num(v) });   /* not written: ours only */
    }
    else if (n->kind == N_BFIELD && !bfield(k, 0)) putbits(vals[n->val].b, vals[n->val].i >> 8, vals[n->val].i & 255, (uint64_t)num(v));
}

/* A package's element (names in packages: their values; a method or a
 * device: nothing, no error). */
static const uint8_t *element(const uint8_t *p, int scope, val_t *o)
{
    name_t m;
    if (!isname(*p)) { frame_t f = { .scope = scope }; return ev(p, &f, o); }
    p = getname(p, &m);
    int k = find(scope, &m);
    *o = (val_t){ .t = V_NONE };
    if (!dry && k >= 0 && nd[k].kind != N_METHOD && nd[k].kind != N_DEV && nd[k].kind != N_SCOPE) get(k, o);
    return p;
}
static void index_of(const val_t *a, int64_t i, val_t *o)
{
    *o = (val_t){ .t = V_INT };
    if ((a->t == V_BUF || a->t == V_STR) && i >= 0 && i < a->n) o->i = a->b[i];
    else if (a->t == V_PKG && i >= 0 && i < a->n) *o = ((val_t *)a->b)[i];
    else fail("index", 0);
}

/* Where a result goes (a "target"): nowhere, a local, an argument, a name, an element. */
static const uint8_t *store(const uint8_t *p, frame_t *f, const val_t *v)
{
    uint8_t op = *p;
    name_t m;
    if (!op) return p + 1;
    if (op >= 0x60 && op <= 0x67) { if (!dry) f->l[op - 0x60] = *v; return p + 1; }
    if (op >= 0x68 && op <= 0x6E) { if (!dry) f->a[op - 0x68] = *v; return p + 1; }
    if (op == 0x5B && p[1] == 0x31) return p + 2;                       /* Debug */
    if (op == 0x88) {                                                   /* Index (buffer or package, i) */
        val_t b, i, z = { .t = V_NONE }, x = *v;
        p = store(ev(ev(p + 1, f, &b), f, &i), f, &z);
        int64_t k = num(&i);
        if (dry) return p;
        if (b.t == V_BUF && k >= 0 && k < b.n) b.b[k] = (uint8_t)num(v);
        else if (b.t == V_PKG && k >= 0 && k < b.n) { if (!temp(b.b)) keep(&x); ((val_t *)b.b)[k] = x; }
        else fail("store to Index", p);
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
    val_t a, b, c;
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
        mkbuf(o, V_BUF, (uint32_t)(x > e - p ? x : e - p), p, (uint32_t)(e - p), 0, 0);
        return e;
    case 0x12: case 0x13:                                               /* Package, VarPackage: an array */
        p = pkg(p, &e);
        if (op == 0x12) x = *p++; else p = ev(p, f, &a), x = num(&a);
        if (dry || err || x < 0 || x > 4096) return e;
        *o = (val_t){ .t = V_PKG, .n = (uint32_t)x, .b = alloc((uint32_t)x * sizeof(val_t) + 1) };
        for (int64_t k = 0; k < x; k++) {
            val_t *el = (val_t *)o->b + k;
            *el = (val_t){ .t = V_INT };
            if (p < e) p = element(p, f->scope, el);
        }
        return e;
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: *o = f->l[op - 0x60]; return p;
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: *o = f->a[op - 0x68]; return p;
    case 0x70: case 0x9D: p = ev(p, f, o); return store(p, f, o);      /* Store, CopyObject (value, target) */
    case 0x71: return ev(p, f, o);                                      /* RefOf: the value will do */
    case 0x72: case 0x74: case 0x77: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: case 0x85:
        p = ev(ev(p, f, &a), f, &b);
        x = num(&a); y = num(&b);
        o->i = op == 0x72 ? x + y : op == 0x74 ? x - y : op == 0x77 ? x * y : op == 0x79 ? (y < 64 ? x << y : 0) :
               op == 0x7A ? (y < 64 ? (int64_t)((uint64_t)x >> y) : 0) : op == 0x7B ? x & y : op == 0x7C ? ~(x & y) :
               op == 0x7D ? x | y : op == 0x7E ? ~(x | y) : op == 0x7F ? x ^ y : y ? (int64_t)((uint64_t)x % (uint64_t)y) : 0;
        return store(p, f, o);
    case 0x78:                                                          /* Divide (a, b, remainder, quotient) */
        p = ev(ev(p, f, &a), f, &b);
        x = num(&a); y = num(&b);
        if (!y && !dry) { fail("divide by zero", at); return p; }
        c = (val_t){ .t = V_INT, .i = y ? (int64_t)((uint64_t)x % (uint64_t)y) : 0 };
        o->i = y ? (int64_t)((uint64_t)x / (uint64_t)y) : 0;
        return store(store(p, f, &c), f, o);
    case 0x73:                                                          /* Concatenate (a, b, target) */
        p = ev(ev(p, f, &a), f, &b);
        if (a.t == V_STR && b.t == V_STR) mkbuf(o, V_STR, a.n + b.n, a.b, a.n, b.b, b.n);
        else { bytes(&a, &a); bytes(&b, &b); mkbuf(o, V_BUF, a.n + b.n, a.b, a.n, b.b, b.n); }
        return store(p, f, o);
    case 0x75: case 0x76:                                               /* Increment, Decrement */
        e = ev(p, f, &a);
        *o = (val_t){ .t = V_INT, .i = num(&a) + (op == 0x75 ? 1 : -1) };
        store(p, f, o);
        return e;
    case 0x80: p = ev(p, f, &a); o->i = ~num(&a); return store(p, f, o);   /* Not */
    case 0x81: case 0x82:                                               /* FindSetLeftBit, FindSetRightBit */
        p = ev(p, f, &a);
        x = num(&a);
        for (int k = 0; x && k < 64; k++) if ((uint64_t)x >> k & 1) { o->i = k + 1; if (op == 0x82) break; }
        return store(p, f, o);
    case 0x83: return ev(p, f, o);                                      /* DerefOf: Index gives values already */
    case 0x86:                                                          /* Notify (object, value) */
        e = getname(p, &m);
        p = ev(e, f, &a);
        if (!dry && aml_notify) aml_notify(find(f->scope, &m), num(&a));
        return p;
    case 0x87: p = ev(p, f, &a); o->i = a.t == V_INT ? 8 : a.n; return p;  /* SizeOf */
    case 0x88:                                                          /* Index (source, i, target) */
        p = ev(ev(p, f, &a), f, &b);
        if (!dry) index_of(&a, num(&b), o);
        return store(p, f, o);
    case 0x8E: p = ev(p, f, &a); o->i = a.t == V_NONE ? 0 : a.t + 1; return p;   /* ObjectType */
    case 0x90: p = ev(ev(p, f, &a), f, &b); o->i = num(&a) && num(&b) ? -1 : 0; return p;   /* LAnd */
    case 0x91: p = ev(ev(p, f, &a), f, &b); o->i = num(&a) || num(&b) ? -1 : 0; return p;   /* LOr */
    case 0x92: p = ev(p, f, &a); o->i = num(&a) ? 0 : -1; return p;    /* LNot (also LNotEqual...) */
    case 0x93: case 0x94: case 0x95:                                    /* LEqual, LGreater, LLess */
        p = ev(ev(p, f, &a), f, &b);
        x = dry ? 0 : cmp(&a, &b);
        o->i = (op == 0x93 ? !x : op == 0x94 ? x > 0 : x < 0) ? -1 : 0;
        return p;
    case 0x96: p = ev(p, f, &a); bytes(&a, o); return store(p, f, o);  /* ToBuffer */
    case 0x99: p = ev(p, f, &a); o->i = num(&a); return store(p, f, o);    /* ToInteger */
    case 0x9C:                                                          /* ToString (buffer, length, target) */
        p = ev(ev(p, f, &a), f, &b);
        bytes(&a, &a);
        for (y = 0; y < a.n && (uint64_t)y < (uint64_t)num(&b) && a.b[y]; y++) ;
        mkbuf(o, V_STR, (uint32_t)y, a.b, (uint32_t)y, 0, 0);
        return store(p, f, o);
    case 0x9E:                                                          /* Mid (source, index, length, target) */
        p = ev(ev(ev(p, f, &a), f, &b), f, &c);
        x = num(&b); y = num(&c);
        if (x > a.n || x < 0) x = a.n;
        if (y < 0 || y > a.n - x) y = a.n - x;
        mkbuf(o, a.t == V_STR ? V_STR : V_BUF, (uint32_t)y, a.b + x, (uint32_t)y, 0, 0);
        return store(p, f, o);
    case 0x84:                                                          /* ConcatenateResTemplate */
        p = ev(ev(p, f, &a), f, &b);
        if (!dry && (a.t != V_BUF || b.t != V_BUF || a.n < 2)) { fail("ConcatenateResTemplate", at); return p; }
        mkbuf(o, V_BUF, a.n - 2 + b.n, a.b, a.n - 2, b.b, b.n);         /* (without the first one's end tag) */
        return store(p, f, o);
    case 0x5B:
        switch (*p++) {
        case 0x12:                                                      /* CondRefOf (name, target) */
            if (isname(*p)) { p = getname(p, &m); o->i = !dry && find(f->scope, &m) >= 0 && nd[find(f->scope, &m)].kind != N_EXT ? -1 : 0; }
            else p = ev(p, f, &a), o->i = -1;
            return store(p, f, &(val_t){ .t = V_NONE });
        case 0x21: case 0x22:                                           /* Stall (us), Sleep (ms) */
            p = ev(p, f, &a);
#ifndef AML_HOST
            if (!dry) sys_sleep((uint64_t)num(&a) * (at[1] == 0x21 ? 1000 : 1000000));
#endif
            return p;
        case 0x23: o->i = 0; return getname(p, &m) + 2;                 /* Acquire (mutex, timeout): ours */
        case 0x24: case 0x26: case 0x27: return getname(p, &m);         /* Signal, Reset (event), Release (mutex) */
        case 0x25: o->i = 0; return ev(getname(p, &m), f, &a);          /* Wait (event, timeout) */
        case 0x28: case 0x29: {                                         /* FromBCD, ToBCD (value, target) */
            p = ev(p, f, &a);
            uint64_t u = (uint64_t)num(&a), r = 0, d = at[1] == 0x28 ? 16 : 10;
            for (uint64_t m = 1; u; u /= d, m *= 26 - d) r += u % d * m;     /* (BCD digits in, decimal ones out, or back) */
            o->i = (int64_t)r;
            return store(p, f, o);
        }
        case 0x30: o->i = 2; return p;                                  /* Revision */
        case 0x33:                                                      /* Timer: 100 ns units */
#ifndef AML_HOST
            o->i = sys_clock() / 100;
#endif
            return p;
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
            if (k < nn0) keep(&v);
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
                fields(q, e, f->scope, find(f->scope, &m));
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
            if (p[1] == 0x01 || p[1] == 0x02) { p = getname(p + 2, &m) + (p[1] == 0x01); declare(f->scope, &m, N_OTHER); continue; }   /* Mutex, Event */
            /* fall through */
        default: p = ev(p, f, &v);
        }
    }
}

/* A Field's list (p: its flags): named runs of bits of a region, one after the other. */
static void fields(const uint8_t *p, const uint8_t *e, int s, int reg)
{
    uint32_t off = 0, n;
    uint8_t acc = *p++ & 15;
    while (p < e) {
        if (!*p) { p = plen(p + 1, &n); off += n; }                     /* reserved bits */
        else if (*p == 1) acc = p[1] & 15, p += 3;                      /* AccessField */
        else if (*p == 3) p += 4;                                       /* ExtendedAccessField */
        else if (*p == 2) { name_t m; const uint8_t *q; p = p[1] == 0x11 ? (pkg(p + 2, &q), q) : getname(p + 1, &m); }   /* Connection */
        else {
            uint32_t seg;
            memcpy(&seg, p, 4);
            p = plen(p + 4, &n);
            int k = add(s, seg, reg >= 0 && nd[reg].kind == N_REGION ? N_FIELD : N_OTHER);
            nd[k].reg = reg; nd[k].off = off; nd[k].bits = n; nd[k].argc = acc;
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
            if (!run && e <= end) fields(q, e, s, k);
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
        if (!memcmp(t + o, "DSDT", 4) || !memcmp(t + o, "SSDT", 4)) load(t + o + 36, t + o + len, 0, 0);
        o += len;
    }
    name_t m = { .n = 1 };
    memcpy(m.s, "OSYS", 4);
    int k = find(0, &m);                                                /* we are Windows 2015 (_OSI) */
    if (k >= 0 && nd[k].kind == N_FIELD) setval(k, &(val_t){ .t = V_INT, .i = 0x07DF });
    return nn;
}

/* ---- Devices. */
void aml_path(int k, char *s, int n)
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
    top();
    if (nd[k].kind == N_METHOD) call(k, a, o); else get(k, o);
    if (err) { char p[48]; aml_path(dev, p, sizeof p); log_line("aml: (in %s.%s)\n", p, seg); err = 0; o->t = V_NONE; }
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
static int is(const val_t *v, const char *a, const char *b)            /* an id (or a package of them) a or b? */
{
    char s[12];
    if (v->t == V_PKG) { for (uint32_t i = 0; i < v->n; i++) if (is((val_t *)v->b + i, a, b)) return 1; return 0; }
    if (v->t != V_STR && v->t != V_INT) return 0;
    id(v, s);
    return !strcmp(s, a) || (b && !strcmp(s, b));
}
static int has(int k, const char *a, const char *b)                     /* device k's _CID or _HID says so */
{
    val_t v, x[1];
    return nd[k].kind == N_DEV && ((!obj(k, "_CID", x, &v) && is(&v, a, b)) || (!obj(k, "_HID", x, &v) && is(&v, a, b)));
}
int aml_devs(const char *hid, int *dev, int max)
{
    int found = 0;
    val_t v, x[1];
    for (int k = 1, n0 = nn; k < n0 && found < max; k++)
        if (has(k, hid, 0) && (obj(k, "_STA", x, &v) || num(&v) & 1)) dev[found++] = k;
    return found;
}
static void cstr(char *d, uint32_t dn, const uint8_t *s, uint32_t sn)   /* a string from a descriptor */
{
    uint32_t k = 0;
    for (; k + 1 < dn && k < sn && s[k]; k++) d[k] = (char)s[k];
    d[k] = 0;
}
static void resources(aml_hid_t *d, const uint8_t *b, uint32_t n)
{
    int nio = 0;
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
        if (t == 0x8E && r[5] == 3 && !d->baud) {                       /* UartSerialBus */
            d->baud = r[12] | r[13] << 8 | r[14] << 16 | (uint32_t)r[15] << 24;
            d->flow = (r[7] & 3) == 1;
            if (!d->baud) d->baud = 1;                                  /* (one, speed unknown) */
        }
        if (t == 0x8C && !r[4]) {                                       /* GpioInt */
            uint32_t pt = r[14] | r[15] << 8, sn = r[17] | r[18] << 8;
            int pin = pt + 1 < hdr + len ? r[pt] | r[pt + 1] << 8 : -1;
            if (d->npins < 4) d->pins[d->npins++] = pin;
            if (d->pin < 0) {
                d->pin_edge = r[7] & 1; d->pin_low = (r[7] >> 1 & 3) == 1;
                d->pin = pin;
                if (sn < hdr + len) cstr(d->gpio, sizeof d->gpio, r + sn, hdr + len - sn);
            }
        }
        if (t == 0x89 && d->irq < 0 && r[4]) d->irq = r[5] | r[6] << 8 | r[7] << 16;   /* Interrupt */
        if (t == 0x40 && nio < 2) d->io[nio++] = r[2] | r[3] << 8;     /* IO (decode, min, max, align, length) */
    }
}
static void blank(aml_hid_t *d) { memset(d, 0, sizeof *d); d->addr = d->pin = d->irq = d->desc = d->io[0] = d->io[1] = -1; }

int aml_hid(aml_hid_t *d, int max)
{
    static const uint8_t dsm[16] = { 0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45, 0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE };
    int found = 0, n0 = nn;
    val_t v, a[4];
    for (int k = 1; k < n0 && found < max; k++) {
        if (!has(k, "PNP0C50", "ACPI0C50")) continue;
        aml_hid_t *h = &d[found];
        blank(h);
        aml_path(k, h->path, sizeof h->path);
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

int aml_find(int dev, const char *s)                                    /* "\_SB.GINF", "_BST" (dev's own) */
{
    name_t m = { .root = *s == '\\' };
    for (s += m.root; *s && m.n < 8; m.n++) {
        char seg[4] = { '_', '_', '_', '_' };
        for (int i = 0; *s && *s != '.'; s++) if (i < 4) seg[i++] = *s;
        memcpy(&m.s[m.n], seg, 4);
        if (*s == '.') s++;
    }
    return m.n == 1 && !m.root ? child(dev, m.s[0]) : find(dev, &m);
}
static int eval(int dev, const char *s, int argc, const int64_t *arg, val_t *o)   /* (s 0: dev itself) */
{
    val_t a[7];
    int k = s ? aml_find(dev, s) : dev;
    if (k < 0) return -1;
    for (int i = 0; i < argc && i < 7; i++) a[i] = (val_t){ .t = V_INT, .i = arg[i] };
    top();
    if (nd[k].kind == N_METHOD) call(k, a, o); else get(k, o);
    if (err && !quiet) { char p[48]; aml_path(k, p, sizeof p); log_line("aml: (in %s)\n", p); }
    if (err || o->t == V_NONE) { err = 0; return -1; }
    return 0;
}
int aml_eval(int dev, const char *path, int argc, const int64_t *arg, int64_t *v, int max)
{
    val_t o;
    if (eval(dev, path, argc, arg, &o)) return -1;
    if (o.t != V_PKG) { if (max > 0) *v = num(&o); return 1; }
    int n = 0;
    for (val_t *e = (val_t *)o.b; n < (int)o.n && n < max; n++) v[n] = e[n].t == V_PKG ? -1 : num(e + n);   /* (a package in it: -1) */
    return n;
}
int aml_gpio(int pin, aml_gpio_t *g)
{
    int64_t a[2] = { 0, 8 }, n = 0, first = 0, at = 0, off = 0, own = 0, sts = 0;
    quiet = 1;                                  /* (no log: a short package says which layout) */
    int f = aml_eval(0, "\\_SB.GINF", 2, a, &first, 1) < 0 ? 6 : 8;   /* a group's first pin: 8 (Tiger Lake on), 6 (Cannon, Ice Lake) */
    quiet = 0;
    for (int k = 0; k < 32; k++) {
        a[0] = k; a[1] = 1;
        if (aml_eval(0, "\\_SB.GINF", 2, a, &n, 1) < 0 || (a[1] = f, aml_eval(0, "\\_SB.GINF", 2, a, &first, 1) < 0)) break;
        if (pin < first || pin >= first + n) continue;
        a[1] = 2;
        if (aml_eval(0, "\\_SB.GADR", 2, a, &at, 1) < 0 || (a[1] = 2, aml_eval(0, "\\_SB.GINF", 2, a, &off, 1) < 0) ||
            (a[1] = 3, aml_eval(0, "\\_SB.GINF", 2, a, &own, 1) < 0) || (a[1] = 5, aml_eval(0, "\\_SB.GINF", 2, a, &sts, 1) < 0)) return -1;
        g->group = k; g->bit = (int)(pin - first);
        g->base = (uint64_t)(at - off);
        g->pad = (uint32_t)(off + 16 * g->bit); g->own = (uint32_t)own; g->sts = (uint32_t)sts;
        return 0;
    }
    return -1;
}

int aml_wake(int dev, int s, int on)
{
    int64_t v[2], a[3] = { on, s, 0 }, r;
    if (aml_eval(dev, "_PRW", 0, 0, v, 2) != 2 || v[0] < 0 || v[1] < s) return -1;
    if (on >= 0) aml_eval(dev, aml_find(dev, "_DSW") >= 0 ? "_DSW" : "_PSW", 3, a, &r, 1);
    return (int)v[0];
}
/* PCI interrupt links ("PNP0C0F": the chipset's routing of the PCI lines
 * to interrupts): their current setting (_CRS) kept, or given back (_SRS),
 * which the sleep may lose (QEMU's PIIX does). -> how many */
int aml_links(int back)
{
    static int dev[16], n;
    static val_t crs[16];
    val_t o;
    if (!back) n = aml_devs("PNP0C0F", dev, 16);
    for (int i = 0; i < n; i++) {
        int k = child(dev[i], sg(back ? "_SRS" : "_CRS"));
        top();
        if (k >= 0 && back && crs[i].t == V_BUF) call(k, &crs[i], &o);
        else if (k >= 0 && !back) { get(k, &o); if (!err && o.t == V_BUF) keep(&o), crs[i] = o; }
        err = 0;
    }
    return n;
}
int aml_res(int dev, const char *path, aml_hid_t *d)
{
    val_t o;
    blank(d);
    if (eval(dev, path, 0, 0, &o) || o.t != V_BUF) return -1;
    resources(d, o.b, o.n);
    return 0;
}
