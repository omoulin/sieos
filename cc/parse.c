/*
 * parse.c - Tokens to a syntax tree with types (C11 and the GNU extensions
 * SIEOS uses), and constant expressions.
 *
 * A recursive-descent parser, one function per grammar rule. Every implicit
 * conversion of C (integer promotion, the usual arithmetic conversions, the
 * conversion to the type of an assignment, a parameter or a return value)
 * becomes an explicit cast node here, so the code generator never has to
 * think about C's typing rules. Pointer arithmetic is scaled here too.
 *
 * x += y and x++ keep x as one lvalue (ND_ASSIGN with val 1 or 2; the right
 * side reads it through ND_SELF), so x is computed once and a local x can
 * stay in a register.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "sicc.h"

int opt_general_regs_only;
int opt_lse;
int opt_dotprod;                    /* AArch64: the dot product instructions (ARMv8.2 +dotprod: the Pi 5's Cortex-A76) */
Buf global_asm;

/* ---- Types */
static Type t_void = { .kind = TY_VOID, .size = 1, .align = 1 };
static Type t_bool = { .kind = TY_BOOL, .size = 1, .align = 1, .is_unsigned = 1 };
static Type t_char = { .kind = TY_CHAR, .size = 1, .align = 1 };
static Type t_schar = { .kind = TY_CHAR, .size = 1, .align = 1 };
static Type t_uchar = { .kind = TY_CHAR, .size = 1, .align = 1, .is_unsigned = 1 };
static Type t_short = { .kind = TY_SHORT, .size = 2, .align = 2 };
static Type t_ushort = { .kind = TY_SHORT, .size = 2, .align = 2, .is_unsigned = 1 };
static Type t_int = { .kind = TY_INT, .size = 4, .align = 4 };
static Type t_uint = { .kind = TY_INT, .size = 4, .align = 4, .is_unsigned = 1 };
static Type t_long = { .kind = TY_LONG, .size = 8, .align = 8 };
static Type t_ulong = { .kind = TY_LONG, .size = 8, .align = 8, .is_unsigned = 1 };
static Type t_float = { .kind = TY_FLOAT, .size = 4, .align = 4 };
static Type t_double = { .kind = TY_DOUBLE, .size = 8, .align = 8 };
static Type t_int128 = { .kind = TY_INT128, .size = 16, .align = 16 };
static Type t_uint128 = { .kind = TY_INT128, .size = 16, .align = 16, .is_unsigned = 1 };
static Type t_ldouble = { .kind = TY_LDOUBLE, .size = 16, .align = 16 };   /* x87 extended: 10 bytes used */
Type *ty_int128 = &t_int128, *ty_uint128 = &t_uint128, *ty_ldouble = &t_ldouble;
Type *ty_void = &t_void, *ty_bool = &t_bool, *ty_char = &t_char, *ty_schar = &t_schar,
     *ty_uchar = &t_uchar, *ty_short = &t_short, *ty_ushort = &t_ushort, *ty_int = &t_int,
     *ty_uint = &t_uint, *ty_long = &t_long, *ty_ulong = &t_ulong, *ty_float = &t_float,
     *ty_double = &t_double;

/* AArch64 (AAPCS64, as SIEOS uses it): plain char is unsigned, and long
 * double is the same as double (as on another major 64-bit ARM platform:
 * nothing in SIEOS needs more, and quad precision would be emulated in
 * software). */
void target_init(void)
{
    if (target != T_AARCH64) return;
    t_char.is_unsigned = 1;
    ty_ldouble = ty_double;
}

static Type *new_type(int kind, int size, int align)
{
    Type *t = arena(sizeof *t);
    t->kind = kind;
    t->size = size;
    t->align = align;
    return t;
}

Type *pointer_to(Type *base)
{
    Type *t = new_type(TY_PTR, 8, 8);
    t->base = base;
    t->is_unsigned = 1;
    return t;
}

Type *array_of(Type *base, int len)
{
    Type *t = new_type(TY_ARRAY, base->size * (len < 0 ? 0 : len), base->align);
    t->base = base;
    t->len = len;
    return t;
}

Type *func_type(Type *ret)
{
    Type *t = new_type(TY_FUNC, 1, 1);
    t->base = ret;
    return t;
}

/* float/double/long double _Complex: two of the base, real part first */
Type *complex_of(Type *base)
{
    static Type *c[3];
    int k = base->kind == TY_FLOAT ? 0 : base->kind == TY_DOUBLE ? 1 : 2;
    if (!c[k]) {
        c[k] = new_type(TY_COMPLEX, base->size * 2, base->align);
        c[k]->base = base;
    }
    return c[k];
}

Type *copy_type(Type *t)
{
    Type *n = arena(sizeof *n);
    *n = *t;
    n->origin = t;
    n->next = 0;
    return n;
}

int is_integer(Type *t) { int k = t->kind; return k == TY_BOOL || k == TY_CHAR || k == TY_SHORT || k == TY_INT || k == TY_LONG || k == TY_ENUM || k == TY_INT128; }
int is_flonum(Type *t) { return t->kind == TY_FLOAT || t->kind == TY_DOUBLE || t->kind == TY_LDOUBLE; }
int is_numeric(Type *t) { return is_integer(t) || is_flonum(t) || t->kind == TY_COMPLEX; }
int is_scalar(Type *t) { return is_numeric(t) || t->kind == TY_PTR; }
int align_to(long n, int a) { return (n + a - 1) / a * a; }
static int is_aggregate(Type *t) { return t->kind == TY_ARRAY || t->kind == TY_STRUCT || t->kind == TY_UNION; }

int is_compatible(Type *a, Type *b)
{
    if (a == b) return 1;
    if (a->origin) return is_compatible(a->origin, b);
    if (b->origin) return is_compatible(a, b->origin);
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case TY_CHAR: case TY_SHORT: case TY_INT: case TY_LONG: case TY_INT128:
        return a->is_unsigned == b->is_unsigned && (a != ty_char) == (b != ty_char);
    case TY_FLOAT: case TY_DOUBLE: case TY_LDOUBLE: case TY_BOOL: case TY_VOID: return 1;
    case TY_PTR: case TY_COMPLEX: return is_compatible(a->base, b->base);
    case TY_ARRAY: return is_compatible(a->base, b->base) && (a->len < 0 || b->len < 0 || a->len == b->len);
    case TY_FUNC: {
        if (!is_compatible(a->base, b->base) || a->is_variadic != b->is_variadic) return 0;
        Type *p = a->params, *q = b->params;
        for (; p && q; p = p->next, q = q->next) if (!is_compatible(p, q)) return 0;
        return !p && !q;
    }
    }
    return 0;
}

/* ---- Scopes: names (variables, typedefs, enum constants) and tags */
typedef struct { Obj *var; Type *tdef; Type *enum_ty; long enum_val; } Sym;
typedef struct Scope { struct Scope *up; Map syms, tags; } Scope;
static Scope global_scope, *scope = &global_scope;

static void enter(void) { Scope *s = arena(sizeof *s); s->up = scope; scope = s; }
static void leave(void) { scope = scope->up; }

static Sym *find_sym(const char *name)
{
    for (Scope *s = scope; s; s = s->up) { Sym *x = map_get(&s->syms, name); if (x) return x; }
    return 0;
}

static Sym *push_sym(const char *name)
{
    Sym *x = arena(sizeof *x);
    map_put(&scope->syms, name, x);
    return x;
}

static Type *find_tag(const char *name)
{
    for (Scope *s = scope; s; s = s->up) { Type *t = map_get(&s->tags, name); if (t) return t; }
    return 0;
}

static Type *find_typedef(Token *t)
{
    if (t->kind != TK_IDENT) return 0;
    Sym *s = find_sym(t->name);
    return s ? s->tdef : 0;
}

/* ---- Parser state */
static Obj *globals, **globals_tail = &globals;
static Obj *cur_fn;                 /* the function being parsed */
static Obj *cur_owner;              /* who gets references to globals (function, or variable being initialized) */
static Obj *cur_locals;
static Node *cur_switch, *cur_brk, *cur_cont;
static int anon_n;

/* Variable-length arrays live on the stack: a block that declares one
 * saves the stack pointer before the first, and gives the space back at
 * its end, or when break, continue or goto leave it. */
typedef struct VlaScope { Obj *sp; struct VlaScope *up; } VlaScope;
static VlaScope *vla_scope;          /* the innermost block holding VLAs */
static Obj *block_sp;                /* this block's saved stack pointer (0: no VLA yet) */
static int param_depth, in_stmt_expr;
static Vec fn_gotos, fn_labels;      /* this function's gotos (and &&label) and labels: checked at its end */
static Map fn_label_names;           /* label -> its assembler name, .Llab<n>, unique in the unit */
static int label_n;

static const char *label_name(Token *t)
{
    const char *s = map_get(&fn_label_names, t->name);
    if (!s) { s = fmt(".Llab%d", label_n++); map_put(&fn_label_names, t->name, (void *)s); }
    return s;
}

typedef struct {
    int is_typedef, is_static, is_extern, is_inline, is_noreturn, is_tls, align;
    int packed, aligned, used, weak, noinline, always_inline, vecsize;
    const char *section;
} VarAttr;
static Obj *declare_function(Type *ty, VarAttr *a, Token *name, const char *asm_name);

static Type *declspec(Token **rest, Token *tok, VarAttr *a);
static Type *declarator(Token **rest, Token *tok, Type *ty, Token **name);
static Type *typename(Token **rest, Token *tok);
static Node *expr(Token **rest, Token *tok);
static Node *assign(Token **rest, Token *tok);
static Node *conditional(Token **rest, Token *tok);
static Node *cast(Token **rest, Token *tok);
static Node *unary(Token **rest, Token *tok);
static Node *postfix(Token **rest, Token *tok);
static Node *compound_stmt(Token **rest, Token *tok);
static Node *stmt(Token **rest, Token *tok);
static Node *declaration(Token **rest, Token *tok, Type *base, VarAttr *a);
static Token *global_decl(Token *tok, Type *base, VarAttr *a);
static long eval_rval(Node *n, const char **label);
static Node *complex_part(Node *e, int imag, Token *op);
static int is_vec(Node *n);
static Node *vec_node(int op, Node *l, Node *r, Type *ty, Token *tok);
static Node *vec_binop(int kind, Node *l, Node *r, Token *op);
static int has_vla(Type *t);
static Node *vla_sizes(Type *t, Token *tok);
static Node *builtin_node(int b, Node *a, Type *ty, Token *tok);
static Obj *vla_restore(VlaScope *from, VlaScope *to, Token *tok);
static Obj *new_lvar(const char *name, Type *ty);
static int is_typename(Token *t);

static Token *skip(Token *t, int p)
{
    if (!tok_is(t, p)) {
        char want[4] = { 0 };
        if (p < 256) want[0] = p;
        error_at(t, "expected '%s'", p < 256 ? want : p == P_ELLIPSIS ? "..." : "?");
    }
    return t->next;
}

/* between declarators: ',' to go on, else the declaration should have ended */
static Token *next_declarator(Token *t)
{
    if (!tok_is(t, ',')) error_at(t, "expected ';'");
    return t->next;
}

static int consume(Token **rest, Token *t, int p)
{
    if (tok_is(t, p)) { *rest = t->next; return 1; }
    *rest = t;
    return 0;
}

static const char *ident(Token *t)
{
    if (t->kind != TK_IDENT) error_at(t, "expected an identifier");
    return t->name;
}

/* ---- Nodes */
static Node *new_node(int kind, Token *tok)
{
    Node *n = arena(sizeof *n);
    n->kind = kind;
    n->tok = tok;
    return n;
}

static Node *new_unary(int kind, Node *l, Token *tok) { Node *n = new_node(kind, tok); n->lhs = l; return n; }
static Node *new_binary(int kind, Node *l, Node *r, Token *tok) { Node *n = new_node(kind, tok); n->lhs = l; n->rhs = r; return n; }

static Node *new_num(long v, Token *tok) { Node *n = new_node(ND_NUM, tok); n->val = v; n->ty = ty_int; return n; }
static Node *new_long(long v, Token *tok) { Node *n = new_node(ND_NUM, tok); n->val = v; n->ty = ty_long; return n; }
static Node *new_ulong(long v, Token *tok) { Node *n = new_node(ND_NUM, tok); n->val = v; n->ty = ty_ulong; return n; }

static void add_ref(Obj *g) { if (cur_owner) vec_push(&cur_owner->refs, g); }

static Node *new_var_node(Obj *v, Token *tok)
{
    if (v->vla_ptr) {                              /* a VLA: *(its hidden pointer) */
        Node *p = new_node(ND_VAR, tok);
        p->var = v->vla_ptr;
        p->ty = v->vla_ptr->ty;
        Node *d = new_unary(ND_DEREF, p, tok);
        d->ty = v->ty;
        return d;
    }
    Node *n = new_node(ND_VAR, tok);
    n->var = v;
    n->ty = v->ty;
    if (!v->is_local) add_ref(v);
    return n;
}

void add_type(Node *n);

/* The same representation: a cast between them changes nothing. */
static int same_repr(Type *a, Type *b)
{
    if (a->kind == TY_ENUM) a = a->is_unsigned ? ty_uint : ty_int;
    if (b->kind == TY_ENUM) b = b->is_unsigned ? ty_uint : ty_int;
    if (a->kind == TY_PTR && b->kind == TY_PTR) return 1;
    return a->kind == b->kind && a->size == b->size && a->is_unsigned == b->is_unsigned && a->kind != TY_STRUCT && a->kind != TY_UNION;
}

static long truncate_to(long v, Type *t);

static Node *new_cast(Node *e, Type *ty)
{
    add_type(e);
    if (e->ty == ty) return e;
    Node *n = new_node(ND_CAST, e->tok);
    n->lhs = e;
    n->ty = copy_type(ty);
    n->ty->is_const = n->ty->is_volatile = 0;
    if (same_repr(e->ty, ty) && e->kind != ND_NUM) return n;   /* kept: it changes the type */
    if (e->kind == ND_NUM && is_integer(ty) && is_integer(e->ty) && ty->kind != TY_INT128 && e->ty->kind != TY_INT128) {   /* fold now */
        Node *c = new_node(ND_NUM, e->tok);
        long v = truncate_to(e->val, ty);
        c->val = v;
        c->ty = n->ty;
        return c;
    }
    return n;
}

static Type *integer_promote(Type *t)
{
    if (t->kind == TY_ENUM) return t->is_unsigned ? ty_uint : ty_int;
    if (is_integer(t) && t->size < 4) return ty_int;
    return t;
}

static Type *common_type(Type *a, Type *b)
{
    if (a->kind == TY_ARRAY) return pointer_to(a->base);
    if (a->kind == TY_FUNC) return pointer_to(a);
    if (a->kind == TY_COMPLEX || b->kind == TY_COMPLEX) {     /* complex, of the wider real type */
        Type *x = a->kind == TY_COMPLEX ? a->base : is_flonum(a) ? a : ty_double;
        Type *y = b->kind == TY_COMPLEX ? b->base : is_flonum(b) ? b : ty_double;
        return complex_of(x->size >= y->size ? x : y);
    }
    if (a->kind == TY_LDOUBLE || b->kind == TY_LDOUBLE) return ty_ldouble;
    if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE) return ty_double;
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT) return ty_float;
    a = integer_promote(a);
    b = integer_promote(b);
    if (a->size != b->size) return a->size > b->size ? a : b;
    if (b->is_unsigned) return b;
    return a;
}

static void usual_arith(Node **l, Node **r)
{
    Type *t = common_type((*l)->ty, (*r)->ty);
    *l = new_cast(*l, t);
    *r = new_cast(*r, t);
}

static Node *promote(Node *n) { add_type(n); return new_cast(n, integer_promote(n->ty)); }

void add_type(Node *n)
{
    if (!n || n->ty) return;
    add_type(n->lhs);
    add_type(n->rhs);
    add_type(n->cond);
    add_type(n->then);
    add_type(n->els);
    add_type(n->init);
    add_type(n->inc);
    for (Node *x = n->body; x; x = x->next) add_type(x);
    for (Node *x = n->args; x; x = x->next) add_type(x);
    switch (n->kind) {
    case ND_NUM: n->ty = ty_int; return;
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD: case ND_AND: case ND_OR: case ND_XOR:
        usual_arith(&n->lhs, &n->rhs);
        n->ty = n->lhs->ty;
        return;
    case ND_NEG: case ND_BITNOT:                    /* (~ of a complex: its conjugate) */
        n->lhs = is_flonum(n->lhs->ty) || n->lhs->ty->kind == TY_COMPLEX ? n->lhs : promote(n->lhs);
        n->ty = n->lhs->ty;
        return;
    case ND_SHL: case ND_SHR:
        n->lhs = promote(n->lhs);
        n->rhs = promote(n->rhs);
        n->ty = n->lhs->ty;
        return;
    case ND_ASSIGN:
        if (n->lhs->ty->kind == TY_ARRAY) error_at(n->tok, "an array cannot be assigned");
        if (n->lhs->ty->kind != TY_STRUCT && n->lhs->ty->kind != TY_UNION) n->rhs = new_cast(n->rhs, n->lhs->ty);
        n->ty = n->lhs->ty;
        return;
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
        if (n->lhs->ty->kind == TY_PTR || n->lhs->ty->kind == TY_ARRAY || n->rhs->ty->kind == TY_PTR || n->rhs->ty->kind == TY_ARRAY) {
            if (is_integer(n->lhs->ty)) n->lhs = new_cast(n->lhs, ty_ulong);
            if (is_integer(n->rhs->ty)) n->rhs = new_cast(n->rhs, ty_ulong);
        } else usual_arith(&n->lhs, &n->rhs);
        n->ty = ty_int;
        return;
    case ND_NOT: case ND_LOGAND: case ND_LOGOR: n->ty = ty_int; return;
    case ND_VAR: n->ty = n->var->ty; return;
    case ND_COMMA: n->ty = n->rhs->ty; return;
    case ND_MEMBER: n->ty = n->member->ty; return;
    case ND_ADDR:
        n->ty = pointer_to(n->lhs->ty);
        return;
    case ND_DEREF: {
        Type *t = n->lhs->ty;
        if (t->kind != TY_PTR && t->kind != TY_ARRAY) {
            if (t->kind == TY_FUNC) { n->ty = t; return; }
            error_at(n->tok, "not a pointer");
        }
        if (t->base->kind == TY_VOID) error_at(n->tok, "dereferencing a void pointer");
        n->ty = t->base;
        return;
    }
    case ND_COND:
        if (n->then->ty->kind == TY_VOID || n->els->ty->kind == TY_VOID) n->ty = ty_void;
        else if (is_numeric(n->then->ty) && is_numeric(n->els->ty)) { usual_arith(&n->then, &n->els); n->ty = n->then->ty; }
        else if (n->then->ty->kind == TY_PTR || n->then->ty->kind == TY_ARRAY) {
            n->ty = n->then->ty->kind == TY_ARRAY ? pointer_to(n->then->ty->base) : n->then->ty;
            if (is_integer(n->els->ty)) n->els = new_cast(n->els, n->ty);
        } else if (n->els->ty->kind == TY_PTR || n->els->ty->kind == TY_ARRAY) {
            n->ty = n->els->ty->kind == TY_ARRAY ? pointer_to(n->els->ty->base) : n->els->ty;
            if (is_integer(n->then->ty)) n->then = new_cast(n->then, n->ty);
        } else n->ty = n->then->ty;
        return;
    case ND_STMT_EXPR: {
        Node *last = n->body;
        while (last && last->next) last = last->next;
        n->ty = last && last->kind == ND_EXPR_STMT ? last->lhs->ty : ty_void;
        return;
    }
    }
}

/* ---- Pointer arithmetic */
static Node *new_add(Node *l, Node *r, Token *tok)
{
    if (is_vec(l) || is_vec(r)) return vec_binop(ND_ADD, l, r, tok);
    add_type(l);
    add_type(r);
    int lp = l->ty->kind == TY_PTR || l->ty->kind == TY_ARRAY, rp = r->ty->kind == TY_PTR || r->ty->kind == TY_ARRAY;
    if (!lp && !rp) return new_binary(ND_ADD, l, r, tok);
    if (lp && rp) error_at(tok, "adding two pointers");
    if (rp) { Node *t = l; l = r; r = t; }
    if (!is_integer(r->ty)) error_at(tok, "invalid operand to pointer arithmetic");
    Type *pt = l->ty->kind == TY_ARRAY ? pointer_to(l->ty->base) : l->ty;
    int size = pt->base->kind == TY_VOID || pt->base->kind == TY_FUNC ? 1 : pt->base->size;
    Node *off = new_cast(r, ty_long);
    if (pt->base->is_vla) off = new_binary(ND_MUL, off, new_cast(new_var_node(pt->base->vla_size, tok), ty_long), tok);
    else if (size != 1) off = new_binary(ND_MUL, off, new_long(size, tok), tok);
    add_type(off);
    Node *n = new_binary(ND_ADD, l, off, tok);
    n->ty = pt;
    return n;
}

static Node *new_sub(Node *l, Node *r, Token *tok)
{
    if (is_vec(l) || is_vec(r)) return vec_binop(ND_SUB, l, r, tok);
    add_type(l);
    add_type(r);
    int lp = l->ty->kind == TY_PTR || l->ty->kind == TY_ARRAY, rp = r->ty->kind == TY_PTR || r->ty->kind == TY_ARRAY;
    if (!lp && !rp) return new_binary(ND_SUB, l, r, tok);
    if (lp && !rp) {
        Type *pt = l->ty->kind == TY_ARRAY ? pointer_to(l->ty->base) : l->ty;
        int size = pt->base->kind == TY_VOID ? 1 : pt->base->size;
        Node *off = new_cast(r, ty_long);
        if (pt->base->is_vla) off = new_binary(ND_MUL, off, new_cast(new_var_node(pt->base->vla_size, tok), ty_long), tok);
        else if (size != 1) off = new_binary(ND_MUL, off, new_long(size, tok), tok);
        add_type(off);
        Node *n = new_binary(ND_SUB, l, off, tok);
        n->ty = pt;
        return n;
    }
    if (lp && rp) {                                   /* the number of elements between */
        Node *n = new_binary(ND_SUB, l, r, tok);
        n->ty = ty_long;
        Type *b = l->ty->base;
        int size = b->kind == TY_VOID ? 1 : b->size;
        if (b->is_vla) { Node *d = new_binary(ND_DIV, n, new_cast(new_var_node(b->vla_size, tok), ty_long), tok); add_type(d); return d; }
        if (size == 1) return n;
        Node *d = new_binary(ND_DIV, n, new_long(size, tok), tok);
        add_type(d);
        return d;
    }
    error_at(tok, "invalid operands to '-'");
}

static int is_lvalue(Node *n)
{
    return n->kind == ND_VAR || n->kind == ND_DEREF || n->kind == ND_MEMBER;
}

static Node *new_assign(Node *l, Node *r, Token *tok)
{
    add_type(l);
    add_type(r);
    if (!is_lvalue(l)) error_at(tok, "not assignable");
    if ((l->ty->kind == TY_STRUCT || l->ty->kind == TY_UNION) && !is_compatible(l->ty, r->ty) && l->ty->members != r->ty->members)
        error_at(tok, "assigning a different type");
    Node *n = new_binary(ND_ASSIGN, l, r, tok);
    add_type(n);
    return n;
}

/* x op= y, and ++/-- (post: the value is the old one) */
static Node *compound(int op, Node *l, Node *r, Token *tok, int post)
{
    add_type(l);
    add_type(r);
    if (!is_lvalue(l)) error_at(tok, "not assignable");
    if (l->ty->is_vector) return new_assign(l, vec_binop(op, l, r, tok), tok);   /* (l: evaluated twice) */
    if (l->ty->is_atomic && cur_fn && r->kind != ND_NUM && !(r->kind == ND_VAR && r->var->is_local)) {    /* (tmp = r, l op= tmp): r once, the update may retry */
        Obj *t = new_lvar(0, r->ty->kind == TY_ARRAY ? pointer_to(r->ty->base) : r->ty);
        Node *set = new_assign(new_var_node(t, tok), r, tok);
        Node *n = new_binary(ND_COMMA, set, compound(op, l, new_var_node(t, tok), tok, post), tok);
        add_type(n);
        return n;
    }
    Node *self = new_node(ND_SELF, tok);
    self->ty = l->ty;
    Node *v;
    if (op == ND_ADD) v = new_add(self, r, tok);
    else if (op == ND_SUB) v = new_sub(self, r, tok);
    else v = new_binary(op, self, r, tok);
    add_type(v);
    Node *n = new_binary(ND_ASSIGN, l, new_cast(v, l->ty), tok);
    n->val = post ? 2 : 1;
    n->ty = l->ty;
    return n;
}

/* ---- Attributes: __attribute__((...)) */
static Token *attributes(Token *tok, VarAttr *a)
{
    while (tok->kw == K_ATTRIBUTE) {
        tok = skip(tok->next, '(');
        tok = skip(tok, '(');
        while (!tok_is(tok, ')')) {
            if (tok->kind != TK_IDENT && tok->kind != TK_PUNCT) error_at(tok, "expected an attribute");
            Token *name = tok;
            tok = tok->next;
            int len = name->len;
            const char *s = name->loc;
            if (len > 4 && s[0] == '_' && s[1] == '_' && s[len - 1] == '_' && s[len - 2] == '_') { s += 2; len -= 4; }
#define IS(x) (len == (int)sizeof(x) - 1 && !strncmp(s, x, len))
            if (tok_is(tok, '(')) {
                Token *arg = tok->next;
                if (IS("aligned")) {
                    Node *e = conditional(&tok, arg);
                    if (a) a->aligned = const_eval(e);
                    tok = skip(tok, ')');
                } else if (IS("vector_size")) {
                    Node *e = conditional(&tok, arg);
                    if (a) a->vecsize = const_eval(e);
                    tok = skip(tok, ')');
                } else if (IS("section") && arg->kind == TK_STR) {
                    if (a) a->section = xstrndup(arg->loc + 1, arg->len - 2);
                    tok = skip(arg->next, ')');
                } else {                            /* arguments we do not need: skip them */
                    int depth = 0;
                    do {
                        if (tok->kind == TK_EOF) error_at(name, "unterminated attribute");
                        if (tok_is(tok, '(')) depth++;
                        if (tok_is(tok, ')')) depth--;
                        tok = tok->next;
                    } while (depth > 0);
                }
            } else if (a) {
                if (IS("packed")) a->packed = 1;
                else if (IS("aligned")) a->aligned = 16;
                else if (IS("noreturn")) a->is_noreturn = 1;
                else if (IS("used")) a->used = 1;
                else if (IS("noinline") || IS("__noinline__")) a->noinline = 1;
                else if (IS("always_inline") || IS("__always_inline__")) a->always_inline = 1;
                else if (IS("weak")) a->weak = 1;
            }
#undef IS
            if (!tok_is(tok, ',')) break;
            tok = tok->next;
        }
        tok = skip(tok, ')');
        tok = skip(tok, ')');
    }
    return tok;
}

/* asm("name") after a declarator: a register variable, or the symbol's name */
static Token *asm_label(Token *tok, const char **out)
{
    if (tok->kw != K_ASM) return tok;
    tok = skip(tok->next, '(');
    if (tok->kind != TK_STR) error_at(tok, "expected a string");
    *out = xstrndup(tok->loc + 1, tok->len - 2);
    return skip(tok->next, ')');
}

/* ---- Constant expressions */
static long truncate_to(long v, Type *t)
{
    if (!is_integer(t) && t->kind != TY_PTR) return v;
    switch (t->size) {
    case 1: return t->kind == TY_BOOL ? v != 0 : t->is_unsigned ? (unsigned char)v : (signed char)v;
    case 2: return t->is_unsigned ? (unsigned short)v : (short)v;
    case 4: return t->is_unsigned ? (long)(unsigned)v : (int)v;
    }
    return v;
}

static int uns(Node *n) { return n->ty && (n->ty->is_unsigned || n->ty->kind == TY_PTR); }

long eval_addr(Node *n, const char **label)
{
    add_type(n);
    if (is_flonum(n->ty)) return (long)eval_double(n);
    long l, r;
    switch (n->kind) {
    case ND_NUM: return n->val;
    case ND_ADD: return truncate_to(eval_addr(n->lhs, label) + const_eval(n->rhs), n->ty);
    case ND_SUB:
        if ((n->lhs->ty->kind == TY_PTR || n->lhs->ty->kind == TY_ARRAY) && (n->rhs->ty->kind == TY_PTR || n->rhs->ty->kind == TY_ARRAY)) {
            const char *la = 0, *lb = 0;          /* two addresses in the same object */
            long x = eval_addr(n->lhs, &la), y = eval_addr(n->rhs, &lb);
            if (la != lb) error_at(n->tok, "not a compile-time constant");
            return x - y;
        }
        return truncate_to(eval_addr(n->lhs, label) - const_eval(n->rhs), n->ty);
    case ND_MUL: return truncate_to(const_eval(n->lhs) * const_eval(n->rhs), n->ty);
    case ND_DIV: case ND_MOD:
        l = const_eval(n->lhs);
        r = const_eval(n->rhs);
        if (!r) error_at(n->tok, "division by zero");
        if (uns(n)) return truncate_to(n->kind == ND_DIV ? (long)((unsigned long)l / r) : (long)((unsigned long)l % r), n->ty);
        return truncate_to(n->kind == ND_DIV ? l / r : l % r, n->ty);
    case ND_AND: return const_eval(n->lhs) & const_eval(n->rhs);
    case ND_OR: return const_eval(n->lhs) | const_eval(n->rhs);
    case ND_XOR: return const_eval(n->lhs) ^ const_eval(n->rhs);
    case ND_SHL: return truncate_to((unsigned long)const_eval(n->lhs) << (const_eval(n->rhs) & 63), n->ty);
    case ND_SHR:
        l = const_eval(n->lhs);
        r = const_eval(n->rhs) & 63;
        if (uns(n)) return truncate_to((long)((unsigned long)(n->ty->size == 4 ? (unsigned)l : (unsigned long)l) >> r), n->ty);
        return l >> r;
    case ND_EQ: return const_eval(n->lhs) == const_eval(n->rhs);
    case ND_NE: return const_eval(n->lhs) != const_eval(n->rhs);
    case ND_LT: case ND_LE:
        if (is_flonum(n->lhs->ty)) return n->kind == ND_LT ? eval_double(n->lhs) < eval_double(n->rhs) : eval_double(n->lhs) <= eval_double(n->rhs);
        l = const_eval(n->lhs);
        r = const_eval(n->rhs);
        if (uns(n->lhs)) return n->kind == ND_LT ? (unsigned long)l < (unsigned long)r : (unsigned long)l <= (unsigned long)r;
        return n->kind == ND_LT ? l < r : l <= r;
    case ND_COND: return const_eval(n->cond) ? eval_addr(n->then, label) : eval_addr(n->els, label);
    case ND_COMMA: return eval_addr(n->rhs, label);
    case ND_NEG: return truncate_to(-const_eval(n->lhs), n->ty);
    case ND_NOT: return is_flonum(n->lhs->ty) ? !eval_double(n->lhs) : !const_eval(n->lhs);
    case ND_BITNOT: return truncate_to(~const_eval(n->lhs), n->ty);
    case ND_LOGAND: return const_eval(n->lhs) && const_eval(n->rhs);
    case ND_LOGOR: return const_eval(n->lhs) || const_eval(n->rhs);
    case ND_CAST: {
        if (is_flonum(n->lhs->ty)) {
            double d = eval_double(n->lhs);
            if (n->ty->kind == TY_BOOL) return d != 0;
            return truncate_to(n->ty->is_unsigned && n->ty->size == 8 ? (long)(unsigned long)d : (long)d, n->ty);
        }
        long v = eval_addr(n->lhs, label);
        if (is_integer(n->ty) && n->ty->size < 8 && label && *label) error_at(n->tok, "an address does not fit");
        return truncate_to(v, n->ty);
    }
    case ND_ADDR: return eval_rval(n->lhs, label);
    case ND_MEMBER:
        if (n->ty->kind != TY_ARRAY) break;
        return eval_rval(n, label);
    case ND_DEREF:
        if (n->ty->kind != TY_ARRAY && n->ty->kind != TY_FUNC) break;
        return eval_addr(n->lhs, label);
    case ND_LABEL_VAL:                                 /* static void *t[] = { &&a, &&b } */
        if (!label) error_at(n->tok, "not a compile-time constant");
        *label = n->label;
        return 0;
    case ND_VAR:
        if (n->var->ty->kind != TY_ARRAY && n->var->ty->kind != TY_FUNC) break;
        if (!label) error_at(n->tok, "not a compile-time constant");
        if (n->var->is_local || n->var->is_tls) break;
        *label = n->var->asm_name;
        return 0;
    }
    error_at(n->tok, "not a compile-time constant");
}

static long eval_rval(Node *n, const char **label)
{
    switch (n->kind) {
    case ND_VAR:
        if (n->var->is_local || n->var->is_tls) error_at(n->tok, "not a compile-time constant");
        if (!label) error_at(n->tok, "an address is not an integer constant");
        *label = n->var->asm_name;
        return 0;
    case ND_DEREF: return eval_addr(n->lhs, label);
    case ND_MEMBER: return eval_rval(n->lhs, label) + n->member->offset;
    }
    error_at(n->tok, "not a compile-time constant");
}

long const_eval(Node *n) { return eval_addr(n, 0); }

int is_const_expr(Node *n)
{
    add_type(n);
    switch (n->kind) {
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD: case ND_AND: case ND_OR: case ND_XOR:
    case ND_SHL: case ND_SHR: case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_LOGAND: case ND_LOGOR:
        return is_const_expr(n->lhs) && is_const_expr(n->rhs);
    case ND_COND:
        if (!is_const_expr(n->cond)) return 0;
        return is_const_expr(const_eval(n->cond) ? n->then : n->els);
    case ND_COMMA: return is_const_expr(n->rhs);
    case ND_NEG: case ND_NOT: case ND_BITNOT: return is_const_expr(n->lhs);
    case ND_CAST: return is_scalar(n->ty) && is_const_expr(n->lhs);
    case ND_NUM: return 1;
    }
    return 0;
}

double eval_double(Node *n)
{
    add_type(n);
    if (n->ty->kind == TY_COMPLEX || (n->kind == ND_CAST && n->lhs->ty && n->lhs->ty->kind == TY_COMPLEX)) return eval_ld(n);
    if (n->ty->kind == TY_LDOUBLE || (n->kind == ND_CAST && n->lhs->ty && n->lhs->ty->kind == TY_LDOUBLE)) return n->ty->kind == TY_FLOAT ? (float)eval_ld(n) : (double)eval_ld(n);
    if (is_integer(n->ty)) return n->ty->is_unsigned ? (double)(unsigned long)const_eval(n) : (double)const_eval(n);
    switch (n->kind) {
    case ND_ADD: return eval_double(n->lhs) + eval_double(n->rhs);
    case ND_SUB: return eval_double(n->lhs) - eval_double(n->rhs);
    case ND_MUL: return eval_double(n->lhs) * eval_double(n->rhs);
    case ND_DIV: return eval_double(n->lhs) / eval_double(n->rhs);
    case ND_NEG: return -eval_double(n->lhs);
    case ND_COND: return const_eval(n->cond) ? eval_double(n->then) : eval_double(n->els);
    case ND_COMMA: return eval_double(n->rhs);
    case ND_CAST:
        if (is_flonum(n->lhs->ty)) return n->ty->kind == TY_FLOAT ? (float)eval_double(n->lhs) : eval_double(n->lhs);
        return n->lhs->ty->is_unsigned ? (double)(unsigned long)const_eval(n->lhs) : (double)const_eval(n->lhs);
    case ND_NUM: return n->fval;
    }
    error_at(n->tok, "not a compile-time constant");
}

/* A constant of type __int128: the low 64 bits, and *hi the upper ones
 * (computed on two 64-bit halves). */
static unsigned long mulhi(unsigned long a, unsigned long b)    /* the high half of a 64x64 product */
{
    unsigned long a0 = a & 0xffffffff, a1 = a >> 32, b0 = b & 0xffffffff, b1 = b >> 32;
    unsigned long p01 = a0 * b1, p10 = a1 * b0;
    unsigned long mid = (a0 * b0 >> 32) + (p01 & 0xffffffff) + (p10 & 0xffffffff);
    return a1 * b1 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

static unsigned long eval128(Node *n, unsigned long *hi)
{
    add_type(n);
    if (n->ty->kind != TY_INT128) {                /* a narrower value: extended as its type says */
        long v = const_eval(n);
        *hi = !n->ty->is_unsigned && v < 0 ? ~0UL : 0;
        return v;
    }
    unsigned long al, ah, bl = 0, bh = 0, lo;
    switch (n->kind) {
    case ND_NUM: *hi = n->val < 0 ? ~0UL : 0; return n->val;
    case ND_CAST: return eval128(n->lhs, hi);
    case ND_NEG: al = eval128(n->lhs, &ah); *hi = ~ah + (al == 0); return -al;
    case ND_BITNOT: al = eval128(n->lhs, &ah); *hi = ~ah; return ~al;
    case ND_SHL: case ND_SHR: {
        al = eval128(n->lhs, &ah);
        int k = const_eval(n->rhs) & 127;
        if (!k) { *hi = ah; return al; }
        if (n->kind == ND_SHL) {
            if (k >= 64) { *hi = al << (k - 64); return 0; }
            *hi = ah << k | al >> (64 - k);
            return al << k;
        }
        unsigned long fill = !n->ty->is_unsigned && (long)ah < 0 ? ~0UL : 0;
        if (k >= 64) { *hi = fill; return k == 64 ? ah : (ah >> (k - 64)) | (fill << (128 - k)); }
        *hi = ah >> k | (fill << (64 - k));
        return al >> k | ah << (64 - k);
    }
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_AND: case ND_OR: case ND_XOR:
        al = eval128(n->lhs, &ah);
        bl = eval128(n->rhs, &bh);
        switch (n->kind) {
        case ND_ADD: lo = al + bl; *hi = ah + bh + (lo < al); return lo;
        case ND_SUB: *hi = ah - bh - (al < bl); return al - bl;
        case ND_MUL: *hi = mulhi(al, bl) + al * bh + ah * bl; return al * bl;
        case ND_AND: *hi = ah & bh; return al & bl;
        case ND_OR: *hi = ah | bh; return al | bl;
        default: *hi = ah ^ bh; return al ^ bl;
        }
    case ND_DIV: case ND_MOD: {                    /* long division, one bit at a time */
        al = eval128(n->lhs, &ah);
        bl = eval128(n->rhs, &bh);
        if (!bl && !bh) error_at(n->tok, "division by zero");
        int neg_a = !n->ty->is_unsigned && (long)ah < 0, neg_b = !n->ty->is_unsigned && (long)bh < 0;
        if (neg_a) { ah = ~ah + (al == 0); al = -al; }
        if (neg_b) { bh = ~bh + (bl == 0); bl = -bl; }
        unsigned long ql = 0, qh = 0, rl = 0, rh = 0;
        for (int i = 127; i >= 0; i--) {
            rh = rh << 1 | rl >> 63;
            rl = rl << 1 | ((i >= 64 ? ah >> (i - 64) : al >> i) & 1);
            if (rh > bh || (rh == bh && rl >= bl)) {
                rh = rh - bh - (rl < bl);
                rl -= bl;
                if (i >= 64) qh |= 1UL << (i - 64); else ql |= 1UL << i;
            }
        }
        if (n->kind == ND_DIV) {
            if (neg_a != neg_b) { qh = ~qh + (ql == 0); ql = -ql; }
            *hi = qh;
            return ql;
        }
        if (neg_a) { rh = ~rh + (rl == 0); rl = -rl; }
        *hi = rh;
        return rl;
    }
    case ND_COND: return const_eval(n->cond) ? eval128(n->then, hi) : eval128(n->els, hi);
    case ND_COMMA: return eval128(n->rhs, hi);
    }
    error_at(n->tok, "not a compile-time constant");
}

/* A complex constant: its real and imaginary parts. */
void eval_complex(Node *n, long double *re, long double *im)
{
    add_type(n);
    if (n->ty->kind != TY_COMPLEX) { *re = eval_ld(n); *im = 0; return; }
    long double a, b, c, d;
    switch (n->kind) {
    case ND_NUM: *re = n->fval; *im = n->ldval; return;
    case ND_CAST: eval_complex(n->lhs, re, im); break;
    case ND_NEG: eval_complex(n->lhs, &a, &b); *re = -a; *im = -b; break;
    case ND_BITNOT: eval_complex(n->lhs, &a, &b); *re = a; *im = -b; break;
    case ND_ADD: eval_complex(n->lhs, &a, &b); eval_complex(n->rhs, &c, &d); *re = a + c; *im = b + d; break;
    case ND_SUB: eval_complex(n->lhs, &a, &b); eval_complex(n->rhs, &c, &d); *re = a - c; *im = b - d; break;
    case ND_MUL: eval_complex(n->lhs, &a, &b); eval_complex(n->rhs, &c, &d); *re = a * c - b * d; *im = a * d + b * c; break;
    case ND_COND: if (const_eval(n->cond)) eval_complex(n->then, re, im); else eval_complex(n->els, re, im); break;
    case ND_COMMA: eval_complex(n->rhs, re, im); break;
    default: error_at(n->tok, "not a compile-time constant");
    }
    Type *b0 = n->ty->base;                        /* rounded to the parts' type */
    if (b0->kind == TY_FLOAT) { *re = (float)*re; *im = (float)*im; }
    if (b0->kind == TY_DOUBLE) { *re = (double)*re; *im = (double)*im; }
}

/* A constant of type long double, computed in long double. */
long double eval_ld(Node *n)
{
    add_type(n);
    if (n->ty->kind == TY_COMPLEX) { long double re, im; eval_complex(n, &re, &im); return re; }
    if (is_integer(n->ty)) return n->ty->is_unsigned ? (long double)(unsigned long)const_eval(n) : (long double)const_eval(n);
    switch (n->kind) {
    case ND_ADD: return eval_ld(n->lhs) + eval_ld(n->rhs);
    case ND_SUB: return eval_ld(n->lhs) - eval_ld(n->rhs);
    case ND_MUL: return eval_ld(n->lhs) * eval_ld(n->rhs);
    case ND_DIV: return eval_ld(n->lhs) / eval_ld(n->rhs);
    case ND_NEG: return -eval_ld(n->lhs);
    case ND_COND: return const_eval(n->cond) ? eval_ld(n->then) : eval_ld(n->els);
    case ND_COMMA: return eval_ld(n->rhs);
    case ND_CAST:
        if (n->ty->kind == TY_FLOAT) return (float)eval_ld(n->lhs);
        if (n->ty->kind == TY_DOUBLE) return (double)eval_ld(n->lhs);
        return eval_ld(n->lhs);
    case ND_NUM: return n->ty->kind == TY_LDOUBLE ? n->ldval : n->fval;
    }
    error_at(n->tok, "not a compile-time constant");
}

static long const_expr(Token **rest, Token *tok) { return const_eval(conditional(rest, tok)); }

/* ---- Objects */
static Obj *new_var(const char *name, Type *ty)
{
    Obj *o = arena(sizeof *o);
    o->name = name;
    o->asm_name = name;
    o->ty = ty;
    o->align = ty->align;
    if (name) push_sym(name)->var = o;
    return o;
}

static Obj *new_lvar(const char *name, Type *ty)
{
    Obj *o = new_var(name, ty);
    o->is_local = 1;
    o->next = cur_locals;
    cur_locals = o;
    return o;
}

static Obj *new_gvar(const char *name, Type *ty)
{
    Obj *o = new_var(name, ty);
    *globals_tail = o;
    globals_tail = &o->next;
    return o;
}

static Obj *new_anon_gvar(Type *ty)
{
    const char *name = intern(fmt(".L.anon.%d", anon_n), strlen(fmt(".L.anon.%d", anon_n)));
    anon_n++;
    Obj *o = arena(sizeof *o);
    o->name = o->asm_name = name;
    o->ty = ty;
    o->align = ty->align;
    o->is_static = o->is_def = 1;
    *globals_tail = o;
    globals_tail = &o->next;
    return o;
}

/* String literals: one global per distinct contents. */
static Map strlits;
static Obj *new_string(char *p, int len, Type *ty)
{
    const char *key = intern(fmt("%d:", ty->base->size), strlen(fmt("%d:", ty->base->size)));
    char *k = arena(len + 8);
    memcpy(k, key, strlen(key));
    memcpy(k + strlen(key), p, len);
    const char *ik = intern(k, strlen(key) + len);
    Obj *o = map_get(&strlits, ik);
    if (o) return o;
    o = new_anon_gvar(ty);
    o->data = p;
    o->has_init = 1;
    o->is_str = 1;
    map_put(&strlits, ik, o);
    return o;
}

/* ---- Literals */
static int hexval(int c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

static void put_utf8(Buf *b, unsigned c)
{
    if (c < 0x80) buf_c(b, c);
    else if (c < 0x800) { buf_c(b, 0xC0 | c >> 6); buf_c(b, 0x80 | (c & 63)); }
    else if (c < 0x10000) { buf_c(b, 0xE0 | c >> 12); buf_c(b, 0x80 | (c >> 6 & 63)); buf_c(b, 0x80 | (c & 63)); }
    else { buf_c(b, 0xF0 | c >> 18); buf_c(b, 0x80 | (c >> 12 & 63)); buf_c(b, 0x80 | (c >> 6 & 63)); buf_c(b, 0x80 | (c & 63)); }
}

/* One character of a literal, escapes decoded; *wide: \u/\U code point */
static unsigned read_escape(const char **pp, Token *tok, int *uni)
{
    const char *p = *pp;
    *uni = 0;
    if (*p != '\\') {                                   /* a UTF-8 sequence is kept as bytes */
        *pp = p + 1;
        return (unsigned char)*p;
    }
    p++;
    unsigned c;
    if (*p >= '0' && *p <= '7') {
        c = *p++ - '0';
        if (*p >= '0' && *p <= '7') c = c * 8 + *p++ - '0';
        if (*p >= '0' && *p <= '7') c = c * 8 + *p++ - '0';
        *pp = p;
        return c;
    }
    if (*p == 'x') {
        p++;
        if (!((*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'f'))) error_at(tok, "invalid \\x escape");
        for (c = 0; (*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'f'); p++) c = c * 16 + hexval(*p);
        *pp = p;
        return c;
    }
    if (*p == 'u' || *p == 'U') {
        int n = *p == 'u' ? 4 : 8;
        c = 0;
        for (p++; n--; p++) c = c * 16 + hexval(*p);
        *pp = p;
        *uni = 1;
        return c;
    }
    *pp = p + 1;
    switch (*p) {
    case 'a': return 7;
    case 'b': return 8;
    case 't': return 9;
    case 'n': return 10;
    case 'v': return 11;
    case 'f': return 12;
    case 'r': return 13;
    case 'e': return 27;
    default: return (unsigned char)*p;
    }
}

/* A string literal (adjacent ones joined): its bytes, and its type. */
static Token *string_literal(Token *tok, char **out, int *outlen, Type **ty)
{
    int esize = 1;
    Type *base = ty_char;
    Buf b = { 0 };
    for (; tok->kind == TK_STR; tok = tok->next) {
        const char *p = tok->loc;
        if (*p == 'u' && p[1] == '8') p += 2;
        else if (*p == 'u') { esize = 2; base = ty_ushort; p++; }
        else if (*p == 'U') { esize = 4; base = ty_uint; p++; }
        else if (*p == 'L') { esize = 4; base = ty_int; p++; }
        p++;
        const char *end = tok->loc + tok->len - 1;
        while (p < end) {
            int uni;
            unsigned c = read_escape(&p, tok, &uni);
            if (esize == 1) { if (uni) put_utf8(&b, c); else buf_c(&b, c); }
            else {
                if (!uni && c >= 0x80 && p[-1] != '\\') {      /* decode UTF-8 for wide strings */
                    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
                    c &= n == 3 ? 7 : n == 2 ? 15 : 31;
                    while (n-- && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 63);
                }
                if (esize == 2 && c > 0xFFFF) {
                    c -= 0x10000;
                    unsigned hi = 0xD800 + (c >> 10), lo = 0xDC00 + (c & 0x3FF);
                    buf_add(&b, &hi, 2);
                    buf_add(&b, &lo, 2);
                } else buf_add(&b, &c, esize);
            }
        }
    }
    buf_zero(&b, esize);
    *out = arena(b.len);
    memcpy(*out, b.p, b.len);
    *outlen = b.len;
    *ty = array_of(base, b.len / esize);
    free(b.p);
    return tok;
}

static Node *number(Token *tok)
{
    const char *p = tok->loc;
    int len = tok->len;
    /* floating point? */
    int is_float = 0;
    if (!(len > 1 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))) {
        for (int i = 0; i < len; i++) if (p[i] == '.' || p[i] == 'e' || p[i] == 'E') is_float = 1;
    } else for (int i = 0; i < len; i++) if (p[i] == '.' || p[i] == 'p' || p[i] == 'P') is_float = 1;
    if (is_float) {
        char *s = xstrndup(p, len);
        char *end;
        long double ld = strtold(s, &end);
        double d = strtod(s, 0);
        Type *ty = ty_double;
        int imag = 0;                              /* GNU 2.0i: an imaginary constant */
        for (; *end; end++) {
            if ((*end == 'f' || *end == 'F') && ty == ty_double) { ty = ty_float; d = (float)d; }
            else if ((*end == 'l' || *end == 'L') && ty == ty_double) ty = ty_ldouble;
            else if ((*end == 'i' || *end == 'I' || *end == 'j' || *end == 'J') && !imag) imag = 1;
            else break;
        }
        if (end != s + len) error_at(tok, "invalid number");
        Node *n = new_node(ND_NUM, tok);
        n->fval = d;
        n->ldval = ty == ty_ldouble ? ld : d;
        n->ty = imag ? complex_of(ty) : ty;
        if (imag) n->fval = 0;                     /* (fval: the real part) */
        return n;
    }
    int base = 10;
    const char *q = p;
    if (len > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; q += 2; }
    else if (len > 2 && p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; q += 2; }
    else if (p[0] == '0') base = 8;
    unsigned long v = 0;
    const char *end = p + len;
    for (; q < end; q++) {
        int d;
        if (*q >= '0' && *q <= '9') d = *q - '0';
        else if ((*q | 32) >= 'a' && (*q | 32) <= 'f' && base == 16) d = (*q | 32) - 'a' + 10;
        else break;
        if (d >= base) error_at(tok, "invalid digit in a number");
        v = v * base + d;
    }
    int l = 0, u = 0;
    for (; q < end; q++) {
        if (*q == 'u' || *q == 'U') u = 1;
        else if (*q == 'l' || *q == 'L') l++;
        else error_at(tok, "invalid number suffix");
    }
    Type *ty;
    if (base == 10) {
        if (u) ty = (l || v >> 32) ? ty_ulong : ty_uint;
        else if (l || v >> 31) ty = ty_long;
        else ty = ty_int;
    } else {
        if (l) ty = (u || v >> 63) ? ty_ulong : ty_long;
        else if (u) ty = v >> 32 ? ty_ulong : ty_uint;
        else if (v >> 63) ty = ty_ulong;
        else if (v >> 32) ty = ty_long;
        else if (v >> 31) ty = ty_uint;
        else ty = ty_int;
    }
    if (l == 2 && ty->kind == TY_LONG) { ty = copy_type(ty); ty->is_llong = 1; }
    Node *n = new_node(ND_NUM, tok);
    n->val = v;
    n->ty = ty;
    return n;
}

static Node *char_literal(Token *tok)
{
    const char *p = tok->loc;
    Type *ty = ty_int;
    int wide = 0;
    if (*p == 'u' && p[1] == '8') p += 2;
    else if (*p == 'u') { ty = ty_ushort; wide = 1; p++; }
    else if (*p == 'U') { ty = ty_uint; wide = 1; p++; }
    else if (*p == 'L') { ty = ty_int; wide = 1; p++; }
    p++;
    long v = 0;
    const char *end = tok->loc + tok->len - 1;
    int n = 0;
    while (p < end) {
        int uni;
        unsigned c = read_escape(&p, tok, &uni);
        if (wide && !uni && c >= 0x80) {
            int k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            c &= k == 3 ? 7 : k == 2 ? 15 : 31;
            while (k-- && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 63);
        }
        v = wide ? (long)c : (n == 0 ? (long)(signed char)c : v << 8 | (c & 255));
        n++;
    }
    if (n == 0) error_at(tok, "empty character constant");
    Node *r = new_node(ND_NUM, tok);
    r->val = truncate_to(v, ty);
    r->ty = ty;
    return r;
}

/* ---- Declaration specifiers */
static int is_typename(Token *t)
{
    switch (t->kw) {
    case K_VOID: case K_CHAR: case K_SHORT: case K_INT: case K_LONG: case K_FLOAT: case K_DOUBLE:
    case K_SIGNED: case K_UNSIGNED: case K_BOOL: case K_STRUCT: case K_UNION: case K_ENUM:
    case K_TYPEDEF: case K_EXTERN: case K_STATIC: case K_AUTO: case K_REGISTER: case K_INLINE:
    case K_CONST: case K_VOLATILE: case K_RESTRICT: case K_ALIGNAS: case K_NORETURN:
    case K_ATTRIBUTE: case K_TYPEOF: case K_VA_LIST: case K_THREAD_LOCAL: case K_ATOMIC:
    case K_COMPLEX: case K_INT128:
        return 1;
    }
    return find_typedef(t) != 0;
}

static Type *struct_decl(Token **rest, Token *tok, int kind);
static Type *enum_decl(Token **rest, Token *tok);

static Type *qualify(Type *t, int c, int v)
{
    if (!c && !v) return t;
    if (t->kind == TY_STRUCT || t->kind == TY_UNION || t->kind == TY_ARRAY || t->kind == TY_FUNC) return t;
    t = copy_type(t);
    if (c) t->is_const = 1;
    if (v) t->is_volatile = 1;
    return t;
}

static Type *make_atomic(Type *t, Token *tok)
{
    if (t->kind == TY_ARRAY || t->kind == TY_FUNC) error_at(tok, "_Atomic of an array or a function");
    if (t->size > 8 && t->kind != TY_STRUCT && t->kind != TY_UNION) error_at(tok, "atomic objects larger than 8 bytes are not supported");
    t = copy_type(t);
    t->is_atomic = 1;
    return t;
}

/* va_list. System V: an array of one struct {gp_offset, fp_offset,
 * overflow_arg_area, reg_save_area}. AAPCS64: a struct {__stack, __gr_top,
 * __vr_top, __gr_offs, __vr_offs} (the offsets are negative, from the tops of
 * the saved argument registers). */
static Type *va_list_type(void)
{
    static Type *t;
    if (t) return t;
    int a64 = target == T_AARCH64;
    Type *s = new_type(TY_STRUCT, a64 ? 32 : 24, 8);
    Member *m[5];
    static const char *names[4] = { "gp_offset", "fp_offset", "overflow_arg_area", "reg_save_area" };
    static const char *names64[5] = { "__stack", "__gr_top", "__vr_top", "__gr_offs", "__vr_offs" };
    Type *tys[5] = { ty_uint, ty_uint, pointer_to(ty_void), pointer_to(ty_void) };
    int offs[5] = { 0, 4, 8, 16 };
    if (a64) {
        Type *t64[5] = { pointer_to(ty_void), pointer_to(ty_void), pointer_to(ty_void), ty_int, ty_int };
        int o64[5] = { 0, 8, 16, 24, 28 };
        for (int i = 0; i < 5; i++) {
            m[i] = arena(sizeof(Member));
            m[i]->ty = t64[i];
            m[i]->name = intern(names64[i], strlen(names64[i]));
            m[i]->offset = o64[i];
            m[i]->align = t64[i]->align;
            if (i) m[i - 1]->next = m[i];
        }
        s->members = m[0];
        return t = s;
    }
    for (int i = 0; i < 4; i++) {
        m[i] = arena(sizeof(Member));
        m[i]->ty = tys[i];
        m[i]->name = intern(names[i], strlen(names[i]));
        m[i]->offset = offs[i];
        m[i]->align = tys[i]->align;
        if (i) m[i - 1]->next = m[i];
    }
    s->members = m[0];
    return t = array_of(s, 1);
}

static Type *declspec(Token **rest, Token *tok, VarAttr *a)
{
    enum { VOID = 1 << 0, BOOL = 1 << 2, CHAR = 1 << 4, SHORT = 1 << 6, INT = 1 << 8, LONG = 1 << 10,
           FLOAT = 1 << 12, DOUBLE = 1 << 14, OTHER = 1 << 16, SIGNED = 1 << 17, UNSIGNED = 1 << 18, INT128 = 1 << 19 };
    Type *ty = ty_int;
    int counter = 0, is_const = 0, is_volatile = 0, complex = 0, atomic = 0;
    Token *atomic_tok = tok;
    VarAttr dummy = { 0 };
    if (!a) a = &dummy;
    while (is_typename(tok) || tok->kw == K_EXTENSION) {
        int kw = tok->kw;
        if (kw == K_TYPEDEF || kw == K_STATIC || kw == K_EXTERN || kw == K_INLINE || kw == K_NORETURN ||
            kw == K_AUTO || kw == K_REGISTER || kw == K_THREAD_LOCAL) {
            if (a == &dummy && kw != K_REGISTER && kw != K_AUTO) error_at(tok, "a storage class is not allowed here");
            if (kw == K_TYPEDEF) a->is_typedef = 1;
            else if (kw == K_STATIC) a->is_static = 1;
            else if (kw == K_EXTERN) a->is_extern = 1;
            else if (kw == K_INLINE) a->is_inline = 1;
            else if (kw == K_NORETURN) a->is_noreturn = 1;
            else if (kw == K_THREAD_LOCAL) a->is_tls = 1;
            tok = tok->next;
            continue;
        }
        if (kw == K_CONST) { is_const = 1; tok = tok->next; continue; }
        if (kw == K_VOLATILE) { is_volatile = 1; tok = tok->next; continue; }
        if (kw == K_RESTRICT || kw == K_EXTENSION) { tok = tok->next; continue; }
        if (kw == K_ATTRIBUTE) { tok = attributes(tok, a); continue; }
        if (kw == K_ATOMIC) {
            atomic_tok = tok;
            if (tok_is(tok->next, '(')) {                   /* _Atomic(type) */
                if (counter) break;
                ty = typename(&tok, tok->next->next);
                tok = skip(tok, ')');
                counter += OTHER;
            } else tok = tok->next;                        /* the qualifier */
            atomic = 1;
            continue;
        }
        if (kw == K_COMPLEX) { complex = 1; tok = tok->next; if (!counter) ty = ty_double; continue; }
        if (kw == K_ALIGNAS) {
            tok = skip(tok->next, '(');
            if (is_typename(tok)) a->align = typename(&tok, tok)->align;
            else a->align = const_expr(&tok, tok);
            tok = skip(tok, ')');
            continue;
        }
        Type *td = find_typedef(tok);
        if (kw == K_STRUCT || kw == K_UNION || kw == K_ENUM || kw == K_TYPEOF || kw == K_VA_LIST || td) {
            if (counter) break;                        /* a typedef name used as a declarator */
            if (kw == K_STRUCT) ty = struct_decl(&tok, tok->next, TY_STRUCT);
            else if (kw == K_UNION) ty = struct_decl(&tok, tok->next, TY_UNION);
            else if (kw == K_ENUM) ty = enum_decl(&tok, tok->next);
            else if (kw == K_VA_LIST) { ty = va_list_type(); tok = tok->next; }
            else if (kw == K_TYPEOF) {
                tok = skip(tok->next, '(');
                if (is_typename(tok)) ty = typename(&tok, tok);
                else { Node *e = expr(&tok, tok); add_type(e); ty = e->ty; }
                tok = skip(tok, ')');
            } else { ty = td; tok = tok->next; }
            counter += OTHER;
            continue;
        }
        switch (kw) {
        case K_VOID: counter += VOID; break;
        case K_BOOL: counter += BOOL; break;
        case K_CHAR: counter += CHAR; break;
        case K_SHORT: counter += SHORT; break;
        case K_INT: counter += INT; break;
        case K_LONG: counter += LONG; break;
        case K_FLOAT: counter += FLOAT; break;
        case K_DOUBLE: counter += DOUBLE; break;
        case K_SIGNED: counter |= SIGNED; break;
        case K_UNSIGNED: counter |= UNSIGNED; break;
        case K_INT128: counter += INT128; break;
        }
        switch (counter) {
        case VOID: ty = ty_void; break;
        case BOOL: ty = ty_bool; break;
        case CHAR: ty = ty_char; break;
        case SIGNED + CHAR: ty = ty_schar; break;
        case UNSIGNED + CHAR: ty = ty_uchar; break;
        case SHORT: case SHORT + INT: case SIGNED + SHORT: case SIGNED + SHORT + INT: ty = ty_short; break;
        case UNSIGNED + SHORT: case UNSIGNED + SHORT + INT: ty = ty_ushort; break;
        case INT: case SIGNED: case SIGNED + INT: ty = ty_int; break;
        case UNSIGNED: case UNSIGNED + INT: ty = ty_uint; break;
        case LONG: case LONG + INT: case SIGNED + LONG: case SIGNED + LONG + INT: ty = ty_long; break;
        case LONG + LONG: case LONG + LONG + INT: case SIGNED + LONG + LONG: case SIGNED + LONG + LONG + INT:
            ty = copy_type(ty_long); ty->is_llong = 1; break;
        case UNSIGNED + LONG: case UNSIGNED + LONG + INT: ty = ty_ulong; break;
        case UNSIGNED + LONG + LONG: case UNSIGNED + LONG + LONG + INT: ty = copy_type(ty_ulong); ty->is_llong = 1; break;
        case INT128: case SIGNED + INT128: ty = ty_int128; break;
        case UNSIGNED + INT128: ty = ty_uint128; break;
        case FLOAT: ty = ty_float; break;
        case DOUBLE: ty = ty_double; break;
        case LONG + DOUBLE: ty = ty_ldouble; break;
        default: error_at(tok, "invalid type");
        }
        tok = tok->next;
    }
    if (counter == 0 && !is_const && !is_volatile && !complex && a == &dummy) error_at(tok, "expected a type");
    *rest = tok;
    if (complex) {
        if (!is_flonum(ty)) error_at(tok, "_Complex needs float, double or long double");
        ty = complex_of(ty);
    }
    if (atomic) ty = make_atomic(ty, atomic_tok);
    if (a->vecsize) { ty = vector_of(ty, a->vecsize, tok); a->vecsize = 0; }
    return qualify(ty, is_const, is_volatile);
}

/* ---- Vectors (GNU vector_size): a struct of one array, aligned to its size;
 * operators work element by element (ND_VEC) */
Type *vector_of(Type *e, int size, Token *tok)
{
    static Type *made[256];
    static int nmade;
    if (!is_integer(e) && e->kind != TY_FLOAT && e->kind != TY_DOUBLE) error_at(tok, "a vector of a type that is not an integer or floating type");
    if (e->kind == TY_INT128 || e->kind == TY_BOOL || size <= 0 || size % e->size || (size & (size - 1)) || size > 32)
        error_at(tok, "vector_size must be a power of two, up to 32, a multiple of the element size");
    for (int k = 0; k < nmade; k++)
        if (made[k]->size == size && made[k]->base->kind == e->kind && made[k]->base->size == e->size && made[k]->base->is_unsigned == e->is_unsigned) return made[k];
    Member *m = arena(sizeof *m);
    m->ty = array_of(e, size / e->size);
    m->name = intern("__v", 3);
    m->align = e->align;
    Type *t = new_type(TY_STRUCT, size, size > 16 ? 32 : size);
    t->members = m;
    t->base = e;
    t->is_vector = 1;
    if (nmade < 256) made[nmade++] = t;
    return t;
}

static int is_vec(Node *n) { add_type(n); return n->ty->is_vector; }

static Node *vec_node(int op, Node *l, Node *r, Type *ty, Token *tok)
{
    Node *n = new_node(ND_VEC, tok);
    n->val = op; n->lhs = l; n->rhs = r; n->ty = ty;
    return n;
}

/* the mask type of a comparison: signed integers of the element's size */
static Type *vec_mask_type(Type *v)
{
    Type *e = v->base->size == 1 ? ty_schar : v->base->size == 2 ? ty_short : v->base->size == 4 ? ty_int : ty_long;
    return vector_of(e, v->size, 0);
}

static Node *vec_binop(int kind, Node *l, Node *r, Token *op)
{
    Type *vt = is_vec(l) ? l->ty : r->ty;
    int shift = kind == ND_SHL || kind == ND_SHR;
    if (shift && !is_vec(r) && is_const_expr(r)) {     /* by a constant: one instruction */
        if (!is_vec(l) || is_flonum(vt->base)) error_at(op, "invalid vector shift");
        Node *n = vec_node(kind == ND_SHL ? VOP_SHLI : vt->base->is_unsigned ? VOP_SHRI : VOP_SARI, l, 0, vt, op);
        n->hi = const_eval(r);
        return n;
    }
    if (!is_vec(l)) l = vec_node(VOP_SPLAT, new_cast(l, vt->base), 0, vt, op);    /* a scalar: in every element */
    if (!is_vec(r)) r = vec_node(VOP_SPLAT, new_cast(r, vt->base), 0, vt, op);
    if (l->ty->size != r->ty->size || l->ty->base->size != r->ty->base->size || is_flonum(l->ty->base) != is_flonum(r->ty->base))
        error_at(op, "vectors of different types");
    static const int vops[] = { [ND_ADD] = VOP_ADD + 1, [ND_SUB] = VOP_SUB + 1, [ND_MUL] = VOP_MUL + 1, [ND_DIV] = VOP_DIV + 1, [ND_MOD] = VOP_MOD + 1,
        [ND_AND] = VOP_AND + 1, [ND_OR] = VOP_OR + 1, [ND_XOR] = VOP_XOR + 1, [ND_SHL] = VOP_SHL + 1, [ND_SHR] = VOP_SHR + 1,
        [ND_EQ] = VOP_EQ + 1, [ND_NE] = VOP_NE + 1, [ND_LT] = VOP_LT + 1, [ND_LE] = VOP_LE + 1 };
    int v = kind < (int)(sizeof vops / sizeof vops[0]) ? vops[kind] - 1 : -1;
    if (v < 0) error_at(op, "invalid operation on vectors");
    if (is_flonum(vt->base) && (v == VOP_AND || v == VOP_OR || v == VOP_XOR || v == VOP_MOD || shift)) error_at(op, "invalid operation on floating vectors");
    return vec_node(v, l, r, v >= VOP_EQ && v <= VOP_LE ? vec_mask_type(vt) : vt, op);
}

/* ---- Declarators */
static Type *func_params(Token **rest, Token *tok, Type *ret)
{
    Type *fn = func_type(ret);
    if (tok->kw == K_VOID && tok_is(tok->next, ')')) { *rest = tok->next->next; return fn; }
    if (tok_is(tok, ')')) { fn->noproto = 1; *rest = tok->next; return fn; }
    Type head = { 0 }, *cur = &head;
    param_depth++;
    enter();                                        /* a parameter's name is visible to the next ones: f(int n, int a[n]) */
    while (!tok_is(tok, ')')) {
        if (cur != &head) tok = skip(tok, ',');
        if (tok_is(tok, P_ELLIPSIS)) { fn->is_variadic = 1; tok = tok->next; break; }
        VarAttr pa = { 0 };
        Type *t = declspec(&tok, tok, &pa);
        Token *name = 0;
        t = declarator(&tok, tok, t, &name);
        tok = attributes(tok, 0);
        if (t->kind == TY_ARRAY) t = pointer_to(t->base);
        else if (t->kind == TY_FUNC) t = pointer_to(t);
        t = copy_type(t);
        t->name = name;
        if (name) {                                 /* the parameter, ready for the definition */
            Obj *o = arena(sizeof *o);
            o->name = o->asm_name = name->name;
            o->ty = t;
            o->align = t->align;
            o->is_local = o->is_param = 1;
            o->tok = name;
            push_sym(name->name)->var = o;
            t->pobj = o;
        }
        cur = cur->next = t;
    }
    leave();
    param_depth--;
    fn->params = head.next;
    *rest = skip(tok, ')');
    return fn;
}

static Type *array_dims(Token **rest, Token *tok, Type *ty);

static Type *type_suffix(Token **rest, Token *tok, Type *ty)
{
    if (tok_is(tok, '(')) return func_params(rest, tok->next, ty);
    if (tok_is(tok, '[')) return array_dims(rest, tok->next, ty);
    *rest = tok;
    return ty;
}

static Type *array_dims(Token **rest, Token *tok, Type *ty)
{
    while (tok->kw == K_STATIC || tok->kw == K_CONST || tok->kw == K_VOLATILE || tok->kw == K_RESTRICT) tok = tok->next;
    if (tok_is(tok, ']')) {
        ty = type_suffix(rest, tok->next, ty);
        return array_of(ty, -1);
    }
    if (tok_is(tok, '*') && tok_is(tok->next, ']')) {  /* [*]: a VLA of unspecified size, prototypes only */
        if (!param_depth) error_at(tok, "[*] outside a function prototype");
        Type *t = array_of(type_suffix(rest, tok->next->next, ty), -1);
        t->is_vla = 1;
        t->vla_len = new_num(1, tok);
        return t;
    }
    Node *e = conditional(&tok, tok);
    tok = skip(tok, ']');
    ty = type_suffix(rest, tok, ty);
    if (!is_const_expr(e) || ty->is_vla) {          /* a variable length array */
        if (!cur_fn && !param_depth) error_at(e->tok, "a variable length array outside a function");
        Type *t = array_of(ty, -1);
        t->is_vla = 1;
        t->vla_len = e;
        return t;
    }
    long len = const_eval(e);
    if (len < 0) error_at(e->tok, "negative array size");
    return array_of(ty, len);
}

static Type *pointers(Token **rest, Token *tok, Type *ty)
{
    while (tok_is(tok, '*')) {
        ty = pointer_to(ty);
        tok = tok->next;
        int c = 0, v = 0;
        for (;;) {
            if (tok->kw == K_CONST) c = 1;
            else if (tok->kw == K_VOLATILE) v = 1;
            else if (tok->kw == K_ATOMIC) { ty = make_atomic(ty, tok); }
            else if (tok->kw == K_RESTRICT) ;
            else if (tok->kw == K_ATTRIBUTE) { tok = attributes(tok, 0); continue; }
            else break;
            tok = tok->next;
        }
        ty = qualify(ty, c, v);
    }
    *rest = tok;
    return ty;
}

/* declarator: pointers, then a name (or a declarator in parentheses), then
 * array/function suffixes. *name gets the identifier (0 if abstract). */
static Type *declarator(Token **rest, Token *tok, Type *ty, Token **name)
{
    ty = pointers(&tok, tok, ty);
    tok = attributes(tok, 0);
    if (tok_is(tok, '(') && !is_typename(tok->next) && !tok_is(tok->next, ')')) {
        Token *start = tok;
        Type dummy = { 0 };
        Token *ignored;
        declarator(&tok, start->next, &dummy, &ignored);    /* find where the parentheses end */
        tok = skip(tok, ')');
        ty = type_suffix(rest, tok, ty);
        return declarator(&tok, start->next, ty, name);
    }
    *name = 0;
    if (tok->kind == TK_IDENT && !tok->kw) { *name = tok; tok = tok->next; }
    return type_suffix(rest, tok, ty);
}

static Type *typename(Token **rest, Token *tok)
{
    Type *ty = declspec(&tok, tok, 0);
    Token *name;
    ty = declarator(rest, tok, ty, &name);
    if (name) error_at(name, "unexpected name in a type");
    return ty;
}

/* ---- struct, union, enum */
static Member *find_member(Type *ty, const char *name, long *off)
{
    for (Member *m = ty->members; m; m = m->next) {
        if (m->name == name) { *off += m->offset; return m; }
        if (!m->name && (m->ty->kind == TY_STRUCT || m->ty->kind == TY_UNION)) {
            long o = *off + m->offset;
            Member *r = find_member(m->ty, name, &o);
            if (r) { *off = o; return r; }
        }
    }
    return 0;
}

static void layout(Type *ty, int packed, int aligned)
{
    long bits = 0;
    int align = 1;
    for (Member *m = ty->members; m; m = m->next) {
        int ma = packed && m->align <= m->ty->align ? 1 : m->align;
        if (ty->kind == TY_UNION) {
            m->offset = 0;
            if (m->is_bitfield) { m->bit_off = 0; m->unit = packed ? (m->bit_width + 7) / 8 : m->ty->size; }
            long sz = m->is_bitfield ? (m->bit_width + 7) / 8 : m->ty->size;
            if (sz * 8 > bits) bits = sz * 8;
            if (!m->is_bitfield || m->name) align = ma > align ? ma : align;
            continue;
        }
        if (m->is_bitfield) {
            int unit = m->ty->size * 8;
            if (m->bit_width == 0) { bits = align_to(bits, unit); continue; }
            if (!packed && bits / unit != (bits + m->bit_width - 1) / unit) bits = align_to(bits, unit);
            if (packed) {                           /* any bit: just the bytes it covers */
                m->offset = bits / 8;
                m->bit_off = bits % 8;
                m->unit = (m->bit_off + m->bit_width + 7) / 8;
            } else {
                m->offset = bits / unit * (unit / 8);
                m->bit_off = bits % unit;
                m->unit = unit / 8;
            }
            bits += m->bit_width;
            if (m->name && !packed && m->ty->align > align) align = m->ty->align;
            continue;
        }
        bits = align_to(bits, ma * 8);
        m->offset = bits / 8;
        bits += (long)m->ty->size * 8;
        if (ma > align) align = ma;
    }
    if (aligned > align) align = aligned;
    ty->align = align;
    ty->size = align_to(align_to(bits, 8) / 8, align);
    ty->is_packed = packed;
}

static Type *struct_decl(Token **rest, Token *tok, int kind)
{
    VarAttr sa = { 0 };
    tok = attributes(tok, &sa);
    Token *tag = 0;
    if (tok->kind == TK_IDENT && !tok->kw) { tag = tok; tok = tok->next; }
    if (tag && !tok_is(tok, '{')) {
        *rest = tok;
        Type *t = find_tag(tag->name);
        if (t) {
            if (t->kind != kind) error_at(tag, "'%.*s' is another kind of tag", tag->len, tag->loc);
            return t;
        }
        t = new_type(kind, 0, 1);
        t->is_incomplete = 1;
        map_put(&scope->tags, tag->name, t);
        return t;
    }
    tok = skip(tok, '{');
    Member head = { 0 }, *cur = &head;
    while (!tok_is(tok, '}')) {
        if (tok->kw == K_STATIC_ASSERT) { declaration(&tok, tok, 0, 0); continue; }
        VarAttr ma = { 0 };
        Type *base = declspec(&tok, tok, &ma);
        if (tok_is(tok, ';') && (base->kind == TY_STRUCT || base->kind == TY_UNION)) {   /* anonymous member */
            Member *m = arena(sizeof *m);
            m->ty = base;
            m->tok = tok;
            m->align = base->align;
            cur = cur->next = m;
            tok = tok->next;
            continue;
        }
        int first = 1;
        while (!tok_is(tok, ';')) {
            if (!first) tok = skip(tok, ',');
            first = 0;
            Member *m = arena(sizeof *m);
            Token *name = 0;
            VarAttr a2 = ma;
            m->ty = tok_is(tok, ':') ? base : declarator(&tok, tok, base, &name);
            m->tok = name ? name : tok;
            m->name = name ? name->name : 0;
            if (tok_is(tok, ':')) {
                m->is_bitfield = 1;
                m->bit_width = const_expr(&tok, tok->next);
                if (!is_integer(m->ty)) error_at(m->tok, "a bit-field must have an integer type");
                if (m->bit_width > m->ty->size * 8) error_at(m->tok, "bit-field too wide");
            }
            tok = attributes(tok, &a2);
            m->align = m->ty->align;
            if (a2.aligned > m->align) m->align = a2.aligned;
            if (a2.packed) m->align = 1;
            if (m->ty->kind == TY_ARRAY && m->ty->len < 0) { m->ty = array_of(m->ty->base, 0); m->ty->is_flex = 1; }
            cur = cur->next = m;
        }
        tok = tok->next;
    }
    tok = attributes(tok->next, &sa);
    *rest = tok;
    Type *ty;
    Type *old = tag ? map_get(&scope->tags, tag->name) : 0;
    if (old && old->is_incomplete) ty = old;
    else {
        ty = new_type(kind, 0, 1);
        if (tag) map_put(&scope->tags, tag->name, ty);
    }
    ty->kind = kind;
    ty->members = head.next;
    ty->is_incomplete = 0;
    for (Member *m = head.next; m; m = m->next) if (!m->next && m->ty->is_flex) ty->is_flex = 1;
    layout(ty, sa.packed, sa.aligned);
    return ty;
}

static Type *enum_decl(Token **rest, Token *tok)
{
    tok = attributes(tok, 0);
    Token *tag = 0;
    if (tok->kind == TK_IDENT && !tok->kw) { tag = tok; tok = tok->next; }
    if (tag && !tok_is(tok, '{')) {
        Type *t = find_tag(tag->name);
        if (!t) {                                   /* a forward-declared enum */
            t = new_type(TY_ENUM, 4, 4);
            map_put(&scope->tags, tag->name, t);
        }
        if (t->kind != TY_ENUM) error_at(tag, "not an enum tag");
        *rest = tok;
        return t;
    }
    Type *ty = tag ? map_get(&scope->tags, tag->name) : 0;
    if (!ty || ty->kind != TY_ENUM) ty = new_type(TY_ENUM, 4, 4);
    tok = skip(tok, '{');
    long v = 0, min = 0, max = 0;
    Vec syms = { 0 };
    while (!tok_is(tok, '}')) {
        const char *name = ident(tok);
        tok = attributes(tok->next, 0);
        if (tok_is(tok, '=')) v = const_expr(&tok, tok->next);
        Sym *s = push_sym(name);
        s->enum_ty = ty;
        s->enum_val = v;
        vec_push(&syms, s);
        if (v < min) min = v;
        if (v > max) max = v;
        v++;
        if (!consume(&tok, tok, ',')) break;
    }
    tok = skip(tok, '}');
    *rest = attributes(tok, 0);
    if (min >= 0 && max > 0x7fffffff) ty->is_unsigned = 1;
    if (min < -0x80000000L || max > 0xffffffffL) { ty->size = ty->align = 8; }
    if (tag) map_put(&scope->tags, tag->name, ty);
    return ty;
}

/* ---- Initializers */
typedef struct Init Init;
struct Init {
    Type *ty;
    Token *tok;
    Node *expr;
    Init **child;
    int n;
    Member *mem;                /* union: the member initialized */
    int is_flex;
};

static Init *new_init(Type *ty, int is_flex)
{
    Init *in = arena(sizeof *in);
    in->ty = ty;
    if (ty->kind == TY_ARRAY) {
        if (is_flex && ty->len < 0) { in->is_flex = 1; return in; }
        in->n = ty->len < 0 ? 0 : ty->len;
        in->child = arena(sizeof(Init *) * (in->n + 1));
        for (int i = 0; i < in->n; i++) in->child[i] = new_init(ty->base, 0);
        return in;
    }
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
        int n = 0;
        for (Member *m = ty->members; m; m = m->next) n++;
        in->n = n;
        in->child = arena(sizeof(Init *) * (n + 1));
        int i = 0;
        for (Member *m = ty->members; m; m = m->next, i++) {
            if (is_flex && ty->is_flex && !m->next) {
                Init *c = arena(sizeof *c);
                c->ty = m->ty;
                c->is_flex = 1;
                in->child[i] = c;
            } else in->child[i] = new_init(m->ty, 0);
        }
    }
    return in;
}

static void init2(Token **rest, Token *tok, Init *in);

static int is_end(Token *t) { return tok_is(t, '}') || (tok_is(t, ',') && tok_is(t->next, '}')); }
static int consume_end(Token **rest, Token *t)
{
    if (tok_is(t, '}')) { *rest = t->next; return 1; }
    if (tok_is(t, ',') && tok_is(t->next, '}')) { *rest = t->next->next; return 1; }
    return 0;
}

static Token *skip_excess(Token *tok)
{
    if (tok_is(tok, '{')) {
        tok = skip_excess(tok->next);
        return skip(tok, '}');
    }
    assign(&tok, tok);
    return tok;
}

static int member_index(Type *ty, Member *m)
{
    int i = 0;
    for (Member *x = ty->members; x != m; x = x->next) i++;
    return i;
}

/* .a.b[3] = x: the designator path, then the initializer */
static void designation(Token **rest, Token *tok, Init *in)
{
    if (tok_is(tok, '[')) {
        if (in->ty->kind != TY_ARRAY) error_at(tok, "array index in a non-array initializer");
        long b = const_expr(&tok, tok->next), e = b;
        if (tok_is(tok, P_ELLIPSIS)) e = const_expr(&tok, tok->next);
        tok = skip(tok, ']');
        if (b < 0 || e >= in->n) error_at(tok, "array index out of range");
        Token *after = tok;
        for (long i = b; i <= e; i++) designation(&after, tok, in->child[i]);
        *rest = after;
        return;
    }
    if (tok_is(tok, '.')) {
        if (in->ty->kind != TY_STRUCT && in->ty->kind != TY_UNION) error_at(tok, "field name in a non-struct initializer");
        const char *name = ident(tok->next);
        for (Member *m = in->ty->members; m; m = m->next) {
            if (m->name == name) {
                if (in->ty->kind == TY_UNION) in->mem = m;
                designation(rest, tok->next->next, in->child[member_index(in->ty, m)]);
                return;
            }
            long off = 0;
            if (!m->name && (m->ty->kind == TY_STRUCT || m->ty->kind == TY_UNION) && find_member(m->ty, name, &off)) {
                if (in->ty->kind == TY_UNION) in->mem = m;
                designation(rest, tok, in->child[member_index(in->ty, m)]);
                return;
            }
        }
        error_at(tok->next, "no member named '%s'", name);
    }
    tok = skip(tok, '=');
    init2(rest, tok, in);
}

static void string_init(Token **rest, Token *tok, Init *in)
{
    char *p;
    int len;
    Type *ty;
    *rest = string_literal(tok, &p, &len, &ty);
    if (in->is_flex) *in = *new_init(array_of(in->ty->base, ty->len), 0);
    int es = in->ty->base->size;
    for (int i = 0; i < in->n && i < ty->len; i++) {
        long v = 0;
        memcpy(&v, p + i * es, es);
        in->child[i]->expr = new_num(truncate_to(v, in->ty->base), tok);
    }
}

/* How many elements does a braced array initializer have (flexible arrays)? */
static int count_elems(Token *tok, Type *ty)
{
    Init *dummy = new_init(ty->base, 0);
    int i = 0, max = 0;
    tok = tok->next;
    for (int first = 1; !consume_end(&tok, tok); first = 0) {
        if (!first) tok = skip(tok, ',');
        if (tok_is(tok, '[')) {
            i = const_expr(&tok, tok->next);
            if (tok_is(tok, P_ELLIPSIS)) i = const_expr(&tok, tok->next);
            tok = skip(tok, ']');
            designation(&tok, tok, dummy);
        } else init2(&tok, tok, dummy);
        i++;
        if (i > max) max = i;
    }
    return max;
}

static void array_init1(Token **rest, Token *tok, Init *in)
{
    if (in->is_flex) *in = *new_init(array_of(in->ty->base, count_elems(tok, in->ty)), 0);
    tok = skip(tok, '{');
    for (int i = 0, first = 1; !consume_end(rest, tok); i++, first = 0) {
        if (!first) tok = skip(tok, ',');
        if (tok_is(tok, '[')) {
            long b = const_expr(&tok, tok->next), e = b;
            if (tok_is(tok, P_ELLIPSIS)) e = const_expr(&tok, tok->next);
            tok = skip(tok, ']');
            if (b < 0 || e >= in->n) error_at(tok, "array index out of range");
            Token *after = tok;
            for (long j = b; j <= e; j++) designation(&after, tok, in->child[j]);
            tok = after;
            i = e;
            continue;
        }
        if (i < in->n) init2(&tok, tok, in->child[i]);
        else tok = skip_excess(tok);
    }
}

static void array_init2(Token **rest, Token *tok, Init *in)
{
    if (in->is_flex) {                              /* count what follows, to the closing brace */
        int n = 0;
        Token *t = tok;
        Init *dummy = new_init(in->ty->base, 0);
        for (; !is_end(t) && t->kind != TK_EOF; n++) {
            if (n) t = skip(t, ',');
            if (tok_is(t, '.') || tok_is(t, '[')) break;
            init2(&t, t, dummy);
        }
        *in = *new_init(array_of(in->ty->base, n), 0);
    }
    for (int i = 0; i < in->n && !is_end(tok); i++) {
        Token *start = tok;
        if (i) tok = skip(tok, ',');
        if (tok_is(tok, '[') || tok_is(tok, '.')) { *rest = start; return; }
        init2(&tok, tok, in->child[i]);
    }
    *rest = tok;
}

static void struct_init1(Token **rest, Token *tok, Init *in)
{
    tok = skip(tok, '{');
    Member *m = in->ty->members;
    for (int first = 1; !consume_end(rest, tok); first = 0) {
        if (!first) tok = skip(tok, ',');
        if (tok_is(tok, '.')) {
            const char *name = ident(tok->next);
            Member *x = in->ty->members;
            for (; x; x = x->next) {
                long off = 0;
                if (x->name == name || (!x->name && (x->ty->kind == TY_STRUCT || x->ty->kind == TY_UNION) && find_member(x->ty, name, &off))) break;
            }
            if (!x) error_at(tok->next, "no member named '%s'", name);
            if (x->name == name) designation(&tok, tok->next->next, in->child[member_index(in->ty, x)]);
            else designation(&tok, tok, in->child[member_index(in->ty, x)]);
            m = x->next;
            continue;
        }
        while (m && m->is_bitfield && !m->name) m = m->next;      /* unnamed bit-fields take nothing */
        if (m) {
            init2(&tok, tok, in->child[member_index(in->ty, m)]);
            m = m->next;
        } else tok = skip_excess(tok);
    }
}

static void struct_init2(Token **rest, Token *tok, Init *in)
{
    int first = 1;
    for (Member *m = in->ty->members; m && !is_end(tok); m = m->next) {
        if (m->is_bitfield && !m->name) continue;
        Token *start = tok;
        if (!first) tok = skip(tok, ',');
        first = 0;
        if (tok_is(tok, '[') || tok_is(tok, '.')) { *rest = start; return; }
        init2(&tok, tok, in->child[member_index(in->ty, m)]);
    }
    *rest = tok;
}

static void union_init(Token **rest, Token *tok, Init *in)
{
    if (tok_is(tok, '{') && tok_is(tok->next, '.')) {
        Member *m = in->ty->members;
        tok = tok->next;
        designation(&tok, tok, in);
        if (!in->mem) in->mem = m;
        consume(&tok, tok, ',');
        *rest = skip(tok, '}');
        return;
    }
    in->mem = in->ty->members;
    if (!in->mem) { *rest = skip(skip(tok, '{'), '}'); return; }
    if (tok_is(tok, '{')) {
        init2(&tok, tok->next, in->child[0]);
        consume(&tok, tok, ',');
        *rest = skip(tok, '}');
    } else init2(rest, tok, in->child[0]);
}

static void init2(Token **rest, Token *tok, Init *in)
{
    in->tok = tok;
    if (in->ty->kind == TY_ARRAY && tok->kind == TK_STR) { string_init(rest, tok, in); return; }
    if (in->ty->kind == TY_ARRAY && tok_is(tok, '{') && tok->next->kind == TK_STR && is_end(tok->next->next) && in->ty->base->size <= 4) {
        string_init(&tok, tok->next, in);
        consume_end(rest, tok);
        return;
    }
    if (in->ty->kind == TY_ARRAY) {
        if (tok_is(tok, '{')) array_init1(rest, tok, in);
        else array_init2(rest, tok, in);
        return;
    }
    if (in->ty->kind == TY_STRUCT) {
        if (tok_is(tok, '{')) { struct_init1(rest, tok, in); return; }
        Token *start = tok;
        Node *e = assign(&tok, tok);
        add_type(e);
        if (e->ty->kind == TY_STRUCT) { in->expr = e; *rest = tok; return; }
        struct_init2(rest, start, in);
        return;
    }
    if (in->ty->kind == TY_UNION) {
        if (!tok_is(tok, '{')) {
            Token *start = tok;
            Node *e = assign(&tok, tok);
            add_type(e);
            if (e->ty->kind == TY_UNION) { in->expr = e; *rest = tok; return; }
            tok = start;
        }
        union_init(rest, tok, in);
        return;
    }
    if (tok_is(tok, '{')) {                         /* braces around a scalar (extra values: ignored, as gcc does) */
        init2(&tok, tok->next, in);
        while (!consume_end(rest, tok)) {
            tok = skip(tok, ',');
            tok = skip_excess(tok);
        }
        return;
    }
    in->expr = assign(rest, tok);
}

static Init *initializer(Token **rest, Token *tok, Type *ty, Type **newty)
{
    Init *in = new_init(ty, 1);
    init2(rest, tok, in);
    if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->is_flex) {
        Type *t = copy_type(ty);
        t->origin = ty->origin ? ty->origin : ty;
        Member *last = ty->members;
        while (last->next) last = last->next;
        Init *fl = in->child[in->n - 1];
        if (fl->ty->kind == TY_ARRAY && fl->ty->len > 0) t->size = align_to(last->offset + fl->ty->size, t->align);
        *newty = t;
        return in;
    }
    *newty = in->ty;
    return in;
}

/* A local's initializer: zero the whole object, then assign each part. */
typedef struct Desg { struct Desg *up; int idx; Member *mem; Obj *var; } Desg;

static Node *desg_expr(Desg *d, Token *tok)
{
    if (d->var) return new_var_node(d->var, tok);
    if (d->mem) {
        Node *n = new_unary(ND_MEMBER, desg_expr(d->up, tok), tok);
        n->member = d->mem;
        return n;
    }
    Node *l = desg_expr(d->up, tok);
    Node *n = new_add(l, new_num(d->idx, tok), tok);
    return new_unary(ND_DEREF, n, tok);
}

static Node *lvar_init_tree(Init *in, Type *ty, Desg *d, Token *tok)
{
    if (ty->kind == TY_ARRAY) {
        Node *n = new_node(ND_NULL, tok);
        for (int i = 0; i < ty->len; i++) {
            Desg d2 = { d, i, 0, 0 };
            Node *r = lvar_init_tree(in->child[i], ty->base, &d2, tok);
            if (r->kind != ND_NULL) n = n->kind == ND_NULL ? r : new_binary(ND_COMMA, n, r, tok);
        }
        return n;
    }
    if (ty->kind == TY_STRUCT && !in->expr) {
        Node *n = new_node(ND_NULL, tok);
        int i = 0;
        for (Member *m = ty->members; m; m = m->next, i++) {
            Desg d2 = { d, 0, m, 0 };
            Node *r = lvar_init_tree(in->child[i], m->ty, &d2, tok);
            if (r->kind != ND_NULL) n = n->kind == ND_NULL ? r : new_binary(ND_COMMA, n, r, tok);
        }
        return n;
    }
    if (ty->kind == TY_UNION && !in->expr) {
        Member *m = in->mem ? in->mem : ty->members;
        if (!m) return new_node(ND_NULL, tok);
        Desg d2 = { d, 0, m, 0 };
        return lvar_init_tree(in->child[member_index(ty, m)], m->ty, &d2, tok);
    }
    if (!in->expr) return new_node(ND_NULL, tok);
    return new_assign(desg_expr(d, tok), in->expr, tok);
}

static Node *lvar_initializer(Token **rest, Token *tok, Obj *var)
{
    Init *in = initializer(rest, tok, var->ty, &var->ty);
    Desg d = { 0, 0, 0, var };
    Node *n = lvar_init_tree(in, var->ty, &d, tok);
    if (is_aggregate(var->ty)) {
        Node *z = new_node(ND_MEMZERO, tok);
        z->var = var;
        n = n->kind == ND_NULL ? z : new_binary(ND_COMMA, z, n, tok);
    }
    add_type(n);
    return n;
}

/* A global's initializer: bytes, plus relocations for addresses. */
static Reloc **write_data(Reloc **cur, Init *in, Type *ty, char *buf, long off)
{
    if (ty->kind == TY_ARRAY) {
        for (int i = 0; i < ty->len; i++) cur = write_data(cur, in->child[i], ty->base, buf, off + (long)ty->base->size * i);
        return cur;
    }
    if (ty->kind == TY_STRUCT && !in->expr) {
        int i = 0;
        for (Member *m = ty->members; m; m = m->next, i++) {
            if (m->is_bitfield) {                     /* bit by bit: works for any span */
                Node *e = in->child[i]->expr;
                if (!e) continue;
                unsigned long v = const_eval(e);
                unsigned char *p = (unsigned char *)buf + off + m->offset;
                for (int b = 0; b < m->bit_width; b++) {
                    int at = m->bit_off + b;
                    if (v >> b & 1) p[at / 8] |= 1 << at % 8;
                    else p[at / 8] &= ~(1 << at % 8);
                }
                continue;
            }
            cur = write_data(cur, in->child[i], m->ty, buf, off + m->offset);
        }
        return cur;
    }
    if (ty->kind == TY_UNION && !in->expr) {
        Member *m = in->mem ? in->mem : 0;
        if (!m) return cur;
        return write_data(cur, in->child[member_index(ty, m)], m->ty, buf, off);
    }
    if (!in->expr) return cur;
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) error_at(in->tok, "not a compile-time constant");
    if (ty->kind == TY_FLOAT) { float f = eval_double(in->expr); memcpy(buf + off, &f, 4); return cur; }
    if (ty->kind == TY_DOUBLE) { double d = eval_double(in->expr); memcpy(buf + off, &d, 8); return cur; }
    if (ty->kind == TY_LDOUBLE) { long double d = eval_ld(in->expr); memcpy(buf + off, &d, 10); return cur; }
    if (ty->kind == TY_COMPLEX) {
        long double re, im;
        eval_complex(new_cast(in->expr, ty), &re, &im);
        int k = ty->base->size;
        if (k == 4) { float a = re, b = im; memcpy(buf + off, &a, 4); memcpy(buf + off + 4, &b, 4); }
        else if (k == 8) { double a = re, b = im; memcpy(buf + off, &a, 8); memcpy(buf + off + 8, &b, 8); }
        else { memcpy(buf + off, &re, 10); memcpy(buf + off + 16, &im, 10); }
        return cur;
    }
    if (ty->kind == TY_INT128) { unsigned long hi, lo = eval128(in->expr, &hi); memcpy(buf + off, &lo, 8); memcpy(buf + off + 8, &hi, 8); return cur; }
    const char *label = 0;
    long v = eval_addr(in->expr, &label);
    if (!label) { memcpy(buf + off, &v, ty->size); return cur; }
    if (ty->size != 8) error_at(in->tok, "an address needs 8 bytes");
    Reloc *r = arena(sizeof *r);
    r->off = off;
    r->sym = label;
    r->addend = v;
    *cur = r;
    return &r->next;
}

static void gvar_initializer(Token **rest, Token *tok, Obj *var)
{
    Obj *saved = cur_owner;
    cur_owner = var;
    Init *in = initializer(rest, tok, var->ty, &var->ty);
    var->data = arena(var->ty->size + 1);
    var->rel = 0;
    write_data(&var->rel, in, var->ty, var->data, 0);
    var->has_init = 1;
    cur_owner = saved;
}

/* ---- Statements */
static Node *asm_stmt(Token **rest, Token *tok)
{
    Token *start = tok;
    tok = tok->next;
    while (tok->kw == K_VOLATILE || tok->kw == K_INLINE || tok_id(tok, "goto")) tok = tok->next;
    tok = skip(tok, '(');
    if (tok->kind != TK_STR) error_at(tok, "expected the asm string");
    AsmStmt *a = arena(sizeof *a);
    char *p;
    int len;
    Type *ty;
    tok = string_literal(tok, &p, &len, &ty);
    a->tmpl = p;
    a->is_basic = !tok_is(tok, ':');
    for (int part = 0; part < 3 && tok_is(tok, ':'); part++) {
        tok = tok->next;
        Vec v = { 0 };
        while (!tok_is(tok, ':') && !tok_is(tok, ')')) {
            if (v.n) tok = skip(tok, ',');
            if (part == 2) {
                if (tok->kind != TK_STR) error_at(tok, "expected a clobber name");
                vec_push(&v, xstrndup(tok->loc + 1, tok->len - 2));
                tok = tok->next;
                continue;
            }
            AsmOp *op = arena(sizeof *op);
            if (tok_is(tok, '[')) { op->name = ident(tok->next); tok = skip(tok->next->next, ']'); }
            if (tok->kind != TK_STR) error_at(tok, "expected an operand constraint");
            op->cons = xstrndup(tok->loc + 1, tok->len - 2);
            tok = skip(tok->next, '(');
            op->expr = expr(&tok, tok);
            add_type(op->expr);
            tok = skip(tok, ')');
            /* a memory-only operand needs its variable in memory */
            if (op->expr->kind == ND_VAR && strchr(op->cons, 'm') && !strpbrk(op->cons, "rqabcdSDgi"))
                op->expr->var->addr_taken = 1;
            if (part == 0 && !is_lvalue(op->expr)) error_at(op->expr->tok, "an asm output must be an lvalue");
            vec_push(&v, op);
        }
        if (part == 0) { a->nout = v.n; a->out = arena(sizeof(AsmOp) * (v.n + 1)); for (int i = 0; i < v.n; i++) a->out[i] = *(AsmOp *)v.v[i]; }
        if (part == 1) { a->nin = v.n; a->in = arena(sizeof(AsmOp) * (v.n + 1)); for (int i = 0; i < v.n; i++) a->in[i] = *(AsmOp *)v.v[i]; }
        if (part == 2) { a->nclob = v.n; a->clob = (const char **)v.v; }
    }
    tok = skip(tok, ')');
    *rest = skip(tok, ';');
    Node *n = new_node(ND_ASM, start);
    n->asm_ = a;
    return n;
}

static Node *stmt(Token **rest, Token *tok)
{
    switch (tok->kw) {
    case K_RETURN: {
        Node *n = new_node(ND_RETURN, tok);
        if (consume(rest, tok->next, ';')) return n;
        Node *e = expr(&tok, tok->next);
        *rest = skip(tok, ';');
        add_type(e);
        Type *rt = cur_fn->ty->base;
        if (rt->kind != TY_STRUCT && rt->kind != TY_UNION && rt->kind != TY_VOID) e = new_cast(e, rt);
        n->lhs = e;
        return n;
    }
    case K_IF: {
        Node *n = new_node(ND_IF, tok);
        tok = skip(tok->next, '(');
        n->cond = expr(&tok, tok);
        tok = skip(tok, ')');
        n->then = stmt(&tok, tok);
        if (tok->kw == K_ELSE) n->els = stmt(&tok, tok->next);
        *rest = tok;
        return n;
    }
    case K_SWITCH: {
        Node *n = new_node(ND_SWITCH, tok);
        tok = skip(tok->next, '(');
        n->cond = promote(expr(&tok, tok));
        tok = skip(tok, ')');
        Node *sw = cur_switch, *brk = cur_brk;
        cur_switch = n;
        cur_brk = n->brk = new_node(ND_NULL, tok);
        n->brk->vla = vla_scope;
        n->then = stmt(rest, tok);
        cur_switch = sw;
        cur_brk = brk;
        return n;
    }
    case K_CASE: {
        if (!cur_switch) error_at(tok, "case outside a switch");
        if (vla_scope != cur_switch->brk->vla) error_at(tok, "a jump into the scope of a variable length array");
        Node *n = new_node(ND_CASE, tok);
        long lo = const_expr(&tok, tok->next), hi = lo;
        if (tok_is(tok, P_ELLIPSIS)) hi = const_expr(&tok, tok->next);
        tok = skip(tok, ':');
        n->lo = truncate_to(lo, cur_switch->cond->ty);
        n->hi = truncate_to(hi, cur_switch->cond->ty);
        n->lhs = tok_is(tok, '}') ? new_node(ND_NULL, tok) : stmt(rest, tok);
        if (tok_is(tok, '}')) *rest = tok;
        n->case_next = cur_switch->cases;
        cur_switch->cases = n;
        return n;
    }
    case K_DEFAULT: {
        if (!cur_switch) error_at(tok, "default outside a switch");
        if (vla_scope != cur_switch->brk->vla) error_at(tok, "a jump into the scope of a variable length array");
        Node *n = new_node(ND_CASE, tok);
        tok = skip(tok->next, ':');
        n->lhs = tok_is(tok, '}') ? new_node(ND_NULL, tok) : stmt(rest, tok);
        if (tok_is(tok, '}')) *rest = tok;
        cur_switch->dflt = n;
        return n;
    }
    case K_FOR: {
        Node *n = new_node(ND_FOR, tok);
        tok = skip(tok->next, '(');
        enter();
        Node *brk = cur_brk, *cont = cur_cont;
        cur_brk = n->brk = new_node(ND_NULL, tok);
        cur_cont = n->cont = new_node(ND_NULL, tok);
        n->brk->vla = n->cont->vla = vla_scope;
        if (is_typename(tok)) {
            VarAttr a = { 0 };
            Type *base = declspec(&tok, tok, &a);
            n->init = declaration(&tok, tok, base, &a);
        } else if (!tok_is(tok, ';')) {
            n->init = new_unary(ND_EXPR_STMT, expr(&tok, tok), tok);
            tok = skip(tok, ';');
        } else tok = tok->next;
        if (!tok_is(tok, ';')) n->cond = expr(&tok, tok);
        tok = skip(tok, ';');
        if (!tok_is(tok, ')')) n->inc = expr(&tok, tok);
        tok = skip(tok, ')');
        n->then = stmt(rest, tok);
        leave();
        cur_brk = brk;
        cur_cont = cont;
        return n;
    }
    case K_WHILE: {
        Node *n = new_node(ND_FOR, tok);
        tok = skip(tok->next, '(');
        n->cond = expr(&tok, tok);
        tok = skip(tok, ')');
        Node *brk = cur_brk, *cont = cur_cont;
        cur_brk = n->brk = new_node(ND_NULL, tok);
        cur_cont = n->cont = new_node(ND_NULL, tok);
        n->brk->vla = n->cont->vla = vla_scope;
        n->then = stmt(rest, tok);
        cur_brk = brk;
        cur_cont = cont;
        return n;
    }
    case K_DO: {
        Node *n = new_node(ND_DO, tok);
        Node *brk = cur_brk, *cont = cur_cont;
        cur_brk = n->brk = new_node(ND_NULL, tok);
        cur_cont = n->cont = new_node(ND_NULL, tok);
        n->brk->vla = n->cont->vla = vla_scope;
        n->then = stmt(&tok, tok->next);
        cur_brk = brk;
        cur_cont = cont;
        if (tok->kw != K_WHILE) error_at(tok, "expected 'while'");
        tok = skip(tok->next, '(');
        n->cond = expr(&tok, tok);
        tok = skip(tok, ')');
        *rest = skip(tok, ';');
        return n;
    }
    case K_GOTO: {
        Node *n = new_node(ND_GOTO, tok);
        if (tok_is(tok->next, '*')) {                   /* goto *address (GNU) */
            n->lhs = new_cast(expr(&tok, tok->next->next), pointer_to(ty_void));
            *rest = skip(tok, ';');
            return n;
        }
        ident(tok->next);
        n->label = label_name(tok->next);
        n->vla = vla_scope;
        vec_push(&fn_gotos, n);
        *rest = skip(tok->next->next, ';');
        return n;
    }
    case K_BREAK: {
        if (!cur_brk) error_at(tok, "break outside a loop or switch");
        Node *n = new_node(ND_BREAK, tok);
        n->brk = cur_brk;
        n->var = vla_restore(vla_scope, cur_brk->vla, tok);
        *rest = skip(tok->next, ';');
        return n;
    }
    case K_CONTINUE: {
        if (!cur_cont) error_at(tok, "continue outside a loop");
        Node *n = new_node(ND_CONTINUE, tok);
        n->cont = cur_cont;
        n->var = vla_restore(vla_scope, cur_cont->vla, tok);
        *rest = skip(tok->next, ';');
        return n;
    }
    case K_ASM: return asm_stmt(rest, tok);
    }
    if (tok->kind == TK_IDENT && tok_is(tok->next, ':') && !tok->kw) {
        Node *n = new_node(ND_LABEL, tok);
        n->label = label_name(tok);
        n->vla = vla_scope;
        vec_push(&fn_labels, n);
        tok = attributes(tok->next->next, 0);
        if (tok_is(tok, '}')) { n->lhs = new_node(ND_NULL, tok); *rest = tok; return n; }
        n->lhs = stmt(rest, tok);
        return n;
    }
    if (tok_is(tok, '{')) return compound_stmt(rest, tok->next);
    if (tok_is(tok, ';')) { *rest = tok->next; return new_node(ND_NULL, tok); }
    Node *n = new_unary(ND_EXPR_STMT, expr(&tok, tok), tok);
    *rest = skip(tok, ';');
    return n;
}

static Node *compound_stmt(Token **rest, Token *tok)
{
    Node *n = new_node(ND_BLOCK, tok);
    Node head = { 0 }, *cur = &head;
    Obj *outer_sp = block_sp;
    VlaScope *outer_scope = vla_scope;
    int stmt_expr = in_stmt_expr;
    in_stmt_expr = 0;
    block_sp = 0;
    enter();
    while (!tok_is(tok, '}')) {
        if (tok->kind == TK_EOF) error_at(tok, "unexpected end of file");
        if (tok->kw == K_EXTENSION && is_typename(tok->next)) { tok = tok->next; continue; }
        if (is_typename(tok) && !tok_is(tok->next, ':')) {
            VarAttr a = { 0 };
            Type *base = declspec(&tok, tok, &a);
            if (a.is_typedef) {
                Token *t2 = tok;
                for (int first = 1; !consume(&tok, t2, ';'); first = 0) {
                    if (!first) t2 = skip(t2, ',');
                    Token *name;
                    Type *ty = declarator(&t2, t2, base, &name);
                    VarAttr ta = { 0 };
                    t2 = attributes(t2, &ta);
                    if (ta.vecsize) ty = vector_of(ty, ta.vecsize, t2);
                    if (!name) error_at(t2, "typedef name missing");
                    push_sym(name->name)->tdef = ty;
                    if (has_vla(ty)) {                    /* its size: now, once */
                        cur = cur->next = new_unary(ND_EXPR_STMT, vla_sizes(ty, name), name);
                        for (Type *x = ty; x && (x->kind == TY_PTR || x->kind == TY_ARRAY); x = x->base) x->vla_fixed = 1;
                    }
                }
                continue;
            }
            cur = cur->next = declaration(&tok, tok, base, &a);
            continue;
        }
        if (tok->kw == K_STATIC_ASSERT) { declaration(&tok, tok, 0, 0); continue; }
        cur = cur->next = stmt(&tok, tok);
        add_type(cur);
    }
    leave();
    if (block_sp && !stmt_expr)                     /* give the VLAs' space back */
        cur = cur->next = new_unary(ND_EXPR_STMT, builtin_node(B_SPRESTORE, new_var_node(block_sp, tok), ty_void, tok), tok);
    block_sp = outer_sp;
    vla_scope = outer_scope;
    n->body = head.next;
    *rest = tok->next;
    return n;
}

static void static_assert_decl(Token **rest, Token *tok)
{
    tok = skip(tok->next, '(');
    Token *start = tok;
    Node *e = conditional(&tok, tok);
    Token *msg = 0;
    if (consume(&tok, tok, ',')) { msg = tok; while (tok->kind == TK_STR) tok = tok->next; }
    tok = skip(tok, ')');
    *rest = skip(tok, ';');
    if (!const_eval(e)) {
        if (msg) error_at(start, "static assertion failed: %.*s", msg->len, msg->loc);
        error_at(start, "static assertion failed");
    }
}

/* ---- VLA sizes: computed (in bytes) when the declaration is reached */
static int has_vla(Type *t)
{
    for (; t && (t->kind == TY_PTR || t->kind == TY_ARRAY); t = t->base) if (t->is_vla) return 1;
    return 0;
}

static Node *vla_sizes(Type *t, Token *tok)        /* assignments of every VLA size in t, innermost first */
{
    if (!t || (t->kind != TY_PTR && t->kind != TY_ARRAY)) return new_node(ND_NULL, tok);
    Node *n = vla_sizes(t->base, tok);
    if (!t->is_vla || t->vla_fixed) return n;
    if (!t->vla_size) t->vla_size = new_lvar(0, ty_ulong);
    Node *bsz = t->base->is_vla ? new_var_node(t->base->vla_size, tok) : new_ulong(t->base->size, tok);
    Node *set = new_assign(new_var_node(t->vla_size, tok), new_binary(ND_MUL, new_cast(t->vla_len, ty_ulong), bsz, tok), tok);
    n = n->kind == ND_NULL ? set : new_binary(ND_COMMA, n, set, tok);
    add_type(n);
    return n;
}

static Node *builtin_node(int which, Node *arg, Type *ty, Token *tok)
{
    Node *n = new_node(ND_BUILTIN, tok);
    n->val = which;
    n->args = arg;
    n->ty = ty;
    return n;
}

/* A VLA variable: its sizes, the block's stack pointer saved if it is the
 * first, then the space taken from the stack (v is reached through a pointer). */
static Node *vla_alloc(Obj *v, Token *tok)
{
    Node *n = vla_sizes(v->ty, tok);
    if (!block_sp) {
        block_sp = new_lvar(0, pointer_to(ty_void));
        VlaScope *sc = arena(sizeof *sc);
        sc->sp = block_sp;
        sc->up = vla_scope;
        vla_scope = sc;
        Node *save = new_assign(new_var_node(block_sp, tok), builtin_node(B_SPSAVE, 0, pointer_to(ty_void), tok), tok);
        n = n->kind == ND_NULL ? save : new_binary(ND_COMMA, save, n, tok);
    }
    Obj *p = new_lvar(0, pointer_to(v->ty));
    Node *mem = builtin_node(B_ALLOCA, new_var_node(v->ty->vla_size, tok), pointer_to(ty_void), tok);
    Node *set = new_assign(new_var_node(p, tok), new_cast(mem, p->ty), tok);
    v->vla_ptr = p;
    n = new_binary(ND_COMMA, n, set, tok);
    add_type(n);
    return n;
}

/* the stack pointer to restore when jumping from scope `from` to scope `to` (0: none) */
static Obj *vla_restore(VlaScope *from, VlaScope *to, Token *tok)
{
    if (from == to) return 0;
    VlaScope *s = from;
    while (s && s->up != to) s = s->up;
    if (!s) error_at(tok, "a jump into the scope of a variable length array");
    return s->sp;
}

/* A declaration inside a function: local variables (or extern/static ones). */
static Node *declaration(Token **rest, Token *tok, Type *base, VarAttr *a)
{
    if (tok->kw == K_STATIC_ASSERT) { static_assert_decl(rest, tok); return new_node(ND_NULL, tok); }
    Node head = { 0 }, *cur = &head;
    for (int first = 1; !tok_is(tok, ';'); first = 0) {
        if (!first) tok = next_declarator(tok);
        Token *name;
        VarAttr va = *a;
        Type *ty = declarator(&tok, tok, base, &name);
        const char *reg = 0;
        tok = asm_label(tok, &reg);
        tok = attributes(tok, &va);
        if (va.vecsize) { ty = vector_of(ty, va.vecsize, tok); va.vecsize = 0; }
        if (!name) error_at(tok, "variable name missing");
        if (ty->kind == TY_VOID) error_at(name, "variable declared void");
        if (ty->kind == TY_FUNC || va.is_extern) {     /* a declaration of a global */
            Sym *s = find_sym(name->name);
            Obj *g = s && s->var && !s->var->is_local ? s->var : 0;
            if (!g) {
                Scope *sc = scope;
                scope = &global_scope;
                g = map_get(&scope->syms, name->name) ? ((Sym *)map_get(&scope->syms, name->name))->var : 0;
                if (!g) {
                    g = new_gvar(name->name, ty);
                    g->is_func = ty->kind == TY_FUNC;
                    g->is_extern = 1;
                    g->is_tls = va.is_tls;
                    if (reg) g->asm_name = reg;
                }
                scope = sc;
                push_sym(name->name)->var = g;
            }
            continue;
        }
        if (va.is_tls && !va.is_static) error_at(name, "a thread-local variable in a block must be static or extern");
        if (va.is_static) {                            /* a static local: a global with a hidden name */
            Obj *g = new_anon_gvar(ty);
            g->is_tls = va.is_tls;
            g->name = name->name;
            g->asm_name = fmt("%s.%d", name->name, anon_n++);
            push_sym(name->name)->var = g;
            if (va.aligned > g->align) g->align = va.aligned;
            if (va.align > g->align) g->align = va.align;
            g->section = va.section;
            add_ref(g);
            if (tok_is(tok, '=')) gvar_initializer(&tok, tok->next, g);
            continue;
        }
        Obj *v = new_lvar(name->name, ty);
        v->tok = name;
        v->reg = reg;
        if (ty->is_vla) {
            if (tok_is(tok, '=')) error_at(tok, "a variable length array cannot be initialized");
            cur = cur->next = new_unary(ND_EXPR_STMT, vla_alloc(v, name), name);
            continue;
        }
        if (has_vla(ty)) cur = cur->next = new_unary(ND_EXPR_STMT, vla_sizes(ty, name), name);
        if (va.aligned > v->align) v->align = va.aligned;
        if (va.align > v->align) v->align = va.align;
        if (ty->is_volatile) v->addr_taken = 1;
        if (tok_is(tok, '=')) {
            Node *init = lvar_initializer(&tok, tok->next, v);
            cur = cur->next = new_unary(ND_EXPR_STMT, init, tok);
        }
        if (v->ty->size < 0 || (v->ty->kind == TY_ARRAY && v->ty->len < 0)) error_at(name, "variable has an incomplete type");
        if ((v->ty->kind == TY_STRUCT || v->ty->kind == TY_UNION) && v->ty->is_incomplete) error_at(name, "variable has an incomplete type");
    }
    *rest = tok->next;
    Node *b = new_node(ND_BLOCK, tok);
    b->body = head.next;
    return b;
}

/* ---- Expressions */
static Node *expr(Token **rest, Token *tok)
{
    Node *n = assign(&tok, tok);
    while (tok_is(tok, ',')) {
        Token *op = tok;
        n = new_binary(ND_COMMA, n, assign(&tok, tok->next), op);
        add_type(n);
    }
    *rest = tok;
    return n;
}

static Node *assign(Token **rest, Token *tok)
{
    Node *n = conditional(&tok, tok);
    Token *op = tok;
    int k = -1;
    if (tok->kind == TK_PUNCT) switch (tok->punct) {
        case '=': return new_assign(n, assign(rest, tok->next), op);
        case P_ADDA: k = ND_ADD; break;
        case P_SUBA: k = ND_SUB; break;
        case P_MULA: k = ND_MUL; break;
        case P_DIVA: k = ND_DIV; break;
        case P_MODA: k = ND_MOD; break;
        case P_ANDA: k = ND_AND; break;
        case P_ORA: k = ND_OR; break;
        case P_XORA: k = ND_XOR; break;
        case P_SHLA: k = ND_SHL; break;
        case P_SHRA: k = ND_SHR; break;
    }
    if (k >= 0) return compound(k, n, assign(rest, tok->next), op, 0);
    *rest = tok;
    return n;
}

static Node *logor(Token **rest, Token *tok);

static Node *conditional(Token **rest, Token *tok)
{
    Node *c = logor(&tok, tok);
    if (!tok_is(tok, '?')) { *rest = tok; return c; }
    Token *op = tok;
    Node *n = new_node(ND_COND, op);
    if (tok_is(tok->next, ':')) {                   /* GNU a ?: b: a, computed once */
        add_type(c);
        Type *t = c->ty->kind == TY_ARRAY ? pointer_to(c->ty->base) : c->ty->kind == TY_FUNC ? pointer_to(c->ty) : c->ty;
        if (cur_fn && !(c->kind == ND_VAR || c->kind == ND_NUM)) {   /* (tmp = a) ? tmp : b */
            Obj *tmp = new_lvar(0, t);
            n->cond = new_assign(new_var_node(tmp, op), c, op);
            n->then = new_var_node(tmp, op);
        } else n->cond = n->then = c;              /* no side effects to repeat */
        n->els = conditional(rest, tok->next->next);
        add_type(n);
        return n;
    }
    n->cond = c;
    n->then = expr(&tok, tok->next);
    tok = skip(tok, ':');
    n->els = conditional(rest, tok);
    add_type(n);
    return n;
}

static Node *binop(int kind, Node *l, Node *r, Token *op)
{
    if (is_vec(l) || is_vec(r)) return vec_binop(kind, l, r, op);
    Node *n = new_binary(kind, l, r, op);
    add_type(n);
    return n;
}

static Node *bitor(Token **rest, Token *tok);
static Node *logand(Token **rest, Token *tok);
static Node *logor(Token **rest, Token *tok)
{
    Node *n = logand(&tok, tok);
    while (tok_is(tok, P_OR)) { Token *op = tok; n = binop(ND_LOGOR, n, logand(&tok, tok->next), op); }
    *rest = tok;
    return n;
}

static Node *logand(Token **rest, Token *tok)
{
    Node *n = bitor(&tok, tok);
    while (tok_is(tok, P_AND)) { Token *op = tok; n = binop(ND_LOGAND, n, bitor(&tok, tok->next), op); }
    *rest = tok;
    return n;
}

static Node *bitxor(Token **rest, Token *tok);
static Node *bitor(Token **rest, Token *tok)
{
    Node *n = bitxor(&tok, tok);
    while (tok_is(tok, '|')) { Token *op = tok; n = binop(ND_OR, n, bitxor(&tok, tok->next), op); }
    *rest = tok;
    return n;
}

static Node *bitand(Token **rest, Token *tok);
static Node *bitxor(Token **rest, Token *tok)
{
    Node *n = bitand(&tok, tok);
    while (tok_is(tok, '^')) { Token *op = tok; n = binop(ND_XOR, n, bitand(&tok, tok->next), op); }
    *rest = tok;
    return n;
}

static Node *equality(Token **rest, Token *tok);
static Node *bitand(Token **rest, Token *tok)
{
    Node *n = equality(&tok, tok);
    while (tok_is(tok, '&')) { Token *op = tok; n = binop(ND_AND, n, equality(&tok, tok->next), op); }
    *rest = tok;
    return n;
}

static Node *relational(Token **rest, Token *tok);
static Node *equality(Token **rest, Token *tok)
{
    Node *n = relational(&tok, tok);
    for (;;) {
        Token *op = tok;
        if (tok_is(tok, P_EQ)) n = binop(ND_EQ, n, relational(&tok, tok->next), op);
        else if (tok_is(tok, P_NE)) n = binop(ND_NE, n, relational(&tok, tok->next), op);
        else break;
    }
    *rest = tok;
    return n;
}

static Node *shift(Token **rest, Token *tok);
static Node *relational(Token **rest, Token *tok)
{
    Node *n = shift(&tok, tok);
    for (;;) {
        Token *op = tok;
        if (tok_is(tok, '<')) n = binop(ND_LT, n, shift(&tok, tok->next), op);
        else if (tok_is(tok, P_LE)) n = binop(ND_LE, n, shift(&tok, tok->next), op);
        else if (tok_is(tok, '>')) n = binop(ND_LT, shift(&tok, tok->next), n, op);
        else if (tok_is(tok, P_GE)) n = binop(ND_LE, shift(&tok, tok->next), n, op);
        else break;
    }
    *rest = tok;
    return n;
}

static Node *add(Token **rest, Token *tok);
static Node *shift(Token **rest, Token *tok)
{
    Node *n = add(&tok, tok);
    for (;;) {
        Token *op = tok;
        if (tok_is(tok, P_SHL)) n = binop(ND_SHL, n, add(&tok, tok->next), op);
        else if (tok_is(tok, P_SHR)) n = binop(ND_SHR, n, add(&tok, tok->next), op);
        else break;
    }
    *rest = tok;
    return n;
}

static Node *mul(Token **rest, Token *tok);
static Node *add(Token **rest, Token *tok)
{
    Node *n = mul(&tok, tok);
    for (;;) {
        Token *op = tok;
        if (tok_is(tok, '+')) n = new_add(n, mul(&tok, tok->next), op);
        else if (tok_is(tok, '-')) n = new_sub(n, mul(&tok, tok->next), op);
        else break;
        add_type(n);
    }
    *rest = tok;
    return n;
}

static Node *mul(Token **rest, Token *tok)
{
    Node *n = cast(&tok, tok);
    for (;;) {
        Token *op = tok;
        if (tok_is(tok, '*')) n = binop(ND_MUL, n, cast(&tok, tok->next), op);
        else if (tok_is(tok, '/')) n = binop(ND_DIV, n, cast(&tok, tok->next), op);
        else if (tok_is(tok, '%')) n = binop(ND_MOD, n, cast(&tok, tok->next), op);
        else break;
    }
    *rest = tok;
    return n;
}

static Node *compound_literal(Token **rest, Token *tok, Type *ty, Token *start)
{
    if (!cur_fn || scope == &global_scope) {
        Obj *g = new_anon_gvar(ty);
        gvar_initializer(rest, tok, g);
        return new_var_node(g, start);
    }
    Obj *v = new_lvar(0, ty);
    Node *init = lvar_initializer(rest, tok, v);
    Node *n = new_binary(ND_COMMA, init, new_var_node(v, start), start);
    add_type(n);
    return n;
}

static Node *cast(Token **rest, Token *tok)
{
    if (tok_is(tok, '(') && is_typename(tok->next)) {
        Token *start = tok;
        Type *ty = typename(&tok, tok->next);
        tok = skip(tok, ')');
        if (tok_is(tok, '{')) return compound_literal(rest, tok, ty, start);   /* (type){...} */
        Node *e = cast(rest, tok);
        add_type(e);
        if (ty->kind != TY_VOID && !is_scalar(ty) && !(is_compatible(ty, e->ty)) && !(ty->is_vector && e->ty->is_vector && ty->size == e->ty->size))
            error_at(start, "invalid cast");
        Node *n = new_cast(e, ty);
        n->tok = start;
        if (has_vla(ty) && cur_fn) {                /* (int (*)[n])p: the sizes first */
            n = new_binary(ND_COMMA, vla_sizes(ty, start), n, start);
            add_type(n);
        }
        return n;
    }
    return unary(rest, tok);
}

/* __real__ z, __imag__ z: a member of the complex value (an lvalue if z is one) */
static Node *complex_part(Node *e, int imag, Token *op)
{
    if (e->ty->kind != TY_COMPLEX) return imag ? new_cast(new_num(0, op), e->ty) : e;
    Member *m = arena(sizeof *m);
    m->ty = e->ty->base;
    m->offset = imag ? e->ty->base->size : 0;
    m->align = m->ty->align;
    Node *n = new_unary(ND_MEMBER, e, op);
    n->member = m;
    add_type(n);
    return n;
}

static Node *unary(Token **rest, Token *tok)
{
    Token *op = tok;
    if (tok->kind == TK_PUNCT) switch (tok->punct) {
        case '+': return promote(cast(rest, tok->next));
        case '-': { Node *e = cast(rest, tok->next); if (is_vec(e)) return vec_node(VOP_NEG, e, 0, e->ty, op);
                    Node *n = new_unary(ND_NEG, e, op); add_type(n); return n; }
        case '!': { Node *n = new_unary(ND_NOT, cast(rest, tok->next), op); add_type(n); return n; }
        case '~': { Node *e = cast(rest, tok->next); if (is_vec(e)) return vec_node(VOP_NOT, e, 0, e->ty, op);
                    Node *n = new_unary(ND_BITNOT, e, op); add_type(n); return n; }
        case P_AND:                                    /* &&label (GNU): the label's address */
            if (tok->next->kind != TK_IDENT) break;
            {
                Node *n = new_node(ND_LABEL_VAL, op);
                n->label = label_name(tok->next);
                n->ty = pointer_to(ty_void);
                vec_push(&fn_gotos, n);
                *rest = tok->next->next;
                return n;
            }
        case '&': {
            Node *e = cast(rest, tok->next);
            add_type(e);
            if (e->kind == ND_MEMBER && e->member->is_bitfield) error_at(op, "address of a bit-field");
            if (!is_lvalue(e) && e->ty->kind != TY_FUNC) {
                if (e->kind == ND_COMMA && is_lvalue(e->rhs)) { }   /* compound literal */
                else error_at(op, "taking the address of a value");
            }
            for (Node *x = e; x; x = x->kind == ND_MEMBER ? x->lhs : x->kind == ND_COMMA ? x->rhs : 0)
                if (x->kind == ND_VAR) { x->var->addr_taken = 1; break; }
            if (e->kind == ND_COMMA) {
                Node *n = new_binary(ND_COMMA, e->lhs, new_unary(ND_ADDR, e->rhs, op), op);
                add_type(n);
                return n;
            }
            Node *n = new_unary(ND_ADDR, e, op);
            add_type(n);
            return n;
        }
        case '*': {
            Node *e = cast(rest, tok->next);
            add_type(e);
            if (e->ty->kind == TY_FUNC) return e;
            Node *n = new_unary(ND_DEREF, e, op);
            add_type(n);
            return n;
        }
        case P_INC: return compound(ND_ADD, unary(rest, tok->next), new_num(1, op), op, 0);
        case P_DEC: return compound(ND_SUB, unary(rest, tok->next), new_num(1, op), op, 0);
    }
    if (tok->kw == K_SIZEOF || tok->kw == K_ALIGNOF) {
        int is_size = tok->kw == K_SIZEOF;
        Type *ty;
        if (tok_is(tok->next, '(') && is_typename(tok->next->next)) {
            ty = typename(&tok, tok->next->next);
            *rest = skip(tok, ')');
            if (has_vla(ty) && is_size && ty->is_vla) {   /* sizeof (int[n]): computed here */
                Node *sz = vla_sizes(ty, op);         /* creates ty->vla_size */
                Node *n = new_binary(ND_COMMA, sz, new_var_node(ty->vla_size, op), op);
                add_type(n);
                return n;
            }
            if (tok_is(*rest, '{')) {                  /* sizeof (type){...} */
                Node *e = compound_literal(rest, *rest, ty, op);
                ty = e->ty;
            }
        } else {
            Node *e = unary(rest, tok->next);
            add_type(e);
            ty = e->ty;
        }
        if (ty->is_vla && is_size) {                 /* known when running */
            Node *n = new_var_node(ty->vla_size, op);
            if (!ty->vla_size || !ty->vla_size->is_local) error_at(op, "internal: VLA size");
            return n;
        }
        if (ty->kind == TY_ARRAY && ty->len < 0 && is_size) error_at(op, "sizeof of an incomplete array");
        if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->is_incomplete) error_at(op, "sizeof of an incomplete type");
        return new_ulong(is_size ? ty->size : ty->align, op);
    }
    if (tok->kw == K_EXTENSION) return cast(rest, tok->next);
    if (tok_id(tok, "__real__") || tok_id(tok, "__real") || tok_id(tok, "__imag__") || tok_id(tok, "__imag")) {
        int imag = tok->name[2] == 'i';
        Node *e = cast(rest, tok->next);
        add_type(e);
        return complex_part(e, imag, op);
    }
    return postfix(rest, tok);
}

static Node *struct_ref(Node *l, Token *tok)
{
    add_type(l);
    if (l->ty->kind != TY_STRUCT && l->ty->kind != TY_UNION) error_at(tok, "not a struct or union");
    if (l->ty->is_incomplete) error_at(tok, "incomplete type");
    const char *name = ident(tok);
    /* find the member, through anonymous members */
    for (Member *m = l->ty->members; m; m = m->next) {
        if (m->name == name) {
            Node *n = new_unary(ND_MEMBER, l, tok);
            n->member = m;
            add_type(n);
            return n;
        }
        long off = 0;
        if (!m->name && (m->ty->kind == TY_STRUCT || m->ty->kind == TY_UNION) && find_member(m->ty, name, &off)) {
            Node *n = new_unary(ND_MEMBER, l, tok);
            n->member = m;
            add_type(n);
            return struct_ref(n, tok);
        }
    }
    error_at(tok, "no member named '%s'", name);
}

static Node *funcall(Token **rest, Token *tok, Node *fn)
{
    add_type(fn);
    Type *ft = fn->ty;
    if (ft->kind == TY_PTR && ft->base->kind == TY_FUNC) ft = ft->base;
    if (ft->kind != TY_FUNC) error_at(fn->tok, "not a function");
    Node head = { 0 }, *cur = &head;
    Type *p = ft->params;
    int n = 0;
    tok = tok->next;
    while (!tok_is(tok, ')')) {
        if (n++) tok = skip(tok, ',');
        Node *a = assign(&tok, tok);
        add_type(a);
        if (p) {
            if (p->kind == TY_STRUCT || p->kind == TY_UNION) {
                if (a->ty->kind != p->kind) error_at(a->tok, "passing a wrong type");
            } else a = new_cast(a, p);
            p = p->next;
        } else {
            if (!ft->is_variadic && !ft->noproto) error_at(a->tok, "too many arguments");
            if (a->ty->kind == TY_FLOAT) a = new_cast(a, ty_double);
            else if (is_integer(a->ty)) a = promote(a);
            else if (a->ty->kind == TY_ARRAY) a = new_cast(a, pointer_to(a->ty->base));
            else if (a->ty->kind == TY_FUNC) a = new_cast(a, pointer_to(a->ty));
        }
        cur = cur->next = a;
    }
    if (p) error_at(tok, "too few arguments");
    *rest = skip(tok, ')');
    Node *c = new_unary(ND_CALL, fn, tok);
    c->fty = ft;
    c->ty = ft->base;
    c->args = head.next;
    return c;
}

static Node *postfix(Token **rest, Token *tok);
static Node *primary(Token **rest, Token *tok);

static Node *postfix(Token **rest, Token *tok)
{
    Node *n = primary(&tok, tok);
    for (;;) {
        Token *op = tok;
        if (tok_is(tok, '(')) { n = funcall(&tok, tok, n); continue; }
        if (tok_is(tok, '[')) {
            Node *i = expr(&tok, tok->next);
            tok = skip(tok, ']');
            if (is_vec(n)) {                                 /* v[i]: an element of its array */
                Node *m = new_unary(ND_MEMBER, n, op);
                m->member = n->ty->members;
                add_type(m);
                n = m;
            }
            n = new_unary(ND_DEREF, new_add(n, i, op), op);
            add_type(n);
            continue;
        }
        if (tok_is(tok, '.')) { n = struct_ref(n, tok->next); tok = tok->next->next; continue; }
        if (tok_is(tok, P_ARROW)) {
            n = new_unary(ND_DEREF, n, op);
            add_type(n);
            n = struct_ref(n, tok->next);
            tok = tok->next->next;
            continue;
        }
        if (tok_is(tok, P_INC)) { n = compound(ND_ADD, n, new_num(1, op), op, 1); tok = tok->next; continue; }
        if (tok_is(tok, P_DEC)) { n = compound(ND_SUB, n, new_num(1, op), op, 1); tok = tok->next; continue; }
        break;
    }
    *rest = tok;
    return n;
}

/* An implicitly declared library function (__builtin_memcpy -> memcpy). */
static Node *libfunc(const char *name, Type *ret, Type *params, Token *tok)
{
    const char *n = intern(name, strlen(name));
    Sym *s = map_get(&global_scope.syms, n);
    Obj *f = s ? s->var : 0;
    if (!f) {
        Scope *sc = scope;
        scope = &global_scope;
        Type *ft = func_type(ret);
        ft->params = params;
        f = new_gvar(n, ft);
        f->is_func = f->is_extern = 1;
        scope = sc;
    }
    return new_var_node(f, tok);
}

static Type *param(Type *t) { return copy_type(t); }

/* Cast the argument in *slot to t, keeping the list linked. */
static void cast_arg(Node **slot, Type *t)
{
    Node *next = (*slot)->next;
    *slot = new_cast(*slot, t);
    if (*slot != next) (*slot)->next = next;
}

static Node *builtin(Token **rest, Token *tok)
{
    const char *s = tok->name;
    Token *start = tok;
    int len = tok->len;
#define IS(x) (len == (int)sizeof(x) - 1 && !strncmp(s, x, len))
    tok = skip(tok->next, '(');
    Node *n = new_node(ND_BUILTIN, start);
    Node head = { 0 }, *cur = &head;
    if (IS("__builtin_va_arg")) {
        Node *ap = assign(&tok, tok);
        tok = skip(tok, ',');
        Type *ty = typename(&tok, tok);
        *rest = skip(tok, ')');
        n->val = B_VA_ARG;
        n->args = ap;
        n->ty = ty;
        add_type(ap);
        return n;
    }
    if (IS("__builtin_offsetof")) {
        Type *ty = typename(&tok, tok);
        tok = skip(tok, ',');
        long off = 0;
        Type *t = ty;
        for (int first = 1;; first = 0) {
            if (first || tok_is(tok, '.')) {
                if (!first) tok = tok->next;
                if (t->kind != TY_STRUCT && t->kind != TY_UNION) error_at(tok, "not a struct");
                Member *m = find_member(t, ident(tok), &off);
                if (!m) error_at(tok, "no member named '%.*s'", tok->len, tok->loc);
                t = m->ty;
                tok = tok->next;
            } else if (tok_is(tok, '[')) {
                long i = const_expr(&tok, tok->next);
                tok = skip(tok, ']');
                if (t->kind != TY_ARRAY) error_at(tok, "not an array");
                t = t->base;
                off += i * t->size;
            } else break;
        }
        *rest = skip(tok, ')');
        return new_ulong(off, start);
    }
    if (IS("__builtin_shufflevector")) {              /* (a, b, i...): elements of a (0..n-1) and b (n..2n-1) */
        Node *a = assign(&tok, tok);
        tok = skip(tok, ',');
        Node *b = assign(&tok, tok);
        if (!is_vec(a) || !is_vec(b) || a->ty->size != b->ty->size || a->ty->base->size != b->ty->base->size || is_flonum(a->ty->base) != is_flonum(b->ty->base))
            error_at(start, "__builtin_shufflevector needs two vectors of one type");
        Node head = { 0 }, *cur = &head;
        int n = 0;
        while (consume(&tok, tok, ',')) {
            Node *e = conditional(&tok, tok);
            long k = const_eval(e);
            if (k < 0 || k >= 2 * (a->ty->size / a->ty->base->size)) error_at(e->tok, "shuffle index out of range");
            cur = cur->next = new_num(k, e->tok);
            n++;
        }
        *rest = skip(tok, ')');
        if (!n || (n & (n - 1)) || n * a->ty->base->size > 32) error_at(start, "__builtin_shufflevector: 1, 2, 4... elements");
        Node *v = vec_node(VOP_SHUF, a, b, vector_of(a->ty->base, n * a->ty->base->size, start), start);
        v->args = head.next;
        return v;
    }
    if (IS("__builtin_vec")) {                        /* (VOP_*, a, b, imm): an operation of the intrinsic headers */
        Node *o = conditional(&tok, tok);
        tok = skip(tok, ',');
        Node *a = assign(&tok, tok);
        tok = skip(tok, ',');
        Node *b = assign(&tok, tok);
        tok = skip(tok, ',');
        Node *imm = conditional(&tok, tok);
        *rest = skip(tok, ')');
        long op = const_eval(o);
        if (op < 0 || op >= VOP_COUNT || op == VOP_SHUF || op == VOP_SPLAT) error_at(start, "__builtin_vec: unknown operation");
        if (!is_vec(a)) error_at(a->tok, "__builtin_vec needs vectors");
        Node *v = vec_node(op, a, is_vec(b) ? b : 0, op == VOP_MOVEMASK ? ty_int : a->ty, start);
        v->hi = const_eval(imm);
        return v;
    }
    if (IS("__builtin_neon")) {                       /* (VOP_*, (T *)0, a, b, acc, imm): an operation of arm_neon.h */
        Node *o = conditional(&tok, tok);
        tok = skip(tok, ',');
        Node *rt = assign(&tok, tok);                 /* T: the result's type, given as a null pointer to it */
        tok = skip(tok, ',');
        Node *a = assign(&tok, tok);
        tok = skip(tok, ',');
        Node *b = assign(&tok, tok);
        tok = skip(tok, ',');
        Node *c = assign(&tok, tok);
        tok = skip(tok, ',');
        Node *imm = conditional(&tok, tok);
        *rest = skip(tok, ')');
        long op = const_eval(o);
        add_type(rt);
        if (target != T_AARCH64) error_at(start, "__builtin_neon: AArch64 only");
        if (op < 0 || op >= VOP_COUNT || op == VOP_SHUF || op == VOP_MOVEMASK) error_at(start, "__builtin_neon: unknown operation");
        if (op == VOP_SPLAT && rt->ty->kind == TY_PTR && rt->ty->base->is_vector)   /* a scalar in every element (one dup) */
            return vec_node(VOP_SPLAT, new_cast(a, rt->ty->base->base), 0, rt->ty->base, start);
        if (rt->ty->kind != TY_PTR || !is_vec(a)) error_at(start, "__builtin_neon: (op, (T *)0, vector, ...)");
        if ((op == VOP_DOT || op == VOP_DOTL) && !opt_dotprod) error_at(start, "the dot product instructions need -march=armv8.2-a+dotprod");
        Type *t = rt->ty->base;
        if (op == VOP_ADDV ? !is_numeric(t) : !t->is_vector) error_at(start, "__builtin_neon: bad result type");
        Node *v = vec_node(op, a, is_vec(b) ? b : 0, t, start);
        if (is_vec(c)) v->cond = c;
        v->hi = const_eval(imm);
        return v;
    }
    if (IS("__builtin_complex")) {                    /* (re, im) of the same real type */
        Node *re = assign(&tok, tok);
        tok = skip(tok, ',');
        Node *im = assign(&tok, tok);
        *rest = skip(tok, ')');
        add_type(re);
        add_type(im);
        Type *ct = complex_of(is_flonum(re->ty) ? re->ty : ty_double);
        Node *n = new_node(ND_NUM, start);              /* constant parts: a constant */
        if (is_const_expr(re) && is_const_expr(im)) {
            n->ty = ct;
            n->fval = eval_double(re);
            n->ldval = eval_ld(im);
            if (ct->base->kind == TY_LDOUBLE) n->fval = eval_ld(re);
            return n;
        }
        if (!cur_fn) error_at(start, "not a compile-time constant");
        Obj *t = new_lvar(0, ct);                         /* else: t = re, __imag__ t = im, t */
        Node *a = new_assign(new_var_node(t, start), re, start);
        Node *b = new_assign(complex_part(new_var_node(t, start), 1, start), im, start);
        Node *c = new_binary(ND_COMMA, new_binary(ND_COMMA, a, b, start), new_var_node(t, start), start);
        add_type(c);
        return c;
    }
    if (IS("__builtin_types_compatible_p")) {
        Type *a = typename(&tok, tok);
        tok = skip(tok, ',');
        Type *b = typename(&tok, tok);
        *rest = skip(tok, ')');
        return new_num(is_compatible(a, b), start);
    }
    for (int i = 0; !tok_is(tok, ')'); i++) {
        if (i) tok = skip(tok, ',');
        Node *a = assign(&tok, tok);
        add_type(a);
        cur = cur->next = a;
    }
    *rest = skip(tok, ')');
    Node *a = head.next;
    int nargs = 0;
    for (Node *x = a; x; x = x->next) nargs++;
    n->args = a;
    n->ty = ty_void;
    if (IS("__builtin_va_start")) { n->val = B_VA_START; return n; }
    if (IS("__builtin_va_end")) { n->val = B_VA_END; return n; }
    if (IS("__builtin_va_copy")) { n->val = B_VA_COPY; return n; }
    if (IS("__builtin_unreachable")) { n->val = B_UNREACHABLE; return n; }
    if (IS("__builtin_trap")) { n->val = B_TRAP; return n; }
    if (IS("__builtin_expect") || IS("__builtin_expect_with_probability") || IS("__builtin_assume_aligned")) {
        return a;
    }
    if (IS("__builtin_constant_p")) return new_num(is_const_expr(a), start);
    if (IS("__builtin_frame_address") || IS("__builtin_return_address")) {
        n->val = B_FRAME_ADDRESS;
        n->lo = IS("__builtin_return_address");
        n->ty = pointer_to(ty_void);
        return n;
    }
    if (IS("__atomic_thread_fence") || IS("__atomic_signal_fence") || IS("__sync_synchronize")) { n->val = B_FENCE; return n; }
    /* the atomics: the first argument is a pointer to the object */
    Type *obj = a && a->ty->kind == TY_PTR ? a->ty->base : ty_int;
    if (a && (a->ty->kind == TY_PTR) && obj->kind != TY_VOID && obj->size > 8) error_at(start, "atomic object too large");
    if (IS("__atomic_load_n")) { n->val = B_ATOMIC_LOAD; n->ty = obj; return n; }
    int need2 = nargs >= 2;
    if (IS("__atomic_store_n") && need2) { cast_arg(&a->next, obj); n->val = B_ATOMIC_STORE; return n; }
    if ((IS("__atomic_exchange_n") || IS("__sync_lock_test_and_set")) && need2) { cast_arg(&a->next, obj); n->val = B_ATOMIC_EXCHANGE; n->ty = obj; return n; }
    /* read-modify-write: fetch_op returns the old value, op_fetch (and __sync_op_and_fetch) the new one */
    static const struct { const char *op; int b; } rmw[] = {
        { "add", B_ATOMIC_FETCH_ADD }, { "sub", B_ATOMIC_FETCH_SUB }, { "and", B_ATOMIC_FETCH_AND },
        { "or", B_ATOMIC_FETCH_OR }, { "xor", B_ATOMIC_FETCH_XOR }, { "nand", B_ATOMIC_FETCH_NAND },
    };
    for (unsigned k = 0; k < sizeof rmw / sizeof rmw[0] && need2; k++) {
        const char *o = rmw[k].op;
#define ISF(x) (len == (int)strlen(x) && !strncmp(s, x, len))
        int fetch_op = ISF(fmt("__atomic_fetch_%s", o)) || ISF(fmt("__sync_fetch_and_%s", o));
        int op_fetch = ISF(fmt("__atomic_%s_fetch", o)) || ISF(fmt("__sync_%s_and_fetch", o));
#undef ISF
        if (!fetch_op && !op_fetch) continue;
        cast_arg(&a->next, obj->kind == TY_PTR ? ty_long : obj);
        n->val = rmw[k].b;
        n->hi = op_fetch;
        n->ty = obj;
        return n;
    }
    if (IS("__atomic_test_and_set")) { n->val = B_ATOMIC_TAS; n->ty = ty_bool; return n; }
    if (IS("__atomic_is_lock_free") || IS("__atomic_always_lock_free")) {   /* (size, pointer): up to 8 bytes */
        long sz = const_eval(a);
        return new_num(sz == 1 || sz == 2 || sz == 4 || sz == 8, start);
    }
    /* the generic forms take pointers to the values: rewritten to the _n ones */
    if ((IS("__atomic_load") && nargs == 3) || (IS("__atomic_store") && nargs == 3) ||
        (IS("__atomic_exchange") && nargs == 4) || (IS("__atomic_compare_exchange") && nargs == 6)) {
        Node *x = new_node(ND_BUILTIN, start);
        x->ty = obj;
        x->args = a;
        Node *third = a->next->next;
        if (IS("__atomic_load")) {                    /* *ret = load(p) */
            x->val = B_ATOMIC_LOAD;
            Node *ret = a->next;
            a->next = third;
            Node *r = new_assign(new_unary(ND_DEREF, ret, start), x, start);
            add_type(r);
            return r;
        }
        Node *val = new_unary(ND_DEREF, a->next, start);    /* *val */
        add_type(val);
        if (IS("__atomic_store")) { val->next = third; a->next = val; x->val = B_ATOMIC_STORE; x->ty = ty_void; return x; }
        if (IS("__atomic_exchange")) {                /* *ret = exchange(p, *val) */
            Node *ret = third;
            val->next = ret->next;
            a->next = val;
            x->val = B_ATOMIC_EXCHANGE;
            Node *r = new_assign(new_unary(ND_DEREF, ret, start), x, start);
            add_type(r);
            return r;
        }
        Node *des = new_unary(ND_DEREF, third, start);      /* compare_exchange(p, expected, *desired, ...) */
        add_type(des);
        des->next = third->next;
        a->next->next = des;
        x->val = B_CAS;
        x->lo = 1;
        x->ty = ty_bool;
        return x;
    }
    if (IS("__atomic_clear") || IS("__sync_lock_release")) { n->val = B_ATOMIC_CLEAR; return n; }
    if (IS("__atomic_compare_exchange_n") && nargs >= 3) {   /* (p, &expected, desired, weak, ok, fail) */
        cast_arg(&a->next->next, obj);
        n->val = B_CAS;
        n->lo = 1;
        n->ty = ty_bool;
        return n;
    }
    if ((IS("__sync_bool_compare_and_swap") || IS("__sync_val_compare_and_swap")) && nargs >= 3) {   /* (p, old, new) */
        cast_arg(&a->next, obj);
        cast_arg(&a->next->next, obj);
        n->val = B_CAS;
        n->lo = IS("__sync_val_compare_and_swap") ? 2 : 0;
        n->ty = n->lo == 2 ? obj : ty_bool;
        return n;
    }
    if (IS("__builtin_clz") || IS("__builtin_clzl") || IS("__builtin_clzll") ||
        IS("__builtin_ctz") || IS("__builtin_ctzl") || IS("__builtin_ctzll") ||
        IS("__builtin_popcount") || IS("__builtin_popcountl") || IS("__builtin_popcountll")) {
        int l = s[len - 1] == 'l';
        n->args = new_cast(a, l ? ty_ulong : ty_uint);
        n->val = s[11] == 'l' ? B_CLZ : s[11] == 't' ? B_CTZ : B_POPCOUNT;   /* __builtin_c[l]z, c[t]z, p[o]pcount */
        n->ty = ty_int;
        return n;
    }
    if (IS("__builtin_bswap16")) {
        Node *x = new_cast(a, ty_uint);
        Node *r = new_binary(ND_OR, new_binary(ND_AND, new_binary(ND_SHR, x, new_num(8, start), start), new_num(255, start), start),
                             new_binary(ND_AND, new_binary(ND_SHL, x, new_num(8, start), start), new_num(0xff00, start), start), start);
        add_type(r);
        return new_cast(r, ty_ushort);
    }
    if (IS("__builtin_bswap32")) { n->args = new_cast(a, ty_uint); n->val = B_BSWAP32; n->ty = ty_uint; return n; }
    if (IS("__builtin_bswap64")) { n->args = new_cast(a, ty_ulong); n->val = B_BSWAP64; n->ty = ty_ulong; return n; }
    /* library functions under a builtin name */
    Type *vp = pointer_to(ty_void), *cvp = pointer_to(ty_void);
    Type *ps = 0;
    Node *fn = 0;
    if (IS("__builtin_memcpy") || IS("__builtin_memmove")) {
        ps = param(vp); ps->next = param(cvp); ps->next->next = param(ty_ulong);
        fn = libfunc(s + 10, vp, ps, start);
    } else if (IS("__builtin_memset")) {
        ps = param(vp); ps->next = param(ty_int); ps->next->next = param(ty_ulong);
        fn = libfunc("memset", vp, ps, start);
    } else if (IS("__builtin_memcmp")) {
        ps = param(cvp); ps->next = param(cvp); ps->next->next = param(ty_ulong);
        fn = libfunc("memcmp", ty_int, ps, start);
    } else if (IS("__builtin_strlen")) {
        ps = param(pointer_to(ty_char));
        fn = libfunc("strlen", ty_ulong, ps, start);
    } else if (IS("__builtin_abort")) {
        fn = libfunc("abort", ty_void, 0, start);
    } else if (IS("__builtin_sqrt") || IS("__builtin_sqrtf")) {   /* the instruction when optimizing (ir.c) */
        Type *t = IS("__builtin_sqrtf") ? ty_float : ty_double;
        ps = param(t);
        fn = libfunc(IS("__builtin_sqrtf") ? "sqrtf" : "sqrt", t, ps, start);
    }
    if (fn) {
        add_type(fn);
        Node *c = new_unary(ND_CALL, fn, start);
        c->fty = fn->ty;
        c->ty = fn->ty->base;
        Node h2 = { 0 }, *c2 = &h2;
        Type *p = fn->ty->params;
        for (Node *x = a, *nx; x; x = nx, p = p ? p->next : 0) { nx = x->next; x->next = 0; c2 = c2->next = p ? new_cast(x, p) : x; }
        c->args = h2.next;
        return c;
    }
#undef IS
    error_at(start, "unknown builtin '%.*s'", len, s);
}

static Node *generic_selection(Token **rest, Token *tok)
{
    Token *start = tok;
    tok = skip(tok->next, '(');
    Node *c = assign(&tok, tok);
    add_type(c);
    Type *t = c->ty;
    if (t->kind == TY_ARRAY) t = pointer_to(t->base);
    else if (t->kind == TY_FUNC) t = pointer_to(t);
    else if (t->is_const || t->is_volatile) t = t->origin ? t->origin : t;
    Node *ret = 0, *dflt = 0;
    while (!tok_is(tok, ')')) {
        tok = skip(tok, ',');
        if (tok->kw == K_DEFAULT) {
            tok = skip(tok->next, ':');
            dflt = assign(&tok, tok);
            continue;
        }
        Type *u = typename(&tok, tok);
        tok = skip(tok, ':');
        Node *e = assign(&tok, tok);
        if (!ret && is_compatible(t, u)) ret = e;
    }
    *rest = tok->next;
    if (!ret) ret = dflt;
    if (!ret) error_at(start, "no _Generic association matches");
    return ret;
}

static Node *primary(Token **rest, Token *tok)
{
    Token *start = tok;
    if (tok_is(tok, '(') && tok_is(tok->next, '{')) {                  /* ({ ... }) */
        Node *n = new_node(ND_STMT_EXPR, tok);
        in_stmt_expr = 1;
        n->body = compound_stmt(&tok, tok->next->next)->body;
        *rest = skip(tok, ')');
        add_type(n);
        return n;
    }
    if (tok_is(tok, '(')) {
        Node *n = expr(&tok, tok->next);
        *rest = skip(tok, ')');
        return n;
    }
    if (tok->kind == TK_NUM) { *rest = tok->next; return number(tok); }
    if (tok->kind == TK_CHAR) { *rest = tok->next; return char_literal(tok); }
    if (tok->kind == TK_STR) {
        char *p;
        int len;
        Type *ty;
        *rest = string_literal(tok, &p, &len, &ty);
        Obj *o = new_string(p, len, ty);
        return new_var_node(o, start);
    }
    if (tok->kw == K_GENERIC) return generic_selection(rest, tok);
    if (tok->kind == TK_IDENT && !tok->kw) {
        if (!strncmp(tok->name, "__builtin_", 10) && tok_is(tok->next, '(') && !find_sym(tok->name)) {
            /* __builtin_memcpy & co: the library function (declared as the standard says, if not yet) */
            static const struct { const char *name; int ret, p[3]; } lib[] = {   /* 1 void *, 2 const void *, 3 size_t, 4 int, 5 const char * */
                { "memcpy", 1, { 1, 2, 3 } }, { "memmove", 1, { 1, 2, 3 } }, { "memset", 1, { 1, 4, 3 } },
                { "memcmp", 4, { 2, 2, 3 } }, { "strlen", 3, { 5 } }, { "strcmp", 4, { 5, 5 } },
            };
            for (unsigned k = 0; k < sizeof lib / sizeof lib[0]; k++) {
                if (strcmp(tok->name + 10, lib[k].name)) continue;
                Token *t = copy_token(tok);
                t->next = tok->next;
                t->name = intern(lib[k].name, strlen(lib[k].name));
                if (!find_sym(t->name)) {
                    Type *ts[6] = { 0, pointer_to(ty_void), pointer_to(qualify(ty_void, 1, 0)), ty_ulong, ty_int, pointer_to(qualify(ty_char, 1, 0)) };
                    Type *ft = func_type(ts[lib[k].ret]), head = { 0 }, *cur = &head;
                    for (int i = 0; i < 3 && lib[k].p[i]; i++) cur = cur->next = copy_type(ts[lib[k].p[i]]);
                    ft->params = head.next;
                    VarAttr va = { 0 };
                    Scope *sc = scope;
                    scope = &global_scope;
                    push_sym(t->name)->var = declare_function(ft, &va, t, 0);
                    scope = sc;
                }
                tok = t;
                break;
            }
        }
        if (!strncmp(tok->name, "__builtin_", 10) || !strncmp(tok->name, "__atomic_", 9) || !strncmp(tok->name, "__sync_", 7)) {
            Sym *s = find_sym(tok->name);
            if (!s && tok_is(tok->next, '(')) return builtin(rest, tok);
        }
        if (tok_id(tok, "__func__") || tok_id(tok, "__FUNCTION__") || tok_id(tok, "__PRETTY_FUNCTION__")) {
            if (!cur_fn) error_at(tok, "__func__ outside a function");
            *rest = tok->next;
            int n = strlen(cur_fn->name);
            char *p = arena(n + 1);
            memcpy(p, cur_fn->name, n);
            return new_var_node(new_string(p, n + 1, array_of(ty_char, n + 1)), tok);
        }
        Sym *s = find_sym(tok->name);
        *rest = tok->next;
        if (s && s->var) return new_var_node(s->var, tok);
        if (s && s->enum_ty) { Node *n = new_num(s->enum_val, tok); n->ty = s->enum_ty->is_unsigned ? ty_uint : s->enum_ty->size == 8 ? ty_long : ty_int; return n; }
        if (tok_is(tok->next, '(')) error_at(tok, "implicit declaration of function '%s'", tok->name);
        error_at(tok, "undeclared identifier '%s'", tok->name);
    }
    error_at(tok, "expected an expression");
}

/* ---- Functions and globals */
/* A function's declaration (with or without a body to follow). */
static Obj *declare_function(Type *ty, VarAttr *a, Token *name, const char *asm_name)
{
    Sym *s = map_get(&global_scope.syms, name->name);
    Obj *fn = s ? s->var : 0;
    if (fn && !fn->is_func) error_at(name, "'%s' redeclared as a function", name->name);
    if (!fn) {
        Scope *sc = scope;
        scope = &global_scope;
        fn = new_gvar(name->name, ty);
        scope = sc;
        fn->is_func = 1;
        fn->is_extern = 1;
        fn->tok = name;
    } else if (!fn->is_def) fn->ty = ty;
    if (a->is_static) fn->is_static = 1;
    if (a->noinline) fn->noinline = 1;
    if (a->always_inline) fn->always_inline = 1;
    if (asm_name) fn->asm_name = asm_name;
    if (a->is_inline) fn->is_inline = 1;
    if (a->is_extern) fn->is_extern_decl = 1;
    if (a->is_noreturn || ty->noreturn) fn->is_noreturn = 1;
    if (a->section) fn->section = a->section;
    if (a->used) fn->used = 1;
    return fn;
}

/* A function's body. */
static Token *define_function(Token *tok, Obj *fn, Token *name)
{
    if (fn->is_def) error_at(name, "'%s' is defined twice", name->name);
    if (fn->is_inline && !fn->is_extern_decl) fn->is_static = 1;   /* a plain inline definition stays local */
    Type *ty = fn->ty;
    fn->is_def = 1;
    fn->is_extern = 0;
    fn->tok = name;
    cur_fn = cur_owner = fn;
    cur_locals = 0;
    enter();
    Obj *params = 0, **tail = &params;
    fn_gotos.n = fn_labels.n = 0;
    fn_label_names = (Map){ 0 };
    vla_scope = 0;
    block_sp = 0;
    Node *sizes = new_node(ND_NULL, name);
    for (Type *p = ty->params; p; p = p->next) {
        if (!p->name) error_at(name, "a parameter name is missing");
        Obj *v = p->pobj;
        if (v) { v->next = cur_locals; cur_locals = v; push_sym(v->name)->var = v; }
        else v = new_lvar(p->name->name, p);
        v->is_param = 1;
        v->tok = p->name;
        *tail = v;
        tail = &v->pnext;
    }
    for (Type *p = ty->params; p; p = p->next)       /* int a[n][m]: the size of a row, from m */
        if (has_vla(p)) { Node *z = vla_sizes(p, name); sizes = sizes->kind == ND_NULL ? z : new_binary(ND_COMMA, sizes, z, name); }
    fn->params = params;
    if (ty->is_variadic) {
        /* the argument registers, saved for va_arg: System V rdi..r9 and xmm0-7;
         * AAPCS64 x0-x7 (64 bytes) then q0-q7 (128) */
        fn->va_area = new_lvar(intern("__va_area__", 11), array_of(ty_char, target == T_AARCH64 ? 192 : 176));
        fn->va_area->addr_taken = 1;
        fn->va_area->align = 16;                   /* saved with movaps */
    }
    fn->body = compound_stmt(&tok, skip(tok, '{'));
    if (sizes->kind != ND_NULL) {
        Node *s0 = new_unary(ND_EXPR_STMT, sizes, name);
        s0->next = fn->body->body;
        fn->body->body = s0;
    }
    for (int i = 0; i < fn_gotos.n; i++) {             /* a goto out of VLA blocks gives their space back */
        Node *g = fn_gotos.v[i];
        int found = 0;
        for (int k = 0; k < fn_labels.n; k++) {
            Node *l = fn_labels.v[k];
            if (l->label != g->label) continue;
            found = 1;
            if (g->kind == ND_LABEL_VAL) l->val = 1;   /* its address is taken: a computed goto may come */
            else g->var = vla_restore(g->vla, l->vla, g->tok);
        }
        if (!found) error_at(g->tok->next, "label '%s' is not defined", g->tok->next->name);
    }
    leave();
    fn->locals = cur_locals;
    cur_fn = cur_owner = 0;
    return tok;
}

static Token *global_decl(Token *tok, Type *base, VarAttr *a)
{
    for (int first = 1; !consume(&tok, tok, ';'); first = 0) {
        if (!first) tok = next_declarator(tok);
        Token *name;
        VarAttr va = *a;
        Type *ty = declarator(&tok, tok, base, &name);
        const char *asm_name = 0;
        tok = asm_label(tok, &asm_name);
        tok = attributes(tok, &va);
        if (va.vecsize) { ty = vector_of(ty, va.vecsize, tok); va.vecsize = 0; }
        if (!name) error_at(tok, "variable name missing");
        if (va.is_typedef) {
            push_sym(name->name)->tdef = ty;
            continue;
        }
        if (ty->kind == TY_FUNC) {
            if (va.is_noreturn) ty->noreturn = 1;
            Obj *fn = declare_function(ty, &va, name, asm_name);
            if (tok_is(tok, '{')) {
                if (!first) error_at(tok, "a function definition must be alone");
                return define_function(tok, fn, name);
            }
            continue;
        }
        Sym *s = map_get(&global_scope.syms, name->name);
        Obj *g = s ? s->var : 0;
        if (g && g->is_func) error_at(name, "'%s' redeclared as a variable", name->name);
        if (!g) {
            g = new_gvar(name->name, ty);
            g->tok = name;
        } else if (ty->kind == TY_ARRAY && ty->len >= 0) g->ty = ty;     /* extern int a[]; int a[3]; */
        if (asm_name) g->asm_name = asm_name;
        if (va.is_static) g->is_static = 1;
        if (va.is_tls) g->is_tls = 1;
        else if (g->is_tls) error_at(name, "'%s' was declared thread-local", name->name);
        if (va.aligned > g->align) g->align = va.aligned;
        if (va.align > g->align) g->align = va.align;
        if (va.section) g->section = va.section;
        if (va.used) g->used = 1;
        if (tok_is(tok, '=')) {
            if (g->has_init) error_at(name, "'%s' is initialized twice", name->name);
            g->is_def = 1;
            g->is_extern = 0;
            gvar_initializer(&tok, tok->next, g);
        } else if (!va.is_extern) {
            g->is_def = 1;                               /* a tentative definition: zero */
            g->is_extern = 0;
        } else if (!g->is_def) g->is_extern = 1;
    }
    return tok;
}

static void toplevel_asm(Token **rest, Token *tok)
{
    tok = skip(tok->next, '(');
    char *p;
    int len;
    Type *ty;
    tok = string_literal(tok, &p, &len, &ty);
    buf_s(&global_asm, p);
    buf_c(&global_asm, '\n');
    tok = skip(tok, ')');
    *rest = skip(tok, ';');
}

Obj *parse(Token *tok)
{
    static int runs;
    if (runs++) {                               /* a new translation unit: start clean */
        global_scope = (Scope){ 0 };
        scope = &global_scope;
        globals = 0;
        globals_tail = &globals;
        strlits = (Map){ 0 };
        global_asm.len = 0;
    }
    while (tok->kind != TK_EOF) {
        if (tok_is(tok, ';')) { tok = tok->next; continue; }
        if (tok->kw == K_ASM) { toplevel_asm(&tok, tok); continue; }
        if (tok->kw == K_STATIC_ASSERT) { static_assert_decl(&tok, tok); continue; }
        if (tok->kw == K_EXTENSION) { tok = tok->next; continue; }
        VarAttr a = { 0 };
        Type *base = declspec(&tok, tok, &a);
        tok = global_decl(tok, base, &a);
    }
    return globals;
}
