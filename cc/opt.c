/*
 * opt.c - Small clean-ups on a function's IR, before register allocation.
 *
 * - Copies: "t = x op y; v = t" (t used only there) becomes "v = x op y".
 *   Lowering produces this pattern for every assignment to a variable.
 * - Dead code: an instruction without side effects whose result nobody
 *   reads is deleted (and then, perhaps, the ones that fed it).
 * - Loop invariants: see hoist_loops().
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ir.h"

static int pure(Ins *i)                  /* no effect but its result */
{
    switch (i->op) {
    case O_MOV: case O_ADD: case O_SUB: case O_MUL: case O_AND: case O_OR: case O_XOR: case O_SHL: case O_SHR:
    case O_SAR: case O_DIV: case O_UDIV: case O_MOD: case O_UMOD: case O_NEG: case O_NOT: case O_EXT: case O_SET:
    case O_LEA: case O_FADD: case O_FSUB: case O_FMUL: case O_FDIV: case O_FNEG: case O_I2F: case O_F2I: case O_F2F:
    case O_BITOP: case O_MULSHR: case O_FRAMEADDR: case O_BITS: case O_FSQRT: case O_ROL:
        return 1;
    case O_LOAD: return !i->cc;          /* not a volatile load */
    }
    return 0;
}

/* ---- Loop-invariant code: a pure computation inside a loop whose inputs
 * never change in the loop moves before it (computed once, not every turn).
 * Loops are found by their back edge (a jump to an earlier block: the code
 * generator lays a loop's blocks out together); the block just before the
 * loop must be the only way in. Loads stay put (a store in the loop might
 * change memory), and so do divisions (they can trap).
 * Each hoisted value holds a register for the whole loop, so values move
 * only while registers remain: the loop's pressure is estimated as the
 * registers it reads from outside plus the variables it updates (per class:
 * NINT_REGS, NFLT_REGS allocatable). Only real work moves: constants,
 * copies and addresses cost nothing inside the loop (x86 folds them into
 * instructions), and hoisting them all once made a crypto loop spill a 4 KB
 * frame. */
enum { NINT_REGS = 10, NFLT_REGS = 8 };
static int succ_of(Block *b, Block **s)
{
    Ins *t = b->last;
    int n = 0;
    if (!t || (t->op != O_JMP && t->op != O_BR && t->op != O_RET && t->op != O_SWITCH && t->op != O_UNREACH)) { if (b->next) s[n++] = b->next; return n; }
    if (t->op == O_JMP) s[n++] = t->t;
    if (t->op == O_BR) { s[n++] = t->t; s[n++] = t->f; }
    if (t->op == O_SWITCH) return -1;
    return n;
}

static int hoistable(Ins *i)
{
    switch (i->op) {
    case O_ADD: case O_SUB: case O_MUL: case O_AND: case O_OR: case O_XOR: case O_SHL: case O_SHR:
    case O_SAR: case O_NEG: case O_NOT: case O_EXT: case O_LEA: case O_MULSHR: {
        int u[64];
        return ins_uses(i, u) > 0;               /* depends on a register */
    }
    }
    return 0;
}

static void hoist_loops(int *ndef)
{
    int nb = 0;
    for (Block *b = F->blocks; b; b = b->next) b->id = nb++;
    Block **at = malloc(sizeof(Block *) * (nb + 1));
    nb = 0;
    for (Block *b = F->blocks; b; b = b->next) at[nb++] = b;
    int nv = F->nvr + 1;
    int *inloop = calloc(nv, sizeof(int));
    char *counted = calloc(nv, 1);
    int u[64], d[64];
    for (int x = 0; x < nb; x++) {
        Block *s[3];
        int ns = succ_of(at[x], s);
        if (ns < 0) continue;
        for (int k = 0; k < ns; k++) {
            int h = s[k]->id, e = x;
            if (h > e || h == 0) continue;               /* not a back edge */
            Block *pre = at[h - 1];
            /* the only way in: from pre */
            int ok = 1;
            for (int y = 0; y < nb && ok; y++) {
                if (y >= h && y <= e) continue;
                Block *t[3];
                int nt = succ_of(at[y], t);
                if (nt < 0) { for (int z = 0; z < at[y]->last->ntab; z++) if (at[y]->last->tab[z]->id >= h && at[y]->last->tab[z]->id <= e) ok = 0; continue; }
                for (int z = 0; z < nt; z++) if (t[z]->id >= h && t[z]->id <= e && at[y] != pre) ok = 0;
            }
            if (!ok) continue;
            /* what the loop writes */
            memset(inloop, 0, sizeof(int) * nv);
            for (int y = h; y <= e; y++)
                for (Ins *i = at[y]->first; i; i = i->next) {
                    int n = ins_defs(i, d);
                    for (int z = 0; z < n; z++) inloop[d[z]]++;
                }
            int room[2] = { NINT_REGS, NFLT_REGS };
            memset(counted, 0, nv);
            for (int y = h; y <= e; y++)
                for (Ins *i = at[y]->first; i; i = i->next) {
                    int n = ins_uses(i, u), inv = hoistable(i) && i->dst && ndef[i->dst] == 1;
                    for (int z = 0; z < n && inv; z++) if (inloop[u[z]]) inv = 0;
                    if (inv) continue;                 /* (moves out, most likely: its operands do not count) */
                    for (int z = 0; z < n; z++) {
                        int v = u[z];
                        if (counted[v] || (inloop[v] && ndef[v] == 1)) continue;   /* (a temporary of the loop) */
                        counted[v] = 1;
                        room[F->vr[v].cls == C_FLT]--;
                    }
                }
            for (int moved = 1; moved; ) {
                moved = 0;
                for (int y = h; y <= e; y++) {
                    Ins *prev = 0;
                    for (Ins *i = at[y]->first; i; ) {
                        Ins *nx = i->next;
                        int inv = hoistable(i) && i->dst && ndef[i->dst] == 1 && inloop[i->dst] == 1 && !F->vr[i->dst].fixed &&
                                  room[F->vr[i->dst].cls == C_FLT] > 1;
                        if (inv) {
                            int n = ins_uses(i, u);
                            for (int z = 0; z < n; z++) if (inloop[u[z]]) inv = 0;
                        }
                        if (!inv) { prev = i; i = nx; continue; }
                        room[F->vr[i->dst].cls == C_FLT]--;
                        /* unlink, and insert before pre's last jump */
                        if (prev) prev->next = nx; else at[y]->first = nx;
                        if (at[y]->last == i) at[y]->last = prev;
                        Ins *t = pre->last;
                        int term = t && (t->op == O_JMP || t->op == O_BR);
                        if (!term) { i->next = 0; if (t) t->next = i; else pre->first = i; pre->last = i; }
                        else {
                            Ins *q = pre->first, *qp = 0;
                            while (q != t) { qp = q; q = q->next; }
                            i->next = t;
                            if (qp) qp->next = i; else pre->first = i;
                        }
                        inloop[i->dst] = 0;
                        moved = 1;
                        i = nx;
                    }
                }
            }
        }
    }
    free(inloop);
    free(counted);
    free(at);
}

/* ---- Value numbering over the dominator tree: common subexpressions,
 * copy and constant propagation, constant folding, simplification, and
 * store-to-load forwarding.
 *
 * The IR is not in SSA form (a C variable is one virtual register, written
 * many times), so a value is named by (register, version). Each definition
 * gets a new version (a clock); walking down the dominator tree, versions
 * set below a block are undone when the walk leaves it. A register written
 * once is known only below its definition (version 0: unknown). A register
 * written several times takes, at each block with several predecessors, a
 * fresh version (another path may have changed it); memory too, and every
 * store, call or other side effect gives memory a fresh version. A remembered
 * value is used only while its own version is unchanged. */
typedef struct {                /* no padding: keys are compared with memcmp */
    int op, cc, w, sz, sgn, bimm, tls, args;
    int a, va, b, vb, base, vbase, index, vindex, scale, slot, mem, spare;
    long imm, off;
    const char *sym;
} VKey;
typedef struct { VKey k; int d, vd, next; } VEnt;

static int *stamp, *ndefs, *cval_v, *copy_src, *copy_v, *copy_sv, *hhead, nhead;
static char *copy_w;
static Ins **defins;                    /* a register written once: by which instruction */
static int *defmem;                     /* ... and the memory version just after */
static int *defav;                      /* ... and the version of its operand a then */
/* the last block copy into a frame slot: loads from it can read the source instead */
static struct { int slot; long off, size; Addr src; int mem, vsb, vsi; } lastcp;
static long *cval;
static int clk, merge_ep, memver;
static VEnt *ents;
static int nents, capents;
static struct { int *at, old; } *undo;
static int nundo, capundo;

static void set_logged(int *at, int v)
{
    if (nundo == capundo) { capundo = capundo ? capundo * 2 : 256; undo = realloc(undo, sizeof *undo * capundo); if (!undo) die("out of memory"); }
    undo[nundo].at = at;
    undo[nundo++].old = *at;
    *at = v;
}

static int ver(int v)
{
    if (!v || F->vr[v].fixed) return 0;
    if (ndefs[v] == 1) return stamp[v];
    return stamp[v] > merge_ep ? stamp[v] : merge_ep;
}

static int fits32(long v) { return v == (long)(int)v; }
static int is_const(int v, long *c) { if (v && cval_v[v] && cval_v[v] == ver(v)) { *c = cval[v]; return 1; } return 0; }
static long canon(long v, int w) { return w == 4 ? (long)(unsigned)v : v; }

static unsigned vhash(VKey *k)
{
    unsigned h = 2166136261u;
    const unsigned char *p = (const unsigned char *)k;
    for (unsigned i = 0; i < sizeof *k; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static int key_of(Ins *i, VKey *k)                 /* 0: not a value to number */
{
    memset(k, 0, sizeof *k);
    if (!pure(i) || !i->dst || F->vr[i->dst].fixed || i->op == O_MOV || i->op == O_FRAMEADDR) return 0;
    k->op = i->op; k->cc = i->cc; k->w = i->w; k->sz = i->sz; k->sgn = i->sgn; k->bimm = i->bimm; k->imm = i->imm;
    int u[64], n = ins_uses(i, u);
    for (int x = 0; x < n; x++) if (!ver(u[x])) return 0;
    if (i->op == O_LOAD || i->op == O_LEA) {
        k->base = i->m.base; k->vbase = ver(i->m.base); k->index = i->m.index; k->vindex = ver(i->m.index);
        k->scale = i->m.scale; k->slot = i->m.slot; k->args = i->m.args; k->sym = i->m.sym; k->tls = i->m.tls; k->off = i->m.off;
        if (i->op == O_LOAD) k->mem = memver;
    } else {
        k->a = i->a; k->va = ver(i->a);
        if (!i->bimm) { k->b = i->b; k->vb = ver(i->b); }
    }
    return 1;
}

static int lookup(VKey *k)
{
    unsigned h = vhash(k) % nhead;
    for (int e = hhead[h]; e >= 0; e = ents[e].next)
        if (!memcmp(&ents[e].k, k, sizeof *k) && ver(ents[e].d) == ents[e].vd && ents[e].vd) return ents[e].d;
    return 0;
}

static void remember(VKey *k, int d)
{
    if (!ver(d)) return;
    if (nents == capents) { capents = capents ? capents * 2 : 256; ents = realloc(ents, sizeof *ents * capents); if (!ents) die("out of memory"); }
    unsigned h = vhash(k) % nhead;
    ents[nents] = (VEnt){ *k, d, ver(d), hhead[h] };
    hhead[h] = nents++;
}

/* does i read only the low 4 bytes of the register in *at? (then a 4-byte copy may stand for it) */
static int reads32(Ins *i, int *at)
{
    if (at == &i->m.base || at == &i->m.index) return 0;
    switch (i->op) {
    case O_SHL: case O_SHR: case O_SAR: if (at == &i->b) return 1; /* fall through */
    case O_ADD: case O_SUB: case O_MUL: case O_AND: case O_OR: case O_XOR: case O_NEG: case O_NOT: case O_SET: case O_BR:
    case O_MOV: case O_DIV: case O_UDIV: case O_MOD: case O_UMOD:
        return i->w == 4;
    case O_STORE: case O_EXT: case O_I2F: return i->sz <= 4;
    }
    return 0;
}

static int use_fields(Ins *i, int **p)              /* the operand fields that may be rewritten */
{
    int n = 0;
    switch (i->op) {
    case O_MOV: case O_NEG: case O_NOT: case O_EXT: case O_FNEG: case O_I2F: case O_F2I: case O_F2F: case O_BITS: case O_BITOP:
        if (!i->bimm) p[n++] = &i->a;
        break;
    case O_ADD: case O_SUB: case O_MUL: case O_AND: case O_OR: case O_XOR: case O_SHL: case O_SHR: case O_SAR:
    case O_DIV: case O_UDIV: case O_MOD: case O_UMOD: case O_SET: case O_BR: case O_FADD: case O_FSUB: case O_FMUL: case O_FDIV:
        p[n++] = &i->a;
        if (!i->bimm) p[n++] = &i->b;
        break;
    case O_LOAD: case O_LEA: p[n++] = &i->m.base; p[n++] = &i->m.index; break;
    case O_STORE: if (!i->bimm) p[n++] = &i->a; p[n++] = &i->m.base; p[n++] = &i->m.index; break;
    }
    return n;
}

static int is_commutative(int op) { return op == O_ADD || op == O_MUL || op == O_AND || op == O_OR || op == O_XOR; }
static int imm_ok(Ins *i, long c) { return i->w == 4 || fits32(c); }

/* both operands known: the result (0: cannot fold) */
static int fold(Ins *i, long x, long y, long *r)
{
    int w = i->w;
    if (w != 4 && w != 8) return 0;
    if (w == 4) { x = (int)x; y = (int)y; }
    unsigned long ux = w == 4 ? (unsigned)x : (unsigned long)x, uy = w == 4 ? (unsigned)y : (unsigned long)y;
    switch (i->op) {
    case O_ADD: *r = (long)(ux + uy); break;
    case O_SUB: *r = (long)(ux - uy); break;
    case O_MUL: *r = (long)(ux * uy); break;
    case O_AND: *r = x & y; break;
    case O_OR: *r = x | y; break;
    case O_XOR: *r = x ^ y; break;
    case O_SHL: *r = (long)(ux << (y & (w * 8 - 1))); break;
    case O_SHR: *r = (long)(ux >> (y & (w * 8 - 1))); break;
    case O_SAR: *r = w == 4 ? (int)x >> (y & 31) : x >> (y & 63); break;
    default: return 0;
    }
    *r = canon(*r, w);
    return 1;
}

static int cmp_true(int cc, long x, long y, int w)
{
    if (w == 4) { x = (int)x; y = (int)y; }
    unsigned long ux = w == 4 ? (unsigned)x : (unsigned long)x, uy = w == 4 ? (unsigned)y : (unsigned long)y;
    switch (cc) {
    case CC_EQ: return x == y; case CC_NE: return x != y; case CC_LT: return x < y; case CC_LE: return x <= y;
    case CC_GT: return x > y; case CC_GE: return x >= y; case CC_ULT: return ux < uy; case CC_ULE: return ux <= uy;
    case CC_UGT: return ux > uy; case CC_UGE: return ux >= uy;
    }
    return -1;
}

static void to_const(Ins *i, long c) { i->op = O_MOV; i->bimm = 1; i->imm = c; i->a = i->b = 0; }
static void to_copy(Ins *i, int a) { i->op = O_MOV; i->bimm = 0; i->a = a; i->b = 0; i->imm = 0; }

/* an address through a register that holds an address (or a sum): fold
 * that in, while the result keeps one base and one index (x86 addressing;
 * a global's address is an absolute 32-bit displacement: sicc's code is
 * never position-independent) */
static void fold_addr(Addr *m)
{
    for (int round = 0; round < 3; round++) {
        int b = m->base;
        if (!b || m->sym || m->slot || m->args || ndefs[b] != 1 || !defins[b] || !ver(b)) return;
        Ins *d = defins[b];
        Addr n = { 0 };
        n.scale = 1;
        if (d->op == O_LEA && !d->m.tls) n = d->m;
        else if (d->op == O_ADD && d->w == 8 && F->vr[d->dst].cls == C_INT) {
            n.base = d->a;
            if (d->bimm) n.off = d->imm; else n.index = d->b;
        } else return;
        /* its registers must still hold what they held there: written once */
        if ((n.base && (ndefs[n.base] != 1 || !ver(n.base))) || (n.index && (ndefs[n.index] != 1 || !ver(n.index)))) return;
        if (m->index && n.index) return;
        if ((n.slot || n.args) && (n.base || (n.index && m->index))) return;
        Addr r = n;
        r.off += m->off;
        if (m->index) { r.index = m->index; r.scale = m->scale; }
        if (!fits32(r.off)) return;
        /* a frame slot with an index: only with a short displacement (else the
         * address in a register, computed once, makes smaller code) */
        if ((r.slot || r.args) && r.index && (round > 0 || d->op == O_LEA)) {
            long off = r.off + (r.args ? 16 : 0);
            if (r.slot) return;                    /* (slot offsets are known only later: assume far) */
            if (off < -128 || off > 127) return;
        }
        *m = r;
    }
}

static void simplify(Ins *i)
{
    long x, y, r;
    int *f[4], n = use_fields(i, f);
    /* propagate copies and constants into the operands */
    for (int k = 0; k < n; k++) {
        int u = *f[k];
        if (!u || !ver(u) || copy_v[u] != ver(u)) continue;
        int s = copy_src[u];
        if (!ver(s) || ver(s) != copy_sv[u]) continue;
        if (F->vr[u].cls != F->vr[s].cls || F->vr[s].fixed) continue;
        if (F->vr[u].cls == C_INT && copy_w[u] != 8 && !reads32(i, f[k])) continue;   /* a 4-byte copy cleared the top */
        *f[k] = s;
    }
    if (i->op == O_LOAD || i->op == O_STORE || i->op == O_LEA) {
        fold_addr(&i->m);
        if (i->op == O_LOAD && lastcp.slot && lastcp.mem == memver && i->m.slot == lastcp.slot && !i->m.base && !i->m.index &&
            i->m.off >= lastcp.off && i->m.off + i->sz <= lastcp.off + lastcp.size &&
            (!lastcp.src.base || ver(lastcp.src.base) == lastcp.vsb) && (!lastcp.src.index || ver(lastcp.src.index) == lastcp.vsi)) {
            long k = i->m.off - lastcp.off;           /* (memory unchanged since the copy) */
            i->m = lastcp.src;
            i->m.off += k;
        }
        if (i->m.index && is_const(i->m.index, &x) && fits32(i->m.off + x * i->m.scale)) {   /* a constant index: a displacement */
            i->m.off += x * i->m.scale;
            i->m.index = 0;
            i->m.scale = 1;
        }
    }
    switch (i->op) {
    case O_MOV:
        if (!i->bimm && F->vr[i->dst].cls == C_INT && is_const(i->a, &x)) to_const(i, canon(x, i->w));
        return;
    case O_ADD: case O_SUB: case O_MUL: case O_AND: case O_OR: case O_XOR: case O_SHL: case O_SHR: case O_SAR:
        if (F->vr[i->dst].cls != C_INT) return;
        if ((i->op == O_OR || i->op == O_XOR || i->op == O_ADD) && !i->bimm && (i->w == 4 || i->w == 8)) {
            /* x << c | x >> (bits - c): a rotation */
            Ins *p = i->a && ndefs[i->a] == 1 && ver(i->a) ? defins[i->a] : 0, *q = i->b && ndefs[i->b] == 1 && ver(i->b) ? defins[i->b] : 0;
            if (p && q && p->op == O_SHR) { Ins *t = p; p = q; q = t; }
            if (p && q && p->op == O_SHL && q->op == O_SHR && p->bimm && q->bimm && p->w == i->w && q->w == i->w && p->a == q->a &&
                defav[p->dst] && defav[p->dst] == defav[q->dst] && p->imm > 0 && p->imm < i->w * 8 && p->imm + q->imm == i->w * 8) {
                i->op = O_ROL; i->a = p->a; i->b = 0; i->bimm = 1; i->imm = p->imm;
                return;
            }
        }
        if (!i->bimm && is_commutative(i->op) && is_const(i->a, &x) && !is_const(i->b, &y)) { int t = i->a; i->a = i->b; i->b = t; }
        if (!i->bimm && is_const(i->b, &y) && imm_ok(i, y)) { i->bimm = 1; i->imm = y; i->b = 0; }
        if (i->bimm && is_const(i->a, &x) && fold(i, x, i->imm, &r)) { to_const(i, r); return; }
        if (!i->bimm) return;
        y = i->w == 4 ? (int)i->imm : i->imm;
        if (y == 0 && i->op != O_MUL && i->op != O_AND) { to_copy(i, i->a); return; }
        if (y == 0) { to_const(i, 0); return; }
        if (i->op == O_MUL && y == 1) { to_copy(i, i->a); return; }
        if (i->op == O_AND && canon(y, i->w) == canon(-1, i->w)) { to_copy(i, i->a); return; }
        return;
    case O_ROL: return;
        if (i->op == O_MUL && y > 1 && !(y & (y - 1))) { i->op = O_SHL; i->imm = __builtin_ctzl(y); }
        return;
    case O_NEG: case O_NOT:
        if (F->vr[i->dst].cls == C_INT && (i->w == 4 || i->w == 8) && is_const(i->a, &x))
            to_const(i, canon(i->op == O_NEG ? (long)-(unsigned long)x : ~x, i->w));
        return;
    case O_EXT:
        if (ndefs[i->a] == 1 && defins[i->a] && ver(i->a) && defins[i->a]->op == O_LOAD && defmem[i->a] == memver) {
            Ins *d = defins[i->a];                 /* an extension of a load: a load that extends so */
            Addr *m = &d->m;
            if (d->sz == i->sz && !d->cc && (!m->base || (ndefs[m->base] == 1 && ver(m->base))) &&
                (!m->index || (ndefs[m->index] == 1 && ver(m->index)))) {
                i->op = O_LOAD; i->m = *m; i->a = 0; i->cc = 0;
                return;
            }
        }
        if (F->vr[i->dst].cls == C_INT && (i->w == 4 || i->w == 8) && is_const(i->a, &x)) {
            long v = i->sz == 1 ? (i->sgn ? (long)(signed char)x : (long)(unsigned char)x)
                   : i->sz == 2 ? (i->sgn ? (long)(short)x : (long)(unsigned short)x)
                   : i->sz == 4 ? (i->sgn ? (long)(int)x : (long)(unsigned)x) : x;
            to_const(i, canon(v, i->w));
        }
        return;
    case O_SET: case O_BR:
        if (F->vr[i->a].cls != C_INT || i->cc >= CC_FEQ) return;
        if (!i->bimm && is_const(i->b, &y) && imm_ok(i, y)) { i->bimm = 1; i->imm = y; i->b = 0; }
        if (i->op == O_SET && i->bimm && is_const(i->a, &x) && (i->w == 4 || i->w == 8)) {
            int t = cmp_true(i->cc, x, i->imm, i->w);
            if (t >= 0) { to_const(i, t); i->w = 4; }
        }
        return;
    case O_STORE:
        if (!i->bimm && F->vr[i->a].cls == C_INT && is_const(i->a, &x) && (i->sz < 8 || fits32(x))) { i->bimm = 1; i->imm = x; i->a = 0; }
        return;
    }
}

static int writes_memory(Ins *i)
{
    switch (i->op) {
    case O_LOAD: case O_LEA: case O_BR: case O_JMP: case O_RET: case O_SWITCH: case O_NOP: case O_UNREACH: case O_SPSAVE:
    case O_TLSADDR: case O_LDCMP: case O_I128CMP:
        return 0;
    }
    return !pure(i);
}

static void number_block(Block *b)
{
    VKey k;
    int d[64];
    for (Ins *i = b->first; i; i = i->next) {
        simplify(i);
        int have = key_of(i, &k);
        if (have) {
            int prev = lookup(&k);
            if (prev && F->vr[prev].cls == F->vr[i->dst].cls) { to_copy(i, prev); have = 0; }
        }
        int nd = ins_defs(i, d);
        if (writes_memory(i)) set_logged(&memver, ++clk);
        if (i->op == O_COPY) lastcp.slot = 0;
        if (i->op == O_COPY && i->imm <= 64 && ndefs[i->a] == 1 && defins[i->a] && defins[i->a]->op == O_LEA &&
            defins[i->a]->m.slot && !defins[i->a]->m.base && !defins[i->a]->m.index && ver(i->a)) {
            Addr src = { 0 };
            src.scale = 1;
            Ins *sd = ndefs[i->b] == 1 && ver(i->b) ? defins[i->b] : 0;
            if (sd && sd->op == O_LEA && !sd->m.tls && (!sd->m.base || ndefs[sd->m.base] == 1) && (!sd->m.index || ndefs[sd->m.index] == 1)) src = sd->m;
            else src.base = i->b;
            lastcp.slot = defins[i->a]->m.slot; lastcp.off = defins[i->a]->m.off; lastcp.size = i->imm;
            lastcp.src = src; lastcp.mem = memver; lastcp.vsb = ver(src.base); lastcp.vsi = ver(src.index);
            if ((src.base && !lastcp.vsb) || (src.index && !lastcp.vsi)) lastcp.slot = 0;
        }
        int av = ver(i->a);
        for (int x = 0; x < nd; x++) { set_logged(&stamp[d[x]], ++clk); if (ndefs[d[x]] == 1) { defins[d[x]] = i; defmem[d[x]] = memver; defav[d[x]] = av; } }
        if (i->op == O_MOV && i->dst && !F->vr[i->dst].fixed) {
            int v = ver(i->dst);
            if (i->bimm) { cval[i->dst] = i->imm; cval_v[i->dst] = v; }
            else if (ver(i->a) && i->a != i->dst && ndefs[i->dst] == 1 && (i->w == 8 || i->w == 4 || F->vr[i->dst].cls != C_INT)) {
                /* (only into temporaries: a variable's uses stay its own, so "t = x op y; v = t" can become "v = x op y") */
                copy_src[i->dst] = i->a; copy_v[i->dst] = v; copy_sv[i->dst] = ver(i->a); copy_w[i->dst] = i->w;
            }
        }
        if (have) remember(&k, i->dst);
        /* a store: a load of the same place, right after, reads the stored register */
        if (i->op == O_STORE && !i->bimm && ver(i->a) && !F->vr[i->a].fixed && (i->sz == 8 || i->sz == 4)) {
            Ins l = { 0 };
            l.op = O_LOAD; l.m = i->m; l.sz = i->sz; l.w = i->sz; l.dst = i->a;
            for (l.sgn = 0; l.sgn < 2; l.sgn++) if (key_of(&l, &k)) remember(&k, i->a);
        }
    }
}

static void value_numbering(int *ndef)
{
    int nb = 0, nv = F->nvr + 1;
    for (Block *b = F->blocks; b; b = b->next) b->id = nb++;
    Block **at = malloc(sizeof(Block *) * (nb + 1)), *s[3];
    /* edges, as lists: successors (in order) and predecessors */
    int *nsuc = calloc(nb + 1, sizeof(int)), *npred = calloc(nb + 1, sizeof(int)), *pstart = calloc(nb + 2, sizeof(int));
    int *seen = calloc(nb + 1, sizeof(int)), *rank = malloc(sizeof(int) * (nb + 1)), *order = malloc(sizeof(int) * (nb + 1));
    int *idom = malloc(sizeof(int) * (nb + 1)), *stk = malloc(sizeof(int) * (nb + 1)), *cur = calloc(nb + 1, sizeof(int));
    if (!at || !nsuc || !npred || !pstart || !seen || !rank || !order || !idom || !stk || !cur) die("out of memory");
    nb = 0;
    for (Block *b = F->blocks; b; b = b->next) at[nb++] = b;
    int nedge = 0;
    for (int x = 0; x < nb; x++) { int k = succ_of(at[x], s); nsuc[x] = k < 0 ? at[x]->last->ntab : k; nedge += nsuc[x]; }
    int *sstart = malloc(sizeof(int) * (nb + 1)), *succ = malloc(sizeof(int) * (nedge + 1)), *pred = malloc(sizeof(int) * (nedge + 1));
    if (!sstart || !succ || !pred) die("out of memory");
    for (int x = 0, e = 0; x < nb; x++) {
        sstart[x] = e;
        int k = succ_of(at[x], s);
        for (int z = 0; z < nsuc[x]; z++) { int y = (k < 0 ? at[x]->last->tab[z] : s[z])->id; succ[e++] = y; npred[y]++; }
    }
    for (int x = 0; x < nb; x++) pstart[x + 1] = pstart[x] + npred[x];
    for (int x = 0; x < nb; x++) cur[x] = pstart[x];
    for (int x = 0; x < nb; x++) for (int z = 0; z < nsuc[x]; z++) { int y = succ[sstart[x] + z]; pred[cur[y]++] = x; }
    /* postorder, by an iterative depth-first search; rank: reverse postorder */
    int sp = 0, n = 0;
    memset(cur, 0, sizeof(int) * nb);
    stk[sp++] = 0; seen[0] = 1;
    while (sp) {
        int x = stk[sp - 1];
        if (cur[x] < nsuc[x]) { int y = succ[sstart[x] + cur[x]++]; if (!seen[y]) { seen[y] = 1; stk[sp++] = y; } continue; }
        order[n++] = x;
        sp--;
    }
    for (int r = 0; r < n; r++) rank[order[n - 1 - r]] = r;
    /* immediate dominators (Cooper, Harvey and Kennedy) */
    for (int x = 0; x < nb; x++) idom[x] = -1;
    idom[0] = 0;
    for (int changed = 1; changed; ) {
        changed = 0;
        for (int r = 1; r < n; r++) {
            int x = order[n - 1 - r], nd = -1;
            for (int q = pstart[x]; q < pstart[x + 1]; q++) {
                int y = pred[q];
                if (!seen[y] || idom[y] < 0) continue;
                if (nd < 0) { nd = y; continue; }
                int a = y, c = nd;
                while (a != c) { while (rank[a] > rank[c]) a = idom[a]; while (rank[c] > rank[a]) c = idom[c]; }
                nd = a;
            }
            if (nd >= 0 && idom[x] != nd) { idom[x] = nd; changed = 1; }
        }
    }
    /* the dominator tree's children, in reverse postorder */
    int *cstart = calloc(nb + 2, sizeof(int)), *kids = malloc(sizeof(int) * (n + 1));
    if (!cstart || !kids) die("out of memory");
    for (int r = 1; r < n; r++) cstart[idom[order[n - 1 - r]] + 1]++;
    for (int x = 0; x < nb; x++) cstart[x + 1] += cstart[x];
    for (int x = 0; x < nb; x++) cur[x] = cstart[x];
    for (int r = 1; r < n; r++) { int x = order[n - 1 - r]; kids[cur[idom[x]]++] = x; }
    /* state */
    stamp = calloc(nv, sizeof(int)); ndefs = ndef; cval = calloc(nv, sizeof(long)); cval_v = calloc(nv, sizeof(int));
    copy_src = calloc(nv, sizeof(int)); copy_v = calloc(nv, sizeof(int)); copy_sv = calloc(nv, sizeof(int)); copy_w = calloc(nv, 1); defins = calloc(nv, sizeof(Ins *)); defmem = calloc(nv, sizeof(int)); defav = calloc(nv, sizeof(int));
    int ninsn = 0;
    for (int x = 0; x < nb; x++) for (Ins *i = at[x]->first; i; i = i->next) ninsn++;
    nhead = ninsn * 2 + 1;
    hhead = malloc(sizeof(int) * nhead);
    int *mark = malloc(sizeof(int) * (nb + 1));             /* the undo log's length when the block was entered */
    if (!stamp || !cval || !cval_v || !copy_src || !copy_v || !copy_sv || !copy_w || !defins || !defmem || !defav || !hhead || !mark) die("out of memory");
    for (int h = 0; h < nhead; h++) hhead[h] = -1;
    nents = nundo = clk = merge_ep = memver = 0;
    lastcp.slot = 0;                                        /* (checked by memory version: valid below the copy only) */
    /* walk: enter a block (number it), its children, leave it (undo) */
    sp = 0;
    stk[sp++] = 0;
    mark[0] = 0;
    set_logged(&merge_ep, ++clk);
    number_block(at[0]);
    for (int x = 0; x < nb; x++) cur[x] = cstart[x];
    while (sp) {
        int x = stk[sp - 1];
        if (cur[x] < cstart[x + 1]) {
            int y = kids[cur[x]++];
            mark[y] = nundo;
            if (npred[y] != 1) { set_logged(&merge_ep, ++clk); set_logged(&memver, ++clk); }
            number_block(at[y]);
            stk[sp++] = y;
            continue;
        }
        while (nundo > mark[x]) { nundo--; *undo[nundo].at = undo[nundo].old; }
        sp--;
    }
    free(mark); free(stamp); free(cval); free(cval_v); free(copy_src); free(copy_v); free(copy_sv); free(copy_w); free(defins); free(defmem); free(defav); free(hhead);
    free(at); free(nsuc); free(npred); free(pstart); free(seen); free(rank); free(order); free(idom); free(stk); free(cur);
    free(sstart); free(succ); free(pred); free(cstart); free(kids);
}

/* a frame slot whose address only ever serves as a block copy's destination
 * (its contents never read: the loads were forwarded): the copies go */
static void dead_slot_copies(void)
{
    int ns = F->nslots + 1, nv = F->nvr + 1;
    char *bad = calloc(ns, 1), *dstonly = calloc(nv, 1);
    int *slotof = calloc(nv, sizeof(int));
    if (!bad || !dstonly || !slotof) die("out of memory");
    int u[64];
    for (Block *b = F->blocks; b; b = b->next)
        for (Ins *i = b->first; i; i = i->next) {
            if (i->op == O_LEA && i->m.slot && !i->m.base && !i->m.index && i->dst) { if (slotof[i->dst]) bad[i->m.slot] = 1; slotof[i->dst] = i->m.slot; continue; }
            if (i->op == O_ASM) for (int s = 1; s < ns; s++) bad[s] = 1;          /* (slots used out of sight) */
            if (i->op == O_ENTRY) for (int k = 0; k < i->npin; k++) if (i->pin[k].slot > 0) bad[i->pin[k].slot] = 1;
            if (i->m.slot > 0 && i->m.slot < ns && i->op != O_LEA) bad[i->m.slot] = 1;
            if (i->op == O_LEA && i->m.slot) bad[i->m.slot] = 1;
        }
    for (Block *b = F->blocks; b; b = b->next)        /* every use of a slot's address: as a copy's destination? */
        for (Ins *i = b->first; i; i = i->next) {
            int n = ins_uses(i, u);
            for (int k = 0; k < n; k++) {
                int v = u[k];
                if (!slotof[v]) continue;
                if (!(i->op == O_COPY && i->a == v && i->b != v)) bad[slotof[v]] = 1;
            }
        }
    for (Block *b = F->blocks; b; b = b->next)
        for (Ins *i = b->first; i; i = i->next)
            if (i->op == O_COPY && slotof[i->a] && !bad[slotof[i->a]]) i->op = O_NOP;
    free(bad); free(dstonly); free(slotof);
}

void optimize(void)
{
    int nv = F->nvr + 1;
    int *nuse = calloc(nv, sizeof(int)), *ndef = calloc(nv, sizeof(int));
    int u[64], d[64];
    for (Block *b = F->blocks; b; b = b->next)
        for (Ins *i = b->first; i; i = i->next) {
            int n = ins_uses(i, u);
            for (int k = 0; k < n; k++) nuse[u[k]]++;
            n = ins_defs(i, d);
            for (int k = 0; k < n; k++) ndef[d[k]]++;
        }
    value_numbering(ndef);
    dead_slot_copies();
    memset(nuse, 0, sizeof(int) * nv);                 /* (numbering changed the uses) */
    for (Block *b = F->blocks; b; b = b->next)
        for (Ins *i = b->first; i; i = i->next) { int n = ins_uses(i, u); for (int k = 0; k < n; k++) nuse[u[k]]++; }
    /* copies into variables */
    for (Block *b = F->blocks; b; b = b->next)
        for (Ins *i = b->first; i && i->next; i = i->next) {
            Ins *m = i->next;
            if (m->op != O_MOV || m->bimm || !pure(i) || !i->dst || i->op == O_FRAMEADDR) continue;
            int t = i->dst, v = m->dst;
            if (m->a != t || nuse[t] != 1 || ndef[t] != 1 || F->vr[t].cls != F->vr[v].cls) continue;
            if (m->w > i->w && i->op != O_LOAD && i->op != O_EXT && i->op != O_LEA && i->w != 8) continue;
            i->dst = v;
            m->op = O_NOP;
            nuse[t] = 0;
        }
    hoist_loops(ndef);
    /* dead instructions, until none is left */
    for (int changed = 1; changed; ) {
        changed = 0;
        for (Block *b = F->blocks; b; b = b->next)
            for (Ins *i = b->first; i; i = i->next) {
                if (i->op == O_NOP || !pure(i) || !i->dst || nuse[i->dst] || F->vr[i->dst].fixed) continue;
                int n = ins_uses(i, u);
                for (int k = 0; k < n; k++) nuse[u[k]]--;
                i->op = O_NOP;
                changed = 1;
            }
    }
    /* unlink the NOPs */
    for (Block *b = F->blocks; b; b = b->next) {
        Ins **pp = &b->first, *last = 0;
        for (Ins *i = b->first; i; i = i->next) {
            if (i->op == O_NOP) continue;
            *pp = i;
            pp = &i->next;
            last = i;
        }
        *pp = 0;
        b->last = last;
    }
    free(nuse);
    free(ndef);
}
