/*
 * pp.c - The C preprocessor: #include, #define, #if..., macro expansion.
 *
 * Macro expansion follows the standard's rules with "hidesets" (Prosser's
 * algorithm): each token carries the set of macros it came out of, and a
 * macro is never expanded inside its own expansion. A function-like
 * macro's arguments are fully expanded before they are substituted, except
 * next to # (stringizing) and ## (pasting), which take them as written.
 *
 * Header guards (#ifndef X / #define X ... #endif around a whole file) and
 * #pragma once are remembered, so a header included again is not even read.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "sicc.h"

int opt_asm_mode, opt_freestanding;

typedef struct Macro {
    const char *name;
    int is_func, variadic, deleted;
    const char **params;            /* parameter names (interned); __VA_ARGS__ last if variadic */
    int nparams;
    Token *body;
    int dyn;                        /* __FILE__, __LINE__...: computed at each use */
} Macro;

struct Hideset { Hideset *next; const char *name; };

typedef struct Cond { struct Cond *next; int taken, in_else; Token *tok; } Cond;

static Map macros;                  /* name -> Macro */
static Map guards;                  /* file path -> header guard macro name */
static Map once;                    /* file path -> 1 for #pragma once */
static Cond *conds;
static Vec incdirs;
static Buf predef;
static int counter;
enum { DYN_FILE = 1, DYN_LINE, DYN_COUNTER, DYN_DATE, DYN_TIME };

static Token *expand(Token *tok);

/* ---- Hidesets */
static int hs_has(Hideset *h, const char *name) { for (; h; h = h->next) if (h->name == name) return 1; return 0; }
static Hideset *hs_add(Hideset *h, const char *name)
{
    Hideset *n = arena(sizeof *n);
    n->name = name;
    n->next = h;
    return n;
}
static Hideset *hs_union(Hideset *a, Hideset *b)
{
    for (; a; a = a->next) if (!hs_has(b, a->name)) b = hs_add(b, a->name);
    return b;
}
static Hideset *hs_inter(Hideset *a, Hideset *b)
{
    Hideset *r = 0;
    for (; a; a = a->next) if (hs_has(b, a->name)) r = hs_add(r, a->name);
    return r;
}

/* ---- Token list helpers */
static Token *append(Token *a, Token *b)         /* a (until EOF), copied, then b */
{
    Token head = { 0 }, *cur = &head;
    for (; a->kind != TK_EOF; a = a->next) cur = cur->next = copy_token(a);
    cur->next = b;
    return head.next;
}

static Token *skip_line(Token *t)                /* to the next line, ignoring the rest */
{
    while (!t->bol) t = t->next;
    return t;
}

static Token *copy_line(Token **rest, Token *t)  /* the rest of the line, ended by an EOF */
{
    Token head = { 0 }, *cur = &head;
    for (; !t->bol; t = t->next) cur = cur->next = copy_token(t);
    cur->next = new_eof(t);
    *rest = t;
    return head.next;
}

static int is_hash(Token *t) { return t->bol && tok_is(t, '#'); }

static Macro *find_macro(Token *t)
{
    if (t->kind != TK_IDENT) return 0;
    Macro *m = map_get(&macros, t->name);
    return m && !m->deleted ? m : 0;
}

/* A token made of new text (pasting, stringizing, numbers). */
static Token *new_token_text(const char *text, Token *at)
{
    File *f = new_file(at->file ? at->file->name : "<built-in>", text);
    Token *t = tokenize(f);
    t->file = at->file;
    t->line = at->line;
    t->origin = at->origin;
    t->space = at->space;
    t->bol = 0;
    t->hs = at->hs;
    return t;
}

static Token *new_num(long v, Token *at)
{
    return new_token_text(fmt("%ld", v), at);
}

static char *quote(const char *s, int n)         /* a C string literal of s */
{
    Buf b = { 0 };
    buf_c(&b, '"');
    for (int i = 0; i < n; i++) {
        if (s[i] == '\\' || s[i] == '"') buf_c(&b, '\\');
        buf_c(&b, s[i]);
    }
    buf_c(&b, '"');
    char *r = xstrndup(b.p, b.len);
    free(b.p);
    return r;
}

/* #x: the argument's spelling, as a string literal */
static Token *stringize(Token *arg, Token *at)
{
    Buf b = { 0 };
    for (Token *t = arg; t->kind != TK_EOF; t = t->next) {
        if (t != arg && t->space) buf_c(&b, ' ');
        buf_add(&b, t->loc, t->len);
    }
    Token *r = new_token_text(quote(b.p ? b.p : "", b.len), at);
    free(b.p);
    return r;
}

/* a ## b: one token made of both spellings */
static Token *paste(Token *a, Token *b)
{
    char *s = fmt("%.*s%.*s", a->len, a->loc, b->len, b->loc);
    Token *t = new_token_text(s, a);
    if (t->next->kind != TK_EOF) error_at(a, "pasting \"%.*s\" and \"%.*s\" does not give one token", a->len, a->loc, b->len, b->loc);
    return t;
}

/* ---- #define */
static void read_define(Token **rest, Token *t)
{
    if (t->kind != TK_IDENT) error_at(t, "macro name missing");
    Macro *m = arena(sizeof *m);
    m->name = t->name;
    t = t->next;
    if (tok_is(t, '(') && !t->space) {               /* function-like */
        m->is_func = 1;
        Vec ps = { 0 };
        t = t->next;
        while (!tok_is(t, ')')) {
            if (ps.n) { if (!tok_is(t, ',')) error_at(t, "expected ',' in the macro's parameters"); t = t->next; }
            if (tok_is(t, P_ELLIPSIS)) {
                m->variadic = 1;
                vec_push(&ps, (void *)intern("__VA_ARGS__", 11));
                t = t->next;
                break;
            }
            if (t->kind != TK_IDENT) error_at(t, "expected a parameter name");
            vec_push(&ps, (void *)t->name);
            t = t->next;
            if (tok_is(t, P_ELLIPSIS)) { m->variadic = 1; t = t->next; break; }   /* GNU: args... */
        }
        if (!tok_is(t, ')')) error_at(t, "expected ')'");
        t = t->next;
        m->params = (const char **)ps.v;
        m->nparams = ps.n;
    }
    m->body = copy_line(rest, t);
    map_put(&macros, m->name, m);
}

/* ---- Macro arguments */
typedef struct { Token *raw, *exp; } Arg;

static Token *read_one_arg(Token **rest, Token *t, int all)   /* all: take the commas too */
{
    Token head = { 0 }, *cur = &head;
    int depth = 0;
    for (;;) {
        if (t->kind == TK_EOF) error_at(t, "unterminated macro call");
        if (depth == 0 && (tok_is(t, ')') || (!all && tok_is(t, ',')))) break;
        if (tok_is(t, '(')) depth++;
        if (tok_is(t, ')')) depth--;
        cur = cur->next = copy_token(t);
        t = t->next;
    }
    cur->next = new_eof(t);
    *rest = t;
    return head.next;
}

static Arg *read_args(Token **rest, Token *t, Macro *m, Token **rparen)
{
    Arg *args = arena(sizeof(Arg) * (m->nparams + 1));
    t = t->next;                                   /* past '(' */
    int named = m->nparams - (m->variadic ? 1 : 0);
    for (int i = 0; i < named; i++) {
        if (i) { if (!tok_is(t, ',')) error_at(t, "too few arguments to macro %s", m->name); t = t->next; }
        args[i].raw = read_one_arg(&t, t, 0);
    }
    if (m->variadic) {
        if (named && tok_is(t, ',')) t = t->next;
        if (tok_is(t, ')')) args[named].raw = new_eof(t);
        else args[named].raw = read_one_arg(&t, t, 1);
    } else if (!tok_is(t, ')')) error_at(t, "too many arguments to macro %s", m->name);
    *rparen = t;
    *rest = t->next;
    return args;
}

static int find_param(Macro *m, Token *t)
{
    if (t->kind != TK_IDENT) return -1;
    for (int i = 0; i < m->nparams; i++) if (m->params[i] == t->name) return i;
    return -1;
}

static Token *expanded(Arg *a)
{
    if (!a->exp) a->exp = expand(append(a->raw, new_eof(a->raw)));   /* a copy: expand relinks */
    return a->exp;
}

/* The macro's body with its parameters replaced. */
static Token *subst(Macro *m, Arg *args, Token *at)
{
    Token head = { 0 }, *cur = &head;
    const char *va_opt = intern("__VA_OPT__", 10);
    for (Token *t = m->body; t->kind != TK_EOF; t = t->next) {
        int p;
        if (tok_is(t, '#') && (p = find_param(m, t->next)) >= 0) {           /* #param */
            cur = cur->next = stringize(args[p].raw, t);
            t = t->next;
            continue;
        }
        if (tok_is(t, ',') && tok_is(t->next, P_HASHHASH) && m->variadic &&    /* GNU , ## __VA_ARGS__ */
            find_param(m, t->next->next) == m->nparams - 1) {
            Arg *va = &args[m->nparams - 1];
            if (va->raw->kind != TK_EOF) {
                cur = cur->next = copy_token(t);
                for (Token *a = va->raw; a->kind != TK_EOF; a = a->next) cur = cur->next = copy_token(a);
            }
            t = t->next->next;
            continue;
        }
        if (t->kind == TK_IDENT && t->name == va_opt && tok_is(t->next, '(')) {   /* __VA_OPT__(x) */
            Token *s = t->next->next, *e = s;
            int depth = 0;
            for (; e->kind != TK_EOF && !(depth == 0 && tok_is(e, ')')); e = e->next) {
                if (tok_is(e, '(')) depth++;
                if (tok_is(e, ')')) depth--;
            }
            if (m->variadic && args[m->nparams - 1].raw->kind != TK_EOF) {
                Macro sub = *m;
                Token *body = 0, **bp = &body;
                for (Token *x = s; x != e; x = x->next) { *bp = copy_token(x); bp = &(*bp)->next; }
                *bp = new_eof(e);
                sub.body = body;
                Token *r = subst(&sub, args, at);
                for (; r->kind != TK_EOF; r = r->next) cur = cur->next = r;
            }
            t = e;
            continue;
        }
        if (tok_is(t, P_HASHHASH)) {                                       /* x ## y */
            if (cur == &head) error_at(t, "'##' cannot start a macro");
            Token *r = t->next;
            if (r->kind == TK_EOF) error_at(t, "'##' cannot end a macro");
            if ((p = find_param(m, r)) >= 0) {
                Token *a = args[p].raw;
                if (a->kind != TK_EOF) {
                    *cur = *paste(cur, a);
                    for (a = a->next; a->kind != TK_EOF; a = a->next) cur = cur->next = copy_token(a);
                }
            } else *cur = *paste(cur, r);
            cur->next = 0;
            t = r;
            continue;
        }
        if ((p = find_param(m, t)) >= 0) {
            if (tok_is(t->next, P_HASHHASH)) {                             /* param ## ...: as written */
                Token *a = args[p].raw;
                if (a->kind == TK_EOF) {
                    int q = find_param(m, t->next->next);
                    if (q >= 0) {                                          /* empty ## param */
                        for (a = args[q].raw; a->kind != TK_EOF; a = a->next) cur = cur->next = copy_token(a);
                        t = t->next->next;
                    } else if (t->next->next->kind != TK_EOF) {
                        cur = cur->next = copy_token(t->next->next);
                        t = t->next->next;
                    }
                    continue;
                }
                for (; a->kind != TK_EOF; a = a->next) cur = cur->next = copy_token(a);
                continue;
            }
            Token *a = expanded(&args[p]);
            int first = 1;
            for (; a->kind != TK_EOF; a = a->next) {
                cur = cur->next = copy_token(a);
                if (first) { cur->space = t->space; first = 0; }
            }
            continue;
        }
        cur = cur->next = copy_token(t);
    }
    cur->next = new_eof(at);
    return head.next;
}

static Token *dyn_macro(Macro *m, Token *t)
{
    Token *o = t;
    while (o->origin) o = o->origin;
    switch (m->dyn) {
    case DYN_FILE: return new_token_text(quote(o->file->name, strlen(o->file->name)), t);
    case DYN_LINE: return new_num(o->line, t);
    case DYN_COUNTER: return new_num(counter++, t);
    case DYN_DATE: return new_token_text("\"Jan  1 2026\"", t);
    default: return new_token_text("\"00:00:00\"", t);
    }
}

/* If tok names a macro to expand here, replace it: *rest is what follows,
 * starting with the expansion, which is scanned again. */
static int expand_macro(Token **rest, Token *tok)
{
    if (tok->kind != TK_IDENT || hs_has(tok->hs, tok->name)) return 0;
    Macro *m = find_macro(tok);
    if (!m) return 0;
    if (m->dyn) {
        Token *t = dyn_macro(m, tok);
        t->next = tok->next;
        *rest = t;
        return 1;
    }
    if (!m->is_func) {
        Hideset *hs = hs_add(tok->hs, m->name);
        Token head = { 0 }, *cur = &head;
        for (Token *b = m->body; b->kind != TK_EOF; b = b->next) {
            cur = cur->next = copy_token(b);
            cur->hs = hs_union(cur->hs, hs);
            cur->origin = tok;
            cur->bol = 0;
        }
        if (head.next) { head.next->space = tok->space; head.next->bol = tok->bol; }
        cur->next = tok->next;
        *rest = head.next ? head.next : tok->next;
        return 1;
    }
    if (!tok_is(tok->next, '(')) return 0;             /* a function-like macro's name alone */
    Token *rparen, *after;
    Arg *args = read_args(&after, tok->next, m, &rparen);
    Hideset *hs = hs_add(hs_inter(tok->hs, rparen->hs), m->name);
    Token *body = subst(m, args, tok);
    Token head = { 0 }, *cur = &head;
    for (Token *b = body; b->kind != TK_EOF; b = b->next) {
        cur = cur->next = b;
        b->hs = hs_union(b->hs, hs);
        b->origin = tok;
        b->bol = 0;
    }
    if (head.next) { head.next->space = tok->space; head.next->bol = tok->bol; }
    cur->next = after;
    *rest = head.next ? head.next : after;
    return 1;
}

/* ---- #if expressions: integer arithmetic (intmax_t / uintmax_t) */
typedef struct { long v; int u; } Val;
static Token *ce;                               /* the expression's current token */

static Val pp_num(Token *t)
{
    Val r = { 0, 0 };
    if (t->kind == TK_CHAR) {
        const char *p = t->loc;
        while (*p != '\'') p++;
        p++;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': r.v = '\n'; break;
            case 't': r.v = '\t'; break;
            case 'r': r.v = '\r'; break;
            case '0': r.v = 0; break;
            case 'x': r.v = strtoull(p + 1, 0, 16); break;
            default: r.v = *p;
            }
        } else r.v = (signed char)*p;
        return r;
    }
    const char *p = t->loc;
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) base = 16, p += 2;
    else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) base = 2, p += 2;
    else if (p[0] == '0') base = 8;
    char *end;
    unsigned long x = strtoull(p, &end, base);
    for (; end < t->loc + t->len; end++) {
        if (*end == 'u' || *end == 'U') r.u = 1;
        else if (*end != 'l' && *end != 'L') error_at(t, "invalid number in #if");
    }
    r.v = x;
    if ((long)x < 0) r.u = 1;
    return r;
}

static Val cond_expr(void);
static Val primary(void)
{
    Token *t = ce;
    if (tok_is(t, '(')) {
        ce = t->next;
        Val v = cond_expr();
        if (!tok_is(ce, ')')) error_at(ce, "expected ')' in #if");
        ce = ce->next;
        return v;
    }
    if (t->kind == TK_NUM || t->kind == TK_CHAR) { ce = t->next; return pp_num(t); }
    if (t->kind == TK_IDENT) {                           /* an unknown identifier is 0 */
        ce = t->next;
        Val v = { tok_id(t, "true"), 0 };
        return v;
    }
    error_at(t, "invalid #if expression");
}

static Val unary(void)
{
    Token *t = ce;
    if (tok_is(t, '+')) { ce = t->next; return unary(); }
    if (tok_is(t, '-')) { ce = t->next; Val v = unary(); v.v = -v.v; return v; }
    if (tok_is(t, '~')) { ce = t->next; Val v = unary(); v.v = ~v.v; return v; }
    if (tok_is(t, '!')) { ce = t->next; Val v = unary(); v.v = !v.v; v.u = 0; return v; }
    return primary();
}

static int prec(Token *t)
{
    if (t->kind != TK_PUNCT) return 0;
    switch (t->punct) {
    case '*': case '/': case '%': return 10;
    case '+': case '-': return 9;
    case P_SHL: case P_SHR: return 8;
    case '<': case '>': case P_LE: case P_GE: return 7;
    case P_EQ: case P_NE: return 6;
    case '&': return 5;
    case '^': return 4;
    case '|': return 3;
    case P_AND: return 2;
    case P_OR: return 1;
    }
    return 0;
}

static Val binary(int minp)
{
    Val l = unary();
    for (;;) {
        Token *op = ce;
        int p = prec(op);
        if (!p || p < minp) return l;
        ce = op->next;
        Val r = binary(p + 1);
        int u = l.u || r.u;
        unsigned long a = l.v, b = r.v;
        long v;
        switch (op->punct) {
        case '*': v = a * b; break;
        case '/': if (!b) error_at(op, "division by zero in #if"); v = u ? (long)(a / b) : l.v / r.v; break;
        case '%': if (!b) error_at(op, "division by zero in #if"); v = u ? (long)(a % b) : l.v % r.v; break;
        case '+': v = a + b; break;
        case '-': v = a - b; break;
        case P_SHL: v = a << (b & 63); break;
        case P_SHR: v = l.u ? (long)(a >> (b & 63)) : l.v >> (b & 63); break;
        case '<': v = u ? a < b : l.v < r.v; u = 0; break;
        case '>': v = u ? a > b : l.v > r.v; u = 0; break;
        case P_LE: v = u ? a <= b : l.v <= r.v; u = 0; break;
        case P_GE: v = u ? a >= b : l.v >= r.v; u = 0; break;
        case P_EQ: v = a == b; u = 0; break;
        case P_NE: v = a != b; u = 0; break;
        case '&': v = a & b; break;
        case '^': v = a ^ b; break;
        case '|': v = a | b; break;
        case P_AND: v = l.v && r.v; u = 0; break;
        default: v = l.v || r.v; u = 0; break;
        }
        l.v = v;
        l.u = u;
    }
}

static Val cond_expr(void)
{
    Val c = binary(1);
    if (!tok_is(ce, '?')) return c;
    ce = ce->next;
    Val a = cond_expr();
    if (!tok_is(ce, ':')) error_at(ce, "expected ':' in #if");
    ce = ce->next;
    Val b = cond_expr();
    Val r = c.v ? a : b;
    r.u = a.u || b.u;
    return r;
}

static const char *search_include(const char *name, int quoted, File *from);

/* The value of the #if/#elif line starting at t. */
static long eval_if(Token **rest, Token *t)
{
    Token *start = t;
    Token *line = copy_line(rest, t);
    /* defined X, defined(X), __has_include(...) first: before macro expansion */
    Token head = { 0 }, *cur = &head;
    for (Token *x = line; x->kind != TK_EOF; x = x->next) {
        if (tok_id(x, "defined")) {
            Token *s = x->next;
            int paren = tok_is(s, '(');
            if (paren) s = s->next;
            if (s->kind != TK_IDENT) error_at(s, "expected a macro name after 'defined'");
            cur = cur->next = new_num(find_macro(s) ? 1 : 0, s);
            x = s;
            if (paren) { x = x->next; if (!tok_is(x, ')')) error_at(x, "expected ')'"); }
            continue;
        }
        if (tok_id(x, "__has_include") && tok_is(x->next, '(')) {
            Token *s = x->next->next;
            const char *name;
            int quoted;
            if (s->kind == TK_STR) { name = xstrndup(s->loc + 1, s->len - 2); quoted = 1; s = s->next; }
            else {
                Buf b = { 0 };
                for (s = s->next; !tok_is(s, '>'); s = s->next) {
                    if (s->kind == TK_EOF) error_at(x, "expected '>'");
                    buf_add(&b, s->loc, s->len);
                }
                name = xstrndup(b.p, b.len);
                s = s->next;
                quoted = 0;
            }
            if (!tok_is(s, ')')) error_at(s, "expected ')'");
            cur = cur->next = new_num(search_include(name, quoted, start->file) != 0, x);
            x = s;
            continue;
        }
        if (tok_id(x, "__has_attribute") || tok_id(x, "__has_builtin") || tok_id(x, "__has_feature") ||
            tok_id(x, "__has_extension") || tok_id(x, "__has_c_attribute") || tok_id(x, "__has_cpp_attribute")) {
            Token *s = x->next;
            int depth = 0;
            for (; s->kind != TK_EOF; s = s->next) {
                if (tok_is(s, '(')) depth++;
                if (tok_is(s, ')') && --depth == 0) break;
            }
            cur = cur->next = new_num(0, x);
            x = s;
            continue;
        }
        cur = cur->next = copy_token(x);
    }
    cur->next = new_eof(t);
    Token *e = expand(head.next);
    ce = e;
    Val v = cond_expr();
    if (ce->kind != TK_EOF) error_at(ce, "extra tokens in #if");
    return v.v;
}

/* Skip a false #if group: to the matching #elif, #else or #endif. */
static Token *skip_cond(Token *t)
{
    int depth = 0;
    for (; t->kind != TK_EOF; t = t->next) {
        if (!is_hash(t)) continue;
        Token *d = t->next;
        if (tok_id(d, "if") || tok_id(d, "ifdef") || tok_id(d, "ifndef")) { depth++; continue; }
        if (tok_id(d, "endif")) { if (depth-- == 0) return t; continue; }
        if (depth == 0 && (tok_id(d, "elif") || tok_id(d, "else"))) return t;
    }
    return t;
}

static void push_cond(Token *t, int taken)
{
    Cond *c = arena(sizeof *c);
    c->next = conds;
    c->tok = t;
    c->taken = taken;
    conds = c;
}

/* ---- #include */
static char *dir_of(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? xstrndup(path, s - path) : ".";
}

static int exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != 0;
}

static const char *search_include(const char *name, int quoted, File *from)
{
    if (name[0] == '/') return exists(name) ? name : 0;
    if (quoted && from) {
        char *p = fmt("%s/%s", dir_of(from->name), name);
        if (exists(p)) return p;
    }
    for (int i = 0; i < incdirs.n; i++) {
        char *p = fmt("%s/%s", (char *)incdirs.v[i], name);
        if (exists(p)) return p;
    }
    return 0;
}

/* #ifndef X / #define X ... #endif covering the whole file: X, else 0 */
static const char *guard_of(Token *t)
{
    if (!is_hash(t) || !tok_id(t->next, "ifndef")) return 0;
    Token *name = t->next->next;
    if (name->kind != TK_IDENT) return 0;
    Token *d = name->next;
    if (!is_hash(d) || !tok_id(d->next, "define") || d->next->next->kind != TK_IDENT || d->next->next->name != name->name) return 0;
    int depth = 0;
    for (t = d; t->kind != TK_EOF; t = t->next) {
        if (!is_hash(t)) continue;
        Token *x = t->next;
        if (tok_id(x, "if") || tok_id(x, "ifdef") || tok_id(x, "ifndef")) depth++;
        else if (tok_id(x, "endif")) {
            if (depth-- == 0) return skip_line(x->next)->kind == TK_EOF ? name->name : 0;
        }
    }
    return 0;
}

static Token *include_file(Token *rest, const char *path, Token *at)
{
    const char *key = intern(path, strlen(path));
    if (map_get(&once, key)) return rest;
    const char *g = map_get(&guards, key);
    if (g && map_get(&macros, g) && !((Macro *)map_get(&macros, g))->deleted) return rest;
    Buf b = { 0 };
    if (read_file(path, &b) < 0) error_at(at, "cannot read %s", path);
    Token *t = tokenize(new_file(key, b.p));
    free(b.p);
    g = guard_of(t);
    if (g) map_put(&guards, key, (void *)g);
    return append(t, rest);
}

static Token *do_include(Token **rest, Token *t)
{
    Token *start = t;
    const char *name;
    int quoted;
    if (t->kind == TK_STR) {
        name = xstrndup(t->loc + 1, t->len - 2);
        quoted = 1;
        *rest = skip_line(t->next);
    } else if (tok_is(t, '<')) {
        Buf b = { 0 };
        for (t = t->next; !tok_is(t, '>'); t = t->next) {
            if (t->bol || t->kind == TK_EOF) error_at(start, "expected '>'");
            if (t->space && b.len) buf_c(&b, ' ');
            buf_add(&b, t->loc, t->len);
        }
        name = xstrndup(b.p, b.len);
        free(b.p);
        quoted = 0;
        *rest = skip_line(t->next);
    } else {                                    /* #include MACRO */
        Token *line = copy_line(rest, t);
        Token *e = expand(line);
        Token *r;
        return do_include(&r, e);
    }
    const char *path = search_include(name, quoted, start->file);
    if (!path) error_at(start, "%s: no such header", name);
    return include_file(*rest, path, start);
}

/* ---- The main loop: directives, and macro expansion of everything else */
static Token *expand(Token *tok)
{
    Token head = { 0 }, *cur = &head;
    while (tok->kind != TK_EOF) {
        if (expand_macro(&tok, tok)) continue;
        if (!is_hash(tok)) {
            cur = cur->next = tok;
            tok = tok->next;
            continue;
        }
        Token *start = tok, *d = tok->next;
        tok = d->next;
        if (d->bol) { tok = d; continue; }                       /* "#" alone */
        if (tok_id(d, "include")) { tok = do_include(&tok, tok); continue; }
        if (tok_id(d, "define")) { read_define(&tok, tok); continue; }
        if (tok_id(d, "undef")) {
            if (tok->kind != TK_IDENT) error_at(tok, "macro name missing");
            Macro *m = map_get(&macros, tok->name);
            if (m) m->deleted = 1;
            tok = skip_line(tok->next);
            continue;
        }
        if (tok_id(d, "if")) {
            long v = eval_if(&tok, tok);
            push_cond(start, v != 0);
            if (!v) tok = skip_cond(tok);
            continue;
        }
        if (tok_id(d, "ifdef") || tok_id(d, "ifndef")) {
            int v = find_macro(tok) != 0;
            if (tok_id(d, "ifndef")) v = !v;
            push_cond(start, v);
            tok = skip_line(tok->next);
            if (!v) tok = skip_cond(tok);
            continue;
        }
        if (tok_id(d, "elif")) {
            if (!conds || conds->in_else) error_at(start, "#elif without #if");
            if (!conds->taken && eval_if(&tok, tok)) conds->taken = 1;
            else tok = skip_cond(skip_line(tok));
            continue;
        }
        if (tok_id(d, "else")) {
            if (!conds || conds->in_else) error_at(start, "#else without #if");
            conds->in_else = 1;
            tok = skip_line(tok);
            if (conds->taken) tok = skip_cond(tok);
            continue;
        }
        if (tok_id(d, "endif")) {
            if (!conds) error_at(start, "#endif without #if");
            conds = conds->next;
            tok = skip_line(tok);
            continue;
        }
        if (tok_id(d, "error")) {
            Buf b = { 0 };
            for (Token *x = tok; !x->bol; x = x->next) { buf_c(&b, ' '); buf_add(&b, x->loc, x->len); }
            error_at(start, "#error%s", b.p ? b.p : "");
        }
        if (tok_id(d, "warning")) { warn_at(start, "#warning"); tok = skip_line(tok); continue; }
        if (tok_id(d, "pragma")) {
            if (tok_id(tok, "once")) map_put(&once, intern(start->file->name, strlen(start->file->name)), (void *)1);
            tok = skip_line(tok);
            continue;
        }
        if (tok_id(d, "line") || d->kind == TK_NUM || tok_id(d, "ident") || opt_asm_mode) {   /* ignored */
            tok = skip_line(tok);
            continue;
        }
        error_at(d, "unknown directive #%.*s", d->len, d->loc);
    }
    cur->next = tok;
    return head.next;
}

/* ---- Set-up */
static void defmacro(const char *s) { buf_f(&predef, "#define %s\n", s); }
static void add_dynamic(void);

void pp_define(const char *def)
{
    const char *eq = strchr(def, '=');
    if (eq) buf_f(&predef, "#define %.*s %s\n", (int)(eq - def), def, eq + 1);
    else buf_f(&predef, "#define %s 1\n", def);
}

void pp_undef(const char *name) { buf_f(&predef, "#undef %s\n", name); }
void pp_add_include(const char *dir) { vec_push(&incdirs, (void *)dir); }

void pp_init(void)
{
    static const char *defs[] = {
        "__STDC__ 1", "__STDC_VERSION__ 201710L",
        "__LP64__ 1", "_LP64 1", "__sieos__ 1", "__SICC__ 1", "__CHAR_BIT__ 8",
        "__SIZEOF_SHORT__ 2", "__SIZEOF_INT__ 4", "__SIZEOF_LONG__ 8", "__SIZEOF_LONG_LONG__ 8",
        "__SIZEOF_POINTER__ 8", "__SIZEOF_FLOAT__ 4", "__SIZEOF_DOUBLE__ 8",
        "__SIZEOF_SIZE_T__ 8", "__SIZEOF_WCHAR_T__ 4", "__SIZEOF_PTRDIFF_T__ 8", "__SIZE_TYPE__ unsigned long",
        "__PTRDIFF_TYPE__ long", "__WINT_TYPE__ unsigned int", "__INTMAX_TYPE__ long",
        "__UINTMAX_TYPE__ unsigned long", "__INTPTR_TYPE__ long", "__UINTPTR_TYPE__ unsigned long",
        "__SCHAR_MAX__ 0x7f", "__SHRT_MAX__ 0x7fff", "__INT_MAX__ 0x7fffffff",
        "__LONG_MAX__ 0x7fffffffffffffffL", "__LONG_LONG_MAX__ 0x7fffffffffffffffLL",
        "__ORDER_LITTLE_ENDIAN__ 1234", "__ORDER_BIG_ENDIAN__ 4321", "__ORDER_PDP_ENDIAN__ 3412",
        "__BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__", "__ATOMIC_RELAXED 0", "__ATOMIC_CONSUME 1",
        "__ATOMIC_ACQUIRE 2", "__ATOMIC_RELEASE 3", "__ATOMIC_ACQ_REL 4", "__ATOMIC_SEQ_CST 5",
        "__USER_LABEL_PREFIX__", "__ELF__ 1", "__SIZEOF_INT128__ 16",
    };
    for (unsigned i = 0; i < sizeof defs / sizeof defs[0]; i++) defmacro(defs[i]);
    /* what differs by machine (AArch64: SIEOS's ABI is AAPCS64 with a 64-bit long double, see docs/cc.md) */
    static const char *x86[] = { "__x86_64__ 1", "__x86_64 1", "__amd64__ 1", "__amd64 1", "__SIZEOF_LONG_DOUBLE__ 16", "__WCHAR_TYPE__ int" };
    static const char *a64[] = { "__aarch64__ 1", "__AARCH64EL__ 1", "__ARM_ARCH 8", "__ARM_64BIT_STATE 1",
        "__ARM_ARCH_ISA_A64 1", "__ARM_NEON 1", "__CHAR_UNSIGNED__ 1", "__SIZEOF_LONG_DOUBLE__ 8", "__WCHAR_TYPE__ unsigned int" };
    if (target == T_AARCH64) for (unsigned i = 0; i < sizeof a64 / sizeof a64[0]; i++) defmacro(a64[i]);
    else for (unsigned i = 0; i < sizeof x86 / sizeof x86[0]; i++) defmacro(x86[i]);
    add_dynamic();
}

/* The macros whose value changes: __FILE__, __LINE__... */
static void add_dynamic(void)
{
    static const struct { const char *n; int d; } dyn[] = {
        { "__FILE__", DYN_FILE }, { "__LINE__", DYN_LINE }, { "__COUNTER__", DYN_COUNTER },
        { "__DATE__", DYN_DATE }, { "__TIME__", DYN_TIME },
    };
    for (unsigned i = 0; i < sizeof dyn / sizeof dyn[0]; i++) {
        Macro *m = arena(sizeof *m);
        m->name = intern(dyn[i].n, strlen(dyn[i].n));
        m->dyn = dyn[i].d;
        map_put(&macros, m->name, m);
    }
}

/* Forget the macros of the previous file (several sources in one run):
 * only the predefined ones and the command line's -D/-U stay. */
static void pp_reset(void)
{
    macros = (Map){ 0 };
    guards = (Map){ 0 };
    once = (Map){ 0 };
    conds = 0;
    counter = 0;
    add_dynamic();
}

Token *preprocess(File *f)
{
    static int runs;
    if (runs++) pp_reset();
    long keep = predef.len;                     /* the definitions below are for this file only */
    if (opt_asm_mode) defmacro("__ASSEMBLER__ 1");
    defmacro(opt_freestanding ? "__STDC_HOSTED__ 0" : "__STDC_HOSTED__ 1");
    Token *pre = tokenize(new_file("<built-in>", xstrndup(predef.p ? predef.p : "", predef.len)));
    predef.len = keep;
    Token *t = tokenize(f);
    Token *all = append(pre, t);
    Token *r = expand(all);
    if (conds) error_at(conds->tok, "unterminated #if");
    return r;
}

/* Tokens back to text: one source line per line, spaces where they were. */
void pp_print(Token *t, Buf *out)
{
    int first = 1;
    for (; t->kind != TK_EOF; t = t->next) {
        if (t->bol && !first) buf_c(out, '\n');
        else if (t->space && !first) buf_c(out, ' ');
        buf_add(out, t->loc, t->len);
        first = 0;
    }
    buf_c(out, '\n');
}
