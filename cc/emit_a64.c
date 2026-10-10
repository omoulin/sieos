/*
 * emit_a64.c - IR to AArch64 assembly text (GNU syntax), AAPCS64 calls.
 *
 * AArch64 instructions work on registers only (loads and stores apart), so a
 * spilled virtual register is loaded into a scratch register, used, and
 * stored back. Scratch registers: x14-x18 and v30-v31. (x18 is the
 * "platform register" of AAPCS64; SIEOS gives it no other use.)
 *
 * The stack frame:   [ stack arguments  ]  <- x29 + 16
 *                    [ saved x29, x30   ]  <- x29
 *                    [ saved x19-x28, d8-d15 (those used) ]
 *                    [ stack slots: locals, spills ]   <- sp (16-aligned)
 * A leaf that needs none of it gets no frame at all.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ir.h"

enum { S0 = 14, S1 = 15, S2 = 16, S3 = 17, S4 = 18, VS0 = A64_V0 + 30, VS1 = A64_V0 + 31 };

static Buf *o;
static int fn_id, uniq, noframe;
static Block *nextb;
static Buf tables;

/* ---- names */
static char *tmp(void) { static char ring[16][96]; static int k; return ring[k++ & 15]; }

static const char *X(int p) { char *s = tmp(); if (p == A64_SP) return "sp"; snprintf(s, 96, "x%d", p); return s; }
static const char *W(int p) { char *s = tmp(); snprintf(s, 96, "w%d", p); return s; }
static const char *G(int p, int w) { return w == 8 ? X(p) : W(p); }          /* a general register, by width */
static const char *V(int p, int w) { char *s = tmp(); snprintf(s, 96, "%c%d", w == 4 ? 's' : w == 16 ? 'q' : 'd', p - A64_V0); return s; }

static void out(const char *f, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(line, sizeof line, f, ap);
    va_end(ap);
    buf_c(o, '\t');
    buf_add(o, line, n);
    buf_c(o, '\n');
}

static int isreg(int v) { return F->vr[v].phys >= 0; }
static int ph(int v) { return F->vr[v].phys; }
static int isflt(int v) { return F->vr[v].cls == C_FLT; }
static long slot_off(int slot) { return F->slots[slot].off; }

/* ---- immediates */
static void mov_imm(int p, long imm, int w)     /* general register p = imm (movz/movn/movk) */
{
    unsigned long v = w == 4 ? (unsigned long)(unsigned)imm : (unsigned long)imm;
    int n = w == 4 ? 2 : 4, zeros = 0, ones = 0;
    unsigned long mask = w == 4 ? 0xffffffffUL : ~0UL;
    for (int k = 0; k < n; k++) { int c = v >> (16 * k) & 0xffff; zeros += c == 0; ones += c == 0xffff; }
    int inv = ones > zeros;                       /* movn: start from all ones */
    int first = 1;
    for (int k = 0; k < n; k++) {
        int c = v >> (16 * k) & 0xffff;
        if (c == (inv ? 0xffff : 0) && !(first && k == n - 1)) continue;
        if (first) {
            if (inv) out("movn %s, #%d, lsl #%d", G(p, w), (int)(~c & 0xffff), 16 * k);
            else out("movz %s, #%d, lsl #%d", G(p, w), c, 16 * k);
            first = 0;
        } else out("movk %s, #%d, lsl #%d", G(p, w), c, 16 * k);
    }
    (void)mask;
}

/* Is v a logical immediate (a repeated, rotated run of ones) for a w-byte operation? */
int a64_logical_imm(unsigned long v, int w)
{
    if (w == 4) v = (v & 0xffffffffUL) | (v & 0xffffffffUL) << 32;
    if (v == 0 || v == ~0UL) return 0;
    for (int size = 2; size <= 64; size *= 2) {
        unsigned long m = size == 64 ? ~0UL : (1UL << size) - 1, e = v & m;
        int rep = 1;
        for (int s = size; s < 64; s += size) if ((v >> s & m) != e) { rep = 0; break; }
        if (!rep) continue;
        for (int r = 0; r < size; r++) {          /* rotated so that the ones are at the bottom */
            unsigned long x = r ? ((e >> r) | (e << (size - r))) & m : e;
            if (x && !(x & (x + 1))) return 1;
        }
        return 0;
    }
    return 0;
}

static int add_imm_ok(long v) { return (v >= 0 && v < 4096) || (v >= 0 && v < (1L << 24) && !(v & 0xfff)); }

/* ---- operands: a virtual register's value in a register */
static const char *spill(int v)                 /* its frame slot, as an address (x29-relative) */
{
    long off = slot_off(F->vr[v].slot);
    char *s = tmp();
    if (off >= -256) { snprintf(s, 96, "[x29, #%ld]", off); return s; }
    if (-off < 4096) out("sub x%d, x29, #%ld", S4, -off);
    else { mov_imm(S4, -off, 8); out("sub x%d, x29, x%d", S4, S4); }
    return "[x18]";
}

static int use_reg(int v, int scratch, int w)   /* v's register, or the scratch one loaded */
{
    if (isreg(v)) return ph(v);
    if (isflt(v)) out("ldr %s, %s", V(scratch, w), spill(v));
    else out("ldr %s, %s", G(scratch, w == 8 ? 8 : 4), spill(v));
    return scratch;
}
static int def_reg(int v, int scratch) { return isreg(v) ? ph(v) : scratch; }
static void def_done(int v, int r, int w)       /* a result in scratch r: into the spilled v */
{
    if (isreg(v)) return;
    if (isflt(v)) out("str %s, %s", V(r, w), spill(v));
    else out("str %s, %s", G(r, w == 8 ? 8 : 4), spill(v));
}

static void mov_rr(int d, int s, int w)          /* register copy (general or float) */
{
    if (d == s) return;
    if (d >= A64_V0 && s >= A64_V0) out("fmov %s, %s", V(d, 8), V(s, 8));
    else if (d >= A64_V0) out("fmov %s, %s", V(d, w == 4 ? 4 : 8), G(s, w == 4 ? 4 : 8));
    else if (s >= A64_V0) out("fmov %s, %s", G(d, w == 4 ? 4 : 8), V(s, w == 4 ? 4 : 8));
    else out("mov %s, %s", X(d), X(s));
}

/* ---- addresses: [base], [base, #off], [base, index, lsl #s]; anything else
 * is computed into scratch S2 first. size: the access size (for the range of
 * scaled offsets). */
static const char *amode(Addr *m, int size)
{
    char *s = tmp();
    long off = m->off;
    int base = -1;
    if (m->tls) {                                   /* thread-local: tpidr_el0 + the offset the linker knows */
        out("mrs x%d, tpidr_el0", S2);
        out("add x%d, x%d, #:tprel_hi12:%s%+ld, lsl #12", S2, S2, m->sym, off);
        out("add x%d, x%d, #:tprel_lo12_nc:%s%+ld", S2, S2, m->sym, off);
        base = S2;
        off = 0;
    } else if (m->sym) {
        out("adrp x%d, %s%+ld", S2, m->sym, off);
        out("add x%d, x%d, :lo12:%s%+ld", S2, S2, m->sym, off);
        base = S2;
        off = 0;
    } else if (m->slot) { base = 29; off += slot_off(m->slot); }
    else if (m->args) { base = 29; off += 16; }
    if (m->base) {
        int b = use_reg(m->base, S3, 8);
        if (base >= 0) { out("add x%d, %s, %s", S2, X(base), X(b)); base = S2; }
        else base = b;
    }
    if (m->index) {                                 /* (S1: the base may be in S3, the value in S0) */
        int x = use_reg(m->index, S1, 8), sh = __builtin_ctz(m->scale ? m->scale : 1);
        if (base < 0) { if (sh) out("lsl x%d, %s, #%d", S2, X(x), sh); else out("mov x%d, %s", S2, X(x)); }
        else if (!off && !sh) { snprintf(s, 96, "[%s, %s]", X(base), X(x)); return s; }
        else if (!off && 1 << sh == size) { snprintf(s, 96, "[%s, %s, lsl #%d]", X(base), X(x), sh); return s; }
        else out("add x%d, %s, %s, lsl #%d", S2, X(base), X(x), sh);
        base = S2;
    }
    if (base < 0) { mov_imm(S2, off, 8); snprintf(s, 96, "[x%d]", S2); return s; }   /* an absolute address */
    int scaled = off >= 0 && off % size == 0 && off / size < 4096;
    if (!scaled && (off < -256 || off > 255)) {     /* out of reach: into the base */
        if (off < 0 && -off < 4096) out("sub x%d, %s, #%ld", S2, X(base), -off);
        else if (off > 0 && add_imm_ok(off)) out("add x%d, %s, #%ld", S2, X(base), off);
        else { mov_imm(S4, off, 8); out("add x%d, %s, x%d", S2, X(base), S4); }
        base = S2;
        off = 0;
    }
    if (off) snprintf(s, 96, "[%s, #%ld]", X(base), off);
    else snprintf(s, 96, "[%s]", X(base));
    return s;
}

static void addr_into(int d, Addr *m)              /* d = the address of m */
{
    Addr a = *m;
    long off = a.off;
    int base = -1;
    if (a.tls || a.sym) {                          /* the symbol (+ offset) into S2, then base and index */
        Addr s = { .sym = a.sym, .tls = a.tls, .off = a.off };
        amode(&s, 1);
        base = S2;
        off = 0;
    } else if (a.slot) { base = 29; off += slot_off(a.slot); }
    else if (a.args) { base = 29; off += 16; }
    if (a.base) {
        int b = use_reg(a.base, S3, 8);
        if (base >= 0) { out("add x%d, %s, %s", S2, X(base), X(b)); base = S2; } else base = b;
    }
    if (a.index) {
        int x = use_reg(a.index, S4, 8), sh = __builtin_ctz(a.scale ? a.scale : 1);
        if (base < 0) { if (sh) out("lsl x%d, %s, #%d", S2, X(x), sh); else out("mov x%d, %s", S2, X(x)); }
        else out("add x%d, %s, %s, lsl #%d", S2, X(base), X(x), sh);
        base = S2;
    }
    if (base < 0) { mov_imm(d, off, 8); return; }
    if (!off) { if (d != base) out("mov %s, %s", X(d), X(base)); }
    else if (off < 0 && -off < 4096) out("sub %s, %s, #%ld", X(d), X(base), -off);
    else if (off > 0 && add_imm_ok(off)) out("add %s, %s, #%ld", X(d), X(base), off);
    else { mov_imm(S4, off, 8); out("add %s, %s, x%d", X(d), X(base), S4); }
}

/* ---- parallel moves (call arguments, parameters, asm operands, struct returns) */
typedef struct { int dreg, sv, sreg, simm, w, done; long imm; } Move;

static void par_move(Move *mv, int n)
{
    for (int i = 0; i < n; i++) {
        mv[i].done = 0;
        if (mv[i].sv && isreg(mv[i].sv)) { mv[i].sreg = ph(mv[i].sv); mv[i].sv = 0; }
        else if (mv[i].sv || mv[i].simm) mv[i].sreg = -1;
        if (mv[i].sreg >= 0 && mv[i].sreg == mv[i].dreg) mv[i].done = 1;
    }
    for (;;) {
        int left = 0, progress = 0;
        for (int i = 0; i < n; i++) {
            Move *m = &mv[i];
            if (m->done || m->sreg < 0) continue;
            left++;
            int blocked = 0;
            for (int j = 0; j < n; j++) if (j != i && !mv[j].done && mv[j].sreg == m->dreg) { blocked = 1; break; }
            if (blocked) continue;
            mov_rr(m->dreg, m->sreg, m->w);
            m->done = 1;
            progress = 1;
        }
        if (!left) break;
        if (!progress) {                         /* a cycle: free one source through a scratch register */
            for (int i = 0; i < n; i++) {
                Move *m = &mv[i];
                if (m->done || m->sreg < 0) continue;
                int s = m->sreg, t = s >= A64_V0 ? VS1 : S3;
                mov_rr(t, s, 8);
                for (int j = 0; j < n; j++) if (!mv[j].done && mv[j].sreg == s) mv[j].sreg = t;
                break;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        Move *m = &mv[i];
        if (m->done) continue;
        if (m->simm) {
            if (m->dreg >= A64_V0) { mov_imm(S3, m->imm, 8); out("fmov %s, x%d", V(m->dreg, 8), S3); }
            else mov_imm(m->dreg, m->imm, 8);
        } else if (m->dreg >= A64_V0) out("ldr %s, %s", V(m->dreg, m->w == 4 ? 4 : 8), spill(m->sv));
        else out("ldr %s, %s", X(m->dreg), spill(m->sv));
        m->done = 1;
    }
}

/* ---- conditions */
static const char *cc_name(int cc)
{
    static const char *n[] = { "eq", "ne", "lt", "le", "gt", "ge", "lo", "ls", "hi", "hs",
                               "eq", "ne", "mi", "ls", "gt", "ge" };   /* floats: unordered makes each false, but ne */
    return n[cc];
}
static int cc_invert(int cc)
{
    static const int inv[] = { CC_NE, CC_EQ, CC_GE, CC_GT, CC_LE, CC_LT, CC_UGE, CC_UGT, CC_ULE, CC_ULT };
    return inv[cc];
}

static void compare(Ins *i)                        /* the flags of a - b */
{
    int w = i->w;
    if (i->cc >= CC_FEQ) {
        int a = use_reg(i->a, VS0, w);
        if (i->bimm && i->imm == 0) out("fcmp %s, #0.0", V(a, w));
        else out("fcmp %s, %s", V(a, w), V(use_reg(i->b, VS1, w), w));
        return;
    }
    int a = use_reg(i->a, S0, w);
    if (i->bimm) {
        long v = w == 4 ? (long)(int)i->imm : i->imm;
        if (v >= 0 && add_imm_ok(v)) out("cmp %s, #%ld", G(a, w), v);
        else if (v < 0 && add_imm_ok(-v)) out("cmn %s, #%ld", G(a, w), -v);
        else { mov_imm(S1, v, w); out("cmp %s, %s", G(a, w), G(S1, w)); }
        return;
    }
    out("cmp %s, %s", G(a, w), G(use_reg(i->b, S1, w), w));
}

/* ---- instructions */
static void binop(Ins *i, const char *op, int logical)
{
    int w = i->w, d = def_reg(i->dst, S0), a = use_reg(i->a, S0, w);
    if (i->bimm) {
        long v = w == 4 ? (long)(int)i->imm : i->imm;
        if (!logical && v >= 0 && add_imm_ok(v)) out("%s %s, %s, #%ld", op, G(d, w), G(a, w), v);
        else if (!logical && v < 0 && add_imm_ok(-v)) out("%s %s, %s, #%ld", !strcmp(op, "add") ? "sub" : "add", G(d, w), G(a, w), -v);
        else if (logical && a64_logical_imm(v, w)) out("%s %s, %s, #%ld", op, G(d, w), G(a, w), w == 4 ? (long)(unsigned)v : v);
        else { mov_imm(S1, v, w); out("%s %s, %s, %s", op, G(d, w), G(a, w), G(S1, w)); }
    } else out("%s %s, %s, %s", op, G(d, w), G(a, w), G(use_reg(i->b, S1, w), w));
    def_done(i->dst, d, w);
}

static void shift(Ins *i, const char *op)
{
    int w = i->w, d = def_reg(i->dst, S0), a = use_reg(i->a, S0, w);
    if (i->bimm) {
        int k = i->imm & (w * 8 - 1);
        if (i->op == O_ROL) out("ror %s, %s, #%d", G(d, w), G(a, w), (w * 8 - k) & (w * 8 - 1));
        else out("%s %s, %s, #%d", op, G(d, w), G(a, w), k);
    } else {
        int b = use_reg(i->b, S1, w);
        if (i->op == O_ROL) { out("neg %s, %s", G(S1, w), G(b, w)); b = S1; }
        out("%s %s, %s, %s", op, G(d, w), G(a, w), G(b, w));
    }
    def_done(i->dst, d, w);
}

static void divide(Ins *i)
{
    int w = i->w, sgn = i->op == O_DIV || i->op == O_MOD;
    int a = use_reg(i->a, S0, w), b = use_reg(i->b, S1, w), d = def_reg(i->dst, S2);
    if (i->op == O_DIV || i->op == O_UDIV) out("%s %s, %s, %s", sgn ? "sdiv" : "udiv", G(d, w), G(a, w), G(b, w));
    else {
        out("%s %s, %s, %s", sgn ? "sdiv" : "udiv", G(S3, w), G(a, w), G(b, w));
        out("msub %s, %s, %s, %s", G(d, w), G(S3, w), G(b, w), G(a, w));
    }
    def_done(i->dst, d, w);
}

static void extend(Ins *i)
{
    int w = i->w, d = def_reg(i->dst, S0), a = use_reg(i->a, S0, i->sz == 8 ? 8 : 4);
    int ww = i->sgn && w == 8 ? 8 : 4;
    switch (i->sz) {
    case 1: if (i->sgn) out("sxtb %s, %s", G(d, ww), W(a)); else out("and %s, %s, #255", W(d), W(a)); break;
    case 2: if (i->sgn) out("sxth %s, %s", G(d, ww), W(a)); else out("and %s, %s, #65535", W(d), W(a)); break;
    case 4: if (i->sgn) out("sxtw %s, %s", X(d), W(a)); else out("mov %s, %s", W(d), W(a)); break;
    default: if (d != a) out("mov %s, %s", X(d), X(a));
    }
    def_done(i->dst, d, w);
}

static const char *ld_mn(int sz, int sgn, int w)    /* the load for sz bytes, extended to w */
{
    switch (sz) {
    case 1: return sgn ? "ldrsb" : "ldrb";
    case 2: return sgn ? "ldrsh" : "ldrh";
    case 4: return sgn && w == 8 ? "ldrsw" : "ldr";
    }
    return "ldr";
}

static void load(Ins *i)
{
    if (isflt(i->dst)) {
        int d = def_reg(i->dst, VS0);
        out("ldr %s, %s", V(d, i->sz == 4 ? 4 : 8), amode(&i->m, i->sz));
        def_done(i->dst, d, i->w);
        return;
    }
    int d = def_reg(i->dst, S0), w = i->w;
    int rw = (i->sz == 8) || (i->sgn && w == 8 && i->sz < 8) ? 8 : 4;
    if (i->cc == 2) {                               /* acquire: ldar (address only in a register) */
        addr_into(S2, &i->m);
        static const char *ac[] = { 0, "ldarb", "ldarh", 0, "ldar" };
        out("%s %s, [x%d]", i->sz == 8 ? "ldar" : ac[(int)i->sz], G(d, i->sz == 8 ? 8 : 4), S2);
        if (i->sgn && i->sz < 4) out("sxt%c %s, %s", i->sz == 1 ? 'b' : 'h', G(d, rw), W(d));
        else if (i->sgn && i->sz == 4 && w == 8) out("sxtw %s, %s", X(d), W(d));
        def_done(i->dst, d, w);
        return;
    }
    out("%s %s, %s", ld_mn(i->sz, i->sgn, w), G(d, rw), amode(&i->m, i->sz));
    def_done(i->dst, d, w);
}

static void store(Ins *i)
{
    int sz = i->sz;
    static const char *st[] = { 0, "strb", "strh", 0, "str", 0, 0, 0, "str" };
    int r;
    if (i->bimm) {
        if (i->imm == 0) r = 31;                    /* wzr / xzr */
        else { mov_imm(S0, i->imm, sz == 8 ? 8 : 4); r = S0; }
    } else if (isflt(i->a)) {
        int f = use_reg(i->a, VS0, sz);
        if (i->cc == 2) { out("fmov %s, %s", G(S0, sz), V(f, sz)); r = S0; }
        else { out("str %s, %s", V(f, sz), amode(&i->m, sz)); return; }
    } else r = use_reg(i->a, S0, sz == 8 ? 8 : 4);
    const char *rn = r == 31 ? (sz == 8 ? "xzr" : "wzr") : G(r, sz == 8 ? 8 : 4);
    if (i->cc == 2) {                               /* release: stlr */
        addr_into(S2, &i->m);
        out("%s %s, [x%d]", sz == 1 ? "stlrb" : sz == 2 ? "stlrh" : "stlr", rn, S2);
        return;
    }
    out("%s %s, %s", st[sz], rn, amode(&i->m, sz));
}

static void fbin(Ins *i, const char *op)
{
    int w = i->w, a = use_reg(i->a, VS0, w), b = use_reg(i->b, VS1, w), d = def_reg(i->dst, VS0);
    out("%s %s, %s, %s", op, V(d, w), V(a, w), V(b, w));
    def_done(i->dst, d, w);
}

/* ---- memory blocks: S0 destination, S1 source, S3 data, S2 count */
static void copy_block(int dreg, int sreg, long n, int zero)
{
    if (dreg != S0) out("mov x%d, %s", S0, X(dreg));
    if (!zero && sreg != S1) out("mov x%d, %s", S1, X(sreg));
    if (n >= 64) {                                  /* a loop of 8-byte moves */
        int l = uniq++;
        mov_imm(S2, n / 8, 8);
        buf_f(o, ".Lu%d:\n", l);
        if (zero) out("str xzr, [x%d], #8", S0);
        else { out("ldr x%d, [x%d], #8", S3, S1); out("str x%d, [x%d], #8", S3, S0); }
        out("subs x%d, x%d, #1", S2, S2);
        out("b.ne .Lu%d", l);
        n %= 8;
    }
    long off = 0;
    for (; n >= 16; n -= 16, off += 16) {            /* 16 bytes at a time: a pair of zeros, or a q register */
        if (zero) out("stp xzr, xzr, [x%d, #%ld]", S0, off);
        else if (!opt_general_regs_only) { out("ldr q%d, [x%d, #%ld]", VS0 - A64_V0, S1, off); out("str q%d, [x%d, #%ld]", VS0 - A64_V0, S0, off); }
        else break;
    }
    static const char *ld[] = { 0, "ldrb", "ldrh", 0, "ldr", 0, 0, 0, "ldr" }, *st[] = { 0, "strb", "strh", 0, "str", 0, 0, 0, "str" };
    for (int k = 8; k >= 1; k /= 2)
        for (; n >= k; n -= k, off += k) {
            if (zero) out("%s %s, [x%d, #%ld]", st[k], k == 8 ? "xzr" : "wzr", S0, off);
            else { out("%s %s, [x%d, #%ld]", ld[k], G(S3, k == 8 ? 8 : 4), S1, off); out("%s %s, [x%d, #%ld]", st[k], G(S3, k == 8 ? 8 : 4), S0, off); }
        }
}

static void copy(Ins *i) { int d = use_reg(i->a, S0, 8); if (d != S0) out("mov x%d, %s", S0, X(d)); copy_block(S0, use_reg(i->b, S1, 8), i->imm, 0); }
static void zero(Ins *i) { copy_block(use_reg(i->a, S0, 8), 0, i->imm, 1); }

/* ---- calls */
static void call(Ins *i)
{
    Call *c = i->call;
    if (c->stack) out("sub sp, sp, #%ld", c->stack);
    for (int k = 0; k < c->nargs; k++) {           /* stack arguments first */
        CallArg *a = &c->args[k];
        if (a->phys >= 0) continue;
        if (a->addr) {
            int s = use_reg(a->addr, S1, 8);
            out("add x%d, sp, #%ld", S0, a->stack_off);
            copy_block(S0, s, a->size, 0);
            continue;
        }
        if (a->cls == C_FLT) out("str %s, [sp, #%ld]", V(use_reg(a->v, VS0, a->w), a->w == 4 ? 4 : 8), a->stack_off);
        else out("str %s, [sp, #%ld]", X(use_reg(a->v, S0, 8)), a->stack_off);
    }
    for (int k = 0; k < c->nargs; k++) {           /* vectors: into their v registers, from memory (before */
        CallArg *a = &c->args[k];                   /* the moves below overwrite the registers of addresses) */
        if (a->phys >= A64_V0 && a->addr) out("ldr %c%d, [x%d]", a->size == 16 ? 'q' : 'd', a->phys - A64_V0, use_reg(a->addr, S1, 8));
    }
    if (!c->sym) { int f = use_reg(c->fn, S2, 8); if (f != S2) out("mov x%d, %s", S2, X(f)); }
    Move mv[24];
    int n = 0;
    for (int k = 0; k < c->nargs && n < 24; k++) {
        CallArg *a = &c->args[k];
        if (a->phys < 0 || a->isimm || a->addr) continue;
        mv[n] = (Move){ a->phys, a->v, -1, 0, a->w, 0, 0 };
        n++;
    }
    par_move(mv, n);
    for (int k = 0; k < c->nargs; k++) if (c->args[k].isimm) mov_imm(c->args[k].phys, c->args[k].imm, 8);
    if (c->sym) out("bl %s", c->sym);
    else out("blr x%d", S2);
    if (c->stack) out("add sp, sp, #%ld", c->stack);
    for (int k = 0; k < c->vret_n; k++) {          /* vectors in v0-v3: into their slot */
        Addr m = { 0 };
        m.slot = c->vret_slot; m.off = (long)k * c->vret_es; m.scale = 1;
        out("str %c%d, %s", c->vret_es == 16 ? 'q' : 'd', k, amode(&m, c->vret_es));
    }
    Move rm[4];                                     /* the results: spilled ones first, then a parallel move */
    int nr = 0;                                     /* (x0 and x1 may go to x1 and x0) */
    for (int k = 0; k < c->nret; k++) {
        int v = c->ret[k], p = c->retphys[k], w = c->retw[k];
        if (isreg(v)) rm[nr++] = (Move){ ph(v), 0, p, 0, w, 0, 0 };
        else if (p >= A64_V0) out("str %s, %s", V(p, w == 4 ? 4 : 8), spill(v));
        else out("str %s, %s", G(p, w == 8 ? 8 : 4), spill(v));
    }
    par_move(rm, nr);
}

static void entry(Ins *i)
{
    if (F->obj->ty->is_variadic) {                 /* the argument registers, for va_arg */
        out("sub x%d, x29, #%ld", S0, -slot_off(F->va_slot));
        for (int k = 0; k < 8; k += 2) out("stp x%d, x%d, [x%d, #%d]", k, k + 1, S0, k * 8);
        if (!opt_general_regs_only) for (int k = 0; k < 8; k++) out("str q%d, [x%d, #%d]", k, S0, 64 + 16 * k);
    }
    for (int k = 0; k < i->npin; k++) {            /* into frame slots and spilled registers */
        ParamIn *p = &i->pin[k];
        if (p->phys < 0) continue;
        if (p->slot) {
            Addr m = { 0 };
            m.slot = p->slot; m.off = p->off; m.scale = 1;
            if (p->cls == C_FLT) out("str %s, %s", V(p->phys, p->w), amode(&m, p->w));
            else out("%s %s, %s", p->w == 1 ? "strb" : p->w == 2 ? "strh" : "str", G(p->phys, p->w == 8 ? 8 : 4), amode(&m, p->w));
        } else if (p->v && !isreg(p->v) && F->vr[p->v].end >= 0) {
            if (p->cls == C_FLT) out("str %s, %s", V(p->phys, p->w), spill(p->v));
            else out("str %s, %s", X(p->phys), spill(p->v));
        }
    }
    Move mv[24];
    int n = 0;
    for (int k = 0; k < i->npin && n < 24; k++) {
        ParamIn *p = &i->pin[k];
        if (p->phys < 0 || p->slot || !p->v || !isreg(p->v)) continue;
        mv[n] = (Move){ ph(p->v), 0, p->phys, 0, p->w, 0, 0 };
        n++;
    }
    par_move(mv, n);
    for (int k = 0; k < i->npin; k++) {            /* from the stack */
        ParamIn *p = &i->pin[k];
        if (p->phys >= 0 || !p->v || F->vr[p->v].end < 0) continue;
        Addr m = { 0 };
        m.args = 1; m.off = p->stack_off; m.scale = 1;
        if (p->cls == C_FLT) { int d = def_reg(p->v, VS0); out("ldr %s, %s", V(d, p->w), amode(&m, p->w)); def_done(p->v, d, p->w); }
        else { int d = def_reg(p->v, S0); out("ldr %s, %s", X(d), amode(&m, 8)); def_done(p->v, d, 8); }
    }
}

/* ---- inline asm: operands substituted; %wN %xN (general registers by
 * width), %sN %dN %qN %bN %hN (floating/SIMD by width), %cN (a constant),
 * %= (a number unique to this asm) */
static void asm_ins(Ins *i)
{
    AsmIns *a = i->asmi;
    if (a->is_basic) { buf_s(o, a->tmpl); buf_c(o, '\n'); return; }
    Move mv[32];
    int n = 0;
    for (int k = 0; k < a->n && n < 32; k++) {
        AsmOperand *op = &a->op[k];
        int dreg = op->kind == 'r' && op->in ? op->phys : op->mreg > 0 ? op->mreg : -1;
        if (dreg < 0 || !op->v) continue;
        mv[n] = (Move){ dreg, op->v, -1, 0, dreg >= A64_V0 ? op->size : 8, 0, 0 };
        n++;
    }
    par_move(mv, n);
    Buf t = { 0 };
    static int asm_id;
    asm_id++;
    for (const char *p = a->tmpl; *p; p++) {
        if (*p != '%') { buf_c(&t, *p); continue; }
        p++;
        if (*p == '%') { buf_c(&t, '%'); continue; }
        if (*p == '=') { buf_f(&t, "%d", asm_id); continue; }
        char mod = 0;
        if (*p && strchr("wxsdqbhcn", *p) && (p[1] == '[' || (p[1] >= '0' && p[1] <= '9'))) mod = *p++;
        int k = -1;
        if (*p == '[') {
            const char *e = strchr(p, ']');
            for (int j = 0; j < a->n; j++)
                if (a->op[j].name && (long)strlen(a->op[j].name) == e - p - 1 && !strncmp(a->op[j].name, p + 1, e - p - 1)) k = j;
            p = e;
        } else if (*p >= '0' && *p <= '9') {
            k = *p - '0';
            if (p[1] >= '0' && p[1] <= '9') k = k * 10 + *++p - '0';
        }
        if (k < 0 || k >= a->n) die("asm: invalid operand in \"%s\"", a->tmpl);
        AsmOperand *op = &a->op[k];
        if (op->kind == 'i') { buf_f(&t, "%ld", mod == 'n' ? -op->imm : op->imm); continue; }
        if (op->kind == 'r') {
            if (op->phys >= A64_V0) {
                char c = mod && strchr("sdqbh", mod) ? mod : op->size == 4 ? 's' : 'd';
                buf_f(&t, "%c%d", c, op->phys - A64_V0);
            } else {
                int w = mod == 'w' ? 4 : mod == 'x' ? 8 : op->size == 8 ? 8 : 4;
                buf_s(&t, op->phys == A64_SP ? (w == 8 ? "sp" : "wsp") : G(op->phys, w));
            }
            continue;
        }
        buf_f(&t, "[%s]", X(op->mreg));
    }
    buf_add(o, t.p ? t.p : "", t.len);
    buf_c(o, '\n');
    free(t.p);
    for (int k = 0; k < a->n; k++) {               /* outputs: spilled ones first, then a parallel move */
        AsmOperand *op = &a->op[k];
        if (!op->out || op->kind != 'r' || !op->outv || isreg(op->outv)) continue;
        if (op->phys >= A64_V0) out("str %s, %s", V(op->phys, op->size == 4 ? 4 : 8), spill(op->outv));
        else out("str %s, %s", X(op->phys), spill(op->outv));
    }
    n = 0;
    for (int k = 0; k < a->n && n < 32; k++) {
        AsmOperand *op = &a->op[k];
        if (!op->out || op->kind != 'r' || !op->outv || !isreg(op->outv)) continue;
        mv[n] = (Move){ ph(op->outv), 0, op->phys, 0, 8, 0, 0 };
        n++;
    }
    par_move(mv, n);
}

/* ---- atomics: load-exclusive / store-exclusive loops (acquire, release:
 * sequentially consistent). S0 the address, S1 the operand, S2 the old
 * value, S3 the new one, w18 the store's status. */
static void atomic_rmw(Ins *i)
{
    int sz = i->sz, w = sz == 8 ? 8 : 4;
    static const char *lx[] = { 0, "ldaxrb", "ldaxrh", 0, "ldaxr", 0, 0, 0, "ldaxr" }, *sx[] = { 0, "stlxrb", "stlxrh", 0, "stlxr", 0, 0, 0, "stlxr" };
    int p = use_reg(i->a, S0, 8);
    if (p != S0) out("mov x%d, %s", S0, X(p));
    int b = use_reg(i->b, S1, w);
    if (b != S1) out("mov %s, %s", G(S1, w), G(b, w));
    if (opt_lse) {                                  /* ARMv8.1: one instruction, acquire and release */
        static const char *sfx[] = { 0, "b", "h", 0, "", 0, 0, 0, "" };
        if (i->op == O_CMPXCHG) {                  /* casal: S3 holds the expected value, then the old one */
            int e = use_reg(i->m.base, S3, w);
            if (e != S3) out("mov %s, %s", G(S3, w), G(e, w));
            out("casal%s %s, %s, [x%d]", sfx[sz], G(S3, w), G(S1, w), S0);
            if (sz < 4) out("%sxt%c w%d, w%d", i->sgn ? "s" : "u", sz == 1 ? 'b' : 'h', S2, S3);
            else out("mov %s, %s", G(S2, w), G(S3, w));
        } else out("%s%s %s, %s, [x%d]", i->op == O_XADD ? "ldaddal" : "swpal", sfx[sz], G(S1, w), G(S2, w), S0);
        int d = def_reg(i->dst, S2);
        if (d != S2) out("mov %s, %s", G(d, i->w), G(S2, i->w));
        def_done(i->dst, d, i->w);
        return;
    }
    int l = uniq++, done = uniq++;
    if (i->op == O_CMPXCHG) {                      /* the expected value, compared at the stored size */
        int e = use_reg(i->m.base, S3, w);
        if (sz == 1) out("and w%d, %s, #255", S3, W(e));
        else if (sz == 2) out("and w%d, %s, #65535", S3, W(e));
        else if (e != S3) out("mov %s, %s", G(S3, w), G(e, w));
    }
    buf_f(o, ".Lu%d:\n", l);
    out("%s %s, [x%d]", lx[sz], G(S2, w), S0);
    if (i->op == O_CMPXCHG) {
        out("cmp %s, %s", G(S2, w), G(S3, w));
        out("b.ne .Lu%d", done);
        out("%s w18, %s, [x%d]", sx[sz], G(S1, w), S0);
    } else if (i->op == O_XADD) {
        out("add %s, %s, %s", G(S3, w), G(S2, w), G(S1, w));
        out("%s w18, %s, [x%d]", sx[sz], G(S3, w), S0);
    } else out("%s w18, %s, [x%d]", sx[sz], G(S1, w), S0);
    out("cbnz w18, .Lu%d", l);
    buf_f(o, ".Lu%d:\n", done);
    if (i->op == O_CMPXCHG && i->sgn && sz < 4) out("sxt%c w%d, w%d", sz == 1 ? 'b' : 'h', S2, S2);
    int d = def_reg(i->dst, S2);
    if (d != S2) out("mov %s, %s", G(d, i->w), G(S2, i->w));
    def_done(i->dst, d, i->w);
}

/* ---- __int128 in memory: S2/S3 the result's halves */
static void i128(Ins *i)
{
    if (i->op == O_I128EXT) {
        int a = use_reg(i->a, S2, 8);
        if (a != S2) out("mov x%d, %s", S2, X(a));
        if (i->sgn) out("asr x%d, x%d, #63", S3, S2); else out("mov x%d, xzr", S3);
    } else if (i->op == O_I128CMP) {
        int swap = i->cc == CC_LE || i->cc == CC_GT || i->cc == CC_ULE || i->cc == CC_UGT;
        int a = use_reg(swap ? i->b : i->a, S0, 8);
        out("ldp x%d, x%d, [%s]", S2, S3, X(a));
        int b = use_reg(swap ? i->a : i->b, S0, 8);
        out("ldp x%d, x%d, [%s]", S0, S1, X(b));
        const char *cc;
        if (i->cc == CC_EQ || i->cc == CC_NE) {
            out("eor x%d, x%d, x%d", S2, S2, S0);
            out("eor x%d, x%d, x%d", S3, S3, S1);
            out("orr x%d, x%d, x%d", S2, S2, S3);
            out("cmp x%d, #0", S2);
            cc = i->cc == CC_EQ ? "eq" : "ne";
        } else {
            out("cmp x%d, x%d", S2, S0);
            out("sbcs xzr, x%d, x%d", S3, S1);
            switch (i->cc) {                       /* (swapped: a <= b is !(b < a)) */
            case CC_LT: case CC_GT: cc = "lt"; break;
            case CC_GE: case CC_LE: cc = "ge"; break;
            case CC_ULT: case CC_UGT: cc = "lo"; break;
            default: cc = "hs";
            }
        }
        int d = def_reg(i->dst, S0);
        out("cset %s, %s", W(d), cc);
        def_done(i->dst, d, 4);
        return;
    } else if (i->op == O_I128SH) {
        int a = use_reg(i->a, S0, 8);
        out("ldp x%d, x%d, [%s]", S2, S3, X(a));
        if (i->bimm) mov_imm(S0, i->imm & 127, 8);
        else { int b = use_reg(i->b, S0, 4); out("and w%d, %s, #127", S0, W(b)); }
        int big = uniq++, done = uniq++;
        out("cbz w%d, .Lu%d", S0, done);
        out("cmp w%d, #64", S0);
        out("b.hs .Lu%d", big);
        out("mov w%d, #64", S1);
        out("sub w%d, w%d, w%d", S1, S1, S0);      /* 64 - n */
        if (i->sz == 0) {                          /* shl */
            out("lsr x%d, x%d, x%d", S4, S2, S1);
            out("lsl x%d, x%d, x%d", S3, S3, S0);
            out("orr x%d, x%d, x%d", S3, S3, S4);
            out("lsl x%d, x%d, x%d", S2, S2, S0);
        } else {
            out("lsl x%d, x%d, x%d", S4, S3, S1);
            out("lsr x%d, x%d, x%d", S2, S2, S0);
            out("orr x%d, x%d, x%d", S2, S2, S4);
            out("%s x%d, x%d, x%d", i->sz == 1 ? "lsr" : "asr", S3, S3, S0);
        }
        out("b .Lu%d", done);
        buf_f(o, ".Lu%d:\n", big);
        out("sub w%d, w%d, #64", S0, S0);
        if (i->sz == 0) { out("lsl x%d, x%d, x%d", S3, S2, S0); out("mov x%d, xzr", S2); }
        else if (i->sz == 1) { out("lsr x%d, x%d, x%d", S2, S3, S0); out("mov x%d, xzr", S3); }
        else { out("asr x%d, x%d, x%d", S2, S3, S0); out("asr x%d, x%d, #63", S3, S3); }
        buf_f(o, ".Lu%d:\n", done);
    } else {
        int a = use_reg(i->a, S0, 8);
        out("ldp x%d, x%d, [%s]", S2, S3, X(a));
        if (i->imm <= 5 && i->imm != 6 && i->imm != 7) {
            int b = use_reg(i->b, S0, 8);
            out("ldp x%d, x%d, [%s]", S0, S1, X(b));
        }
        switch (i->imm) {
        case 0: out("adds x%d, x%d, x%d", S2, S2, S0); out("adc x%d, x%d, x%d", S3, S3, S1); break;
        case 1: out("subs x%d, x%d, x%d", S2, S2, S0); out("sbc x%d, x%d, x%d", S3, S3, S1); break;
        case 2:                                    /* lo*lo (128 bits) + the cross products in the high half */
            out("mul x%d, x%d, x%d", S4, S3, S0);  /* a1*b0 */
            out("mul x%d, x%d, x%d", S3, S2, S1);  /* a0*b1 */
            out("add x%d, x%d, x%d", S3, S3, S4);
            out("umulh x%d, x%d, x%d", S4, S2, S0);
            out("add x%d, x%d, x%d", S3, S3, S4);
            out("mul x%d, x%d, x%d", S2, S2, S0);
            break;
        case 3: out("and x%d, x%d, x%d", S2, S2, S0); out("and x%d, x%d, x%d", S3, S3, S1); break;
        case 4: out("orr x%d, x%d, x%d", S2, S2, S0); out("orr x%d, x%d, x%d", S3, S3, S1); break;
        case 5: out("eor x%d, x%d, x%d", S2, S2, S0); out("eor x%d, x%d, x%d", S3, S3, S1); break;
        case 6: out("negs x%d, x%d", S2, S2); out("ngc x%d, x%d", S3, S3); break;
        default: out("mvn x%d, x%d", S2, S2); out("mvn x%d, x%d", S3, S3);
        }
    }
    int d = use_reg(i->m.base, S0, 8);
    out("stp x%d, x%d, [%s]", S2, S3, X(d));
}

static void label_of(Block *b, char *s) { snprintf(s, 32, ".L%d_%d", fn_id, b->id); }
static void jmp(Block *b) { char l[32]; if (b == nextb) return; label_of(b, l); out("b %s", l); }

static void ret_value(Ins *i)
{
    if (i->sgn == 3) {                              /* vectors (an HVA): v0-v3 loaded from the address a */
        int r = use_reg(i->a, S0, 8);
        for (int k = 0; k < i->imm; k++) out("ldr %c%d, [x%d, #%d]", i->cc == 16 ? 'q' : 'd', k, r, k * i->cc);
        return;
    }
    if (i->sgn == 2) {                              /* several registers (a struct): pin[] */
        Move mv[4];
        for (int k = 0; k < i->npin; k++) mv[k] = (Move){ i->pin[k].phys, i->pin[k].v, -1, 0, i->pin[k].w, 0, 0 };
        par_move(mv, i->npin);
        return;
    }
    if (i->bimm) { mov_imm(0, i->imm, i->w == 8 ? 8 : 4); return; }
    if (!i->a) return;
    if (i->sz == 1) { int r = use_reg(i->a, VS0, i->w); mov_rr(A64_V0, r, i->w); return; }
    int r = use_reg(i->a, 0, i->w);
    if (r != 0) out("mov %s, %s", X(0), X(r));
}

static void bitop(Ins *i)
{
    int w = i->w, d = def_reg(i->dst, S0), a = use_reg(i->a, S1, w);
    switch (i->imm) {
    case 0: out("clz %s, %s", G(d, w), G(a, w)); break;
    case 1: out("rbit %s, %s", G(S3, w), G(a, w)); out("clz %s, %s", G(d, w), G(S3, w)); break;
    case 2: {                                      /* popcount: bit counting in parallel (SWAR) */
        const char *x = G(S1, w), *t = G(S3, w);
        const char *m1 = w == 8 ? "0x5555555555555555" : "0x55555555", *m2 = w == 8 ? "0x3333333333333333" : "0x33333333",
                   *m4 = w == 8 ? "0x0f0f0f0f0f0f0f0f" : "0x0f0f0f0f";
        if (a != S1) out("mov %s, %s", x, G(a, w));
        out("lsr %s, %s, #1", t, x);
        out("and %s, %s, #%s", t, t, m1);
        out("sub %s, %s, %s", x, x, t);
        out("lsr %s, %s, #2", t, x);
        out("and %s, %s, #%s", t, t, m2);
        out("and %s, %s, #%s", x, x, m2);
        out("add %s, %s, %s", x, x, t);
        out("add %s, %s, %s, lsr #4", x, x, x);
        out("and %s, %s, #%s", x, x, m4);
        mov_imm(S3, 0x0101010101010101L, w);
        out("mul %s, %s, %s", x, x, t);
        out("lsr %s, %s, #%d", G(d, w), x, w * 8 - 8);
        break;
    }
    default: out("rev %s, %s", G(d, w), G(a, w));
    }
    def_done(i->dst, d, w > 4 ? w : 4);
}


/* ---- Vectors: values in memory (their address in a register); one NEON
 * instruction per 16 bytes, in v0-v4. No value lives in the argument
 * registers v0-v7 between instructions, so they are free here. */
static const char *arr(int es, int n)            /* the arrangement: 16b 8h 4s 2d, 8b 4h 2s 1d */
{
    static const char *t16[] = { 0, "16b", "8h", 0, "4s", 0, 0, 0, "2d" }, *t8[] = { 0, "8b", "4h", 0, "2s", 0, 0, 0, "1d" };
    return n == 16 ? t16[es] : t8[es];
}
static char elc(int es) { return es == 1 ? 'b' : es == 2 ? 'h' : es == 4 ? 's' : 'd'; }

/* a generic vector operation (operators, the portable part of __builtin_vec): one instruction? */
int a64_vec_ok(int op, int kind, int es)
{
    int f = kind == 2;
    switch (op) {
    case VOP_ADD: case VOP_SUB: case VOP_AND: case VOP_OR: case VOP_XOR: case VOP_NEG: case VOP_NOT: case VOP_ANDNOT:
    case VOP_EQ: case VOP_NE: case VOP_LT: case VOP_LE: case VOP_GT: case VOP_GE: case VOP_UNPCKLO: case VOP_UNPCKHI:
        return 1;
    case VOP_ABS: return f || es <= 8;
    case VOP_MUL: case VOP_MIN: case VOP_MAX: return f || es <= 4;
    case VOP_DIV: case VOP_SQRT: return f;
    case VOP_SHLI: case VOP_SHRI: case VOP_SARI: case VOP_SHL: case VOP_SHR: return !f;
    case VOP_CVTI2F: case VOP_CVTF2I: case VOP_CVTF2IR: return es == 4 || es == 8;
    case VOP_AVG: case VOP_ADDS: case VOP_SUBS: return !f;
    }
    return 0;
}

static void vld(int v, int base, int off, int n) { out("ldr %c%d, [x%d, #%d]", n == 16 ? 'q' : 'd', v, base, off); }
static void vst(int v, int base, int off, int n) { out("str %c%d, [x%d, #%d]", n == 16 ? 'q' : 'd', v, base, off); }

/* the sum of all elements of a, into a scalar register (VOP_ADDV) */
static void vec_addv(Ins *i, int pa)
{
    int es = i->sz, n = i->w, kind = i->sgn;
    vld(1, pa, 0, n);
    if (kind == 2) {                                  /* floating: pairwise adds down to one */
        if (es == 4 && n == 16) { out("faddp v0.4s, v1.4s, v1.4s"); out("faddp s0, v0.2s"); }
        else if (es == 4) out("faddp s0, v1.2s");
        else if (n == 16) out("faddp d0, v1.2d");
        else out("fmov d0, d1");
        int d = def_reg(i->dst, VS0);
        mov_rr(d, A64_V0, 8);
        def_done(i->dst, d, es);
        return;
    }
    if (es == 8) { if (n == 16) out("addp d0, v1.2d"); else out("fmov d0, d1"); }
    else if (es == 4 && n == 8) out("addp v0.2s, v1.2s, v1.2s");
    else out("addv %c0, v1.%s", elc(es), arr(es, n));
    int d = def_reg(i->dst, S3);
    if (es == 8) out("fmov %s, d0", X(d));
    else if (es == 4) out("fmov %s, s0", W(d));
    else out("%cmov %s, v0.%c[0]", kind ? 'u' : 's', W(d), elc(es));
    def_done(i->dst, d, es == 8 ? 8 : 4);
}

static void vec(Ins *i)
{
    int op = i->cc, es = i->sz, kind = i->sgn, n = i->w, f = kind == 2, u = kind == 1;
    if (op == VOP_SPLAT) {                            /* a scalar (a: its register) in every element */
        int pd = use_reg(i->m.base, S2, 8), h = n < 16 ? n : 16;
        if (isflt(i->a)) out("dup v0.%s, v%d.%c[0]", arr(es, h), use_reg(i->a, VS0, es) - A64_V0, elc(es));
        else { int r = use_reg(i->a, S0, es == 8 ? 8 : 4); out("dup v0.%s, %s", arr(es, h), es == 8 ? X(r) : W(r)); }
        for (int off = 0; off < n; off += 16) vst(0, pd, off, h);
        return;
    }
    int pa = use_reg(i->a, S0, 8), pb = i->b ? use_reg(i->b, S1, 8) : -1;
    if (op == VOP_ADDV) { vec_addv(i, pa); return; }
    int pd = use_reg(i->m.base, S2, 8);
    char s = u ? 'u' : 's';
    for (int off = 0; off < n; off += 16) {
        int h = n < 16 ? n : 16, rn = h;          /* bytes per instruction; rn: of the result */
        const char *T = arr(es, h), *B = h == 16 ? "16b" : "8b";
        vld(1, pa, off, h);
        if (pb >= 0) vld(2, pb, off, i->npin && i->npin < h ? (int)i->npin : h);   /* (npin: b's size, if smaller) */
        switch (op) {
        /* the generic operations */
        case VOP_ADD: out("%s v0.%s, v1.%s, v2.%s", f ? "fadd" : "add", T, T, T); break;
        case VOP_SUB: out("%s v0.%s, v1.%s, v2.%s", f ? "fsub" : "sub", T, T, T); break;
        case VOP_MUL: out("%s v0.%s, v1.%s, v2.%s", f ? "fmul" : "mul", T, T, T); break;
        case VOP_DIV: out("fdiv v0.%s, v1.%s, v2.%s", T, T, T); break;
        case VOP_AND: out("and v0.%s, v1.%s, v2.%s", B, B, B); break;
        case VOP_OR: out("orr v0.%s, v1.%s, v2.%s", B, B, B); break;
        case VOP_XOR: out("eor v0.%s, v1.%s, v2.%s", B, B, B); break;
        case VOP_ANDNOT: out("bic v0.%s, v2.%s, v1.%s", B, B, B); break;      /* ~a & b */
        case VOP_NEG: out("%s v0.%s, v1.%s", f ? "fneg" : "neg", T, T); break;
        case VOP_NOT: out("not v0.%s, v1.%s", B, B); break;
        case VOP_ABS: out("%s v0.%s, v1.%s", f ? "fabs" : "abs", T, T); break;
        case VOP_SQRT: out("fsqrt v0.%s, v1.%s", T, T); break;
        case VOP_MIN: case VOP_MAX:
            if (f) out("%s v0.%s, v1.%s, v2.%s", op == VOP_MIN ? "fmin" : "fmax", T, T, T);
            else out("%c%s v0.%s, v1.%s, v2.%s", s, op == VOP_MIN ? "min" : "max", T, T, T);
            break;
        case VOP_EQ: case VOP_NE:
            out("%s v0.%s, v1.%s, v2.%s", f ? "fcmeq" : "cmeq", T, T, T);
            if (op == VOP_NE) out("not v0.%s, v0.%s", B, B);
            break;
        case VOP_GT: case VOP_GE: case VOP_LT: case VOP_LE: {       /* (lt, le: gt, ge with the operands swapped) */
            int ge = op == VOP_GE || op == VOP_LE, sw = op == VOP_LT || op == VOP_LE;
            const char *m = f ? (ge ? "fcmge" : "fcmgt") : u ? (ge ? "cmhs" : "cmhi") : (ge ? "cmge" : "cmgt");
            out("%s v0.%s, v%d.%s, v%d.%s", m, T, sw ? 2 : 1, T, sw ? 1 : 2, T);
            break;
        }
        case VOP_SHLI: case VOP_SHRI: case VOP_SARI: {
            long k = i->imm, bits = es * 8;
            if (k == 0) out("orr v0.%s, v1.%s, v1.%s", B, B, B);
            else if (k >= bits && op != VOP_SARI) out("movi v0.%s, #0", B);
            else if (op == VOP_SHLI) out("shl v0.%s, v1.%s, #%ld", T, T, k);
            else out("%cshr v0.%s, v1.%s, #%ld", op == VOP_SARI ? 's' : 'u', T, T, k >= bits ? bits : k);
            break;
        }
        case VOP_SHL: out("%cshl v0.%s, v1.%s, v2.%s", s, T, T, T); break;  /* (a negative count shifts right) */
        case VOP_SHR: out("neg v3.%s, v2.%s", T, T); out("%cshl v0.%s, v1.%s, v3.%s", s, T, T, T); break;
        case VOP_CVTI2F: out("%ccvtf v0.%s, v1.%s", s, T, T); break;
        case VOP_CVTF2I: out("fcvtz%c v0.%s, v1.%s", i->imm ? 'u' : 's', T, T); break;   /* (imm 1: to unsigned) */
        case VOP_CVTF2IR: out("fcvtn%c v0.%s, v1.%s", i->imm ? 'u' : 's', T, T); break;
        case VOP_UNPCKLO: out("zip1 v0.%s, v1.%s, v2.%s", T, T, T); break;
        case VOP_UNPCKHI: out("zip2 v0.%s, v1.%s, v2.%s", T, T, T); break;
        case VOP_AVG: out("%crhadd v0.%s, v1.%s, v2.%s", s, T, T, T); break;
        case VOP_ADDS: out("%cqadd v0.%s, v1.%s, v2.%s", s, T, T, T); break;
        case VOP_SUBS: out("%cqsub v0.%s, v1.%s, v2.%s", s, T, T, T); break;
        /* NEON (arm_neon.h) */
        case VOP_FMA: case VOP_FMS: case VOP_MLA: case VOP_MLS:
            vld(0, pd, off, h);
            out("%s v0.%s, v1.%s, v2.%s", op == VOP_FMA ? "fmla" : op == VOP_FMS ? "fmls" : op == VOP_MLA ? "mla" : "mls", T, T, T);
            break;
        case VOP_DOT: case VOP_DOTL:
            vld(0, pd, off, h);
            if (op == VOP_DOT) out("%cdot v0.%s, v1.%s, v2.%s", s, arr(4, h), B, B);
            else out("%cdot v0.%s, v1.%s, v2.4b[%ld]", s, arr(4, h), B, i->imm);
            break;
        case VOP_MULL: case VOP_MLAL:                 /* the low halves (or 8-byte vectors), widened */
            if (op == VOP_MLAL) vld(0, pd, 0, 16);
            out("%c%s v0.%s, v1.%s, v2.%s", s, op == VOP_MULL ? "mull" : "mlal", arr(es * 2, 16), arr(es, 8), arr(es, 8));
            rn = 16;
            break;
        case VOP_MULL2: case VOP_MLAL2:
            if (op == VOP_MLAL2) vld(0, pd, 0, 16);
            out("%c%s v0.%s, v1.%s, v2.%s", s, op == VOP_MULL2 ? "mull2" : "mlal2", arr(es * 2, 16), T, T);
            break;
        case VOP_PADDL: out("%caddlp v0.%s, v1.%s", s, arr(es * 2, h), T); break;
        case VOP_PADAL: vld(0, pd, off, h); out("%cadalp v0.%s, v1.%s", s, arr(es * 2, h), T); break;
        case VOP_ADDP: out("%s v0.%s, v1.%s, v2.%s", f ? "faddp" : "addp", T, T, T); break;
        case VOP_MOVL: out("%cxtl v0.%s, v1.%s", s, arr(es * 2, 16), arr(es, 8)); rn = 16; break;
        case VOP_MOVL2: out("%cxtl2 v0.%s, v1.%s", s, arr(es * 2, 16), T); break;
        case VOP_XTN: out("xtn v0.%s, v1.%s", arr(es / 2, 8), T); rn = 8; break;
        case VOP_QXTN: out("%cqxtn v0.%s, v1.%s", s, arr(es / 2, 8), T); rn = 8; break;
        case VOP_QXTUN: out("sqxtun v0.%s, v1.%s", arr(es / 2, 8), T); rn = 8; break;
        case VOP_UZP1: case VOP_UZP2: case VOP_TRN1: case VOP_TRN2: {
            static const char *m[] = { "uzp1", "uzp2", "trn1", "trn2" };
            out("%s v0.%s, v1.%s, v2.%s", m[op - VOP_UZP1], T, T, T);
            break;
        }
        case VOP_EXT: out("ext v0.%s, v1.%s, v2.%s, #%ld", B, B, B, i->imm); break;
        case VOP_DUPL: out("dup v0.%s, v1.%c[%ld]", T, elc(es), i->imm); break;
        case VOP_TBL: rn = i->npin; out("tbl v0.%s, {v1.16b}, v2.%s", rn == 16 ? "16b" : "8b", rn == 16 ? "16b" : "8b"); break;
        case VOP_CNT: out("cnt v0.%s, v1.%s", B, B); break;
        case VOP_BSL: vld(0, pd, off, h); out("bsl v0.%s, v1.%s, v2.%s", B, B, B); break;
        case VOP_FCVTL: out("fcvtl v0.4s, v1.4h"); rn = 16; break;
        case VOP_FCVTL2: out("fcvtl2 v0.4s, v1.8h"); break;
        case VOP_FCVTN: out("fcvtn v0.4h, v1.4s"); rn = 8; break;
        default: die("a vector operation without an AArch64 instruction (%d)", op);
        }
        vst(0, pd, off, rn);
        if (n <= 16) break;
    }
}

static void ins(Ins *i)
{
    char l1[32], l2[32];
    switch (i->op) {
    case O_NOP: return;
    case O_MOV:
        if (isflt(i->dst)) {
            int d = def_reg(i->dst, VS0);
            if (i->bimm) {
                if (i->imm == 0) out("fmov %s, %s", V(d, i->w), i->w == 4 ? "wzr" : "xzr");
                else { mov_imm(S0, i->imm, i->w == 4 ? 4 : 8); out("fmov %s, %s", V(d, i->w), G(S0, i->w == 4 ? 4 : 8)); }
            } else { int a = use_reg(i->a, VS0, i->w); if (a != d) out("fmov %s, %s", V(d, 8), V(a, 8)); }
            def_done(i->dst, d, i->w);
            return;
        } else {
            int d = def_reg(i->dst, S0);
            if (i->bimm) mov_imm(d, i->imm, i->w == 8 ? 8 : 4);
            else { int a = use_reg(i->a, S0, i->w); if (a != d) out("mov %s, %s", X(d), X(a)); }
            def_done(i->dst, d, i->w);
            return;
        }
    case O_ADD: binop(i, "add", 0); return;
    case O_SUB: binop(i, "sub", 0); return;
    case O_AND: binop(i, "and", 1); return;
    case O_OR: binop(i, "orr", 1); return;
    case O_XOR: binop(i, "eor", 1); return;
    case O_MUL: {
        int w = i->w, d = def_reg(i->dst, S0), a = use_reg(i->a, S0, w), b;
        if (i->bimm) { mov_imm(S1, i->imm, w); b = S1; } else b = use_reg(i->b, S1, w);
        out("mul %s, %s, %s", G(d, w), G(a, w), G(b, w));
        def_done(i->dst, d, w);
        return;
    }
    case O_SHL: shift(i, "lsl"); return;
    case O_SHR: shift(i, "lsr"); return;
    case O_SAR: shift(i, "asr"); return;
    case O_ROL: shift(i, "ror"); return;
    case O_DIV: case O_UDIV: case O_MOD: case O_UMOD: divide(i); return;
    case O_NEG: case O_NOT: {
        int w = i->w, d = def_reg(i->dst, S0), a = use_reg(i->a, S0, w);
        out("%s %s, %s", i->op == O_NEG ? "neg" : "mvn", G(d, w), G(a, w));
        def_done(i->dst, d, w);
        return;
    }
    case O_EXT: extend(i); return;
    case O_SET: {
        compare(i);
        int d = def_reg(i->dst, S0);
        out("cset %s, %s", W(d), cc_name(i->cc));
        def_done(i->dst, d, 4);
        return;
    }
    case O_LOAD: load(i); return;
    case O_STORE: store(i); return;
    case O_LEA: { int d = def_reg(i->dst, S0); addr_into(d, &i->m); def_done(i->dst, d, 8); return; }
    case O_FADD: fbin(i, "fadd"); return;
    case O_FSUB: fbin(i, "fsub"); return;
    case O_FMUL: fbin(i, "fmul"); return;
    case O_FDIV: fbin(i, "fdiv"); return;
    case O_FNEG: case O_FSQRT: {
        int w = i->w, a = use_reg(i->a, VS0, w), d = def_reg(i->dst, VS0);
        out("%s %s, %s", i->op == O_FNEG ? "fneg" : "fsqrt", V(d, w), V(a, w));
        def_done(i->dst, d, w);
        return;
    }
    case O_I2F: {
        int a = use_reg(i->a, S0, i->sz == 8 ? 8 : 4), d = def_reg(i->dst, VS0);
        out("%s %s, %s", i->sgn ? "scvtf" : "ucvtf", V(d, i->w), G(a, i->sz == 8 ? 8 : 4));
        def_done(i->dst, d, i->w);
        return;
    }
    case O_F2I: {
        int a = use_reg(i->a, VS0, i->sz), d = def_reg(i->dst, S0);
        out("%s %s, %s", i->sgn ? "fcvtzs" : "fcvtzu", G(d, i->w == 8 ? 8 : 4), V(a, i->sz));
        def_done(i->dst, d, i->w);
        return;
    }
    case O_F2F: {
        int a = use_reg(i->a, VS0, i->sz), d = def_reg(i->dst, VS0);
        out("fcvt %s, %s", V(d, i->w), V(a, i->sz));
        def_done(i->dst, d, i->w);
        return;
    }
    case O_CALL: call(i); return;
    case O_BR: {
        label_of(i->t, l1);
        label_of(i->f, l2);
        int cc = i->cc;
        if (cc < CC_FEQ && i->bimm && i->imm == 0 && (cc == CC_EQ || cc == CC_NE)) {   /* compare with 0: cbz, cbnz */
            int a = use_reg(i->a, S0, i->w);
            if (i->t == nextb) out("%s %s, %s", cc == CC_EQ ? "cbnz" : "cbz", G(a, i->w), l2);
            else { out("%s %s, %s", cc == CC_EQ ? "cbz" : "cbnz", G(a, i->w), l1); jmp(i->f); }
            return;
        }
        compare(i);
        if (i->t == nextb && cc < CC_FEQ) { out("b.%s %s", cc_name(cc_invert(cc)), l2); return; }
        out("b.%s %s", cc_name(cc), l1);
        jmp(i->f);
        return;
    }
    case O_JMP: jmp(i->t); return;
    case O_RET:
        ret_value(i);
        if (nextb) out("b .Lret%d", fn_id);
        return;
    case O_SWITCH: {
        int r = use_reg(i->a, S0, 8);
        if (i->cc) { out("br %s", X(r)); return; }  /* goto *p */
        int t = uniq++;
        out("adr x%d, .Ltab%d", S2, t);
        out("ldrsw x%d, [x%d, %s, lsl #2]", S3, S2, X(r));
        out("add x%d, x%d, x%d", S2, S2, S3);
        out("br x%d", S2);
        buf_f(&tables, "\t.p2align 2\n.Ltab%d:\n", t);
        for (int k = 0; k < i->ntab; k++) { label_of(i->tab[k], l1); buf_f(&tables, "\t.word %s-.Ltab%d\n", l1, t); }
        return;
    }
    case O_COPY: copy(i); return;
    case O_ZERO: zero(i); return;
    case O_ASM: asm_ins(i); return;
    case O_XADD: case O_XCHG: case O_CMPXCHG: atomic_rmw(i); return;
    case O_FENCE: out("dmb ish"); return;
    case O_ALLOCA: {                              /* sp -= the size, rounded up to 16 */
        int a = use_reg(i->a, S0, 8);
        out("add x%d, %s, #15", S0, X(a));
        out("and x%d, x%d, #-16", S0, S0);
        out("sub sp, sp, x%d", S0);
        int d = def_reg(i->dst, S0);
        out("mov %s, sp", X(d));
        def_done(i->dst, d, 8);
        return;
    }
    case O_SPSAVE: { int d = def_reg(i->dst, S0); out("mov %s, sp", X(d)); def_done(i->dst, d, 8); return; }
    case O_SPRESTORE: out("mov sp, %s", X(use_reg(i->a, S0, 8))); return;
    case O_TLSADDR: {                             /* local-exec (a static program: also for another module's variable) */
        int d = def_reg(i->dst, S0);
        out("mrs %s, tpidr_el0", X(d));
        out("add %s, %s, #:tprel_hi12:%s, lsl #12", X(d), X(d), i->m.sym);
        out("add %s, %s, #:tprel_lo12_nc:%s", X(d), X(d), i->m.sym);
        def_done(i->dst, d, 8);
        return;
    }
    case O_BITS: {                                /* fmov between a general and a floating-point register */
        int w = i->w;
        if (isflt(i->dst)) { int a = use_reg(i->a, S0, w), d = def_reg(i->dst, VS0); out("fmov %s, %s", V(d, w), G(a, w)); def_done(i->dst, d, w); }
        else { int a = use_reg(i->a, VS0, w), d = def_reg(i->dst, S0); out("fmov %s, %s", G(d, w), V(a, w)); def_done(i->dst, d, w); }
        return;
    }
    case O_VEC: vec(i); return;
    case O_I128: case O_I128SH: case O_I128CMP: case O_I128EXT: i128(i); return;
    case O_ENTRY: entry(i); return;
    case O_UNREACH: out("brk #1000"); return;
    case O_BITOP: bitop(i); return;
    case O_MULSHR: {                              /* (128-bit a * b) >> imm */
        int a = use_reg(i->a, S0, 8), b = use_reg(i->b, S1, 8), d = def_reg(i->dst, S2);
        long k = i->imm;
        out("%s x%d, %s, %s", i->sgn ? "smulh" : "umulh", S3, X(a), X(b));
        if (k >= 64) { if (k > 64) out("%s %s, x%d, #%ld", i->sgn ? "asr" : "lsr", X(d), S3, k - 64); else out("mov %s, x%d", X(d), S3); }
        else if (k) { out("mul x%d, %s, %s", S4, X(a), X(b)); out("extr %s, x%d, x%d, #%ld", X(d), S3, S4, k); }
        else out("mul %s, %s, %s", X(d), X(a), X(b));
        def_done(i->dst, d, 8);
        return;
    }
    case O_FRAMEADDR: {
        int d = def_reg(i->dst, S0);
        if (i->imm) out("ldr %s, [x29, #8]", X(d)); else out("mov %s, x29", X(d));
        def_done(i->dst, d, 8);
        return;
    }
    }
    die("internal: IR op %d on AArch64", i->op);
}

/* ---- a function: saved registers (x19-x28, d8-d15), slots below them */
void emit_function_a64(Buf *dst)
{
    o = dst;
    Obj *fn = F->obj;
    static int ids;
    fn_id = ids++;
    int saved[20], nsaved = 0;
    for (int r = 19; r <= 28; r++) if (F->used_regs & BIT(r)) saved[nsaved++] = r;
    for (int r = A64_V0 + 8; r <= A64_V0 + 15; r++) if (F->used_regs & BIT(r)) saved[nsaved++] = r;
    long size = nsaved * 8;
    int uses_frame = 0;
    for (int s = 1; s <= F->nslots; s++) {
        Slot *sl = &F->slots[s];
        size = align_to(size + sl->size, sl->align);
        sl->off = -size;
        uses_frame = 1;
    }
    long total = align_to(size, 16);
    F->frame = total;
    int needs = uses_frame || nsaved || F->has_calls || fn->ty->is_variadic || !opt_optimize;
    for (Block *b = F->blocks; b && !needs; b = b->next)
        for (Ins *i = b->first; i; i = i->next) {
            if (i->op == O_FRAMEADDR || i->op == O_ASM || i->op == O_ALLOCA || i->op == O_SPSAVE || i->op == O_CALL) needs = 1;
            if ((i->op == O_LOAD || i->op == O_STORE || i->op == O_LEA) && i->m.args) needs = 1;
            if (i->op == O_ENTRY) for (int k = 0; k < i->npin; k++) if (i->pin[k].phys < 0) needs = 1;
        }
    for (int v = 1; v <= F->nvr && !needs; v++) if (F->vr[v].phys < 0 && F->vr[v].end >= 0) needs = 1;
    noframe = !needs;
    const char *name = fn->asm_name;
    buf_f(o, "\t.section %s\n", fn->section ? fn->section : ".text");
    buf_s(o, opt_optimize == 2 ? "\t.p2align 2\n" : "\t.p2align 4\n");
    if (!fn->is_static) buf_f(o, "\t.globl %s\n", name);
    buf_f(o, "\t.type %s, @function\n%s:\n", name, name);
    int src = src_pos(fn->tok);
    if (src) buf_f(o, "\t.loc %d %d\n", src >> 20, src & 0xFFFFF);
    if (!noframe) {
        out("stp x29, x30, [sp, #-16]!");
        out("mov x29, sp");
        if (total) {
            if (add_imm_ok(total)) out("sub sp, sp, #%ld", total);
            else { mov_imm(S0, total, 8); out("sub sp, sp, x%d", S0); }
        }
        for (int k = 0; k < nsaved; k++) {
            int r = saved[k];
            if (r >= A64_V0) out("str d%d, [x29, #%d]", r - A64_V0, -8 * (k + 1));
            else out("str x%d, [x29, #%d]", r, -8 * (k + 1));
        }
    }
    tables.len = 0;
    char l[32];
    for (Block *b = F->blocks; b; b = b->next) {
        nextb = b->next;
        label_of(b, l);
        buf_f(o, "%s:\n", l);
        if (b->sym) buf_f(o, "%s:\n", b->sym);
        for (Ins *i = b->first; i; i = i->next) {
            if (i->src && i->src != src) { src = i->src; buf_f(o, "\t.loc %d %d\n", src >> 20, src & 0xFFFFF); }
            ins(i);
        }
    }
    buf_f(o, ".Lret%d:\n", fn_id);
    if (!noframe) {
        for (int k = 0; k < nsaved; k++) {
            int r = saved[k];
            if (r >= A64_V0) out("ldr d%d, [x29, #%d]", r - A64_V0, -8 * (k + 1));
            else out("ldr x%d, [x29, #%d]", r, -8 * (k + 1));
        }
        out("mov sp, x29");
        out("ldp x29, x30, [sp], #16");
    }
    out("ret");
    if (opt_debug) buf_f(o, ".Lfe%d:\n", fn_id);
    buf_f(o, "\t.size %s, .-%s\n", name, name);
    if (tables.len) buf_add(o, tables.p, tables.len);   /* jump tables: in the code, next to it */
}

int a64_fn_id(void) { return fn_id; }
