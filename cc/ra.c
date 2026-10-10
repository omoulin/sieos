/*
 * ra.c - Register allocation: from virtual registers to x86-64 registers.
 *
 * 1. Number the instructions, in layout order.
 * 2. Liveness: which virtual registers are live at each block's start and
 *    end (the classic backward data-flow, iterated to a fixed point).
 * 3. Live intervals: for each virtual register, the first and last
 *    position where it is live (one interval each: simple, a little
 *    conservative).
 * 4. Linear scan (Poletto and Sarkar): walk the intervals by start; give
 *    each a free register, or spill the interval that ends last to a stack
 *    slot. An interval that crosses a call (or an instruction that uses
 *    fixed registers, like inline asm) only gets registers that survive it:
 *    the callee-saved ones.
 *
 * x86-64: rax, rcx, rdx and r11 (and xmm0-xmm7) are never allocated: the
 * code generator uses them as scratch registers, for division, shifts,
 * calls. AArch64: x14-x17 and v30-v31 are its scratch registers; x18
 * (platform), x29 (frame) and x30 (link) are never allocated; v0-v7 (the
 * argument registers) neither, for simpler calls.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ir.h"

static const int x86_int[] = { RSI, RDI, R8, R9, R10, RBX, R12, R13, R14, R15, -1 };
static const int x86_flt[] = { 24, 25, 26, 27, 28, 29, 30, 31, -1 };      /* xmm8-xmm15 */
/* AArch64: caller-saved first (short values, no saving), then callee-saved */
static const int a64_int[] = { 9, 10, 11, 12, 13, 8, 7, 6, 5, 4, 3, 2, 1, 0, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, -1 };
static const int a64_flt[] = { 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 40, 41, 42, 43, 44, 45, 46, 47, -1 };

static int allocatable(int p)                    /* may a value be placed in p (a hint)? */
{
    const int *t = target == T_AARCH64 ? (p >= A64_V0 ? a64_flt : a64_int) : (p >= XMM0 ? x86_flt : x86_int);
    for (; *t >= 0; t++) if (*t == p) return 1;
    return 0;
}

/* The virtual registers an instruction reads (uses) and writes (defs). */
int ins_uses(Ins *i, int *u)
{
    int n = 0;
#define U(x) do { if (x) u[n++] = (x); } while (0)
    switch (i->op) {
    case O_MOV: case O_NEG: case O_NOT: case O_EXT: case O_FNEG: case O_FSQRT: case O_I2F: case O_F2I: case O_F2F: case O_BITS:
    case O_BITOP: case O_SWITCH: case O_ZERO: case O_ALLOCA: case O_SPRESTORE:
        if (!i->bimm) U(i->a);
        break;
    case O_LOAD: case O_LEA: U(i->m.base); U(i->m.index); break;
    case O_STORE: if (!i->bimm) U(i->a); U(i->m.base); U(i->m.index); break;
    case O_CALL:
        U(i->call->fn);
        for (int k = 0; k < i->call->nargs; k++) { U(i->call->args[k].v); U(i->call->args[k].addr); }
        break;
    case O_RET: if (!i->bimm) U(i->a); U(i->b); for (int k = 0; k < i->npin; k++) U(i->pin[k].v); break;
    case O_ASM:
        for (int k = 0; k < i->asmi->n; k++) if (i->asmi->op[k].in || i->asmi->op[k].mreg > 0) U(i->asmi->op[k].v);
        break;
    case O_CMPXCHG: U(i->a); U(i->b); U(i->m.base); break;
    case O_ENTRY: case O_JMP: case O_FENCE: case O_UNREACH: case O_FRAMEADDR: case O_NOP: case O_SPSAVE: case O_TLSADDR: break;
    case O_MULSHR: U(i->a); U(i->b); break;
    case O_LDBIN: case O_LDNEG: case O_LDCMP: case O_LDCVT: case O_I128: case O_I128CMP: case O_I128EXT: case O_VEC:
        U(i->a); U(i->b); U(i->m.base); break;
    case O_I128SH: U(i->a); if (!i->bimm) U(i->b); U(i->m.base); break;
    default: U(i->a); if (!i->bimm) U(i->b); break;
    }
#undef U
    return n;
}

int ins_defs(Ins *i, int *d)
{
    int n = 0;
    switch (i->op) {
    case O_STORE: case O_BR: case O_JMP: case O_RET: case O_SWITCH: case O_COPY: case O_ZERO:
    case O_FENCE: case O_UNREACH: case O_NOP: case O_SPRESTORE:
        break;
    case O_CALL: for (int k = 0; k < i->call->nret; k++) d[n++] = i->call->ret[k]; break;
    case O_ASM: for (int k = 0; k < i->asmi->n; k++) if (i->asmi->op[k].outv) d[n++] = i->asmi->op[k].outv; break;
    case O_ENTRY: for (int k = 0; k < i->npin; k++) if (i->pin[k].v) d[n++] = i->pin[k].v; break;
    default: if (i->dst) d[n++] = i->dst;
    }
    return n;
}

static int succs(Block *b, Block **s, int max)
{
    Ins *t = b->last;
    int n = 0;
    if (!t) { if (b->next) s[n++] = b->next; return n; }
    switch (t->op) {
    case O_JMP: s[n++] = t->t; break;
    case O_BR: s[n++] = t->t; s[n++] = t->f; break;
    case O_RET: case O_UNREACH: break;
    case O_SWITCH: for (int k = 0; k < t->ntab && n < max; k++) s[n++] = t->tab[k]; break;
    default: if (b->next) s[n++] = b->next;
    }
    return n;
}

#define BSET(s, v) ((s)[(v) >> 5] |= 1u << ((v) & 31))
#define BHAS(s, v) ((s)[(v) >> 5] >> ((v) & 31) & 1)

typedef struct { int v, start, end; RegSet cross; } Interval;

static int by_start(const void *a, const void *b)
{
    const Interval *x = a, *y = b;
    return x->start != y->start ? x->start - y->start : x->v - y->v;
}

void regalloc(void)
{
    int nv = F->nvr + 1, words = (nv + 31) / 32;
    /* 1. positions */
    int pos = 2, nblocks = 0;
    for (Block *b = F->blocks; b; b = b->next) {
        b->start = pos;
        for (Ins *i = b->first; i; i = i->next) { i->pos = pos; pos += 2; }
        b->end = pos - 2 < b->start ? b->start : pos - 2;
        nblocks++;
    }
    /* 2. liveness */
    Block **order = malloc(sizeof(Block *) * (nblocks + 1));
    int k = 0;
    for (Block *b = F->blocks; b; b = b->next) order[k++] = b;
    unsigned *mem = calloc((size_t)nblocks * words * 4 + 1, sizeof(unsigned));
    if (!order || !mem) die("out of memory");
    for (int j = 0; j < nblocks; j++) {
        Block *b = order[j];
        b->in = mem + (size_t)j * words * 4;
        b->out = b->in + words;
        unsigned *use = b->in + 2 * words, *def = b->in + 3 * words;
        int u[64], d[64];
        for (Ins *i = b->first; i; i = i->next) {
            int nu = ins_uses(i, u);
            for (int x = 0; x < nu; x++) if (!BHAS(def, u[x])) BSET(use, u[x]);
            int nd = ins_defs(i, d);
            for (int x = 0; x < nd; x++) BSET(def, d[x]);
        }
    }
    Block **sb = malloc(sizeof(Block *) * 4100);
    for (int changed = 1; changed; ) {
        changed = 0;
        for (int j = nblocks - 1; j >= 0; j--) {
            Block *b = order[j];
            unsigned *use = b->in + 2 * words, *def = b->in + 3 * words;
            int ns = succs(b, sb, 4096);
            for (int w = 0; w < words; w++) {
                unsigned o = 0;
                for (int s = 0; s < ns; s++) o |= sb[s]->in[w];
                unsigned in = use[w] | (o & ~def[w]);
                if (o != b->out[w] || in != b->in[w]) { b->out[w] = o; b->in[w] = in; changed = 1; }
            }
        }
    }
    free(sb);
    /* 3. intervals */
    Interval *iv = malloc(sizeof(Interval) * nv);
    for (int v = 0; v < nv; v++) { iv[v].v = v; iv[v].start = 1 << 30; iv[v].end = -1; iv[v].cross = 0; }
#define EXT(v, p) do { if ((p) < iv[v].start) iv[v].start = (p); if ((p) > iv[v].end) iv[v].end = (p); } while (0)
    for (int j = 0; j < nblocks; j++) {
        Block *b = order[j];
        for (int w = 0; w < words; w++) {
            unsigned x = b->in[w];
            while (x) { int bit = __builtin_ctz(x); x &= x - 1; int v = w * 32 + bit; EXT(v, b->start); }
            x = b->out[w];
            while (x) { int bit = __builtin_ctz(x); x &= x - 1; int v = w * 32 + bit; EXT(v, b->end + 1); }
        }
        int u[64], d[64];
        for (Ins *i = b->first; i; i = i->next) {
            int nu = ins_uses(i, u);
            for (int x = 0; x < nu; x++) EXT(u[x], i->pos);
            int nd = ins_defs(i, d);
            for (int x = 0; x < nd; x++) EXT(d[x], i->pos);
        }
    }
    /* what each interval crosses: instructions that destroy registers */
    Vec clobs = { 0 };
    for (int j = 0; j < nblocks; j++)
        for (Ins *i = order[j]->first; i; i = i->next) {
            if (i->clob) vec_push(&clobs, i);
            if (i->op == O_ASM) F->used_regs |= i->clob;
            if (i->op == O_CALL) F->has_calls = 1;
        }
    for (int v = 1; v < nv; v++) {
        if (iv[v].end < 0) continue;
        int lo = 0, hi = clobs.n;              /* first clobbering instruction after start */
        while (lo < hi) { int m = (lo + hi) / 2; if (((Ins *)clobs.v[m])->pos <= iv[v].start) lo = m + 1; else hi = m; }
        for (int c = lo; c < clobs.n && ((Ins *)clobs.v[c])->pos < iv[v].end; c++) iv[v].cross |= ((Ins *)clobs.v[c])->clob;
    }
    free(clobs.v);
    /* spill costs: each use or definition, weighted by its loop depth (loops:
     * back edges in the layout); and, for coalescing, the instruction that
     * starts each interval */
    int npos = pos + 2;
    int *depth = calloc(npos + 1, sizeof(int));
    Ins **first = calloc((size_t)nv + 1, sizeof(Ins *));
    float *cost = calloc((size_t)nv + 1, sizeof(float));
    if (!depth || !first || !cost) die("out of memory");
    for (int j = 0; j < nblocks; j++) {
        Block *b = order[j], *s[64];
        int ns = succs(b, s, 64);
        for (int x = 0; x < ns; x++) if (s[x]->start <= b->start) { depth[s[x]->start]++; depth[b->end + 1]--; }
    }
    for (int p = 1; p <= npos; p++) depth[p] += depth[p - 1];
    for (int j = 0; j < nblocks; j++)
        for (Ins *i = order[j]->first; i; i = i->next) {
            int u[64], d[64], dp = depth[i->pos] > 6 ? 6 : depth[i->pos];
            float f = 1;
            for (int x = 0; x < dp; x++) f *= 8;
            int nu = ins_uses(i, u), nd = ins_defs(i, d);
            for (int x = 0; x < nu; x++) cost[u[x]] += f;
            for (int x = 0; x < nd; x++) { cost[d[x]] += f; if (iv[d[x]].start == i->pos && !first[d[x]]) first[d[x]] = i; }
        }
    /* per position held: spilling a long, rarely used interval frees a register for long, at little cost */
    for (int v = 1; v < nv; v++) if (iv[v].end >= 0) cost[v] /= iv[v].end - iv[v].start + 2;
    /* 4. linear scan */
    Interval *list = malloc(sizeof(Interval) * nv);
    int n = 0;
    for (int v = 1; v < nv; v++) if (iv[v].end >= 0) list[n++] = iv[v];
    qsort(list, n, sizeof *list, by_start);
    int *active = malloc(sizeof(int) * (n + 1));     /* indexes into list, sorted by end */
    int nact = 0;
    int owner[64];
    for (int r = 0; r < 64; r++) owner[r] = -1;
    for (int x = 0; x < n; x++) {
        Interval *it = &list[x];
        /* expire */
        int keep = 0;
        for (int a = 0; a < nact; a++) {
            Interval *o = &list[active[a]];
            if (o->end < it->start) { owner[F->vr[o->v].phys] = -1; continue; }
            active[keep++] = active[a];
        }
        nact = keep;
        VReg *vr = &F->vr[it->v];
        int r = -1;
        /* coalescing: defined from a register that dies right here: take its place (no move) */
        Ins *di = first[it->v];
        int src = 0;
        if (di && !vr->fixed && vr->hint < 0) switch (di->op) {
        case O_EXT: if (di->sz == 4 && !di->sgn) break;      /* (movl %r, %r: never eliminated by the CPU) */
            /* fall through */
        case O_MOV: case O_NEG: case O_NOT: case O_ADD: case O_SUB: case O_AND: case O_OR: case O_XOR:
        case O_SHL: case O_SHR: case O_SAR: case O_FADD: case O_FSUB: case O_FMUL: case O_FDIV:
            if (!di->bimm || di->op != O_MOV) src = di->a;
            break;
        case O_LEA: if (!di->m.index) src = di->m.base; break;
        }
        if (src && src != it->v && iv[src].end == di->pos && F->vr[src].cls == vr->cls && F->vr[src].phys >= 0 && !F->vr[src].fixed) {
            int p = F->vr[src].phys;
            for (int a = 0; a < nact; a++)
                if (list[active[a]].v == src && !(it->cross & BIT(p))) { active[a] = active[--nact]; owner[p] = -1; r = p; break; }
        }
        if (r >= 0) ;
        else if (vr->hint >= 0 && owner[vr->hint] < 0 && !(it->cross & BIT(vr->hint)) && allocatable(vr->hint))
            r = vr->hint;                    /* where its value arrives or goes: no move needed */
        else {
            const int *t = target == T_AARCH64 ? (vr->cls == C_INT ? a64_int : a64_flt) : (vr->cls == C_INT ? x86_int : x86_flt);
            for (; *t >= 0; t++)
                if (owner[*t] < 0 && !(it->cross & BIT(*t))) { r = *t; break; }
        }
        if (r < 0) {                           /* spill the cheapest: fewest (loop-weighted) uses per position, then the longest */
            int best = -1;
            for (int a = 0; a < nact; a++) {
                Interval *o = &list[active[a]];
                int p = F->vr[o->v].phys;
                if (F->vr[o->v].cls != vr->cls || (it->cross & BIT(p)) || F->vr[o->v].fixed) continue;
                if (best < 0 || cost[o->v] < cost[list[active[best]].v] ||
                    (cost[o->v] == cost[list[active[best]].v] && o->end > list[active[best]].end)) best = a;
            }
            if (best >= 0 && (cost[list[active[best]].v] < cost[it->v] ||
                              (cost[list[active[best]].v] == cost[it->v] && list[active[best]].end > it->end))) {
                Interval *o = &list[active[best]];
                r = F->vr[o->v].phys;
                F->vr[o->v].phys = -1;
                active[best] = active[--nact];
            }
        }
        if (r < 0) { vr->phys = -1; continue; }
        vr->phys = r;
        owner[r] = x;
        F->used_regs |= BIT(r);
        active[nact++] = x;
    }
    for (int v = 1; v < nv; v++) {
        VReg *vr = &F->vr[v];
        if (vr->phys < 0 && iv[v].end >= 0) vr->slot = new_slot(8, 8);
        vr->start = iv[v].start;
        vr->end = iv[v].end;
    }
    free(depth);
    free(first);
    free(cost);
    free(list);
    free(active);
    free(iv);
    free(mem);
    free(order);
}
