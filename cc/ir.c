/*
 * ir.c - From a function's syntax tree to IR (see ir.h).
 *
 * Expressions become instructions over new virtual registers; conditions
 * become branches (a && b never computes a 0/1 value it does not need).
 * Local variables whose address is never taken live in virtual registers;
 * the others, and all arrays and structs, get a slot in the stack frame.
 * Struct values are handled by address.
 *
 * Calls follow the x86-64 System V ABI (AArch64: AAPCS64, ir_a64.inc): classify() decides which struct
 * arguments travel in registers, and how.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ir.h"

Fn *F;
int opt_kernel_model, opt_optimize = 1, opt_debug, opt_avx, opt_pic;

static Block *cur;                       /* where instructions go */
static Map labels;                       /* label name -> block */
static Vec addr_blocks, cgotos;          /* labels whose address is taken; computed gotos (their targets) */
static int retptr;                       /* the hidden struct-return pointer */

static int ngp_named, nfp_named;         /* variadic: registers taken by named parameters */
static long stack_named;                 /* ... and bytes on the stack */

typedef struct { int v; long imm; int k; } Op;    /* a value: a virtual register, or (k=1) a constant */
static Op self_val;                       /* ND_SELF: the lvalue's value in x op= y */

static Op R(int v) { Op o = { v, 0, 0 }; return o; }
static Op I(long imm) { Op o = { 0, imm, 1 }; return o; }

/* ---- Building blocks */
int new_vreg(int cls)
{
    if (F->nvr + 1 >= F->capvr) {
        F->capvr = F->capvr ? F->capvr * 2 : 256;
        F->vr = realloc(F->vr, sizeof(VReg) * F->capvr);
        if (!F->vr) die("out of memory");
    }
    VReg *r = &F->vr[++F->nvr];
    memset(r, 0, sizeof *r);
    r->cls = cls;
    r->phys = -1;
    r->hint = -1;
    r->w = 4;
    return F->nvr;
}

int new_slot(int size, int align)
{
    if (F->nslots + 1 >= F->capslots) {
        F->capslots = F->capslots ? F->capslots * 2 : 64;
        F->slots = realloc(F->slots, sizeof(Slot) * F->capslots);
        if (!F->slots) die("out of memory");
    }
    Slot *s = &F->slots[++F->nslots];
    s->size = size < 1 ? 1 : size;
    s->align = align < 1 ? 1 : align;
    s->off = 0;
    return F->nslots;
}

static Block *new_block(void)
{
    Block *b = arena(sizeof *b);
    b->id = F->nblocks++;
    return b;
}

static int terminated(Block *b)
{
    if (!b->last) return 0;
    int op = b->last->op;
    return op == O_BR || op == O_JMP || op == O_RET || op == O_SWITCH || op == O_UNREACH;
}

/* -g: the position of the statement being lowered; positions of macro
 * expansions are those of the macro's use */
Vec dbg_files;
static int cur_src;

int src_pos(Token *t)
{
    if (!opt_debug || !t) return 0;
    while (t->origin) t = t->origin;
    if (!t->file || t->line <= 0 || t->line >= 1 << 20) return 0;
    int k = 0;
    while (k < dbg_files.n && dbg_files.v[k] != t->file) k++;
    if (k == dbg_files.n) vec_push(&dbg_files, t->file);
    return (k + 1) << 20 | t->line;
}

static Ins *emit(int op)
{
    Ins *i = arena(sizeof *i);
    i->op = op;
    i->src = cur_src;
    if (cur->last) cur->last->next = i; else cur->first = i;
    cur->last = i;
    return i;
}

static void jump(Block *to) { if (!terminated(cur)) { Ins *i = emit(O_JMP); i->t = to; } }

static void place(Block *b)             /* continue in block b (falling through into it) */
{
    jump(b);
    if (F->last) F->last->next = b; else F->blocks = b;
    F->last = b;
    cur = b;
}

static void dead_code(void) { place(new_block()); }   /* after return, goto...: unreachable */

static int is_ld(Type *t) { return t->kind == TY_LDOUBLE; }
static int is_i128(Type *t) { return t->kind == TY_INT128; }
static int is_cplx(Type *t) { return t->kind == TY_COMPLEX; }

static int wof(Type *t)
{
    if (is_ld(t) || is_i128(t) || is_cplx(t)) return 8;   /* (its address) */
    if (is_flonum(t)) return t->size;
    return t->size == 8 || t->kind == TY_PTR || t->kind == TY_ARRAY || t->kind == TY_FUNC ||
           t->kind == TY_STRUCT || t->kind == TY_UNION ? 8 : 4;
}
static int clsof(Type *t) { return is_flonum(t) && !is_ld(t) ? C_FLT : C_INT; }
static int is_agg(Type *t) { return t->kind == TY_ARRAY || t->kind == TY_STRUCT || t->kind == TY_UNION || t->kind == TY_FUNC; }
/* values handled by their address: aggregates, and long doubles (x87 has no
 * registers the allocator could give out: they live in 16-byte slots) */
static int is_mem(Type *t) { return is_agg(t) || is_ld(t) || is_i128(t) || is_cplx(t); }

static void need_float(Node *n)
{
    if (opt_general_regs_only) error_at(n->tok, "floating point is not allowed with -mgeneral-regs-only");
}

/* ---- Small instruction helpers */
static int mov_imm(long imm, int w)
{
    int v = new_vreg(C_INT);
    Ins *i = emit(O_MOV);
    i->dst = v;
    i->bimm = 1;
    i->imm = imm;
    i->w = w;
    return v;
}

static int reg(Op o, int w) { return o.k ? mov_imm(o.imm, w) : o.v; }

static int copy(int v, int cls, int w)
{
    int d = new_vreg(cls);
    Ins *i = emit(O_MOV);
    i->dst = d;
    i->a = v;
    i->w = w;
    return d;
}

static void move(int dst, Op src, int w)
{
    Ins *i = emit(O_MOV);
    i->dst = dst;
    if (src.k) { i->bimm = 1; i->imm = src.imm; } else i->a = src.v;
    i->w = w;
}

static int fits32(long v) { return v == (long)(int)v; }

static Op arith(int op, Op a, Op b, int w);

/* ---- Division by a constant: a multiplication by its reciprocal, the high
 * half of the 128-bit product, shifted (Granlund and Montgomery, "Division by
 * invariant integers using multiplication", 1994). m = ceil(2^(n+l) / d) is
 * exact for every n-bit dividend when m*d - 2^(n+l) <= 2^l. */
typedef unsigned __int128 u128;

static int magic(unsigned long d, int n, u128 limit, unsigned long *m)   /* the smallest such l (-1: none) */
{
    for (int l = 0; l < 64; l++) {
        u128 p = (u128)1 << (n + l), mm = (p + d - 1) / d;
        if (mm < limit && mm * d - p <= (u128)1 << l) { *m = (unsigned long)mm; return l; }
    }
    return -1;
}

static Op mulshr(Op x, unsigned long m, int k, int sgn)   /* (128-bit x * m) >> k */
{
    int a = reg(x, 8), b = reg(I((long)m), 8), d = new_vreg(C_INT);
    Ins *i = emit(O_MULSHR);
    i->dst = d; i->a = a; i->b = b; i->imm = k; i->w = 8; i->sgn = sgn;
    return R(d);
}

static Op ext64(Op a, int sgn)                             /* a 4-byte value, extended */
{
    if (a.k) return I(sgn ? (long)(int)a.imm : (long)(unsigned)a.imm);
    int d = new_vreg(C_INT);
    Ins *i = emit(O_EXT);
    i->dst = d; i->a = a.v; i->sz = 4; i->sgn = sgn; i->w = 8;
    return R(d);
}

static int div_const(Op a, long dv, int sgn, int w, Op *q)   /* *q = a / dv, toward zero; 0: not done */
{
    unsigned long m = 0;
    if (!sgn) {
        unsigned long d = w == 4 ? (unsigned)dv : (unsigned long)dv;
        if (d < 2) return 0;
        if (w == 4) { int l = magic(d, 32, (u128)1 << 64, &m); if (l < 0) return 0; *q = mulshr(ext64(a, 0), m, 32 + l, 0); return 1; }
        int l = magic(d, 64, (u128)1 << 64, &m);
        if (l >= 0) { *q = mulshr(a, m, 64 + l, 0); return 1; }
        /* a 65-bit multiplier: q = (t + ((a - t) >> 1)) >> (L - 1), t the high half of a * (m - 2^64) */
        int L = 64 - __builtin_clzl(d - 1);
        unsigned long mm = (unsigned long)((((u128)1 << 64) * (((u128)1 << L) - d)) / d + 1);
        Op t = mulshr(a, mm, 64, 0);
        *q = arith(O_SHR, arith(O_ADD, t, arith(O_SHR, arith(O_SUB, a, t, 8), I(1), 8), 8), I(L - 1), 8);
        return 1;
    }
    if (dv == (long)(-0x7fffffffffffffffL - 1) || (w == 4 && (dv == (int)0x80000000 || dv != (int)dv))) return 0;
    unsigned long d = dv < 0 ? -(unsigned long)dv : (unsigned long)dv;
    if (d < 2) return 0;
    Op x = w == 4 ? ext64(a, 1) : a;
    int l = magic(d, w * 8 - 1, (u128)1 << 63, &m);
    if (l < 0) return 0;
    Op neg = arith(O_SHR, x, I(63), 8);         /* (operands made in a fixed order: C leaves argument order open) */
    Op r = arith(O_ADD, mulshr(x, m, w * 8 - 1 + l, 1), neg, 8);   /* floor, +1 below zero */
    *q = dv < 0 ? arith(O_SUB, I(0), r, w) : r;
    return 1;
}

/* dst = a op b, folding constants */
static Op arith(int op, Op a, Op b, int w)
{
    if (a.k && b.k) {
        long x = a.imm, y = b.imm, r;
        int ok = 1;
        switch (op) {
        case O_ADD: r = x + y; break;
        case O_SUB: r = x - y; break;
        case O_MUL: r = x * y; break;
        case O_AND: r = x & y; break;
        case O_OR: r = x | y; break;
        case O_XOR: r = x ^ y; break;
        case O_SHL: r = (unsigned long)x << (y & (w * 8 - 1)); break;
        case O_SHR: r = w == 4 ? (long)((unsigned)x >> (y & 31)) : (long)((unsigned long)x >> (y & 63)); break;
        case O_SAR: r = w == 4 ? (long)((int)x >> (y & 31)) : x >> (y & 63); break;
        case O_DIV: ok = y != 0 && !(y == -1); if (ok) r = w == 4 ? (int)x / (int)y : x / y; break;
        case O_MOD: ok = y != 0 && !(y == -1); if (ok) r = w == 4 ? (int)x % (int)y : x % y; break;
        case O_UDIV: ok = y != 0; if (ok) r = w == 4 ? (long)((unsigned)x / (unsigned)y) : (long)((unsigned long)x / y); break;
        case O_UMOD: ok = y != 0; if (ok) r = w == 4 ? (long)((unsigned)x % (unsigned)y) : (long)((unsigned long)x % y); break;
        default: ok = 0;
        }
        if (ok) return I(w == 4 ? (long)(int)r : r);
    }
    /* identities */
    if (b.k && ((op == O_ADD || op == O_SUB || op == O_OR || op == O_XOR || op == O_SHL || op == O_SHR || op == O_SAR) && b.imm == 0)) return a;
    if (b.k && (op == O_MUL || op == O_DIV || op == O_UDIV) && b.imm == 1) return a;
    if (a.k && (op == O_ADD || op == O_MUL || op == O_AND || op == O_OR || op == O_XOR)) { Op t = a; a = b; b = t; }
    /* multiplication by a power of 2: a shift */
    if (b.k && op == O_MUL && b.imm > 0 && !(b.imm & (b.imm - 1))) {
        int s = 0;
        while ((1L << s) != b.imm) s++;
        op = O_SHL;
        b.imm = s;
    }
    if (b.k && op == O_UDIV && b.imm > 0 && !(b.imm & (b.imm - 1))) {
        int s = 0;
        while ((1L << s) != b.imm) s++;
        op = O_SHR;
        b.imm = s;
    }
    if (b.k && op == O_UMOD && b.imm > 0 && !(b.imm & (b.imm - 1))) { op = O_AND; b.imm -= 1; }
    if (b.k && (op == O_DIV || op == O_MOD) && b.imm > 1 && !(b.imm & (b.imm - 1)) && !a.k) {
        /* signed division by 2^k: shifts, rounding toward zero (add 2^k-1 to negatives first) */
        int k = 0, bits = w * 8;
        while ((1L << k) != b.imm) k++;
        Op bias = arith(O_SHR, arith(O_SAR, a, I(bits - 1), w), I(bits - k), w);
        Op t = arith(O_ADD, a, bias, w);
        if (op == O_DIV) return arith(O_SAR, t, I(k), w);
        return arith(O_SUB, a, arith(O_AND, t, I(-b.imm), w), w);
    }
    if (b.k && !a.k && opt_optimize == 1 && (op == O_DIV || op == O_MOD || op == O_UDIV || op == O_UMOD)) {
        Op q;
        if (div_const(a, b.imm, op == O_DIV || op == O_MOD, w, &q))
            return op == O_DIV || op == O_UDIV ? q : arith(O_SUB, a, arith(O_MUL, q, b, w), w);
    }
    int ra = reg(a, w), rb = 0;               /* operands before the instruction */
    int bimm = b.k && fits32(b.imm) && op != O_DIV && op != O_MOD && op != O_UDIV && op != O_UMOD;
    if (!bimm) rb = reg(b, w);
    int d = new_vreg(C_INT);
    Ins *i = emit(op);
    i->dst = d;
    i->a = ra;
    i->w = w;
    if (bimm) { i->bimm = 1; i->imm = b.imm; }
    else i->b = rb;
    return R(d);
}

static int lea(Addr m)
{
    if (m.base && !m.index && !m.off && !m.sym && !m.slot && !m.args) return m.base;
    int v = new_vreg(C_INT);
    Ins *i = emit(O_LEA);
    i->dst = v;
    i->m = m;
    i->w = 8;
    return v;
}

static Addr addr_of_reg(int v) { Addr a = { 0 }; a.base = v; a.scale = 1; return a; }
static Addr slot_addr(int slot) { Addr a = { 0 }; a.slot = slot; a.scale = 1; return a; }

static Op load(Addr m, Type *ty)
{
    if (is_mem(ty)) return R(lea(m));
    int cls = clsof(ty);
    int v = new_vreg(cls);
    Ins *i = emit(O_LOAD);
    i->dst = v;
    i->m = m;
    i->sz = ty->size;
    i->w = wof(ty);
    i->sgn = !ty->is_unsigned && ty->kind != TY_PTR;
    i->cc = ty->is_volatile || ty->is_atomic;  /* a volatile (or atomic) load is never removed or merged */
    return R(v);
}

static void store_sz(Addr m, Op val, int size, int cls)
{
    int imm = val.k && (size < 8 || fits32(val.imm));
    int r = imm ? 0 : reg(val, 8);
    Ins *i = emit(O_STORE);
    i->m = m;
    i->sz = size;
    i->w = cls == C_FLT ? size : size == 8 ? 8 : 4;
    if (imm) { i->bimm = 1; i->imm = val.imm; }
    else i->a = r;
}

static void store(Addr m, Op val, Type *ty) { store_sz(m, val, ty->size, clsof(ty)); }

static void copy_mem(int dst, int src, long size)
{
    if (size <= 0) return;
    Ins *i = emit(O_COPY);
    i->a = dst;
    i->b = src;
    i->imm = size;
    if (size > 64 && target == T_X86_64) i->clob = BIT(RSI) | BIT(RDI) | BIT(RCX);   /* (rep movsb) */
}

static void zero_mem(int dst, long size)
{
    if (size <= 0) return;
    Ins *i = emit(O_ZERO);
    i->a = dst;
    i->imm = size;
    if (size > 64 && target == T_X86_64) i->clob = BIT(RDI) | BIT(RCX) | BIT(RAX);
}

/* ---- long double: a 16-byte slot per value, x87 instructions between them */
static int ld_slot(void) { return lea(slot_addr(new_slot(16, 16))); }

static Op ld_const(long double v)
{
    int a = ld_slot();
    unsigned long lo = 0;
    unsigned short hi = 0;
    memcpy(&lo, &v, 8);
    memcpy(&hi, (char *)&v + 8, 2);
    Addr m = addr_of_reg(a);
    store_sz(m, I(lo), 8, C_INT);
    m.off = 8;
    store_sz(m, I(hi), 2, C_INT);
    return R(a);
}

static Op ld_op(int op, int k, int a, int b)    /* a new value: a op b, or -a */
{
    int d = ld_slot();
    Ins *i = emit(op);
    i->a = a;
    i->b = b;
    i->imm = k;
    i->m = addr_of_reg(d);
    return R(d);
}

static int ld_cmp(int a, int b, int cc)        /* 0/1 */
{
    int d = new_vreg(C_INT);
    Ins *i = emit(O_LDCMP);
    i->dst = d; i->a = a; i->b = b; i->cc = cc; i->w = 4;
    return d;
}

/* ---- __int128: a 16-byte slot per value; operations on the two halves */
static Op i128_op(int op, int k, int a, int b)   /* a new value: *a op *b (b: 0 for neg, not) */
{
    int d = ld_slot();
    Ins *i = emit(op);
    i->a = a;
    i->b = b;
    i->imm = k;
    i->m = addr_of_reg(d);
    return R(d);
}

static Op i128_ext(Op v, int sgn)              /* from a 64-bit integer */
{
    int x = reg(v, 8), d = ld_slot();
    Ins *i = emit(O_I128EXT);
    i->a = x;
    i->sgn = sgn;
    i->m = addr_of_reg(d);
    return R(d);
}

static int i128_cmp(int a, int b, int cc)
{
    int d = new_vreg(C_INT);
    Ins *i = emit(O_I128CMP);
    i->dst = d; i->a = a; i->b = b; i->cc = cc; i->w = 4;
    return d;
}

static Op gen_call(Node *n);

/* A call to a run-time helper (__divti3, __muldc3...) with values already computed. */
static Op helper_n(const char *sym, Type *ret, int n, Type **ty, Op *v)
{
    Type *ft = func_type(ret);
    Node *call = arena(sizeof *call), *fn = arena(sizeof *fn), head = { 0 }, *cur = &head;
    Obj *f = arena(sizeof *f);
    f->name = f->asm_name = sym;
    f->ty = ft;
    f->is_func = 1;
    fn->kind = ND_VAR; fn->var = f; fn->ty = ft;
    Type ph = { 0 }, *pc = &ph;
    for (int k = 0; k < n; k++) {
        Node *a = arena(sizeof *a);
        a->kind = ND_IRVAL; a->ty = ty[k]; a->val = v[k].k ? v[k].imm : v[k].v; a->lo = v[k].k;
        cur = cur->next = a;
        pc = pc->next = copy_type(ty[k]);
    }
    ft->params = ph.next;
    call->kind = ND_CALL; call->lhs = fn; call->args = head.next; call->fty = ft; call->ty = ret;
    return gen_call(call);
}

static Op helper(const char *sym, Type *ret, Type *t1, Op v1, Type *t2, Op v2)
{
    Type *t[2] = { t1, t2 };
    Op v[2] = { v1, v2 };
    return helper_n(sym, ret, t2 ? 2 : 1, t, v);
}

/* ---- _Complex: a slot with the real part, then the imaginary one */
static Op cpart(int a, Type *ct, int k)        /* a float/double in a register, or a long double's address */
{
    Addr m = addr_of_reg(a);
    m.off = k * ct->base->size;
    return load(m, ct->base);
}

static Op fconst(double d, int size);
static Op czero(Type *b) { return is_ld(b) ? ld_const(0) : fconst(0, b->size); }

static Op cmake(Type *ct, Op re, Op im)
{
    int d = lea(slot_addr(new_slot(ct->size, ct->align)));
    for (int k = 0; k < 2; k++) {
        Op v = k ? im : re;
        Addr m = addr_of_reg(d);
        m.off = k * ct->base->size;
        if (is_ld(ct->base)) copy_mem(lea(m), reg(v, 8), 16);
        else store(m, R(reg(v, ct->base->size)), ct->base);
    }
    return R(d);
}

static Op fop(int kind, Type *b, Op x, Op y)   /* one real operation on the parts' type */
{
    int k = kind == ND_ADD ? 0 : kind == ND_SUB ? 1 : kind == ND_MUL ? 2 : 3;
    if (is_ld(b)) {
        if (kind == ND_NEG) return ld_op(O_LDNEG, 0, reg(x, 8), 0);
        int ry = reg(y, 8), rx = reg(x, 8);
        return ld_op(O_LDBIN, k, rx, ry);
    }
    int d = new_vreg(C_FLT);
    Ins *i = emit(kind == ND_NEG ? O_FNEG : k == 0 ? O_FADD : k == 1 ? O_FSUB : k == 2 ? O_FMUL : O_FDIV);
    i->dst = d; i->a = reg(x, b->size); i->w = b->size;
    if (kind != ND_NEG) i->b = reg(y, b->size);
    return R(d);
}

static Op fcmp(Type *b, Op x, Op y, int cc)    /* 0/1 */
{
    if (is_ld(b)) { int ry = reg(y, 8), rx = reg(x, 8); return R(ld_cmp(rx, ry, cc)); }
    int d = new_vreg(C_INT);
    Ins *i = emit(O_SET);
    i->dst = d; i->a = reg(x, b->size); i->b = reg(y, b->size); i->cc = cc; i->w = b->size;
    return R(d);
}

static const char *csuffix(Type *ct) { return ct->base->kind == TY_FLOAT ? "sc3" : ct->base->kind == TY_DOUBLE ? "dc3" : "xc3"; }

/* ---- Variables */
static Addr var_addr(Obj *v)
{
    Addr a = { 0 };
    a.scale = 1;
    if (v->is_tls && v->is_def) { a.sym = v->asm_name; a.tls = 1; return a; }   /* local-exec */
    if (v->is_tls) {                               /* initial-exec: the offset from the GOT */
        Ins *i = emit(O_TLSADDR);
        i->dst = a.base = new_vreg(C_INT);
        i->m.sym = v->asm_name;
        i->imm = 1;
        i->w = 8;
        return a;
    }
    if (!v->is_local) { a.sym = v->asm_name; return a; }
    if (v->slot == -1) { a.args = 1; a.off = v->param_stack_off; return a; }
    if (v->slot == -2) { a.base = v->ref_vreg; return a; }
    if (!v->slot) die("internal: variable %s has no storage", v->name);
    a.slot = v->slot;
    return a;
}

static Op gen(Node *n);
static Addr gen_addr(Node *n);
static void gen_cond(Node *n, Block *t, Block *f);
static void gen_stmt(Node *n);

static int gen_reg(Node *n)
{
    Op o = gen(n);
    add_type(n);
    return reg(o, wof(n->ty));
}

/* ---- Bit-fields. A bit-field lives in a unit of m->unit bytes at its
 * offset: the type's size normally, and for a packed struct exactly the
 * bytes its bits cover (1 to 9: never a byte outside the field). */
static Op cast(Op v, Type *from, Type *to, Node *n);

static Op load_bytes(Addr a, int n)          /* n (1-8) bytes, zero-extended to 64 bits */
{
    if (n == 1 || n == 2 || n == 4 || n == 8) return load(a, n == 8 ? ty_ulong : n == 4 ? ty_uint : n == 2 ? ty_ushort : ty_uchar);
    Op v = I(0);
    for (int done = 0; done < n; ) {          /* pieces of 4, 2, 1 bytes, low to high */
        int k = n - done >= 4 ? 4 : n - done >= 2 ? 2 : 1;
        Addr p = a;
        p.off += done;
        Op x = cast(load(p, k == 4 ? ty_uint : k == 2 ? ty_ushort : ty_uchar), ty_uint, ty_ulong, 0);
        v = arith(O_OR, v, arith(O_SHL, x, I(done * 8), 8), 8);
        done += k;
    }
    return v;
}

static void store_bytes(Addr a, int n, Op v)  /* the low n (1-8) bytes of v */
{
    for (int done = 0; done < n; ) {
        int k = n - done == 8 ? 8 : n - done >= 4 ? 4 : n - done >= 2 ? 2 : 1;
        Addr p = a;
        p.off += done;
        store_sz(p, done ? arith(O_SHR, v, I(done * 8), 8) : v, k, C_INT);
        done += k;
    }
}

/* the field's value from its unit (bits from bit_off, width bit_width) */
static Op bitfield_value(Op unit, Member *m, int W)
{
    int bits = W * 8;
    if (m->ty->is_unsigned || m->ty->kind == TY_BOOL) {
        Op v = arith(O_SHR, unit, I(m->bit_off), W);
        if (m->bit_width < bits) v = arith(O_AND, v, I(m->bit_width == 32 ? 0xffffffffL : (1L << m->bit_width) - 1), W);
        return v;
    }
    Op v = arith(O_SHL, unit, I(bits - m->bit_off - m->bit_width), W);
    return arith(O_SAR, v, I(bits - m->bit_width), W);
}

static int bf_width(Member *m) { return m->ty->size == 8 || m->unit > 4 ? 8 : 4; }

static Op bitfield_load(Addr a, Member *m)
{
    Addr u = a;
    u.off += m->offset;
    int W = bf_width(m);
    if (m->unit <= 8) {
        Op v = bitfield_value(load_bytes(u, m->unit), m, W);
        return W == 8 && m->ty->size < 8 ? cast(v, ty_long, m->ty->is_unsigned ? ty_uint : ty_int, 0) : v;
    }
    /* 9 bytes: a 64-bit field (or nearly) starting inside a byte */
    Addr h = u;
    h.off += 8;
    Op lo = arith(O_SHR, load_bytes(u, 8), I(m->bit_off), 8);
    Op hi = arith(O_SHL, load_bytes(h, 1), I(64 - m->bit_off), 8);
    Op v = arith(O_OR, lo, hi, 8);
    if (m->bit_width == 64) return v;
    Member whole = *m;
    whole.bit_off = 0;
    return bitfield_value(v, &whole, 8);
}

static Op bitfield_store(Addr a, Member *m, Op val)
{
    Addr u = a;
    u.off += m->offset;
    int W = bf_width(m);
    unsigned long field = m->bit_width == 64 ? ~0UL : (1UL << m->bit_width) - 1;
    if (W == 8 && m->ty->size < 8) val = cast(val, m->ty->is_unsigned ? ty_uint : ty_int, ty_ulong, 0);
    if (m->unit <= 8) {
        unsigned long mask = field << m->bit_off;
        Op old = load_bytes(u, m->unit);
        Op cleared = arith(O_AND, old, I(W == 4 ? (long)(unsigned)~mask : (long)~mask), W);
        Op nv = arith(O_AND, arith(O_SHL, val, I(m->bit_off), W), I(W == 4 ? (long)(unsigned)mask : (long)mask), W);
        Op merged = arith(O_OR, cleared, nv, W);
        store_bytes(u, m->unit, merged);
        Op r = bitfield_value(merged, m, W);
        return W == 8 && m->ty->size < 8 ? cast(r, ty_long, m->ty->is_unsigned ? ty_uint : ty_int, 0) : r;
    }
    Addr h = u;
    h.off += 8;
    unsigned long lomask = ~0UL << m->bit_off;                 /* bits of the field in the first 8 bytes */
    Op sh = arith(O_SHL, val, I(m->bit_off), 8);
    Op lo = arith(O_OR, arith(O_AND, load_bytes(u, 8), I(~lomask), 8), sh, 8);
    store_bytes(u, 8, lo);
    int hibits = m->bit_off + m->bit_width - 64;
    long himask = (1L << hibits) - 1;
    Op hv = arith(O_AND, arith(O_SHR, val, I(64 - m->bit_off), 8), I(himask), 8);
    Op hi = arith(O_OR, arith(O_AND, load_bytes(h, 1), I(~himask & 255), 8), hv, 8);
    store_bytes(h, 1, hi);
    if (m->bit_width == 64) return val;
    Member whole = *m;
    whole.bit_off = 0;
    return bitfield_value(val, &whole, 8);
}

/* ---- Conversions */
static Op cast(Op v, Type *from, Type *to, Node *n)
{
    if (to->kind == TY_VOID) return v;
    if (is_cplx(to) || is_cplx(from)) {
        need_float(n);
        if (is_cplx(to) && is_cplx(from)) {
            if (to->base->kind == from->base->kind) return v;
            Op im = cast(cpart(v.v, from, 1), from->base, to->base, n), re = cast(cpart(v.v, from, 0), from->base, to->base, n);
            return cmake(to, re, im);
        }
        if (is_cplx(to)) { Op z = czero(to->base); return cmake(to, cast(v, from, to->base, n), z); }
        if (to->kind == TY_BOOL) {                 /* re != 0 || im != 0 */
            Op im = cast(cpart(v.v, from, 1), from->base, ty_bool, n), re = cast(cpart(v.v, from, 0), from->base, ty_bool, n);
            return arith(O_OR, re, im, 4);
        }
        return cast(cpart(v.v, from, 0), from->base, to, n);   /* the real part */
    }
    if ((is_ld(to) || is_ld(from)) && !is_i128(to) && !is_i128(from)) {   /* (with __int128: below) */
        need_float(n);
        if (is_ld(to) && is_ld(from)) return v;
        if (to->kind == TY_BOOL) return R(ld_cmp(reg(v, 8), 0, CC_FNE));
        if (is_ld(to)) {                           /* to long double */
            int d = ld_slot();
            Ins *i;
            if (is_flonum(from)) { int x = reg(v, from->size); i = emit(O_LDCVT); i->imm = 2; i->a = x; i->sz = from->size; }
            else {
                if (is_agg(from)) from = ty_ulong;
                int sz = from->size < 4 ? 4 : from->size;   /* char, short: already extended */
                int x = reg(v, sz == 8 ? 8 : 4);
                i = emit(O_LDCVT); i->imm = 0; i->a = x; i->sz = sz;
                i->sgn = !(from->is_unsigned && from->size >= 4);
            }
            i->m = addr_of_reg(d);
            return R(d);
        }
        if (is_flonum(to)) {                       /* to float, double */
            int d = new_vreg(C_FLT);
            Ins *i = emit(O_LDCVT);
            i->imm = 3; i->a = reg(v, 8); i->dst = d; i->w = to->size; i->m.slot = new_slot(16, 16);
            return R(d);
        }
        int d = new_vreg(C_INT);                   /* to an integer: 64 bits, then narrowed */
        Ins *i = emit(O_LDCVT);
        i->imm = 1; i->a = reg(v, 8); i->dst = d; i->w = 8; i->sgn = !(to->is_unsigned && to->size == 8);
        i->m.slot = new_slot(16, 16);
        if (to->size < 8) return cast(R(d), ty_long, to, n);
        return R(d);
    }
    if (is_i128(to) || is_i128(from)) {
        if (is_i128(to) && is_i128(from)) return v;
        if (is_flonum(to) || is_flonum(from)) {    /* through the run-time helpers */
            need_float(n);
            int u = (is_i128(to) ? to : from)->is_unsigned;
            Type *f = is_flonum(to) ? to : from;
            const char *k = f->kind == TY_FLOAT ? "sf" : f->kind == TY_DOUBLE ? "df" : "xf";
            if (is_i128(to)) return helper(fmt(u ? "__fixuns%sti" : "__fix%sti", k), to, from, v, 0, I(0));
            return helper(fmt(u ? "__floatunti%s" : "__floatti%s", k), to, from, v, 0, I(0));
        }
        if (is_i128(to)) {                         /* from a smaller integer */
            if (is_agg(from)) from = ty_ulong;
            Op x = from->size < 8 ? cast(v, from, from->is_unsigned ? ty_ulong : ty_long, n) : v;
            return i128_ext(x, !from->is_unsigned);
        }
        Op lo = load(addr_of_reg(reg(v, 8)), ty_ulong);   /* to a smaller integer: the low half */
        if (to->kind == TY_BOOL) {
            Addr h = addr_of_reg(reg(v, 8));
            h.off = 8;
            Op either = arith(O_OR, lo, load(h, ty_ulong), 8);
            return cast(either, ty_ulong, ty_bool, n);
        }
        return cast(lo, ty_ulong, to, n);
    }
    if (is_agg(from)) from = ty_ulong;             /* an address */
    if (is_agg(to)) return v;
    int fw = wof(from), tw = wof(to);
    if (to->kind == TY_BOOL) {
        if (is_flonum(from)) {
            need_float(n);
            int z = new_vreg(C_FLT);
            Ins *zi = emit(O_MOV);           /* 0.0 */
            zi->dst = z; zi->bimm = 1; zi->imm = 0; zi->w = from->size;
            int d = new_vreg(C_INT);
            Ins *i = emit(O_SET);
            i->dst = d; i->a = reg(v, fw); i->b = z; i->cc = CC_FNE; i->w = from->size;
            return R(d);
        }
        if (v.k) return I(v.imm != 0);
        int d = new_vreg(C_INT);
        Ins *i = emit(O_SET);
        i->dst = d; i->a = v.v; i->bimm = 1; i->imm = 0; i->cc = CC_NE; i->w = fw;
        return R(d);
    }
    if (is_flonum(to) || is_flonum(from)) need_float(n);
    if (is_flonum(to) && is_flonum(from)) {
        if (from->size == to->size) return v;
        int d = new_vreg(C_FLT);
        Ins *i = emit(O_F2F);
        i->dst = d; i->a = reg(v, fw); i->w = to->size; i->sz = from->size;
        return R(d);
    }
    if (is_flonum(to)) {                           /* integer -> float */
        int a = reg(v, fw);
        int sz = from->size < 4 ? 4 : from->size;
        if (from->is_unsigned && from->kind != TY_BOOL && sz == 4) {   /* unsigned 32: zero-extend, then signed 64 */
            int e = new_vreg(C_INT);
            Ins *x = emit(O_EXT);
            x->dst = e; x->a = a; x->sz = 4; x->sgn = 0; x->w = 8;
            a = e;
            sz = 8;
        }
        int d = new_vreg(C_FLT);
        Ins *i = emit(O_I2F);
        i->dst = d; i->a = a; i->sz = sz; i->w = to->size;
        i->sgn = !(from->is_unsigned && sz == 8 && from->kind != TY_BOOL);   /* sgn=0: unsigned 64 */
        return R(d);
    }
    if (is_flonum(from)) {                         /* float -> integer */
        int d = new_vreg(C_INT);
        Ins *i = emit(O_F2I);
        i->dst = d; i->a = reg(v, fw); i->sz = from->size;
        i->w = tw == 8 || (to->is_unsigned && to->size == 4) ? 8 : 4;
        i->sgn = !(to->is_unsigned && to->size == 8);
        Op r = R(d);
        if (to->size < 4) return cast(r, ty_int, to, n);
        return r;
    }
    /* integer -> integer */
    if (v.k) {
        long x = v.imm;
        switch (from->size) {                      /* the value as its own type says... */
        case 1: x = from->is_unsigned ? (unsigned char)x : (signed char)x; break;
        case 2: x = from->is_unsigned ? (unsigned short)x : (short)x; break;
        case 4: x = from->is_unsigned ? (long)(unsigned)x : (long)(int)x; break;
        }
        switch (to->size) {                        /* ... then as the new one */
        case 1: x = to->is_unsigned ? (unsigned char)x : (signed char)x; break;
        case 2: x = to->is_unsigned ? (unsigned short)x : (short)x; break;
        case 4: x = to->is_unsigned ? (long)(unsigned)x : (long)(int)x; break;
        }
        return I(x);
    }
    if (to->size < 4 && (from->size > to->size || from->is_unsigned != to->is_unsigned)) {
        int d = new_vreg(C_INT);
        Ins *i = emit(O_EXT);
        i->dst = d; i->a = v.v; i->sz = to->size; i->sgn = !to->is_unsigned; i->w = 4;
        return R(d);
    }
    if (tw == 8 && fw == 4) {
        int d = new_vreg(C_INT);
        Ins *i = emit(O_EXT);
        i->dst = d; i->a = v.v; i->sz = 4; i->sgn = !from->is_unsigned; i->w = 8;
        return R(d);
    }
    return v;                                      /* same width, or narrowing 8 -> 4 */
}

/* ---- A 128-bit expression whose result is narrowed to 64 bits: the low
 * half of a sum, product or bit operation only needs the operands' low
 * halves, and (a * b) >> k of two 64-bit values is one mul + shrd (how
 * SIEOS scales a clock). Anything else takes the general 128-bit path. */
static int from64(Node *n) { return (n->kind == ND_CAST && n->lhs->ty->size <= 8 && is_integer(n->lhs->ty)) || n->kind == ND_NUM; }

static int narrowable(Node *n)
{
    add_type(n);
    switch (n->kind) {
    case ND_NUM: return 1;
    case ND_CAST: return n->lhs->ty->kind != TY_INT128 && is_integer(n->lhs->ty);
    case ND_SHR: {
        Node *r = n->rhs->kind == ND_CAST && n->rhs->lhs->kind == ND_NUM ? n->rhs->lhs : n->rhs;
        return n->lhs->kind == ND_MUL && r->kind == ND_NUM && r->val >= 0 && r->val < 128 && from64(n->lhs->lhs) && from64(n->lhs->rhs);
    }
    case ND_COND: return narrowable(n->then) && narrowable(n->els);
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_AND: case ND_OR: case ND_XOR: return narrowable(n->lhs) && narrowable(n->rhs);
    }
    return 0;
}

static Op narrow128(Node *n)
{
    add_type(n);
    if (n->kind == ND_NUM) return I(n->val);
    if (n->kind == ND_CAST) return cast(gen(n->lhs), n->lhs->ty, n->ty->is_unsigned ? ty_ulong : ty_long, n);
    if (n->kind == ND_SHR) {
        if (n->rhs->kind == ND_CAST && n->rhs->lhs->kind == ND_NUM) n->rhs = n->rhs->lhs;
        int a = reg(narrow128(n->lhs->lhs), 8), b = reg(narrow128(n->lhs->rhs), 8);
        int d = new_vreg(C_INT);
        Ins *i = emit(O_MULSHR);
        i->dst = d; i->a = a; i->b = b; i->imm = n->rhs->val; i->w = 8; i->sgn = !n->ty->is_unsigned;
        return R(d);
    }
    if (n->kind == ND_COND) {
        Block *t = new_block(), *f = new_block(), *done = new_block();
        int r = new_vreg(C_INT);
        gen_cond(n->cond, t, f);
        place(t);
        move(r, narrow128(n->then), 8);
        jump(done);
        place(f);
        move(r, narrow128(n->els), 8);
        place(done);
        return R(r);
    }
    int op = n->kind == ND_ADD ? O_ADD : n->kind == ND_SUB ? O_SUB : n->kind == ND_MUL ? O_MUL : n->kind == ND_AND ? O_AND :
             n->kind == ND_OR ? O_OR : O_XOR;
    return arith(op, narrow128(n->lhs), narrow128(n->rhs), 8);   /* the low 64 bits */
}

/* ---- Addresses */
static Addr gen_addr(Node *n)
{
    switch (n->kind) {
    case ND_VAR:
        if (n->var->is_local && n->var->vreg) error_at(n->tok, "internal: the address of a register variable");
        return var_addr(n->var);
    case ND_DEREF: {
        Node *e = n->lhs;
        add_type(e);
        if (e->kind == ND_ADD && (e->ty->kind == TY_PTR) && opt_optimize) {
            Node *l = e->lhs, *r = e->rhs;
            add_type(r);
            if (r->kind == ND_NUM && fits32(r->val)) {           /* p + constant */
                Addr a = addr_of_reg(gen_reg(l));
                a.off = r->val;
                return a;
            }
            if (r->kind == ND_MUL && r->rhs->kind == ND_NUM && (r->rhs->val == 2 || r->rhs->val == 4 || r->rhs->val == 8)) {
                int b = gen_reg(l);
                Addr a = addr_of_reg(b);
                a.index = gen_reg(r->lhs);
                a.scale = r->rhs->val;
                return a;
            }
            if (r->kind == ND_SHL && r->rhs->kind == ND_NUM && r->rhs->val >= 1 && r->rhs->val <= 3) {
                int b = gen_reg(l);
                Addr a = addr_of_reg(b);
                a.index = gen_reg(r->lhs);
                a.scale = 1 << r->rhs->val;
                return a;
            }
            int b = gen_reg(l);
            Addr a = addr_of_reg(b);
            a.index = gen_reg(r);
            a.scale = 1;
            return a;
        }
        return addr_of_reg(gen_reg(e));
    }
    case ND_MEMBER: {
        Addr a = gen_addr(n->lhs);
        a.off += n->member->offset;
        return a;
    }
    case ND_COMMA:
        gen(n->lhs);
        return gen_addr(n->rhs);
    }
    add_type(n);
    if (is_mem(n->ty)) return addr_of_reg(gen_reg(n));   /* a struct value: its address */
    error_at(n->tok, "not an lvalue");
}

/* ---- Calls */

/* Is every scalar in bytes [lo, hi) of a type at offset `off` a float? */
static int only_floats(Type *t, long lo, long hi, long off)
{
    if (t->kind == TY_STRUCT || t->kind == TY_UNION) {
        for (Member *m = t->members; m; m = m->next)
            if (!only_floats(m->ty, lo, hi, off + m->offset)) return 0;
        return 1;
    }
    if (t->kind == TY_ARRAY) {
        for (int i = 0; i < t->len; i++) if (!only_floats(t->base, lo, hi, off + (long)i * t->base->size)) return 0;
        return 1;
    }
    if (t->kind == TY_COMPLEX) return only_floats(t->base, lo, hi, off) && only_floats(t->base, lo, hi, off + t->base->size);
    return off + t->size <= lo || off >= hi || is_flonum(t);   /* (a 16-byte integer covers both eightbytes) */
}

static int misaligned(Type *t, long off)
{
    if (t->kind == TY_STRUCT || t->kind == TY_UNION) {
        for (Member *m = t->members; m; m = m->next) {
            if (m->is_bitfield) continue;
            if (misaligned(m->ty, off + m->offset)) return 1;
        }
        return 0;
    }
    if (t->kind == TY_ARRAY) return misaligned(t->base, off);
    return off % t->align != 0;
}

static int has_ld(Type *t)
{
    if (t->kind == TY_LDOUBLE) return 1;
    if (t->kind == TY_COMPLEX) return has_ld(t->base);
    if (t->kind == TY_ARRAY) return has_ld(t->base);
    if (t->kind == TY_STRUCT || t->kind == TY_UNION) for (Member *m = t->members; m; m = m->next) if (has_ld(m->ty)) return 1;
    return 0;
}

int classify(Type *ty, int cls[2])
{
    if (ty->size > 16 || ty->size == 0 || misaligned(ty, 0) || has_ld(ty)) return 0;   /* (long double: memory) */
    int n = (ty->size + 7) / 8;
    for (int i = 0; i < n; i++) cls[i] = only_floats(ty, i * 8, i * 8 + 8, 0) ? C_FLT : C_INT;
    return n;
}

static const int argregs[6] = { RDI, RSI, RDX, RCX, R8, R9 };

/* The eightbyte i of a struct at address a, into a register (no reading past the end). */
static int load_eightbyte(int a, Type *ty, int i, int cls)
{
    int rem = ty->size - i * 8;
    if (rem > 8) rem = 8;
    Addr m = addr_of_reg(a);
    m.off = i * 8;
    int v = new_vreg(cls);
    if (rem == 8 || rem == 4 || (cls == C_INT && (rem == 2 || rem == 1))) {
        Ins *l = emit(O_LOAD);
        l->dst = v; l->m = m; l->sz = rem; l->w = cls == C_FLT ? rem : 8; l->sgn = 0;
        return v;
    }
    /* odd sizes: copy to a zeroed 8-byte temporary first */
    int t = lea(slot_addr(new_slot(8, 8)));
    zero_mem(t, 8);
    copy_mem(t, lea(m), rem);
    Ins *l = emit(O_LOAD);
    l->dst = v; l->m = addr_of_reg(t); l->sz = 8; l->w = 8;
    return v;
}

#include "ir_a64.inc"

/* ---- Inlining: a call to a small function (or to a static function used
 * only by calls) becomes its body: its locals get storage here, the
 * parameters are assigned from the arguments, return assigns the result and
 * jumps out. Limits: no recursion, a nesting depth, and the stack: a body
 * whose locals need more than INL_FRAME bytes of memory is not inlined, nor
 * any more once a function has grown by INL_TOTAL (an inlined body's frame
 * lives as long as the caller's, also across the caller's own calls: deep
 * call chains, like a kernel's, must not grow much). */
enum { INL_SMALL = 24, INL_SMALL_OS = 6, INL_ONCE = 600, INL_FRAME = 256, INL_TOTAL = 1024, INL_DEPTH = 8 };
typedef struct Inl { Block *exit; int res, resaddr; Type *rt; struct Inl *up; } Inl;
static Inl *inl;
static int inl_depth, inl_frame;

static int cost_of(Node *n, int c, int *bad)       /* nodes, and whether something forbids inlining */
{
    for (; n && c < 100000; n = n->next) {
        c++;
        switch (n->kind) {
        case ND_BUILTIN:
            if (n->val == B_ALLOCA || n->val == B_SPSAVE || n->val == B_SPRESTORE || n->val == B_FRAME_ADDRESS || n->val == B_VA_START) *bad = 1;
            break;
        case ND_LABEL_VAL: *bad = 1; break;
        case ND_GOTO: if (n->lhs) *bad = 1; break;
        case ND_CALL:
            if (n->lhs->kind == ND_VAR) {
                const char *s = n->lhs->var->name;
                if (!strcmp(s, "setjmp") || !strcmp(s, "_setjmp") || !strcmp(s, "sigsetjmp") || !strcmp(s, "vfork")) *bad = 1;
            }
            break;
        }
        Node *k[] = { n->lhs, n->rhs, n->cond, n->then, n->els, n->init, n->inc, n->body, n->args };
        for (int x = 0; x < 9; x++) if (k[x]) c = cost_of(k[x], c, bad);
    }
    return c;
}

static int local_in_reg(Obj *v) { return is_scalar(v->ty) && !is_mem(v->ty) && !v->addr_taken && !v->ty->is_volatile && !v->ty->is_atomic; }

static int inlinable(Obj *g)                       /* by its body (counted once) */
{
    if (!g->inl_cost) {
        int bad = !g->body || g->ty->is_variadic || g->ty->noproto || g->noinline;
        for (Obj *v = g->locals; v; v = v->next) {
            if (v->ty->is_vla || v->vla_ptr) bad = 1;
            if (!local_in_reg(v)) g->inl_frame += align_to(v->ty->size, 8);
        }
        g->inl_cost = g->body ? cost_of(g->body, 0, &bad) : 1;
        if (bad) g->inl_cost = -1;
    }
    return g->inl_cost > 0;
}

/* direct calls of each function (ncalls -1: it calls itself) */
void count_calls(Node *n, Obj *owner)
{
    for (; n; n = n->next) {
        if (n->kind == ND_CALL && n->lhs->kind == ND_VAR && n->lhs->var->ty->kind == TY_FUNC && !n->lhs->var->is_local) {
            Obj *g = n->lhs->var;
            if (g == owner) g->ncalls = -1;
            else if (g->ncalls >= 0) g->ncalls++;
        }
        Node *k[] = { n->lhs, n->rhs, n->cond, n->then, n->els, n->init, n->inc, n->body, n->args };
        for (int x = 0; x < 9; x++) if (k[x]) count_calls(k[x], owner);
    }
}

/* to be inlined at every call (and not emitted): by size; called once: almost any size */
int inline_candidate(Obj *g)
{
    return inlinable(g) && g->inl_frame <= INL_FRAME && (g->always_inline || g->inl_cost <= (g->ncalls == 1 ? INL_ONCE : opt_optimize == 2 ? INL_SMALL_OS : INL_SMALL));
}

static int want_inline(Obj *g, Node *call)
{
    if (!opt_optimize || !g->is_def || g->is_local || g == F->obj || g->inl_active || inl_depth >= INL_DEPTH || !inlinable(g)) return 0;
    int np = 0, na = 0;
    for (Obj *p = g->params; p; p = p->pnext) np++;
    for (Node *a = call->args; a; a = a->next) na++;
    if (np != na) return 0;
    if (g->always_inline) return 1;
    if (g->inl_frame > INL_FRAME || inl_frame + g->inl_frame > INL_TOTAL) return 0;
    return g->inl_all || g->inl_cost <= (opt_optimize == 2 ? INL_SMALL_OS : INL_SMALL);
}

static Op gen_inline(Node *n, Obj *g)
{
    Type *rt = n->ty;
    /* storage for its locals (saved: g may be inlined again, inside an argument) */
    int nl = 0;
    for (Obj *v = g->locals; v; v = v->next) nl++;
    int *saved = malloc(sizeof(int) * 2 * (nl + 1));
    if (!saved) die("out of memory");
    nl = 0;
    for (Obj *v = g->locals; v; v = v->next) {
        saved[nl++] = v->vreg;
        saved[nl++] = v->slot;
        v->vreg = v->slot = 0;
        if (local_in_reg(v)) {
            v->vreg = new_vreg(clsof(v->ty));
            F->vr[v->vreg].w = wof(v->ty);
            if (v->reg) F->vr[v->vreg].fixed = v->reg;
        } else v->slot = new_slot(v->ty->size, v->align > v->ty->align ? v->align : v->ty->align);
    }
    inl_frame += g->inl_frame;
    /* the arguments, in order, into the parameters */
    Node *a = n->args;
    for (Obj *p = g->params; p; p = p->pnext, a = a->next) {
        add_type(a);
        Type *t = p->ty;
        if (p->vreg) { Op v = gen(a); move(p->vreg, v, wof(t)); }
        else if (t->kind == TY_STRUCT || t->kind == TY_UNION || is_mem(t)) {
            int src = gen_reg(a), dst = lea(slot_addr(p->slot));    /* (in this order) */
            copy_mem(dst, src, t->size);
        }
        else store(slot_addr(p->slot), gen(a), t);
    }
    Inl ctx = { new_block(), 0, 0, rt, inl };
    if (rt->kind == TY_STRUCT || rt->kind == TY_UNION || is_mem(rt)) ctx.resaddr = lea(slot_addr(new_slot(rt->size, rt->align)));
    else if (rt->kind != TY_VOID) { ctx.res = new_vreg(clsof(rt)); F->vr[ctx.res].w = wof(rt); }
    Map outer = labels;
    labels = (Map){ 0 };
    inl = &ctx;
    inl_depth++;
    g->inl_active = 1;
    gen_stmt(g->body);
    g->inl_active = 0;
    inl_depth--;
    inl = ctx.up;
    free(labels.k);
    free(labels.v);
    labels = outer;
    jump(ctx.exit);
    place(ctx.exit);
    nl = 0;
    for (Obj *v = g->locals; v; v = v->next) { v->vreg = saved[nl++]; v->slot = saved[nl++]; }
    free(saved);
    if (ctx.resaddr) return R(ctx.resaddr);
    return ctx.res ? R(ctx.res) : I(0);
}

static Op gen_call(Node *n)
{
    if (n->lhs->kind == ND_VAR && n->lhs->var->ty->kind == TY_FUNC) {
        Obj *g = n->lhs->var;
        if (want_inline(g, n)) return gen_inline(n, g);
        /* memcpy and memset (to 0) of a few bytes, known: moves */
        int mc = !strcmp(g->name, "memcpy") || !strcmp(g->name, "__builtin_memcpy"), ms = !strcmp(g->name, "memset") || !strcmp(g->name, "__builtin_memset");
        if ((mc || ms) && opt_optimize && !g->is_def && n->args && n->args->next && n->args->next->next && !n->args->next->next->next) {
            Node *a1 = n->args, *a2 = a1->next, *a3 = a2->next;
            add_type(a2); add_type(a3);
            if (is_const_expr(a3) && (mc || (is_const_expr(a2) && !const_eval(a2)))) {
                long size = const_eval(a3);
                if (size > 0 && size <= (opt_optimize == 2 ? 16 : 64)) {
                    int d = gen_reg(a1);
                    if (mc) { int s = gen_reg(a2); copy_mem(d, s, size); }
                    else zero_mem(d, size);
                    return R(d);
                }
            }
        }
        /* sqrt, sqrtf: the instruction (errno is not set: math_errhandling is MATH_ERREXCEPT) */
        int sq = !g->is_def && n->args && !n->args->next && opt_optimize && !opt_general_regs_only ? 
                 (!strcmp(g->name, "sqrt") || !strcmp(g->name, "__builtin_sqrt") ? 8 : !strcmp(g->name, "sqrtf") || !strcmp(g->name, "__builtin_sqrtf") ? 4 : 0) : 0;
        if (sq && is_flonum(n->args->ty) && n->args->ty->size == sq && n->ty->size == sq && is_flonum(n->ty)) {
            int a = gen_reg(n->args), d = new_vreg(C_FLT);
            Ins *i = emit(O_FSQRT);
            i->dst = d; i->a = a; i->w = sq;
            return R(d);
        }
        if (g->inl_all) g->inl_needed = 1;          /* a real call after all: emit it */
    }
    Type *ft = n->fty, *rt = n->ty;
    Call *c = arena(sizeof *c);
    int nargs = 1;
    for (Node *a = n->args; a; a = a->next) nargs += target == T_AARCH64 ? 4 : 2;   /* registers per argument: an HFA has 4 */
    c->args = arena(sizeof(CallArg) * nargs);
    Node *fn = n->lhs;
    add_type(fn);
    if (fn->kind == ND_VAR && fn->var->ty->kind == TY_FUNC && !fn->var->is_local) c->sym = fn->var->asm_name;
    else if (fn->kind == ND_DEREF && fn->ty->kind == TY_FUNC) c->fn = gen_reg(fn->lhs);
    else c->fn = gen_reg(fn);
    if (target == T_AARCH64) return a64_call(n, c);
    int rcls[2], nr = 0, memret = 0;
    int x87c = is_cplx(rt) && is_ld(rt->base);     /* complex long double: returned in st0, st1 */
    if ((rt->kind == TY_STRUCT || rt->kind == TY_UNION || is_i128(rt) || is_cplx(rt)) && !x87c) { nr = classify(rt, rcls); memret = !nr; }
    int gp = 0, fp = 0;
    long stk = 0;
    int retaddr = 0;
    if (memret) {
        int s = new_slot(rt->size, rt->align);
        retaddr = lea(slot_addr(s));
        CallArg *ca = &c->args[c->nargs++];
        ca->v = retaddr; ca->phys = RDI; ca->cls = C_INT; ca->w = 8;
        gp = 1;
    }
    for (Node *a = n->args; a; a = a->next) {
        add_type(a);
        Type *t = a->ty;
        if (t->kind == TY_STRUCT || t->kind == TY_UNION || is_ld(t) || is_i128(t) || is_cplx(t)) {
            if (is_ld(t) || is_cplx(t)) need_float(a);
            int addr = gen_reg(a);
            int cls[2], ne = classify(t, cls), ngp = 0, nfp = 0;
            for (int i = 0; i < ne; i++) { if (cls[i] == C_INT) ngp++; else nfp++; }
            if (ne && gp + ngp <= 6 && fp + nfp <= 8) {
                if (cls[0] == C_FLT || (ne > 1 && cls[1] == C_FLT)) need_float(a);
                for (int i = 0; i < ne; i++) {
                    CallArg *ca = &c->args[c->nargs++];
                    ca->v = load_eightbyte(addr, t, i, cls[i]);
                    ca->cls = cls[i];
                    ca->w = 8;
                    ca->phys = cls[i] == C_INT ? argregs[gp++] : XMM0 + fp++;
                }
            } else {
                CallArg *ca = &c->args[c->nargs++];
                stk = align_to(stk, t->align > 8 ? 16 : 8);
                ca->addr = addr;
                ca->size = t->size;
                ca->phys = -1;
                ca->stack_off = stk;
                stk += align_to(t->size, 8);
            }
            continue;
        }
        CallArg *ca = &c->args[c->nargs++];
        ca->cls = clsof(t);
        ca->w = wof(t);
        Op av = gen(a);
        if (av.k && ca->cls == C_INT && gp < 6) { ca->isimm = 1; ca->imm = av.imm; ca->phys = argregs[gp++]; continue; }
        ca->v = reg(av, ca->w);
        if (ca->cls == C_INT && gp < 6 && F->vr[ca->v].hint < 0) F->vr[ca->v].hint = argregs[gp];
        if (ca->cls == C_FLT) {
            need_float(a);
            if (fp < 8) { ca->phys = XMM0 + fp++; continue; }
        } else if (gp < 6) { ca->phys = argregs[gp++]; continue; }
        ca->phys = -1;
        ca->stack_off = stk;
        stk += 8;
    }
    c->stack = align_to(stk, 16);
    c->nsse = fp;
    c->variadic = ft->is_variadic || ft->noproto;
    Ins *i = emit(O_CALL);
    i->call = c;
    i->clob = CALLER_SAVED;
    F->has_calls = 1;
    if (rt->kind == TY_VOID) return I(0);
    if (memret) return R(retaddr);
    if (is_ld(rt) || x87c) {                       /* returned in st0 (st1): stored to a slot */
        c->x87slot = new_slot(rt->size, 16);
        c->x87cplx = x87c;
        return R(lea(slot_addr(c->x87slot)));
    }
    if (nr) {                                      /* a struct in registers: store it in a temporary */
        int ngp = 0, nfp = 0;
        for (int k = 0; k < nr; k++) {
            int v = new_vreg(rcls[k]);
            c->ret[k] = v;
            c->retw[k] = 8;
            c->retphys[k] = rcls[k] == C_INT ? (ngp++ ? RDX : RAX) : (nfp++ ? XMM0 + 1 : XMM0);
        }
        c->nret = nr;
        int s = new_slot(align_to(rt->size, 8), rt->align > 8 ? rt->align : 8);
        int t = lea(slot_addr(s));
        for (int k = 0; k < nr; k++) {
            Addr m = addr_of_reg(t);
            m.off = k * 8;
            store_sz(m, R(c->ret[k]), 8, rcls[k]);
        }
        return R(t);
    }
    int v = new_vreg(clsof(rt));
    c->ret[0] = v;
    c->retw[0] = wof(rt);
    c->retphys[0] = is_flonum(rt) ? XMM0 : RAX;
    c->nret = 1;
    /* a function returning char or short: the caller extends the value */
    if (is_integer(rt) && rt->size < 4) {
        int d = new_vreg(C_INT);
        Ins *x = emit(O_EXT);
        x->dst = d; x->a = v; x->sz = rt->size; x->sgn = !rt->is_unsigned; x->w = 4;
        if (rt->kind == TY_BOOL) x->sgn = 0;
        return R(d);
    }
    return R(v);
}

/* ---- va_arg */
static Op gen_va_arg(Node *n)
{
    if (target == T_AARCH64) return a64_va_arg(n);
    int ap = gen_reg(n->args);
    Type *t = n->ty;
    int cls[2], ne, ngp = 0, nfp = 0;
    if (t->kind == TY_STRUCT || t->kind == TY_UNION || is_ld(t) || is_i128(t) || is_cplx(t)) ne = classify(t, cls);
    else { ne = 1; cls[0] = clsof(t); }
    for (int i = 0; i < ne; i++) { if (cls[i] == C_INT) ngp++; else nfp++; }
    Addr gpo = addr_of_reg(ap), fpo = addr_of_reg(ap), ovf = addr_of_reg(ap), rsa = addr_of_reg(ap);
    fpo.off = 4; ovf.off = 8; rsa.off = 16;
    int agg = is_mem(t);
    int res = new_vreg(agg ? C_INT : clsof(t));
    Block *regs = new_block(), *stack = new_block(), *done = new_block();
    if (ne) {
        Op g = load(gpo, ty_uint), f = load(fpo, ty_uint);
        Block *chk2 = new_block();
        Ins *b1 = emit(O_BR);
        b1->cc = CC_ULE; b1->a = reg(g, 4); b1->bimm = 1; b1->imm = 48 - 8 * ngp; b1->w = 4; b1->t = chk2; b1->f = stack;
        place(chk2);
        Ins *b2 = emit(O_BR);
        b2->cc = CC_ULE; b2->a = reg(f, 4); b2->bimm = 1; b2->imm = 176 - 16 * nfp; b2->w = 4; b2->t = regs; b2->f = stack;
        place(regs);
        int area = load(rsa, pointer_to(ty_void)).v;
        if (!agg) {
            Op off = cls[0] == C_INT ? g : f;
            Op e = cast(off, ty_uint, ty_ulong, n);
            Addr m = addr_of_reg(area);
            m.index = reg(e, 8);
            m.scale = 1;
            Ins *l = emit(O_LOAD);
            l->dst = res; l->m = m; l->sz = t->size; l->w = wof(t); l->sgn = !t->is_unsigned && t->kind != TY_PTR;
        } else {
            int s = new_slot(align_to(t->size, 8), 8);
            int tmp = lea(slot_addr(s));
            Op gg = g, ff = f;
            for (int i = 0; i < ne; i++) {
                Op off = cls[i] == C_INT ? gg : ff;
                Op e = cast(off, ty_uint, ty_ulong, n);
                Addr m = addr_of_reg(area);
                m.index = reg(e, 8);
                m.scale = 1;
                int v = new_vreg(C_INT);
                Ins *l = emit(O_LOAD);
                l->dst = v; l->m = m; l->sz = 8; l->w = 8;
                Addr d = addr_of_reg(tmp);
                d.off = i * 8;
                store_sz(d, R(v), 8, C_INT);
                if (cls[i] == C_INT) gg = arith(O_ADD, gg, I(8), 4); else ff = arith(O_ADD, ff, I(16), 4);
            }
            move(res, R(tmp), 8);
        }
        if (ngp) store(gpo, arith(O_ADD, g, I(8 * ngp), 4), ty_uint);
        if (nfp) store(fpo, arith(O_ADD, f, I(16 * nfp), 4), ty_uint);
        jump(done);
    } else jump(stack);
    place(stack);
    Op p = load(ovf, pointer_to(ty_void));
    int pv = reg(p, 8);
    if (t->align > 8) pv = reg(arith(O_AND, arith(O_ADD, R(pv), I(15), 8), I(-16), 8), 8);
    if (agg) move(res, R(pv), 8);
    else {
        Ins *l = emit(O_LOAD);
        l->dst = res; l->m = addr_of_reg(pv); l->sz = t->size; l->w = wof(t); l->sgn = !t->is_unsigned && t->kind != TY_PTR;
    }
    store(ovf, arith(O_ADD, R(pv), I(align_to(t->size, 8)), 8), pointer_to(ty_void));
    place(done);
    return R(res);
}

static int bits_of(int v, int to_cls, int w);

static int order_of(Node *n)          /* a memory order argument: SEQ_CST if unknown */
{
    if (!n) return 5;
    add_type(n);
    return is_const_expr(n) ? const_eval(n) : 5;
}

static Op gen_builtin(Node *n)
{
    Node *a = n->args;
    switch (n->val) {
    case B_VA_START: {
        int ap = gen_reg(a);
        if (target == T_AARCH64) { a64_va_start(ap); return I(0); }
        Addr m = addr_of_reg(ap);
        store_sz(m, I(8 * ngp_named), 4, C_INT);
        m.off = 4;
        store_sz(m, I(48 + 16 * nfp_named), 4, C_INT);
        Addr args = { 0 };
        args.args = 1;
        args.off = stack_named;
        args.scale = 1;
        m.off = 8;
        store_sz(m, R(lea(args)), 8, C_INT);
        m.off = 16;
        store_sz(m, R(lea(slot_addr(F->va_slot))), 8, C_INT);
        return I(0);
    }
    case B_VA_ARG: return gen_va_arg(n);
    case B_VA_END: gen(a); return I(0);
    case B_VA_COPY: copy_mem(gen_reg(a), gen_reg(a->next), target == T_AARCH64 ? 32 : 24); return I(0);
    case B_UNREACHABLE: case B_TRAP:
        emit(O_UNREACH);
        dead_code();
        return I(0);
    case B_FENCE:                                  /* (x86: only seq_cst needs an instruction; AArch64: all but relaxed) */
        if (order_of(a) == 5 || (target == T_AARCH64 && order_of(a))) emit(O_FENCE);
        return I(0);
    case B_ATOMIC_LOAD: {
        Type *t = n->ty;
        Op v = load(addr_of_reg(gen_reg(a)), t);
        if (target == T_AARCH64 && order_of(a->next) && cur->last && cur->last->op == O_LOAD) cur->last->cc = 2;   /* ldar */
        return v;
    }
    case B_ATOMIC_STORE: {
        Type *t = a->ty->base;
        int p = gen_reg(a);
        Op v = gen(a->next);
        int fl = clsof(t) == C_FLT;
        if (order_of(a->next->next) == 5) {          /* seq_cst: xchg (a full barrier) */
            int rv = fl ? bits_of(reg(v, t->size), C_INT, t->size) : reg(v, 8);
            Ins *i = emit(O_XCHG);
            i->dst = new_vreg(C_INT); i->a = p; i->b = rv; i->sz = t->size; i->w = t->size == 8 ? 8 : 4;
        } else {
            store(addr_of_reg(p), v, t);
            if (target == T_AARCH64 && order_of(a->next->next) && cur->last && cur->last->op == O_STORE) cur->last->cc = 2;   /* stlr */
        }
        return I(0);
    }
    case B_ATOMIC_EXCHANGE: case B_ATOMIC_FETCH_ADD: case B_ATOMIC_FETCH_SUB: {
        Type *t = a->ty->base;
        int p = gen_reg(a), w = t->size == 8 ? 8 : 4;
        Op v = gen(a->next);
        int fl = clsof(t) == C_FLT;
        if (fl && n->val != B_ATOMIC_EXCHANGE) error_at(n->tok, "atomic arithmetic on a floating-point object: use a compare-and-swap loop");
        Op add = n->val == B_ATOMIC_FETCH_SUB ? arith(O_SUB, I(0), v, w) : v;
        int rv = fl ? bits_of(reg(v, t->size), C_INT, t->size) : reg(add, 8);
        Ins *i = emit(n->val == B_ATOMIC_EXCHANGE ? O_XCHG : O_XADD);
        int d = new_vreg(C_INT);
        i->dst = d; i->a = p; i->b = rv; i->sz = t->size; i->w = w;
        if (fl) return R(bits_of(d, C_FLT, t->size));
        Op old = cast(R(d), t->size < 4 ? (t->is_unsigned ? ty_uint : ty_int) : t, t, n);
        if (n->hi) return cast(arith(O_ADD, old, add, w), t->size < 4 ? ty_int : t, t, n);   /* add_fetch, sub_fetch */
        return old;
    }
    case B_ATOMIC_FETCH_AND: case B_ATOMIC_FETCH_OR: case B_ATOMIC_FETCH_XOR: case B_ATOMIC_FETCH_NAND: {   /* a compare-and-swap loop */
        Type *t = a->ty->base;
        int w = wof(t);
        int p = gen_reg(a);
        int v = reg(gen(a->next), w);
        int old = new_vreg(C_INT);
        Ins *l = emit(O_LOAD);
        l->dst = old; l->m = addr_of_reg(p); l->sz = t->size; l->w = w; l->cc = 1;
        Block *loop = new_block(), *done = new_block();
        place(loop);
        Op nv;
        switch (n->val) {
        case B_ATOMIC_FETCH_AND: nv = arith(O_AND, R(old), R(v), w); break;
        case B_ATOMIC_FETCH_OR: nv = arith(O_OR, R(old), R(v), w); break;
        case B_ATOMIC_FETCH_XOR: nv = arith(O_XOR, R(old), R(v), w); break;
        default: { int x = reg(arith(O_AND, R(old), R(v), w), w), d = new_vreg(C_INT); Ins *ni = emit(O_NOT); ni->dst = d; ni->a = x; ni->w = w; nv = R(d); }
        }
        int got = new_vreg(C_INT), rnv = reg(nv, w);
        Ins *x = emit(O_CMPXCHG);
        x->dst = got; x->a = p; x->b = rnv; x->m.base = old; x->sz = t->size; x->w = w;
        Ins *br = emit(O_BR);
        br->cc = CC_EQ; br->a = got; br->b = old; br->w = w; br->t = done;
        Block *retry = new_block();
        br->f = retry;
        place(retry);
        move(old, R(got), w);
        jump(loop);
        place(done);
        return cast(R(n->hi ? rnv : old), t->size < 4 ? (t->is_unsigned ? ty_uint : ty_int) : t, t, n);
    }
    case B_ATOMIC_TAS: {
        int p = gen_reg(a);
        int d = new_vreg(C_INT);
        int one = mov_imm(1, 4);
        Ins *i = emit(O_XCHG);
        i->dst = d; i->a = p; i->b = one; i->sz = 1; i->w = 4;
        int r = new_vreg(C_INT);
        Ins *s = emit(O_SET);
        s->dst = r; s->a = d; s->bimm = 1; s->imm = 0; s->cc = CC_NE; s->w = 4;
        return R(r);
    }
    case B_ATOMIC_CLEAR: {
        int p = gen_reg(a);
        if (order_of(a->next) == 5) {
            int zero = mov_imm(0, 4);
            Ins *i = emit(O_XCHG);
            i->dst = new_vreg(C_INT); i->a = p; i->b = zero; i->sz = 1; i->w = 4;
        } else store_sz(addr_of_reg(p), I(0), 1, C_INT);
        return I(0);
    }
    case B_CAS: {
        Type *t = a->ty->base;
        int w = wof(t);
        int p = gen_reg(a);
        int expected_ptr = 0, expected;
        Node *desired;
        if (n->lo == 1) {                          /* __atomic_compare_exchange_n(p, &exp, des, ...) */
            expected_ptr = gen_reg(a->next);
            expected = reg(load(addr_of_reg(expected_ptr), t), w);
            desired = a->next->next;
        } else {
            expected = reg(gen(a->next), w);
            desired = a->next->next;
        }
        int des = reg(gen(desired), w);
        int got = new_vreg(C_INT);
        Ins *x = emit(O_CMPXCHG);
        x->dst = got; x->a = p; x->b = des; x->m.base = expected; x->sz = t->size; x->w = w;
        if (n->lo == 2) return R(got);
        int ok = new_vreg(C_INT);
        Ins *s = emit(O_SET);
        s->dst = ok; s->a = got; s->b = expected; s->cc = CC_EQ; s->w = w;
        if (n->lo == 1) {
            Block *fail = new_block(), *done = new_block();
            Ins *br = emit(O_BR);
            br->cc = CC_NE; br->a = ok; br->bimm = 1; br->imm = 0; br->w = 4; br->t = done; br->f = fail;
            place(fail);
            store(addr_of_reg(expected_ptr), R(got), t);
            place(done);
        }
        return R(ok);
    }
    case B_CLZ: case B_CTZ: case B_POPCOUNT: case B_BSWAP32: case B_BSWAP64: {
        add_type(a);
        int w = wof(a->ty);
        int v = gen_reg(a);
        int d = new_vreg(C_INT);
        Ins *i = emit(O_BITOP);
        i->dst = d; i->a = v; i->w = w;
        i->imm = n->val == B_CLZ ? 0 : n->val == B_CTZ ? 1 : n->val == B_POPCOUNT ? 2 : 3;
        return R(d);
    }
    case B_ALLOCA: case B_SPSAVE: {                /* stack space (VLAs, alloca), the stack pointer */
        int x = n->val == B_ALLOCA ? reg(cast(gen(a), a->ty, ty_ulong, n), 8) : 0;
        int d = new_vreg(C_INT);
        Ins *i = emit(n->val == B_ALLOCA ? O_ALLOCA : O_SPSAVE);
        i->dst = d; i->a = x; i->w = 8;
        return R(d);
    }
    case B_SPRESTORE: {
        int x = gen_reg(a);
        Ins *i = emit(O_SPRESTORE);
        i->a = x;
        return I(0);
    }
    case B_FRAME_ADDRESS: {
        int d = new_vreg(C_INT);
        Ins *i = emit(O_FRAMEADDR);
        i->dst = d; i->imm = n->lo; i->w = 8;
        return R(d);
    }
    }
    error_at(n->tok, "internal: builtin %ld", n->val);
}

/* ---- Inline assembly */
static int asm_reg_of(char c)
{
    switch (c) {
    case 'a': return RAX; case 'b': return RBX; case 'c': return RCX; case 'd': return RDX;
    case 'S': return RSI; case 'D': return RDI;
    }
    return -1;
}

static int reg_by_name(const char *s)
{
    static const char *n64[] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
    static const char *n32[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    if (*s == '%') s++;
    for (int i = 0; i < 16; i++) if (!strcmp(s, n64[i])) return i;
    for (int i = 0; i < 8; i++) if (!strcmp(s, n32[i])) return i;
    if (!strncmp(s, "xmm", 3)) {
        int n = 0;
        for (s += 3; *s >= '0' && *s <= '9'; s++) n = n * 10 + *s - '0';
        return XMM0 + n;
    }
    return -1;
}

static Op gen_asm(Node *n)
{
    if (target == T_AARCH64) return a64_asm(n);
    AsmStmt *s = n->asm_;
    AsmIns *ai = arena(sizeof *ai);
    ai->tmpl = s->tmpl;
    ai->is_basic = s->is_basic;
    int total = s->nout + s->nin;
    ai->op = arena(sizeof(AsmOperand) * (total + 1));
    ai->n = total;
    RegSet taken = BIT(RSP) | BIT(RBP);
    RegSet clob = 0;
    for (int i = 0; i < s->nclob; i++) {
        int r = reg_by_name(s->clob[i]);
        if (r >= 0) clob |= BIT(r);
    }
    taken |= clob;
    Addr outaddr[32];
    Node *outlv[32];
    /* first: the registers named by constraints (a, b, c, d, S, D, register variables) */
    for (int k = 0; k < total; k++) {
        AsmOp *o = k < s->nout ? &s->out[k] : &s->in[k - s->nout];
        AsmOperand *op = &ai->op[k];
        op->phys = -1;
        op->name = o->name;
        add_type(o->expr);
        op->size = o->expr->ty->size;
        if (op->size > 8 || is_mem(o->expr->ty)) op->size = 8;
        const char *c = o->cons;
        while (*c == '=' || *c == '+' || *c == '&') c++;
        for (const char *p = c; *p; p++) {
            int r = asm_reg_of(*p);
            if (r >= 0) { op->phys = r; op->kind = 'r'; break; }
        }
        if (op->phys < 0 && o->expr->kind == ND_VAR && o->expr->var->reg && strpbrk(c, "rqg")) {
            op->phys = reg_by_name(o->expr->var->reg);
            op->kind = 'r';
        }
        if (op->phys >= 0) taken |= BIT(op->phys);
    }
    /* then the others */
    for (int k = 0; k < total; k++) {
        int is_out = k < s->nout;
        AsmOp *o = is_out ? &s->out[k] : &s->in[k - s->nout];
        AsmOperand *op = &ai->op[k];
        const char *c = o->cons;
        int inout = *c == '+';
        while (*c == '=' || *c == '+' || *c == '&') c++;
        op->out = is_out;
        op->in = !is_out || inout;
        if (op->phys >= 0) continue;
        if (!is_out && *c >= '0' && *c <= '9') {               /* the same place as an output */
            AsmOperand *m = &ai->op[*c - '0'];
            op->kind = 'r';
            op->phys = m->phys;
            continue;
        }
        add_type(o->expr);
        int can_imm = strpbrk(c, "inN") != 0 || strchr(c, 'g');
        if (!is_out && can_imm && is_const_expr(o->expr)) {
            long v = const_eval(o->expr);
            if (!strchr(c, 'N') || (v >= 0 && v <= 255)) { op->kind = 'i'; op->imm = v; continue; }
        }
        if (strpbrk(c, "rqg") || (strchr(c, 'N') && strchr(c, 'd'))) {
            if (strchr(c, 'N') && !strpbrk(c, "rqg")) { op->phys = RDX; }
            else {
                static const int pool[] = { RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11, RBX, R12, R13, R14, R15 };
                int qonly = strchr(c, 'q') && !strpbrk(c, "rg");
                for (unsigned i = 0; i < sizeof pool / sizeof pool[0]; i++) {
                    int r = pool[i];
                    if (taken & BIT(r)) continue;
                    if (qonly && r > RBX) continue;
                    op->phys = r;
                    break;
                }
                if (op->phys < 0) error_at(o->expr->tok, "asm: no register left for this operand");
            }
            op->kind = 'r';
            taken |= BIT(op->phys);
            continue;
        }
        if (strchr(c, 'm')) { op->kind = 'm'; continue; }
        error_at(o->expr->tok, "asm: unsupported constraint \"%s\"", o->cons);
    }
    /* values: outputs' addresses, inputs' values */
    for (int k = 0; k < total; k++) {
        int is_out = k < s->nout;
        AsmOp *o = is_out ? &s->out[k] : &s->in[k - s->nout];
        AsmOperand *op = &ai->op[k];
        Node *e = o->expr;
        if (is_out) {
            outlv[k] = e;
            int regvar = e->kind == ND_VAR && e->var->is_local && e->var->vreg;
            if (op->kind == 'm' || !regvar) {
                if (e->kind == ND_MEMBER && e->member->is_bitfield) error_at(e->tok, "asm output to a bit-field");
                outaddr[k] = regvar ? (Addr){ 0 } : gen_addr(e);
            }
            if (op->kind == 'm') {
                Addr m = outaddr[k];
                if (m.base || m.index) { op->v = lea(m); op->mreg = -2; }
                else op->m = m;
                op->in = 1;               /* the address goes in */
            } else {
                op->outv = new_vreg(C_INT);
                if (op->in) {             /* "+": the current value goes in too */
                    op->v = regvar ? e->var->vreg : reg(load(outaddr[k], e->ty), 8);
                }
            }
            continue;
        }
        if (op->kind == 'i') continue;
        if (op->kind == 'm') {
            Addr m;
            if (e->kind == ND_VAR || e->kind == ND_DEREF || e->kind == ND_MEMBER) {
                if (e->kind == ND_VAR && e->var->is_local && e->var->vreg) {
                    int sl = new_slot(8, 8);
                    m = slot_addr(sl);
                    store(m, R(e->var->vreg), e->ty);
                } else m = gen_addr(e);
            } else {
                int sl = new_slot(8, 8);
                m = slot_addr(sl);
                store(m, gen(e), e->ty);
            }
            if (m.base || m.index) { op->v = lea(m); op->mreg = -2; }
            else op->m = m;
            continue;
        }
        Op v = gen(e);
        op->v = reg(v, 8);
    }
    /* memory operands through a register: pick one */
    for (int k = 0; k < total; k++) {
        AsmOperand *op = &ai->op[k];
        if (op->mreg != -2) continue;
        static const int pool[] = { RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11, RBX, R12, R13, R14, R15 };
        op->mreg = -1;
        for (unsigned i = 0; i < sizeof pool / sizeof pool[0]; i++)
            if (!(taken & BIT(pool[i]))) { op->mreg = pool[i]; break; }
        if (op->mreg < 0) error_at(n->tok, "asm: no register left for a memory operand");
        taken |= BIT(op->mreg);
    }
    Ins *i = emit(O_ASM);
    i->asmi = ai;
    i->clob = (taken | clob) & ~(BIT(RSP) | BIT(RBP));
    if (ai->is_basic) i->clob = 0;
    /* outputs: from their temporaries into the lvalues */
    for (int k = 0; k < s->nout; k++) {
        AsmOperand *op = &ai->op[k];
        if (op->kind == 'm') continue;
        Node *e = outlv[k];
        if (e->kind == ND_VAR && e->var->is_local && e->var->vreg) move(e->var->vreg, R(op->outv), 8);
        else store(outaddr[k], R(op->outv), e->ty);
    }
    return I(0);
}

/* ---- Expressions */
static Op gen_logical(Node *n)                 /* a 0/1 value from a condition */
{
    Block *t = new_block(), *f = new_block(), *done = new_block();
    int r = new_vreg(C_INT);
    gen_cond(n, t, f);
    place(t);
    move(r, I(1), 4);
    jump(done);
    place(f);
    move(r, I(0), 4);
    place(done);
    return R(r);
}

static int cmp_cc(Node *n, int swap)
{
    Type *t = n->lhs->ty;
    int u = t->is_unsigned || t->kind == TY_PTR || t->kind == TY_ARRAY;
    if (is_flonum(t)) {
        switch (n->kind) {
        case ND_EQ: return CC_FEQ;
        case ND_NE: return CC_FNE;
        case ND_LT: return swap ? CC_FGT : CC_FLT;
        default: return swap ? CC_FGE : CC_FLE;
        }
    }
    switch (n->kind) {
    case ND_EQ: return CC_EQ;
    case ND_NE: return CC_NE;
    case ND_LT: return swap ? (u ? CC_UGT : CC_GT) : (u ? CC_ULT : CC_LT);
    default: return swap ? (u ? CC_UGE : CC_GE) : (u ? CC_ULE : CC_LE);
    }
}

static Op fconst(double d, int size)
{
    int v = new_vreg(C_FLT);
    Ins *i = emit(O_MOV);
    i->dst = v;
    i->bimm = 1;
    if (size == 4) { float f = d; int x; memcpy(&x, &f, 4); i->imm = (unsigned)x; }
    else memcpy(&i->imm, &d, 8);
    i->w = size;
    return R(v);
}

static Op gen_binary(Node *n)
{
    add_type(n);
    if (is_cplx(n->ty)) {
        Type *ct = n->ty, *b = ct->base;
        need_float(n);
        int x = gen_reg(n->lhs), y = gen_reg(n->rhs);
        if (n->kind == ND_ADD || n->kind == ND_SUB)
            return cmake(ct, fop(n->kind, b, cpart(x, ct, 0), cpart(y, ct, 0)), fop(n->kind, b, cpart(x, ct, 1), cpart(y, ct, 1)));
        if (n->kind != ND_MUL && n->kind != ND_DIV) error_at(n->tok, "invalid operation on complex numbers");
        Type *t[4] = { b, b, b, b };               /* __muldc3(a, b, c, d): (a+bi)(c+di), with IEEE care */
        Op v[4] = { cpart(x, ct, 0), cpart(x, ct, 1), cpart(y, ct, 0), cpart(y, ct, 1) };
        return helper_n(fmt("__%s%s", n->kind == ND_MUL ? "mul" : "div", csuffix(ct)), ct, 4, t, v);
    }
    if (is_i128(n->ty)) {
        int u = n->ty->is_unsigned;
        if (n->kind == ND_DIV || n->kind == ND_MOD) {
            static const char *h[2][2] = { { "__divti3", "__modti3" }, { "__udivti3", "__umodti3" } };
            Op a = gen(n->lhs), b = gen(n->rhs);
            return helper(h[u][n->kind == ND_MOD], n->ty, n->ty, a, n->ty, b);
        }
        if (n->kind == ND_SHL || n->kind == ND_SHR) {
            int a = gen_reg(n->lhs);
            add_type(n->rhs);
            Op c = is_i128(n->rhs->ty) ? cast(gen(n->rhs), n->rhs->ty, ty_int, n) : gen(n->rhs);
            int d = ld_slot();
            Ins *i = emit(O_I128SH);
            i->a = a;
            if (c.k) { i->bimm = 1; i->imm = c.imm & 127; } else i->b = reg(c, 4);
            i->sz = n->kind == ND_SHL ? 0 : u ? 1 : 2;
            i->m = addr_of_reg(d);
            return R(d);
        }
        int k = n->kind == ND_ADD ? 0 : n->kind == ND_SUB ? 1 : n->kind == ND_MUL ? 2 : n->kind == ND_AND ? 3 : n->kind == ND_OR ? 4 : 5;
        int a = gen_reg(n->lhs), b = gen_reg(n->rhs);
        return i128_op(O_I128, k, a, b);
    }
    if (is_ld(n->ty)) {
        need_float(n);
        int a = gen_reg(n->lhs), b = gen_reg(n->rhs);
        int k = n->kind == ND_ADD ? 0 : n->kind == ND_SUB ? 1 : n->kind == ND_MUL ? 2 : n->kind == ND_DIV ? 3 : -1;
        if (k < 0) error_at(n->tok, "invalid floating-point operation");
        return ld_op(O_LDBIN, k, a, b);
    }
    if (is_flonum(n->ty)) {
        need_float(n);
        int a = gen_reg(n->lhs), b = gen_reg(n->rhs);
        int d = new_vreg(C_FLT);
        int op = n->kind == ND_ADD ? O_FADD : n->kind == ND_SUB ? O_FSUB : n->kind == ND_MUL ? O_FMUL : O_FDIV;
        if (n->kind != ND_ADD && n->kind != ND_SUB && n->kind != ND_MUL && n->kind != ND_DIV) error_at(n->tok, "invalid floating-point operation");
        Ins *i = emit(op);
        i->dst = d; i->a = a; i->b = b; i->w = n->ty->size;
        return R(d);
    }
    int w = wof(n->ty);
    int u = n->ty->is_unsigned || n->ty->kind == TY_PTR;
    Op a = gen(n->lhs), b = gen(n->rhs);
    switch (n->kind) {
    case ND_ADD: return arith(O_ADD, a, b, w);
    case ND_SUB: return arith(O_SUB, a, b, w);
    case ND_MUL: return arith(O_MUL, a, b, w);
    case ND_DIV: return arith(u ? O_UDIV : O_DIV, a, b, w);
    case ND_MOD: return arith(u ? O_UMOD : O_MOD, a, b, w);
    case ND_AND: return arith(O_AND, a, b, w);
    case ND_OR: return arith(O_OR, a, b, w);
    case ND_XOR: return arith(O_XOR, a, b, w);
    case ND_SHL: return arith(O_SHL, a, b, w);
    case ND_SHR: return arith(n->lhs->ty->is_unsigned ? O_SHR : O_SAR, a, b, w);
    }
    error_at(n->tok, "internal: binary %d", n->kind);
}

static int bits_of(int v, int to_cls, int w)   /* a float's bits in an integer register, or back */
{
    int d = new_vreg(to_cls);
    Ins *i = emit(O_BITS);
    i->dst = d; i->a = v; i->w = w;
    return d;
}

/* An _Atomic object: stores swap (xchg: a full barrier), and x op= y is
 * load, compute, compare-and-swap, and again until no one changed it. */
static Op gen_atomic_assign(Node *n)
{
    Node *l = n->lhs;
    Type *t = l->ty;
    if (t->kind == TY_STRUCT || t->kind == TY_UNION || is_mem(t)) error_at(n->tok, "this atomic type is not supported here");
    int w = wof(t), fl = clsof(t) == C_FLT, sz = t->size;
    int p = lea(gen_addr(l));
    Op saved = self_val;
    if (!n->val) {
        Op v = gen(n->rhs);
        int b = fl ? bits_of(reg(v, w), C_INT, w) : reg(v, 8);
        Ins *x = emit(O_XCHG);
        x->dst = new_vreg(C_INT); x->a = p; x->b = b; x->sz = sz; x->w = sz == 8 ? 8 : 4;
        return v;
    }
    int old = new_vreg(fl ? C_FLT : C_INT);       /* the value we think it has */
    Ins *ld = emit(O_LOAD);
    ld->dst = old; ld->m = addr_of_reg(p); ld->sz = sz; ld->w = w; ld->sgn = !t->is_unsigned && t->kind != TY_PTR; ld->cc = 1;
    Block *loop = new_block(), *done = new_block(), *retry = new_block();
    place(loop);
    self_val = R(old);
    Op nv = gen(n->rhs);
    self_val = saved;
    int nvr = reg(nv, w);
    int ob = fl ? bits_of(old, C_INT, w) : old, nb = fl ? bits_of(nvr, C_INT, w) : nvr;
    int got = new_vreg(C_INT);
    Ins *x = emit(O_CMPXCHG);
    x->dst = got; x->a = p; x->b = nb; x->m.base = ob; x->sz = sz; x->w = sz == 8 ? 8 : 4;
    x->sgn = !fl && ld->sgn;                       /* the value read back, extended as the load did */
    Ins *br = emit(O_BR);
    br->cc = CC_EQ; br->a = got; br->b = ob; br->w = sz == 8 ? 8 : 4; br->t = done; br->f = retry;
    place(retry);
    if (fl) move(old, R(bits_of(got, C_FLT, w)), w);
    else move(old, R(got), w);
    jump(loop);
    place(done);
    return n->val == 2 ? R(old) : R(nvr);
}

static Op gen_assign(Node *n)
{
    Node *l = n->lhs;
    add_type(n);
    Type *t = l->ty;
    Op saved = self_val, result;
    if (t->is_atomic) return gen_atomic_assign(n);
    if (l->kind == ND_VAR && l->var->is_local && l->var->vreg) {      /* a register variable */
        int v = l->var->vreg;
        int w = wof(t);
        Op old = R(v);
        if (n->val == 2) old = R(copy(v, clsof(t), w));
        self_val = R(v);
        Op r = gen(n->rhs);
        if (r.k || r.v != v) {
            if (r.k && clsof(t) == C_FLT) r = R(reg(r, w));
            move(v, r, w);
        }
        self_val = saved;
        return n->val == 2 ? old : R(v);
    }
    if (is_ld(t) || is_i128(t) || is_cplx(t)) {    /* copied, by address */
        int da = lea(gen_addr(l));
        Op old = I(0);
        if (n->val) {
            self_val = R(da);
            if (n->val == 2) { int c = lea(slot_addr(new_slot(t->size, t->align))); copy_mem(c, da, t->size); old = R(c); }
        }
        int src = gen_reg(n->rhs);
        self_val = saved;
        copy_mem(da, src, t->size);
        return n->val == 2 ? old : R(da);
    }
    if (t->kind == TY_STRUCT || t->kind == TY_UNION) {
        Addr d = gen_addr(l);
        int src = gen_reg(n->rhs);
        int da = lea(d);
        copy_mem(da, src, t->size);
        return R(da);
    }
    if (l->kind == ND_MEMBER && l->member->is_bitfield) {
        Addr a = gen_addr(l->lhs);
        Op old = I(0);
        if (n->val) { self_val = bitfield_load(a, l->member); old = self_val; }
        Op r = gen(n->rhs);
        self_val = saved;
        Op nv = bitfield_store(a, l->member, r);
        return n->val == 2 ? old : nv;
    }
    Addr a = gen_addr(l);
    Op old = I(0);
    if (n->val) {
        self_val = load(a, t);
        old = self_val;
    }
    Op r = gen(n->rhs);
    self_val = saved;
    if (r.k && clsof(t) == C_FLT) r = R(reg(r, t->size));
    store(a, r, t);
    result = n->val == 2 ? old : r;
    return result;
}

/* ---- Vectors: values in memory (their address), like structs. An operation
 * that SSE instructions do is one O_VEC (16 bytes at a time); the others go
 * element by element, as scalar code. */
static int vec_ok(int op, int kind, int es)
{
    if (target == T_AARCH64) return a64_vec_ok(op, kind, es);
    switch (op) {
    case VOP_NEG: case VOP_NOT: case VOP_MOVEMASK: return 1;
    case VOP_NE: op = VOP_EQ; break;
    case VOP_LT: case VOP_LE: case VOP_GE: op = VOP_GT; break;
    }
    if (kind == 1 && op == VOP_GT) return 0;                 /* (SSE has no unsigned compares) */
    if (kind == 2 && (op == VOP_EQ || op == VOP_GT)) return 1;
    return vec_insn(op, kind, es) != 0;
}

/* AArch64 NEON operations (arm_neon.h): one O_VEC each. The result goes to a
 * new slot; an accumulating operation (n->cond: its old value) starts from a
 * copy of it. VOP_ADDV gives a scalar, in a register. */
static Op gen_neon(Node *n, int es, int kind)
{
    Node *l = n->lhs, *r = n->rhs;
    Type *t = n->ty;
    if (n->val == VOP_ADDV) {
        int a = gen_reg(l), d = new_vreg(is_flonum(t) ? C_FLT : C_INT);
        if (is_flonum(t)) need_float(n);
        Ins *i = emit(O_VEC);
        i->dst = d; i->a = a; i->cc = VOP_ADDV; i->sz = es; i->sgn = kind; i->w = l->ty->size;
        return R(d);
    }
    int d = lea(slot_addr(new_slot(t->size, 16)));
    if (n->cond) copy_mem(d, gen_reg(n->cond), t->size);
    int a = gen_reg(l), b = r ? gen_reg(r) : 0;
    Ins *i = emit(O_VEC);
    i->a = a; i->b = b; i->m.base = d; i->m.scale = 1; i->cc = n->val; i->sz = es; i->sgn = kind; i->w = l->ty->size; i->imm = n->hi;
    i->npin = r ? r->ty->size : 0;                  /* (O_VEC: b's size, for the table lookup) */
    return R(d);
}

static Op gen_vec(Node *n)
{
    Node *l = n->lhs, *r = n->rhs;
    Type *t = n->ty, *lt = l->ty, *e = t->is_vector ? t->base : 0;
    int op = n->val, es = lt->is_vector ? lt->base->size : 0, kind = lt->is_vector ? (is_flonum(lt->base) ? 2 : lt->base->is_unsigned) : 0;
    if (target == T_AARCH64) {
        if (opt_general_regs_only && op != VOP_SPLAT && op != VOP_SHUF) error_at(n->tok, "a vector operation with -mgeneral-regs-only");
        if (op >= VOP_FMA) return gen_neon(n, es, kind);
        if (op == VOP_MOVEMASK || (op >= VOP_MADD && op <= VOP_PSHUFD && op != VOP_SPLAT && op != VOP_SHUF && op != VOP_CVTF2IR &&
                                       op != VOP_AVG && op != VOP_ADDS && op != VOP_SUBS))
            error_at(n->tok, "an x86 vector operation, not on AArch64 (arm_neon.h has its own)");
    }
    if (op == VOP_MOVEMASK) {
        int a = gen_reg(l), d = new_vreg(C_INT);
        Ins *i = emit(O_VEC);
        i->dst = d; i->a = a; i->cc = op; i->sz = es; i->sgn = kind; i->w = lt->size;
        return R(d);
    }
    int d = lea(slot_addr(new_slot(t->size, 16))), cnt = t->size / e->size;
    if (op == VOP_SPLAT) {                                   /* a scalar in every element */
        Op x = gen(l);
        if (!x.k) x = R(reg(x, wof(e)));
        if (target == T_AARCH64 && !opt_general_regs_only) {  /* one dup */
            int v = reg(x, wof(e));                             /* (before the O_VEC: it may emit a move) */
            Ins *i = emit(O_VEC);
            i->a = v; i->m.base = d; i->m.scale = 1; i->cc = VOP_SPLAT; i->sz = e->size;
            i->sgn = is_flonum(e) ? 2 : e->is_unsigned; i->w = t->size;
            return R(d);
        }
        for (int k = 0; k < cnt; k++) { Addr m = addr_of_reg(d); m.off = k * e->size; store(m, x, e); }
        return R(d);
    }
    int a = gen_reg(l), b = r ? gen_reg(r) : 0;
    if (op == VOP_SHUF) {                                    /* elements by index */
        int half = lt->size / es, k = 0;
        for (Node *x = n->args; x; x = x->next, k++) {
            Addr s = addr_of_reg(x->val < half ? a : b), m = addr_of_reg(d);
            s.off = (x->val % half) * es;
            m.off = k * es;
            store(m, load(s, e), e);
        }
        return R(d);
    }
    if (vec_ok(op, kind, es)) {
        Ins *i = emit(O_VEC);
        i->a = a; i->b = b; i->m.base = d; i->m.scale = 1; i->cc = op; i->sz = es; i->sgn = kind; i->w = lt->size; i->imm = n->hi;
        return R(d);
    }
    /* element by element (integers: SSE has no such instruction) */
    Type *et = lt->base;
    int w = es == 8 ? 8 : 4, u = kind == 1;
    for (int k = 0; k < cnt; k++) {
        Addr ma = addr_of_reg(a), mb = addr_of_reg(b), md = addr_of_reg(d);
        ma.off = mb.off = md.off = k * es;
        Op x = load(ma, et), y = b ? load(mb, et) : I(n->hi), res;
        int cc = -1;
        switch (op) {
        case VOP_ADD: res = arith(O_ADD, x, y, w); break;
        case VOP_SUB: res = arith(O_SUB, x, y, w); break;
        case VOP_MUL: res = arith(O_MUL, x, y, w); break;
        case VOP_DIV: res = arith(u ? O_UDIV : O_DIV, x, y, w); break;
        case VOP_MOD: res = arith(u ? O_UMOD : O_MOD, x, y, w); break;
        case VOP_AND: res = arith(O_AND, x, y, w); break;
        case VOP_OR: res = arith(O_OR, x, y, w); break;
        case VOP_XOR: res = arith(O_XOR, x, y, w); break;
        case VOP_SHL: case VOP_SHLI: res = arith(O_SHL, x, y, w); break;
        case VOP_SHR: res = arith(u ? O_SHR : O_SAR, x, y, w); break;
        case VOP_SHRI: res = arith(O_SHR, x, y, w); break;
        case VOP_SARI: res = arith(O_SAR, x, y, w); break;
        case VOP_EQ: cc = CC_EQ; break;
        case VOP_NE: cc = CC_NE; break;
        case VOP_LT: cc = u ? CC_ULT : CC_LT; break;
        case VOP_LE: cc = u ? CC_ULE : CC_LE; break;
        case VOP_GT: cc = u ? CC_UGT : CC_GT; break;
        case VOP_GE: cc = u ? CC_UGE : CC_GE; break;
        case VOP_MIN: case VOP_MAX: {                       /* y ^ ((x ^ y) & -(x < y)), or > */
            int s = new_vreg(C_INT);
            Ins *c = emit(O_SET);
            c->dst = s; c->a = reg(x, w); c->b = reg(y, w); c->cc = op == VOP_MIN ? (u ? CC_ULT : CC_LT) : (u ? CC_UGT : CC_GT); c->w = w;
            Op mask = arith(O_SUB, I(0), R(s), w);
            res = arith(O_XOR, y, arith(O_AND, arith(O_XOR, x, y, w), mask, w), w);
            break;
        }
        default: error_at(n->tok, "this vector operation needs an instruction set extension (-mavx2)");
        }
        if (cc >= 0) {                                       /* a comparison: all ones, or zero */
            int s = new_vreg(C_INT);
            Ins *c = emit(O_SET);
            c->dst = s; c->a = reg(x, w); c->b = reg(y, w); c->cc = cc; c->w = w;
            res = arith(O_SUB, I(0), R(s), w);
        }
        store(md, res, e);
    }
    return R(d);
}

static Op gen(Node *n)
{
    add_type(n);
    switch (n->kind) {
    case ND_VEC: return gen_vec(n);
    case ND_NUM:
        if (is_ld(n->ty)) { need_float(n); return ld_const(n->ldval); }
        if (is_i128(n->ty)) return i128_ext(I(n->val), 1);
        if (is_cplx(n->ty)) {                      /* fval: real part, ldval: imaginary part */
            Type *b = n->ty->base;
            need_float(n);
            return cmake(n->ty, is_ld(b) ? ld_const(n->fval) : fconst(n->fval, b->size), is_ld(b) ? ld_const(n->ldval) : fconst(n->ldval, b->size));
        }
        if (is_flonum(n->ty)) { need_float(n); return fconst(n->fval, n->ty->size); }
        return I(n->val);
    case ND_LABEL_VAL: { Addr a = { 0 }; a.scale = 1; a.sym = n->label; return R(lea(a)); }
    case ND_VAR: {
        Obj *v = n->var;
        if (v->is_local && v->vreg) return R(v->vreg);
        return load(var_addr(v), n->ty);
    }
    case ND_SELF: return self_val;
    case ND_IRVAL: return n->lo ? I(n->val) : R(n->val);
    case ND_MEMBER:
        if (n->member->is_bitfield) return bitfield_load(gen_addr(n->lhs), n->member);
        return load(gen_addr(n), n->ty);
    case ND_DEREF:
        if (n->ty->kind == TY_FUNC) return R(gen_reg(n->lhs));
        return load(gen_addr(n), n->ty);
    case ND_ADDR:
        if (n->lhs->kind == ND_VAR && n->lhs->var->ty->kind == TY_FUNC) return R(lea(var_addr(n->lhs->var)));
        return R(lea(gen_addr(n->lhs)));
    case ND_ASSIGN: return gen_assign(n);
    case ND_COMMA: gen(n->lhs); return gen(n->rhs);
    case ND_CAST: {
        add_type(n->lhs);
        if (n->lhs->ty->kind == TY_INT128 && is_integer(n->ty) && n->ty->kind != TY_INT128 && opt_optimize && narrowable(n->lhs))
            return cast(narrow128(n->lhs), n->lhs->ty->is_unsigned ? ty_ulong : ty_long, n->ty, n);
        if (n->ty->is_vector) return gen(n->lhs);   /* between vectors of one size: the same bytes */
        Op v = gen(n->lhs);
        return cast(v, n->lhs->ty, n->ty, n);
    }
    case ND_NEG:
        if (is_cplx(n->ty)) { int x = gen_reg(n->lhs); Type *b = n->ty->base; return cmake(n->ty, fop(ND_NEG, b, cpart(x, n->ty, 0), I(0)), fop(ND_NEG, b, cpart(x, n->ty, 1), I(0))); }
        if (is_i128(n->ty)) return i128_op(O_I128, 6, gen_reg(n->lhs), 0);
        if (is_ld(n->ty)) { need_float(n); return ld_op(O_LDNEG, 0, gen_reg(n->lhs), 0); }
        if (is_flonum(n->ty)) {
            need_float(n);
            int x = gen_reg(n->lhs), d = new_vreg(C_FLT);
            Ins *i = emit(O_FNEG);
            i->dst = d; i->a = x; i->w = n->ty->size;
            return R(d);
        }
        return arith(O_SUB, I(0), gen(n->lhs), wof(n->ty));
    case ND_BITNOT: {                              /* (of a complex: the conjugate) */
        if (is_cplx(n->ty)) { int x = gen_reg(n->lhs); return cmake(n->ty, cpart(x, n->ty, 0), fop(ND_NEG, n->ty->base, cpart(x, n->ty, 1), I(0))); }
        if (is_i128(n->ty)) return i128_op(O_I128, 7, gen_reg(n->lhs), 0);
        Op v = gen(n->lhs);
        if (v.k) return I(wof(n->ty) == 4 ? (long)(int)~v.imm : ~v.imm);
        int d = new_vreg(C_INT);
        Ins *i = emit(O_NOT);
        i->dst = d; i->a = v.v; i->w = wof(n->ty);
        return R(d);
    }
    case ND_NOT: case ND_LOGAND: case ND_LOGOR: return gen_logical(n);
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE: {
        add_type(n->lhs);
        if (is_ld(n->lhs->ty)) { int a = gen_reg(n->lhs), b = gen_reg(n->rhs); return R(ld_cmp(a, b, cmp_cc(n, 0))); }
        if (is_i128(n->lhs->ty)) { int a = gen_reg(n->lhs), b = gen_reg(n->rhs); return R(i128_cmp(a, b, cmp_cc(n, 0))); }
        if (is_cplx(n->lhs->ty)) {                 /* == and !=: both parts */
            Type *ct = n->lhs->ty;
            int x = gen_reg(n->lhs), y = gen_reg(n->rhs), eq = n->kind == ND_EQ;
            Op r = fcmp(ct->base, cpart(x, ct, 0), cpart(y, ct, 0), eq ? CC_FEQ : CC_FNE);
            Op i = fcmp(ct->base, cpart(x, ct, 1), cpart(y, ct, 1), eq ? CC_FEQ : CC_FNE);
            return arith(eq ? O_AND : O_OR, r, i, 4);
        }
        Op a = gen(n->lhs), b = gen(n->rhs);
        int fl = is_flonum(n->lhs->ty);
        int w = fl ? n->lhs->ty->size : wof(n->lhs->ty);
        int swap = 0;
        if (a.k && !fl) { Op t = a; a = b; b = t; swap = 1; }
        if (a.k && b.k) {
            long x = a.imm, y = b.imm;
            int u = n->lhs->ty->is_unsigned;
            if (w == 4) { x = u ? (long)(unsigned)x : (int)x; y = u ? (long)(unsigned)y : (int)y; }
            if (swap) { long t = x; x = y; y = t; }
            switch (n->kind) {
            case ND_EQ: return I(x == y);
            case ND_NE: return I(x != y);
            case ND_LT: return I(u ? (unsigned long)x < (unsigned long)y : x < y);
            default: return I(u ? (unsigned long)x <= (unsigned long)y : x <= y);
            }
        }
        int bimm = !fl && b.k && fits32(b.imm);
        int ra = reg(a, w), rb = bimm ? 0 : reg(b, w);
        int d = new_vreg(C_INT);
        Ins *i = emit(O_SET);
        i->dst = d;
        i->a = ra;
        i->cc = cmp_cc(n, swap);
        i->w = w;
        if (bimm) { i->bimm = 1; i->imm = b.imm; }
        else i->b = rb;
        return R(d);
    }
    case ND_COND: {
        Block *t = new_block(), *f = new_block(), *done = new_block();
        int agg = is_mem(n->ty);
        int cls = agg ? C_INT : clsof(n->ty), w = agg ? 8 : wof(n->ty);
        int r = n->ty->kind == TY_VOID ? 0 : new_vreg(cls);
        gen_cond(n->cond, t, f);
        place(t);
        Op a = gen(n->then);
        if (r) { if (a.k && cls == C_FLT) a = R(reg(a, w)); move(r, a, w); }
        jump(done);
        place(f);
        Op b = gen(n->els);
        if (r) { if (b.k && cls == C_FLT) b = R(reg(b, w)); move(r, b, w); }
        place(done);
        return r ? R(r) : I(0);
    }
    case ND_CALL: return gen_call(n);
    case ND_BUILTIN: return gen_builtin(n);
    case ND_STMT_EXPR: {
        Node *last = n->body;
        while (last && last->next) last = last->next;
        for (Node *s = n->body; s != last; s = s->next) gen_stmt(s);
        if (!last) return I(0);
        if (last->kind == ND_EXPR_STMT) return gen(last->lhs);
        gen_stmt(last);
        return I(0);
    }
    case ND_MEMZERO: {
        int a = lea(var_addr(n->var));
        zero_mem(a, n->var->ty->size);
        return I(0);
    }
    case ND_NULL: return I(0);
    }
    if (n->kind >= ND_ADD && n->kind <= ND_SHR) return gen_binary(n);
    error_at(n->tok, "internal: expression kind %d", n->kind);
}

/* ---- Conditions */
static void gen_cond(Node *n, Block *t, Block *f)
{
    add_type(n);
    switch (n->kind) {
    case ND_LOGAND: {
        Block *mid = new_block();
        gen_cond(n->lhs, mid, f);
        place(mid);
        gen_cond(n->rhs, t, f);
        return;
    }
    case ND_LOGOR: {
        Block *mid = new_block();
        gen_cond(n->lhs, t, mid);
        place(mid);
        gen_cond(n->rhs, t, f);
        return;
    }
    case ND_NOT: gen_cond(n->lhs, f, t); return;
    case ND_NUM:
        if (!is_flonum(n->ty)) { jump(n->val ? t : f); dead_code(); return; }
        break;
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE: {
        add_type(n->lhs);
        if (is_ld(n->lhs->ty) || is_i128(n->lhs->ty) || is_cplx(n->lhs->ty)) break;   /* below: a 0/1 value, then a branch */
        int fl = is_flonum(n->lhs->ty);
        int w = fl ? n->lhs->ty->size : wof(n->lhs->ty);
        Op a = gen(n->lhs), b = gen(n->rhs);
        int swap = 0;
        if (a.k && !fl) { Op x = a; a = b; b = x; swap = 1; }
        if (a.k && b.k) {
            Op v = gen(n);
            (void)v;
            break;
        }
        int bimm = !fl && b.k && fits32(b.imm);
        int ra = reg(a, w), rb = bimm ? 0 : reg(b, w);
        Ins *i = emit(O_BR);
        i->a = ra;
        i->cc = cmp_cc(n, swap);
        i->w = w;
        if (bimm) { i->bimm = 1; i->imm = b.imm; }
        else i->b = rb;
        i->t = t;
        i->f = f;
        dead_code();
        return;
    }
    }
    Op v = gen(n);
    if (v.k) { jump(v.imm ? t : f); dead_code(); return; }
    if (is_ld(n->ty)) v = R(ld_cmp(v.v, 0, CC_FNE));     /* a long double: != 0 */
    if (is_i128(n->ty) || is_cplx(n->ty)) v = cast(v, n->ty, ty_bool, n);
    int zero = is_flonum(n->ty) && !is_ld(n->ty) ? fconst(0, n->ty->size).v : 0;
    Ins *i = emit(O_BR);
    if (is_flonum(n->ty) && !is_ld(n->ty)) {
        i->a = v.v;
        i->b = zero;
        i->cc = CC_FNE;
        i->w = n->ty->size;
    } else {
        i->a = v.v;
        i->bimm = 1;
        i->imm = 0;
        i->cc = CC_NE;
        i->w = wof(n->ty);
    }
    i->t = t;
    i->f = f;
    dead_code();
}

/* ---- Statements */
static Block *label_block(const char *name)
{
    Block *b = map_get(&labels, name);
    if (!b) { b = new_block(); map_put(&labels, name, b); }
    return b;
}

static void gen_switch(Node *n)
{
    add_type(n->cond);
    int w = wof(n->cond->ty);
    int v = gen_reg(n->cond);
    Block *brk = new_block();
    n->brk->blk = brk;
    /* cases, in source order */
    Vec cs = { 0 };
    for (Node *c = n->cases; c; c = c->case_next) vec_push(&cs, c);
    for (int i = 0, j = cs.n - 1; i < j; i++, j--) { void *t = cs.v[i]; cs.v[i] = cs.v[j]; cs.v[j] = t; }
    for (int i = 0; i < cs.n; i++) ((Node *)cs.v[i])->blk = new_block();
    Block *dflt = brk;
    if (n->dflt) { n->dflt->blk = new_block(); dflt = n->dflt->blk; }
    long min = 0, max = 0, count = 0;
    for (int i = 0; i < cs.n; i++) {
        Node *c = cs.v[i];
        if (i == 0 || c->lo < min) min = c->lo;
        if (i == 0 || c->hi > max) max = c->hi;
        count += c->hi - c->lo + 1;
    }
    unsigned long range = (unsigned long)(max - min);
    if (cs.n >= 4 && range < 4096 && range <= (unsigned long)count * 3) {      /* a jump table */
        Op d = arith(O_SUB, R(v), I(min), w);
        int dv = reg(d, w);
        Block *in = new_block();
        Ins *br = emit(O_BR);
        br->cc = CC_UGT; br->a = dv; br->bimm = 1; br->imm = range; br->w = w; br->t = dflt; br->f = in;
        place(in);
        int idx = dv;
        if (w == 4) {
            idx = new_vreg(C_INT);
            Ins *x = emit(O_EXT);
            x->dst = idx; x->a = dv; x->sz = 4; x->sgn = 0; x->w = 8;
        }
        Ins *sw = emit(O_SWITCH);
        sw->a = idx;
        sw->ntab = range + 1;
        sw->tab = arena(sizeof(Block *) * (range + 1));
        for (unsigned long k = 0; k <= range; k++) sw->tab[k] = dflt;
        for (int i = cs.n - 1; i >= 0; i--) {
            Node *c = cs.v[i];
            for (long x = c->lo; x <= c->hi; x++) sw->tab[x - min] = c->blk;
        }
        dead_code();
    } else {
        for (int i = 0; i < cs.n; i++) {
            Node *c = cs.v[i];
            Block *next = new_block();
            Ins *br;
            if (c->lo == c->hi) {
                int k = fits32(c->lo) ? 0 : mov_imm(c->lo, w);
                br = emit(O_BR);
                br->cc = CC_EQ;
                br->a = v;
                if (k) br->b = k; else { br->bimm = 1; br->imm = c->lo; }
            } else {                            /* lo <= v <= hi: (unsigned)(v - lo) <= hi - lo */
                int dv = reg(arith(O_SUB, R(v), I(c->lo), w), w);
                br = emit(O_BR);
                br->cc = CC_ULE;
                br->a = dv;
                br->bimm = 1;
                br->imm = c->hi - c->lo;
            }
            br->w = w;
            br->t = c->blk;
            br->f = next;
            place(next);
        }
        jump(dflt);
        dead_code();
    }
    gen_stmt(n->then);
    place(brk);
}

static void gen_return(Node *n)
{
    Ins *r;
    if (inl) {                                     /* in an inlined body: the result, and out */
        if (n->lhs) {
            add_type(n->lhs);
            Type *t = n->lhs->ty;
            if (t->kind == TY_STRUCT || t->kind == TY_UNION || is_mem(t)) copy_mem(inl->resaddr, gen_reg(n->lhs), t->size);
            else if (inl->res) move(inl->res, gen(n->lhs), wof(inl->rt));
            else gen(n->lhs);
        }
        jump(inl->exit);
        dead_code();
        return;
    }
    if (!n->lhs) { emit(O_RET); dead_code(); return; }
    Type *t = n->lhs->ty;
    add_type(n->lhs);
    t = n->lhs->ty;
    if (is_ld(t) || (is_cplx(t) && is_ld(t->base))) {   /* in st0 (and st1) */
        int a = gen_reg(n->lhs);
        r = emit(O_RET);
        r->a = a;
        r->w = 8;
        r->sz = is_ld(t) ? 2 : 3;
        dead_code();
        return;
    }
    if (t->kind == TY_STRUCT || t->kind == TY_UNION || is_i128(t) || is_cplx(t)) {
        int a = gen_reg(n->lhs);
        if (target == T_AARCH64) { a64_return(t, a); dead_code(); return; }
        int cls[2], ne = classify(t, cls);
        if (!ne) {                                 /* through the caller's buffer */
            copy_mem(retptr, a, t->size);
            r = emit(O_RET);
            r->a = retptr;
            r->w = 8;
            dead_code();
            return;
        }
        int v0 = load_eightbyte(a, t, 0, cls[0]), v1 = ne > 1 ? load_eightbyte(a, t, 1, cls[1]) : 0;
        r = emit(O_RET);
        r->a = v0;
        r->b = v1;
        r->w = 8;
        r->sz = (cls[0] == C_FLT) | (ne > 1 && cls[1] == C_FLT) << 1;
        r->sgn = 1;                                /* a struct in registers */
        dead_code();
        return;
    }
    Op v = gen(n->lhs);
    int fv = is_flonum(t) ? reg(v, t->size) : 0;
    r = emit(O_RET);
    r->w = wof(t);
    if (is_flonum(t)) { r->a = fv; r->sz = 1; }
    else if (t->kind != TY_VOID) {
        if (v.k) { r->bimm = 1; r->imm = v.imm; }
        else r->a = v.v;
    }
    dead_code();
}

static void gen_stmt(Node *n)
{
    if (opt_debug && n->kind != ND_BLOCK) cur_src = src_pos(n->tok);
    switch (n->kind) {
    case ND_NULL: return;
    case ND_BLOCK: for (Node *s = n->body; s; s = s->next) gen_stmt(s); return;
    case ND_EXPR_STMT: gen(n->lhs); return;
    case ND_RETURN: gen_return(n); return;
    case ND_IF: {
        Block *t = new_block(), *f = new_block(), *done = n->els ? new_block() : f;
        gen_cond(n->cond, t, f);
        place(t);
        gen_stmt(n->then);
        if (n->els) {
            jump(done);
            place(f);
            gen_stmt(n->els);
        }
        place(done);
        return;
    }
    case ND_FOR: {
        if (n->init) gen_stmt(n->init);
        Block *body = new_block(), *cont = new_block(), *brk = new_block(), *test = new_block();
        n->brk->blk = brk;
        n->cont->blk = cont;
        /* test at the bottom: one branch per iteration */
        jump(n->cond ? test : body);
        place(body);
        gen_stmt(n->then);
        place(cont);
        if (n->inc) gen(n->inc);
        place(test);
        if (n->cond) gen_cond(n->cond, body, brk);
        else jump(body);
        place(brk);
        return;
    }
    case ND_DO: {
        Block *body = new_block(), *cont = new_block(), *brk = new_block();
        n->brk->blk = brk;
        n->cont->blk = cont;
        place(body);
        gen_stmt(n->then);
        place(cont);
        gen_cond(n->cond, body, brk);
        place(brk);
        return;
    }
    case ND_SWITCH: gen_switch(n); return;
    case ND_CASE: place(n->blk); gen_stmt(n->lhs); return;
    case ND_GOTO:
        if (n->lhs) {                              /* goto *p: a switch whose targets are every &&label */
            int p = gen_reg(n->lhs);
            Ins *i = emit(O_SWITCH);
            i->a = p;
            i->cc = 1;                             /* (cc on a switch: computed) */
            vec_push(&cgotos, i);
            dead_code();
            return;
        }
        /* fall through */
    case ND_BREAK: case ND_CONTINUE:
        if (n->var) {                              /* leaving VLA blocks: their space back */
            int sp = gen_reg(&(Node){ .kind = ND_VAR, .var = n->var, .ty = n->var->ty, .tok = n->tok });
            Ins *i = emit(O_SPRESTORE);
            i->a = sp;
        }
        jump(n->kind == ND_BREAK ? n->brk->blk : n->kind == ND_CONTINUE ? n->cont->blk : label_block(n->label));
        dead_code();
        return;
    case ND_LABEL: {
        Block *b = label_block(n->label);
        if (n->val && !b->addr_taken) { b->sym = n->label; b->addr_taken = 1; vec_push(&addr_blocks, b); }
        place(b);
        gen_stmt(n->lhs);
        return;
    }
    case ND_ASM: gen_asm(n); return;
    }
    gen(n);
}

/* ---- A function */
void lower_function(Obj *fn)
{
    F = arena(sizeof *F);
    F->obj = fn;
    cur_src = src_pos(fn->tok);
    inl = 0;
    inl_depth = inl_frame = 0;
    labels = (Map){ 0 };
    addr_blocks.n = cgotos.n = 0;
    cur = new_block();
    F->blocks = F->last = cur;
    /* where each local lives */
    for (Obj *v = fn->locals; v; v = v->next) {
        v->vreg = 0;
        v->slot = 0;
        if (is_scalar(v->ty) && !is_mem(v->ty) && !v->addr_taken && !v->ty->is_volatile && !v->ty->is_atomic) {
            v->vreg = new_vreg(clsof(v->ty));
            F->vr[v->vreg].w = wof(v->ty);
            if (v->reg) F->vr[v->vreg].fixed = v->reg;
            if (is_flonum(v->ty) && opt_general_regs_only) error_at(v->tok, "floating point is not allowed with -mgeneral-regs-only");
        }
    }
    /* the parameters: where they arrive (System V) */
    Type *rt = fn->ty->base;
    int gp = 0, fp = 0;
    long stk = 0;
    Vec pins = { 0 };
    int rcls[2];
    retptr = 0;
    Vec refs = { 0 };
    if (target == T_AARCH64) a64_params(fn, &pins, &refs);
    else {
    if ((rt->kind == TY_STRUCT || rt->kind == TY_UNION || is_i128(rt) || is_cplx(rt)) && !(is_cplx(rt) && is_ld(rt->base)) && !classify(rt, rcls)) {
        retptr = new_vreg(C_INT);
        ParamIn *p = arena(sizeof *p);
        p->v = retptr; p->phys = RDI; p->w = 8; p->cls = C_INT;
        vec_push(&pins, p);
        gp = 1;
    }
    for (Obj *v = fn->params; v; v = v->pnext) {
        Type *t = v->ty;
        if (t->kind == TY_STRUCT || t->kind == TY_UNION || is_ld(t) || is_i128(t) || is_cplx(t)) {
            int cls[2], ne = classify(t, cls), ngp = 0, nfp = 0;
            for (int i = 0; i < ne; i++) { if (cls[i] == C_INT) ngp++; else nfp++; }
            if (ne && gp + ngp <= 6 && fp + nfp <= 8) {
                v->slot = new_slot(align_to(t->size, 8), t->align > 8 ? t->align : 8);
                for (int i = 0; i < ne; i++) {
                    ParamIn *p = arena(sizeof *p);
                    p->slot = v->slot; p->off = i * 8; p->w = 8; p->cls = cls[i];
                    p->phys = cls[i] == C_INT ? argregs[gp++] : XMM0 + fp++;
                    vec_push(&pins, p);
                }
            } else {
                stk = align_to(stk, t->align > 8 ? 16 : 8);
                v->slot = -1;
                v->param_stack_off = stk;
                stk += align_to(t->size, 8);
            }
            continue;
        }
        int cls = clsof(t);
        ParamIn *p = arena(sizeof *p);
        p->w = cls == C_FLT ? t->size : wof(t);
        p->cls = cls;
        if (cls == C_FLT && fp < 8) p->phys = XMM0 + fp++;
        else if (cls == C_INT && gp < 6) p->phys = argregs[gp++];
        else { p->phys = -1; p->stack_off = stk; stk += 8; }
        if (v->vreg) { p->v = v->vreg; F->vr[v->vreg].hint = p->phys; }
        else if (p->phys < 0) {                    /* in memory already: use it in place */
            v->slot = -1;
            v->param_stack_off = p->stack_off;
            continue;
        } else {
            v->slot = new_slot(t->size, t->align);
            p->slot = v->slot;
            p->w = t->size;                     /* exactly the slot: a char's neighbours are other variables */
        }
        if (p->v && p->phys < 0 && t->size < 4 && is_integer(t)) p->w = 4;
        vec_push(&pins, p);
    }
    ngp_named = gp;
    nfp_named = fp;
    stack_named = stk;
    }
    for (Obj *v = fn->locals; v; v = v->next)
        if (!v->vreg && !v->slot) v->slot = new_slot(v->ty->size, v->align > v->ty->align ? v->align : v->ty->align);
    if (fn->va_area) F->va_slot = fn->va_area->slot;
    Ins *e = emit(O_ENTRY);
    e->npin = pins.n;
    e->pin = arena(sizeof(ParamIn) * (pins.n + 1));
    for (int i = 0; i < pins.n; i++) e->pin[i] = *(ParamIn *)pins.v[i];
    free(pins.v);
    /* parameters of type char/short arrive unextended: extend them */
    for (Obj *v = fn->params; v; v = v->pnext)
        if (v->vreg && is_integer(v->ty) && v->ty->size < 4) {
            int d = new_vreg(C_INT);
            Ins *x = emit(O_EXT);
            x->dst = d; x->a = v->vreg; x->sz = v->ty->size; x->sgn = !v->ty->is_unsigned; x->w = 4;
            move(v->vreg, R(d), 4);
        }
    a64_ref_copies(&refs);
    gen_stmt(fn->body);
    for (int k = 0; k < cgotos.n; k++) {          /* computed gotos: all the labels taken by address */
        Ins *i = cgotos.v[k];
        i->ntab = addr_blocks.n;
        i->tab = arena(sizeof(Block *) * (addr_blocks.n + 1));
        memcpy(i->tab, addr_blocks.v, sizeof(Block *) * addr_blocks.n);
    }
    if (!terminated(cur)) {
        Ins *r = emit(O_RET);
        if (!strcmp(fn->name, "main") && rt->kind == TY_INT) { r->bimm = 1; r->imm = 0; r->w = 4; }
    }
    /* blocks: drop the unreachable ones (also needed by the register allocator) */
    for (Block *b = F->blocks; b; b = b->next) b->used = 0;
    Vec work = { 0 };
    F->blocks->used = 1;
    vec_push(&work, F->blocks);
    for (int k = 0; k < addr_blocks.n; k++) { Block *b = addr_blocks.v[k]; b->used = 1; vec_push(&work, b); }
    while (work.n) {
        Block *b = work.v[--work.n];
        Ins *t = b->last;
        Block *succ[2] = { 0, 0 };
        if (!t || t->op != O_JMP) succ[0] = b->next;      /* falls through */
        if (t && t->op == O_RET) succ[0] = 0;
        if (t && t->op == O_UNREACH) succ[0] = 0;
        if (t && (t->op == O_BR || t->op == O_SWITCH)) succ[0] = 0;
        if (t && t->op == O_JMP) succ[0] = t->t;
        if (t && t->op == O_BR) { succ[0] = t->t; succ[1] = t->f; }
        if (t && t->op == O_SWITCH) for (int i = 0; i < t->ntab; i++) if (!t->tab[i]->used) { t->tab[i]->used = 1; vec_push(&work, t->tab[i]); }
        for (int k = 0; k < 2; k++) if (succ[k] && !succ[k]->used) { succ[k]->used = 1; vec_push(&work, succ[k]); }
    }
    free(work.v);
    Block **pp = &F->blocks;
    F->last = 0;
    for (Block *b = F->blocks; b; b = b->next) {
        if (!b->used) continue;
        /* a block that just falls through into an unreachable one: make the jump explicit */
        *pp = b;
        pp = &b->next;
        F->last = b;
    }
    *pp = 0;
    for (Block *b = F->blocks; b; b = b->next) {
        Ins *t = b->last;
        if ((!t || (t->op != O_JMP && t->op != O_BR && t->op != O_RET && t->op != O_SWITCH && t->op != O_UNREACH))) {
            if (!b->next) { Ins *r = arena(sizeof *r); r->op = O_RET; if (t) t->next = r; else b->first = r; b->last = r; }
        }
    }
}
