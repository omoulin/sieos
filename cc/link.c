/*
 * link.c - The static linker (ELF64 x86-64 or AArch64: the machine of its
 * input objects), plus "ar rcs" and
 * "objcopy -O binary".
 *
 *   sicc --ld [-T script] [-e entry] [-o out] [-L dir] [-lname] objects/archives
 *
 * 1. Read the objects; from the archives, take the members that define a
 *    symbol still undefined (again, until nothing changes).
 * 2. Place the input sections into output sections, as the linker script
 *    says (the subset SIEOS uses: ENTRY, SECTIONS with ". =", ALIGN, ADDR,
 *    AT(), KEEP, file and section patterns with *, symbol assignments,
 *    /DISCARD/, PHDRS with FLAGS). Without -T, a default script for a
 *    program at 4 MiB.
 * 3. Give every symbol its address, apply the relocations, and write the
 *    program: ELF header, program headers (one PT_LOAD per segment), the
 *    sections' bytes, section headers and a symbol table.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "sicc.h"

enum { SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4, SHT_NOBITS = 8 };
enum { SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXEC = 4, SHF_TLS = 0x400 };

typedef struct OutSec OutSec;
typedef struct InObj InObj;

typedef struct InSec {
    InObj *obj;
    const char *name;
    int type, idx;
    long flags, align, size;
    unsigned char *data;
    unsigned char *rela;       /* its relocations (ELF64_Rela) */
    long nrela;
    OutSec *out;
    void *rule;                /* the script rule that takes it */
    unsigned long addr;
    int discard;
} InSec;

typedef struct { const char *name; int bind, type, shndx; unsigned long value, size; struct Gsym *g; } LSym;

struct InObj {
    const char *name;
    unsigned char *img;
    long len;
    InSec **secs;
    int nsecs;
    LSym *syms;
    int nsyms;
};

typedef struct Gsym {
    const char *name;
    InSec *sec;                /* defined in (0: absolute or undefined) */
    unsigned long val;
    int defined, weak, abs, referenced, is_func, tls;
    unsigned long size;
    unsigned long addr;
} Gsym;

struct OutSec {
    const char *name;
    int type;
    long flags, align;
    unsigned long vma, lma, size, off;
    Vec in;
    int phdr;                  /* its segment (index), -1: none */
    int shidx;
};

static int machine;            /* the inputs' ELF machine: 62 x86-64, 183 AArch64 */
static Map gsyms;
static Vec gorder;                                 /* the same symbols, as first seen: a fixed output order */
static Vec objs, outs;
static const char *out_name = "a.out", *entry_name;

_Noreturn static void lerr(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    fputs("sicc ld: ", stderr);
    vfprintf(stderr, f, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static unsigned long rd(const unsigned char *p, int n)
{
    unsigned long v = 0;
    for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

static void wr(unsigned char *p, unsigned long v, int n) { for (int i = 0; i < n; i++) p[i] = v >> (8 * i); }

static Gsym *gsym(const char *name)
{
    const char *k = intern(name, strlen(name));
    Gsym *g = map_get(&gsyms, k);
    if (!g) {
        g = arena(sizeof *g);
        g->name = k;
        map_put(&gsyms, k, g);
        vec_push(&gorder, g);
    }
    return g;
}

/* ---- Reading objects */
static InObj *read_object(const char *name, unsigned char *img, long len)
{
    if (len < 64 || memcmp(img, "\177ELF\2\1", 6)) lerr("%s: not an ELF64 file", name);
    if (rd(img + 16, 2) != 1) lerr("%s: not a relocatable object", name);
    int m = rd(img + 18, 2);
    if (!machine) machine = m;
    else if (m != machine) lerr("%s: objects for two different machines", name);
    InObj *o = arena(sizeof *o);
    o->name = name;
    o->img = img;
    o->len = len;
    unsigned long shoff = rd(img + 40, 8);
    int shnum = rd(img + 60, 2), shstrndx = rd(img + 62, 2);
    unsigned char *sh = img + shoff;
    const char *shstr = (char *)img + rd(sh + shstrndx * 64 + 24, 8);
    o->secs = arena(sizeof(InSec *) * (shnum + 1));
    o->nsecs = shnum;
    int symtab = -1;
    for (int i = 1; i < shnum; i++) {
        unsigned char *h = sh + i * 64;
        int type = rd(h + 4, 4);
        if (type == SHT_SYMTAB) { symtab = i; continue; }
        if (type == SHT_RELA || type == SHT_STRTAB) continue;
        InSec *s = arena(sizeof *s);
        s->obj = o;
        s->idx = i;
        s->name = intern(shstr + rd(h, 4), strlen(shstr + rd(h, 4)));
        s->type = type;
        s->flags = rd(h + 8, 8);
        s->size = rd(h + 32, 8);
        s->align = rd(h + 48, 8);
        if (s->align < 1) s->align = 1;
        s->data = type == SHT_NOBITS ? 0 : img + rd(h + 24, 8);
        o->secs[i] = s;
    }
    for (int i = 1; i < shnum; i++) {                       /* relocations, for their sections */
        unsigned char *h = sh + i * 64;
        if (rd(h + 4, 4) != SHT_RELA) continue;
        InSec *s = o->secs[rd(h + 44, 4)];
        if (!s) continue;
        s->rela = img + rd(h + 24, 8);
        s->nrela = rd(h + 32, 8) / 24;
    }
    if (symtab < 0) return o;
    unsigned char *h = sh + symtab * 64;
    unsigned char *st = img + rd(h + 24, 8);
    int n = rd(h + 32, 8) / 24;
    const char *strtab = (char *)img + rd(sh + rd(h + 40, 4) * 64 + 24, 8);
    o->syms = arena(sizeof(LSym) * (n + 1));
    o->nsyms = n;
    for (int i = 0; i < n; i++) {
        unsigned char *e = st + i * 24;
        LSym *s = &o->syms[i];
        s->name = strtab + rd(e, 4);
        s->bind = e[4] >> 4;
        s->type = e[4] & 15;
        s->shndx = rd(e + 6, 2);
        s->value = rd(e + 8, 8);
        s->size = rd(e + 16, 8);
    }
    return o;
}

/* Its global symbols join the global table. */
static void add_object(InObj *o)
{
    vec_push(&objs, o);
    for (int i = 1; i < o->nsyms; i++) {
        LSym *s = &o->syms[i];
        if (s->bind == 0) continue;                         /* local */
        Gsym *g = gsym(s->name);
        s->g = g;
        if (s->shndx == 0) { g->referenced = 1; continue; } /* undefined here */
        if (s->shndx == 0xFFF2) {                           /* COMMON: a zeroed block in .bss */
            if (g->defined) continue;
            InSec *c = arena(sizeof *c);
            c->obj = o;
            c->name = intern("COMMON", 6);
            c->type = SHT_NOBITS;
            c->flags = SHF_ALLOC | SHF_WRITE;
            c->size = s->size;
            c->align = s->value ? s->value : 1;
            g->sec = c;
            g->val = 0;
            g->defined = 1;
            continue;
        }
        if (g->defined && !g->weak) {
            if (s->bind == 2) continue;                    /* a weak one after a strong one */
            lerr("%s: %s is defined twice (also in %s)", o->name, s->name, g->sec ? g->sec->obj->name : "the script");
        }
        g->defined = 1;
        g->weak = s->bind == 2;
        g->is_func = s->type == 2;
        g->tls = s->type == 6;
        g->size = s->size;
        if (s->shndx == 0xFFF1) { g->abs = 1; g->sec = 0; g->val = s->value; }
        else { g->sec = o->secs[s->shndx]; g->val = s->value; }
    }
}

/* ---- Archives */
typedef struct { const char *name; unsigned char *img; long len; InObj *obj; int loaded; } ArMember;

static void read_archive(const char *path, unsigned char *img, long len, Vec *members)
{
    long p = 8;
    const char *longnames = 0;
    while (p + 60 <= len) {
        unsigned char *h = img + p;
        long size = strtoull(xstrndup((char *)h + 48, 10), 0, 10);
        char nm[17];
        memcpy(nm, h, 16);
        nm[16] = 0;
        unsigned char *data = h + 60;
        if (!strncmp(nm, "// ", 3) || !strncmp(nm, "//  ", 4)) longnames = (char *)data;
        else if (nm[0] == '/' && (nm[1] == ' ' || nm[1] == '/')) { }       /* the symbol index */
        else {
            char name[256];
            if (nm[0] == '/' && longnames) {
                long off = strtoull(nm + 1, 0, 10);
                int k = 0;
                while (longnames[off + k] && longnames[off + k] != '/' && longnames[off + k] != '\n' && k < 255) { name[k] = longnames[off + k]; k++; }
                name[k] = 0;
            } else {
                int k = 0;
                while (k < 16 && nm[k] != '/' && nm[k] != ' ') { name[k] = nm[k]; k++; }
                name[k] = 0;
            }
            ArMember *m = arena(sizeof *m);
            m->name = fmt("%s(%s)", path, name);
            m->img = data;
            m->len = size;
            vec_push(members, m);
        }
        p += 60 + size + (size & 1);
    }
}

static unsigned char *load(const char *path, long *len)
{
    Buf b = { 0 };
    if (read_file(path, &b) < 0) lerr("%s: cannot read", path);
    *len = b.len;
    return (unsigned char *)b.p;
}

/* ---- Linker scripts */
typedef struct Ex Ex;
struct Ex { int kind; long v; const char *name; Ex *l, *r; int op; };   /* kinds: n number, s symbol, d dot, b binary, f function, u unary */

enum { ST_DOT, ST_ASSIGN, ST_OUT, ST_ENTRY };
typedef struct { const char *file; Vec secs; int keep; } Pattern;
typedef struct Stmt {
    int kind;
    const char *name;          /* symbol, or output section */
    Ex *e;                     /* value / address */
    Ex *at;                    /* AT(...) */
    Vec body;                  /* output section: Stmt (assignments) and patterns, in order */
    Vec pats;
    const char *phdr;
    int discard;
} Stmt;
typedef struct { Stmt *st; Pattern *pat; } BodyItem;
typedef struct { const char *name; int flags; int has_flags; } Phdr;

static const char *sp;                 /* the script, while parsing */
static Vec script, phdrs;
static Vec tls_phdrs;                  /* PHDRS names of type PT_TLS: the header is made anyway (write_output) */

static void sws(void)
{
    for (;;) {
        while (*sp == ' ' || *sp == '\t' || *sp == '\n' || *sp == '\r') sp++;
        if (sp[0] == '/' && sp[1] == '*') { const char *e = strstr(sp + 2, "*/"); if (!e) lerr("script: unterminated comment"); sp = e + 2; continue; }
        break;
    }
}

static int is_namech(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$' || c == '*' || c == '?' || c == '-' || c == '[' || c == ']'; }

static char *sword(void)
{
    sws();
    const char *s = sp;
    if (*sp == '/' && !strncmp(sp, "/DISCARD/", 9)) { sp += 9; return "/DISCARD/"; }
    while (is_namech(*sp)) sp++;
    if (sp == s) lerr("script: expected a name at '%.20s'", sp);
    return xstrndup(s, sp - s);
}

static int saccept(const char *t)
{
    sws();
    int n = strlen(t);
    if (!strncmp(sp, t, n)) { sp += n; return 1; }
    return 0;
}

static void sexpect(const char *t) { if (!saccept(t)) lerr("script: expected '%s' at '%.20s'", t, sp); }

static Ex *sexpr(void);
static Ex *sprimary(void)
{
    sws();
    Ex *e = arena(sizeof *e);
    if (*sp == '(') { sp++; e = sexpr(); sexpect(")"); return e; }
    if (*sp == '-' || *sp == '~' || *sp == '!') { e->kind = 'u'; e->op = *sp++; e->l = sprimary(); return e; }
    if (*sp >= '0' && *sp <= '9') {
        char *end;
        e->kind = 'n';
        e->v = strtoull(sp, &end, 0);
        sp = end;
        if (*sp == 'K' || *sp == 'k') { e->v <<= 10; sp++; }
        else if (*sp == 'M' || *sp == 'm') { e->v <<= 20; sp++; }
        return e;
    }
    if (*sp == '.' && !is_namech(sp[1])) { sp++; e->kind = 'd'; return e; }
    const char *s = sp;
    while ((*sp >= 'a' && *sp <= 'z') || (*sp >= 'A' && *sp <= 'Z') || (*sp >= '0' && *sp <= '9') || *sp == '_' || *sp == '.' || *sp == '$') sp++;
    if (sp == s) lerr("script: bad expression at '%.20s'", sp);
    char *name = xstrndup(s, sp - s);
    sws();
    if (*sp == '(') {                          /* ALIGN(x), ADDR(sec), SIZEOF(sec), LOADADDR(sec), ALIGNOF(sec), MAX, MIN */
        sp++;
        e->kind = 'f';
        e->name = name;
        if (!strcmp(name, "ADDR") || !strcmp(name, "SIZEOF") || !strcmp(name, "LOADADDR") || !strcmp(name, "ALIGNOF")) {
            Ex *a = arena(sizeof *a);
            a->kind = 's';
            a->name = sword();
            e->l = a;
        } else {
            e->l = sexpr();
            if (saccept(",")) e->r = sexpr();
        }
        sexpect(")");
        return e;
    }
    e->kind = 's';
    e->name = name;
    return e;
}

static int sprec(int *op)
{
    sws();
    static const struct { const char *s; int p; } t[] = { { "<<", 5 }, { ">>", 5 }, { "|", 1 }, { "&", 2 }, { "+", 6 }, { "-", 6 }, { "*", 7 }, { "/", 7 }, { "%", 7 } };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strncmp(sp, t[i].s, strlen(t[i].s)) && !(t[i].s[0] == '/' && sp[1] == '*')) { *op = t[i].s[0] << 8 | t[i].s[1]; return t[i].p; }
    return 0;
}

static Ex *sbin(int minp)
{
    Ex *l = sprimary();
    for (;;) {
        int op, p = sprec(&op);
        if (!p || p < minp) return l;
        sp += (op & 0xff) ? 2 : 1;
        Ex *e = arena(sizeof *e);
        e->kind = 'b';
        e->op = op;
        e->l = l;
        e->r = sbin(p + 1);
        l = e;
    }
}

static Ex *sexpr(void) { return sbin(1); }

static Stmt *new_stmt(int kind) { Stmt *s = arena(sizeof *s); s->kind = kind; return s; }

static Pattern *parse_pattern(void)
{
    Pattern *p = arena(sizeof *p);
    if (saccept("KEEP")) { sexpect("("); p = parse_pattern(); p->keep = 1; sexpect(")"); return p; }
    p->file = sword();
    sexpect("(");
    while (!saccept(")")) vec_push(&p->secs, sword());
    return p;
}

static void parse_script(const char *text)
{
    sp = text;
    for (;;) {
        sws();
        if (!*sp) break;
        if (saccept("ENTRY")) { sexpect("("); Stmt *s = new_stmt(ST_ENTRY); s->name = sword(); sexpect(")"); vec_push(&script, s); continue; }
        if (saccept("OUTPUT_FORMAT") || saccept("OUTPUT_ARCH") || saccept("SEARCH_DIR")) {
            while (*sp && *sp != ')') sp++;
            sexpect(")");
            continue;
        }
        if (saccept("PHDRS")) {
            sexpect("{");
            while (!saccept("}")) {
                Phdr *ph = arena(sizeof *ph);
                ph->name = sword();
                char *type = sword();
                int tls = !strcmp(type, "PT_TLS");      /* the TLS header: written from the TLS sections */
                if (strcmp(type, "PT_LOAD") && !tls) lerr("script: only PT_LOAD and PT_TLS segments are supported");
                if (saccept("FLAGS")) { sexpect("("); ph->flags = strtol(sp, (char **)&sp, 0); ph->has_flags = 1; sexpect(")"); }
                sexpect(";");
                vec_push(tls ? &tls_phdrs : &phdrs, ph);
            }
            continue;
        }
        if (saccept("SECTIONS")) {
            sexpect("{");
            while (!saccept("}")) {
                sws();
                if (*sp == '.' && !is_namech(sp[1])) {             /* . = expr; */
                    sp++;
                    sexpect("=");
                    Stmt *s = new_stmt(ST_DOT);
                    s->e = sexpr();
                    sexpect(";");
                    vec_push(&script, s);
                    continue;
                }
                const char *save = sp;
                char *name = sword();
                sws();
                if (*sp == '=' && sp[1] != '=') {                  /* symbol = expr; */
                    sp++;
                    Stmt *s = new_stmt(ST_ASSIGN);
                    s->name = name;
                    s->e = sexpr();
                    sexpect(";");
                    vec_push(&script, s);
                    continue;
                }
                (void)save;
                Stmt *s = new_stmt(ST_OUT);                       /* an output section */
                s->name = name;
                s->discard = !strcmp(name, "/DISCARD/");
                sws();
                if (*sp != ':') s->e = sexpr();
                sexpect(":");
                if (saccept("AT")) { sexpect("("); s->at = sexpr(); sexpect(")"); }
                sexpect("{");
                while (!saccept("}")) {
                    sws();
                    BodyItem *bi = arena(sizeof *bi);
                    if (*sp == '.' && !is_namech(sp[1])) {
                        sp++;
                        sexpect("=");
                        bi->st = new_stmt(ST_DOT);
                        bi->st->e = sexpr();
                        sexpect(";");
                    } else {
                        const char *at = sp;
                        char *w = sword();
                        sws();
                        if (*sp == '=') {
                            sp++;
                            bi->st = new_stmt(ST_ASSIGN);
                            bi->st->name = w;
                            bi->st->e = sexpr();
                            sexpect(";");
                        } else {
                            sp = at;
                            bi->pat = parse_pattern();
                        }
                    }
                    vec_push(&s->body, bi);
                }
                sws();
                while (*sp == ':' && sp[1] != ':') {       /* ":data", maybe ":tls" too (the TLS one is implied) */
                    sp++;
                    char *n = sword();
                    int t = 0;
                    for (int p = 0; p < tls_phdrs.n; p++) t |= !strcmp(((Phdr *)tls_phdrs.v[p])->name, n);
                    if (!t && !s->phdr) s->phdr = n;
                    sws();
                }
                vec_push(&script, s);
            }
            continue;
        }
        if (is_namech(*sp)) {                              /* NAME = expr; outside SECTIONS */
            char *name = sword();
            sexpect("=");
            Stmt *s = new_stmt(ST_ASSIGN);
            s->name = name;
            s->e = sexpr();
            sexpect(";");
            vec_push(&script, s);
            continue;
        }
        lerr("script: unexpected '%.20s'", sp);
    }
}

static const char default_script[] =
    "ENTRY(_start)\n"
    "SECTIONS {\n"
    "  . = 0x400000;\n"
    "  .text : { *(.text .text.*) }\n"
    "  . = ALIGN(4096);\n"
    "  .rodata : { *(.rodata .rodata.*) }\n"
    "  . = ALIGN(4096);\n"
    "  .data : { *(.data .data.*) }\n"
    "  .tdata : { *(.tdata .tdata.*) }\n"
    "  .tbss : { *(.tbss .tbss.*) }\n"
    "  .bss : { *(.bss .bss.*) *(COMMON) }\n"
    "}\n";

/* .tbss: the zero tail of the thread-local template; each thread gets its own
 * copy, so it takes no room at its address (the next section starts there) */
static int is_tbss(OutSec *o) { return (o->flags & SHF_TLS) && o->type == SHT_NOBITS; }

/* the thread-local template: [start, start + filesz) initialized, memsz in all */
static unsigned long tls_start, tls_filesz, tls_memsz, tls_align = 1;
static unsigned long up(unsigned long v, unsigned long a) { return (v + a - 1) / a * a; }

static int glob(const char *p, const char *s)
{
    if (!*p) return !*s;
    if (*p == '*') return glob(p + 1, s) || (*s && glob(p, s + 1));
    if (*p == '?') return *s && glob(p + 1, s + 1);
    return *p == *s && glob(p + 1, s + 1);
}

static unsigned long dot;
static const char *base_of(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

static OutSec *find_out(const char *name)
{
    for (int i = 0; i < outs.n; i++) if (!strcmp(((OutSec *)outs.v[i])->name, name)) return outs.v[i];
    return 0;
}

static unsigned long sym_value(const char *name)
{
    Gsym *g = map_get(&gsyms, intern(name, strlen(name)));
    if (!g || !g->defined) lerr("script: %s is not defined (yet)", name);
    if (g->abs || !g->sec) return g->val;
    if (!g->sec->out) lerr("script: %s is not placed yet", name);
    return g->sec->addr + g->val;
}

static unsigned long seval(Ex *e)
{
    switch (e->kind) {
    case 'n': return e->v;
    case 'd': return dot;
    case 's': return sym_value(e->name);
    case 'u': { unsigned long v = seval(e->l); return e->op == '-' ? -v : e->op == '~' ? ~v : !v; }
    case 'f': {
        if (!strcmp(e->name, "ALIGN")) {
            unsigned long a = e->r ? seval(e->r) : seval(e->l), v = e->r ? seval(e->l) : dot;
            return a ? (v + a - 1) / a * a : v;
        }
        if (!strcmp(e->name, "ADDR") || !strcmp(e->name, "SIZEOF") || !strcmp(e->name, "LOADADDR") || !strcmp(e->name, "ALIGNOF")) {
            OutSec *o = find_out(e->l->name);
            if (!o && (e->name[0] == 'S' || e->name[1] == 'L')) return 0;     /* an empty section: size 0, no alignment */
            if (!o) lerr("script: no section %s", e->l->name);
            return e->name[1] == 'L' ? (unsigned long)o->align : e->name[0] == 'A' ? o->vma : e->name[0] == 'S' ? o->size : o->lma;
        }
        if (!strcmp(e->name, "MAX")) { unsigned long a = seval(e->l), b = seval(e->r); return a > b ? a : b; }
        if (!strcmp(e->name, "MIN")) { unsigned long a = seval(e->l), b = seval(e->r); return a < b ? a : b; }
        lerr("script: unknown function %s", e->name);
    }
    }
    unsigned long a = seval(e->l), b = seval(e->r);
    switch (e->op) {
    case '+' << 8: return a + b;
    case '-' << 8: return a - b;
    case '*' << 8: return a * b;
    case '/' << 8: return b ? a / b : 0;
    case '%' << 8: return b ? a % b : 0;
    case '&' << 8: return a & b;
    case '|' << 8: return a | b;
    case ('<' << 8) | '<': return a << b;
    case ('>' << 8) | '>': return a >> b;
    }
    lerr("script: bad operator");
}

static void define_abs(const char *name, unsigned long v)
{
    Gsym *g = gsym(name);
    g->defined = 1;
    g->abs = 1;
    g->sec = 0;
    g->val = v;
}

/* ---- Placing sections */
static int matches(Pattern *p, InSec *s)
{
    if (!glob(p->file, base_of(s->obj->name))) { if (strcmp(p->file, "*")) return 0; }
    for (int i = 0; i < p->secs.n; i++) if (glob(p->secs.v[i], s->name)) return 1;
    return 0;
}

static void assign_sections(void)
{
    /* each input section goes to the first rule that matches it */
    for (int o = 0; o < objs.n; o++) {
        InObj *ob = objs.v[o];
        for (int i = 1; i < ob->nsecs; i++) {
            InSec *s = ob->secs[i];
            if (!s || !(s->flags & SHF_ALLOC)) continue;
            int placed = 0;
            for (int k = 0; k < script.n && !placed; k++) {
                Stmt *st = script.v[k];
                if (st->kind != ST_OUT) continue;
                for (int b = 0; b < st->body.n && !placed; b++) {
                    BodyItem *bi = st->body.v[b];
                    if (!bi->pat || !matches(bi->pat, s)) continue;
                    if (st->discard) s->discard = 1;
                    else s->rule = bi;
                    placed = 1;
                }
            }
        }
    }
}

static void layout(void)
{
    dot = 0;
    unsigned long delta = 0;                                  /* lma - vma of the previous section */
    for (int k = 0; k < script.n; k++) {
        Stmt *st = script.v[k];
        if (st->kind == ST_ENTRY) { if (!entry_name) entry_name = st->name; continue; }
        if (st->kind == ST_DOT) { dot = seval(st->e); continue; }
        if (st->kind == ST_ASSIGN) { define_abs(st->name, seval(st->e)); continue; }
        if (st->discard) continue;
        OutSec *o = arena(sizeof *o);
        o->name = st->name;
        o->type = SHT_NOBITS;
        o->align = 1;
        o->phdr = -1;
        /* its input sections, in rule order then object order */
        for (int b = 0; b < st->body.n; b++) {
            BodyItem *bi = st->body.v[b];
            if (!bi->pat) continue;
            for (int ob = 0; ob < objs.n; ob++) {
                InObj *obj = objs.v[ob];
                for (int i = 1; i < obj->nsecs; i++) {
                    InSec *s = obj->secs[i];
                    if (s && s->rule == bi) {
                        if (s->align > o->align) o->align = s->align;
                        o->flags |= s->flags;
                        if (s->type != SHT_NOBITS) o->type = SHT_PROGBITS;
                    }
                }
            }
        }
        if (st->e) dot = seval(st->e);
        dot = (dot + o->align - 1) / o->align * o->align;
        o->vma = dot;
        vec_push(&outs, o);                                   /* (AT may name it: ADDR(.text)) */
        if (st->at) delta = seval(st->at) - dot;
        o->lma = o->vma + delta;
        for (int b = 0; b < st->body.n; b++) {
            BodyItem *bi = st->body.v[b];
            if (bi->st) {
                if (bi->st->kind == ST_DOT) dot = seval(bi->st->e);
                else define_abs(bi->st->name, seval(bi->st->e));
                continue;
            }
            for (int ob = 0; ob < objs.n; ob++) {
                InObj *obj = objs.v[ob];
                for (int i = 1; i < obj->nsecs; i++) {
                    InSec *s = obj->secs[i];
                    if (!s || s->rule != bi) continue;
                    dot = (dot + s->align - 1) / s->align * s->align;
                    s->addr = dot;
                    s->out = o;
                    vec_push(&o->in, s);
                    dot += s->size;
                }
                /* COMMON symbols of this object, for a *(COMMON) rule */
                for (int c = 0; c < bi->pat->secs.n; c++) {
                    if (strcmp(bi->pat->secs.v[c], "COMMON")) continue;
                    for (int y = 1; y < obj->nsyms; y++) {
                        LSym *ls = &obj->syms[y];
                        if (ls->shndx != 0xFFF2 || !ls->g || !ls->g->sec || ls->g->sec->out) continue;
                        InSec *s = ls->g->sec;
                        dot = (dot + s->align - 1) / s->align * s->align;
                        s->addr = dot;
                        s->out = o;
                        vec_push(&o->in, s);
                        dot += s->size;
                    }
                }
            }
        }
        o->size = dot - o->vma;
        if (!o->in.n && !o->size) { outs.n--; continue; }   /* empty: drop it */
        if (is_tbss(o)) dot = o->vma;
        if (st->phdr) {
            for (int p = 0; p < phdrs.n; p++) if (!strcmp(((Phdr *)phdrs.v[p])->name, st->phdr)) o->phdr = p;
            if (o->phdr < 0) lerr("script: no segment %s", st->phdr);
        }
    }
    /* orphans: each in an output section of its own name, after the others */
    for (int ob = 0; ob < objs.n; ob++) {
        InObj *obj = objs.v[ob];
        for (int i = 1; i < obj->nsecs; i++) {
            InSec *s = obj->secs[i];
            if (!s || !(s->flags & SHF_ALLOC) || s->discard || s->out) continue;
            OutSec *o = find_out(s->name);
            if (!o) {
                o = arena(sizeof *o);
                o->name = s->name;
                o->type = s->type;
                o->align = s->align;
                o->flags = s->flags;
                o->phdr = -1;
                dot = (dot + s->align - 1) / s->align * s->align;
                o->vma = o->lma = dot;
                vec_push(&outs, o);
            } else if (o != outs.v[outs.n - 1]) lerr("%s: cannot place section %s", obj->name, s->name);
            dot = (dot + s->align - 1) / s->align * s->align;
            s->addr = dot;
            s->out = o;
            if (s->type != SHT_NOBITS) o->type = SHT_PROGBITS;
            vec_push(&o->in, s);
            dot += s->size;
            o->size = dot - o->vma;
            if (is_tbss(o)) dot = o->vma;
        }
    }
    /* the TLS template: the thread-local sections, which must be adjacent */
    for (int i = 0; i < outs.n; i++) {
        OutSec *o = outs.v[i];
        if (!(o->flags & SHF_TLS)) continue;
        if (!tls_memsz) tls_start = o->vma;
        else if (o->vma != tls_start + tls_memsz && o->vma != up(tls_start + tls_memsz, o->align))
            lerr("thread-local section %s is not next to the others", o->name);
        tls_memsz = o->vma + o->size - tls_start;
        if (o->type != SHT_NOBITS) tls_filesz = tls_memsz;
        if (o->align > (long)tls_align) tls_align = o->align;
    }
}

/* a thread-local variable's offset from the thread pointer: the block ends there (x86-64, variant II) */
/* a thread-local variable's offset from the thread pointer: x86-64 puts the
 * block below it, AArch64 above it, after 16 reserved bytes */
static long tpoff(unsigned long S)
{
    if (machine == 183) return (long)(S - tls_start) + (long)up(16, tls_align);
    return (long)(S - tls_start) - (long)up(tls_memsz, tls_align);
}

static unsigned long sym_addr(InObj *o, int idx, InSec *at, long off);

/* AArch64 relocations: data, and the immediate fields of instructions */
static void relocate_a64(InSec *s, unsigned char *buf)
{
    for (long i = 0; i < s->nrela; i++) {
        unsigned char *r = s->rela + i * 24;
        unsigned long off = rd(r, 8), info = rd(r + 8, 8);
        long A = rd(r + 16, 8);
        int type = info & 0xffffffff, idx = info >> 32;
        unsigned long S = sym_addr(s->obj, idx, s, off), P = s->addr + off;
        unsigned char *p = buf + off;
        unsigned w = rd(p, 4);
        long x = S + A - P, v;
        const char *who = s->obj->syms[idx].name;
#define RANGE(bits) do { if (x < -(1L << ((bits) - 1)) || x >= (1L << ((bits) - 1))) lerr("%s: a reference to %s does not reach", s->obj->name, who); } while (0)
        switch (type) {
        case 0: break;
        case 257: wr(p, S + A, 8); break;                                       /* ABS64 */
        case 258: v = S + A; if (v < -(1L << 31) || v > 0xffffffffL) lerr("%s: %s does not fit in 32 bits", s->obj->name, who); wr(p, v, 4); break;
        case 259: wr(p, S + A, 2); break;
        case 260: wr(p, x, 8); break;                                           /* PREL64 */
        case 261: RANGE(32); wr(p, x, 4); break;
        case 262: RANGE(16); wr(p, x, 2); break;
        case 274: RANGE(21); wr(p, (w & 0x9f00001f) | (x & 3) << 29 | ((x >> 2) & 0x7ffff) << 5, 4); break;   /* ADR */
        case 275:                                                               /* ADRP: the page */
            x = (long)(((S + A) & ~0xfffUL) - (P & ~0xfffUL)) >> 12;
            if (x < -(1L << 20) || x >= (1L << 20)) lerr("%s: %s is more than 4 GiB away", s->obj->name, who);
            wr(p, (w & 0x9f00001f) | (x & 3) << 29 | ((x >> 2) & 0x7ffff) << 5, 4);
            break;
        case 277: wr(p, (w & ~(0xfffu << 10)) | ((S + A) & 0xfff) << 10, 4); break;   /* ADD :lo12: */
        case 278: case 284: case 285: case 286: case 299: {                    /* LDST :lo12:, scaled */
            int sh = type == 278 ? 0 : type == 284 ? 1 : type == 285 ? 2 : type == 286 ? 3 : 4;
            unsigned long lo = (S + A) & 0xfff;
            if (lo & ((1u << sh) - 1)) lerr("%s: %s is not aligned for this access", s->obj->name, who);
            wr(p, (w & ~(0xfffu << 10)) | (lo >> sh) << 10, 4);
            break;
        }
        case 279: RANGE(16); wr(p, (w & ~(0x3fffu << 5)) | ((x >> 2) & 0x3fff) << 5, 4); break;     /* TBZ */
        case 280: RANGE(21); wr(p, (w & ~(0x7ffffu << 5)) | ((x >> 2) & 0x7ffff) << 5, 4); break;   /* B.cond, CBZ */
        case 282: case 283: RANGE(28); wr(p, (w & 0xfc000000) | ((x >> 2) & 0x3ffffff), 4); break;   /* B, BL */
        case 549: case 550: case 551:                                           /* TLS local-exec: tp + offset */
            v = tpoff(S) + A;
            if (v < 0 || (type == 549 && v >= 1L << 24) || (type == 550 && v >= 4096)) lerr("%s: thread-local offset of %s out of range", s->obj->name, who);
            wr(p, (w & ~(0xfffu << 10)) | ((type == 549 ? v >> 12 : v) & 0xfff) << 10, 4);
            break;
        default: lerr("%s: unsupported AArch64 relocation type %d", s->obj->name, type);
        }
#undef RANGE
    }
}

/* ---- Relocations */
static unsigned long sym_addr(InObj *o, int idx, InSec *at, long off)
{
    LSym *s = &o->syms[idx];
    if (s->bind != 0 && s->g) {
        Gsym *g = s->g;
        if (!g->defined) {
            if (s->bind == 2 || g->weak) return 0;
            lerr("%s: undefined reference to %s", o->name, s->name);
        }
        if (g->abs || !g->sec) return g->val;
        return g->sec->addr + g->val;
    }
    if (s->shndx == 0xFFF1) return s->value;
    if (s->shndx == 0 || s->shndx >= o->nsecs || !o->secs[s->shndx]) lerr("%s: a relocation against an unknown section (at %s+%ld)", o->name, at->name, off);
    InSec *t = o->secs[s->shndx];
    if (t->discard) lerr("%s: a reference to the discarded section %s", o->name, t->name);
    return t->addr + s->value;
}

static void relocate(InSec *s, unsigned char *buf)
{
    if (machine == 183) { relocate_a64(s, buf); return; }
    for (long i = 0; i < s->nrela; i++) {
        unsigned char *r = s->rela + i * 24;
        unsigned long off = rd(r, 8), info = rd(r + 8, 8);
        long A = rd(r + 16, 8);
        int type = info & 0xffffffff, idx = info >> 32;
        unsigned long S = sym_addr(s->obj, idx, s, off), P = s->addr + off;
        unsigned char *p = buf + off;
        long v;
        switch (type) {
        case 0: break;
        case 1: wr(p, S + A, 8); break;                                 /* 64 */
        case 24: wr(p, S + A - P, 8); break;                            /* PC64 */
        case 2: case 4:                                                 /* PC32, PLT32 */
            v = S + A - P;
            if (v != (long)(int)v) lerr("%s: a pc-relative reference to %s does not reach", s->obj->name, s->obj->syms[idx].name);
            wr(p, v, 4);
            break;
        case 10:                                                        /* 32 */
            v = S + A;
            if ((unsigned long)v > 0xffffffffUL) lerr("%s: address of %s does not fit in 32 bits", s->obj->name, s->obj->syms[idx].name);
            wr(p, v, 4);
            break;
        case 11:                                                        /* 32S */
            v = S + A;
            if (v != (long)(int)v) lerr("%s: address of %s does not fit in signed 32 bits", s->obj->name, s->obj->syms[idx].name);
            wr(p, v, 4);
            break;
        case 23:                                                        /* TPOFF32 */
            v = tpoff(S) + A;
            if (v != (long)(int)v) lerr("%s: thread-local offset of %s does not fit", s->obj->name, s->obj->syms[idx].name);
            wr(p, v, 4);
            break;
        case 18: wr(p, tpoff(S) + A, 8); break;                        /* TPOFF64 */
        case 22:                                                        /* GOTTPOFF: static, so become local-exec */
            if (off < 3 || (p[-3] & 0xF8) != 0x48 || (p[-1] & 0xC7) != 0x05) lerr("%s: unexpected code at a GOTTPOFF relocation", s->obj->name);
            if (p[-2] == 0x8B) p[-2] = 0xC7;                            /* movq x@gottpoff(%rip), %r -> movq $off, %r */
            else if (p[-2] == 0x03) p[-2] = 0x81;                       /* addq x@gottpoff(%rip), %r -> addq $off, %r */
            else lerr("%s: unexpected instruction at a GOTTPOFF relocation", s->obj->name);
            p[-3] = 0x48 | (p[-3] & 4) >> 2;                            /* REX.R -> REX.B */
            p[-1] = (unsigned char)(0xC0 | (p[-1] >> 3 & 7));                /* ModRM: register direct, /0 */
            wr(p, tpoff(S) + A + 4, 4);                                 /* (A was -4: pc-relative to the end) */
            break;
        case 12: wr(p, S + A, 2); break;
        case 13: wr(p, S + A - P, 2); break;
        case 14: wr(p, S + A, 1); break;
        case 15: wr(p, S + A - P, 1); break;
        default: lerr("%s: unsupported relocation type %d", s->obj->name, type);
        }
    }
}

/* ---- Output */
typedef struct { unsigned long vaddr, paddr, off, filesz, memsz, align; int flags; } Seg;

static void put(Buf *b, unsigned long v, int n) { for (int i = 0; i < n; i++) buf_c(b, i < 8 ? v >> (8 * i) : 0); }

static void write_output(void)
{
    /* segments: PHDRS, or one per run of sections with the same rights and load offset */
    Vec segs = { 0 };
    if (phdrs.n) {
        for (int p = 0; p < phdrs.n; p++) { Seg *g = arena(sizeof *g); g->flags = ((Phdr *)phdrs.v[p])->flags; vec_push(&segs, g); }
        int last = -1;
        for (int i = 0; i < outs.n; i++) {
            OutSec *o = outs.v[i];
            if (o->phdr < 0) o->phdr = last;
            last = o->phdr;
        }
    } else {
        Seg *g = 0;
        OutSec *prev = 0;
        for (int i = 0; i < outs.n; i++) {
            OutSec *o = outs.v[i];
            if (is_tbss(o)) { o->phdr = segs.n - 1; continue; }
            int fl = 4 | (o->flags & SHF_WRITE ? 2 : 0) | (o->flags & SHF_EXEC ? 1 : 0);
            if (!g || g->flags != fl || prev->lma - prev->vma != o->lma - o->vma || o->vma < prev->vma + prev->size ||
                o->vma - (prev->vma + prev->size) >= 4096 || (prev->type == SHT_NOBITS && o->type != SHT_NOBITS)) {
                g = arena(sizeof *g);
                g->flags = fl;
                vec_push(&segs, g);
            }
            o->phdr = segs.n - 1;
            prev = o;
        }
    }
    /* file layout: headers, then each segment's sections, congruent to their address mod the page size */
    int nph = segs.n + (tls_memsz != 0);                      /* + PT_TLS */
    long off = 64 + 56L * nph;
    for (int k = 0; k < segs.n; k++) {
        Seg *g = segs.v[k];
        int first = 1;
        for (int i = 0; i < outs.n; i++) {
            OutSec *o = outs.v[i];
            if (o->phdr != k) continue;
            if (is_tbss(o)) { o->off = g->off + (o->vma - g->vaddr); continue; }
            if (first) {
                long want = o->vma % 4096;
                off = off % 4096 <= want ? off - off % 4096 + want : off - off % 4096 + 4096 + want;
                g->vaddr = o->vma;
                g->paddr = o->lma;
                g->off = off;
                g->align = 4096;
                first = 0;
            }
            o->off = g->off + (o->vma - g->vaddr);
            if (o->type != SHT_NOBITS) { g->filesz = o->vma + o->size - g->vaddr; off = o->off + o->size; }
            g->memsz = o->vma + o->size - g->vaddr;
        }
    }
    /* the image */
    Buf b = { 0 };
    long total = off;
    for (int i = 0; i < outs.n; i++) { OutSec *o = outs.v[i]; if (o->type != SHT_NOBITS && (long)(o->off + o->size) > total) total = o->off + o->size; }
    buf_zero(&b, total);
    for (int i = 0; i < outs.n; i++) {
        OutSec *o = outs.v[i];
        if (o->type == SHT_NOBITS) continue;
        for (int k = 0; k < o->in.n; k++) {
            InSec *s = o->in.v[k];
            if (s->type == SHT_NOBITS) continue;
            unsigned char *dst = (unsigned char *)b.p + o->off + (s->addr - o->vma);
            memcpy(dst, s->data, s->size);
            relocate(s, dst);
        }
    }
    for (int i = 0; i < outs.n; i++) {                    /* relocations of .bss parts (none expected) */
        OutSec *o = outs.v[i];
        for (int k = 0; k < o->in.n; k++) { InSec *s = o->in.v[k]; if (s->type == SHT_NOBITS && s->nrela) lerr("relocations in a .bss section"); }
    }
    /* .debug_*: not loaded; joined by name after the image, and relocated */
    Vec dbg = { 0 };
    for (int ob = 0; ob < objs.n; ob++) {
        InObj *obj = objs.v[ob];
        for (int i = 1; i < obj->nsecs; i++) {
            InSec *s = obj->secs[i];
            if (!s || (s->flags & SHF_ALLOC) || s->type != SHT_PROGBITS || strncmp(s->name, ".debug", 6)) continue;
            OutSec *o = 0;
            for (int k = 0; k < dbg.n && !o; k++) if (!strcmp(((OutSec *)dbg.v[k])->name, s->name)) o = dbg.v[k];
            if (!o) { o = arena(sizeof *o); o->name = s->name; o->type = SHT_PROGBITS; o->align = 1; vec_push(&dbg, o); }
            if (s->align > o->align) o->align = s->align;
            o->size = up(o->size, s->align);
            s->addr = o->size;
            s->out = o;
            vec_push(&o->in, s);
            o->size += s->size;
        }
    }
    long dend = b.len;                                    /* (past the .bss offsets too: no overlap) */
    for (int i = 0; i < outs.n; i++) if ((long)((OutSec *)outs.v[i])->off > dend) dend = ((OutSec *)outs.v[i])->off;
    if (dbg.n) buf_zero(&b, dend - b.len);
    for (int k = 0; k < dbg.n; k++) {
        OutSec *o = dbg.v[k];
        o->off = b.len;
        buf_zero(&b, o->size);
        for (int i = 0; i < o->in.n; i++) {
            InSec *s = o->in.v[i];
            memcpy(b.p + o->off + s->addr, s->data, s->size);
            relocate(s, (unsigned char *)b.p + o->off + s->addr);
        }
    }
    /* symbol table (globals, for debugging tools) and section headers */
    Buf str = { 0 }, symb = { 0 }, shstr = { 0 };
    buf_c(&str, 0);
    buf_c(&shstr, 0);
    put(&symb, 0, 24);
    int nsyms = 1;
    for (int i = 0; i < gorder.n; i++) {
        Gsym *g = gorder.v[i];
        if (!g || !g->defined) continue;
        unsigned long addr = g->abs || !g->sec ? g->val : g->sec->addr + g->val;
        int shndx = 0xFFF1;
        if (g->sec && g->sec->out) shndx = g->sec->out->shidx;
        put(&symb, str.len, 4);
        buf_s(&str, g->name);
        buf_c(&str, 0);
        buf_c(&symb, 1 << 4 | (g->is_func ? 2 : g->tls ? 6 : g->sec ? 1 : 0));
        buf_c(&symb, 0);
        put(&symb, shndx, 2);
        put(&symb, addr, 8);
        put(&symb, g->size, 8);
        nsyms++;
    }
    for (int i = 0; i < outs.n; i++) ((OutSec *)outs.v[i])->shidx = i + 1;
    /* (the symbol table was built before shidx: fix the section indexes) */
    for (int k = 1, i = 0; i < gorder.n; i++) {
        Gsym *g = gorder.v[i];
        if (!g || !g->defined) continue;
        if (g->sec && g->sec->out) wr((unsigned char *)symb.p + k * 24 + 6, g->sec->out->shidx, 2);
        k++;
    }
    long symoff = align_to(b.len, 8);
    buf_zero(&b, symoff - b.len);
    buf_add(&b, symb.p, symb.len);
    long stroff = b.len;
    buf_add(&b, str.p, str.len);
    int nsh = outs.n + dbg.n + 4;
    long shstroff = b.len;
    Buf sh = { 0 };
    put(&sh, 0, 64);
    for (int i = 0; i < outs.n; i++) {
        OutSec *o = outs.v[i];
        put(&sh, shstr.len, 4);
        buf_s(&shstr, o->name);
        buf_c(&shstr, 0);
        put(&sh, o->type, 4); put(&sh, o->flags, 8); put(&sh, o->vma, 8); put(&sh, o->off, 8); put(&sh, o->size, 8);
        put(&sh, 0, 4); put(&sh, 0, 4); put(&sh, o->align, 8); put(&sh, 0, 8);
    }
    for (int k = 0; k < dbg.n; k++) {
        OutSec *o = dbg.v[k];
        put(&sh, shstr.len, 4);
        buf_s(&shstr, o->name);
        buf_c(&shstr, 0);
        put(&sh, o->type, 4); put(&sh, 0, 8); put(&sh, 0, 8); put(&sh, o->off, 8); put(&sh, o->size, 8);
        put(&sh, 0, 4); put(&sh, 0, 4); put(&sh, o->align, 8); put(&sh, 0, 8);
    }
    int symidx = outs.n + dbg.n + 1;
    long n1 = shstr.len; buf_s(&shstr, ".symtab"); buf_c(&shstr, 0);
    long n2 = shstr.len; buf_s(&shstr, ".strtab"); buf_c(&shstr, 0);
    long n3 = shstr.len; buf_s(&shstr, ".shstrtab"); buf_c(&shstr, 0);
    put(&sh, n1, 4); put(&sh, SHT_SYMTAB, 4); put(&sh, 0, 8); put(&sh, 0, 8); put(&sh, symoff, 8); put(&sh, symb.len, 8);
    put(&sh, symidx + 1, 4); put(&sh, 1, 4); put(&sh, 8, 8); put(&sh, 24, 8);
    put(&sh, n2, 4); put(&sh, SHT_STRTAB, 4); put(&sh, 0, 8); put(&sh, 0, 8); put(&sh, stroff, 8); put(&sh, str.len, 8);
    put(&sh, 0, 4); put(&sh, 0, 4); put(&sh, 1, 8); put(&sh, 0, 8);
    put(&sh, n3, 4); put(&sh, SHT_STRTAB, 4); put(&sh, 0, 8); put(&sh, 0, 8); put(&sh, shstroff, 8); put(&sh, shstr.len, 8);
    put(&sh, 0, 4); put(&sh, 0, 4); put(&sh, 1, 8); put(&sh, 0, 8);
    buf_add(&b, shstr.p, shstr.len);
    long shoff = align_to(b.len, 8);
    buf_zero(&b, shoff - b.len);
    buf_add(&b, sh.p, sh.len);
    /* ELF header and program headers */
    unsigned char *e = (unsigned char *)b.p;
    memcpy(e, "\177ELF\2\1\1", 7);
    wr(e + 16, 2, 2);                                         /* ET_EXEC */
    wr(e + 18, machine ? machine : 62, 2);
    wr(e + 20, 1, 4);
    const char *en = entry_name ? entry_name : "_start";
    Gsym *eg = map_get(&gsyms, intern(en, strlen(en)));
    unsigned long entry = 0;
    if (eg && eg->defined) entry = eg->abs || !eg->sec ? eg->val : eg->sec->addr + eg->val;
    else if (entry_name) lerr("entry point %s is not defined", en);
    wr(e + 24, entry, 8);
    wr(e + 32, 64, 8);                                        /* program headers */
    wr(e + 40, shoff, 8);
    wr(e + 52, 64, 2);
    wr(e + 54, 56, 2);
    wr(e + 56, nph, 2);
    wr(e + 58, 64, 2);
    wr(e + 60, nsh, 2);
    wr(e + 62, nsh - 1, 2);
    for (int k = 0; k < segs.n; k++) {
        Seg *g = segs.v[k];
        unsigned char *ph = e + 64 + 56 * k;
        wr(ph, 1, 4);                                         /* PT_LOAD */
        wr(ph + 4, g->flags, 4);
        wr(ph + 8, g->off, 8);
        wr(ph + 16, g->vaddr, 8);
        wr(ph + 24, g->paddr, 8);
        wr(ph + 32, g->filesz, 8);
        wr(ph + 40, g->memsz, 8);
        wr(ph + 48, g->align, 8);
    }
    if (tls_memsz) {                                          /* PT_TLS: the template each thread copies */
        unsigned char *ph = e + 64 + 56 * segs.n;
        OutSec *first = 0;
        for (int i = 0; i < outs.n && !first; i++) if (((OutSec *)outs.v[i])->flags & SHF_TLS) first = outs.v[i];
        wr(ph, 7, 4);
        wr(ph + 4, 4, 4);
        wr(ph + 8, first->off, 8);
        wr(ph + 16, tls_start, 8);
        wr(ph + 24, first->lma, 8);
        wr(ph + 32, tls_filesz, 8);
        wr(ph + 40, tls_memsz, 8);
        wr(ph + 48, tls_align, 8);
    }
    if (write_file(out_name, b.p, b.len) < 0) lerr("%s: cannot write", out_name);
    free(b.p);
}

/* ---- -r: one relocatable object from several. Sections of the same name
 * are joined end to end; symbols and relocations follow their section. */
typedef struct { const char *name; int type; long flags, align, size; Buf data, rela; Vec in; int shidx, symidx; } RSec;

static void write_relocatable(void)
{
    Vec rs = { 0 };
    for (int o = 0; o < objs.n; o++) {                    /* output sections, in first-seen order */
        InObj *ob = objs.v[o];
        for (int i = 1; i < ob->nsecs; i++) {
            InSec *s = ob->secs[i];
            if (!s) continue;
            RSec *r = 0;
            for (int k = 0; k < rs.n && !r; k++) if (((RSec *)rs.v[k])->name == s->name && ((RSec *)rs.v[k])->type == s->type) r = rs.v[k];
            if (!r) { r = arena(sizeof *r); r->name = s->name; r->type = s->type; r->align = 1; vec_push(&rs, r); }
            r->flags |= s->flags;
            if (s->align > r->align) r->align = s->align;
            r->size = (r->size + s->align - 1) / s->align * s->align;
            s->addr = r->size;                             /* its offset in the joined section */
            s->rule = r;
            if (s->type != SHT_NOBITS) { buf_zero(&r->data, s->addr - r->data.len); buf_add(&r->data, s->data, s->size); }
            r->size += s->size;
            vec_push(&r->in, s);
        }
    }
    /* symbols: null, one per section, the objects' locals, then the globals */
    Buf symb = { 0 }, str = { 0 };
    buf_c(&str, 0);
    put(&symb, 0, 24);
    int nsym = 1;
    for (int k = 0; k < rs.n; k++) {
        RSec *r = rs.v[k];
        r->shidx = k + 1;
        r->symidx = nsym++;
        put(&symb, 0, 4); buf_c(&symb, 3); buf_c(&symb, 0); put(&symb, r->shidx, 2); put(&symb, 0, 8); put(&symb, 0, 8);
    }
    Vec lidx = { 0 };                                     /* per object: its symbols' new indexes */
    for (int o = 0; o < objs.n; o++) {
        InObj *ob = objs.v[o];
        int *map = arena(sizeof(int) * (ob->nsyms + 1));
        vec_push(&lidx, map);
        for (int i = 1; i < ob->nsyms; i++) {
            LSym *ls = &ob->syms[i];
            if (ls->bind != 0) continue;
            if (ls->type == 3) { if (ls->shndx < ob->nsecs && ob->secs[ls->shndx]) map[i] = -1; continue; }   /* a section */
            if (ls->type == 4) continue;                  /* the file name */
            InSec *in = ls->shndx && ls->shndx < 0xFF00 ? ob->secs[ls->shndx] : 0;
            put(&symb, str.len, 4); buf_s(&str, ls->name); buf_c(&str, 0);
            buf_c(&symb, ls->type); buf_c(&symb, 0);
            put(&symb, in ? ((RSec *)in->rule)->shidx : ls->shndx, 2);
            put(&symb, ls->value + (in ? in->addr : 0), 8); put(&symb, ls->size, 8);
            map[i] = nsym++;
        }
    }
    int first_global = nsym;
    for (int i = 0; i < gorder.n; i++) {
        Gsym *g = gorder.v[i];
        if (!g || (!g->defined && !g->referenced)) continue;
        put(&symb, str.len, 4); buf_s(&str, g->name); buf_c(&str, 0);
        int common = g->sec && !strcmp(g->sec->name, "COMMON") && !g->sec->rule;
        buf_c(&symb, (g->weak ? 2 : 1) << 4 | (g->is_func ? 2 : g->tls ? 6 : g->defined ? 1 : 0));
        buf_c(&symb, 0);
        put(&symb, !g->defined ? 0 : common ? 0xFFF2 : g->abs || !g->sec ? 0xFFF1 : ((RSec *)g->sec->rule)->shidx, 2);
        put(&symb, common ? (unsigned long)g->sec->align : g->defined && g->sec && !g->abs ? g->sec->addr + g->val : g->val, 8);
        put(&symb, common ? (unsigned long)g->sec->size : g->size, 8);
        g->addr = nsym++;                                 /* (its index, while writing) */
    }
    /* relocations, moved to the joined sections */
    for (int k = 0; k < rs.n; k++) {
        RSec *r = rs.v[k];
        for (int j = 0; j < r->in.n; j++) {
            InSec *s = r->in.v[j];
            int *map = 0;
            for (int o = 0; o < objs.n; o++) if (objs.v[o] == s->obj) map = lidx.v[o];
            for (long x = 0; x < s->nrela; x++) {
                unsigned char *e = s->rela + x * 24;
                unsigned long off = rd(e, 8), info = rd(e + 8, 8);
                long add = rd(e + 16, 8);
                int idx = info >> 32, type = info & 0xffffffff, ni;
                LSym *ls = &s->obj->syms[idx];
                if (ls->bind != 0 && ls->g) ni = ls->g->addr;
                else if (ls->type == 3 || map[idx] == -1) { InSec *t = s->obj->secs[ls->shndx]; ni = ((RSec *)t->rule)->symidx; add += t->addr + ls->value; }
                else ni = map[idx];
                put(&r->rela, s->addr + off, 8);
                put(&r->rela, (unsigned long)ni << 32 | type, 8);
                put(&r->rela, add, 8);
            }
        }
    }
    /* the file: header, sections, their relocations, symbols, names */
    Buf b = { 0 }, shstr = { 0 }, sh = { 0 };
    buf_zero(&b, 64);
    buf_c(&shstr, 0);
    put(&sh, 0, 64);
    int nrela = 0;
    for (int k = 0; k < rs.n; k++) if (((RSec *)rs.v[k])->rela.len) nrela++;
    int symtab = rs.n + nrela + 1;
    for (int k = 0; k < rs.n; k++) {
        RSec *r = rs.v[k];
        while ((long)b.len % r->align) buf_c(&b, 0);
        long off = b.len;
        if (r->type != SHT_NOBITS) buf_add(&b, r->data.p ? r->data.p : "", r->data.len);
        put(&sh, shstr.len, 4); buf_s(&shstr, r->name); buf_c(&shstr, 0);
        put(&sh, r->type, 4); put(&sh, r->flags, 8); put(&sh, 0, 8); put(&sh, off, 8); put(&sh, r->size, 8);
        put(&sh, 0, 4); put(&sh, 0, 4); put(&sh, r->align, 8); put(&sh, 0, 8);
    }
    for (int k = 0; k < rs.n; k++) {
        RSec *r = rs.v[k];
        if (!r->rela.len) continue;
        while (b.len % 8) buf_c(&b, 0);
        long off = b.len;
        buf_add(&b, r->rela.p, r->rela.len);
        put(&sh, shstr.len, 4); buf_s(&shstr, ".rela"); buf_s(&shstr, r->name); buf_c(&shstr, 0);
        put(&sh, SHT_RELA, 4); put(&sh, 0x40, 8); put(&sh, 0, 8); put(&sh, off, 8); put(&sh, r->rela.len, 8);
        put(&sh, symtab, 4); put(&sh, r->shidx, 4); put(&sh, 8, 8); put(&sh, 24, 8);
    }
    while (b.len % 8) buf_c(&b, 0);
    long symoff = b.len;
    buf_add(&b, symb.p, symb.len);
    long stroff = b.len;
    buf_add(&b, str.p, str.len);
    put(&sh, shstr.len, 4); buf_s(&shstr, ".symtab"); buf_c(&shstr, 0);
    put(&sh, SHT_SYMTAB, 4); put(&sh, 0, 8); put(&sh, 0, 8); put(&sh, symoff, 8); put(&sh, symb.len, 8);
    put(&sh, symtab + 1, 4); put(&sh, first_global, 4); put(&sh, 8, 8); put(&sh, 24, 8);
    put(&sh, shstr.len, 4); buf_s(&shstr, ".strtab"); buf_c(&shstr, 0);
    put(&sh, SHT_STRTAB, 4); put(&sh, 0, 8); put(&sh, 0, 8); put(&sh, stroff, 8); put(&sh, str.len, 8);
    put(&sh, 0, 4); put(&sh, 0, 4); put(&sh, 1, 8); put(&sh, 0, 8);
    long shstroff = b.len;
    put(&sh, shstr.len, 4); buf_s(&shstr, ".shstrtab"); buf_c(&shstr, 0);
    put(&sh, SHT_STRTAB, 4); put(&sh, 0, 8); put(&sh, 0, 8); put(&sh, shstroff, 8); put(&sh, shstr.len, 8);
    put(&sh, 0, 4); put(&sh, 0, 4); put(&sh, 1, 8); put(&sh, 0, 8);
    buf_add(&b, shstr.p, shstr.len);
    while (b.len % 8) buf_c(&b, 0);
    long shoff = b.len;
    buf_add(&b, sh.p, sh.len);
    int nsh = symtab + 3;
    unsigned char *e = (unsigned char *)b.p;
    memcpy(e, "\177ELF\2\1\1", 7);
    wr(e + 16, 1, 2);                                     /* ET_REL */
    wr(e + 18, machine ? machine : 62, 2);
    wr(e + 20, 1, 4);
    wr(e + 40, shoff, 8);
    wr(e + 52, 64, 2);
    wr(e + 58, 64, 2);
    wr(e + 60, nsh, 2);
    wr(e + 62, nsh - 1, 2);
    if (write_file(out_name, b.p, b.len) < 0) lerr("%s: cannot write", out_name);
}

int link_main(int argc, char **argv)
{
    int relocatable = 0;
    Vec inputs = { 0 }, libdirs = { 0 };
    const char *script_file = 0;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) out_name = argv[++i];
        else if (!strcmp(a, "-T") && i + 1 < argc) script_file = argv[++i];
        else if (!strcmp(a, "-e") && i + 1 < argc) entry_name = argv[++i];
        else if (!strcmp(a, "-z") && i + 1 < argc) i++;
        else if (!strncmp(a, "-L", 2)) vec_push(&libdirs, a[2] ? a + 2 : argv[++i]);
        else if (!strncmp(a, "-l", 2)) {
            const char *found = 0;
            for (int k = 0; k < libdirs.n && !found; k++) {
                char *p = fmt("%s/lib%s.a", (char *)libdirs.v[k], a + 2);
                FILE *f = fopen(p, "rb");
                if (f) { fclose(f); found = p; }
            }
            if (!found) lerr("cannot find -l%s", a + 2);
            vec_push(&inputs, (void *)found);
        } else if (!strcmp(a, "-r") || !strcmp(a, "--relocatable")) relocatable = 1;
        else if (a[0] == '-') { /* --no-warn-rwx-segments, -static, -nostdlib...: nothing to do */ }
        else vec_push(&inputs, a);
    }
    /* objects first, then archives as needed */
    Vec members = { 0 };
    for (int i = 0; i < inputs.n; i++) {
        long len;
        unsigned char *img = load(inputs.v[i], &len);
        if (len >= 8 && !memcmp(img, "!<arch>\n", 8)) read_archive(inputs.v[i], img, len, &members);
        else add_object(read_object(inputs.v[i], img, len));
    }
    for (int changed = 1; changed; ) {
        changed = 0;
        for (int i = 0; i < members.n; i++) {
            ArMember *m = members.v[i];
            if (m->loaded) continue;
            if (!m->obj) m->obj = read_object(m->name, m->img, m->len);
            int wanted = 0;
            for (int k = 1; k < m->obj->nsyms && !wanted; k++) {
                LSym *s = &m->obj->syms[k];
                if (s->bind == 0 || s->shndx == 0) continue;
                Gsym *g = map_get(&gsyms, intern(s->name, strlen(s->name)));
                if (g && !g->defined && g->referenced) wanted = 1;
            }
            if (wanted) { add_object(m->obj); m->loaded = 1; changed = 1; }
        }
    }
    if (relocatable) { write_relocatable(); return 0; }
    if (script_file) {
        Buf t = { 0 };
        if (read_file(script_file, &t) < 0) lerr("%s: cannot read", script_file);
        parse_script(t.p);
    } else parse_script(default_script);
    assign_sections();
    layout();
    write_output();
    return 0;
}

/* ---- ar rcs out.a objects: an archive with a symbol index */
int ar_main(int argc, char **argv)
{
    if (argc < 3 || !strchr(argv[1], 'r')) lerr("usage: ar rcs archive.a objects...");
    const char *out = argv[2];
    Buf names = { 0 };
    Vec offs = { 0 };
    Buf body = { 0 };
    Vec files = { 0 }, lens = { 0 };
    for (int i = 3; i < argc; i++) {
        long len;
        unsigned char *img = load(argv[i], &len);
        vec_push(&files, img);
        vec_push(&lens, (void *)len);
    }
    /* the index needs the members' offsets: header size first */
    int nsym = 0;
    for (int i = 0; i < files.n; i++) {
        InObj *o = read_object(argv[i + 3], files.v[i], (long)lens.v[i]);
        for (int k = 1; k < o->nsyms; k++) {
            LSym *s = &o->syms[k];
            if (s->bind == 0 || s->shndx == 0) continue;
            buf_s(&names, s->name);
            buf_c(&names, 0);
            vec_push(&offs, (void *)(long)i);
            nsym++;
        }
    }
    long index_size = 4 + 4L * nsym + names.len;
    long pos = 8 + 60 + index_size + (index_size & 1);
    long *member_off = arena(sizeof(long) * (files.n + 1));
    for (int i = 0; i < files.n; i++) {
        member_off[i] = pos;
        long len = (long)lens.v[i];
        pos += 60 + len + (len & 1);
    }
    buf_s(&body, "!<arch>\n");
    char h[96];
    snprintf(h, sizeof h, "%-16s%-12s%-6s%-6s%-8s%-10ld`\n", "/", "0", "0", "0", "0", index_size);
    buf_add(&body, h, 60);
    for (int k = 3; k >= 0; k--) buf_c(&body, nsym >> (8 * k));
    for (int i = 0; i < nsym; i++) { long o = member_off[(long)offs.v[i]]; for (int k = 3; k >= 0; k--) buf_c(&body, o >> (8 * k)); }
    buf_add(&body, names.p ? names.p : "", names.len);
    if (index_size & 1) buf_c(&body, '\n');
    for (int i = 0; i < files.n; i++) {
        const char *b = base_of(argv[i + 3]);
        char nm[17];
        snprintf(nm, sizeof nm, "%.15s/", b);
        long len = (long)lens.v[i];
        snprintf(h, sizeof h, "%-16s%-12s%-6s%-6s%-8s%-10ld`\n", nm, "0", "0", "0", "644", len);
        buf_add(&body, h, 60);
        buf_add(&body, files.v[i], len);
        if (len & 1) buf_c(&body, '\n');
    }
    if (write_file(out, body.p, body.len) < 0) lerr("%s: cannot write", out);
    return 0;
}

/* ---- objcopy -O binary in out: the loadable bytes, from the lowest load
 * address to the end of the highest (gaps filled with zeros) */
int objcopy_main(int argc, char **argv)
{
    if (argc != 5 || strcmp(argv[1], "-O") || strcmp(argv[2], "binary")) lerr("usage: objcopy -O binary in out");
    long len;
    unsigned char *img = load(argv[3], &len);
    if (len < 64 || memcmp(img, "\177ELF\2\1", 6)) lerr("%s: not an ELF64 file", argv[3]);
    unsigned long phoff = rd(img + 32, 8), shoff = rd(img + 40, 8);
    int phnum = rd(img + 56, 2), shnum = rd(img + 60, 2);
    unsigned long lo = ~0UL, hi = 0;
    /* each allocated section with contents, at its load address */
    typedef struct { unsigned long lma, off, size; } Part;
    Vec parts = { 0 };
    for (int i = 1; i < shnum; i++) {
        unsigned char *h = img + shoff + i * 64;
        if (rd(h + 4, 4) != SHT_PROGBITS || !(rd(h + 8, 8) & SHF_ALLOC)) continue;
        unsigned long addr = rd(h + 16, 8), off = rd(h + 24, 8), size = rd(h + 32, 8);
        if (!size) continue;
        unsigned long lma = addr;
        for (int k = 0; k < phnum; k++) {
            unsigned char *p = img + phoff + k * 56;
            unsigned long va = rd(p + 16, 8), pa = rd(p + 24, 8), msz = rd(p + 40, 8);
            if (rd(p, 4) == 1 && addr >= va && addr < va + msz) { lma = addr - va + pa; break; }
        }
        Part *pt = arena(sizeof *pt);
        pt->lma = lma;
        pt->off = off;
        pt->size = size;
        vec_push(&parts, pt);
        if (lma < lo) lo = lma;
        if (lma + size > hi) hi = lma + size;
    }
    if (!parts.n) lerr("%s: nothing to copy", argv[3]);
    unsigned char *outb = calloc(1, hi - lo + 1);
    for (int i = 0; i < parts.n; i++) {
        Part *pt = parts.v[i];
        memcpy(outb + (pt->lma - lo), img + pt->off, pt->size);
    }
    if (write_file(argv[4], outb, hi - lo) < 0) lerr("%s: cannot write", argv[4]);
    return 0;
}
