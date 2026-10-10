/*
 * ir_a64.inc - AArch64 calls, parameters, returns, va_arg and inline asm
 * operands, by the AAPCS64 procedure call standard (included by ir.c: it
 * uses its helpers). SIEOS's one difference: long double is double.
 *
 * Where a value travels (a64_class):
 *   float, double: a v register (v0-v7), else 8 bytes of stack;
 *   an HFA (a struct, array or complex of 1-4 floats, or 1-4 doubles, and
 *     nothing else): one v register per member, all or none (else the stack);
 *   other values of 16 bytes or less (integers, pointers, __int128, small
 *     structs): 1 or 2 general registers (x0-x7; 16-byte aligned ones start
 *     at an even register), all or none (else the stack);
 *   a short vector (8 or 16 bytes), or an HVA (a struct or array of 1-4 short
 *     vectors of one size): one v register per vector, all or none;
 *   larger structs: copied by the caller, and their address passed instead.
 * A struct result too large for x0/x1 (or v0-v3) goes to a buffer whose
 * address the caller passes in x8.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */

enum { PA_REF, PA_GPR, PA_HFA, PA_FLT, PA_HVA };
typedef struct { int kind, n, esize; } A64Class;    /* n registers; esize: an HFA member's size */

/* Is t made only of members of one floating-point type? *base: that type, *n: how many */
static int hfa_walk(Type *t, Type **base, int *n)
{
    if (t->is_vector) {                         /* a short vector: an HVA's member */
        if (t->size != 8 && t->size != 16) return 0;
        if (*base && !((*base)->is_vector && (*base)->size == t->size)) return 0;
        *base = t;
        (*n)++;
        return 1;
    }
    if (t->kind == TY_FLOAT || t->kind == TY_DOUBLE) {
        if (*base && (*base)->kind != t->kind) return 0;
        *base = t;
        (*n)++;
        return 1;
    }
    if (t->kind == TY_COMPLEX) return hfa_walk(t->base, base, n) && hfa_walk(t->base, base, n);
    if (t->kind == TY_ARRAY) {
        if (t->len <= 0) return t->len == 0;
        for (int i = 0; i < t->len; i++) if (!hfa_walk(t->base, base, n)) return 0;
        return 1;
    }
    if (t->kind == TY_STRUCT && !t->is_vector) {
        for (Member *m = t->members; m; m = m->next) if (m->is_bitfield || !hfa_walk(m->ty, base, n)) return 0;
        return 1;
    }
    if (t->kind == TY_UNION) {                  /* every member an HFA of the same type; counted by size */
        int any = 0;
        for (Member *m = t->members; m; m = m->next) { int k = 0; if (m->is_bitfield || !hfa_walk(m->ty, base, &k)) return 0; any = 1; }
        if (any && *base) *n += t->size / (*base)->size;
        return any;
    }
    return 0;
}

static A64Class a64_class(Type *t)
{
    A64Class c = { PA_GPR, 1, 0 };
    if (is_flonum(t) && !is_cplx(t)) { c.kind = PA_FLT; c.esize = t->size; return c; }
    if (t->kind == TY_STRUCT || t->kind == TY_UNION || t->kind == TY_ARRAY || is_cplx(t)) {
        Type *b = 0;
        int n = 0;
        if (hfa_walk(t, &b, &n) && b && n >= 1 && n <= 4 && t->size == n * b->size) {
            c.kind = b->is_vector ? PA_HVA : PA_HFA; c.n = n; c.esize = b->size;
            return c;
        }
    }
    if (t->size > 16) { c.kind = PA_REF; return c; }
    c.n = t->size > 8 ? 2 : 1;
    return c;
}

/* member k of an HFA at address a, into a v register (as a float or double value) */
static int load_member(int a, int k, int esize)
{
    Addr m = addr_of_reg(a);
    m.off = (long)k * esize;
    int v = new_vreg(C_FLT);
    Ins *l = emit(O_LOAD);
    l->dst = v; l->m = m; l->sz = esize; l->w = esize;
    return v;
}

/* a copy of the struct at a in a new frame slot (by-reference arguments: the callee may change it) */
static int copy_to_slot(int a, Type *t)
{
    int s = lea(slot_addr(new_slot(t->size, t->align > 8 ? t->align : 8)));
    copy_mem(s, a, t->size);
    return s;
}

static int is_aggr(Type *t) { return t->kind == TY_STRUCT || t->kind == TY_UNION || is_i128(t) || is_cplx(t); }

static Op a64_call(Node *n, Call *c)
{
    Type *rt = n->ty;
    A64Class rc = { 0 };
    int memret = 0, retaddr = 0;
    if (is_aggr(rt)) { rc = a64_class(rt); memret = rc.kind == PA_REF; }
    if (memret) {                                  /* the result's buffer: x8 */
        retaddr = lea(slot_addr(new_slot(rt->size, rt->align)));
        CallArg *ca = &c->args[c->nargs++];
        ca->v = retaddr; ca->phys = 8; ca->cls = C_INT; ca->w = 8;
    }
    int gp = 0, fp = 0;
    long stk = 0;
    for (Node *a = n->args; a; a = a->next) {
        add_type(a);
        Type *t = a->ty;
        if (is_aggr(t)) {
            if (is_cplx(t)) need_float(a);
            int addr = gen_reg(a);
            A64Class cl = a64_class(t);
            if (cl.kind == PA_HFA) need_float(a);
            if (cl.kind == PA_REF) {               /* a copy, by its address */
                int p = copy_to_slot(addr, t);
                CallArg *ca = &c->args[c->nargs++];
                ca->v = p; ca->cls = C_INT; ca->w = 8;
                if (gp < 8) ca->phys = gp++;
                else { ca->phys = -1; ca->stack_off = stk; stk += 8; }
                continue;
            }
            if (cl.kind == PA_GPR && t->align >= 16) gp = (gp + 1) & ~1;
            int vr = cl.kind == PA_HFA || cl.kind == PA_HVA;
            if (vr ? fp + cl.n <= 8 : gp + cl.n <= 8) {
                for (int i = 0; i < cl.n; i++) {
                    CallArg *ca = &c->args[c->nargs++];
                    if (cl.kind == PA_HVA) {           /* a vector: loaded from its address into v<fp> by the call */
                        ca->addr = i ? reg(arith(O_ADD, R(addr), I((long)i * cl.esize), 8), 8) : addr;
                        ca->size = cl.esize; ca->cls = C_FLT; ca->w = cl.esize; ca->phys = A64_V0 + fp++;
                        continue;
                    }
                    if (cl.kind == PA_HFA) { ca->v = load_member(addr, i, cl.esize); ca->cls = C_FLT; ca->w = cl.esize; ca->phys = A64_V0 + fp++; }
                    else { ca->v = load_eightbyte(addr, t, i, C_INT); ca->cls = C_INT; ca->w = 8; ca->phys = gp++; }
                }
                continue;
            }
            if (vr) fp = 8; else gp = 8;
            CallArg *ca = &c->args[c->nargs++];
            stk = align_to(stk, t->align >= 16 ? 16 : 8);
            ca->addr = addr; ca->size = t->size; ca->phys = -1; ca->stack_off = stk;
            stk += align_to(t->size, 8);
            continue;
        }
        CallArg *ca = &c->args[c->nargs++];
        ca->cls = clsof(t);
        ca->w = wof(t);
        Op av = gen(a);
        if (av.k && ca->cls == C_INT && gp < 8) { ca->isimm = 1; ca->imm = av.imm; ca->phys = gp++; continue; }
        ca->v = reg(av, ca->w);
        if (ca->cls == C_FLT) {
            need_float(a);
            if (fp < 8) { ca->phys = A64_V0 + fp++; continue; }
        } else if (gp < 8) {
            if (F->vr[ca->v].hint < 0) F->vr[ca->v].hint = gp;
            ca->phys = gp++;
            continue;
        }
        ca->phys = -1;
        ca->stack_off = stk;
        stk += 8;
    }
    c->stack = align_to(stk, 16);
    Ins *i = emit(O_CALL);
    i->call = c;
    i->clob = A64_CALLER_SAVED;
    F->has_calls = 1;
    if (rt->kind == TY_VOID) return I(0);
    if (memret) return R(retaddr);
    if (is_aggr(rt) && rc.kind == PA_HVA) {        /* vectors in v0-v3: the call stores them in a slot */
        c->vret_slot = new_slot(align_to(rt->size, 8), 16);
        c->vret_n = rc.n;
        c->vret_es = rc.esize;
        return R(lea(slot_addr(c->vret_slot)));
    }
    if (is_aggr(rt)) {                             /* in registers: stored in a temporary */
        int hfa = rc.kind == PA_HFA, sz = hfa ? rc.esize : 8;
        for (int k = 0; k < rc.n; k++) {
            c->ret[k] = new_vreg(hfa ? C_FLT : C_INT);
            c->retw[k] = sz;
            c->retphys[k] = hfa ? A64_V0 + k : k;
        }
        c->nret = rc.n;
        int t = lea(slot_addr(new_slot(align_to(rt->size, 8), rt->align > 8 ? rt->align : 8)));
        for (int k = 0; k < rc.n; k++) {
            Addr m = addr_of_reg(t);
            m.off = (long)k * sz;
            store_sz(m, R(c->ret[k]), sz, hfa ? C_FLT : C_INT);
        }
        return R(t);
    }
    int v = new_vreg(clsof(rt));
    c->ret[0] = v;
    c->retw[0] = wof(rt);
    c->retphys[0] = is_flonum(rt) ? A64_V0 : 0;
    c->nret = 1;
    if (is_integer(rt) && rt->size < 4) {          /* char, short: extended by the caller */
        int d = new_vreg(C_INT);
        Ins *x = emit(O_EXT);
        x->dst = d; x->a = v; x->sz = rt->size; x->sgn = !rt->is_unsigned; x->w = 4;
        if (rt->kind == TY_BOOL) x->sgn = 0;
        return R(d);
    }
    return R(v);
}

/* ---- Parameters: where each arrives (and by-reference structs, copied in after O_ENTRY) */
typedef struct { Obj *v; int ptr; } RefParam;

static void a64_params(Obj *fn, Vec *pins, Vec *refs)
{
    Type *rt = fn->ty->base;
    int gp = 0, fp = 0;
    long stk = 0;
    if (is_aggr(rt) && a64_class(rt).kind == PA_REF) {
        retptr = new_vreg(C_INT);
        ParamIn *p = arena(sizeof *p);
        p->v = retptr; p->phys = 8; p->w = 8; p->cls = C_INT;
        vec_push(pins, p);
    }
    for (Obj *v = fn->params; v; v = v->pnext) {
        Type *t = v->ty;
        if (is_aggr(t)) {
            A64Class cl = a64_class(t);
            if (cl.kind == PA_REF) {               /* its address arrives: the caller's copy, ours to use */
                RefParam *r = arena(sizeof *r);
                r->v = v;
                r->ptr = new_vreg(C_INT);
                ParamIn *p = arena(sizeof *p);
                p->v = r->ptr; p->w = 8; p->cls = C_INT;
                if (gp < 8) p->phys = gp++;
                else { p->phys = -1; p->stack_off = stk; stk += 8; }
                vec_push(pins, p);
                vec_push(refs, r);
                continue;
            }
            if (cl.kind == PA_GPR && t->align >= 16) gp = (gp + 1) & ~1;
            int vr = cl.kind == PA_HFA || cl.kind == PA_HVA;
            if (vr ? fp + cl.n <= 8 : gp + cl.n <= 8) {
                v->slot = new_slot(align_to(t->size, 8), t->align > 8 ? t->align : 8);
                for (int i = 0; i < cl.n; i++) {
                    ParamIn *p = arena(sizeof *p);
                    p->slot = v->slot;
                    if (vr) { p->off = (long)i * cl.esize; p->w = cl.esize; p->cls = C_FLT; p->phys = A64_V0 + fp++; }
                    else { p->off = i * 8; p->w = 8; p->cls = C_INT; p->phys = gp++; }
                    vec_push(pins, p);
                }
                continue;
            }
            if (vr) fp = 8; else gp = 8;
            stk = align_to(stk, t->align >= 16 ? 16 : 8);
            v->slot = -1;
            v->param_stack_off = stk;
            stk += align_to(t->size, 8);
            continue;
        }
        int cls = clsof(t);
        ParamIn *p = arena(sizeof *p);
        p->w = cls == C_FLT ? t->size : wof(t);
        p->cls = cls;
        if (cls == C_FLT && fp < 8) p->phys = A64_V0 + fp++;
        else if (cls == C_INT && gp < 8) p->phys = gp++;
        else { p->phys = -1; p->stack_off = stk; stk += 8; }
        if (v->vreg) { p->v = v->vreg; F->vr[v->vreg].hint = p->phys; }
        else if (p->phys < 0) { v->slot = -1; v->param_stack_off = p->stack_off; continue; }
        else {
            v->slot = new_slot(t->size, t->align);
            p->slot = v->slot;
            p->w = t->size;                     /* exactly the slot: a char's neighbours are other variables */
        }
        if (p->v && p->phys < 0 && t->size < 4 && is_integer(t)) p->w = 4;
        vec_push(pins, p);
    }
    ngp_named = gp;
    nfp_named = fp;
    stack_named = stk;
}

static void a64_ref_copies(Vec *refs)
{
    for (int k = 0; k < refs->n; k++) {
        RefParam *r = refs->v[k];                  /* the caller's copy is ours: used in place */
        r->v->slot = -2;
        r->v->ref_vreg = r->ptr;
    }
}

/* ---- return */
static void a64_return(Type *t, int a)
{
    Ins *r;
    A64Class cl = a64_class(t);
    if (cl.kind == PA_REF) {                       /* into the caller's buffer */
        copy_mem(retptr, a, t->size);
        r = emit(O_RET);
        r->w = 8;
        return;
    }
    if (cl.kind == PA_HVA) {                       /* vectors: loaded into v0-v3 from their address */
        r = emit(O_RET);
        r->a = a; r->sgn = 3; r->imm = cl.n; r->cc = cl.esize; r->w = 8;
        return;
    }
    int hfa = cl.kind == PA_HFA;
    ParamIn *pin = arena(sizeof(ParamIn) * cl.n);
    for (int k = 0; k < cl.n; k++) {
        pin[k].v = hfa ? load_member(a, k, cl.esize) : load_eightbyte(a, t, k, C_INT);
        pin[k].phys = hfa ? A64_V0 + k : k;
        pin[k].w = hfa ? cl.esize : 8;
        pin[k].cls = hfa ? C_FLT : C_INT;
    }
    r = emit(O_RET);
    r->pin = pin;
    r->npin = cl.n;
    r->sgn = 2;                                    /* the values are in pin[] */
    r->w = 8;
}

/* ---- va_start, va_arg. va_list: { __stack, __gr_top, __vr_top, __gr_offs, __vr_offs };
 * the save area (192 bytes): x0-x7, then q0-q7. */
static void a64_va_start(int ap)
{
    Addr m = addr_of_reg(ap);
    Addr args = { 0 };
    args.args = 1;
    args.off = stack_named;
    args.scale = 1;
    store_sz(m, R(lea(args)), 8, C_INT);
    int area = lea(slot_addr(F->va_slot));
    m.off = 8;
    store_sz(m, arith(O_ADD, R(area), I(64), 8), 8, C_INT);
    m.off = 16;
    store_sz(m, arith(O_ADD, R(area), I(192), 8), 8, C_INT);
    m.off = 24;
    store_sz(m, I((ngp_named - 8) * 8), 4, C_INT);
    m.off = 28;
    store_sz(m, I((nfp_named - 8) * 16), 4, C_INT);
}

static Op a64_va_arg(Node *n)
{
    int ap = gen_reg(n->args);
    Type *t = n->ty;
    A64Class cl = is_aggr(t) ? a64_class(t) : (A64Class){ is_flonum(t) ? PA_FLT : PA_GPR, 1, t->size };
    if (cl.kind == PA_HVA) error_at(n->tok, "va_arg of a vector type is not supported on AArch64");
    int byref = cl.kind == PA_REF, fpk = cl.kind == PA_HFA || cl.kind == PA_FLT, agg = is_mem(t);
    int nreg = byref ? 1 : cl.n;
    Addr stackp = addr_of_reg(ap), top = addr_of_reg(ap), offs = addr_of_reg(ap);
    top.off = fpk ? 16 : 8;
    offs.off = fpk ? 28 : 24;
    int res = new_vreg(agg ? C_INT : clsof(t));
    Block *stack = new_block(), *done = new_block(), *try = new_block(), *inreg = new_block();
    Op o = load(offs, ty_int);
    Ins *b = emit(O_BR);
    b->cc = CC_GE; b->a = reg(o, 4); b->bimm = 1; b->imm = 0; b->w = 4; b->t = stack; b->f = try;
    place(try);
    if (!fpk && !byref && t->align >= 16) o = arith(O_AND, arith(O_ADD, o, I(15), 4), I(-16), 4);
    Op no = arith(O_ADD, o, I(fpk ? 16 * nreg : 8 * nreg), 4);
    store(offs, no, ty_int);
    b = emit(O_BR);
    b->cc = CC_GT; b->a = reg(no, 4); b->bimm = 1; b->imm = 0; b->w = 4; b->t = stack; b->f = inreg;
    place(inreg);
    Op tv = load(top, pointer_to(ty_void)), ov = cast(o, ty_int, ty_long, n);    /* (in this order: C leaves argument order open) */
    int base = reg(arith(O_ADD, tv, ov, 8), 8);
    if (cl.kind == PA_HFA) {                       /* members 16 bytes apart: gathered into a temporary */
        int tmp = lea(slot_addr(new_slot(t->size, 8)));
        for (int k = 0; k < cl.n; k++) {
            Addr d = addr_of_reg(tmp);
            d.off = (long)k * cl.esize;
            int v = new_vreg(C_FLT);
            Addr s = addr_of_reg(base);
            s.off = 16L * k;
            Ins *l = emit(O_LOAD);
            l->dst = v; l->m = s; l->sz = cl.esize; l->w = cl.esize;
            store_sz(d, R(v), cl.esize, C_FLT);
        }
        move(res, R(tmp), 8);
    } else if (byref) {
        Ins *l = emit(O_LOAD);
        l->dst = res; l->m = addr_of_reg(base); l->sz = 8; l->w = 8;
    } else if (agg) move(res, R(base), 8);
    else {
        Ins *l = emit(O_LOAD);
        l->dst = res; l->m = addr_of_reg(base); l->sz = t->size; l->w = wof(t); l->sgn = !t->is_unsigned && t->kind != TY_PTR;
    }
    jump(done);
    place(stack);
    int pv = reg(load(stackp, pointer_to(ty_void)), 8);
    if (!byref && t->align >= 16) pv = reg(arith(O_AND, arith(O_ADD, R(pv), I(15), 8), I(-16), 8), 8);
    if (byref) {
        Ins *l = emit(O_LOAD);
        l->dst = res; l->m = addr_of_reg(pv); l->sz = 8; l->w = 8;
    } else if (agg) move(res, R(pv), 8);
    else {
        Ins *l = emit(O_LOAD);
        l->dst = res; l->m = addr_of_reg(pv); l->sz = t->size; l->w = wof(t); l->sgn = !t->is_unsigned && t->kind != TY_PTR;
    }
    store(stackp, arith(O_ADD, R(pv), I(byref ? 8 : align_to(t->size, 8)), 8), pointer_to(ty_void));
    place(done);
    return R(res);
}

/* ---- Inline asm operands. Constraints: r (a general register), w (a
 * floating-point/SIMD register), m and Q (memory, through a base register),
 * i n I J K L M N (constants), a digit (the same place as that output);
 * register variables keep their register. */
static int a64_reg_by_name(const char *s)
{
    int n = 0;
    if (!strcmp(s, "sp")) return A64_SP;
    if (!strcmp(s, "lr")) return 30;
    if (!strcmp(s, "fp")) return 29;
    if (!*s || !s[1] || s[1] < '0' || s[1] > '9') return -1;
    for (const char *p = s + 1; *p; p++) { if (*p < '0' || *p > '9') return -1; n = n * 10 + *p - '0'; }
    if (n > 31) return -1;
    if (*s == 'x' || *s == 'w') return n < 31 ? n : -1;
    if (strchr("vbhsdq", *s)) return A64_V0 + n;
    return -1;
}

static const int a64_asm_gp[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28 };

static int a64_pick(RegSet *taken, int fl)
{
    if (fl) {
        for (int r = A64_V0; r < A64_V0 + 30; r++) if (!(*taken & BIT(r))) { *taken |= BIT(r); return r; }
        return -1;
    }
    for (unsigned i = 0; i < sizeof a64_asm_gp / sizeof a64_asm_gp[0]; i++)
        if (!(*taken & BIT(a64_asm_gp[i]))) { *taken |= BIT(a64_asm_gp[i]); return a64_asm_gp[i]; }
    return -1;
}

static Op a64_asm(Node *n)
{
    AsmStmt *s = n->asm_;
    AsmIns *ai = arena(sizeof *ai);
    ai->tmpl = s->tmpl;
    ai->is_basic = s->is_basic;
    int total = s->nout + s->nin;
    ai->op = arena(sizeof(AsmOperand) * (total + 1));
    ai->n = total;
    RegSet taken = BIT(18) | BIT(29) | BIT(30) | BIT(A64_SP) | BIT(14) | BIT(15) | BIT(16) | BIT(17), clob = 0;
    for (int i = 0; i < s->nclob; i++) { int r = a64_reg_by_name(s->clob[i]); if (r >= 0) clob |= BIT(r); }
    taken |= clob;
    Addr outaddr[32];
    Node *outlv[32];
    for (int k = 0; k < total; k++) {           /* register variables first */
        AsmOp *o = k < s->nout ? &s->out[k] : &s->in[k - s->nout];
        AsmOperand *op = &ai->op[k];
        op->phys = -1;
        op->name = o->name;
        add_type(o->expr);
        op->size = is_mem(o->expr->ty) || o->expr->ty->size > 8 ? 8 : o->expr->ty->size;
        if (o->expr->kind == ND_VAR && o->expr->var->reg && strpbrk(o->cons, "rw")) {
            op->phys = a64_reg_by_name(o->expr->var->reg);
            if (op->phys < 0) error_at(o->expr->tok, "asm: unknown register \"%s\"", o->expr->var->reg);
            op->kind = 'r';
            taken |= BIT(op->phys);
        }
    }
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
        if (!is_out && *c >= '0' && *c <= '9') { op->kind = 'r'; op->phys = ai->op[*c - '0'].phys; continue; }
        if (!is_out && strpbrk(c, "inIJKLMN") && is_const_expr(o->expr)) { op->kind = 'i'; op->imm = const_eval(o->expr); continue; }
        if (strpbrk(c, "rw")) {
            op->phys = a64_pick(&taken, strchr(c, 'w') && !strchr(c, 'r'));
            if (op->phys < 0) error_at(o->expr->tok, "asm: no register left for this operand");
            op->kind = 'r';
            continue;
        }
        if (strpbrk(c, "mQ")) { op->kind = 'm'; continue; }
        error_at(o->expr->tok, "asm: unsupported constraint \"%s\"", o->cons);
    }
    for (int k = 0; k < total; k++) {           /* values: outputs' addresses, inputs */
        int is_out = k < s->nout;
        AsmOp *o = is_out ? &s->out[k] : &s->in[k - s->nout];
        AsmOperand *op = &ai->op[k];
        Node *e = o->expr;
        int fl = op->kind == 'r' && op->phys >= A64_V0;
        if (is_out) {
            outlv[k] = e;
            int regvar = e->kind == ND_VAR && e->var->is_local && e->var->vreg;
            if (op->kind == 'm' || !regvar) outaddr[k] = regvar ? (Addr){ 0 } : gen_addr(e);
            if (op->kind == 'm') { op->v = lea(outaddr[k]); op->mreg = -2; op->in = 1; }
            else {
                op->outv = new_vreg(fl ? C_FLT : C_INT);
                if (op->in) op->v = regvar ? e->var->vreg : reg(load(outaddr[k], e->ty), fl ? op->size : 8);
            }
            continue;
        }
        if (op->kind == 'i') continue;
        if (op->kind == 'm') {
            Addr m;
            if (e->kind == ND_VAR && e->var->is_local && e->var->vreg) { m = slot_addr(new_slot(8, 8)); store(m, R(e->var->vreg), e->ty); }
            else if (e->kind == ND_VAR || e->kind == ND_DEREF || e->kind == ND_MEMBER) m = gen_addr(e);
            else { m = slot_addr(new_slot(8, 8)); store(m, gen(e), e->ty); }
            op->v = lea(m);
            op->mreg = -2;
            continue;
        }
        op->v = reg(gen(e), fl ? op->size : 8);
    }
    for (int k = 0; k < total; k++) {           /* memory operands: their base register */
        AsmOperand *op = &ai->op[k];
        if (op->mreg != -2) continue;
        op->mreg = a64_pick(&taken, 0);
        if (op->mreg < 0) error_at(n->tok, "asm: no register left for a memory operand");
    }
    Ins *i = emit(O_ASM);
    i->asmi = ai;
    i->clob = (taken | clob) & ~(BIT(A64_SP) | BIT(29) | BIT(18) | BIT(14) | BIT(15) | BIT(16) | BIT(17));
    if (ai->is_basic) i->clob = 0;
    for (int k = 0; k < s->nout; k++) {
        AsmOperand *op = &ai->op[k];
        if (op->kind == 'm') continue;
        Node *e = outlv[k];
        if (e->kind == ND_VAR && e->var->is_local && e->var->vreg) move(e->var->vreg, R(op->outv), op->phys >= A64_V0 ? op->size : 8);
        else store(outaddr[k], R(op->outv), e->ty);
    }
    return I(0);
}
