/*
 * make - Remake files from the rules of a makefile: POSIX make, with GNU
 * make's common additions (docs/make.md).
 *
 *   make [-f FILE] [-C DIR] [-n] [-k] [-s] [-B] [-i] [-r] [-j1] [VAR=VALUE...] [TARGET...]
 *
 * The makefile (makefile or Makefile, or -f's) is read whole: variables,
 * explicit, pattern and suffix rules, includes. Then each goal is made
 * depth first: its prerequisites, then its recipe if it is missing or older
 * than one of them (the file server's modification times). Each recipe
 * line runs alone, as "sh -c LINE" in make's folder, and must succeed.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

/* ---- Messages. While a makefile is read (or a recipe from it expanded),
 * they name its file and line, as GNU make's do. */
static const char *mf_name;
static int mf_line;

struct fb { char b[512]; size_t n; };
static void fput(void *ctx, char c) { struct fb *f = ctx; if (f->n < sizeof f->b - 1) f->b[f->n++] = c; }
static void say(const char *pre, const char *post, const char *fmt, va_list ap)
{
    struct fb f = { .n = 0 };
    vformat(fput, &f, fmt, ap);
    f.b[f.n] = 0;
    if (mf_name) printf("%s:%d: %s%s%s\n", mf_name, mf_line, pre, f.b, post);
    else printf("make: %s%s%s\n", pre, f.b, post);
}
static __attribute__((noreturn, format(printf, 1, 2))) void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    say("*** ", ".  Stop.", fmt, ap);
    va_end(ap);
    sys_exit(2);
}
static __attribute__((format(printf, 1, 2))) void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    say("warning: ", "", fmt, ap);
    va_end(ap);
}

/* ---- Strings that grow (0-terminated once anything is added) */
typedef struct { char *s; size_t n, cap; } str_t;

static void sadd(str_t *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        while (cap < b->n + n + 1) cap *= 2;
        if (!(b->s = realloc(b->s, cap))) die("out of memory");
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->s[b->n += n] = 0;
}
static void sputs(str_t *b, const char *s) { sadd(b, s, strlen(s)); }
static char *copy(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (!d) die("out of memory");
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
static char *sdone(str_t *b) { return b->s ? b->s : copy("", 0); }
static int blank(char c) { return c == ' ' || c == '\t'; }
static int prefix(const char *s, const char *p, size_t n) { for (size_t i = 0; i < n; i++) if (s[i] != p[i]) return 0; return 1; }

/* The next word of *p (blanks separate them), or 0 at the end; *n its length. */
static const char *word(const char **p, size_t *n)
{
    const char *s = *p, *e;
    while (blank(*s)) s++;
    if (!*s) return 0;
    for (e = s; *e && !blank(*e); e++) ;
    *p = e;
    *n = (size_t)(e - s);
    return s;
}
/* A word of a list being built: one space before all but the first. */
static void wadd(str_t *o, const char *w, size_t n, int *first)
{
    if (!*first) sadd(o, " ", 1);
    *first = 0;
    sadd(o, w, n);
}
static int wcmp(const char *a, size_t an, const char *b, size_t bn)
{
    int c = memcmp(a, b, an < bn ? an : bn);
    return c ? c : (an > bn) - (an < bn);
}
/* The words of t, sorted, each once. */
static void sortwords(const char *t, str_t *o, int *first)
{
    const char *p = t, *w;
    size_t k, n = 0;
    while (word(&p, &k)) n++;
    if (!n) return;
    const char **v = malloc(n * sizeof *v);
    size_t *l = malloc(n * sizeof *l);
    if (!v || !l) die("out of memory");
    n = 0;
    for (p = t; (w = word(&p, &k)); n++) {             /* insertion sort */
        size_t i = n;
        for (; i && wcmp(v[i - 1], l[i - 1], w, k) > 0; i--) { v[i] = v[i - 1]; l[i] = l[i - 1]; }
        v[i] = w; l[i] = k;
    }
    for (size_t i = 0; i < n; i++)
        if (!i || wcmp(v[i - 1], l[i - 1], v[i], l[i])) wadd(o, v[i], l[i], first);
    free(v);
    free(l);
}

/* w against a pattern with one % (one without % must be equal): 1 if it
 * matches, with the stem in *st, *sn. */
static int pmatch(const char *p, size_t pn, const char *w, size_t wn, const char **st, size_t *sn)
{
    size_t a = 0;
    while (a < pn && p[a] != '%') a++;
    if (a == pn) { *st = w; *sn = 0; return pn == wn && !memcmp(p, w, wn); }
    size_t z = pn - a - 1;
    if (wn < a + z || memcmp(w, p, a) || memcmp(w + wn - z, p + a + 1, z)) return 0;
    *st = w + a;
    *sn = wn - a - z;
    return 1;
}
/* A pattern with its first % replaced by a stem. */
static void psub(str_t *o, const char *p, size_t pn, const char *st, size_t sn)
{
    for (size_t i = 0; i < pn; i++)
        if (p[i] == '%') { sadd(o, p, i); sadd(o, st, sn); sadd(o, p + i + 1, pn - i - 1); return; }
    sadd(o, p, pn);
}
static void patsubst(const char *p, const char *r, const char *t, str_t *o)
{
    const char *w, *st;
    size_t k, sn, pn = strlen(p), rn = strlen(r);
    int first = 1;
    while ((w = word(&t, &k))) {
        if (!first) sadd(o, " ", 1);
        first = 0;
        if (pmatch(p, pn, w, k, &st, &sn)) psub(o, r, rn, st, sn);
        else sadd(o, w, k);
    }
}

/* ---- Files matching * and ? (in the last part of a path), sorted. */
static char dirbuf[4096];
static int fnm(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (; *s; s++) if (fnm(p, s)) return 1;
            return 0;
        }
        if (!*s || (*p != '?' && *p != *s)) return 0;
    }
    return !*s;
}
static int wild(const char *s, size_t n) { for (size_t i = 0; i < n; i++) if (s[i] == '*' || s[i] == '?') return 1; return 0; }
static void glob(const char *pat, str_t *o, int *first)
{
    size_t n = strlen(pat), d = n;
    while (d && pat[d - 1] != '/') d--;
    const char *base = pat + d;
    siefs_stat_t st;
    if (!wild(base, n - d)) { if (!fs_stat(pat, &st, 0)) wadd(o, pat, n, first); return; }
    char dir[FS_PATH];
    if (d >= sizeof dir || wild(pat, d)) return;          /* (no wildcards in folder names) */
    if (d) { memcpy(dir, pat, d); dir[d > 1 ? d - 1 : 1] = 0; } else strlcpy(dir, ".", sizeof dir);
    long h = fs_open(dir, FS_RDONLY | FS_DIRECTORY, 0), r;
    if (h < 0) return;
    str_t all = { 0 };
    uint64_t cur = 0, ino;
    int type;
    char name[256];
    while ((r = fs_readdir(h, &cur, dirbuf, sizeof dirbuf)) > 0)
        for (const char *p = dirbuf; fs_dirent(&p, dirbuf + r, &ino, &type, name); )
            if ((name[0] != '.' || base[0] == '.') && fnm(base, name)) {
                if (all.n) sadd(&all, " ", 1);
                sadd(&all, pat, d);
                sputs(&all, name);
            }
    fs_close(h);
    if (all.s) sortwords(all.s, o, first);
    free(all.s);
}

/* ---- Variables. Recursive ones (=) keep their text and are expanded
 * when used; simple ones (:=) keep the expanded value. */
enum { O_DEFAULT, O_FILE, O_CMDLINE, O_AUTO };        /* where a value comes from */
typedef struct var { struct var *next; char *name, *value; int simple, origin, busy; } var_t;
static var_t *vars[256];

static unsigned hash(const char *s, size_t n) { unsigned h = 5381; while (n--) h = h * 33 + (unsigned char)*s++; return h; }
static var_t **vslot(const char *name, size_t n)
{
    var_t **v = &vars[hash(name, n) % 256];
    while (*v && (strlen((*v)->name) != n || memcmp((*v)->name, name, n))) v = &(*v)->next;
    return v;
}
static var_t *vlook(const char *name, size_t n) { return *vslot(name, n); }
static var_t *vput(const char *name, size_t n, char *value, int simple, int origin)   /* takes value */
{
    var_t **s = vslot(name, n), *v = *s;
    if (!v) {
        if (!(v = malloc(sizeof *v))) die("out of memory");
        v->next = 0; v->name = copy(name, n); v->value = 0; v->busy = 0;
        *s = v;
    }
    free(v->value);
    v->value = value; v->simple = simple; v->origin = origin;
    return v;
}
/* $(foreach) and $(call) set a variable for a while, then put it back. */
typedef struct { var_t *v; char *value; int simple, origin, had; } saved_t;
static void vpush(saved_t *s, const char *name, char *value)
{
    var_t *v = vlook(name, strlen(name));
    if ((s->had = v != 0)) { s->value = v->value; s->simple = v->simple; s->origin = v->origin; v->value = 0; }
    s->v = vput(name, strlen(name), value, 1, O_AUTO);
}
static void vpop(saved_t *s)
{
    var_t *v = s->v;
    free(v->value);
    if (s->had) { v->value = s->value; v->simple = s->simple; v->origin = s->origin; return; }
    *vslot(v->name, strlen(v->name)) = v->next;
    free(v->name);
    free(v);
}

/* ---- Expansion: $(VAR), ${VAR}, $X, $$, $(VAR:a=b), $(function args) */
static char *autov[7];                     /* $@ $< $^ $? $* $+ $|, while a recipe runs */
static const char autoc[] = "@<^?*+|";
static void expand(const char *s, size_t n, str_t *o);
static char *xp(const char *s, size_t n) { str_t b = { 0 }; expand(s, n, &b); return sdone(&b); }

/* The closing parenthesis (or brace) of a reference opened just before s. */
static const char *closing(const char *s, const char *e, char open)
{
    char close = open == '(' ? ')' : '}';
    for (int d = 1; s < e; s++)
        if (*s == open) d++;
        else if (*s == close && !--d) return s;
    die("unterminated variable reference");
}
/* The first of the characters of `set` in s[0..n) outside references, or n. */
static size_t scan(const char *s, size_t n, const char *set)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '$' && i + 1 < n && (s[i + 1] == '(' || s[i + 1] == '{')) { i = (size_t)(closing(s + i + 2, s + n, s[i + 1]) - s); continue; }
        if (s[i] == '$') { i++; continue; }
        if (s[i] && strchr(set, s[i])) return i;
    }
    return n;
}

static void value(const char *name, size_t n, str_t *o)
{
    const char *a = n && n <= 2 && *name ? strchr(autoc, name[0]) : 0;
    if (a && (n == 1 || name[1] == 'D' || name[1] == 'F')) {
        const char *s = autov[a - autoc] ? autov[a - autoc] : "", *w;
        size_t k;
        int first = 1;
        if (n == 1) { sputs(o, s); return; }
        while ((w = word(&s, &k))) {                   /* $(@D) $(@F): folder, file name */
            size_t d = k;
            while (d && w[d - 1] != '/') d--;
            if (name[1] == 'F') wadd(o, w + d, k - d, &first);
            else if (d) wadd(o, w, d > 1 ? d - 1 : 1, &first);
            else wadd(o, ".", 1, &first);
        }
        return;
    }
    var_t *v = vlook(name, n);
    if (!v) return;
    if (v->simple) { sputs(o, v->value); return; }
    if (v->busy) die("Recursive variable '%s' references itself (eventually)", v->name);
    v->busy = 1;
    expand(v->value, strlen(v->value), o);
    v->busy = 0;
}

enum { F_SUBST, F_PATSUBST, F_STRIP, F_FINDSTRING, F_FILTER, F_FILTER_OUT, F_SORT, F_WORD, F_WORDS,
       F_FIRSTWORD, F_LASTWORD, F_DIR, F_NOTDIR, F_SUFFIX, F_BASENAME, F_ADDSUFFIX, F_ADDPREFIX,
       F_WILDCARD, F_FOREACH, F_IF, F_CALL, F_INFO, F_WARNING, F_ERROR, F_SHELL };
static const struct { const char *name; int args; } fns[] = {
    { "subst", 3 }, { "patsubst", 3 }, { "strip", 1 }, { "findstring", 2 }, { "filter", 2 },
    { "filter-out", 2 }, { "sort", 1 }, { "word", 2 }, { "words", 1 }, { "firstword", 1 },
    { "lastword", 1 }, { "dir", 1 }, { "notdir", 1 }, { "suffix", 1 }, { "basename", 1 },
    { "addsuffix", 2 }, { "addprefix", 2 }, { "wildcard", 1 }, { "foreach", 3 }, { "if", 3 },
    { "call", 10 }, { "info", 1 }, { "warning", 1 }, { "error", 1 }, { "shell", 1 },
};
#define NFN (int)(sizeof fns / sizeof fns[0])

/* dir, notdir, suffix or basename of one file name. */
static void part(int f, const char *w, size_t k, str_t *o, int *first)
{
    size_t d = k, dot = k;
    while (d && w[d - 1] != '/') d--;                  /* w[0..d): the folder, with its '/' */
    while (dot > d && w[dot - 1] != '.') dot--;        /* w[dot - 1]: the name's last '.' (dot == d: none) */
    if (f == F_DIR) wadd(o, d ? w : "./", d ? d : 2, first);
    else if (f == F_NOTDIR) wadd(o, w + d, k - d, first);
    else if (f == F_SUFFIX) { if (dot > d) wadd(o, w + dot - 1, k - dot + 1, first); }
    else wadd(o, w, dot > d ? dot - 1 : k, first);
}

/* $(name args): 0 if name is no function. */
static int func(const char *name, size_t nn, const char *a, size_t n, str_t *o)
{
    int f = 0;
    while (f < NFN && (strlen(fns[f].name) != nn || memcmp(fns[f].name, name, nn))) f++;
    if (f == NFN) return 0;
    const char *arg[10];
    size_t len[10];
    char *x[10];
    int na = 0, first = 1;
    while (n && blank(*a)) a++, n--;
    for (;;) {                                         /* the arguments: split at commas */
        size_t c = na + 1 < fns[f].args ? scan(a, n, ",") : n;
        arg[na] = a; len[na++] = c;
        if (c >= n) break;
        a += c + 1; n -= c + 1;
    }
    for (int i = 0; i < 10; i++)                       /* foreach and if expand some only later */
        x[i] = i >= na ? 0 : (f == F_FOREACH && i == 2) || (f == F_IF && i) ? 0 : xp(arg[i], len[i]);
#define X(i) (x[i] ? x[i] : "")
    const char *t = X(fns[f].args > 3 ? 0 : fns[f].args - 1), *w, *p;
    size_t k, l0 = strlen(X(0));
    switch (f) {
    case F_SUBST:
        for (const char *s = t; *s; )
            if (l0 && prefix(s, x[0], l0)) { sputs(o, X(1)); s += l0; }
            else sadd(o, s++, 1);
        break;
    case F_PATSUBST: patsubst(X(0), X(1), t, o); break;
    case F_STRIP: while ((w = word(&t, &k))) wadd(o, w, k, &first); break;
    case F_FINDSTRING:
        for (const char *s = t; *s; s++) if (prefix(s, X(0), l0)) { sputs(o, X(0)); break; }
        break;
    case F_FILTER: case F_FILTER_OUT:
        while ((w = word(&t, &k))) {
            int in = 0;
            const char *q, *st;
            size_t m, sn;
            for (p = X(0); !in && (q = word(&p, &m)); ) in = pmatch(q, m, w, k, &st, &sn);
            if (in == (f == F_FILTER)) wadd(o, w, k, &first);
        }
        break;
    case F_SORT: sortwords(t, o, &first); break;
    case F_WORD: {
        long i = 0, want = -1;
        p = X(0);
        if ((w = word(&p, &k))) { char *num = copy(w, k); want = strnum(num); free(num); }
        if (want < 1) die("first argument to 'word' function must be greater than 0");
        while ((w = word(&t, &k))) if (++i == want) { sadd(o, w, k); break; }
        break;
    }
    case F_WORDS: {
        char num[24], *e = num + sizeof num;
        size_t i = 0;
        while (word(&t, &k)) i++;
        do *--e = (char)('0' + i % 10); while (i /= 10);
        sadd(o, e, (size_t)(num + sizeof num - e));
        break;
    }
    case F_FIRSTWORD: if ((w = word(&t, &k))) sadd(o, w, k); break;
    case F_LASTWORD: {
        const char *last = 0;
        size_t lk = 0;
        while ((w = word(&t, &k))) { last = w; lk = k; }
        if (last) sadd(o, last, lk);
        break;
    }
    case F_DIR: case F_NOTDIR: case F_SUFFIX: case F_BASENAME:
        while ((w = word(&t, &k))) part(f, w, k, o, &first);
        break;
    case F_ADDSUFFIX: case F_ADDPREFIX:
        while ((w = word(&t, &k))) {
            if (!first) sadd(o, " ", 1);
            first = 0;
            if (f == F_ADDPREFIX) sputs(o, X(0));
            sadd(o, w, k);
            if (f == F_ADDSUFFIX) sputs(o, X(0));
        }
        break;
    case F_WILDCARD:
        for (p = X(0); (w = word(&p, &k)); ) { char *g = copy(w, k); glob(g, o, &first); free(g); }
        break;
    case F_FOREACH: {
        const char *vn = X(0);
        if ((w = word(&vn, &k))) {
            char *name = copy(w, k);
            for (p = X(1); (w = word(&p, &k)); ) {
                saved_t s;
                vpush(&s, name, copy(w, k));
                char *r = na > 2 ? xp(arg[2], len[2]) : copy("", 0);
                wadd(o, r, strlen(r), &first);
                free(r);
                vpop(&s);
            }
            free(name);
        }
        break;
    }
    case F_IF:
        p = X(0);
        if (word(&p, &k)) { if (na > 1) expand(arg[1], len[1], o); }
        else if (na > 2) expand(arg[2], len[2], o);
        break;
    case F_CALL: {                                     /* $(call VAR,a,b): VAR with $(1)=a $(2)=b */
        saved_t s[10];
        char num[2] = "0";
        p = X(0);
        if (!(w = word(&p, &k))) break;
        for (int i = 0; i < na; i++) { num[0] = (char)('0' + i); vpush(&s[i], num, i ? copy(X(i), strlen(X(i))) : copy(w, k)); }
        value(w, k, o);
        for (int i = na - 1; i >= 0; i--) vpop(&s[i]);
        break;
    }
    case F_INFO: printf("%s\n", X(0)); break;
    case F_WARNING: if (mf_name) printf("%s:%d: %s\n", mf_name, mf_line, X(0)); else printf("make: %s\n", X(0)); break;
    case F_ERROR: die("%s", X(0));
    case F_SHELL: die("$(shell ...) is not supported on SIEOS (a program's output cannot be read yet)");
    }
#undef X
    for (int i = 0; i < 10; i++) free(x[i]);
    return 1;
}

/* The inside of $(...): a function call, or a variable, maybe with a
 * substitution ($(SRC:.c=.o), $(SRC:%.c=%.o)). */
static void ref(const char *a, size_t n, str_t *o)
{
    size_t k = 0;
    while (k < n && !blank(a[k])) k++;
    if (k < n && func(a, k, a + k + 1, n - k - 1, o)) return;
    size_t c = scan(a, n, ":"), q = c < n ? c + 1 + scan(a + c + 1, n - c - 1, "=") : n;
    if (q >= n) c = n;                                 /* (no '=': a name with a ':') */
    char *name = xp(a, c);
    str_t v = { 0 };
    value(name, strlen(name), &v);
    if (c < n) {
        char *from = xp(a + c + 1, q - c - 1), *to = xp(a + q + 1, n - q - 1);
        if (!strchr(from, '%')) {                      /* a suffix: as %suffix */
            str_t f = { 0 }, t = { 0 };
            sadd(&f, "%", 1); sputs(&f, from);
            sadd(&t, "%", 1); sputs(&t, to);
            free(from); free(to);
            from = f.s; to = t.s;
        }
        patsubst(from, to, v.s ? v.s : "", o);
        free(from); free(to);
    } else if (v.n) sadd(o, v.s, v.n);
    free(v.s);
    free(name);
}

static void expand(const char *s, size_t n, str_t *o)
{
    const char *e = s + n;
    while (s < e) {
        const char *d = s;
        while (d < e && *d != '$') d++;
        sadd(o, s, (size_t)(d - s));
        if (d + 1 >= e) break;                         /* (a '$' at the end is dropped) */
        char c = d[1];
        if (c == '(' || c == '{') {
            const char *z = closing(d + 2, e, c);
            ref(d + 2, (size_t)(z - d - 2), o);
            s = z + 1;
        } else {
            if (c == '$') sadd(o, "$", 1);
            else ref(d + 1, 1, o);
            s = d + 2;
        }
    }
}

/* NAME op VALUE: op '=' (expanded when used), ':' (now), '?' (if not
 * set yet), '+' (appended). The command line's values win. */
static void assign(const char *name, const char *val, int op, int origin)
{
    size_t n = strlen(name);
    var_t *v = vlook(name, n);
    if (v && v->origin == O_CMDLINE && origin != O_CMDLINE) return;
    if (op == '?' && v) return;
    if (op == '+' && v) {
        str_t b = { 0 };
        sputs(&b, v->value);
        if (b.n && *val) sadd(&b, " ", 1);
        if (v->simple) expand(val, strlen(val), &b);
        else sputs(&b, val);
        vput(name, n, sdone(&b), v->simple, origin);
        return;
    }
    vput(name, n, op == ':' ? xp(val, strlen(val)) : copy(val, strlen(val)), op == ':', origin);
}

/* ---- Files, rules and recipes */
typedef struct { char **line; int n, cap; const char *mf; int at; } recipe_t;   /* at: its rule's line */
typedef struct dep { struct dep *next; struct file *f; int order; } dep_t;      /* order: after '|' */
typedef struct file {
    struct file *next;                 /* in the table */
    char *name, *stem;                 /* stem: $*, from a pattern rule */
    dep_t *deps;
    recipe_t *rc;                      /* its recipe (its rule's, or an implicit rule's), or 0 */
    int64_t mtime;                     /* 0: missing */
    char target, phony, quiet, noerr;  /* target: named in a rule */
    char stated, state, remade, failed;   /* state 1: being made, 2: done */
} file_t;
static file_t *files[512];

static file_t *fget(const char *name, size_t n)
{
    file_t **p = &files[hash(name, n) % 512];
    for (; *p; p = &(*p)->next) if (strlen((*p)->name) == n && !memcmp((*p)->name, name, n)) return *p;
    file_t *f = malloc(sizeof *f);
    if (!f) die("out of memory");
    memset(f, 0, sizeof *f);
    f->name = copy(name, n);
    return *p = f;
}
static void adddep(file_t *f, const char *name, size_t n, int order)
{
    dep_t **p = &f->deps, *d = malloc(sizeof *d);
    if (!d) die("out of memory");
    while (*p) p = &(*p)->next;
    d->next = 0; d->f = fget(name, n); d->order = order;
    *p = d;
}
static int64_t mtime(file_t *f)
{
    siefs_stat_t st;
    if (f->phony) return 0;
    if (!f->stated) { f->mtime = fs_stat(f->name, &st, 0) ? 0 : st.mtime > 0 ? st.mtime : 1; f->stated = 1; }
    return f->mtime;
}
static void addline(recipe_t *r, const char *s)
{
    if (r->n == r->cap && !(r->line = realloc(r->line, (r->cap = r->cap ? r->cap * 2 : 4) * sizeof *r->line))) die("out of memory");
    r->line[r->n++] = copy(s, strlen(s));
}

/* Pattern rules (%.o: %.c), suffix rules turned into them, and the built-in ones. */
typedef struct prule { struct prule *next; char *target, **dep; int ndep, builtin; recipe_t *rc; } prule_t;
static prule_t *prules, **prlast = &prules;
static int reading_builtin;
static char *sufs[32];                 /* .SUFFIXES */
static int nsuf;

static void addprule(const char *target, const char *deps, recipe_t *rc)
{
    prule_t *r = malloc(sizeof *r);
    const char *p = deps, *w;
    size_t k;
    int n = 0;
    while (word(&p, &k)) n++;
    if (!r || !(r->dep = malloc((n + 1) * sizeof *r->dep))) die("out of memory");
    r->ndep = 0;
    for (p = deps; (w = word(&p, &k)); ) if (k != 1 || *w != '|') r->dep[r->ndep++] = copy(w, k);
    r->target = copy(target, strlen(target)); r->rc = rc; r->builtin = reading_builtin; r->next = 0;
    *prlast = r;
    prlast = &r->next;
}

/* A pattern rule that can make `name`: its prerequisites exist, are
 * targets, or can be made in turn (a chain of up to 3 rules). */
static prule_t *rule_for(const char *name, int depth, const char **st, size_t *sn)
{
    size_t n = strlen(name);
    for (int bi = 0; bi < 2; bi++)                     /* the makefile's rules first */
        for (prule_t *r = prules; r; r = r->next) {
            if (r->builtin != bi || !r->rc->n || !pmatch(r->target, strlen(r->target), name, n, st, sn)) continue;
            if (depth && !strcmp(r->target, "%")) continue;    /* "%: ...": not for a file in a chain */
            int ok = 1;
            for (int i = 0; i < r->ndep && ok; i++) {
                str_t b = { 0 };
                const char *s2;
                size_t n2;
                psub(&b, r->dep[i], strlen(r->dep[i]), *st, *sn);
                file_t *d = fget(b.s, b.n);
                ok = d->target || mtime(d) || (depth < 2 && rule_for(d->name, depth + 1, &s2, &n2));
                free(b.s);
            }
            if (ok) return r;
        }
    return 0;
}
/* Give f the recipe of an implicit rule; its prerequisites come first. */
static void implicit(file_t *f)
{
    const char *st;
    size_t sn;
    prule_t *r = rule_for(f->name, 0, &st, &sn);
    if (!r) return;
    f->rc = r->rc;
    f->stem = copy(st, sn);
    dep_t *old = f->deps;
    f->deps = 0;
    for (int i = 0; i < r->ndep; i++) {
        str_t b = { 0 };
        psub(&b, r->dep[i], strlen(r->dep[i]), f->stem, sn);
        adddep(f, b.s, b.n, 0);
        free(b.s);
    }
    dep_t **p = &f->deps;
    while (*p) p = &(*p)->next;
    *p = old;
}

/* ---- Reading makefiles */
static file_t *goal;                   /* the first target: the default goal */
static int silent_all, ignore_all;     /* .SILENT: and .IGNORE: without prerequisites */
/* The rule being read: its targets get its recipe when it ends. */
static recipe_t *rule_rc;
static file_t **rule_f;
static int rule_nf, rule_cap;

static void rule_end(void)
{
    if (rule_rc && rule_rc->n)
        for (int i = 0; i < rule_nf; i++) {
            file_t *f = rule_f[i];
            if (f->rc && f->rc != rule_rc) warn("overriding recipe for target '%s'", f->name);
            f->rc = rule_rc;
        }
    rule_rc = 0;
    rule_nf = 0;
}

/* A suffix rule's target (.c.o, .c) -> the pattern rule's target and prerequisite. */
static int suffix_rule(const char *t, str_t *tp, str_t *dp)
{
    for (int i = 0; i < nsuf; i++) {
        size_t a = strlen(sufs[i]);
        if (!prefix(t, sufs[i], a)) continue;
        for (int j = -1; j < nsuf; j++)
            if (j < 0 ? !t[a] : !strcmp(t + a, sufs[j])) {
                sadd(tp, "%", 1); if (j >= 0) sputs(tp, sufs[j]);
                sadd(dp, "%", 1); sputs(dp, sufs[i]);
                return 1;
            }
    }
    return 0;
}

/* targets: prerequisites [; recipe], or targets: pattern: prerequisite patterns
 * (a static pattern rule). s[i] is the ':'. */
static void rule(char *s, size_t i)
{
    char *tg = xp(s, i), *p = s + i + 1, *tpat = 0, *deps;
    if (*p == ':') p++;                                /* (a "::" rule is read as ":") */
    size_t n = strlen(p), semi = scan(p, n, ";"), c = scan(p, semi, ":=");
    if (c < semi && p[c] == '=') die("target-specific variables are not supported");
    if (c < semi) {
        const char *q = tpat = xp(p, c), *w;
        size_t k;
        if ((w = word(&q, &k))) { memmove(tpat, w, k); tpat[k] = 0; }
        deps = xp(p + c + 1, semi - c - 1);
    } else deps = xp(p, semi);
    rule_rc = malloc(sizeof *rule_rc);
    if (!rule_rc) die("out of memory");
    memset(rule_rc, 0, sizeof *rule_rc);
    rule_rc->mf = mf_name; rule_rc->at = mf_line;
    if (semi < n) {
        const char *r = p + semi + 1;
        while (blank(*r)) r++;
        if (*r) addline(rule_rc, r);
    }
    const char *tq = tg, *t, *q, *w;
    size_t tk, k;
    while ((t = word(&tq, &tk))) {
        char *name = copy(t, tk);
        int special = name[0] == '.';
        for (int j = 1; name[j] && special; j++) special = (name[j] >= 'A' && name[j] <= 'Z') || name[j] == '_';
        str_t tp = { 0 }, dp = { 0 };
        q = deps;
        if (special) {
            int any = 0;
            for (; (w = word(&q, &k)); any = 1) {
                file_t *d = fget(w, k);
                if (!strcmp(name, ".PHONY")) d->phony = 1;
                else if (!strcmp(name, ".SILENT")) d->quiet = 1;
                else if (!strcmp(name, ".IGNORE")) d->noerr = 1;
                else if (!strcmp(name, ".SUFFIXES") && nsuf < 32) sufs[nsuf++] = copy(w, k);
            }
            if (!any && !strcmp(name, ".SUFFIXES")) nsuf = 0;
            if (!any && !strcmp(name, ".SILENT")) silent_all = 1;
            if (!any && !strcmp(name, ".IGNORE")) ignore_all = 1;
        } else if (!tpat && strchr(name, '%')) addprule(name, deps, rule_rc);
        else if (!tpat && name[0] == '.' && !word(&q, &k) && suffix_rule(name, &tp, &dp)) addprule(tp.s, dp.s, rule_rc);
        else {
            file_t *f = fget(t, tk);
            const char *st = 0;
            size_t sn = 0;
            f->target = 1;
            if (rule_nf == rule_cap && !(rule_f = realloc(rule_f, (rule_cap = rule_cap ? rule_cap * 2 : 8) * sizeof *rule_f))) die("out of memory");
            rule_f[rule_nf++] = f;
            if (tpat && !pmatch(tpat, strlen(tpat), t, tk, &st, &sn)) warn("target '%s' doesn't match the target pattern", name);
            else if (tpat) { free(f->stem); f->stem = copy(st, sn); }
            int order = 0;
            for (q = deps; (w = word(&q, &k)); ) {
                if (k == 1 && *w == '|') { order = 1; continue; }
                str_t b = { 0 };
                if (tpat) psub(&b, w, k, st, sn);
                else sadd(&b, w, k);
                if (!tpat && wild(w, k)) {             /* *.c: the files there are (or the word itself) */
                    str_t g = { 0 };
                    int first = 1;
                    glob(b.s, &g, &first);
                    if (g.n) { free(b.s); b = g; } else free(g.s);
                }
                const char *bq = b.s, *bw;
                size_t bk;
                while ((bw = word(&bq, &bk))) adddep(f, bw, bk, order);
                free(b.s);
            }
            if (!goal && (name[0] != '.' || strchr(name, '/'))) goal = f;
        }
        free(tp.s); free(dp.s); free(name);
    }
    free(tg); free(tpat); free(deps);
}

static void readmf(const char *path, int must);

/* One line of a makefile that is not a recipe line. */
static void line(char *s)
{
    char *o = s, *p = s;
    for (; *p && *p != '#'; p++) {                     /* '#' starts a comment; "\#" is a '#' */
        if (*p == '\\' && p[1] == '#') p++;
        *o++ = *p;
    }
    while (o > s && blank(o[-1])) o--;
    *o = 0;
    while (blank(*s)) s++;
    if (!*s) return;                                   /* (blank lines do not end a recipe) */
    rule_end();
    size_t k = 0;
    while (s[k] && !blank(s[k])) k++;
    char *rest = s + k;
    while (blank(*rest)) rest++;
    if ((k == 7 && prefix(s, "include", 7)) || (k == 8 && (prefix(s, "-include", 8) || prefix(s, "sinclude", 8)))) {
        char *x = xp(rest, strlen(rest));
        const char *q = x, *w;
        while ((w = word(&q, &k))) {
            char *f = copy(w, k);
            if (wild(w, k)) {
                str_t g = { 0 };
                int first = 1;
                glob(f, &g, &first);
                const char *gq = g.s ? g.s : "", *gw;
                size_t gk;
                while ((gw = word(&gq, &gk))) { char *one = copy(gw, gk); readmf(one, 0); free(one); }
                free(g.s);
            } else readmf(f, s[0] == 'i');
            free(f);
        }
        free(x);
        return;
    }
    if (k == 8 && prefix(s, "unexport", 8)) return;   /* (no environment on SIEOS) */
    if (k == 6 && prefix(s, "export", 6)) { if (!*rest) return; s = rest; }
    static const char *unsupported[] = { "ifeq", "ifneq", "ifdef", "ifndef", "else", "endif", "define", "endef",
                                         "override", "undefine", "vpath", 0 };
    for (int i = 0; unsupported[i]; i++)
        if (strlen(unsupported[i]) == k && prefix(s, unsupported[i], k)) die("'%s' is not supported by SIEOS's make", unsupported[i]);
    size_t n = strlen(s), i = scan(s, n, ":=");
    if (i == n) {
        if (s == rest) return;                         /* "export NAME": nothing to do */
        die("missing separator");
    }
    if (s[i] == '=' || s[i + 1] == '=' || (s[i + 1] == ':' && s[i + 2] == '=')) {
        int op = '=';
        size_t ne = i;
        if (s[i] == ':') { op = ':'; i += s[i + 1] == ':' ? 2 : 1; }
        else if (i && strchr("+?!", s[i - 1])) { op = s[i - 1]; ne = i - 1; }
        if (op == '!') die("'!=' is not supported on SIEOS (a program's output cannot be read yet)");
        while (ne && blank(s[ne - 1])) ne--;
        char *name = xp(s, ne), *v = s + i + 1;
        while (blank(*v)) v++;
        assign(name, v, op, O_FILE);
        free(name);
        return;
    }
    rule(s, i);
}

/* Read makefile text: lines joined at "\" + newline; recipe lines start
 * with a tab, after a rule. */
static void parse(const char *t, const char *name)
{
    const char *oname = mf_name;
    int oline = mf_line, ln = 0;
    str_t l = { 0 };
    rule_end();
    mf_name = name;
    for (const char *p = t; *p; ) {
        int at = ln + 1, tab = *p == '\t';
        l.n = 0;
        for (;;) {
            const char *e = p;
            while (*e && *e != '\n') e++;
            ln++;
            size_t k = (size_t)(e - p), b = 0;
            while (b < k && p[k - 1 - b] == '\\') b++;
            int more = b % 2 && *e;
            sadd(&l, p, more ? k - 1 : k);
            p = *e ? e + 1 : e;
            if (!more) break;
            while (blank(*p)) p++;
            sadd(&l, " ", 1);
        }
        mf_line = at;
        if (tab && rule_rc) {
            const char *c = l.s + 1;
            while (blank(*c)) c++;
            if (*c) addline(rule_rc, c);
        } else line(l.s);
    }
    rule_end();
    free(l.s);
    mf_name = oname;
    mf_line = oline;
}

static void readmf(const char *path, int must)
{
    char *t = file_get(path, 0);
    if (!t) {
        if (must) die("%s: No such file or directory", path);
        return;
    }
    parse(t, copy(path, strlen(path)));                 /* (the name stays: recipes keep it) */
    free(t);
}

/* ---- Making */
static int dry, keep, silent, always, ignore;          /* -n -k -s -B -i */
static int ran;                                        /* recipe lines run (or shown, with -n) */

/* One recipe line, as "sh -c LINE" -> its exit status. A program's
 * arguments and the folder it starts in fit in 256 bytes and 31 words. */
static long shell(char *c)
{
    char args[256];
    const char *p = c;
    size_t n = strlen(c), cw = strlen(fs_getcwd()), k, words = 0;
    for (char *t = c; *t; t++) if (*t == '\t') *t = ' ';
    while (word(&p, &k)) words++;
    if (6 + n + 2 + cw + 1 > sizeof args || words > 29) {
        printf("make: this command is too long for SIEOS (at most 29 words, and 256 bytes with the folder)\n");
        return 1;
    }
    memcpy(args, "sh -c ", 6);
    memcpy(args + 6, c, n + 1);
    return run("/bin/sh", args);
}

static int recipe(file_t *f)
{
    recipe_t *rc = f->rc;
    str_t v[7];
    memset(v, 0, sizeof v);
    int64_t t = mtime(f);
    sputs(&v[0], f->name);
    for (dep_t *d = f->deps; d; d = d->next) {         /* the automatic variables */
        const char *n = d->f->name;
        size_t k = strlen(n);
        int seen = 0, first;
        if (d->order) { first = !v[6].n; wadd(&v[6], n, k, &first); continue; }
        if (!v[1].n) sadd(&v[1], n, k);
        first = !v[5].n; wadd(&v[5], n, k, &first);
        for (dep_t *e = f->deps; e != d && !seen; e = e->next) seen = e->f == d->f && !e->order;
        if (seen) continue;
        first = !v[2].n; wadd(&v[2], n, k, &first);
        if (!t || d->f->remade || mtime(d->f) > t) { first = !v[3].n; wadd(&v[3], n, k, &first); }
    }
    if (f->stem) sputs(&v[4], f->stem);
    else
        for (int i = 0; i < nsuf; i++) {                /* $* of an explicit rule: the name less a known suffix */
            size_t a = strlen(f->name), b = strlen(sufs[i]);
            if (a > b && !strcmp(f->name + a - b, sufs[i])) { sadd(&v[4], f->name, a - b); break; }
        }
    for (int i = 0; i < 7; i++) autov[i] = sdone(&v[i]);
    const char *omf = mf_name;
    int oline = mf_line, bad = 0;
    mf_name = rc->mf; mf_line = rc->at;
    for (int i = 0; i < rc->n && !bad; i++) {
        const char *raw = rc->line[i];
        char *x = xp(raw, strlen(raw)), *c = x;
        int quiet = silent || silent_all || f->quiet, noerr = ignore || ignore_all || f->noerr, force = 0;
        for (;; c++)                                   /* @: not shown; -: errors ignored; +: run even with -n */
            if (*c == '@') quiet = 1;
            else if (*c == '-') noerr = 1;
            else if (*c == '+') force = 1;
            else if (!blank(*c)) break;
        for (const char *m = raw; *m && !force; m++) force = prefix(m, "$(MAKE)", 7) || prefix(m, "${MAKE}", 7);
        if (*c) {
            ran++;
            if (!quiet || dry) printf("%s\n", c);
            long r = dry && !force ? 0 : shell(c);
            if (r < 0 && !EXIT_SIGNO(r)) { printf("make: /bin/sh: cannot start it (error %ld)\n", r); r = 127; }
            if (r && noerr) printf("make: [%s] Error %ld (ignored)\n", f->name, r);
            else if (EXIT_SIGNO(r)) { printf("make: *** [%s] %s\n", f->name, EXIT_SIGNO(r) == SIGINT ? "Interrupt" : "Killed"); bad = 1; }
            else if (r) { printf("make: *** [%s] Error %ld\n", f->name, r); bad = 1; }
        }
        free(x);
    }
    mf_name = omf; mf_line = oline;
    for (int i = 0; i < 7; i++) { free(autov[i]); autov[i] = 0; }
    return bad;
}

/* Make f: its prerequisites first, then its recipe if it is missing or
 * older than one of them -> 1 if it failed. */
static int make(file_t *f, file_t *up)
{
    if (f->state == 2) return f->failed;
    if (f->state == 1) { printf("make: Circular %s <- %s dependency dropped.\n", up->name, f->name); return 0; }
    f->state = 1;
    if (!f->rc && !f->phony) implicit(f);
    int64_t t = mtime(f);
    int need = !t || always, bad = 0;
    for (dep_t *d = f->deps; d && (!bad || keep); d = d->next) {
        if (make(d->f, f)) { bad = 1; continue; }
        if (!d->order && (d->f->remade || mtime(d->f) > t)) need = 1;
    }
    if (!bad && !f->rc && !t && !f->target && !f->phony) {
        if (up) printf("make: *** No rule to make target '%s', needed by '%s'.%s\n", f->name, up->name, keep ? "" : "  Stop.");
        else printf("make: *** No rule to make target '%s'.%s\n", f->name, keep ? "" : "  Stop.");
        bad = 1;
    }
    if (!bad && need && f->rc) bad = recipe(f);
    f->remade = !bad && (need || f->phony);
    f->failed = (char)bad;
    f->state = 2;
    f->stated = 0;                                     /* (its time again, if its recipe ran) */
    return bad;
}

/* ---- Built in: CC and the rules for C files (not with -r) */
static const char builtin_vars[] = "CC = cc\nSHELL = /bin/sh\n";
static const char builtin_rules[] =
    ".SUFFIXES: .out .a .o .c .s .S .h\n"
    "%.o: %.c\n\t$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@\n"
    "%.o: %.S\n\t$(CC) $(ASFLAGS) $(CPPFLAGS) -c $< -o $@\n"
    "%.o: %.s\n\t$(CC) $(ASFLAGS) -c $< -o $@\n"
    "%: %.c\n\t$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@\n"
    "%: %.o\n\t$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@\n";

static int usage(void)
{
    printf("usage: make [-f file] [-C dir] [-n] [-k] [-s] [-B] [-i] [-r] [-j1] [VAR=value...] [target...]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *mfs[8];
    char *goals[16];
    int nmf = 0, ngoal = 0, nobuiltin = 0, moved = 0;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i], *eq;
        if (a[0] == '-' && a[1]) {
            for (char *o = a + 1; *o; o++) {
                if (*o == 'n') dry = 1;
                else if (*o == 'k') keep = 1;
                else if (*o == 's') silent = 1;
                else if (*o == 'B') always = 1;
                else if (*o == 'i') ignore = 1;
                else if (*o == 'r') nobuiltin = 1;
                else if (*o == 'f' || *o == 'C' || *o == 'j') {
                    char *v = o[1] ? o + 1 : 0;
                    if (!v && i + 1 < argc && (*o != 'j' || strnum(argv[i + 1]) >= 0)) v = argv[++i];
                    if (*o == 'f') { if (!v) return usage(); if (nmf < 8) mfs[nmf++] = v; }
                    else if (*o == 'C') {
                        if (!v) return usage();
                        if (fs_chdir(v)) die("%s: No such file or directory", v);
                        moved = 1;
                    } else if (v && strnum(v) > 1) printf("make: -j%s: SIEOS's make runs one job at a time\n", v);
                    break;
                } else return usage();
            }
        } else if ((eq = strchr(a, '='))) {             /* VAR=value: wins over the makefile's */
            int op = eq > a && strchr(":+?", eq[-1]) ? eq[-1] : '=';
            *(op == '=' ? eq : eq - 1) = 0;
            assign(a, eq + 1, op, O_CMDLINE);
        } else if (ngoal < 16) goals[ngoal++] = a;
    }
    if (moved && !silent) printf("make: Entering directory '%s'\n", fs_getcwd());
    str_t m = { 0 };                                   /* $(MAKE): this make, with its options */
    sputs(&m, "make");
    if (dry) sputs(&m, " -n");
    if (keep) sputs(&m, " -k");
    if (silent) sputs(&m, " -s");
    if (always) sputs(&m, " -B");
    if (ignore) sputs(&m, " -i");
    if (nobuiltin) sputs(&m, " -r");
    vput("MAKE", 4, m.s, 1, O_DEFAULT);
    vput("CURDIR", 6, copy(fs_getcwd(), strlen(fs_getcwd())), 1, O_DEFAULT);
    str_t g = { 0 };
    for (int i = 0; i < ngoal; i++) { int first = !g.n; wadd(&g, goals[i], strlen(goals[i]), &first); }
    vput("MAKECMDGOALS", 12, sdone(&g), 1, O_DEFAULT);
    parse(builtin_vars, 0);
    reading_builtin = 1;
    if (!nobuiltin) parse(builtin_rules, 0);
    reading_builtin = 0;
    siefs_stat_t st;
    if (!nmf) {
        if (!fs_stat("makefile", &st, 0)) mfs[nmf++] = "makefile";
        else if (!fs_stat("Makefile", &st, 0)) mfs[nmf++] = "Makefile";
        else if (!ngoal) die("No targets specified and no makefile found");
    }
    for (int i = 0; i < nmf; i++) readmf(mfs[i], 1);
    var_t *dg = vlook(".DEFAULT_GOAL", 13);
    if (dg) {
        char *x = xp(dg->value, strlen(dg->value));
        const char *q = x, *w;
        size_t k;
        if ((w = word(&q, &k))) goal = fget(w, k);
        free(x);
    }
    if (!ngoal) {
        if (!goal) die("No targets");
        goals[ngoal++] = goal->name;
    }
    int bad = 0;
    for (int i = 0; i < ngoal && (!bad || keep); i++) {
        file_t *f = fget(goals[i], strlen(goals[i]));
        int before = ran;
        if (make(f, 0)) {
            bad = 1;
            if (keep) printf("make: Target '%s' not remade because of errors.\n", f->name);
        } else if (ran == before)
            printf(f->rc && !f->phony ? "make: '%s' is up to date.\n" : "make: Nothing to be done for '%s'.\n", f->name);
    }
    if (moved && !silent) printf("make: Leaving directory '%s'\n", fs_getcwd());
    return bad ? 2 : 0;
}
