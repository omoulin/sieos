/*
 * asm.c - The assembler: AT&T-syntax x86 (16, 32 and 64-bit code), or GNU
 * syntax AArch64 (asm_a64.inc), to an ELF64 relocatable object.
 *
 * 1. Text: comments removed, statements split (newline or ';'), macros
 *    (.macro, .irp, .rept) and conditionals (.if) expanded, numeric local
 *    labels (1: ... 1b, 1f) renamed to unique names.
 * 2. Parsing: each statement becomes an item: an instruction with parsed
 *    operands, data, alignment, a label...
 * 3. Layout, repeated until stable: every item gets its offset. A jump to a
 *    nearby label in the same section starts short (2 bytes); if the label
 *    turns out too far, it grows to the long form (5-6 bytes) and the layout
 *    is redone ("relaxation"). Sizes only ever grow, so this ends.
 * 4. Encoding: the bytes, plus relocations for what only the linker knows.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "sicc.h"

/* ---- ELF constants */
enum { SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4, SHT_NOBITS = 8, SHT_NOTE = 7 };
enum { SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXEC = 4, SHF_MERGE = 0x10, SHF_STRINGS = 0x20, SHF_INFO_LINK = 0x40, SHF_TLS = 0x400 };
enum { R_64 = 1, R_PC32 = 2, R_PLT32 = 4, R_32 = 10, R_32S = 11, R_16 = 12, R_PC16 = 13, R_8 = 14, R_PC8 = 15,
       R_TPOFF64 = 18, R_GOTTPOFF = 22, R_TPOFF32 = 23 };
/* symbol suffixes: sym@tpoff (offset from the thread pointer), sym@gottpoff (a GOT slot holding it), sym@plt */
enum { M_NONE, M_TPOFF, M_GOTTPOFF, M_PLT };

/* ---- Expressions */
enum { E_NUM, E_SYM, E_BIN, E_NEG, E_NOT, E_LNOT, E_DOT };
typedef struct Expr Expr;
struct Expr { int kind, op; long v; Expr *l, *r; const char *name; };

typedef struct Sym {
    const char *name;
    int sec;                /* defining section (1..), 0 undefined, -1 absolute */
    long val;
    char defined, global, weak, local_label, type;
    long size;
    int idx;                /* in the output symbol table */
    Expr *set;              /* sym = expr */
} Sym;

typedef struct { long v; Sym *add, *sub; int mod; } Val;   /* mod: M_... of add */

typedef struct { long off; int type; Sym *sym; long addend; } Rel;

typedef struct {
    const char *name;
    int type;
    long flags, align;
    Buf data;
    long size;              /* (for .bss: no data) */
    Vec rels;
    int idx, symidx;
} Sect;

struct Object {
    Sect **secs;
    int nsecs;
    Vec syms;
};

/* ---- Operands */
enum { RC_GPR, RC_SEG, RC_CR, RC_XMM, RC_DR, RC_ST, RC_YMM };
typedef struct {
    char kind;              /* 'r' register, 'i' immediate, 'm' memory */
    char star;              /* *operand: an indirect jump/call target */
    int reg, size, rclass, high, rexb;
    Expr *e;                /* immediate, or displacement */
    int base, index, scale; /* -1: none */
    int asize;              /* address size: 2, 4 or 8 */
    int seg;                /* segment override, -1: none */
    int rip;
} Opd;

enum { IT_INSN, IT_DATA, IT_ASCII, IT_ALIGN, IT_SKIP, IT_LABEL, IT_FILL, IT_SET, IT_LOC };   /* IT_LOC: .loc (dsize: file, slen: line) */
typedef struct Item {
    int kind, sec, mode, line;
    long off, size;
    const char *mn;
    int pre[4], npre;
    Opd op[3];
    int nop;
    int relax, longj;
    int dsize;              /* data: bytes per value */
    Expr **list;
    int nlist;
    char *str;
    long slen;
    Expr *count, *fill, *fsize;
    Sym *sym;
} Item;

static Map syms;            /* name -> Sym */
static Vec symlist;
static Sect *secs[64];
static int nsecs, cursec, prevsec, mode = 64;
static Vec items;
static Vec dbg_names;       /* .file names, by number - 1 */
static const char *src_name;
static int lineno;

_Noreturn static void aerr(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    fprintf(stderr, "%s:%d: assembler: ", src_name, lineno);
    vfprintf(stderr, f, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static Sym *sym(const char *name)
{
    const char *k = intern(name, strlen(name));
    Sym *s = map_get(&syms, k);
    if (!s) {
        s = arena(sizeof *s);
        s->name = k;
        s->local_label = k[0] == '.' && k[1] == 'L';
        map_put(&syms, k, s);
        vec_push(&symlist, s);
    }
    return s;
}

static int section(const char *name, const char *flags, const char *type)
{
    for (int i = 1; i <= nsecs; i++) if (!strcmp(secs[i]->name, name)) return i;
    if (nsecs >= 62) aerr("too many sections");
    Sect *s = arena(sizeof *s);
    s->name = xstrndup(name, strlen(name));
    s->align = 1;
    s->type = SHT_PROGBITS;
    if (!strncmp(name, ".bss", 4) || !strncmp(name, ".tbss", 5)) s->type = SHT_NOBITS;
    if (!strncmp(name, ".note", 5)) s->type = SHT_NOTE;
    if (flags) {
        for (const char *p = flags; *p; p++)
            switch (*p) {
            case 'a': s->flags |= SHF_ALLOC; break;
            case 'w': s->flags |= SHF_WRITE; break;
            case 'x': s->flags |= SHF_EXEC; break;
            case 'M': s->flags |= SHF_MERGE; break;
            case 'S': s->flags |= SHF_STRINGS; break;
            case 'T': s->flags |= SHF_TLS; break;
            }
    } else if (!strncmp(name, ".text", 5)) s->flags = SHF_ALLOC | SHF_EXEC;
    else if (!strncmp(name, ".data", 5) || !strncmp(name, ".bss", 4)) s->flags = SHF_ALLOC | SHF_WRITE;
    else if (!strncmp(name, ".tdata", 6) || !strncmp(name, ".tbss", 5)) s->flags = SHF_ALLOC | SHF_WRITE | SHF_TLS;
    else if (!strncmp(name, ".rodata", 7)) s->flags = SHF_ALLOC;
    else if (strncmp(name, ".note", 5) && strncmp(name, ".comment", 8) && strncmp(name, ".debug", 6)) s->flags = SHF_ALLOC | SHF_WRITE;
    if (type) {
        if (!strcmp(type, "@nobits") || !strcmp(type, "%nobits")) s->type = SHT_NOBITS;
        else if (!strcmp(type, "@note") || !strcmp(type, "%note")) s->type = SHT_NOTE;
        else s->type = SHT_PROGBITS;
    }
    secs[++nsecs] = s;
    return nsecs;
}

/* ---- Registers */
static const char *gpr8[] = { "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b" };
static const char *gpr16[] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w" };
static const char *gpr32[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d" };
static const char *gpr64[] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
static const char *segs[] = { "es", "cs", "ss", "ds", "fs", "gs" };

static int num(const char *s) { int n = 0; while (*s >= '0' && *s <= '9') n = n * 10 + *s++ - '0'; return n; }
static int atoi_(const char *s) { while (*s == ' ') s++; return num(s); }

static int parse_reg(const char *s, int n, Opd *o)
{
    char b[8];
    if (n <= 0 || n > 6) return 0;
    memcpy(b, s, n);
    b[n] = 0;
    o->kind = 'r';
    o->high = o->rexb = 0;
    o->rclass = RC_GPR;
    for (int i = 0; i < 16; i++) {
        if (!strcmp(b, gpr64[i])) { o->reg = i; o->size = 8; return 1; }
        if (!strcmp(b, gpr32[i])) { o->reg = i; o->size = 4; return 1; }
        if (!strcmp(b, gpr16[i])) { o->reg = i; o->size = 2; return 1; }
        if (!strcmp(b, gpr8[i])) { o->reg = i; o->size = 1; o->rexb = i >= 4 && i < 8; return 1; }
    }
    static const char *hi[] = { "ah", "ch", "dh", "bh" };
    for (int i = 0; i < 4; i++) if (!strcmp(b, hi[i])) { o->reg = 4 + i; o->size = 1; o->high = 1; return 1; }
    for (int i = 0; i < 6; i++) if (!strcmp(b, segs[i])) { o->reg = i; o->size = 2; o->rclass = RC_SEG; return 1; }
    if (!strncmp(b, "cr", 2) && b[2] >= '0' && b[2] <= '9') { o->reg = num(b + 2); o->size = 8; o->rclass = RC_CR; return 1; }
    if (!strncmp(b, "db", 2) && b[2] >= '0' && b[2] <= '9') { o->reg = num(b + 2); o->size = 8; o->rclass = RC_DR; return 1; }
    if (!strncmp(b, "xmm", 3) && b[3] >= '0' && b[3] <= '9') { o->reg = num(b + 3); o->size = 16; o->rclass = RC_XMM; return 1; }
    if (!strcmp(b, "rip")) { o->reg = 16; o->size = 8; return 1; }
    return 0;
}

/* ---- Expression parsing */
static const char *ep;                         /* the current position */

static void skipws(void) { while (*ep == ' ' || *ep == '\t') ep++; }
static int is_symch(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$'; }

static Expr *new_expr(int kind) { Expr *e = arena(sizeof *e); e->kind = kind; return e; }

static Expr *expr_cond(void);
static Expr *expr_primary(void)
{
    skipws();
    if (*ep == '(') {
        ep++;
        Expr *e = expr_cond();
        skipws();
        if (*ep != ')') aerr("expected ')' in an expression");
        ep++;
        return e;
    }
    if (*ep == '-') { ep++; Expr *e = new_expr(E_NEG); e->l = expr_primary(); return e; }
    if (*ep == '+') { ep++; return expr_primary(); }
    if (*ep == '~') { ep++; Expr *e = new_expr(E_NOT); e->l = expr_primary(); return e; }
    if (*ep == '!') { ep++; Expr *e = new_expr(E_LNOT); e->l = expr_primary(); return e; }
    if (*ep == '\'') {
        Expr *e = new_expr(E_NUM);
        ep++;
        if (*ep == '\\') {
            ep++;
            e->v = *ep == 'n' ? 10 : *ep == 't' ? 9 : *ep == '0' ? 0 : *ep;
        } else e->v = (unsigned char)*ep;
        ep++;
        if (*ep == '\'') ep++;
        return e;
    }
    if (*ep >= '0' && *ep <= '9') {
        Expr *e = new_expr(E_NUM);
        char *end;
        if (ep[0] == '0' && (ep[1] == 'b' || ep[1] == 'B') && (ep[2] == '0' || ep[2] == '1')) e->v = strtoull(ep + 2, &end, 2);
        else e->v = strtoull(ep, &end, 0);
        ep = end;
        return e;
    }
    if (*ep == '.' && !is_symch(ep[1])) { ep++; return new_expr(E_DOT); }
    if (is_symch(*ep)) {
        const char *s = ep;
        while (is_symch(*ep)) ep++;
        Expr *e = new_expr(E_SYM);
        e->name = xstrndup(s, ep - s);
        if (*ep == '@') {
            const char *m = ++ep;
            while (is_symch(*ep)) ep++;
            int n = ep - m;
            char low[12] = "";
            for (int k = 0; k < n && k < 11; k++) low[k] = m[k] | 0x20;
            e->op = !strcmp(low, "tpoff") ? M_TPOFF : !strcmp(low, "gottpoff") ? M_GOTTPOFF : !strcmp(low, "plt") ? M_PLT : -1;
            if (e->op < 0) aerr("unknown symbol suffix @%.*s", n, m);
            if (e->op != M_PLT) sym(e->name)->type = 6;    /* STT_TLS, also when undefined here */
        }
        return e;
    }
    aerr("invalid expression at '%.20s'", ep);
}

static int binprec(int *op)
{
    skipws();
    const char *p = ep;
    struct { const char *s; int prec; } t[] = {
        { "||", 1 }, { "&&", 2 }, { "==", 4 }, { "!=", 4 }, { "<=", 5 }, { ">=", 5 }, { "<<", 6 }, { ">>", 6 },
        { "|", 3 }, { "^", 3 }, { "&", 3 }, { "<", 5 }, { ">", 5 }, { "+", 7 }, { "-", 7 }, { "*", 8 }, { "/", 8 }, { "%", 8 },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        int n = strlen(t[i].s);
        if (!strncmp(p, t[i].s, n)) {
                *op = t[i].s[0] << 8 | (n > 1 ? t[i].s[1] : 0);
            return t[i].prec;
        }
    }
    return 0;
}

static Expr *expr_bin(int minp)
{
    Expr *l = expr_primary();
    for (;;) {
        int op;
        int p = binprec(&op);
        if (!p || p < minp) return l;
        ep += (op & 0xff) ? 2 : 1;
        Expr *e = new_expr(E_BIN);
        e->op = op;
        e->l = l;
        e->r = expr_bin(p + 1);
        l = e;
    }
}

static Expr *expr_cond(void) { return expr_bin(1); }

static Expr *parse_expr(const char *s, const char **end)
{
    ep = s;
    Expr *e = expr_cond();
    skipws();
    if (end) *end = ep;
    return e;
}

/* ---- Expression values. dot: the current location. */
static Sym dot_sym;

static Val eval(Expr *e, int sec, long dot)
{
    Val v = { 0 }, a, b;
    switch (e->kind) {
    case E_NUM: v.v = e->v; return v;
    case E_DOT: dot_sym.sec = sec; dot_sym.val = dot; dot_sym.defined = 1; v.add = &dot_sym; return v;
    case E_SYM: {
        Sym *s = sym(e->name);
        if (s->set && s->sec == -1) { v.v = s->val; return v; }
        if (s->set) return eval(s->set, sec, dot);
        v.add = s;
        v.mod = e->op;
        return v;
    }
    case E_NEG: a = eval(e->l, sec, dot); if (a.add || a.sub) { if (a.add && !a.sub) { v.sub = a.add; v.v = -a.v; return v; } aerr("invalid use of a symbol"); } v.v = -a.v; return v;
    case E_NOT: a = eval(e->l, sec, dot); if (a.add) aerr("invalid use of a symbol"); v.v = ~a.v; return v;
    case E_LNOT: a = eval(e->l, sec, dot); if (a.add) aerr("invalid use of a symbol"); v.v = !a.v; return v;
    }
    a = eval(e->l, sec, dot);
    b = eval(e->r, sec, dot);
    int op = e->op;
    if (op == ('+' << 8)) {
        if (a.add && b.add) aerr("adding two symbols");
        v.v = a.v + b.v;
        v.add = a.add ? a.add : b.add;
        v.sub = a.sub ? a.sub : b.sub;
        v.mod = a.mod | b.mod;
        return v;
    }
    if (op == ('-' << 8)) {
        v.v = a.v - b.v;
        v.add = a.add;
        v.sub = a.sub;
        if (b.add) { if (v.sub) aerr("subtracting two symbols twice"); v.sub = b.add; }
        if (b.sub) { if (v.add) aerr("invalid symbol arithmetic"); v.add = b.sub; }
        return v;
    }
    /* a symbol difference in one section is a number by now (layout) */
    for (int k = 0; k < 2; k++) {
        Val *x = k ? &b : &a;
        if (x->add && x->sub && x->add->defined && x->sub->defined && x->add->sec == x->sub->sec) {
            x->v += x->add->val - x->sub->val;
            x->add = x->sub = 0;
        }
        if (x->add || x->sub) aerr("a symbol in an arithmetic expression");
    }
    long l = a.v, r = b.v;
    switch (op) {
    case '*' << 8: v.v = l * r; break;
    case '/' << 8: if (!r) aerr("division by zero"); v.v = l / r; break;
    case '%' << 8: if (!r) aerr("division by zero"); v.v = l % r; break;
    case ('<' << 8) | '<': v.v = (unsigned long)l << r; break;
    case ('>' << 8) | '>': v.v = (unsigned long)l >> r; break;
    case '&' << 8: v.v = l & r; break;
    case '|' << 8: v.v = l | r; break;
    case '^' << 8: v.v = l ^ r; break;
    case ('=' << 8) | '=': v.v = l == r; break;
    case ('!' << 8) | '=': v.v = l != r; break;
    case '<' << 8: v.v = l < r; break;
    case '>' << 8: v.v = l > r; break;
    case ('<' << 8) | '=': v.v = l <= r; break;
    case ('>' << 8) | '=': v.v = l >= r; break;
    case ('&' << 8) | '&': v.v = l && r; break;
    case ('|' << 8) | '|': v.v = l || r; break;
    default: aerr("unknown operator");
    }
    return v;
}

/* A value with symbol differences in one section folded in. */
static Val evalf(Expr *e, int sec, long dot)
{
    Val v = eval(e, sec, dot);
    if (v.add && v.sub && v.add->defined && v.sub->defined && v.add->sec == v.sub->sec) {
        v.v += v.add->val - v.sub->val;
        v.add = v.sub = 0;
    }
    return v;
}

static int has_sym(Expr *e)
{
    if (!e) return 0;
    if (e->kind == E_SYM) { Sym *s = sym(e->name); return !(s->set && s->sec == -1) && !(s->set && !has_sym(s->set)); }
    if (e->kind == E_DOT) return 1;
    return has_sym(e->l) || has_sym(e->r);
}

static long const_val(Expr *e, int sec, long dot)
{
    Val v = evalf(e, sec, dot);
    if (v.add || v.sub) aerr("expected a constant");
    return v.v;
}

/* ---- Operands */
static void parse_operand(const char *s, Opd *o)
{
    memset(o, 0, sizeof *o);
    o->base = o->index = o->seg = -1;
    o->scale = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '*') { o->star = 1; s++; }
    if (*s == '$') {
        o->kind = 'i';
        const char *end;
        o->e = parse_expr(s + 1, &end);
        if (*end) aerr("junk after an immediate: '%s'", end);
        return;
    }
    if (s[0] == '%' && s[1] == 's' && s[2] == 't' && (!s[3] || s[3] == '(')) {   /* x87: %st, %st(N) */
        o->kind = 'r';
        o->rclass = RC_ST;
        o->reg = s[3] == '(' ? atoi_(s + 4) : 0;
        o->base = o->index = o->seg = -1;
        return;
    }
    if (*s == '%') {
        const char *p = s + 1;
        int n = 0;
        while (is_symch(p[n])) n++;
        Opd r;
        if (!parse_reg(p, n, &r)) aerr("unknown register %%%.*s", n, p);
        if (p[n] == ':' && r.rclass == RC_SEG) { o->seg = r.reg; s = p + n + 1; }
        else {
            if (p[n]) aerr("junk after a register: '%s'", p + n);
            int star = o->star;
            *o = r;
            o->star = star;
            o->base = o->index = o->seg = -1;
            return;
        }
    }
    /* memory: [disp](base, index, scale) */
    o->kind = 'm';
    const char *paren = 0;
    for (const char *p = s; *p; p++) {                 /* the "(%" or "(," that starts the registers */
        if (p[0] == '(') {
            const char *q = p + 1;
            while (*q == ' ') q++;
            if (*q == '%' || *q == ',') { paren = p; break; }
        }
    }
    if (paren != s) {
        const char *end;
        char *d = xstrndup(s, paren ? paren - s : (long)strlen(s));
        o->e = parse_expr(d, &end);
        if (*end) aerr("junk in an address: '%s'", end);
    }
    if (!paren) return;
    const char *p = paren + 1;
    Opd r;
    int asize = 0;
    for (int part = 0; *p && *p != ')'; part++) {
        while (*p == ' ') p++;
        if (part > 0) { if (*p != ',') aerr("expected ','"); p++; while (*p == ' ') p++; }
        if (part == 2) { o->scale = strtol(p, (char **)&p, 0); continue; }
        if (*p != '%') { if (part == 0 && *p == ',') { part = 0; continue; } aerr("expected a register in an address"); }
        p++;
        int n = 0;
        while (is_symch(p[n])) n++;
        if (!parse_reg(p, n, &r)) aerr("unknown register %%%.*s", n, p);
        if (r.reg == 16) o->rip = 1;
        else if (part == 0) o->base = r.reg;
        else o->index = r.reg;
        if (r.reg != 16) asize = r.size;
        p += n;
    }
    o->asize = asize;
}

/* ---- Text: statements, macros, conditionals */
typedef struct { const char *name; Vec params, defaults; Vec body; } Macro;
static Map macros;
static Vec stmts;                                 /* the expanded statements */
static Vec stmt_lines;
static int numlab[10];                            /* numeric labels: definitions so far */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

/* split text into statements at newlines and ';' (not inside strings), dropping comments */
static void split(const char *text, long len, Vec *out, Vec *lines, int line0)
{
    Buf cur = { 0 };
    int line = line0;
    for (long i = 0; i <= len; i++) {
        char c = i < len ? text[i] : '\n';
        if (c == '"') {
            buf_c(&cur, c);
            for (i++; i < len && text[i] != '"'; i++) {
                if (text[i] == '\\') buf_c(&cur, text[i++]);
                buf_c(&cur, text[i]);
            }
            buf_c(&cur, '"');
            continue;
        }
        if (c == '/' && i + 1 < len && text[i + 1] == '*') {
            for (i += 2; i + 1 < len && !(text[i] == '*' && text[i + 1] == '/'); i++) if (text[i] == '\n') line++;
            i++;
            continue;
        }
        /* comments: x86 '#'; AArch64 '//' ('#' marks immediates there, a comment only first on a line) */
        int a64 = target == T_AARCH64, blank = 1;
        for (long k = 0; k < cur.len && blank; k++) blank = cur.p[k] == ' ' || cur.p[k] == '\t';
        if ((!a64 && c == '#') || (a64 && c == '/' && i + 1 < len && text[i + 1] == '/') || (a64 && c == '#' && blank)) {
            while (i < len && text[i] != '\n') i++;
            i--;
            continue;
        }
        if (c == '\n' || c == ';') {
            char *s = trim(cur.p ? cur.p : (char *)"");
            if (*s) { vec_push(out, xstrndup(s, strlen(s))); vec_push(lines, (void *)(long)line); }
            cur.len = 0;
            if (cur.p) cur.p[0] = 0;
            if (c == '\n') line++;
            continue;
        }
        buf_c(&cur, c);
    }
    free(cur.p);
}

static int word_is(const char *s, const char *w)
{
    int n = strlen(w);
    return !strncmp(s, w, n) && (s[n] == 0 || s[n] == ' ' || s[n] == '\t');
}

static char *subst_args(const char *s, Vec *names, Vec *values)
{
    Buf b = { 0 };
    for (const char *p = s; *p; ) {
        if (*p == '\\' && is_symch(p[1])) {
            const char *q = p + 1;
            int n = 0;
            while (is_symch(q[n]) && q[n] != '.' && q[n] != '$') n++;
            int k;
            for (k = 0; k < names->n; k++) if ((int)strlen(names->v[k]) == n && !strncmp(names->v[k], q, n)) break;
            if (k < names->n) {
                buf_s(&b, values->v[k]);
                p = q + n;
                if (*p == '\\' && p[1] == '(' && p[2] == ')') p += 3;     /* \arg\() */
                continue;
            }
        }
        buf_c(&b, *p++);
    }
    char *r = xstrndup(b.p ? b.p : "", b.len);
    free(b.p);
    return r;
}

/* split "a, b, c" at top-level commas */
static void split_args(const char *s, Vec *out)
{
    int depth = 0;
    Buf cur = { 0 };
    for (const char *p = s;; p++) {
        if (*p == '(') depth++;
        if (*p == ')') depth--;
        if (*p == '"') { do buf_c(&cur, *p++); while (*p && *p != '"'); }
        if (!*p || (*p == ',' && depth == 0)) {
            char *t = trim(cur.p ? cur.p : (char *)"");
            vec_push(out, xstrndup(t, strlen(t)));
            cur.len = 0;
            if (cur.p) cur.p[0] = 0;
            if (!*p) break;
            continue;
        }
        buf_c(&cur, *p);
    }
    free(cur.p);
}

static void expand(Vec *in, Vec *inlines, int from, int to);

/* collect a block's lines, to the matching end directive */
static int block_end(Vec *in, int i, int to, const char *open1, const char *open2, const char *open3, const char *close)
{
    int depth = 0;
    for (; i < to; i++) {
        char *s = in->v[i];
        if (word_is(s, open1) || (open2 && word_is(s, open2)) || (open3 && word_is(s, open3))) depth++;
        else if (word_is(s, close)) { if (depth-- == 0) return i; }
    }
    aerr("missing %s", close);
}

static long pp_const(const char *s)
{
    const char *end;
    Expr *e = parse_expr(s, &end);
    if (*end) aerr("junk in an expression: '%s'", end);
    return const_val(e, 0, 0);
}

/* a numeric label reference: "1b", "1f" -> its unique name */
static char *numeric_refs(const char *s)
{
    Buf b = { 0 };
    for (const char *p = s; *p; ) {
        if (*p >= '0' && *p <= '9' && (p == s || !is_symch(p[-1])) && (p[1] == 'b' || p[1] == 'f') && !is_symch(p[2])) {
            int n = *p - '0';
            int k = p[1] == 'b' ? numlab[n] : numlab[n] + 1;
            buf_f(&b, ".Lnum%d_%d", n, k);
            p += 2;
            continue;
        }
        buf_c(&b, *p++);
    }
    char *r = xstrndup(b.p ? b.p : "", b.len);
    free(b.p);
    return r;
}

static void expand(Vec *in, Vec *inlines, int from, int to)
{
    for (int i = from; i < to; i++) {
        char *s = in->v[i];
        lineno = (int)(long)inlines->v[i];
        if (word_is(s, ".macro")) {
            int end = block_end(in, i + 1, to, ".macro", 0, 0, ".endm");
            Macro *m = arena(sizeof *m);
            char *p = trim(s + 6);
            int n = 0;
            while (is_symch(p[n])) n++;
            m->name = intern(p, n);
            Vec args = { 0 };
            if (*trim(p + n)) split_args(trim(p + n), &args);
            for (int k = 0; k < args.n; k++) {
                char *a = args.v[k], *eq = strchr(a, '=');
                if (eq) { *eq = 0; vec_push(&m->defaults, trim(eq + 1)); } else vec_push(&m->defaults, "");
                vec_push(&m->params, trim(a));
            }
            for (int k = i + 1; k < end; k++) { vec_push(&m->body, in->v[k]); vec_push(&m->body, inlines->v[k]); }
            map_put(&macros, m->name, m);
            i = end;
            continue;
        }
        if (word_is(s, ".irp") || word_is(s, ".irpc")) {
            int irpc = word_is(s, ".irpc");
            int end = block_end(in, i + 1, to, ".irp", ".irpc", ".rept", ".endr");
            char *p = trim(s + (irpc ? 5 : 4));
            int n = 0;
            while (is_symch(p[n])) n++;
            Vec names = { 0 }, vals = { 0 };
            vec_push(&names, xstrndup(p, n));
            char *list = trim(p + n);
            if (*list == ',') list = trim(list + 1);
            Vec items_ = { 0 };
            if (irpc) for (char *c = list; *c; c++) vec_push(&items_, xstrndup(c, 1));
            else split_args(list, &items_);
            for (int k = 0; k < items_.n; k++) {
                vals.n = 0;
                vec_push(&vals, items_.v[k]);
                Vec body = { 0 }, bl = { 0 };
                for (int j = i + 1; j < end; j++) { vec_push(&body, subst_args(in->v[j], &names, &vals)); vec_push(&bl, inlines->v[j]); }
                expand(&body, &bl, 0, body.n);
            }
            i = end;
            continue;
        }
        if (word_is(s, ".rept")) {
            int end = block_end(in, i + 1, to, ".irp", ".irpc", ".rept", ".endr");
            long n = pp_const(trim(s + 5));
            for (long k = 0; k < n; k++) expand(in, inlines, i + 1, end);
            i = end;
            continue;
        }
        if (word_is(s, ".if") || word_is(s, ".ifdef") || word_is(s, ".ifndef") || word_is(s, ".ifeq") || word_is(s, ".ifne")) {
            int end = i + 1, els = -1, depth = 0;
            for (; end < to; end++) {
                char *t = in->v[end];
                if (!strncmp(t, ".if", 3)) depth++;
                else if (word_is(t, ".endif")) { if (depth-- == 0) break; }
                else if (word_is(t, ".else") && depth == 0) els = end;
            }
            if (end >= to) aerr("missing .endif");
            int c;
            if (word_is(s, ".ifdef") || word_is(s, ".ifndef")) {
                char *name = trim(s + (word_is(s, ".ifdef") ? 6 : 7));
                Sym *x = map_get(&syms, intern(name, strlen(name)));
                c = x && (x->defined || x->set);
                if (word_is(s, ".ifndef")) c = !c;
            } else if (word_is(s, ".ifeq")) c = pp_const(trim(s + 5)) == 0;
            else if (word_is(s, ".ifne")) c = pp_const(trim(s + 5)) != 0;
            else c = pp_const(trim(s + 3)) != 0;
            if (c) expand(in, inlines, i + 1, els >= 0 ? els : end);
            else if (els >= 0) expand(in, inlines, els + 1, end);
            i = end;
            continue;
        }
        /* labels first (a line may be "label: MACRO args") */
        char *t = s;
        for (;;) {
            int n = 0;
            while (is_symch(t[n])) n++;
            if (!n || t[n] != ':') break;
            if (n == 1 && t[0] >= '0' && t[0] <= '9') {              /* "N:": a unique name */
                int d = t[0] - '0';
                numlab[d]++;
                vec_push(&stmts, fmt(".Lnum%d_%d:", d, numlab[d]));
            } else vec_push(&stmts, fmt("%.*s:", n, t));
            vec_push(&stmt_lines, (void *)(long)lineno);
            t = trim(t + n + 1);
        }
        if (!*t) continue;
        /* a macro use */
        int n = 0;
        while (is_symch(t[n])) n++;
        Macro *m = n ? map_get(&macros, intern(t, n)) : 0;
        if (m) {
            Vec args = { 0 }, vals = { 0 };
            char *rest = trim(t + n);
            if (*rest) split_args(rest, &args);
            for (int k = 0; k < m->params.n; k++) vec_push(&vals, k < args.n && *(char *)args.v[k] ? args.v[k] : m->defaults.v[k]);
            Vec body = { 0 }, bl = { 0 };
            for (int k = 0; k < m->body.n; k += 2) {
                char *line = subst_args(m->body.v[k], &m->params, &vals);
                split(line, strlen(line), &body, &bl, (int)(long)m->body.v[k + 1]);
            }
            expand(&body, &bl, 0, body.n);
            continue;
        }
        vec_push(&stmts, numeric_refs(t));
        vec_push(&stmt_lines, (void *)(long)lineno);
    }
}

/* ---- Parsing statements into items */
static Item *new_item(int kind)
{
    Item *it = arena(sizeof *it);
    it->kind = kind;
    it->sec = cursec;
    it->mode = mode;
    it->line = lineno;
    vec_push(&items, it);
    return it;
}

static char *parse_string(const char *s, long *len)
{
    Buf b = { 0 };
    if (*s != '"') aerr("expected a string");
    for (s++; *s && *s != '"'; s++) {
        if (*s != '\\') { buf_c(&b, *s); continue; }
        s++;
        if (*s >= '0' && *s <= '7') {
            int v = 0;
            for (int k = 0; k < 3 && *s >= '0' && *s <= '7'; k++) v = v * 8 + *s++ - '0';
            s--;
            buf_c(&b, v);
            continue;
        }
        if (*s == 'x') {
            int v = 0;
            for (s++; (*s >= '0' && *s <= '9') || ((*s | 32) >= 'a' && (*s | 32) <= 'f'); s++) v = v * 16 + (*s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10);
            s--;
            buf_c(&b, v);
            continue;
        }
        switch (*s) {
        case 'n': buf_c(&b, 10); break;
        case 't': buf_c(&b, 9); break;
        case 'r': buf_c(&b, 13); break;
        case 'b': buf_c(&b, 8); break;
        case 'f': buf_c(&b, 12); break;
        default: buf_c(&b, *s);
        }
    }
    *len = b.len;
    char *r = arena(b.len + 1);
    if (b.len) memcpy(r, b.p, b.len);
    free(b.p);
    return r;
}

static void directive(char *s)
{
    char *p = s;
    while (*p && *p != ' ' && *p != '\t') p++;
    char *name = xstrndup(s, p - s);
    char *arg = trim(p);
    Vec args = { 0 };
#define D(x) (!strcmp(name, x))
    if (D(".text")) { cursec = section(".text", 0, 0); return; }
    if (D(".data")) { cursec = section(".data", 0, 0); return; }
    if (D(".bss")) { cursec = section(".bss", 0, 0); return; }
    if (D(".section") || D(".pushsection")) {
        split_args(arg, &args);
        if (!args.n) aerr(".section needs a name");
        const char *flags = 0, *type = 0;
        if (args.n > 1) { long n; flags = parse_string(args.v[1], &n); }
        if (args.n > 2) type = args.v[2];
        prevsec = cursec;
        cursec = section(args.v[0], flags, type);
        return;
    }
    if (D(".previous") || D(".popsection")) { int t = cursec; cursec = prevsec; prevsec = t; return; }
    if (D(".globl") || D(".global") || D(".weak") || D(".local") || D(".hidden") || D(".protected") || D(".internal")) {
        split_args(arg, &args);
        for (int i = 0; i < args.n; i++) {
            Sym *x = sym(args.v[i]);
            if (D(".globl") || D(".global")) x->global = 1;
            if (D(".weak")) x->weak = x->global = 1;
        }
        return;
    }
    if (D(".type")) {
        split_args(arg, &args);
        if (args.n == 2) sym(args.v[0])->type = strstr(args.v[1], "function") ? 2 : strstr(args.v[1], "tls_object") ? 6 : strstr(args.v[1], "object") ? 1 : 0;
        return;
    }
    if (D(".size")) {
        split_args(arg, &args);
        if (args.n == 2) { Item *it = new_item(IT_SET); it->sym = sym(args.v[0]); it->count = parse_expr(args.v[1], 0); it->dsize = 1; }
        return;
    }
    if (D(".code16")) { mode = 16; return; }
    if (D(".code32")) { mode = 32; return; }
    if (D(".code64")) { mode = 64; return; }
    if (D(".byte") || D(".word") || D(".short") || D(".value") || D(".hword") || D(".2byte") || D(".long") || D(".int") ||
        D(".4byte") || D(".quad") || D(".8byte") || D(".xword") || D(".dword")) {
        Item *it = new_item(IT_DATA);
        it->dsize = D(".byte") ? 1 : ((D(".word") && target != T_AARCH64) || D(".short") || D(".value") || D(".hword") || D(".2byte")) ? 2 :
                    (D(".quad") || D(".8byte") || D(".xword") || D(".dword")) ? 8 : 4;   /* (.word: 2 bytes on x86, 4 on AArch64) */
        split_args(arg, &args);
        it->list = arena(sizeof(Expr *) * (args.n + 1));
        for (int i = 0; i < args.n; i++) {
            const char *end;
            it->list[i] = parse_expr(args.v[i], &end);
            if (*end) aerr("junk in data: '%s'", end);
        }
        it->nlist = args.n;
        return;
    }
    if (D(".ascii") || D(".asciz") || D(".string")) {
        split_args(arg, &args);
        for (int i = 0; i < args.n; i++) {
            Item *it = new_item(IT_ASCII);
            it->str = parse_string(args.v[i], &it->slen);
            if (!D(".ascii")) it->slen++;
        }
        return;
    }
    if (D(".zero") || D(".skip") || D(".space")) {
        Item *it = new_item(IT_SKIP);
        split_args(arg, &args);
        it->count = parse_expr(args.v[0], 0);
        if (args.n > 1) it->fill = parse_expr(args.v[1], 0);
        return;
    }
    if (D(".fill")) {
        Item *it = new_item(IT_FILL);
        split_args(arg, &args);
        it->count = parse_expr(args.v[0], 0);
        it->fsize = args.n > 1 ? parse_expr(args.v[1], 0) : 0;
        it->fill = args.n > 2 ? parse_expr(args.v[2], 0) : 0;
        return;
    }
    if (D(".align") || D(".balign") || D(".p2align")) {
        Item *it = new_item(IT_ALIGN);
        split_args(arg, &args);
        long n = pp_const(args.v[0]);
        if (D(".p2align")) n = 1L << n;
        it->dsize = n < 1 ? 1 : n;
        if (args.n > 1 && *(char *)args.v[1]) it->fill = parse_expr(args.v[1], 0);
        if (args.n > 2) it->slen = pp_const(args.v[2]);   /* the most bytes to skip (else: no alignment) */
        if (it->dsize > secs[cursec]->align) secs[cursec]->align = it->dsize;
        return;
    }
    if (D(".set") || D(".equ")) {
        split_args(arg, &args);
        if (args.n != 2) aerr("%s needs a name and a value", name);
        Item *it = new_item(IT_SET);
        it->sym = sym(args.v[0]);
        it->count = parse_expr(args.v[1], 0);
        Sym *x = it->sym;
        if (!has_sym(it->count)) { x->set = it->count; x->sec = -1; x->val = const_val(it->count, 0, 0); x->defined = 1; }
        else x->set = it->count;
        return;
    }
    if (D(".intel_syntax")) aerr("Intel syntax is not supported");
    if (D(".file") && *arg >= '0' && *arg <= '9') {   /* .file N "name": a file of the line table */
        char *q;
        long n = strtol(arg, &q, 10), len;
        q = trim(q);
        if (n < 1 || n > 4096 || *q != '"') aerr("bad .file");
        while (dbg_names.n < n) vec_push(&dbg_names, "?");
        dbg_names.v[n - 1] = parse_string(q, &len);
        return;
    }
    if (D(".loc")) {                            /* .loc file line [column] [options]: a row of the line table */
        char *q;
        Item *it = new_item(IT_LOC);
        it->dsize = strtol(arg, &q, 10);
        it->slen = strtol(q, &q, 10);
        if (it->dsize < 1 || it->slen < 0) aerr("bad .loc");
        return;
    }
    if (D(".file") || D(".ident") || !strncmp(name, ".cfi", 4) || D(".addrsig") || D(".addrsig_sym") || D(".att_syntax"))
        return;                                 /* debugging and tool information: not needed */
    aerr("unknown directive %s", name);
#undef D
}

static void statement(char *s)
{
    /* labels */
    for (;;) {
        char *p = s;
        while (is_symch(*p)) p++;
        if (p > s && *p == ':') {
            Item *it = new_item(IT_LABEL);
            it->sym = sym(xstrndup(s, p - s));
            if (it->sym->defined) aerr("symbol %s defined twice", it->sym->name);
            it->sym->defined = 1;
            it->sym->sec = cursec;              /* known now: forward jumps can be short */
            s = trim(p + 1);
            if (!*s) return;
            continue;
        }
        break;
    }
    /* name = expr */
    char *eq = strchr(s, '=');
    if (eq && eq > s && eq[1] != '=') {
        char *n = s;
        while (is_symch(*n)) n++;
        if (trim(n) == eq || (*n == ' ' && trim(n)[0] == '=')) {
            Item *it = new_item(IT_SET);
            it->sym = sym(xstrndup(s, n - s));
            it->count = parse_expr(eq + 1, 0);
            if (!has_sym(it->count)) { it->sym->set = it->count; it->sym->sec = -1; it->sym->val = const_val(it->count, 0, 0); it->sym->defined = 1; }
            else it->sym->set = it->count;
            return;
        }
    }
    if (s[0] == '.' && is_symch(s[1])) { directive(s); return; }
    if (target == T_AARCH64) {                  /* AArch64: the mnemonic, and the operands as text (asm_a64.inc) */
        Item *it = new_item(IT_INSN);
        char *p = s;
        while (*p && *p != ' ' && *p != '\t') p++;
        char *m = xstrndup(s, p - s);
        for (char *q = m; *q; q++) if (*q >= 'A' && *q <= 'Z') *q |= 0x20;
        it->mn = m;
        it->str = trim(p);
        return;
    }
    /* an instruction: prefixes, mnemonic, operands */
    Item *it = new_item(IT_INSN);
    for (;;) {
        char *p = s;
        while (*p && *p != ' ' && *p != '\t') p++;
        char *m = xstrndup(s, p - s);
        int pre = !strcmp(m, "lock") ? 0xF0 : (!strcmp(m, "rep") || !strcmp(m, "repe") || !strcmp(m, "repz")) ? 0xF3 :
                  (!strcmp(m, "repne") || !strcmp(m, "repnz")) ? 0xF2 : 0;
        if (pre && it->npre < 4) { it->pre[it->npre++] = pre; s = trim(p); if (!*s) aerr("a prefix alone"); continue; }
        it->mn = m;
        s = trim(p);
        break;
    }
    if (*s) {
        Vec ops = { 0 };
        split_args(s, &ops);
        if (ops.n > 3) aerr("too many operands");
        for (int i = 0; i < ops.n; i++) parse_operand(ops.v[i], &it->op[i]);
        it->nop = ops.n;
    }
}

/* ---- Encoding */
typedef struct {
    Item *it;
    Buf *out;
    int final;
    int sec;
    long base;              /* the instruction's offset in its section */
    long start;             /* where it starts in out */
} Ctx;

static Ctx C;

static void add_rel(long field, int type, Val v)
{
    if (!C.final) return;
    if (v.sub) aerr("a symbol difference that is not a constant");
    Rel *r = arena(sizeof *r);
    r->off = field;
    r->type = type;
    r->sym = v.add;
    r->addend = v.v;
    vec_push(&secs[C.sec]->rels, r);
}

static void put(long v, int n) { for (int i = 0; i < n; i++) buf_c(C.out, v >> (8 * i)); }

/* a value of n bytes; pc: pc-relative to the instruction's end (tail: bytes after this field) */
static void put_expr(Expr *e, int n, int pc, int tail, int sgn32)
{
    long field = C.base + C.out->len - C.start;
    Val v = C.final ? evalf(e, C.sec, C.base) : (Val){ 0 };
    if (!C.final) { put(0, n); return; }
    if (v.mod == M_TPOFF || v.mod == M_GOTTPOFF) {     /* thread-local: the linker computes it */
        if (v.sub || n < 4 || (v.mod == M_GOTTPOFF) != (pc != 0)) aerr("invalid use of @tpoff/@gottpoff");
        if (pc) v.v -= n + tail;
        add_rel(field, v.mod == M_GOTTPOFF ? R_GOTTPOFF : n == 8 ? R_TPOFF64 : R_TPOFF32, v);
        put(0, n);
        return;
    }
    if (pc) {
        long end = field + n + tail;
        if (v.add && v.add->defined && v.add->sec == C.sec && !v.sub && !v.add->global && !v.mod) {
            long d = v.add->val + v.v - end;
            if ((n == 1 && (d < -128 || d > 127)) || (n == 4 && d != (long)(int)d)) aerr("jump target out of range");
            put(d, n);
            return;
        }
        if (!v.add) aerr("a pc-relative constant");
        v.v -= n + tail;
        add_rel(field, n == 4 ? (pc == 2 || v.mod == M_PLT ? R_PLT32 : R_PC32) : n == 2 ? R_PC16 : R_PC8, v);
        put(0, n);
        return;
    }
    if (!v.add && !v.sub) { put(v.v, n); return; }
    if (target == T_AARCH64) {                  /* data on AArch64: ABS64/32/16 (no 8-bit relocation) */
        if (n == 1) aerr("an address in one byte");
        add_rel(field, n == 8 ? 257 : n == 4 ? 258 : 259, v);
        put(0, n);
        return;
    }
    add_rel(field, n == 8 ? R_64 : n == 4 ? (sgn32 ? R_32S : R_32) : n == 2 ? R_16 : R_8, v);
    put(0, n);
}

/* ModRM-encoded instruction pieces */
typedef struct {
    int osize;              /* operand size: 1, 2, 4, 8 (for prefixes / REX.W) */
    int noW;                /* 8-byte operand without REX.W (push, mov cr, near branches) */
    int mandatory;          /* SSE prefix: 0x66, 0xF2, 0xF3 */
    unsigned char op[3];
    int nop;
    int reg;                /* the ModRM reg field (register number or extension) */
    Opd *rm;                /* ModRM r/m operand */
    Expr *imm;
    int immsz, immsgn;
    int forcerex;
} Ins;

static int fits8(long v) { return v >= -128 && v <= 127; }

static void encode(Ins *x)
{
    int m = C.it->mode;
    Opd *rm = x->rm;
    /* prefixes */
    for (int i = 0; i < C.it->npre; i++) buf_c(C.out, C.it->pre[i]);
    if (rm && rm->kind == 'm' && rm->seg >= 0) { static const int sp[] = { 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65 }; buf_c(C.out, sp[rm->seg]); }
    if (rm && rm->kind == 'm' && rm->asize && ((m == 64 && rm->asize == 4) || (m != 64 && rm->asize == 8))) buf_c(C.out, 0x67);
    if ((x->osize == 2 && m != 16) || (x->osize == 4 && m == 16 && !x->noW)) buf_c(C.out, 0x66);
    if (x->mandatory) buf_c(C.out, x->mandatory);
    /* REX */
    int rex = 0;
    if (x->osize == 8 && !x->noW) rex |= 8;
    if (x->reg & 8) rex |= 4;
    if (rm) {
        if (rm->kind == 'r') { if (rm->reg & 8) rex |= 1; }
        else { if (rm->base >= 0 && (rm->base & 8)) rex |= 1; if (rm->index >= 0 && (rm->index & 8)) rex |= 2; }
    }
    if (rex || x->forcerex) {
        if (m != 64) aerr("a 64-bit register or operand outside 64-bit code");
        buf_c(C.out, 0x40 | rex);
    }
    for (int i = 0; i < x->nop; i++) buf_c(C.out, x->op[i]);
    if (!rm) goto imm;
    int reg = x->reg & 7;
    if (rm->kind == 'r') { buf_c(C.out, 0xC0 | reg << 3 | (rm->reg & 7)); goto imm; }
    /* memory */
    if (m == 16) {                              /* only absolute addresses */
        if (rm->base >= 0 || rm->index >= 0) aerr("16-bit addressing with registers is not supported");
        buf_c(C.out, 0x06 | reg << 3);
        put_expr(rm->e, 2, 0, x->immsz, 0);
        goto imm;
    }
    if (rm->rip) {
        if (m != 64) aerr("rip-relative outside 64-bit code");
        buf_c(C.out, 0x05 | reg << 3);
        put_expr(rm->e ? rm->e : new_expr(E_NUM), 4, 1, x->immsz, 0);
        goto imm;
    }
    if (rm->base < 0 && rm->index < 0) {        /* absolute */
        if (m == 64) { buf_c(C.out, 0x04 | reg << 3); buf_c(C.out, 0x25); }
        else buf_c(C.out, 0x05 | reg << 3);
        put_expr(rm->e ? rm->e : new_expr(E_NUM), 4, 0, x->immsz, m == 64);
        goto imm;
    }
    {
        int dsz;
        long dv = 0;
        int sym = rm->e && has_sym(rm->e);
        if (rm->e && !sym) dv = const_val(rm->e, 0, 0);
        if (sym) dsz = 4;
        else if (dv == 0 && !(rm->base >= 0 && (rm->base & 7) == 5)) dsz = 0;
        else dsz = fits8(dv) ? 1 : 4;
        int mod = dsz == 0 ? 0 : dsz == 1 ? 1 : 2;
        int need_sib = rm->index >= 0 || rm->base < 0 || (rm->base & 7) == 4;
        if (rm->base < 0) mod = 0, dsz = 4;     /* index without base: disp32 */
        if (!need_sib) buf_c(C.out, mod << 6 | reg << 3 | (rm->base & 7));
        else {
            buf_c(C.out, mod << 6 | reg << 3 | 4);
            int ss = rm->scale == 1 ? 0 : rm->scale == 2 ? 1 : rm->scale == 4 ? 2 : rm->scale == 8 ? 3 : -1;
            if (ss < 0) aerr("invalid scale %d", rm->scale);
            if (rm->index == 4) aerr("%%rsp cannot be an index");
            int idx = rm->index >= 0 ? rm->index & 7 : 4;
            int base = rm->base >= 0 ? rm->base & 7 : 5;
            buf_c(C.out, ss << 6 | idx << 3 | base);
        }
        if (dsz == 1) buf_c(C.out, dv);
        else if (dsz == 4) { if (sym) put_expr(rm->e, 4, 0, x->immsz, m == 64); else put(dv, 4); }
    }
imm:
    if (x->immsz) put_expr(x->imm, x->immsz, 0, 0, x->immsgn);
}

static int ccode(const char *s)
{
    static const struct { const char *n; int c; } t[] = {
        { "o", 0 }, { "no", 1 }, { "b", 2 }, { "c", 2 }, { "nae", 2 }, { "ae", 3 }, { "nb", 3 }, { "nc", 3 },
        { "e", 4 }, { "z", 4 }, { "ne", 5 }, { "nz", 5 }, { "be", 6 }, { "na", 6 }, { "a", 7 }, { "nbe", 7 },
        { "s", 8 }, { "ns", 9 }, { "p", 10 }, { "pe", 10 }, { "np", 11 }, { "po", 11 }, { "l", 12 }, { "nge", 12 },
        { "ge", 13 }, { "nl", 13 }, { "le", 14 }, { "ng", 14 }, { "g", 15 }, { "nle", 15 },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(s, t[i].n)) return t[i].c;
    return -1;
}

/* the operand size: from a suffix, else from a register operand */
static int opsize(Item *it, int sfx)
{
    if (sfx) return sfx;
    for (int i = it->nop - 1; i >= 0; i--)
        if (it->op[i].kind == 'r' && it->op[i].rclass == RC_GPR) return it->op[i].size;
    for (int i = 0; i < it->nop; i++) if (it->op[i].kind == 'r' && it->op[i].rclass == RC_SEG) return 2;
    return 0;
}

static int suffix_size(char c) { return c == 'b' ? 1 : c == 'w' ? 2 : c == 'l' ? 4 : c == 'q' ? 8 : 0; }

static int is_imm_const(Opd *o, long *v)
{
    if (o->kind != 'i' || has_sym(o->e)) return 0;
    *v = const_val(o->e, 0, 0);
    return 1;
}

static void need(Item *it, int n) { if (it->nop != n) aerr("%s: expected %d operand%s", it->mn, n, n == 1 ? "" : "s"); }

static Ins I0(void) { Ins x; memset(&x, 0, sizeof x); return x; }

/* an ALU-style operation: op src, dst */
static void alu(Item *it, int n, int sz)
{
    need(it, 2);
    Opd *s = &it->op[0], *d = &it->op[1];
    Ins x = I0();
    x.osize = sz;
    if (!sz) aerr("%s: operand size unknown", it->mn);
    if (s->kind == 'i') {
        long v;
        int c = is_imm_const(s, &v);
        x.rm = d;
        x.reg = n;
        x.imm = s->e;
        if (sz == 1) { x.op[0] = 0x80; x.immsz = 1; }
        else if (c && fits8(v)) { x.op[0] = 0x83; x.immsz = 1; }
        else if (d->kind == 'r' && d->reg == 0) {     /* op $imm, %eax: a byte shorter */
            x.op[0] = n * 8 + 5; x.rm = 0; x.immsz = sz == 2 ? 2 : 4; x.immsgn = sz == 8;
        }
        else { x.op[0] = 0x81; x.immsz = sz == 2 ? 2 : 4; x.immsgn = sz == 8; }
        x.nop = 1;
        if (d->kind == 'r' && d->rexb) x.forcerex = 1;
        encode(&x);
        return;
    }
    if (s->kind == 'r') {                          /* reg -> r/m */
        x.op[0] = n * 8 + (sz == 1 ? 0 : 1);
        x.nop = 1;
        x.reg = s->reg;
        x.rm = d;
        if (s->rexb || (d->kind == 'r' && d->rexb)) x.forcerex = 1;
        encode(&x);
        return;
    }
    if (d->kind != 'r') aerr("%s: two memory operands", it->mn);
    x.op[0] = n * 8 + (sz == 1 ? 2 : 3);           /* r/m -> reg */
    x.nop = 1;
    x.reg = d->reg;
    x.rm = s;
    if (d->rexb) x.forcerex = 1;
    encode(&x);
}

/* group "F6/F7 /n" (not, neg, mul, imul, div, idiv) and "FE/FF" (inc, dec) */
static void unary(Item *it, int base, int n, int sz)
{
    need(it, 1);
    if (!sz) aerr("%s: operand size unknown", it->mn);
    Ins x = I0();
    x.osize = sz;
    x.op[0] = sz == 1 ? base : base + 1;
    x.nop = 1;
    x.reg = n;
    x.rm = &it->op[0];
    if (it->op[0].kind == 'r' && it->op[0].rexb) x.forcerex = 1;
    encode(&x);
}

static void shift_op(Item *it, int n, int sz)
{
    Ins x = I0();
    Opd *d = &it->op[it->nop - 1];
    x.osize = sz;
    x.reg = n;
    x.rm = d;
    x.nop = 1;
    if (!sz) aerr("%s: operand size unknown", it->mn);
    if (d->kind == 'r' && d->rexb) x.forcerex = 1;
    long v;
    if (it->nop == 1) x.op[0] = sz == 1 ? 0xD0 : 0xD1;
    else if (it->op[0].kind == 'r' && it->op[0].reg == 1 && it->op[0].size == 1) x.op[0] = sz == 1 ? 0xD2 : 0xD3;
    else if (is_imm_const(&it->op[0], &v) && v == 1) x.op[0] = sz == 1 ? 0xD0 : 0xD1;
    else { x.op[0] = sz == 1 ? 0xC0 : 0xC1; x.imm = it->op[0].e; x.immsz = 1; }
    encode(&x);
}

/* "0F xx /r" with reg = destination register, rm = source */
static void rm_to_reg(Item *it, int mand, int o1, int o2, int sz, int noW)
{
    need(it, 2);
    Ins x = I0();
    x.mandatory = mand;
    x.osize = sz;
    x.noW = noW;
    x.op[0] = o1;
    x.nop = 1;
    if (o2 >= 0) { x.op[1] = o2; x.nop = 2; }
    x.reg = it->op[1].reg;
    x.rm = &it->op[0];
    if ((it->op[0].kind == 'r' && it->op[0].rexb) || it->op[1].rexb) x.forcerex = 1;
    encode(&x);
}

/* "0F xx /r" with reg = source register, rm = destination */
static void reg_to_rm(Item *it, int mand, int o1, int o2, int sz)
{
    need(it, 2);
    Ins x = I0();
    x.mandatory = mand;
    x.osize = sz;
    x.op[0] = o1;
    x.nop = 1;
    if (o2 >= 0) { x.op[1] = o2; x.nop = 2; }
    x.reg = it->op[0].reg;
    x.rm = &it->op[1];
    if (it->op[0].rexb || (it->op[1].kind == 'r' && it->op[1].rexb)) x.forcerex = 1;
    encode(&x);
}

static void simple(const char *bytes, int n)
{
    for (int i = 0; i < C.it->npre; i++) buf_c(C.out, C.it->pre[i]);
    for (int i = 0; i < n; i++) buf_c(C.out, (unsigned char)bytes[i]);
}

static void branch_target(Item *it, int *local)
{
    Opd *t = &it->op[0];
    *local = 0;
    if (t->kind != 'm' || t->base >= 0 || t->index >= 0 || t->rip || t->star) return;
    Val v = eval(t->e, C.sec, C.base);
    *local = v.add && !v.sub && v.add->sec == C.sec && !v.add->global;
}

/* x87: memory forms "op /n" and register forms "op+i" (GNU's AT&T names) */
static int x87_insn(Item *it)
{
    const char *mn = it->mn;
    static const struct { const char *n; int op, ext; } mem[] = {
        { "fldt", 0xDB, 5 }, { "fstpt", 0xDB, 7 }, { "flds", 0xD9, 0 }, { "fldl", 0xDD, 0 }, { "fsts", 0xD9, 2 },
        { "fstl", 0xDD, 2 }, { "fstps", 0xD9, 3 }, { "fstpl", 0xDD, 3 }, { "filds", 0xDF, 0 }, { "fildl", 0xDB, 0 },
        { "fildq", 0xDF, 5 }, { "fildll", 0xDF, 5 }, { "fistps", 0xDF, 3 }, { "fistpl", 0xDB, 3 }, { "fistpq", 0xDF, 7 },
        { "fistpll", 0xDF, 7 }, { "fisttps", 0xDF, 1 }, { "fisttpl", 0xDB, 1 }, { "fisttpq", 0xDD, 1 },
        { "fisttpll", 0xDD, 1 }, { "fadds", 0xD8, 0 }, { "fmuls", 0xD8, 1 }, { "fsubs", 0xD8, 4 }, { "fsubrs", 0xD8, 5 },
        { "fdivs", 0xD8, 6 }, { "fdivrs", 0xD8, 7 }, { "faddl", 0xDC, 0 }, { "fmull", 0xDC, 1 }, { "fsubl", 0xDC, 4 },
        { "fsubrl", 0xDC, 5 }, { "fdivl", 0xDC, 6 }, { "fdivrl", 0xDC, 7 }, { "fnstcw", 0xD9, 7 }, { "fldcw", 0xD9, 5 },
        { "fnstenv", 0xD9, 6 }, { "fldenv", 0xD9, 4 },
    };
    for (unsigned k = 0; k < sizeof mem / sizeof mem[0]; k++)
        if (!strcmp(mn, mem[k].n)) {
            need(it, 1);
            if (it->op[0].kind != 'm') aerr("%s: a memory operand", mn);
            Ins x = I0();
            x.op[0] = mem[k].op;
            x.nop = 1;
            x.reg = mem[k].ext;
            x.rm = &it->op[0];
            encode(&x);
            return 1;
        }
    static const struct { const char *n; int b0, b1; } reg[] = {   /* the register is added to b1 */
        { "faddp", 0xDE, 0xC0 }, { "fmulp", 0xDE, 0xC8 }, { "fsubp", 0xDE, 0xE0 }, { "fsubrp", 0xDE, 0xE8 },
        { "fdivp", 0xDE, 0xF0 }, { "fdivrp", 0xDE, 0xF8 }, { "fucomip", 0xDF, 0xE8 }, { "fcomip", 0xDF, 0xF0 },
        { "fucomi", 0xDB, 0xE8 }, { "fcomi", 0xDB, 0xF0 }, { "fstp", 0xDD, 0xD8 }, { "fst", 0xDD, 0xD0 },
        { "fld", 0xD9, 0xC0 }, { "fxch", 0xD9, 0xC8 }, { "ffree", 0xDD, 0xC0 },
    };
    for (unsigned k = 0; k < sizeof reg / sizeof reg[0]; k++)
        if (!strcmp(mn, reg[k].n)) {
            int r = 1;                          /* no operand: st(1) */
            for (int o = 0; o < it->nop; o++) {
                if (it->op[o].kind != 'r' || it->op[o].rclass != RC_ST) aerr("%s: x87 registers", mn);
                if (it->op[o].reg) r = it->op[o].reg;
                else if (it->nop == 1) r = 0;
            }
            buf_c(C.out, reg[k].b0);
            buf_c(C.out, reg[k].b1 + r);
            return 1;
        }
    static const struct { const char *n; const char *b; } fix[] = {
        { "fchs", "\xD9\xE0" }, { "fabs", "\xD9\xE1" }, { "fldz", "\xD9\xEE" }, { "fld1", "\xD9\xE8" },
        { "fninit", "\xDB\xE3" }, { "fsqrt", "\xD9\xFA" }, { "frndint", "\xD9\xFC" }, { "fscale", "\xD9\xFD" },
        { "fprem", "\xD9\xF8" }, { "fwait", "\x9B" }, { "fldpi", "\xD9\xEB" }, { "fxam", "\xD9\xE5" },
    };
    for (unsigned k = 0; k < sizeof fix / sizeof fix[0]; k++)
        if (!strcmp(mn, fix[k].n) && it->nop == 0) { simple(fix[k].b, strlen(fix[k].b)); return 1; }
    if (!strcmp(mn, "fnstsw")) { buf_c(C.out, 0xDF); buf_c(C.out, 0xE0); return 1; }
    return 0;
}

static void insn(Item *it)
{
    const char *mn = it->mn;
    int len = strlen(mn);
    char last = len > 1 ? mn[len - 1] : 0;
    /* ---- instructions without operands */
    static const struct { const char *n; const char *b; int len; } fixed[] = {
        { "ret", "\xC3", 1 }, { "retq", "\xC3", 1 }, { "leave", "\xC9", 1 }, { "leaveq", "\xC9", 1 },
        { "nop", "\x90", 1 }, { "hlt", "\xF4", 1 }, { "cli", "\xFA", 1 }, { "sti", "\xFB", 1 },
        { "pause", "\xF3\x90", 2 }, { "cpuid", "\x0F\xA2", 2 }, { "rdtsc", "\x0F\x31", 2 }, { "rdmsr", "\x0F\x32", 2 },
        { "wrmsr", "\x0F\x30", 2 }, { "ud2", "\x0F\x0B", 2 }, { "int3", "\xCC", 1 }, { "iretq", "\x48\xCF", 2 },
        { "iret", "\xCF", 1 }, { "iretl", "\xCF", 1 }, { "sysretq", "\x48\x0F\x07", 3 }, { "sysret", "\x0F\x07", 2 },
        { "syscall", "\x0F\x05", 2 }, { "swapgs", "\x0F\x01\xF8", 3 }, { "cltq", "\x48\x98", 2 }, { "cdqe", "\x48\x98", 2 },
        { "cltd", "\x99", 1 }, { "cdq", "\x99", 1 }, { "cqto", "\x48\x99", 2 }, { "cqo", "\x48\x99", 2 },
        { "cwtl", "\x98", 1 }, { "mfence", "\x0F\xAE\xF0", 3 }, { "lfence", "\x0F\xAE\xE8", 3 }, { "sfence", "\x0F\xAE\xF8", 3 },
        { "clc", "\xF8", 1 }, { "stc", "\xF9", 1 }, { "cld", "\xFC", 1 }, { "std", "\xFD", 1 }, { "wbinvd", "\x0F\x09", 2 },
        { "movsb", "\xA4", 1 }, { "movsl", "\xA5", 1 }, { "movsq", "\x48\xA5", 2 }, { "stosb", "\xAA", 1 }, { "stosl", "\xAB", 1 },
        { "stosq", "\x48\xAB", 2 }, { "lodsb", "\xAC", 1 }, { "lodsl", "\xAD", 1 }, { "scasb", "\xAE", 1 }, { "cmpsb", "\xA6", 1 },
        { "movsw", "\x66\xA5", 2 }, { "stosw", "\x66\xAB", 2 }, { "pushfq", "\x9C", 1 }, { "popfq", "\x9D", 1 },
        { "pushf", "\x9C", 1 }, { "popf", "\x9D", 1 }, { "cbtw", "\x66\x98", 2 }, { "int1", "\xF1", 1 },
    };
    if (it->nop == 0 || (!strcmp(mn, "movsd") && it->nop == 0)) {
        for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
            if (!strcmp(mn, fixed[i].n)) {
                if (it->mode != 64 && fixed[i].b[0] == 0x48) aerr("%s: 64-bit only", mn);
                simple(fixed[i].b, fixed[i].len);
                return;
            }
    }
    /* ---- branches */
    if (!strcmp(mn, "jmp") || !strcmp(mn, "jmpq") || !strcmp(mn, "call") || !strcmp(mn, "callq")) {
        need(it, 1);
        int call = mn[0] == 'c';
        Opd *t = &it->op[0];
        if (t->star || t->kind == 'r') {          /* indirect */
            Ins x = I0();
            x.osize = 8;
            x.noW = 1;
            x.op[0] = 0xFF;
            x.nop = 1;
            x.reg = call ? 2 : 4;
            x.rm = t;
            encode(&x);
            return;
        }
        int local;
        branch_target(it, &local);
        if (call) { buf_c(C.out, 0xE8); put_expr(t->e, 4, local ? 1 : 2, 0, 0); return; }
        it->relax = local;
        if (local && !it->longj) { buf_c(C.out, 0xEB); put_expr(t->e, 1, 1, 0, 0); return; }
        buf_c(C.out, 0xE9);
        put_expr(t->e, 4, local ? 1 : 2, 0, 0);
        return;
    }
    if (mn[0] == 'j' && ccode(mn + 1) >= 0) {
        need(it, 1);
        int cc = ccode(mn + 1), local;
        branch_target(it, &local);
        it->relax = local;
        if (local && !it->longj) { buf_c(C.out, 0x70 + cc); put_expr(it->op[0].e, 1, 1, 0, 0); return; }
        buf_c(C.out, 0x0F);
        buf_c(C.out, 0x80 + cc);
        put_expr(it->op[0].e, 4, 1, 0, 0);
        return;
    }
    if (!strcmp(mn, "loop") || !strcmp(mn, "jecxz") || !strcmp(mn, "jrcxz")) {
        need(it, 1);
        buf_c(C.out, mn[0] == 'l' ? 0xE2 : 0xE3);
        put_expr(it->op[0].e, 1, 1, 0, 0);
        return;
    }
    if (!strcmp(mn, "ljmp") || !strcmp(mn, "ljmpl") || !strcmp(mn, "ljmpw")) {
        need(it, 2);
        int osz = !strcmp(mn, "ljmpw") ? 2 : !strcmp(mn, "ljmpl") ? 4 : it->mode == 16 ? 2 : 4;
        if ((osz == 4 && it->mode == 16) || (osz == 2 && it->mode != 16)) buf_c(C.out, 0x66);
        buf_c(C.out, 0xEA);
        put_expr(it->op[1].e, osz, 0, 0, 0);
        put_expr(it->op[0].e, 2, 0, 0, 0);
        return;
    }
    /* ---- set, cmov */
    if (!strncmp(mn, "set", 3) && ccode(mn + 3) >= 0) {
        need(it, 1);
        Ins x = I0();
        x.op[0] = 0x0F;
        x.op[1] = 0x90 + ccode(mn + 3);
        x.nop = 2;
        x.rm = &it->op[0];
        x.osize = 1;
        if (it->op[0].kind == 'r' && it->op[0].rexb) x.forcerex = 1;
        encode(&x);
        return;
    }
    if (!strncmp(mn, "cmov", 4)) {
        char b[8] = "";
        strncpy(b, mn + 4, 7);
        int sz = 0;
        if (ccode(b) < 0 && strlen(b) > 1) { sz = suffix_size(b[strlen(b) - 1]); b[strlen(b) - 1] = 0; }
        if (ccode(b) >= 0) { rm_to_reg(it, 0, 0x0F, 0x40 + ccode(b), opsize(it, sz), 0); return; }
    }
    /* ---- SSE */
    static const struct { const char *n; int mand, op; } sse[] = {
        { "addss", 0xF3, 0x58 }, { "addsd", 0xF2, 0x58 }, { "subss", 0xF3, 0x5C }, { "subsd", 0xF2, 0x5C },
        { "mulss", 0xF3, 0x59 }, { "mulsd", 0xF2, 0x59 }, { "divss", 0xF3, 0x5E }, { "divsd", 0xF2, 0x5E },
        { "sqrtss", 0xF3, 0x51 }, { "sqrtsd", 0xF2, 0x51 }, { "minsd", 0xF2, 0x5D }, { "maxsd", 0xF2, 0x5F },
        { "minss", 0xF3, 0x5D }, { "maxss", 0xF3, 0x5F }, { "ucomiss", 0, 0x2E }, { "ucomisd", 0x66, 0x2E },
        { "comiss", 0, 0x2F }, { "comisd", 0x66, 0x2F }, { "xorps", 0, 0x57 }, { "xorpd", 0x66, 0x57 },
        { "andps", 0, 0x54 }, { "andpd", 0x66, 0x54 }, { "orps", 0, 0x56 }, { "orpd", 0x66, 0x56 },
        { "pxor", 0x66, 0xEF }, { "cvtss2sd", 0xF3, 0x5A }, { "cvtsd2ss", 0xF2, 0x5A }, { "andnps", 0, 0x55 },
        { "unpcklps", 0, 0x14 }, { "cvtps2pd", 0, 0x5A },
    };
    for (unsigned i = 0; i < sizeof sse / sizeof sse[0]; i++)
        if (!strcmp(mn, sse[i].n)) { rm_to_reg(it, sse[i].mand, 0x0F, sse[i].op, 4, 0); return; }
    /* packed SSE/SSE2: "mand 0F op /r" */
    static const struct { const char *n; int mand, op; } packed[] = {
        { "paddb", 0x66, 0xFC }, { "paddw", 0x66, 0xFD }, { "paddd", 0x66, 0xFE }, { "paddq", 0x66, 0xD4 },
        { "psubb", 0x66, 0xF8 }, { "psubw", 0x66, 0xF9 }, { "psubd", 0x66, 0xFA }, { "psubq", 0x66, 0xFB },
        { "pmullw", 0x66, 0xD5 }, { "pmulhw", 0x66, 0xE5 }, { "pmulhuw", 0x66, 0xE4 }, { "pmuludq", 0x66, 0xF4 },
        { "pmaddwd", 0x66, 0xF5 }, { "pand", 0x66, 0xDB }, { "pandn", 0x66, 0xDF }, { "por", 0x66, 0xEB },
        { "pcmpeqb", 0x66, 0x74 }, { "pcmpeqw", 0x66, 0x75 }, { "pcmpeqd", 0x66, 0x76 }, { "pcmpgtb", 0x66, 0x64 },
        { "pcmpgtw", 0x66, 0x65 }, { "pcmpgtd", 0x66, 0x66 }, { "paddsb", 0x66, 0xEC }, { "paddsw", 0x66, 0xED },
        { "paddusb", 0x66, 0xDC }, { "paddusw", 0x66, 0xDD }, { "psubsb", 0x66, 0xE8 }, { "psubsw", 0x66, 0xE9 },
        { "psubusb", 0x66, 0xD8 }, { "psubusw", 0x66, 0xD9 }, { "pminsw", 0x66, 0xEA }, { "pmaxsw", 0x66, 0xEE },
        { "pminub", 0x66, 0xDA }, { "pmaxub", 0x66, 0xDE }, { "pavgb", 0x66, 0xE0 }, { "pavgw", 0x66, 0xE3 },
        { "psadbw", 0x66, 0xF6 }, { "packsswb", 0x66, 0x63 }, { "packssdw", 0x66, 0x6B }, { "packuswb", 0x66, 0x67 },
        { "punpcklbw", 0x66, 0x60 }, { "punpcklwd", 0x66, 0x61 }, { "punpckldq", 0x66, 0x62 }, { "punpcklqdq", 0x66, 0x6C },
        { "punpckhbw", 0x66, 0x68 }, { "punpckhwd", 0x66, 0x69 }, { "punpckhdq", 0x66, 0x6A }, { "punpckhqdq", 0x66, 0x6D },
        { "addps", 0, 0x58 }, { "addpd", 0x66, 0x58 }, { "subps", 0, 0x5C }, { "subpd", 0x66, 0x5C },
        { "mulps", 0, 0x59 }, { "mulpd", 0x66, 0x59 }, { "divps", 0, 0x5E }, { "divpd", 0x66, 0x5E },
        { "minps", 0, 0x5D }, { "minpd", 0x66, 0x5D }, { "maxps", 0, 0x5F }, { "maxpd", 0x66, 0x5F },
        { "sqrtps", 0, 0x51 }, { "sqrtpd", 0x66, 0x51 }, { "andnpd", 0x66, 0x55 }, { "unpckhps", 0, 0x15 },
        { "unpcklpd", 0x66, 0x14 }, { "unpckhpd", 0x66, 0x15 }, { "cvtdq2ps", 0, 0x5B }, { "cvttps2dq", 0xF3, 0x5B },
        { "cvtps2dq", 0x66, 0x5B }, { "movmskps", 0, 0x50 }, { "movmskpd", 0x66, 0x50 }, { "pmovmskb", 0x66, 0xD7 },
    };
    for (unsigned i = 0; i < sizeof packed / sizeof packed[0]; i++)
        if (!strcmp(mn, packed[i].n)) { rm_to_reg(it, packed[i].mand, 0x0F, packed[i].op, 4, 0); return; }
    /* SSSE3/SSE4.1: "66 0F 38 op /r" */
    static const struct { const char *n; int op; } p38[] = {
        { "pshufb", 0x00 }, { "pmaddubsw", 0x04 }, { "pmulld", 0x40 }, { "pcmpeqq", 0x29 }, { "pminsd", 0x39 },
        { "pmaxsd", 0x3D }, { "pminud", 0x3B }, { "pmaxud", 0x3F }, { "pabsb", 0x1C }, { "pabsw", 0x1D }, { "pabsd", 0x1E },
        { "pminsb", 0x38 }, { "pmaxsb", 0x3C }, { "pminuw", 0x3A }, { "pmaxuw", 0x3E }, { "packusdw", 0x2B }, { "pcmpgtq", 0x37 },
    };
    for (unsigned i = 0; i < sizeof p38 / sizeof p38[0]; i++)
        if (!strcmp(mn, p38[i].n)) {
            need(it, 2);
            Ins x = I0();
            x.mandatory = 0x66; x.osize = 4;
            x.op[0] = 0x0F; x.op[1] = 0x38; x.op[2] = p38[i].op; x.nop = 3;
            x.reg = it->op[1].reg; x.rm = &it->op[0];
            if ((it->op[0].kind == 'r' && it->op[0].rexb) || it->op[1].rexb) x.forcerex = 1;
            encode(&x);
            return;
        }
    /* shifts by an immediate: "66 0F 71/72/73 /n ib" */
    static const struct { const char *n; int op, ext; } pshi[] = {
        { "psllw", 0x71, 6 }, { "pslld", 0x72, 6 }, { "psllq", 0x73, 6 }, { "psrlw", 0x71, 2 }, { "psrld", 0x72, 2 },
        { "psrlq", 0x73, 2 }, { "psraw", 0x71, 4 }, { "psrad", 0x72, 4 }, { "pslldq", 0x73, 7 }, { "psrldq", 0x73, 3 },
    };
    for (unsigned i = 0; i < sizeof pshi / sizeof pshi[0]; i++)
        if (!strcmp(mn, pshi[i].n) && it->nop == 2 && it->op[0].kind == 'i') {
            Ins x = I0();
            x.mandatory = 0x66; x.osize = 4;
            x.op[0] = 0x0F; x.op[1] = pshi[i].op; x.nop = 2;
            x.reg = pshi[i].ext; x.rm = &it->op[1];
            x.imm = it->op[0].e; x.immsz = 1;
            encode(&x);
            return;
        }
    /* with an immediate and two registers: pshufd, pshuflw/hw, cmpps/pd, shufps/pd */
    static const struct { const char *n; int mand, op; } p3[] = {
        { "pshufd", 0x66, 0x70 }, { "pshuflw", 0xF2, 0x70 }, { "pshufhw", 0xF3, 0x70 }, { "cmpps", 0, 0xC2 }, { "cmppd", 0x66, 0xC2 },
        { "shufps", 0, 0xC6 }, { "shufpd", 0x66, 0xC6 },
    };
    for (unsigned i = 0; i < sizeof p3 / sizeof p3[0]; i++)
        if (!strcmp(mn, p3[i].n) && it->nop == 3) {
            Ins x = I0();
            x.mandatory = p3[i].mand; x.osize = 4;
            x.op[0] = 0x0F; x.op[1] = p3[i].op; x.nop = 2;
            x.reg = it->op[2].reg; x.rm = &it->op[1];
            if ((it->op[1].kind == 'r' && it->op[1].rexb) || it->op[2].rexb) x.forcerex = 1;
            x.imm = it->op[0].e; x.immsz = 1;
            encode(&x);
            return;
        }
    if ((!strcmp(mn, "movss") || !strcmp(mn, "movsd")) && it->nop == 2) {
        int mand = mn[4] == 's' ? 0xF3 : 0xF2;
        if (it->op[1].kind == 'r' && it->op[1].rclass == RC_XMM) rm_to_reg(it, mand, 0x0F, 0x10, 4, 0);
        else reg_to_rm(it, mand, 0x0F, 0x11, 4);
        return;
    }
    if (!strcmp(mn, "movaps") || !strcmp(mn, "movups") || !strcmp(mn, "movapd") || !strcmp(mn, "movupd")) {
        int mand = mn[5] == 'd' ? 0x66 : 0;
        int o = mn[3] == 'a' ? 0x28 : 0x10;
        if (it->op[1].kind == 'r' && it->op[1].rclass == RC_XMM) rm_to_reg(it, mand, 0x0F, o, 4, 0);
        else reg_to_rm(it, mand, 0x0F, o + 1, 4);
        return;
    }
    if ((!strcmp(mn, "movd") || !strcmp(mn, "movq")) && it->nop == 2 &&
        ((it->op[0].kind == 'r' && it->op[0].rclass == RC_XMM) || (it->op[1].kind == 'r' && it->op[1].rclass == RC_XMM))) {
        Opd *s = &it->op[0], *d = &it->op[1];
        int q = mn[3] == 'q';
        int sx = s->kind == 'r' && s->rclass == RC_XMM, dx = d->kind == 'r' && d->rclass == RC_XMM;
        if (q && sx && dx) { rm_to_reg(it, 0xF3, 0x0F, 0x7E, 4, 0); return; }
        if (q && dx && s->kind == 'm') { rm_to_reg(it, 0xF3, 0x0F, 0x7E, 4, 0); return; }
        if (q && sx && d->kind == 'm') { reg_to_rm(it, 0x66, 0x0F, 0xD6, 4); return; }
        if (dx) { rm_to_reg(it, 0x66, 0x0F, 0x6E, q ? 8 : 4, 0); return; }     /* gpr -> xmm */
        reg_to_rm(it, 0x66, 0x0F, 0x7E, q ? 8 : 4);                            /* xmm -> gpr */
        return;
    }
    if (!strncmp(mn, "cvtsi2s", 7)) {                /* cvtsi2sd[lq] int, xmm */
        int mand = mn[7] == 'd' ? 0xF2 : 0xF3;
        int sz = suffix_size(mn[8]);
        if (!sz) sz = it->op[0].kind == 'r' ? it->op[0].size : 4;
        rm_to_reg(it, mand, 0x0F, 0x2A, sz, 0);
        return;
    }
    if (!strncmp(mn, "cvtts", 5) || !strncmp(mn, "cvts", 4)) {   /* cvttsd2si[lq] xmm, int */
        const char *p = mn + (mn[3] == 't' ? 5 : 4);
        int mand = p[0] == 'd' ? 0xF2 : 0xF3;
        int sz = suffix_size(mn[len - 1]);
        if (mn[len - 1] == 'i') sz = it->op[1].size;
        rm_to_reg(it, mand, 0x0F, mn[3] == 't' ? 0x2C : 0x2D, sz, 0);
        return;
    }
    /* ---- x87 floating point */
    if (mn[0] == 'f' && x87_insn(it)) return;
    /* ---- the rest: mnemonic + optional size suffix */
    char base[16];
    int sfx = 0;
    static const char *names[] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp", "test", "mov", "lea", "inc", "dec",
        "neg", "not", "mul", "imul", "div", "idiv", "shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr", "push", "pop",
        "xchg", "xadd", "cmpxchg", "shrd", "shld", "bsf", "bsr", "popcnt", "bswap", "bt", "bts", "btr", "btc", "in", "out", "lgdt", "lidt",
        "sgdt", "sidt", "ltr", "invlpg", "rdrand", "rdseed", "movabs", "lock", 0 };
    strncpy(base, mn, 15);
    base[15] = 0;
    int found = 0;
    for (int i = 0; names[i]; i++) if (!strcmp(base, names[i])) found = 1;
    if (!found && len > 1 && suffix_size(last)) {
        base[len - 1] = 0;
        for (int i = 0; names[i]; i++) if (!strcmp(base, names[i])) { found = 1; sfx = suffix_size(last); }
        if (!found) base[len - 1] = last;
    }
    /* movzbl, movsbq, movslq... */
    if (!found && (!strncmp(mn, "movz", 4) || !strncmp(mn, "movs", 4)) && len == 6) {
        int from = suffix_size(mn[4]), to = suffix_size(mn[5]);
        if (from && to) {
            if (mn[3] == 's' && from == 4) { rm_to_reg(it, 0, 0x63, -1, 8, 0); return; }   /* movslq */
            if (from == 4) aerr("%s: invalid", mn);
            rm_to_reg(it, 0, 0x0F, (mn[3] == 'z' ? 0xB6 : 0xBE) + (from == 2), to, 0);
            return;
        }
    }
    if (!found) aerr("unknown instruction '%s'", mn);
    int sz = opsize(it, sfx);
    static const char *aluops[] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };
    for (int i = 0; i < 8; i++) if (!strcmp(base, aluops[i])) { alu(it, i, sz); return; }
    if (!strcmp(base, "test")) {
        need(it, 2);
        Opd *s = &it->op[0], *d = &it->op[1];
        if (!sz) aerr("test: operand size unknown");
        Ins x = I0();
        x.osize = sz;
        if (s->kind == 'i') {
            x.op[0] = sz == 1 ? 0xF6 : 0xF7;
            x.nop = 1;
            x.reg = 0;
            x.rm = d;
            x.imm = s->e;
            x.immsz = sz == 1 ? 1 : sz == 2 ? 2 : 4;
            x.immsgn = sz == 8;
        } else {
            x.op[0] = sz == 1 ? 0x84 : 0x85;
            x.nop = 1;
            x.reg = s->reg;
            x.rm = d;
            if (s->rexb || (d->kind == 'r' && d->rexb)) x.forcerex = 1;
        }
        encode(&x);
        return;
    }
    if (!strcmp(base, "mov") || !strcmp(base, "movabs")) {
        need(it, 2);
        Opd *s = &it->op[0], *d = &it->op[1];
        if (s->kind == 'r' && s->rclass == RC_CR) { Ins x = I0(); x.osize = 8; x.noW = 1; x.op[0] = 0x0F; x.op[1] = 0x20; x.nop = 2; x.reg = s->reg; x.rm = d; encode(&x); return; }
        if (d->kind == 'r' && d->rclass == RC_CR) { Ins x = I0(); x.osize = 8; x.noW = 1; x.op[0] = 0x0F; x.op[1] = 0x22; x.nop = 2; x.reg = d->reg; x.rm = s; encode(&x); return; }
        if (s->kind == 'r' && s->rclass == RC_DR) { Ins x = I0(); x.osize = 8; x.noW = 1; x.op[0] = 0x0F; x.op[1] = 0x21; x.nop = 2; x.reg = s->reg; x.rm = d; encode(&x); return; }
        if (d->kind == 'r' && d->rclass == RC_DR) { Ins x = I0(); x.osize = 8; x.noW = 1; x.op[0] = 0x0F; x.op[1] = 0x23; x.nop = 2; x.reg = d->reg; x.rm = s; encode(&x); return; }
        if (d->kind == 'r' && d->rclass == RC_SEG) { Ins x = I0(); x.osize = 4; x.noW = 1; x.op[0] = 0x8E; x.nop = 1; x.reg = d->reg; x.rm = s; encode(&x); return; }
        if (s->kind == 'r' && s->rclass == RC_SEG) { Ins x = I0(); x.osize = d->kind == 'r' ? d->size : 4; if (x.osize == 2 && d->kind == 'r') x.osize = 4; x.noW = 1; x.op[0] = 0x8C; x.nop = 1; x.reg = s->reg; x.rm = d; encode(&x); return; }
        if (!sz) aerr("mov: operand size unknown");
        Ins x = I0();
        x.osize = sz;
        if (s->kind == 'i') {
            long v;
            int c = is_imm_const(s, &v);
            if (d->kind == 'r') {
                if (sz == 8 && (!strcmp(base, "movabs") || (c && v != (long)(int)v))) {   /* REX.W B8+r imm64 */
                    for (int i = 0; i < it->npre; i++) buf_c(C.out, it->pre[i]);
                    buf_c(C.out, 0x48 | (d->reg & 8 ? 1 : 0));
                    buf_c(C.out, 0xB8 + (d->reg & 7));
                    put_expr(s->e, 8, 0, 0, 0);
                    return;
                }
                if (sz == 8) { x.op[0] = 0xC7; x.nop = 1; x.reg = 0; x.rm = d; x.imm = s->e; x.immsz = 4; x.immsgn = 1; encode(&x); return; }
                /* B0+r / B8+r */
                for (int i = 0; i < it->npre; i++) buf_c(C.out, it->pre[i]);
                if ((sz == 2 && it->mode != 16) || (sz == 4 && it->mode == 16)) buf_c(C.out, 0x66);
                if ((d->reg & 8) || d->rexb) { if (it->mode != 64) aerr("64-bit register"); buf_c(C.out, 0x40 | (d->reg & 8 ? 1 : 0)); }
                buf_c(C.out, (sz == 1 ? 0xB0 : 0xB8) + (d->reg & 7));
                put_expr(s->e, sz, 0, 0, 0);
                return;
            }
            x.op[0] = sz == 1 ? 0xC6 : 0xC7;
            x.nop = 1;
            x.reg = 0;
            x.rm = d;
            x.imm = s->e;
            x.immsz = sz == 1 ? 1 : sz == 2 ? 2 : 4;
            x.immsgn = sz == 8;
            encode(&x);
            return;
        }
        if (s->kind == 'r') {
            x.op[0] = sz == 1 ? 0x88 : 0x89;
            x.nop = 1;
            x.reg = s->reg;
            x.rm = d;
            if (s->rexb || (d->kind == 'r' && d->rexb)) x.forcerex = 1;
            encode(&x);
            return;
        }
        if (d->kind != 'r') aerr("mov: two memory operands");
        x.op[0] = sz == 1 ? 0x8A : 0x8B;
        x.nop = 1;
        x.reg = d->reg;
        x.rm = s;
        if (d->rexb) x.forcerex = 1;
        encode(&x);
        return;
    }
    if (!strcmp(base, "lea")) { rm_to_reg(it, 0, 0x8D, -1, sz ? sz : it->op[1].size, 0); return; }
    if (!strcmp(base, "inc")) { unary(it, 0xFE, 0, sz); return; }
    if (!strcmp(base, "dec")) { unary(it, 0xFE, 1, sz); return; }
    if (!strcmp(base, "not")) { unary(it, 0xF6, 2, sz); return; }
    if (!strcmp(base, "neg")) { unary(it, 0xF6, 3, sz); return; }
    if (!strcmp(base, "mul")) { unary(it, 0xF6, 4, sz); return; }
    if (!strcmp(base, "div")) { unary(it, 0xF6, 6, sz); return; }
    if (!strcmp(base, "idiv")) { unary(it, 0xF6, 7, sz); return; }
    if (!strcmp(base, "imul")) {
        if (it->nop == 1) { unary(it, 0xF6, 5, sz); return; }
        if (it->nop == 2) { rm_to_reg(it, 0, 0x0F, 0xAF, sz, 0); return; }
        Ins x = I0();                              /* imul $imm, src, dst */
        long v;
        int c = is_imm_const(&it->op[0], &v);
        x.osize = sz;
        x.op[0] = c && fits8(v) ? 0x6B : 0x69;
        x.nop = 1;
        x.reg = it->op[2].reg;
        x.rm = &it->op[1];
        x.imm = it->op[0].e;
        x.immsz = x.op[0] == 0x6B ? 1 : sz == 2 ? 2 : 4;
        x.immsgn = sz == 8;
        encode(&x);
        return;
    }
    static const char *shifts[] = { "rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar" };
    static const int shn[] = { 0, 1, 2, 3, 4, 5, 4, 7 };
    for (int i = 0; i < 8; i++) if (!strcmp(base, shifts[i])) { shift_op(it, shn[i], sz); return; }
    if (!strcmp(base, "push") || !strcmp(base, "pop")) {
        need(it, 1);
        Opd *o = &it->op[0];
        int push = base[1] == 'u';
        if (o->kind == 'r' && o->rclass == RC_GPR) {
            for (int i = 0; i < it->npre; i++) buf_c(C.out, it->pre[i]);
            if (o->size == 2) buf_c(C.out, 0x66);
            if (o->reg & 8) buf_c(C.out, 0x41);
            buf_c(C.out, (push ? 0x50 : 0x58) + (o->reg & 7));
            return;
        }
        if (o->kind == 'i') {
            long v;
            if (!push) aerr("pop of an immediate");
            if (is_imm_const(o, &v) && fits8(v)) { buf_c(C.out, 0x6A); put(v, 1); }
            else { buf_c(C.out, 0x68); put_expr(o->e, 4, 0, 0, 1); }
            return;
        }
        if (o->kind == 'r') aerr("%s of a segment register is not supported", base);
        Ins x = I0();
        x.osize = 8;
        x.noW = 1;
        x.op[0] = push ? 0xFF : 0x8F;
        x.nop = 1;
        x.reg = push ? 6 : 0;
        x.rm = o;
        encode(&x);
        return;
    }
    if (!strcmp(base, "xchg")) {
        need(it, 2);
        Opd *s = &it->op[0], *d = &it->op[1];
        if (s->kind != 'r') { Opd t = *s; *s = *d; *d = t; }
        Ins x = I0();
        x.osize = sz;
        x.op[0] = sz == 1 ? 0x86 : 0x87;
        x.nop = 1;
        x.reg = it->op[0].reg;
        x.rm = &it->op[1];
        if (it->op[0].rexb || (it->op[1].kind == 'r' && it->op[1].rexb)) x.forcerex = 1;
        encode(&x);
        return;
    }
    if (!strcmp(base, "shrd") || !strcmp(base, "shld")) {        /* $imm (or %cl), src, dst */
        need(it, 3);
        Ins x = I0();
        x.osize = sz ? sz : it->op[2].size;
        x.op[0] = 0x0F;
        x.op[1] = base[2] == 'r' ? 0xAC : 0xA4;
        x.nop = 2;
        x.reg = it->op[1].reg;
        x.rm = &it->op[2];
        if (it->op[0].kind == 'r') x.op[1]++;                     /* by %cl */
        else { x.imm = it->op[0].e; x.immsz = 1; }
        encode(&x);
        return;
    }
    if (!strcmp(base, "xadd")) { reg_to_rm(it, 0, 0x0F, sz == 1 ? 0xC0 : 0xC1, sz); return; }
    if (!strcmp(base, "cmpxchg")) { reg_to_rm(it, 0, 0x0F, sz == 1 ? 0xB0 : 0xB1, sz); return; }
    if (!strcmp(base, "bsf")) { rm_to_reg(it, 0, 0x0F, 0xBC, sz, 0); return; }
    if (!strcmp(base, "bsr")) { rm_to_reg(it, 0, 0x0F, 0xBD, sz, 0); return; }
    if (!strcmp(base, "popcnt")) { rm_to_reg(it, 0xF3, 0x0F, 0xB8, sz, 0); return; }
    if (!strcmp(base, "bswap")) {
        need(it, 1);
        Opd *o = &it->op[0];
        int rex = (o->size == 8 ? 8 : 0) | (o->reg & 8 ? 1 : 0);
        if (rex) buf_c(C.out, 0x40 | rex);
        buf_c(C.out, 0x0F);
        buf_c(C.out, 0xC8 + (o->reg & 7));
        return;
    }
    if (!strcmp(base, "bt") || !strcmp(base, "bts") || !strcmp(base, "btr") || !strcmp(base, "btc")) {
        int n = base[2] == 0 ? 4 : base[2] == 's' ? 5 : base[2] == 'r' ? 6 : 7;
        need(it, 2);
        if (it->op[0].kind == 'i') {
            Ins x = I0();
            x.osize = sz;
            x.op[0] = 0x0F;
            x.op[1] = 0xBA;
            x.nop = 2;
            x.reg = n;
            x.rm = &it->op[1];
            x.imm = it->op[0].e;
            x.immsz = 1;
            encode(&x);
            return;
        }
        reg_to_rm(it, 0, 0x0F, 0xA3 + (n - 4) * 8, sz);
        return;
    }
    if (!strcmp(base, "in") || !strcmp(base, "out")) {
        need(it, 2);
        int in = base[0] == 'i';
        Opd *port = in ? &it->op[0] : &it->op[1], *data = in ? &it->op[1] : &it->op[0];
        int s = sz ? sz : data->size;
        for (int i = 0; i < it->npre; i++) buf_c(C.out, it->pre[i]);
        if ((s == 2 && it->mode != 16) || (s == 4 && it->mode == 16)) buf_c(C.out, 0x66);
        if (port->kind == 'r') buf_c(C.out, (in ? 0xEC : 0xEE) + (s != 1));
        else { buf_c(C.out, (in ? 0xE4 : 0xE6) + (s != 1)); put_expr(port->e, 1, 0, 0, 0); }
        return;
    }
    if (!strcmp(base, "lgdt") || !strcmp(base, "lidt") || !strcmp(base, "sgdt") || !strcmp(base, "sidt") || !strcmp(base, "invlpg")) {
        need(it, 1);
        Ins x = I0();
        x.osize = sfx == 4 && it->mode == 16 ? 4 : 0;
        x.op[0] = 0x0F;
        x.op[1] = 0x01;
        x.nop = 2;
        x.reg = !strcmp(base, "lgdt") ? 2 : !strcmp(base, "lidt") ? 3 : !strcmp(base, "sgdt") ? 0 : !strcmp(base, "sidt") ? 1 : 7;
        x.rm = &it->op[0];
        encode(&x);
        return;
    }
    if (!strcmp(base, "ltr")) {
        need(it, 1);
        Ins x = I0();
        x.op[0] = 0x0F;
        x.op[1] = 0x00;
        x.nop = 2;
        x.reg = 3;
        x.rm = &it->op[0];
        encode(&x);
        return;
    }
    if (!strcmp(base, "rdrand") || !strcmp(base, "rdseed")) {
        need(it, 1);
        Ins x = I0();
        x.osize = it->op[0].size;
        x.op[0] = 0x0F;
        x.op[1] = 0xC7;
        x.nop = 2;
        x.reg = base[2] == 'r' ? 6 : 7;
        x.rm = &it->op[0];
        encode(&x);
        return;
    }
    aerr("unsupported instruction '%s'", mn);
}

/* ---- Layout and output */
#include "asm_a64.inc"

static long item_size(Item *it, long off)
{
    switch (it->kind) {
    case IT_INSN: {
        if (target == T_AARCH64) return 4;      /* every AArch64 instruction */
        Buf b = { 0 };
        C.it = it;
        C.out = &b;
        C.final = 0;
        C.sec = it->sec;
        C.base = off;
        C.start = 0;
        lineno = it->line;
        insn(it);
        long n = b.len;
        free(b.p);
        return n;
    }
    case IT_DATA: return (long)it->dsize * it->nlist;
    case IT_ASCII: return it->slen;
    case IT_ALIGN: {
        long pad = (it->dsize - off % it->dsize) % it->dsize;
        return it->slen && pad > it->slen ? 0 : pad;
    }
    case IT_SKIP: lineno = it->line; return const_val(it->count, it->sec, off);
    case IT_FILL: lineno = it->line; return const_val(it->count, it->sec, off) * (it->fsize ? const_val(it->fsize, it->sec, off) : 1);
    }
    return 0;
}

static void layout(void)
{
    long off[64];
    for (int pass = 0; pass < 100; pass++) {
        memset(off, 0, sizeof off);
        for (int i = 0; i < items.n; i++) {
            Item *it = items.v[i];
            it->off = off[it->sec];
            if (it->kind == IT_LABEL) { it->sym->sec = it->sec; it->sym->val = it->off; continue; }
            if (it->kind == IT_SET && !it->dsize && it->sym->sec != -1) {    /* x = label + n */
                Val v = evalf(it->count, it->sec, it->off);
                if (v.add && !v.sub && v.add->defined) { it->sym->sec = v.add->sec; it->sym->val = v.add->val + v.v; it->sym->defined = 1; }
                else if (!v.add && !v.sub) { it->sym->sec = -1; it->sym->val = v.v; it->sym->defined = 1; }
                continue;
            }
            it->size = item_size(it, it->off);
            off[it->sec] += it->size;
        }
        /* jumps that do not reach: make them long, and do it again */
        int changed = 0;
        for (int i = 0; i < items.n; i++) {
            Item *it = items.v[i];
            if (it->kind != IT_INSN || !it->relax || it->longj) continue;
            Val v = evalf(it->op[0].e, it->sec, it->off);
            if (!v.add || !v.add->defined) { it->longj = 1; changed = 1; continue; }
            long d = v.add->val + v.v - (it->off + it->size);
            if (d < -128 || d > 127) { it->longj = 1; changed = 1; }
        }
        if (!changed) {
            for (int s = 1; s <= nsecs; s++) secs[s]->size = off[s];
            return;
        }
    }
    aerr("the layout does not settle");
}

/* ---- .debug_line (DWARF 4) from the .loc rows: per section with rows, one
 * sequence; rows use the special opcodes when they fit */

static void uleb(Buf *b, unsigned long v) { do { int c = v & 0x7F; v >>= 7; buf_c(b, c | (v ? 0x80 : 0)); } while (v); }
static void sleb(Buf *b, long v)
{
    for (;;) {
        int c = v & 0x7F;
        v >>= 7;
        if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) { buf_c(b, c); return; }
        buf_c(b, c | 0x80);
    }
}

static void line_table(void)
{
    int any = 0;                                /* rows, or a .debug_line to fill (from -g) */
    for (int i = 0; i < items.n && !any; i++) any = ((Item *)items.v[i])->kind == IT_LOC;
    for (int s = 1; s <= nsecs && !any; s++) any = !strcmp(secs[s]->name, ".debug_line");
    if (!any) return;
    Sect *d = secs[section(".debug_line", 0, 0)];
    Buf *b = &d->data;
    long start = b->len;
    static const unsigned char head[] = { 1, 1, 1, (unsigned char)-5, 14, 13, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1 };
    buf_zero(b, 4);                             /* unit length, later */
    buf_c(b, 4); buf_c(b, 0);                   /* version */
    buf_zero(b, 4);                             /* header length, later */
    long hstart = b->len;
    buf_add(b, head, sizeof head);              /* min insn length .. standard opcode lengths */
    buf_c(b, 0);                                /* no include directories */
    for (int k = 0; k < dbg_names.n; k++) { buf_s(b, dbg_names.v[k]); buf_c(b, 0); buf_c(b, 0); buf_c(b, 0); buf_c(b, 0); }
    buf_c(b, 0);
    long hlen = b->len - hstart;
    for (int k = 0; k < 4; k++) b->p[start + 6 + k] = hlen >> (8 * k);
    for (int s = 1; s <= nsecs; s++) {
        long addr = -1, line = 1, file = 1;
        for (int i = 0; i < items.n; i++) {
            Item *it = items.v[i];
            if (it->kind != IT_LOC || it->sec != s) continue;
            if (addr < 0) {                     /* DW_LNE_set_address: the section + the offset */
                buf_c(b, 0); buf_c(b, 9); buf_c(b, 2);
                Sym *base = sym(fmt(".Ldl%d", s));
                base->defined = 1; base->sec = s; base->val = 0;
                Rel *r = arena(sizeof *r);
                r->off = b->len; r->type = target == T_AARCH64 ? 257 : R_64; r->sym = base; r->addend = it->off;
                vec_push(&d->rels, r);
                buf_zero(b, 8);
                addr = it->off;
            }
            if (it->dsize != file) { buf_c(b, 4); uleb(b, it->dsize); file = it->dsize; }
            long dl = it->slen - line, da = it->off - addr, op = dl + 5 + 14 * da + 13;
            if (dl >= -5 && dl <= 8 && op <= 255) buf_c(b, op);
            else {
                if (da) { buf_c(b, 2); uleb(b, da); }
                if (dl) { buf_c(b, 3); sleb(b, dl); }
                buf_c(b, 1);
            }
            line = it->slen;
            addr = it->off;
        }
        if (addr < 0) continue;
        if (secs[s]->size > addr) { buf_c(b, 2); uleb(b, secs[s]->size - addr); }
        buf_c(b, 0); buf_c(b, 1); buf_c(b, 1);  /* DW_LNE_end_sequence */
    }
    long ulen = b->len - start - 4;
    for (int k = 0; k < 4; k++) b->p[start + k] = ulen >> (8 * k);
    d->size = b->len;
}

static void generate(void)
{
    for (int i = 0; i < items.n; i++) {
        Item *it = items.v[i];
        Sect *s = secs[it->sec];
        lineno = it->line;
        C.it = it;
        C.sec = it->sec;
        C.base = it->off;
        C.final = 1;
        if (s->type == SHT_NOBITS) {
            if (it->kind == IT_INSN || it->kind == IT_DATA || it->kind == IT_ASCII) aerr("data in a section without contents (%s)", s->name);
            if (it->kind == IT_SET && it->dsize) it->sym->size = evalf(it->count, it->sec, it->off).v;   /* .size */
            continue;
        }
        Buf *out = &s->data;
        if (out->len != it->off) die("internal: assembler layout");
        C.out = out;
        C.start = out->len;
        switch (it->kind) {
        case IT_INSN: {
            long start = out->len;
            if (target == T_AARCH64) a64_insn(it); else
            insn(it);
            if (out->len - start != it->size) die("internal: %s changed size (%ld, then %ld)", it->mn, it->size, out->len - start);
            break;
        }
        case IT_DATA:
            for (int k = 0; k < it->nlist; k++) put_expr(it->list[k], it->dsize, 0, 0, 0);
            break;
        case IT_ASCII: buf_add(out, it->str, it->slen); break;
        case IT_ALIGN: {
            long n = it->size;
            int fill = it->fill ? const_val(it->fill, 0, 0) : (s->flags & SHF_EXEC) ? 0x90 : 0;
            if ((s->flags & SHF_EXEC) && !it->fill && target == T_AARCH64) {   /* AArch64: nop words */
                while (n % 4) { buf_c(out, 0); n--; }
                for (; n > 0; n -= 4) put(0xd503201f, 4);
            } else if ((s->flags & SHF_EXEC) && !it->fill) {        /* padding in code: the fewest, longest nops */
                static const char *nops[] = { "", "\x90", "\x66\x90", "\x0f\x1f\x00", "\x0f\x1f\x40\x00",
                    "\x0f\x1f\x44\x00\x00", "\x66\x0f\x1f\x44\x00\x00", "\x0f\x1f\x80\x00\x00\x00\x00",
                    "\x0f\x1f\x84\x00\x00\x00\x00\x00", "\x66\x0f\x1f\x84\x00\x00\x00\x00\x00" };
                while (n > 0) {
                    int k = n > 9 ? 9 : n;
                    buf_add(out, nops[k], k);
                    n -= k;
                }
            } else while (n--) buf_c(out, fill);
            break;
        }
        case IT_SKIP: {
            long n = it->size;
            int fill = it->fill ? const_val(it->fill, 0, 0) : 0;
            while (n--) buf_c(out, fill);
            break;
        }
        case IT_FILL: {
            long cnt = const_val(it->count, it->sec, it->off), sz = it->fsize ? const_val(it->fsize, 0, 0) : 1;
            long v = it->fill ? const_val(it->fill, 0, 0) : 0;
            for (long k = 0; k < cnt; k++) put(v, sz);
            break;
        }
        case IT_SET:
            if (it->dsize) { Val v = evalf(it->count, it->sec, it->off); it->sym->size = v.v; }   /* .size */
            break;
        }
    }
}

Object *assemble(const char *name, const char *text, long len)
{
    src_name = name;
    syms = (Map){ 0 };
    symlist = (Vec){ 0 };
    macros = (Map){ 0 };
    stmts = (Vec){ 0 };
    stmt_lines = (Vec){ 0 };
    items = (Vec){ 0 };
    dbg_names = (Vec){ 0 };
    nsecs = 0;
    memset(numlab, 0, sizeof numlab);
    mode = 64;
    cursec = prevsec = section(".text", 0, 0);
    Vec raw = { 0 }, rawl = { 0 };
    split(text, len, &raw, &rawl, 1);
    expand(&raw, &rawl, 0, raw.n);
    for (int i = 0; i < stmts.n; i++) {
        lineno = (int)(long)stmt_lines.v[i];
        statement(stmts.v[i]);
    }
    layout();
    generate();
    line_table();
    Object *o = arena(sizeof *o);
    o->secs = arena(sizeof(Sect *) * (nsecs + 1));
    for (int i = 1; i <= nsecs; i++) o->secs[i] = secs[i];
    o->nsecs = nsecs;
    o->syms = symlist;
    return o;
}

/* ---- The ELF64 relocatable file */
typedef struct { unsigned name, type; unsigned long flags, addr, off, size; unsigned link, info; unsigned long align, entsize; } Shdr;
typedef struct { unsigned name; unsigned char info, other; unsigned short shndx; unsigned long value, size; } ESym;

static void put_u(Buf *b, unsigned long v, int n) { for (int i = 0; i < n; i++) buf_c(b, i < 8 ? v >> (8 * i) : 0); }

void object_write(Object *o, Buf *out)
{
    /* sections: 0, the program's, then .rela.*, .symtab, .strtab, .shstrtab */
    Buf shstr = { 0 }, str = { 0 };
    buf_c(&shstr, 0);
    buf_c(&str, 0);
    int n = 1;
    for (int i = 1; i <= o->nsecs; i++) o->secs[i]->idx = n++;
    int nrela = 0;
    for (int i = 1; i <= o->nsecs; i++) if (o->secs[i]->rels.n) nrela++;
    int symtab_idx = n + nrela, strtab_idx = symtab_idx + 1, shstr_idx = symtab_idx + 2, nsh = shstr_idx + 1;
    /* symbols: null, sections, locals, then globals */
    Buf symb = { 0 };
    put_u(&symb, 0, 24);
    int nsym = 1, first_global = 1;
    for (int i = 1; i <= o->nsecs; i++) {
        ESym e = { 0, 3, 0, (unsigned short)o->secs[i]->idx, 0, 0 };   /* STT_SECTION, local */
        buf_add(&symb, &e, sizeof e);
        o->secs[i]->symidx = nsym++;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < o->syms.n; i++) {
            Sym *s = o->syms.v[i];
            int global = s->global || !s->defined;
            if (s == &dot_sym) continue;
            if (pass == 0 && (global || s->local_label || s->sec == -1)) continue;
            if (pass == 1 && !global) continue;
            if (!s->defined && !s->global) {
                /* an undefined name: global only if something refers to it */
                int used = 0;
                for (int k = 1; k <= o->nsecs && !used; k++)
                    for (int r = 0; r < o->secs[k]->rels.n; r++) if (((Rel *)o->secs[k]->rels.v[r])->sym == s) { used = 1; break; }
                if (!used) continue;
            }
            ESym e;
            e.name = str.len;
            buf_s(&str, s->name);
            buf_c(&str, 0);
            int bind = global ? (s->weak ? 2 : 1) : 0;
            e.info = bind << 4 | (s->type == 2 || s->type == 1 || s->type == 6 ? s->type : 0);
            e.other = 0;
            e.shndx = !s->defined ? 0 : s->sec == -1 ? 0xFFF1 : o->secs[s->sec]->idx;
            e.value = s->defined ? s->val : 0;
            e.size = s->size;
            buf_add(&symb, &e, sizeof e);
            s->idx = nsym++;
        }
        if (pass == 0) first_global = nsym;
    }
    /* relocations: a local symbol becomes its section + offset */
    Buf *rela = arena(sizeof(Buf) * (o->nsecs + 1));
    for (int i = 1; i <= o->nsecs; i++) {
        Sect *s = o->secs[i];
        for (int r = 0; r < s->rels.n; r++) {
            Rel *x = s->rels.v[r];
            Sym *t = x->sym;
            long addend = x->addend;
            int si;
            if (t == &dot_sym || (t->defined && !t->global && t->sec > 0)) { si = o->secs[t->sec]->symidx; addend += t->val; }
            else if (t->defined && t->sec == -1) aerr("relocation against a constant");
            else si = t->idx;
            put_u(&rela[i], x->off, 8);
            put_u(&rela[i], (unsigned long)si << 32 | x->type, 8);
            put_u(&rela[i], addend, 8);
        }
    }
    /* layout: header, section contents, then section headers */
    Buf b = { 0 };
    buf_zero(&b, 64);
    Shdr *sh = calloc(nsh, sizeof(Shdr));
    for (int i = 1; i <= o->nsecs; i++) {
        Sect *s = o->secs[i];
        Shdr *h = &sh[s->idx];
        h->name = shstr.len;
        buf_s(&shstr, s->name);
        buf_c(&shstr, 0);
        h->type = s->type;
        h->flags = s->flags;
        h->align = s->align;
        if (h->align > 1) buf_zero(&b, align_to(b.len, h->align) - b.len);
        h->off = b.len;
        h->size = s->type == SHT_NOBITS ? s->size : s->data.len;
        if (s->type != SHT_NOBITS) buf_add(&b, s->data.p ? s->data.p : "", s->data.len);
    }
    int k = o->nsecs + 1;
    for (int i = 1; i <= o->nsecs; i++) {
        if (!o->secs[i]->rels.n) continue;
        Shdr *h = &sh[k++];
        h->name = shstr.len;
        buf_s(&shstr, ".rela");
        buf_s(&shstr, o->secs[i]->name);
        buf_c(&shstr, 0);
        h->type = SHT_RELA;
        h->flags = SHF_INFO_LINK;
        h->link = symtab_idx;
        h->info = o->secs[i]->idx;
        h->align = 8;
        h->entsize = 24;
        buf_zero(&b, align_to(b.len, 8) - b.len);
        h->off = b.len;
        h->size = rela[i].len;
        buf_add(&b, rela[i].p ? rela[i].p : "", rela[i].len);
    }
    Shdr *h = &sh[symtab_idx];
    h->name = shstr.len; buf_s(&shstr, ".symtab"); buf_c(&shstr, 0);
    h->type = SHT_SYMTAB; h->link = strtab_idx; h->info = first_global; h->align = 8; h->entsize = 24;
    buf_zero(&b, align_to(b.len, 8) - b.len);
    h->off = b.len; h->size = symb.len; buf_add(&b, symb.p, symb.len);
    h = &sh[strtab_idx];
    h->name = shstr.len; buf_s(&shstr, ".strtab"); buf_c(&shstr, 0);
    h->type = SHT_STRTAB; h->align = 1;
    h->off = b.len; h->size = str.len; buf_add(&b, str.p, str.len);
    h = &sh[shstr_idx];
    h->name = shstr.len; buf_s(&shstr, ".shstrtab"); buf_c(&shstr, 0);
    h->type = SHT_STRTAB; h->align = 1;
    h->off = b.len; h->size = shstr.len; buf_add(&b, shstr.p, shstr.len);
    buf_zero(&b, align_to(b.len, 8) - b.len);
    long shoff = b.len;
    for (int i = 0; i < nsh; i++) {
        put_u(&b, sh[i].name, 4); put_u(&b, sh[i].type, 4); put_u(&b, sh[i].flags, 8); put_u(&b, sh[i].addr, 8);
        put_u(&b, sh[i].off, 8); put_u(&b, sh[i].size, 8); put_u(&b, sh[i].link, 4); put_u(&b, sh[i].info, 4);
        put_u(&b, sh[i].align, 8); put_u(&b, sh[i].entsize, 8);
    }
    /* the ELF header */
    unsigned char *e = (unsigned char *)b.p;
    memcpy(e, "\177ELF\2\1\1", 7);
    e[16] = 1;                                  /* ET_REL */
    e[18] = target == T_AARCH64 ? 183 : 62;     /* AArch64, x86-64 */
    e[20] = 1;
    memcpy(e + 40, &shoff, 8);
    e[52] = 64;                                 /* header size */
    e[58] = 64;                                 /* section header size */
    e[60] = nsh;
    e[61] = nsh >> 8;
    e[62] = shstr_idx;
    e[63] = shstr_idx >> 8;
    buf_add(out, b.p, b.len);
    free(b.p);
    free(sh);
}
