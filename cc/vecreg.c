/*
 * vecreg.c - AArch64: NEON vectors from frame slots into vector registers.
 *
 * Lowering keeps a vector like a struct: in a frame slot, operations taking
 * its address (O_VEC: *m.base = *a op *b), copies (O_COPY) and zeroing
 * (O_ZERO) moving it around; the intrinsics of arm_neon.h, inlined, add a
 * slot and a copy for each argument and result. So a loop of three NEON
 * instructions became some two hundred loads, stores and address moves.
 *
 * After the optimizer, this pass finds the slots of 8 or 16 bytes whose
 * address is only ever used whole: as an O_VEC operand or result, as one
 * side of a copy of the slot's size, or zeroed. Each such slot becomes one
 * virtual register of class C_VEC, and its instructions the register forms:
 *
 *   O_VEC  *d = *a op *b      ->  O_VREG  vd = va op vb   (an operand still
 *                                 in memory: O_VLD into a new register first)
 *   O_COPY *d = *s            ->  O_VMOV vd = vs, O_VLD vd = [s], O_VST [d] = vs
 *   O_ZERO *d                 ->  O_VZERO vd
 *
 * and the address computations (O_LEA of the slot, copies of that address)
 * go, and so does the slot's room in the frame. Then copies are propagated
 * (an inlined intrinsic copies its arguments: its body reads the originals
 * instead, along straight-line code), and the vector definitions nothing
 * reads are removed (those copies; an intrinsic's local, zeroed and
 * overwritten at once). Anything else that touches such a slot (an element read through its
 * address, the address passed to a call, a partial copy...) leaves it in
 * memory, as before. An O_VEC whose result stays in memory keeps its
 * operands there too (it reads them by address). The register allocator
 * then places the vectors in v8-v29 (ra.c), and the code generator emits
 * the same NEON instructions on those registers (emit_a64.c).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ir.h"

/* Does a NEON operation also read its result's old value (the accumulator)? */
int vop_acc(int op)
{
    switch (op) {
    case VOP_FMA: case VOP_FMS: case VOP_MLA: case VOP_MLS: case VOP_DOT: case VOP_DOTL:
    case VOP_MLAL: case VOP_MLAL2: case VOP_PADAL: case VOP_BSL:
        return 1;
    }
    return 0;
}

static int dbg;            /* SICC_VECDBG set: say why a slot stays in memory (stderr) */
#define BAD(s, why) do { if (!bad[s] && dbg && candidate(s)) fprintf(stderr, "vecreg: %s: slot %d (%d bytes): %s (op %d)\n", F->obj->name, (s), F->slots[s].size, (why), dbg_op); bad[s] = 1; } while (0)
static int dbg_op;
static int *aslot;          /* virtual register -> the slot whose address it holds (0: unknown, -1: none / several) */
static char *bad;           /* slot -> stays in memory */
static int *vreg;           /* slot -> its C_VEC register */

static int candidate(int s) { return s > 0 && s <= F->nslots && (F->slots[s].size == 8 || F->slots[s].size == 16); }
static void merge(int d, int s)                    /* d holds slot s's address (s -1: something else) */
{
    if (aslot[d] == s) return;
    if (aslot[d] > 0) BAD(aslot[d], "its address register gets another value");   /* (one register, two slots' addresses: neither) */
    if (s > 0 && aslot[d] != 0) BAD(s, "its address register holds another value too");
    aslot[d] = aslot[d] == 0 ? s : -1;
}
static int uses_m(int op)                           /* (the others may keep a stale one: an O_LEA made an O_MOV) */
{
    switch (op) {
    case O_LOAD: case O_STORE: case O_LEA: case O_VEC: case O_CMPXCHG: case O_LDBIN: case O_LDNEG: case O_LDCMP:
    case O_LDCVT: case O_I128: case O_I128SH: case O_I128CMP: case O_I128EXT: case O_TLSADDR:
        return 1;
    }
    return 0;
}
static int whole(const Addr *m) { return !m->index && !m->off && !m->slot && !m->sym && !m->args && !m->tls; }
static int promoted(int v) { return v > 0 && aslot[v] > 0 && !bad[aslot[v]]; }

static Ins *new_ins(int op)
{
    Ins *i = calloc(1, sizeof *i);
    if (!i) die("out of memory");
    i->op = op;
    return i;
}
static void before(Block *b, Ins *prev, Ins *at, Ins *x)   /* x goes in front of at (prev: at's predecessor) */
{
    x->next = at;
    if (prev) prev->next = x; else b->first = x;
}

/* ---- Dead vector definitions: a backward liveness over the C_VEC registers
 * only, then each block backwards. */
static int vsuccs(Block *b, Block **s)
{
    Ins *t = b->last;
    int n = 0;
    if (!t) { if (b->next) s[n++] = b->next; return n; }
    switch (t->op) {
    case O_JMP: s[n++] = t->t; break;
    case O_BR: s[n++] = t->t; s[n++] = t->f; break;
    case O_RET: case O_UNREACH: break;
    case O_SWITCH: return -1;                            /* (many: done conservatively by the caller) */
    default: if (b->next) s[n++] = b->next;
    }
    return n;
}
static int pure_vdef(Ins *i)                             /* a C_VEC definition with no other effect */
{
    return (i->op == O_VZERO || i->op == O_VMOV || i->op == O_VLD || (i->op == O_VREG && i->cc != VOP_ADDV && i->cc != VOP_MOVEMASK))
           && i->dst && F->vr[i->dst].cls == C_VEC;
}
static void vec_dce(int first)                           /* first: the first C_VEC register */
{
    int nv = F->nvr + 1 - first;
    if (nv <= 0) return;
    int words = (nv + 31) / 32, nb = 0;
    for (Block *b = F->blocks; b; b = b->next) b->id = nb++;
    unsigned *in = calloc((size_t)nb * words, 4), *outs = calloc((size_t)nb * words, 4), *live = calloc(words, 4);
    Block **order = calloc(nb + 1, sizeof *order);
    if (!in || !outs || !live || !order) die("out of memory");
    int k = 0, sw = 0;
    for (Block *b = F->blocks; b; b = b->next) { order[k++] = b; if (b->last && b->last->op == O_SWITCH) sw = 1; }
#define VB(v) ((v) >= first ? (v) - first : -1)
    int u[64], d[64];
    for (int pass = 0, changed = 1; changed && pass < 8; pass++) {   /* (liveness changes as definitions go) */
        changed = 0;
        for (int again = 1; again; ) {                   /* live in/out to a fixed point */
            again = 0;
            for (int j = nb - 1; j >= 0; j--) {
                Block *b = order[j], *s[2];
                int ns = vsuccs(b, s);
                unsigned *o = outs + (size_t)j * words;
                for (int w = 0; w < words; w++) live[w] = 0;
                if (ns < 0 || sw) for (int w = 0; w < words; w++) live[w] = ~0u;   /* (a switch: all live) */
                else for (int x = 0; x < ns; x++) for (int w = 0; w < words; w++) live[w] |= in[(size_t)s[x]->id * words + w];
                memcpy(o, live, words * 4);
                Ins *list[4096];
                int n = 0;
                for (Ins *i = b->first; i && n < 4096; i = i->next) list[n++] = i;
                for (int x = n - 1; x >= 0; x--) {
                    Ins *i = list[x];
                    int nd = ins_defs(i, d), nu = ins_uses(i, u);
                    for (int y = 0; y < nd; y++) { int v = VB(d[y]); if (v >= 0) live[v >> 5] &= ~(1u << (v & 31)); }
                    for (int y = 0; y < nu; y++) { int v = VB(u[y]); if (v >= 0) live[v >> 5] |= 1u << (v & 31); }
                }
                unsigned *ib = in + (size_t)j * words;
                if (memcmp(ib, live, words * 4)) { memcpy(ib, live, words * 4); again = 1; }
            }
        }
        for (int j = 0; j < nb; j++) {                   /* remove what is not read afterwards */
            Block *b = order[j];
            memcpy(live, outs + (size_t)j * words, words * 4);
            Ins *list[4096];
            int n = 0;
            for (Ins *i = b->first; i && n < 4096; i = i->next) list[n++] = i;
            for (int x = n - 1; x >= 0; x--) {
                Ins *i = list[x];
                if (pure_vdef(i)) {
                    int v = VB(i->dst);
                    if (v >= 0 && !(live[v >> 5] >> (v & 31) & 1)) { i->op = O_NOP; i->dst = i->a = i->b = 0; changed = 1; continue; }
                }
                int nd = ins_defs(i, d), nu = ins_uses(i, u);
                for (int y = 0; y < nd; y++) { int v = VB(d[y]); if (v >= 0) live[v >> 5] &= ~(1u << (v & 31)); }
                for (int y = 0; y < nu; y++) { int v = VB(u[y]); if (v >= 0) live[v >> 5] |= 1u << (v & 31); }
            }
        }
    }
#undef VB
    free(in); free(outs); free(live); free(order);
}

/* ---- Copies: after vd = vs, read vs where vd is read, until either is
 * written again; through a block into the next one when that one has no
 * other predecessor. */
static void vec_copyprop(int first)
{
    int nv = F->nvr + 1, nb = 0;
    for (Block *b = F->blocks; b; b = b->next) b->id = nb++;
    int *npred = calloc(nb + 1, sizeof *npred), *cp = calloc(nv, sizeof *cp);
    if (!npred || !cp) die("out of memory");
    for (Block *b = F->blocks; b; b = b->next) {
        Block *s[2];
        int ns = vsuccs(b, s);
        if (ns < 0) { for (int k = 0; k < b->last->ntab; k++) npred[b->last->tab[k]->id] += 2; continue; }
        for (int k = 0; k < ns; k++) npred[s[k]->id]++;
    }
    for (Block *x = F->blocks; x; x = x->next) if (x->addr_taken) npred[x->id] += 2;   /* (a computed goto may come) */
    Block *prev = 0;
    for (Block *b = F->blocks; b; prev = b, b = b->next) {
        int cont = prev && npred[b->id] == 1 && prev->last && (prev->last->op == O_JMP ? prev->last->t == b : prev->last->op != O_BR &&
                   prev->last->op != O_RET && prev->last->op != O_SWITCH && prev->last->op != O_UNREACH);
        if (!cont) for (int v = first; v < nv; v++) cp[v] = 0;
        for (Ins *i = b->first; i; i = i->next) {
            /* reads */
            if ((i->op == O_VREG || i->op == O_VMOV || i->op == O_VST) && i->a >= first && cp[i->a] && !(i->op == O_VREG && i->cc == VOP_SPLAT)) i->a = cp[i->a];
            if (i->op == O_VREG && i->b >= first && cp[i->b]) i->b = cp[i->b];
            /* writes: what they end */
            int d[64], nd = ins_defs(i, d);
            for (int k = 0; k < nd; k++) {
                if (d[k] < first) continue;
                cp[d[k]] = 0;
                for (int v = first; v < nv; v++) if (cp[v] == d[k]) cp[v] = 0;
            }
            if (i->op == O_VMOV && i->dst >= first && i->a >= first && i->a != i->dst) cp[i->dst] = i->a;
            /* an 8-byte vector's low half is the register's low 8 bytes: what reads it reads those */
            if (i->op == O_VREG && i->cc == VOP_GETLO && i->dst >= first && i->a >= first && i->a != i->dst &&
                F->vr[i->dst].w == 8) cp[i->dst] = i->a;
        }
    }
    free(npred); free(cp);
}

/* ---- Accumulators: an inlined intrinsic gives r = acc; r op= x; acc = r.
 * When r is that temporary only (three references) and acc is neither read
 * nor written in between, it becomes acc op= x. Along the same straight-line
 * chains as the copies. */
static void vec_accfuse(int first)
{
    int nv = F->nvr + 1, nb = 0;
    int *refs = calloc(nv, sizeof *refs);
    if (!refs) die("out of memory");
    for (Block *b = F->blocks; b; b = b->next, nb++)
        for (Ins *i = b->first; i; i = i->next) {
            int u[64], d[64], nu = ins_uses(i, u), nd = ins_defs(i, d);
            for (int k = 0; k < nu; k++) refs[u[k]]++;
            for (int k = 0; k < nd; k++) refs[d[k]]++;
        }
    int *npred = calloc(nb + 1, sizeof *npred);
    Ins **chain = calloc(8192, sizeof *chain);
    if (!npred || !chain) die("out of memory");
    nb = 0;
    for (Block *b = F->blocks; b; b = b->next) b->id = nb++;
    for (Block *b = F->blocks; b; b = b->next) {
        Block *s[2];
        int ns = vsuccs(b, s);
        if (ns < 0) { for (int k = 0; k < b->last->ntab; k++) npred[b->last->tab[k]->id] += 2; continue; }
        for (int k = 0; k < ns; k++) npred[s[k]->id]++;
    }
    for (Block *x = F->blocks; x; x = x->next) if (x->addr_taken) npred[x->id] += 2;
    int n = 0;
    Block *prev = 0;
    for (Block *b = F->blocks; b; prev = b, b = b->next) {
        int cont = prev && npred[b->id] == 1 && prev->last && (prev->last->op == O_JMP ? prev->last->t == b : prev->last->op != O_BR &&
                   prev->last->op != O_RET && prev->last->op != O_SWITCH && prev->last->op != O_UNREACH);
        if (!cont) n = 0;
        for (Ins *i3 = b->first; i3; i3 = i3->next) {
            if (n < 8192) chain[n++] = i3;
            int r = i3->a, acc = i3->dst;
            if (i3->op != O_VMOV || r < first || acc < first || r == acc || refs[r] != 3) continue;
            int k2 = n - 2;                              /* back: the op that writes r, then r = acc */
            while (k2 >= 0 && chain[k2]->op == O_NOP) k2--;
            if (k2 < 1) continue;
            Ins *i2 = chain[k2];
            if (i2->op != O_VREG || i2->dst != r || !vop_acc(i2->cc) || i2->a == r || i2->b == r) continue;
            int k1 = k2 - 1;
            while (k1 >= 0 && chain[k1]->op == O_NOP) k1--;
            if (k1 < 0) continue;
            Ins *i1 = chain[k1];
            if (i1->op != O_VMOV || i1->dst != r || i1->a != acc) continue;
            i2->dst = acc;                               /* acc op= x, in place */
            i1->op = i3->op = O_NOP;
            i1->dst = i1->a = i3->dst = i3->a = 0;
        }
    }
    free(refs); free(npred); free(chain);
}

void vec_promote(void)
{
    int nv = F->nvr + 1, ns = F->nslots + 1;
    dbg = getenv("SICC_VECDBG") != 0;
    aslot = calloc(nv, sizeof *aslot);
    bad = calloc(ns, 1);
    vreg = calloc(ns, sizeof *vreg);
    if (!aslot || !bad || !vreg) die("out of memory");
    /* slots the calling convention or the frame use directly: never */
    if (F->va_slot > 0 && F->va_slot < ns) bad[F->va_slot] = 1;
    if (F->ret_slot > 0 && F->ret_slot < ns) bad[F->ret_slot] = 1;
    /* 1. which registers hold a slot's address: defined only by O_LEA of the
     *    slot (whole), or copied from such a register */
    int d[64], u[64];
    for (Block *b = F->blocks; b; b = b->next)
        for (Ins *i = b->first; i; i = i->next) {
            dbg_op = i->op;
            if (i->op == O_LEA && i->m.slot && !i->m.base && !i->m.index && !i->m.off && !i->m.sym && !i->m.args && !i->m.tls) {
                merge(i->dst, candidate(i->m.slot) ? i->m.slot : -1);
                continue;
            }
            if (i->op == O_MOV && !i->bimm) continue;      /* (step 2) */
            int nd = ins_defs(i, d);
            for (int k = 0; k < nd; k++) merge(d[k], -1);
        }
    for (int changed = 1; changed; ) {                   /* copies of addresses, to a fixed point */
        changed = 0;
        for (Block *b = F->blocks; b; b = b->next)
            for (Ins *i = b->first; i; i = i->next)
                if (i->op == O_MOV && !i->bimm) {
                    int s = aslot[i->a] ? aslot[i->a] : 0, was = aslot[i->dst];
                    if (!s) continue;
                    merge(i->dst, s);
                    changed |= aslot[i->dst] != was;
                }
    }
    for (Block *b = F->blocks; b; b = b->next)           /* a copy from what never got known: none */
        for (Ins *i = b->first; i; i = i->next)
            if (i->op == O_MOV && !i->bimm && !aslot[i->a]) merge(i->dst, -1);
    /* 2. every use of such an address must be a whole one; every other
     *    reference to the slot keeps it in memory */
    for (Block *b = F->blocks; b; b = b->next)
        for (Ins *i = b->first; i; i = i->next) {
            dbg_op = i->op;
            if (uses_m(i->op) && i->m.slot > 0 && i->m.slot < ns && !(i->op == O_LEA && aslot[i->dst] == i->m.slot))
                BAD(i->m.slot, "used directly (not through an address register)");
            if (i->op == O_ENTRY) for (int k = 0; k < i->npin; k++) if (i->pin[k].slot > 0 && i->pin[k].slot < ns) bad[i->pin[k].slot] = 1;
            if (i->op == O_CALL && i->call->vret_slot > 0 && i->call->vret_slot < ns) bad[i->call->vret_slot] = 1;
            if (i->op == O_CALL && i->call->x87slot > 0 && i->call->x87slot < ns) bad[i->call->x87slot] = 1;
            int nu = ins_uses(i, u);
            for (int k = 0; k < nu; k++) {
                int v = u[k], s = aslot[v];
                if (s <= 0) continue;
                int ok = 0, size = F->slots[s].size;
                switch (i->op) {
                case O_VEC:
                    if (i->cc == VOP_MOVEMASK) break;
                    ok = (v == i->a && i->cc != VOP_SPLAT) || v == i->b || (v == i->m.base && whole(&i->m) && i->cc != VOP_ADDV);
                    if (v == i->a && v == i->m.base) ok = ok && i->w <= 16;
                    if (i->w > 16) ok = 0;                   /* (32-byte vectors: two halves, in memory) */
                    break;
                case O_COPY: ok = (v == i->a || v == i->b) && i->imm == size; break;
                case O_ZERO: ok = v == i->a && i->imm == size; break;
                case O_MOV: ok = !i->bimm && aslot[i->dst] == s; break;
                }
                if (!ok) BAD(s, "its address has another use");
            }
        }
    /* 3. an O_VEC whose result stays in memory reads its operands from memory:
     *    they stay there too (to a fixed point) */
    for (int changed = 1; changed; ) {
        changed = 0;
        for (Block *b = F->blocks; b; b = b->next)
            for (Ins *i = b->first; i; i = i->next) {
                if (i->op != O_VEC || i->cc == VOP_ADDV || i->cc == VOP_MOVEMASK) continue;
                if (promoted(i->m.base)) continue;
                int ops[2] = { i->cc == VOP_SPLAT ? 0 : i->a, i->b };
                for (int k = 0; k < 2; k++)
                    if (promoted(ops[k])) { dbg_op = i->cc; BAD(aslot[ops[k]], "an operand of an operation whose result stays in memory"); changed = 1; }
            }
    }
    int any = 0;
    for (int s = 1; s < ns; s++)
        if (candidate(s) && !bad[s]) { vreg[s] = new_vreg(C_VEC); F->vr[vreg[s]].w = F->slots[s].size; any = 1; }
    if (any) {
        /* (new_vreg may have moved F->vr; aslot covers the old registers only) */
        int nv2 = F->nvr + 1;
        int *a2 = realloc(aslot, sizeof *aslot * nv2);
        if (!a2) die("out of memory");
        aslot = a2;
        for (int v = nv; v < nv2; v++) aslot[v] = -1;
        /* 4. the register forms */
        for (Block *b = F->blocks; b; b = b->next) {
            Ins *prev = 0;
            for (Ins *i = b->first; i; prev = i, i = i->next) {
                switch (i->op) {
                case O_LEA: case O_MOV:
                    if (i->dst < nv && promoted(i->dst)) { i->op = O_NOP; i->dst = i->a = 0; }
                    break;
                case O_ZERO:
                    if (promoted(i->a)) { int s = aslot[i->a]; i->op = O_VZERO; i->dst = vreg[s]; i->w = F->slots[s].size; i->a = 0; }
                    break;
                case O_COPY: {
                    int pd = promoted(i->a), ps = promoted(i->b);
                    if (!pd && !ps) break;
                    int w = (int)i->imm;
                    if (pd && ps) { i->op = O_VMOV; i->dst = vreg[aslot[i->a]]; i->a = vreg[aslot[i->b]]; i->b = 0; }
                    else if (pd) { i->op = O_VLD; i->dst = vreg[aslot[i->a]]; memset(&i->m, 0, sizeof i->m); i->m.base = i->b; i->m.scale = 1; i->a = i->b = 0; }
                    else { i->op = O_VST; memset(&i->m, 0, sizeof i->m); i->m.base = i->a; i->m.scale = 1; i->a = vreg[aslot[i->b]]; i->b = 0; i->dst = 0; }
                    i->w = w;
                    break;
                }
                case O_VEC: {
                    int scalar_dst = i->cc == VOP_ADDV || i->cc == VOP_MOVEMASK;
                    if (i->cc == VOP_MOVEMASK) break;
                    if (scalar_dst ? !promoted(i->a) : !promoted(i->m.base)) break;
                    int opv[2] = { i->cc == VOP_SPLAT ? 0 : i->a, i->b }, size[2] = { i->w, i->npin && i->npin < i->w ? (int)i->npin : i->w };
                    for (int k = 0; k < 2; k++) {
                        if (!opv[k]) continue;
                        if (promoted(opv[k])) { opv[k] = vreg[aslot[opv[k]]]; continue; }
                        Ins *l = new_ins(O_VLD);             /* an operand in memory: loaded first */
                        l->dst = new_vreg(C_VEC);
                        F->vr[l->dst].w = size[k] > 8 ? 16 : 8;
                        l->w = F->vr[l->dst].w;
                        l->m.base = opv[k]; l->m.scale = 1;
                        before(b, prev, i, l);
                        prev = l;
                        opv[k] = l->dst;
                    }
                    if (i->cc != VOP_SPLAT) i->a = opv[0];
                    i->b = opv[1];
                    if (!scalar_dst) i->dst = vreg[aslot[i->m.base]];
                    memset(&i->m, 0, sizeof i->m);
                    i->op = O_VREG;
                    break;
                }
                }
            }
        }
    }
    for (int s = 1; s < ns; s++) if (vreg[s]) F->slots[s].size = 0;   /* (no room in the frame) */
    if (any) { vec_copyprop(nv); vec_dce(nv); vec_accfuse(nv); }
    free(aslot); free(bad); free(vreg);
    aslot = 0; bad = 0; vreg = 0;
}
