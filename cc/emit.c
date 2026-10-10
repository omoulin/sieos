/*
 * emit.c - IR to x86-64 assembly text (AT&T syntax), and the program's data.
 *
 * Each IR instruction becomes a few machine instructions. A virtual
 * register lives in a register or, when spilled, in a stack slot; x86
 * allows one memory operand per instruction, so the scratch registers
 * (rax, rcx, rdx, r11, xmm0, xmm1) carry the rest.
 *
 * The stack frame:   [ return address ]
 *                    [ saved %rbp      ]  <- %rbp
 *                    [ saved rbx, r12-r15 (those used) ]
 *                    [ stack slots: locals, spills ]   <- %rsp (16-aligned)
 * A small leaf function that needs none of it gets no frame at all.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ir.h"

static Buf *o;
static int nsaved, noframe, fn_id, uniq;
static Vec dbg_funcs;                           /* -g: pairs: function, its fn_id */
static Block *nextb;                    /* the block after the current one */
static Buf tables;                      /* jump tables, emitted after the function */

static const char *r64[] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
static const char *r32[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d" };
static const char *r16[] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w" };
static const char *r8n[] = { "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b" };

const char *reg_name(int p, int size)
{
    static char x[8][16];
    static int k;
    if (p >= XMM0) { char *s = x[k++ & 7]; snprintf(s, 16, "xmm%d", (p - XMM0) & 15); return s; }
    return size == 8 ? r64[p] : size == 4 ? r32[p] : size == 2 ? r16[p] : r8n[p];
}

/* Small strings for operands: a ring of buffers, reused. */
static char *tmp(void)
{
    static char ring[16][96];
    static int k;
    return ring[k++ & 15];
}

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

static char sfx(int size) { return size == 1 ? 'b' : size == 2 ? 'w' : size == 4 ? 'l' : 'q'; }

static long slot_off(int slot) { return F->slots[slot].off; }

static int isreg(int v) { return F->vr[v].phys >= 0; }
static int ph(int v) { return F->vr[v].phys; }

/* where virtual register v is, as an operand of the given size */
static const char *loc(int v, int size)
{
    if (!v) die("internal: no virtual register");
    VReg *r = &F->vr[v];
    char *s = tmp();
    if (r->phys >= 0) snprintf(s, 96, "%%%s", reg_name(r->phys, size));
    else snprintf(s, 96, "%ld(%%rbp)", slot_off(r->slot));
    return s;
}

static const char *R(int p, int size) { char *s = tmp(); snprintf(s, 96, "%%%s", reg_name(p, size)); return s; }

/* An address operand; spilled base/index registers are loaded into the
 * given scratch registers. */
static const char *addr(Addr *m, int sbase, int sindex)
{
    char *s = tmp();
    char base[16] = "", index[24] = "";
    int bspill = 0, ispill = 0;
    if (m->base) {
        if (isreg(m->base)) snprintf(base, 16, "%%%s", r64[ph(m->base)]);
        else { out("movq %s, %%%s", loc(m->base, 8), r64[sbase]); snprintf(base, 16, "%%%s", r64[sbase]); bspill = 1; }
    }
    if (m->index) {
        if (isreg(m->index)) snprintf(index, 24, ",%%%s,%d", r64[ph(m->index)], m->scale);
        else { out("movq %s, %%%s", loc(m->index, 8), r64[sindex]); snprintf(index, 24, ",%%%s,%d", r64[sindex], m->scale); ispill = 1; }
    }
    /* -fpic: no absolute sym(base,index) (a 32-bit address, wrong once the
     * program is moved): the symbol's address comes from %rip into the
     * scratch register sbase, which then serves as the base. */
    if (opt_pic && m->sym && !m->tls && (base[0] || index[0])) {
        const char *sb = r64[sbase], *si = r64[sindex];
        char sa[80];
        if (m->off) snprintf(sa, 80, "%s%+ld(%%rip)", m->sym, m->off); else snprintf(sa, 80, "%s(%%rip)", m->sym);
        if (!m->base) out("leaq %s, %%%s", sa, sb);                    /* only an index: sym is the base */
        else if (!bspill) { out("leaq %s, %%%s", sa, sb); out("addq %s, %%%s", base, sb); }
        else if (!ispill) { out("leaq %s, %%%s", sa, si); out("addq %%%s, %%%s", si, sb); }
        else {                                                          /* both spilled: fold the index first */
            out("leaq (%%%s%s), %%%s", sb, index, sb);
            out("leaq %s, %%%s", sa, si); out("addq %%%s, %%%s", si, sb);
            index[0] = 0;
        }
        snprintf(s, 96, "(%%%s%s)", sb, index);
        return s;
    }
    if (m->sym && m->tls) {                     /* %fs:sym@tpoff+off(base,index) */
        int n = m->off ? snprintf(s, 96, "%%fs:%s@tpoff%+ld", m->sym, m->off) : snprintf(s, 96, "%%fs:%s@tpoff", m->sym);
        if (base[0] || index[0]) snprintf(s + n, 96 - n, "(%s%s)", base, index);
        return s;
    }
    if (m->sym && (base[0] || index[0])) {     /* absolute: sym+off(base,index) */
        int n = m->off ? snprintf(s, 96, "%s%+ld", m->sym, m->off) : snprintf(s, 96, "%s", m->sym);
        snprintf(s + n, 96 - n, "(%s%s)", base, index);
        return s;
    }
    if (m->sym) {
        if (m->off) snprintf(s, 96, "%s%+ld(%%rip)", m->sym, m->off);
        else snprintf(s, 96, "%s(%%rip)", m->sym);
        return s;
    }
    long off = m->off;
    if (m->slot) { off += slot_off(m->slot); snprintf(base, 16, "%%rbp"); }
    if (m->args) { off += 16; snprintf(base, 16, "%%rbp"); }
    if (base[0] || index[0]) {
        if (off) snprintf(s, 96, "%ld(%s%s)", off, base, index);
        else snprintf(s, 96, "(%s%s)", base, index);
    } else snprintf(s, 96, "%ld", off);
    return s;
}

/* ---- Moves */
static int fits32(long v) { return v == (long)(int)v; }

static void mov_imm_to(long imm, const char *dst, int w, int dst_is_reg, int dreg)
{
    if (w == 8 && !fits32(imm)) {
        if (dst_is_reg) out("movabsq $%ld, %%%s", imm, r64[dreg]);
        else { out("movabsq $%ld, %%rax", imm); out("movq %%rax, %s", dst); }
        return;
    }
    if (imm == 0 && dst_is_reg) { out("xorl %%%s, %%%s", r32[dreg], r32[dreg]); return; }
    if (w == 8 && dst_is_reg && imm >= 0 && imm <= 0xffffffffL) { out("movl $%ld, %%%s", imm, r32[dreg]); return; }
    out("mov%c $%ld, %s", sfx(w), w == 4 ? (long)(int)imm : imm, dst);
}

/* integer: virtual register d = virtual register s */
static void movv(int d, int s, int w)
{
    if (d == s) return;
    if (isreg(d) && isreg(s) && ph(d) == ph(s)) return;
    if (!isreg(d) && !isreg(s)) {
        out("mov%c %s, %%%s", sfx(w), loc(s, w), reg_name(RAX, w));
        out("mov%c %%%s, %s", sfx(w), reg_name(RAX, w), loc(d, w));
        return;
    }
    out("mov%c %s, %s", sfx(w), loc(s, w), loc(d, w));
}

static const char *fs(int w) { return w == 4 ? "ss" : "sd"; }

/* float: virtual register d = virtual register s */
static void movf(int d, int s, int w)
{
    if (d == s) return;
    if (isreg(d) && isreg(s)) { if (ph(d) != ph(s)) out("movaps %s, %s", loc(s, 8), loc(d, 8)); return; }
    if (!isreg(d) && !isreg(s)) {
        out("mov%c %s, %%%s", sfx(w), loc(s, w), reg_name(RAX, w));
        out("mov%c %%%s, %s", sfx(w), reg_name(RAX, w), loc(d, w));
        return;
    }
    out("mov%s %s, %s", fs(w), loc(s, w), loc(d, w));
}

/* float register d = v's value (a register copy with movaps: movss/movsd
 * between registers keep the top of d, a false dependency on its old value) */
static void fget(int v, int w, int d)
{
    if (isreg(v)) { if (ph(v) != d) out("movaps %%%s, %%%s", reg_name(ph(v), 8), reg_name(d, 8)); }
    else out("mov%s %s, %%%s", fs(w), loc(v, w), reg_name(d, 8));
}

/* a register (phys) for v's value: its own, or the scratch one loaded */
static int use_reg(int v, int scratch, int w)
{
    if (isreg(v)) return ph(v);
    if (F->vr[v].cls == C_FLT) out("mov%s %s, %%%s", fs(w), loc(v, w), reg_name(scratch, 8));
    else out("mov%c %s, %%%s", sfx(w), loc(v, w), reg_name(scratch, w));
    return scratch;
}

/* the register an instruction writes its result to: dst's own, or scratch */
static int def_reg(int v, int scratch) { return isreg(v) ? ph(v) : scratch; }

static void def_done(int v, int r, int w)       /* store the scratch result into a spilled v */
{
    if (isreg(v)) return;
    if (F->vr[v].cls == C_FLT) out("mov%s %%%s, %s", fs(w), reg_name(r, 8), loc(v, w));
    else out("mov%c %%%s, %s", sfx(w), reg_name(r, w), loc(v, w));
}

/* ---- Parallel moves (call arguments, parameters, asm operands) */
typedef struct { int dreg; int dv; int sreg; int sv; long imm; int simm; int w, cls; int done; } Move;

/* Into registers: dreg <- (sreg | virtual register sv | imm). Cycles among
 * registers are broken with the register temp. */
static void par_move(Move *mv, int n, int temp)
{
    /* sources that are virtual registers: their physical register, if any */
    for (int i = 0; i < n; i++) {
        mv[i].done = 0;
        if (mv[i].sv && isreg(mv[i].sv)) { mv[i].sreg = ph(mv[i].sv); mv[i].sv = 0; }
        else if (mv[i].sv) mv[i].sreg = -1;
        if (mv[i].simm) mv[i].sreg = -1;
        if (mv[i].sreg >= 0 && mv[i].sreg == mv[i].dreg) mv[i].done = 1;
    }
    /* register to register, in a safe order */
    for (;;) {
        int left = 0, progress = 0;
        for (int i = 0; i < n; i++) {
            Move *m = &mv[i];
            if (m->done || m->sreg < 0) continue;
            left++;
            int blocked = 0;
            for (int j = 0; j < n; j++)
                if (j != i && !mv[j].done && mv[j].sreg == m->dreg) { blocked = 1; break; }
            if (blocked) continue;
            if (m->dreg >= XMM0 || m->sreg >= XMM0) out("movaps %%%s, %%%s", reg_name(m->sreg, 8), reg_name(m->dreg, 8));
            else out("movq %%%s, %%%s", r64[m->sreg], r64[m->dreg]);
            m->done = 1;
            progress = 1;
        }
        if (!left) break;
        if (!progress) {                       /* a cycle: free one source through temp */
            for (int i = 0; i < n; i++) {
                Move *m = &mv[i];
                if (m->done || m->sreg < 0) continue;
                int s = m->sreg;
                if (s >= XMM0) out("movaps %%%s, %%xmm15", reg_name(s, 8));
                else out("movq %%%s, %%%s", r64[s], r64[temp]);
                for (int j = 0; j < n; j++) if (!mv[j].done && mv[j].sreg == s) mv[j].sreg = s >= XMM0 ? XMM0 + 15 : temp;
                break;
            }
        }
    }
    /* from memory and constants */
    for (int i = 0; i < n; i++) {
        Move *m = &mv[i];
        if (m->done) continue;
        if (m->simm) {
            if (m->dreg >= XMM0) { out("movabsq $%ld, %%rax", m->imm); out("movq %%rax, %%%s", reg_name(m->dreg, 8)); }
            else mov_imm_to(m->imm, R(m->dreg, 8), 8, 1, m->dreg);
        } else if (m->dreg >= XMM0) out("mov%s %s, %%%s", fs(m->w), loc(m->sv, m->w), reg_name(m->dreg, 8));
        else out("movq %s, %%%s", loc(m->sv, 8), r64[m->dreg]);
        m->done = 1;
    }
}

/* ---- Conditions */
static const char *cc_name(int cc)
{
    static const char *n[] = { "e", "ne", "l", "le", "g", "ge", "b", "be", "a", "ae" };
    return n[cc];
}
static int cc_invert(int cc)
{
    static const int inv[] = { CC_NE, CC_EQ, CC_GE, CC_GT, CC_LE, CC_LT, CC_UGE, CC_UGT, CC_ULE, CC_ULT };
    return inv[cc];
}

/* cmp for integers: flags = a - b */
static void cmp_int(Ins *i)
{
    int w = i->w;
    const char *b;
    if (i->bimm) {
        if (i->imm == 0 && isreg(i->a)) { out("test%c %s, %s", sfx(w), loc(i->a, w), loc(i->a, w)); return; }
        b = tmp();
        snprintf((char *)b, 96, "$%ld", w == 4 ? (long)(int)i->imm : i->imm);
    } else b = loc(i->b, w);
    const char *a = loc(i->a, w);
    if (!isreg(i->a) && (i->bimm || !isreg(i->b))) {
        if (i->bimm) { out("cmp%c %s, %s", sfx(w), b, a); return; }
        out("mov%c %s, %%%s", sfx(w), a, reg_name(RAX, w));
        a = R(RAX, w);
    }
    out("cmp%c %s, %s", sfx(w), b, a);
}

/* ucomis for floats, operands ordered so that "a" (above) means the condition */
static int cmp_float(Ins *i)                  /* returns: 0 a/ae form, 1 eq, 2 ne */
{
    int w = i->w;
    int x, y;                                  /* flags from y - x */
    switch (i->cc) {
    case CC_FLT: case CC_FLE: x = i->a; y = i->b; break;   /* b > a */
    default: x = i->b; y = i->a; break;                    /* a > b, a == b */
    }
    int yr = use_reg(y, XMM0, w);
    out("ucomis%c %s, %%%s", w == 4 ? 's' : 'd', loc(x, w), reg_name(yr, 8));
    return i->cc == CC_FEQ ? 1 : i->cc == CC_FNE ? 2 : 0;
}

static const char *fcc(int cc) { return cc == CC_FLT || cc == CC_FGT ? "a" : "ae"; }

/* ---- Instructions */
static void binop(Ins *i, const char *op)
{
    int w = i->w;
    const char *b;
    if (i->bimm) { b = tmp(); snprintf((char *)b, 96, "$%ld", w == 4 ? (long)(int)i->imm : i->imm); }
    else b = loc(i->b, w);
    int comm = !strcmp(op, "add") || !strcmp(op, "and") || !strcmp(op, "or") || !strcmp(op, "xor") || !strcmp(op, "imul");
    if (isreg(i->dst)) {
        int d = ph(i->dst);
        if (!i->bimm && isreg(i->b) && ph(i->b) == d && !(isreg(i->a) && ph(i->a) == d)) {
            if (comm) { out("%s%c %s, %%%s", op, sfx(w), loc(i->a, w), reg_name(d, w)); return; }
            out("mov%c %s, %%%s", sfx(w), loc(i->a, w), reg_name(RAX, w));
            out("%s%c %s, %%%s", op, sfx(w), b, reg_name(RAX, w));
            out("mov%c %%%s, %%%s", sfx(w), reg_name(RAX, w), reg_name(d, w));
            return;
        }
        if (!(isreg(i->a) && ph(i->a) == d)) out("mov%c %s, %%%s", sfx(w), loc(i->a, w), reg_name(d, w));
        out("%s%c %s, %%%s", op, sfx(w), b, reg_name(d, w));
        return;
    }
    out("mov%c %s, %%%s", sfx(w), loc(i->a, w), reg_name(RAX, w));
    out("%s%c %s, %%%s", op, sfx(w), b, reg_name(RAX, w));
    out("mov%c %%%s, %s", sfx(w), reg_name(RAX, w), loc(i->dst, w));
}

static void shift(Ins *i, const char *op)
{
    int w = i->w;
    if (i->bimm) { binop(i, op); return; }
    out("movl %s, %%ecx", loc(i->b, 4));
    int d = def_reg(i->dst, RAX);
    if (!(isreg(i->a) && ph(i->a) == d)) out("mov%c %s, %%%s", sfx(w), loc(i->a, w), reg_name(d, w));
    out("%s%c %%cl, %%%s", op, sfx(w), reg_name(d, w));
    def_done(i->dst, d, w);
}

static void divide(Ins *i)
{
    int w = i->w;
    int sgn = i->op == O_DIV || i->op == O_MOD;
    out("mov%c %s, %%%s", sfx(w), loc(i->a, w), reg_name(RAX, w));
    if (sgn) out(w == 8 ? "cqto" : "cltd");
    else out("xorl %%edx, %%edx");
    out("%s%c %s", sgn ? "idiv" : "div", sfx(w), loc(i->b, w));
    int res = i->op == O_DIV || i->op == O_UDIV ? RAX : RDX;
    if (isreg(i->dst)) out("mov%c %%%s, %%%s", sfx(w), reg_name(res, w), reg_name(ph(i->dst), w));
    else out("mov%c %%%s, %s", sfx(w), reg_name(res, w), loc(i->dst, w));
}

static void extend(Ins *i)
{
    int d = def_reg(i->dst, RAX);
    const char *src = isreg(i->a) ? R(ph(i->a), i->sz) : loc(i->a, i->sz);
    int w = i->w;
    if (i->sz == 1) out("mov%cb%c %s, %%%s", i->sgn ? 's' : 'z', i->sgn && w == 8 ? 'q' : 'l', src, reg_name(d, i->sgn && w == 8 ? 8 : 4));
    else if (i->sz == 2) out("mov%cw%c %s, %%%s", i->sgn ? 's' : 'z', i->sgn && w == 8 ? 'q' : 'l', src, reg_name(d, i->sgn && w == 8 ? 8 : 4));
    else if (i->sgn) out("movslq %s, %%%s", src, r64[d]);
    else out("movl %s, %%%s", src, r32[d]);
    def_done(i->dst, d, w);
}

static void load(Ins *i)
{
    const char *m = addr(&i->m, R11, RCX);
    if (F->vr[i->dst].cls == C_FLT) {
        int d = def_reg(i->dst, XMM0);
        out("mov%s %s, %%%s", fs(i->w), m, reg_name(d, 8));
        def_done(i->dst, d, i->w);
        return;
    }
    int d = def_reg(i->dst, RAX), w = i->w;
    switch (i->sz) {
    case 1: out("mov%cb%c %s, %%%s", i->sgn ? 's' : 'z', i->sgn && w == 8 ? 'q' : 'l', m, reg_name(d, i->sgn && w == 8 ? 8 : 4)); break;
    case 2: out("mov%cw%c %s, %%%s", i->sgn ? 's' : 'z', i->sgn && w == 8 ? 'q' : 'l', m, reg_name(d, i->sgn && w == 8 ? 8 : 4)); break;
    case 4: if (i->sgn && w == 8) out("movslq %s, %%%s", m, r64[d]); else out("movl %s, %%%s", m, r32[d]); break;
    default: out("movq %s, %%%s", m, r64[d]);
    }
    def_done(i->dst, d, w);
}

static void store(Ins *i)
{
    int sz = i->sz;
    if (i->bimm) {
        const char *m = addr(&i->m, R11, RCX);
        if (sz == 8 && !fits32(i->imm)) { out("movabsq $%ld, %%rax", i->imm); out("movq %%rax, %s", m); return; }
        long v = sz == 1 ? (long)(signed char)i->imm : sz == 2 ? (long)(short)i->imm : sz == 4 ? (long)(int)i->imm : i->imm;
        out("mov%c $%ld, %s", sfx(sz), v, m);
        return;
    }
    if (F->vr[i->a].cls == C_FLT) {
        int r = use_reg(i->a, XMM0, sz);
        out("mov%s %%%s, %s", fs(sz), reg_name(r, 8), addr(&i->m, R11, RCX));
        return;
    }
    int r = use_reg(i->a, RAX, sz == 8 ? 8 : 4);
    out("mov%c %%%s, %s", sfx(sz), reg_name(r, sz), addr(&i->m, R11, RCX));
}

static void fbin(Ins *i, const char *op)
{
    int w = i->w;
    int comm = !strcmp(op, "add") || !strcmp(op, "mul");
    int d = def_reg(i->dst, XMM0);
    if (isreg(i->b) && ph(i->b) == d && !(isreg(i->a) && ph(i->a) == d)) {
        if (comm) { out("%s%s %s, %%%s", op, fs(w), loc(i->a, w), reg_name(d, 8)); def_done(i->dst, d, w); return; }
        d = XMM0;
    }
    fget(i->a, w, d);
    out("%s%s %s, %%%s", op, fs(w), loc(i->b, w), reg_name(d, 8));
    if (isreg(i->dst) && ph(i->dst) != d) out("movaps %%%s, %s", reg_name(d, 8), loc(i->dst, 8));
    else def_done(i->dst, d, w);
}

static void i2f(Ins *i)
{
    int w = i->w;
    int d = def_reg(i->dst, XMM0);
    char c = w == 4 ? 's' : 'd';
    out("xorps %%%s, %%%s", reg_name(d, 8), reg_name(d, 8));   /* (cvtsi2s* keeps the top: no false dependency) */
    if (i->sgn) {
        int sz = i->sz;
        const char *src = isreg(i->a) ? R(ph(i->a), sz) : loc(i->a, sz);
        out("cvtsi2s%c%c %s, %%%s", c, sfx(sz), src, reg_name(d, 8));
    } else {                                    /* unsigned 64-bit */
        int l1 = uniq++, l2 = uniq++;
        out("movq %s, %%rax", loc(i->a, 8));
        out("testq %%rax, %%rax");
        out("js .Lu%d", l1);
        out("cvtsi2s%cq %%rax, %%%s", c, reg_name(d, 8));
        out("jmp .Lu%d", l2);
        buf_f(o, ".Lu%d:\n", l1);
        out("movq %%rax, %%rcx");
        out("shrq %%rcx");
        out("andl $1, %%eax");
        out("orq %%rax, %%rcx");
        out("cvtsi2s%cq %%rcx, %%%s", c, reg_name(d, 8));
        out("adds%c %%%s, %%%s", c, reg_name(d, 8), reg_name(d, 8));
        buf_f(o, ".Lu%d:\n", l2);
    }
    def_done(i->dst, d, w);
}

static void f2i(Ins *i)
{
    int w = i->w, sz = i->sz;
    char c = sz == 4 ? 's' : 'd';
    int d = def_reg(i->dst, RAX);
    if (i->sgn) out("cvtts%c2si%c %s, %%%s", c, w == 8 ? 'q' : 'l', loc(i->a, sz), reg_name(d, w));
    else {                                      /* to unsigned 64-bit */
        int l1 = uniq++, l2 = uniq++;
        int x = use_reg(i->a, XMM0, sz);
        if (sz == 4) { out("movl $0x5f000000, %%ecx"); out("movd %%ecx, %%xmm1"); }
        else { out("movabsq $0x43e0000000000000, %%rcx"); out("movq %%rcx, %%xmm1"); }
        out("ucomis%c %%xmm1, %%%s", c, reg_name(x, 8));
        out("jae .Lu%d", l1);
        out("cvtts%c2siq %%%s, %%%s", c, reg_name(x, 8), r64[d]);
        out("jmp .Lu%d", l2);
        buf_f(o, ".Lu%d:\n", l1);
        out("movaps %%%s, %%xmm0", reg_name(x, 8));
        out("subs%c %%xmm1, %%xmm0", c);
        out("cvtts%c2siq %%xmm0, %%%s", c, r64[d]);
        out("btcq $63, %%%s", r64[d]);
        buf_f(o, ".Lu%d:\n", l2);
    }
    def_done(i->dst, d, w);
}

/* ---- long double: the x87 stack holds only what one instruction needs;
 * values live in memory, addresses in registers (r11, rcx, rdx scratch). */
static void ld_set(int cc, int d)              /* after "fucomip": d = condition (operands as in cmp_float) */
{
    if (cc == CC_FEQ) { out("sete %%al"); out("setnp %%cl"); out("andb %%cl, %%al"); }
    else if (cc == CC_FNE) { out("setne %%al"); out("setp %%cl"); out("orb %%cl, %%al"); }
    else out("set%s %%al", fcc(cc));
    out("movzbl %%al, %%%s", r32[d]);
}

static void x87(Ins *i)
{
    char l1[32], l2[32];
    switch (i->op) {
    case O_LDBIN: {
        static const char *ops[] = { "faddp", "fsubp", "fmulp", "fdivp" };   /* st(1) = st(0) op st(1), pop */
        int a = use_reg(i->a, R11, 8), b = use_reg(i->b, RCX, 8), d = use_reg(i->m.base, RDX, 8);
        out("fldt (%%%s)", r64[b]);
        out("fldt (%%%s)", r64[a]);
        out("%s", ops[i->imm]);
        out("fstpt (%%%s)", r64[d]);
        return;
    }
    case O_LDNEG: {
        int a = use_reg(i->a, R11, 8), d = use_reg(i->m.base, RDX, 8);
        out("fldt (%%%s)", r64[a]);
        out("fchs");
        out("fstpt (%%%s)", r64[d]);
        return;
    }
    case O_LDCMP: {                             /* fucomip compares st(0) with st(1) */
        int swap = i->cc == CC_FLT || i->cc == CC_FLE;
        int a = use_reg(i->a, R11, 8), b = i->b ? use_reg(i->b, RCX, 8) : -1;
        int first = swap ? a : b, second = swap ? b : a;
        if (first < 0) out("fldz"); else out("fldt (%%%s)", r64[first]);
        if (second < 0) out("fldz"); else out("fldt (%%%s)", r64[second]);
        out("fucomip %%st(1), %%st");
        out("fstp %%st(0)");
        int d = def_reg(i->dst, RAX);
        ld_set(swap ? (i->cc == CC_FLT ? CC_FGT : CC_FGE) : i->cc, d);
        def_done(i->dst, d, 4);
        return;
    }
    }
    /* O_LDCVT */
    switch (i->imm) {
    case 0: {                                   /* integer -> long double */
        int d = use_reg(i->m.base, RDX, 8);
        if (isreg(i->a)) out("mov%c %%%s, %%%s", i->sz == 8 ? 'q' : 'l', reg_name(ph(i->a), i->sz == 8 ? 8 : 4), reg_name(RAX, i->sz == 8 ? 8 : 4));
        else out("mov%c %s, %%%s", i->sz == 8 ? 'q' : 'l', loc(i->a, i->sz == 8 ? 8 : 4), reg_name(RAX, i->sz == 8 ? 8 : 4));
        if (i->sz == 4 && i->sgn) { out("movl %%eax, (%%%s)", r64[d]); out("fildl (%%%s)", r64[d]); }
        else {                                  /* (unsigned 32: zero-extended by the movl) */
            out("movq %%rax, (%%%s)", r64[d]);
            out("fildq (%%%s)", r64[d]);
            if (i->sz == 8 && !i->sgn) {        /* unsigned 64 with the top bit set: + 2^64 */
                int l = uniq++;
                out("testq %%rax, %%rax");
                out("jns .Lu%d", l);
                out("movl $0x5f800000, 8(%%%s)", r64[d]);
                out("fadds 8(%%%s)", r64[d]);
                buf_f(o, ".Lu%d:\n", l);
            }
        }
        out("fstpt (%%%s)", r64[d]);
        return;
    }
    case 1: {                                   /* long double -> 64-bit integer (truncated) */
        int a = use_reg(i->a, R11, 8);
        long s = slot_off(i->m.slot);
        out("fldt (%%%s)", r64[a]);
        if (i->sgn) { out("fisttpq %ld(%%rbp)", s); out("movq %ld(%%rbp), %%rax", s); }
        else {                                  /* unsigned: values >= 2^63 go through x - 2^63 */
            int big = uniq++, done = uniq++;
            snprintf(l1, 32, ".Lu%d", big);
            snprintf(l2, 32, ".Lu%d", done);
            out("movl $0x5f000000, %ld(%%rbp)", s + 8);
            out("flds %ld(%%rbp)", s + 8);
            out("fucomip %%st(1), %%st");
            out("jbe %s", l1);
            out("fisttpq %ld(%%rbp)", s);
            out("movq %ld(%%rbp), %%rax", s);
            out("jmp %s", l2);
            buf_f(o, "%s:\n", l1);
            out("fsubs %ld(%%rbp)", s + 8);
            out("fisttpq %ld(%%rbp)", s);
            out("movq %ld(%%rbp), %%rax", s);
            out("btcq $63, %%rax");
            buf_f(o, "%s:\n", l2);
        }
        def_done(i->dst, RAX, 8);
        if (isreg(i->dst)) out("movq %%rax, %%%s", r64[ph(i->dst)]);
        return;
    }
    case 2: {                                   /* float, double -> long double */
        int d = use_reg(i->m.base, RDX, 8);
        int x = use_reg(i->a, XMM0, i->sz);
        out("mov%s %%%s, (%%%s)", fs(i->sz), reg_name(x, 8), r64[d]);
        out("fld%c (%%%s)", i->sz == 4 ? 's' : 'l', r64[d]);
        out("fstpt (%%%s)", r64[d]);
        return;
    }
    default: {                                  /* long double -> float, double */
        int a = use_reg(i->a, R11, 8);
        long s = slot_off(i->m.slot);
        out("fldt (%%%s)", r64[a]);
        out("fstp%c %ld(%%rbp)", i->w == 4 ? 's' : 'l', s);
        int d = def_reg(i->dst, XMM0);
        out("mov%s %ld(%%rbp), %%%s", fs(i->w), s, reg_name(d, 8));
        def_done(i->dst, d, i->w);
    }
    }
}

/* ---- __int128: two halves in memory; rax and rdx hold them while working */
static void i128_store(Ins *i)                 /* rax:rdx -> *m.base */
{
    int d = use_reg(i->m.base, R11, 8);
    out("movq %%rax, (%%%s)", r64[d]);
    out("movq %%rdx, 8(%%%s)", r64[d]);
}

static void i128(Ins *i)
{
    switch (i->op) {
    case O_I128EXT: {
        if (isreg(i->a)) out("movq %%%s, %%rax", r64[ph(i->a)]); else out("movq %s, %%rax", loc(i->a, 8));
        if (i->sgn) out("cqto"); else out("xorl %%edx, %%edx");
        i128_store(i);
        return;
    }
    case O_I128SH: {
        int a = use_reg(i->a, R11, 8);
        out("movq (%%%s), %%rax", r64[a]);
        out("movq 8(%%%s), %%rdx", r64[a]);
        if (i->bimm) out("movl $%ld, %%ecx", i->imm);
        else out("movl %s, %%ecx", loc(i->b, 4));
        int l = uniq++;
        if (i->sz == 0) {                           /* shl: within 64, then a whole half */
            out("shldq %%cl, %%rax, %%rdx");
            out("shlq %%cl, %%rax");
            out("testb $64, %%cl");
            out("je .Lu%d", l);
            out("movq %%rax, %%rdx");
            out("xorl %%eax, %%eax");
        } else {
            out("shrdq %%cl, %%rdx, %%rax");
            out("%s %%cl, %%rdx", i->sz == 1 ? "shrq" : "sarq");
            out("testb $64, %%cl");
            out("je .Lu%d", l);
            out("movq %%rdx, %%rax");
            if (i->sz == 1) out("xorl %%edx, %%edx"); else out("sarq $63, %%rdx");
        }
        buf_f(o, ".Lu%d:\n", l);
        i128_store(i);
        return;
    }
    case O_I128CMP: {                           /* flags of a - b over both halves */
        int swap = i->cc == CC_LE || i->cc == CC_GT || i->cc == CC_ULE || i->cc == CC_UGT;
        int a = use_reg(i->a, R11, 8), b = use_reg(i->b, RCX, 8);
        if (swap) { int t = a; a = b; b = t; }
        out("movq (%%%s), %%rax", r64[a]);
        out("movq 8(%%%s), %%rdx", r64[a]);
        const char *cc;
        if (i->cc == CC_EQ || i->cc == CC_NE) {
            out("xorq (%%%s), %%rax", r64[b]);
            out("xorq 8(%%%s), %%rdx", r64[b]);
            out("orq %%rdx, %%rax");
            cc = i->cc == CC_EQ ? "e" : "ne";
        } else {
            out("cmpq (%%%s), %%rax", r64[b]);
            out("sbbq 8(%%%s), %%rdx", r64[b]);
            switch (i->cc) {                        /* (swapped: a <= b is !(b < a), a > b is b < a) */
            case CC_LT: cc = "l"; break;
            case CC_GE: cc = "ge"; break;
            case CC_LE: cc = "ge"; break;
            case CC_GT: cc = "l"; break;
            case CC_ULT: cc = "b"; break;
            case CC_UGE: cc = "ae"; break;
            case CC_ULE: cc = "ae"; break;
            default: cc = "b";
            }
        }
        int d = def_reg(i->dst, RAX);
        out("set%s %%al", cc);
        out("movzbl %%al, %%%s", r32[d]);
        def_done(i->dst, d, 4);
        return;
    }
    }
    /* O_I128: binary and unary */
    int a = use_reg(i->a, R11, 8);
    switch (i->imm) {
    case 0: case 1: case 3: case 4: case 5: {   /* add/adc, sub/sbb, and, or, xor */
        static const char *lo[] = { "addq", "subq", "", "andq", "orq", "xorq" }, *hi[] = { "adcq", "sbbq", "", "andq", "orq", "xorq" };
        int b = use_reg(i->b, RCX, 8);
        out("movq (%%%s), %%rax", r64[a]);
        out("movq 8(%%%s), %%rdx", r64[a]);
        out("%s (%%%s), %%rax", lo[i->imm], r64[b]);
        out("%s 8(%%%s), %%rdx", hi[i->imm], r64[b]);
        break;
    }
    case 2: {                                   /* lo*lo (128 bits) + the cross products in the high half */
        int b = use_reg(i->b, RCX, 8);
        out("movq (%%%s), %%rax", r64[a]);
        out("imulq 8(%%%s), %%rax", r64[b]);
        out("pushq %%rax");
        out("movq 8(%%%s), %%rax", r64[a]);
        out("imulq (%%%s), %%rax", r64[b]);
        out("addq %%rax, (%%rsp)");
        out("movq (%%%s), %%rax", r64[a]);
        out("mulq (%%%s)", r64[b]);
        out("popq %%rcx");
        out("addq %%rcx, %%rdx");
        break;
    }
    case 6:                                     /* neg */
        out("movq (%%%s), %%rax", r64[a]);
        out("movq 8(%%%s), %%rdx", r64[a]);
        out("negq %%rax");
        out("adcq $0, %%rdx");
        out("negq %%rdx");
        break;
    default:                                    /* not */
        out("movq (%%%s), %%rax", r64[a]);
        out("movq 8(%%%s), %%rdx", r64[a]);
        out("notq %%rax");
        out("notq %%rdx");
    }
    i128_store(i);
}

/* ---- Calls */
static const int argregs[6] = { RDI, RSI, RDX, RCX, R8, R9 };

static void call(Ins *i)
{
    Call *c = i->call;
    if (c->stack) out("subq $%ld, %%rsp", c->stack);
    /* stack arguments first (they read the registers the argument registers will hold) */
    for (int k = 0; k < c->nargs; k++) {
        CallArg *a = &c->args[k];
        if (a->phys >= 0) continue;
        if (a->addr) {
            int src = use_reg(a->addr, R11, 8);
            long n = a->size, off = 0;
            for (; n >= 8; n -= 8, off += 8) { out("movq %ld(%%%s), %%rax", off, r64[src]); out("movq %%rax, %ld(%%rsp)", a->stack_off + off); }
            if (n >= 4) { out("movl %ld(%%%s), %%eax", off, r64[src]); out("movl %%eax, %ld(%%rsp)", a->stack_off + off); n -= 4; off += 4; }
            if (n >= 2) { out("movw %ld(%%%s), %%ax", off, r64[src]); out("movw %%ax, %ld(%%rsp)", a->stack_off + off); n -= 2; off += 2; }
            if (n >= 1) { out("movb %ld(%%%s), %%al", off, r64[src]); out("movb %%al, %ld(%%rsp)", a->stack_off + off); }
            continue;
        }
        if (a->cls == C_FLT) {
            int r = use_reg(a->v, XMM0, a->w);
            out("mov%s %%%s, %ld(%%rsp)", fs(a->w), reg_name(r, 8), a->stack_off);
        } else {
            int r = use_reg(a->v, RAX, 8);
            out("movq %%%s, %ld(%%rsp)", r64[r], a->stack_off);
        }
    }
    /* the function's address: out of the way, in r11 */
    if (!c->sym) {
        if (isreg(c->fn)) out("movq %%%s, %%r11", r64[ph(c->fn)]);
        else out("movq %s, %%r11", loc(c->fn, 8));
    }
    Move mv[16];
    int n = 0;
    for (int k = 0; k < c->nargs && n < 16; k++) {
        CallArg *a = &c->args[k];
        if (a->phys < 0 || a->isimm) continue;
        memset(&mv[n], 0, sizeof mv[n]);
        mv[n].dreg = a->phys;
        mv[n].sv = a->v;
        mv[n].sreg = -1;
        mv[n].w = a->w;
        mv[n].cls = a->cls;
        n++;
    }
    par_move(mv, n, RAX);
    for (int k = 0; k < c->nargs; k++)            /* constants: straight into their registers */
        if (c->args[k].isimm) mov_imm_to(c->args[k].imm, R(c->args[k].phys, c->args[k].w), c->args[k].w, 1, c->args[k].phys);
    if (c->variadic) {
        if (c->nsse) out("movl $%d, %%eax", c->nsse);
        else out("xorl %%eax, %%eax");
    }
    if (c->sym) out("call %s", c->sym);
    else out("call *%%r11");
    if (c->stack) out("addq $%ld, %%rsp", c->stack);
    if (c->x87slot) out("fstpt %ld(%%rbp)", slot_off(c->x87slot));
    if (c->x87cplx) out("fstpt %ld(%%rbp)", slot_off(c->x87slot) + 16);
    /* results */
    for (int k = 0; k < c->nret; k++) {
        int v = c->ret[k], p = c->retphys[k], w = c->retw[k];
        if (p >= XMM0) {
            if (isreg(v)) out("movaps %%%s, %%%s", reg_name(p, 8), reg_name(ph(v), 8));
            else out("mov%s %%%s, %s", fs(w), reg_name(p, 8), loc(v, w));
        } else {
            if (isreg(v)) { if (ph(v) != p) out("movq %%%s, %%%s", r64[p], r64[ph(v)]); }
            else out("mov%c %%%s, %s", sfx(w), reg_name(p, w), loc(v, w));
        }
    }
}

/* ---- Parameters */
static void entry(Ins *i)
{
    Obj *fn = F->obj;
    if (fn->ty->is_variadic) {                 /* the register save area for va_arg */
        long base = slot_off(F->va_slot);
        for (int k = 0; k < 6; k++) out("movq %%%s, %ld(%%rbp)", r64[argregs[k]], base + k * 8);
        if (!opt_general_regs_only) {
            int l = uniq++;
            out("testb %%al, %%al");
            out("je .Lu%d", l);
            for (int k = 0; k < 8; k++) out("movaps %%xmm%d, %ld(%%rbp)", k, base + 48 + k * 16);
            buf_f(o, ".Lu%d:\n", l);
        }
    }
    /* stores into frame slots, and into spilled registers, first */
    for (int k = 0; k < i->npin; k++) {
        ParamIn *p = &i->pin[k];
        if (p->phys < 0) continue;
        if (p->slot) {
            if (p->cls == C_FLT) out("mov%s %%%s, %ld(%%rbp)", fs(p->w), reg_name(p->phys, 8), slot_off(p->slot) + p->off);
            else out("mov%c %%%s, %ld(%%rbp)", sfx(p->w), reg_name(p->phys, p->w), slot_off(p->slot) + p->off);
        } else if (p->v && !isreg(p->v) && F->vr[p->v].end >= 0) {
            if (p->cls == C_FLT) out("mov%s %%%s, %s", fs(p->w), reg_name(p->phys, 8), loc(p->v, p->w));
            else out("movq %%%s, %s", r64[p->phys], loc(p->v, 8));
        }
    }
    /* register to register */
    Move mv[16];
    int n = 0;
    for (int k = 0; k < i->npin && n < 16; k++) {
        ParamIn *p = &i->pin[k];
        if (p->phys < 0 || p->slot || !p->v || !isreg(p->v)) continue;
        memset(&mv[n], 0, sizeof mv[n]);
        mv[n].dreg = ph(p->v);
        mv[n].sreg = p->phys;
        mv[n].w = p->w;
        n++;
    }
    par_move(mv, n, RAX);
    /* from the stack */
    for (int k = 0; k < i->npin; k++) {
        ParamIn *p = &i->pin[k];
        if (p->phys >= 0 || !p->v || F->vr[p->v].end < 0) continue;
        if (p->cls == C_FLT) {
            int d = def_reg(p->v, XMM0);
            out("mov%s %ld(%%rbp), %%%s", fs(p->w), 16 + p->stack_off, reg_name(d, 8));
            def_done(p->v, d, p->w);
        } else {
            int d = def_reg(p->v, RAX);
            out("movq %ld(%%rbp), %%%s", 16 + p->stack_off, r64[d]);
            def_done(p->v, d, 8);
        }
    }
}

/* ---- Inline asm */
static void asm_ins(Ins *i)
{
    AsmIns *a = i->asmi;
    if (a->is_basic) { buf_s(o, a->tmpl); buf_c(o, '\n'); return; }
    RegSet used = 0;
    for (int k = 0; k < a->n; k++) {
        if (a->op[k].kind == 'r') used |= BIT(a->op[k].phys);
        if (a->op[k].mreg > 0) used |= BIT(a->op[k].mreg);
    }
    int temp = !(used & BIT(R11)) ? R11 : !(used & BIT(RAX)) ? RAX : !(used & BIT(RCX)) ? RCX : RDX;
    /* inputs into their registers */
    Move mv[32];
    int n = 0;
    for (int k = 0; k < a->n && n < 32; k++) {
        AsmOperand *op = &a->op[k];
        int dreg = op->kind == 'r' && op->in ? op->phys : op->mreg > 0 ? op->mreg : -1;
        if (dreg < 0 || !op->v) continue;
        memset(&mv[n], 0, sizeof mv[n]);
        mv[n].dreg = dreg;
        mv[n].sv = op->v;
        mv[n].sreg = -1;
        mv[n].w = 8;
        n++;
    }
    par_move(mv, n, temp);
    /* the template, operands substituted */
    Buf t = { 0 };
    static int asm_id;                         /* %=: a number unique to this asm instance */
    asm_id++;
    for (const char *p = a->tmpl; *p; p++) {
        if (*p != '%') { buf_c(&t, *p); continue; }
        p++;
        if (*p == '%') { buf_c(&t, '%'); continue; }
        if (*p == '=') { buf_f(&t, "%d", asm_id); continue; }
        char mod = 0;
        if (*p && strchr("bwkqhcPna", *p) && (p[1] == '[' || (p[1] >= '0' && p[1] <= '9'))) mod = *p++;
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
        if (op->kind == 'i') { buf_f(&t, mod == 'c' || mod == 'P' || mod == 'n' ? "%ld" : "$%ld", mod == 'n' ? -op->imm : op->imm); continue; }
        if (op->kind == 'r') {
            int size = op->size < 1 ? 8 : op->size;
            if (mod == 'b') size = 1;
            else if (mod == 'w') size = 2;
            else if (mod == 'k') size = 4;
            else if (mod == 'q') size = 8;
            if (mod == 'h') { static const char *hn[] = { "ah", "ch", "dh", "bh" }; buf_f(&t, "%%%s", hn[op->phys & 3]); continue; }
            buf_f(&t, "%%%s", reg_name(op->phys, size));
            continue;
        }
        if (op->mreg > 0) buf_f(&t, "(%%%s)", r64[op->mreg]);
        else buf_s(&t, addr(&op->m, R11, RCX));
    }
    buf_add(o, t.p ? t.p : "", t.len);
    buf_c(o, '\n');
    free(t.p);
    /* outputs: from their registers, memory destinations first */
    for (int k = 0; k < a->n; k++) {
        AsmOperand *op = &a->op[k];
        if (!op->out || op->kind != 'r' || !op->outv || isreg(op->outv)) continue;
        out("movq %%%s, %s", r64[(int)op->phys], loc(op->outv, 8));
    }
    n = 0;
    for (int k = 0; k < a->n && n < 32; k++) {
        AsmOperand *op = &a->op[k];
        if (!op->out || op->kind != 'r' || !op->outv || !isreg(op->outv)) continue;
        memset(&mv[n], 0, sizeof mv[n]);
        mv[n].dreg = ph(op->outv);
        mv[n].sreg = op->phys;
        mv[n].w = 8;
        n++;
    }
    par_move(mv, n, temp);
}

/* ---- Memory blocks */
static void copy(Ins *i)
{
    long n = i->imm, off = 0;
    if (n > 64) {
        Move mv[2];
        memset(mv, 0, sizeof mv);
        mv[0].dreg = RDI; mv[0].sv = i->a; mv[0].sreg = -1; mv[0].w = 8;
        mv[1].dreg = RSI; mv[1].sv = i->b; mv[1].sreg = -1; mv[1].w = 8;
        par_move(mv, 2, RAX);
        out("movl $%ld, %%ecx", n);
        out("rep movsb");
        return;
    }
    int d = use_reg(i->a, R11, 8), s = use_reg(i->b, RCX, 8);
    for (; n >= 8; n -= 8, off += 8) { out("movq %ld(%%%s), %%rax", off, r64[s]); out("movq %%rax, %ld(%%%s)", off, r64[d]); }
    if (n >= 4) { out("movl %ld(%%%s), %%eax", off, r64[s]); out("movl %%eax, %ld(%%%s)", off, r64[d]); n -= 4; off += 4; }
    if (n >= 2) { out("movw %ld(%%%s), %%ax", off, r64[s]); out("movw %%ax, %ld(%%%s)", off, r64[d]); n -= 2; off += 2; }
    if (n >= 1) { out("movb %ld(%%%s), %%al", off, r64[s]); out("movb %%al, %ld(%%%s)", off, r64[d]); }
}

static void zero(Ins *i)
{
    long n = i->imm, off = 0;
    if (n > 64) {
        Move mv[1];
        memset(mv, 0, sizeof mv);
        mv[0].dreg = RDI; mv[0].sv = i->a; mv[0].sreg = -1; mv[0].w = 8;
        par_move(mv, 1, RAX);
        out("xorl %%eax, %%eax");
        out("movl $%ld, %%ecx", n);
        out("rep stosb");
        return;
    }
    int d = use_reg(i->a, R11, 8);
    for (; n >= 8; n -= 8, off += 8) out("movq $0, %ld(%%%s)", off, r64[d]);
    if (n >= 4) { out("movl $0, %ld(%%%s)", off, r64[d]); n -= 4; off += 4; }
    if (n >= 2) { out("movw $0, %ld(%%%s)", off, r64[d]); n -= 2; off += 2; }
    if (n >= 1) out("movb $0, %ld(%%%s)", off, r64[d]);
}

static void label_of(Block *b, char *s) { snprintf(s, 32, ".L%d_%d", fn_id, b->id); }

static void jmp(Block *b)
{
    char l[32];
    if (b == nextb) return;
    label_of(b, l);
    out("jmp %s", l);
}

static void ret_value(Ins *i)
{
    if (i->sgn) {                               /* a struct in registers */
        int c0 = i->sz & 1, c1 = i->sz >> 1 & 1;
        int r0 = c0 ? XMM0 : RAX;
        int r1 = c1 ? (c0 ? XMM0 + 1 : XMM0) : (c0 ? RAX : RDX);
        if (c0) out("movsd %s, %%xmm0", loc(i->a, 8)); else out("movq %s, %%rax", loc(i->a, 8));
        if (i->b) { if (c1) out("movsd %s, %%%s", loc(i->b, 8), reg_name(r1, 8)); else out("movq %s, %%%s", loc(i->b, 8), r64[r1]); }
        (void)r0;
        return;
    }
    if (i->bimm) { mov_imm_to(i->imm, R(RAX, i->w), i->w, 1, RAX); return; }
    if (!i->a) return;
    if (i->sz == 2) { out("fldt (%%%s)", r64[use_reg(i->a, R11, 8)]); return; }   /* long double: st0 */
    if (i->sz == 3) {                           /* complex long double: st0 real, st1 imaginary */
        int a = use_reg(i->a, R11, 8);
        out("fldt 16(%%%s)", r64[a]);
        out("fldt (%%%s)", r64[a]);
        return;
    }
    if (i->sz == 1) { out("mov%s %s, %%xmm0", fs(i->w), loc(i->a, i->w)); return; }
    out("mov%c %s, %%%s", sfx(i->w), loc(i->a, i->w), reg_name(RAX, i->w));
}

static void epilogue(void)
{
    static const int order[] = { R15, R14, R13, R12, RBX };
    if (noframe == 2) {
        if (F->has_calls && !(nsaved & 1)) out("addq $8, %%rsp");
        for (int k = 0; k < 5; k++) if (F->used_regs & BIT(order[k])) out("popq %%%s", r64[order[k]]);
    }
    if (noframe) { out("ret"); return; }
    if (nsaved) {
        out("leaq -%d(%%rbp), %%rsp", nsaved * 8);
        for (int k = 0; k < 5; k++) if (F->used_regs & BIT(order[k])) out("popq %%%s", r64[order[k]]);
        out("popq %%rbp");
    } else out("leave");
    out("ret");
}

/* ---- Vectors: one SSE instruction per 16 bytes ("op %xmm1, %xmm0"); 0: none */
const char *vec_insn(int op, int kind, int es)
{
    int f = kind == 2, s = kind == 0, i4 = es == 4;
    static const char *bw[] = { 0, "b", "w", 0, "d", 0, 0, 0, "q" };
    static char buf[32];
    const char *x = bw[es];
#define P(fmt, ...) (snprintf(buf, sizeof buf, fmt, __VA_ARGS__), buf)
    switch (op) {
    case VOP_ADD: return f ? (i4 ? "addps" : "addpd") : P("padd%s", x);
    case VOP_SUB: return f ? (i4 ? "subps" : "subpd") : P("psub%s", x);
    case VOP_MUL: return f ? (i4 ? "mulps" : "mulpd") : es == 2 ? "pmullw" : es == 4 && opt_avx ? "pmulld" : 0;
    case VOP_DIV: return f ? (i4 ? "divps" : "divpd") : 0;
    case VOP_AND: return "pand";
    case VOP_OR: return "por";
    case VOP_XOR: return "pxor";
    case VOP_ANDNOT: return "pandn";
    case VOP_EQ: return f ? "" : es == 8 ? (opt_avx ? "pcmpeqq" : 0) : P("pcmpeq%s", x);
    case VOP_GT: return f ? "" : !s ? 0 : es == 8 ? (opt_avx ? "pcmpgtq" : 0) : P("pcmpgt%s", x);
    case VOP_MIN: case VOP_MAX: {
        const char *m = op == VOP_MIN ? "min" : "max";
        if (f) return P("%s%s", m, i4 ? "ps" : "pd");
        if ((es == 2 && s) || (es == 1 && !s)) return P("p%s%s%s", m, s ? "s" : "u", x);
        if (opt_avx && es <= 4) return P("p%s%s%s", m, s ? "s" : "u", x);
        return 0;
    }
    case VOP_SQRT: return f ? (i4 ? "sqrtps" : "sqrtpd") : 0;
    case VOP_UNPCKLO: case VOP_UNPCKHI: {
        const char *h = op == VOP_UNPCKLO ? "l" : "h";
        if (f) return P("unpck%s%s", h, i4 ? "ps" : "pd");
        static const char *u[] = { 0, "bw", "wd", 0, "dq", 0, 0, 0, "qdq" };
        return P("punpck%s%s", h, u[es]);
    }
    case VOP_CVTI2F: return "cvtdq2ps";
    case VOP_CVTF2I: return "cvttps2dq";
    case VOP_CVTF2IR: return "cvtps2dq";
    case VOP_MADD: return "pmaddwd";
    case VOP_MADDUBS: return "pmaddubsw";
    case VOP_AVG: return es == 1 ? "pavgb" : "pavgw";
    case VOP_ADDS: return P("padd%ss%s", s ? "" : "u", x);
    case VOP_SUBS: return P("psub%ss%s", s ? "" : "u", x);
    case VOP_PACKS: return es == 2 ? "packsswb" : "packssdw";
    case VOP_PACKUS: return es == 2 ? "packuswb" : "packusdw";
    case VOP_MULHI: return s ? "pmulhw" : "pmulhuw";
    case VOP_MULUDQ: return "pmuludq";
    case VOP_SAD: return "psadbw";
    case VOP_SHUFB: return "pshufb";
    case VOP_ABS: return P("pabs%s", x);
    case VOP_SHLI: return es == 1 ? 0 : P("psll%s", x);
    case VOP_SHRI: return es == 1 ? 0 : P("psrl%s", x);
    case VOP_SARI: return es == 1 || es == 8 ? 0 : P("psra%s", x);
    case VOP_SLLDQ: return "pslldq";
    case VOP_SRLDQ: return "psrldq";
    case VOP_PSHUFD: return "pshufd";
    }
    return 0;
#undef P
}

static void vec(Ins *i)
{
    int op = i->cc, es = i->sz, kind = i->sgn, n = i->w, f = kind == 2;
    int pa = use_reg(i->a, R11, 8), pb = i->b ? use_reg(i->b, RCX, 8) : -1;
    const char *ld = n == 8 ? "movsd" : "movups";
    if (op == VOP_MOVEMASK) {                     /* the top bit of each element, in an integer */
        const char *m = f ? (es == 4 ? "movmskps" : "movmskpd") : "pmovmskb";
        out("%s (%%%s), %%xmm0", ld, r64[pa]);
        out("%s %%xmm0, %%eax", m);
        if (n == 32) {
            out("movups 16(%%%s), %%xmm0", r64[pa]);
            out("%s %%xmm0, %%ecx", m);
            out("shll $%d, %%ecx", f ? 16 / es : 16);
            out("orl %%ecx, %%eax");
        }
        def_done(i->dst, RAX, 4);
        if (isreg(i->dst)) out("movl %%eax, %%%s", r32[ph(i->dst)]);
        return;
    }
    int pd = use_reg(i->m.base, RDX, 8);
    for (int off = 0; off < n; off += 16) {
        const char *A = tmp(), *B = tmp();
        snprintf((char *)A, 96, "%d(%%%s)", off, r64[pa]);
        if (pb >= 0) snprintf((char *)B, 96, "%d(%%%s)", off, r64[pb]);
        int swap = f ? op == VOP_GT || op == VOP_GE : op == VOP_LT || op == VOP_GE;   /* (cmpps: lt, le; pcmpgt: gt) */
        switch (op) {
        case VOP_NEG:
            if (f) { out("%s %s, %%xmm0", ld, A); out("pcmpeqd %%xmm1, %%xmm1"); out("psll%s $%d, %%xmm1", es == 4 ? "d" : "q", es * 8 - 1); out("xorps %%xmm1, %%xmm0"); }
            else { out("pxor %%xmm0, %%xmm0"); out("%s %s, %%xmm1", ld, A); out("%s %%xmm1, %%xmm0", vec_insn(VOP_SUB, kind, es)); }
            break;
        case VOP_NOT: out("%s %s, %%xmm0", ld, A); out("pcmpeqd %%xmm1, %%xmm1"); out("pxor %%xmm1, %%xmm0"); break;
        case VOP_EQ: case VOP_NE: case VOP_LT: case VOP_LE: case VOP_GT: case VOP_GE:
            out("%s %s, %%xmm0", ld, swap ? B : A);
            out("%s %s, %%xmm1", ld, swap ? A : B);
            if (f) {                              /* cmpps: eq 0, lt 1, le 2, ne 4 */
                int p = op == VOP_EQ ? 0 : op == VOP_NE ? 4 : op == VOP_LT || op == VOP_GT ? 1 : 2;
                out("cmp%s $%d, %%xmm1, %%xmm0", es == 4 ? "ps" : "pd", p);
            } else {
                out("%s %%xmm1, %%xmm0", vec_insn(op == VOP_EQ || op == VOP_NE ? VOP_EQ : VOP_GT, kind, es));
                if (op == VOP_NE || op == VOP_LE || op == VOP_GE) { out("pcmpeqd %%xmm1, %%xmm1"); out("pxor %%xmm1, %%xmm0"); }
            }
            break;
        case VOP_SQRT: case VOP_CVTI2F: case VOP_CVTF2I: case VOP_CVTF2IR: case VOP_ABS:
            out("%s %s, %%xmm0", ld, A); out("%s %%xmm0, %%xmm0", vec_insn(op, kind, es)); break;
        case VOP_SHLI: case VOP_SHRI: case VOP_SARI: case VOP_SLLDQ: case VOP_SRLDQ:
            out("%s %s, %%xmm0", ld, A); out("%s $%ld, %%xmm0", vec_insn(op, kind, es), i->imm); break;
        case VOP_PSHUFD: out("%s %s, %%xmm0", ld, A); out("pshufd $%ld, %%xmm0, %%xmm0", i->imm); break;
        default:
            out("%s %s, %%xmm0", ld, A);
            out("%s %s, %%xmm1", ld, pb >= 0 ? B : A);
            out("%s %%xmm1, %%xmm0", vec_insn(op, kind, es));
        }
        out("%s %%xmm0, %d(%%%s)", ld, off, r64[pd]);
        if (n == 8) break;
    }
}

static void ins(Ins *i)
{
    char l1[32], l2[32];
    switch (i->op) {
    case O_NOP: return;
    case O_MOV:
        if (F->vr[i->dst].cls == C_FLT) {
            if (i->bimm) {
                int d = def_reg(i->dst, XMM0);
                if (i->imm == 0) out("xorps %%%s, %%%s", reg_name(d, 8), reg_name(d, 8));
                else if (i->w == 4) { out("movl $%ld, %%eax", (long)(unsigned)i->imm); out("movd %%eax, %%%s", reg_name(d, 8)); }
                else { out("movabsq $%ld, %%rax", i->imm); out("movq %%rax, %%%s", reg_name(d, 8)); }
                def_done(i->dst, d, i->w);
            } else movf(i->dst, i->a, i->w);
            return;
        }
        if (i->bimm) { mov_imm_to(i->imm, loc(i->dst, i->w), i->w, isreg(i->dst), isreg(i->dst) ? ph(i->dst) : 0); return; }
        movv(i->dst, i->a, i->w);
        return;
    case O_ADD:
        if (i->bimm && i->imm == 1 && isreg(i->dst) && isreg(i->a) && ph(i->dst) == ph(i->a)) { out("inc%c %s", sfx(i->w), loc(i->dst, i->w)); return; }
        if (isreg(i->dst) && isreg(i->a) && ph(i->dst) != ph(i->a) && (i->bimm || isreg(i->b)) && !(!i->bimm && ph(i->b) == ph(i->dst))) {
            /* three-operand add: lea */
            int w = i->w;
            if (i->bimm) out("lea%c %ld(%%%s), %%%s", sfx(w), w == 4 ? (long)(int)i->imm : i->imm, r64[ph(i->a)], reg_name(ph(i->dst), w));
            else out("lea%c (%%%s,%%%s), %%%s", sfx(w), r64[ph(i->a)], r64[ph(i->b)], reg_name(ph(i->dst), w));
            return;
        }
        binop(i, "add");
        return;
    case O_SUB: binop(i, "sub"); return;
    case O_AND: binop(i, "and"); return;
    case O_OR: binop(i, "or"); return;
    case O_XOR: binop(i, "xor"); return;
    case O_MUL: {
        int w = i->w;
        if (i->bimm) {
            int d = def_reg(i->dst, RAX);
            out("imul%c $%ld, %s, %%%s", sfx(w), i->imm, loc(i->a, w), reg_name(d, w));
            def_done(i->dst, d, w);
            return;
        }
        if (isreg(i->dst)) { binop(i, "imul"); return; }
        out("mov%c %s, %%%s", sfx(w), loc(i->a, w), reg_name(RAX, w));
        out("imul%c %s, %%%s", sfx(w), loc(i->b, w), reg_name(RAX, w));
        out("mov%c %%%s, %s", sfx(w), reg_name(RAX, w), loc(i->dst, w));
        return;
    }
    case O_SHL: shift(i, "shl"); return;
    case O_ROL: shift(i, "rol"); return;
    case O_VEC: vec(i); return;
    case O_SHR: shift(i, "shr"); return;
    case O_SAR: shift(i, "sar"); return;
    case O_DIV: case O_UDIV: case O_MOD: case O_UMOD: divide(i); return;
    case O_NEG: case O_NOT: {
        int d = def_reg(i->dst, RAX), w = i->w;
        if (!(isreg(i->a) && ph(i->a) == d)) out("mov%c %s, %%%s", sfx(w), loc(i->a, w), reg_name(d, w));
        out("%s%c %%%s", i->op == O_NEG ? "neg" : "not", sfx(w), reg_name(d, w));
        def_done(i->dst, d, w);
        return;
    }
    case O_EXT: extend(i); return;
    case O_SET: {
        int d = def_reg(i->dst, RAX);
        if (i->cc >= CC_FEQ) {
            int k = cmp_float(i);
            if (k == 1) { out("sete %%al"); out("setnp %%cl"); out("andb %%cl, %%al"); }
            else if (k == 2) { out("setne %%al"); out("setp %%cl"); out("orb %%cl, %%al"); }
            else out("set%s %%al", fcc(i->cc));
            out("movzbl %%al, %%%s", r32[d]);
        } else {
            cmp_int(i);
            out("set%s %%al", cc_name(i->cc));
            out("movzbl %%al, %%%s", r32[d]);
        }
        def_done(i->dst, d, 4);
        return;
    }
    case O_LOAD: load(i); return;
    case O_STORE: store(i); return;
    case O_LEA: {
        int d = def_reg(i->dst, RAX);
        if (i->m.tls) {                         /* lea ignores %fs: the thread pointer, plus the rest */
            Addr m = i->m;
            m.tls = 0;
            m.sym = 0;
            m.off = 0;
            const char *rest = m.base || m.index ? addr(&m, R11, RCX) : 0;
            if (rest) out("leaq %s, %%r11", rest);
            out("movq %%fs:0, %%%s", r64[d]);
            out("leaq %s@tpoff%+ld(%%%s%s), %%%s", i->m.sym, i->m.off, r64[d], rest ? ",%r11" : "", r64[d]);
            def_done(i->dst, d, 8);
            return;
        }
        out("leaq %s, %%%s", addr(&i->m, R11, RCX), r64[d]);
        def_done(i->dst, d, 8);
        return;
    }
    case O_FSQRT: {
        int d = def_reg(i->dst, XMM0);
        fget(i->a, i->w, d);
        out("sqrt%s %%%s, %%%s", fs(i->w), reg_name(d, 8), reg_name(d, 8));
        def_done(i->dst, d, i->w);
        return;
    }
    case O_FADD: fbin(i, "add"); return;
    case O_FSUB: fbin(i, "sub"); return;
    case O_FMUL: fbin(i, "mul"); return;
    case O_FDIV: fbin(i, "div"); return;
    case O_FNEG: {
        int d = def_reg(i->dst, XMM0);
        if (i->w == 4) { out("movl $0x80000000, %%eax"); out("movd %%eax, %%xmm1"); }
        else { out("movabsq $0x8000000000000000, %%rax"); out("movq %%rax, %%xmm1"); }
        fget(i->a, i->w, d);
        out("xorps %%xmm1, %%%s", reg_name(d, 8));
        def_done(i->dst, d, i->w);
        return;
    }
    case O_I2F: i2f(i); return;
    case O_F2I: f2i(i); return;
    case O_F2F: {
        int d = def_reg(i->dst, XMM0);
        out("cvts%c2s%c %s, %%%s", i->sz == 4 ? 's' : 'd', i->w == 4 ? 's' : 'd', loc(i->a, i->sz), reg_name(d, 8));
        def_done(i->dst, d, i->w);
        return;
    }
    case O_CALL: call(i); return;
    case O_BR: {
        label_of(i->t, l1);
        label_of(i->f, l2);
        if (i->cc >= CC_FEQ) {
            int k = cmp_float(i);
            if (k == 1) { out("jp %s", l2); out("je %s", l1); out("jmp %s", l2); return; }
            if (k == 2) { out("jp %s", l1); out("jne %s", l1); jmp(i->f); return; }
            if (i->t == nextb) { out("jn%s %s", fcc(i->cc), l2); return; }
            out("j%s %s", fcc(i->cc), l1);
            jmp(i->f);
            return;
        }
        cmp_int(i);
        if (i->t == nextb) { out("j%s %s", cc_name(cc_invert(i->cc)), l2); return; }
        out("j%s %s", cc_name(i->cc), l1);
        jmp(i->f);
        return;
    }
    case O_JMP: jmp(i->t); return;
    case O_RET:
        ret_value(i);
        if (nextb) out("jmp .Lret%d", fn_id);
        return;
    case O_SWITCH: {
        if (i->cc) { out("jmp *%%%s", r64[use_reg(i->a, RAX, 8)]); return; }     /* goto *p */
        int t = uniq++;
        int r = use_reg(i->a, RAX, 8);
        out("leaq .Ltab%d(%%rip), %%r11", t);
        out("jmp *(%%r11,%%%s,8)", r64[r]);
        buf_f(&tables, "\t.p2align 3\n.Ltab%d:\n", t);
        for (int k = 0; k < i->ntab; k++) { label_of(i->tab[k], l1); buf_f(&tables, "\t.quad %s\n", l1); }
        return;
    }
    case O_COPY: copy(i); return;
    case O_ZERO: zero(i); return;
    case O_ASM: asm_ins(i); return;
    case O_XADD: case O_XCHG: {
        int sz = i->sz, w = i->w;
        int p = use_reg(i->a, R11, 8);
        if (p == RAX) { out("movq %%rax, %%r11"); p = R11; }
        out("mov%c %s, %%%s", sfx(sz == 8 ? 8 : 4), loc(i->b, sz == 8 ? 8 : 4), reg_name(RAX, sz == 8 ? 8 : 4));
        out("%s%c %%%s, (%%%s)", i->op == O_XADD ? "lock xadd" : "xchg", sfx(sz), reg_name(RAX, sz), r64[p]);
        if (sz < 4) out("movz%cl %%%s, %%eax", sz == 1 ? 'b' : 'w', reg_name(RAX, sz));
        def_done(i->dst, RAX, w);
        if (isreg(i->dst)) out("mov%c %%%s, %%%s", sfx(w), reg_name(RAX, w), reg_name(ph(i->dst), w));
        return;
    }
    case O_CMPXCHG: {
        int sz = i->sz, w = i->w;
        int p = use_reg(i->a, R11, 8);
        out("mov%c %s, %%%s", sfx(sz == 8 ? 8 : 4), loc(i->b, sz == 8 ? 8 : 4), reg_name(RCX, sz == 8 ? 8 : 4));
        out("mov%c %s, %%%s", sfx(w), loc(i->m.base, w), reg_name(RAX, w));
        out("lock cmpxchg%c %%%s, (%%%s)", sfx(sz), reg_name(RCX, sz), r64[p]);
        if (sz < 4) out("mov%c%cl %%%s, %%eax", i->sgn ? 's' : 'z', sz == 1 ? 'b' : 'w', reg_name(RAX, sz));
        def_done(i->dst, RAX, w);
        if (isreg(i->dst)) out("mov%c %%%s, %%%s", sfx(w), reg_name(RAX, w), reg_name(ph(i->dst), w));
        return;
    }
    case O_FENCE: out("mfence"); return;
    case O_ALLOCA: {                            /* rsp -= the size, rounded up to 16 */
        out("movq %s, %%rax", loc(i->a, 8));
        out("addq $15, %%rax");
        out("andq $-16, %%rax");
        out("subq %%rax, %%rsp");
        int d = def_reg(i->dst, RAX);
        out("movq %%rsp, %%%s", r64[d]);
        def_done(i->dst, d, 8);
        return;
    }
    case O_SPSAVE: {
        int d = def_reg(i->dst, RAX);
        out("movq %%rsp, %%%s", r64[d]);
        def_done(i->dst, d, 8);
        return;
    }
    case O_SPRESTORE: out("movq %s, %%rsp", loc(i->a, 8)); return;
    case O_TLSADDR: {                           /* local-exec, or initial-exec for another module's variable */
        int d = def_reg(i->dst, RAX);
        if (i->imm) { out("movq %s@gottpoff(%%rip), %%%s", i->m.sym, r64[d]); out("addq %%fs:0, %%%s", r64[d]); }
        else { out("movq %%fs:0, %%%s", r64[d]); out("leaq %s@tpoff(%%%s), %%%s", i->m.sym, r64[d], r64[d]); }
        def_done(i->dst, d, 8);
        return;
    }
    case O_BITS: {                              /* movd/movq between an xmm and a general register */
        int tofl = F->vr[i->dst].cls == C_FLT;
        int a = use_reg(i->a, tofl ? RAX : XMM0, i->w);
        int d = def_reg(i->dst, tofl ? XMM0 : RAX);
        out("mov%c %%%s, %%%s", i->w == 8 ? 'q' : 'd', reg_name(a, i->w), reg_name(d, i->w));
        def_done(i->dst, d, i->w);
        return;
    }
    case O_LDBIN: case O_LDNEG: case O_LDCMP: case O_LDCVT: x87(i); return;
    case O_I128: case O_I128SH: case O_I128CMP: case O_I128EXT: i128(i); return;
    case O_ENTRY: entry(i); return;
    case O_UNREACH: out("ud2"); return;
    case O_BITOP: {
        int d = def_reg(i->dst, RAX), w = i->w;
        const char *src = loc(i->a, w);
        switch (i->imm) {
        case 0: out("bsr%c %s, %%%s", sfx(w), src, reg_name(d, w)); out("xor%c $%d, %%%s", sfx(w), w * 8 - 1, reg_name(d, w)); break;
        case 1: out("bsf%c %s, %%%s", sfx(w), src, reg_name(d, w)); break;
        case 2: out("popcnt%c %s, %%%s", sfx(w), src, reg_name(d, w)); break;
        default:
            if (!(isreg(i->a) && ph(i->a) == d)) out("mov%c %s, %%%s", sfx(w), src, reg_name(d, w));
            out("bswap%c %%%s", sfx(w), reg_name(d, w));
        }
        def_done(i->dst, d, w > 4 ? w : 4);
        return;
    }
    case O_MULSHR: {                            /* rdx:rax = a * b, then the shift */
        out("movq %s, %%rax", loc(i->a, 8));
        out("%s %s", i->sgn ? "imulq" : "mulq", loc(i->b, 8));
        long k = i->imm;
        if (k >= 64) { out("movq %%rdx, %%rax"); if (k > 64) out("%s $%ld, %%rax", i->sgn ? "sarq" : "shrq", k - 64); }
        else if (k) out("shrdq $%ld, %%rdx, %%rax", k);
        def_done(i->dst, RAX, 8);
        if (isreg(i->dst)) out("movq %%rax, %%%s", r64[ph(i->dst)]);
        return;
    }
    case O_FRAMEADDR: {
        int d = def_reg(i->dst, RAX);
        if (i->imm) out("movq 8(%%rbp), %%%s", r64[d]);
        else out("movq %%rbp, %%%s", r64[d]);
        def_done(i->dst, d, 8);
        return;
    }
    }
    die("internal: IR op %d", i->op);
}

/* ---- A function */
void emit_function(Buf *dst)
{
    o = dst;
    Obj *fn = F->obj;
    static int ids;
    fn_id = ids++;
    /* the frame: saved registers, then slots */
    nsaved = 0;
    for (int r = 0; r < 16; r++) if ((F->used_regs & BIT(r)) && (CALLEE_SAVED & BIT(r))) nsaved++;
    long size = nsaved * 8;
    int uses_frame = 0;
    for (int s = 1; s <= F->nslots; s++) {
        Slot *sl = &F->slots[s];
        size = align_to(size + sl->size, sl->align);
        sl->off = -size;
        uses_frame = 1;
    }
    long total = align_to(size, 16);
    F->frame = total - nsaved * 8;
    /* does it need a frame at all? */
    /* noframe 1: no frame at all (a leaf without slots); 2: no %rbp frame, only the
     * callee-saved registers pushed (and 8 bytes to keep calls 16-aligned) */
    int needs = uses_frame || fn->ty->is_variadic, pushes = nsaved || F->has_calls;
    for (Block *b = F->blocks; b && !needs; b = b->next)
        for (Ins *i = b->first; i; i = i->next) {
            if (i->op == O_FRAMEADDR || i->op == O_ASM || i->op == O_ALLOCA || i->op == O_SPSAVE) needs = 1;
            if ((i->op == O_LOAD || i->op == O_STORE || i->op == O_LEA) && i->m.args) needs = 1;
            if (i->op == O_ENTRY) for (int k = 0; k < i->npin; k++) if (i->pin[k].phys < 0) needs = 1;
        }
    noframe = needs || !opt_optimize ? 0 : pushes ? 2 : 1;
    const char *name = fn->asm_name;
    buf_f(o, "\t.section %s\n", fn->section ? fn->section : ".text");
    if (opt_optimize != 2) buf_s(o, "\t.p2align 4\n");
    if (!fn->is_static) buf_f(o, "\t.globl %s\n", name);
    buf_f(o, "\t.type %s, @function\n%s:\n", name, name);
    int src = src_pos(fn->tok);
    if (src) buf_f(o, "\t.loc %d %d\n", src >> 20, src & 0xFFFFF);
    if (noframe == 2) {
        static const int order[] = { RBX, R12, R13, R14, R15 };
        for (int k = 0; k < 5; k++) if (F->used_regs & BIT(order[k])) out("pushq %%%s", r64[order[k]]);
        if (F->has_calls && !(nsaved & 1)) out("subq $8, %%rsp");
    }
    if (!noframe) {
        out("pushq %%rbp");
        out("movq %%rsp, %%rbp");
        static const int order[] = { RBX, R12, R13, R14, R15 };
        for (int k = 0; k < 5; k++) if (F->used_regs & BIT(order[k])) out("pushq %%%s", r64[order[k]]);
        if (F->frame) out("subq $%ld, %%rsp", F->frame);
    }
    tables.len = 0;
    char l[32];
    /* loop heads (targets of a jump back) start on 16 bytes, at -O2, when that costs at most 10 */
    int nb = 0;
    for (Block *b = F->blocks; b; b = b->next) b->start = nb++;
    char *loophead = calloc(nb + 1, 1);
    if (!loophead) die("out of memory");
    if (opt_optimize == 1)
        for (Block *b = F->blocks; b; b = b->next) {
            Ins *t = b->last;
            if (t && (t->op == O_JMP || t->op == O_BR) && t->t->start <= b->start) loophead[t->t->start] = 1;
            if (t && t->op == O_BR && t->f->start <= b->start) loophead[t->f->start] = 1;
        }
    for (Block *b = F->blocks; b; b = b->next) {
        nextb = b->next;
        if (loophead[b->start] && b != F->blocks) buf_s(o, "\t.p2align 4,,10\n");
        label_of(b, l);
        buf_f(o, "%s:\n", l);
        if (b->sym) buf_f(o, "%s:\n", b->sym);
        for (Ins *i = b->first; i; i = i->next) {
            if (i->src && i->src != src) { src = i->src; buf_f(o, "\t.loc %d %d\n", src >> 20, src & 0xFFFFF); }
            ins(i);
        }
    }
    free(loophead);
    buf_f(o, ".Lret%d:\n", fn_id);
    epilogue();
    if (opt_debug) {                            /* for .debug_info: the function's end */
        buf_f(o, ".Lfe%d:\n", fn_id);
        vec_push(&dbg_funcs, fn);
        vec_push(&dbg_funcs, (void *)(long)fn_id);
    }
    buf_f(o, "\t.size %s, .-%s\n", name, name);
    if (tables.len) {
        buf_s(o, "\t.section .rodata\n");
        buf_add(o, tables.p, tables.len);
    }
}

/* ---- -g: DWARF 4. The assembler builds .debug_line from the .loc lines;
 * here: the files, a compile unit with one subprogram per function (name,
 * position, code range) and the address ranges. Variables and types are not
 * described (see docs/cc.md). */

static void emit_debug(Buf *o, const char *src)
{
    for (int k = 0; k < dbg_files.n; k++) buf_f(o, "\t.file %d \"%s\"\n", k + 1, ((File *)dbg_files.v[k])->name);
    buf_s(o, "\t.section .debug_abbrev\n.Ldebug_abbrev0:\n"
             /* 1: compile unit: producer, language, name, stmt_list, low_pc (0: addresses are absolute) */
             "\t.byte 1,0x11,1, 0x25,0x08, 0x13,0x0b, 0x03,0x08, 0x10,0x17, 0x11,0x01, 0,0\n"
             /* 2, 3: subprogram (external, static): name, decl_file, decl_line, low_pc, high_pc (a length) */
             "\t.byte 2,0x2e,0, 0x03,0x08, 0x3a,0x05, 0x3b,0x06, 0x3f,0x19, 0x11,0x01, 0x12,0x07, 0,0\n"
             "\t.byte 3,0x2e,0, 0x03,0x08, 0x3a,0x05, 0x3b,0x06, 0x11,0x01, 0x12,0x07, 0,0\n"
             "\t.byte 0\n");
    buf_s(o, "\t.section .debug_info\n.Ldebug_info0:\n\t.long .Ldi_end-.Ldi_start\n.Ldi_start:\n"
             "\t.value 4\n\t.long .Ldebug_abbrev0\n\t.byte 8\n");
    buf_f(o, "\t.byte 1\n\t.string \"sicc\"\n\t.byte 0x1d\n\t.string \"%s\"\n\t.long .Ldebug_line0\n\t.quad 0\n", src);
    for (int k = 0; k < dbg_funcs.n; k += 2) {
        Obj *fn = dbg_funcs.v[k];
        int id = (int)(long)dbg_funcs.v[k + 1], pos = src_pos(fn->tok);
        buf_f(o, "\t.byte %d\n\t.string \"%s\"\n\t.value %d\n\t.long %d\n\t.quad %s\n\t.quad .Lfe%d-%s\n",
              fn->is_static ? 3 : 2, fn->name, pos >> 20, pos & 0xFFFFF, fn->asm_name, id, fn->asm_name);
    }
    buf_s(o, "\t.byte 0\n.Ldi_end:\n");
    buf_s(o, "\t.section .debug_aranges\n\t.long .Lar_end-.Lar_start\n.Lar_start:\n"
             "\t.value 2\n\t.long .Ldebug_info0\n\t.byte 8,0\n\t.long 0\n");
    for (int k = 0; k < dbg_funcs.n; k += 2) {
        Obj *fn = dbg_funcs.v[k];
        buf_f(o, "\t.quad %s\n\t.quad .Lfe%d-%s\n", fn->asm_name, (int)(long)dbg_funcs.v[k + 1], fn->asm_name);
    }
    buf_s(o, "\t.quad 0,0\n.Lar_end:\n\t.section .debug_line\n.Ldebug_line0:\n");
    dbg_funcs.n = 0;
    dbg_files.n = 0;
}

/* ---- The program: data, then functions */
static int is_ro(Obj *g)
{
    if (g->is_str) return 1;
    Type *t = g->ty;
    while (t->kind == TY_ARRAY) t = t->base;
    return t->is_const && t->kind != TY_STRUCT && t->kind != TY_UNION;
}

static void emit_data(Obj *g, Buf *out)
{
    int align = g->align > g->ty->align ? g->align : g->ty->align;
    long size = g->ty->size;
    if (size == 0 && g->ty->kind != TY_FUNC) size = 0;
    const char *sec = g->section ? g->section : !g->has_init ? ".bss" : is_ro(g) && !g->is_tls ? ".rodata" : ".data";
    int zero = !g->has_init;
    if (g->has_init && !g->rel && !g->section && (!is_ro(g) || g->is_tls)) {      /* all zero: .bss */
        zero = 1;
        for (long i = 0; i < size; i++) if (g->data[i]) { zero = 0; break; }
        if (zero) sec = ".bss";
    }
    if (g->is_tls && !g->section) sec = zero ? ".tbss" : ".tdata";
    buf_f(out, "\t.section %s\n", sec);
    if (align > 1) buf_f(out, "\t.p2align %d\n", __builtin_ctz(align));
    if (!g->is_static) buf_f(out, "\t.globl %s\n", g->asm_name);
    buf_f(out, "\t.type %s, @%sobject\n\t.size %s, %ld\n%s:\n", g->asm_name, g->is_tls ? "tls_" : "", g->asm_name, size, g->asm_name);
    if (zero) { if (size) buf_f(out, "\t.zero %ld\n", size); return; }
    Reloc *r = g->rel;
    for (long i = 0; i < size; ) {
        if (r && r->off == i) {
            if (r->addend) buf_f(out, "\t.quad %s%+ld\n", r->sym, r->addend);
            else buf_f(out, "\t.quad %s\n", r->sym);
            i += 8;
            r = r->next;
            continue;
        }
        long end = r && r->off < size ? r->off : size;
        if (end - i > 16) end = i + 16;
        buf_s(out, "\t.byte ");
        for (long k = i; k < end; k++) buf_f(out, k > i ? ",%d" : "%d", (unsigned char)g->data[k]);
        buf_c(out, '\n');
        i = end;
    }
}

static void mark(Obj *g)
{
    if (g->emitted) return;
    g->emitted = 1;
    for (int k = 0; k < g->refs.n; k++) mark(g->refs.v[k]);
}

static void emit_one(Obj *g, Buf *out)
{
    lower_function(g);
    if (opt_optimize) optimize();
    regalloc();
    if (target == T_AARCH64) {
        emit_function_a64(out);
        if (opt_debug) { vec_push(&dbg_funcs, g); vec_push(&dbg_funcs, (void *)(long)a64_fn_id()); }
    } else emit_function(out);
    free(F->vr);
    free(F->slots);
}

void gen_program(Obj *prog, Buf *out, const char *src)
{
    for (Obj *g = prog; g; g = g->next) {
        if (!g->is_def) continue;
        if (!g->is_static || g->used || (!g->is_func && !g->is_str && strncmp(g->asm_name, ".L", 2) && !g->is_inline && g->name == g->asm_name)) {
            if (g->is_static && !g->used && !g->is_func) continue;          /* static data: only if used */
            mark(g);
        }
    }
    for (Obj *g = prog; g; g = g->next) {
        if (!g->is_def || !g->emitted || g->is_func) continue;
        emit_data(g, out);
    }
    /* inlining: who uses each function, and how (calls, or other references) */
    for (Obj *g = prog; g; g = g->next) for (int k = 0; k < g->refs.n; k++) ((Obj *)g->refs.v[k])->nrefs++;
    for (Obj *g = prog; g; g = g->next) if (g->is_func && g->body) count_calls(g->body, g);
    for (Obj *g = prog; g; g = g->next)
        g->inl_all = opt_optimize && g->is_func && g->is_def && g->is_static && !g->used && !g->section && !g->noinline &&
                     g->ncalls > 0 && g->ncalls == g->nrefs && g->ncalls != -1 && inline_candidate(g);
    for (Obj *g = prog; g; g = g->next) {
        if (!g->is_def || !g->emitted || !g->is_func || g->inl_all) continue;
        emit_one(g, out);
    }
    for (int more = 1; more; ) {                  /* functions to inline everywhere that were called after all */
        more = 0;
        for (Obj *g = prog; g; g = g->next)
            if (g->inl_all && g->inl_needed && g->emitted && g->inl_all != 2) { g->inl_all = 2; emit_one(g, out); more = 1; }
    }
    if (global_asm.len) {
        buf_s(out, "\t.text\n");
        buf_add(out, global_asm.p, global_asm.len);
    }
    if (opt_debug) emit_debug(out, src);
    buf_s(out, "\t.section .note.GNU-stack,\"\",@progbits\n");
}
