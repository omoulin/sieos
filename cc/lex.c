/*
 * lex.c - Source text to tokens (C11 "preprocessing tokens").
 *
 * Before splitting, a file's text gets its line splices (backslash +
 * newline) removed; the removed newlines are put back at the end of the
 * logical line, so the line numbers of the following lines stay right.
 * Comments become white space. Each token remembers whether it starts a
 * line and whether white space precedes it: the preprocessor needs both.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "sicc.h"

int nerrors;
static int nfiles;

const char *kw_names[K_COUNT] = {
    [K_VOID] = "void", [K_CHAR] = "char", [K_SHORT] = "short", [K_INT] = "int",
    [K_LONG] = "long", [K_FLOAT] = "float", [K_DOUBLE] = "double", [K_SIGNED] = "signed",
    [K_UNSIGNED] = "unsigned", [K_BOOL] = "_Bool", [K_STRUCT] = "struct", [K_UNION] = "union",
    [K_ENUM] = "enum", [K_TYPEDEF] = "typedef", [K_EXTERN] = "extern", [K_STATIC] = "static",
    [K_AUTO] = "auto", [K_REGISTER] = "register", [K_INLINE] = "inline", [K_CONST] = "const",
    [K_VOLATILE] = "volatile", [K_RESTRICT] = "restrict", [K_SIZEOF] = "sizeof",
    [K_ALIGNOF] = "_Alignof", [K_ALIGNAS] = "_Alignas", [K_STATIC_ASSERT] = "_Static_assert",
    [K_RETURN] = "return", [K_IF] = "if", [K_ELSE] = "else", [K_WHILE] = "while", [K_DO] = "do",
    [K_FOR] = "for", [K_SWITCH] = "switch", [K_CASE] = "case", [K_DEFAULT] = "default",
    [K_BREAK] = "break", [K_CONTINUE] = "continue", [K_GOTO] = "goto",
    [K_NORETURN] = "_Noreturn", [K_ATTRIBUTE] = "__attribute__", [K_ASM] = "asm",
    [K_TYPEOF] = "typeof", [K_EXTENSION] = "__extension__", [K_VA_LIST] = "__builtin_va_list",
    [K_GENERIC] = "_Generic", [K_THREAD_LOCAL] = "_Thread_local", [K_ATOMIC] = "_Atomic",
    [K_COMPLEX] = "_Complex", [K_INT128] = "__int128",
};

/* Other spellings of the same keywords (GNU's double-underscore forms...). */
static const struct { const char *s; int kw; } kw_alias[] = {
    { "__const", K_CONST }, { "__const__", K_CONST }, { "__volatile", K_VOLATILE },
    { "__volatile__", K_VOLATILE }, { "__inline", K_INLINE }, { "__inline__", K_INLINE },
    { "__restrict", K_RESTRICT }, { "__restrict__", K_RESTRICT }, { "__signed", K_SIGNED },
    { "__signed__", K_SIGNED }, { "__asm", K_ASM }, { "__asm__", K_ASM },
    { "__typeof", K_TYPEOF }, { "__typeof__", K_TYPEOF }, { "__alignof", K_ALIGNOF },
    { "__alignof__", K_ALIGNOF }, { "alignof", K_ALIGNOF }, { "__attribute", K_ATTRIBUTE },
    { "static_assert", K_STATIC_ASSERT }, { "__thread", K_THREAD_LOCAL },
};

static Map kw_map;

static void kw_init(void)
{
    for (int i = 1; i < K_COUNT; i++)
        map_put(&kw_map, intern(kw_names[i], strlen(kw_names[i])), (void *)(long)i);
    for (unsigned i = 0; i < sizeof kw_alias / sizeof kw_alias[0]; i++)
        map_put(&kw_map, intern(kw_alias[i].s, strlen(kw_alias[i].s)), (void *)(long)kw_alias[i].kw);
}

/* ---- Errors: "file:line:col: error: message", the line, and a caret. */
static void report(Token *t, const char *kind, const char *f, va_list ap)
{
    int col = 1;
    const char *ls = 0, *le = 0;
    if (t && t->file && t->loc >= t->file->text && t->loc <= t->file->text + strlen(t->file->text)) {
        ls = t->loc;
        while (ls > t->file->text && ls[-1] != '\n') ls--;
        col = t->loc - ls + 1;
        le = t->loc;
        while (*le && *le != '\n') le++;
    }
    if (t && t->file) fprintf(stderr, "%s:%d:%d: %s: ", t->file->name, t->line, col, kind);
    else fprintf(stderr, "sicc: %s: ", kind);
    vfprintf(stderr, f, ap);
    fputc('\n', stderr);
    if (ls) {
        fprintf(stderr, "  %.*s\n  %*s^\n", (int)(le - ls), ls, col - 1, "");
    }
    for (Token *o = t ? t->origin : 0; o; o = o->origin)
        if (o->file) fprintf(stderr, "%s:%d: note: in the expansion of this macro use\n", o->file->name, o->line);
}

void error_at(Token *t, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    report(t, "error", f, ap);
    va_end(ap);
    exit(1);
}

void warn_at(Token *t, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    report(t, "warning", f, ap);
    va_end(ap);
}

int tok_is(Token *t, int p) { return t->kind == TK_PUNCT && t->punct == p; }
int tok_id(Token *t, const char *s) { return t->kind == TK_IDENT && (int)strlen(s) == t->len && !strncmp(t->loc, s, t->len); }

Token *copy_token(Token *t)
{
    Token *n = arena(sizeof *n);
    *n = *t;
    n->next = 0;
    return n;
}

Token *new_eof(Token *at)
{
    Token *t = copy_token(at);
    t->kind = TK_EOF;
    t->len = 0;
    t->bol = 1;
    return t;
}

/* A file's text, with line splices removed (see the top). */
File *new_file(const char *name, const char *text)
{
    long n = strlen(text);
    char *o = arena(n + 2);
    long j = 0;
    int pending = 0;                         /* newlines taken out of the current line */
    for (long i = 0; i < n; ) {
        if (text[i] == '\\' && text[i + 1] == '\n') { i += 2; pending++; continue; }
        if (text[i] == '\\' && text[i + 1] == '\r' && text[i + 2] == '\n') { i += 3; pending++; continue; }
        if (text[i] == '\r' && text[i + 1] == '\n') { i++; continue; }
        if (text[i] == '\n') { for (; pending; pending--) o[j++] = '\n'; }
        o[j++] = text[i++];
    }
    for (; pending; pending--) o[j++] = '\n';
    if (j == 0 || o[j - 1] != '\n') o[j++] = '\n';
    o[j] = 0;
    File *f = arena(sizeof *f);
    f->name = name;
    f->text = o;
    f->id = ++nfiles;
    if (!kw_map.cap) kw_init();
    return f;
}

static int is_ident1(int c) { c = (unsigned char)c; return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c == '$' && !opt_asm_mode) || c >= 0x80; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_ident2(int c) { return is_ident1(c) || is_digit(c); }

/* a universal character name, \uXXXX or \UXXXXXXXX: its length (0: none) */
static int ucn_len(const char *p)
{
    if (p[0] != '\\' || (p[1] != 'u' && p[1] != 'U')) return 0;
    int n = p[1] == 'u' ? 4 : 8;
    for (int i = 0; i < n; i++) {
        char c = p[2 + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return n + 2;
}

/* an identifier with universal character names: their UTF-8 (the same name as the characters themselves) */
static const char *ucn_name(const char *p, int len)
{
    char buf[1024];
    int n = 0;
    for (const char *e = p + len; p < e && n < 1000; ) {
        int k = ucn_len(p);
        if (!k) { buf[n++] = *p++; continue; }
        unsigned c = (unsigned)strtoull((char[]){ p[2], p[3], p[4], p[5], k > 6 ? p[6] : 0, k > 6 ? p[7] : 0, k > 6 ? p[8] : 0, k > 6 ? p[9] : 0, 0 }, 0, 16);
        p += k;
        if (c < 0x80) buf[n++] = c;
        else if (c < 0x800) { buf[n++] = 0xC0 | c >> 6; buf[n++] = 0x80 | (c & 63); }
        else if (c < 0x10000) { buf[n++] = 0xE0 | c >> 12; buf[n++] = 0x80 | (c >> 6 & 63); buf[n++] = 0x80 | (c & 63); }
        else { buf[n++] = 0xF0 | c >> 18; buf[n++] = 0x80 | (c >> 12 & 63); buf[n++] = 0x80 | (c >> 6 & 63); buf[n++] = 0x80 | (c & 63); }
    }
    return intern(buf, n);
}

/* Longest punctuators first. */
static const struct { const char *s; int p; } puncts[] = {
    { "...", P_ELLIPSIS }, { "<<=", P_SHLA }, { ">>=", P_SHRA }, { "->", P_ARROW }, { "++", P_INC },
    { "--", P_DEC }, { "<<", P_SHL }, { ">>", P_SHR }, { "<=", P_LE }, { ">=", P_GE }, { "==", P_EQ },
    { "!=", P_NE }, { "&&", P_AND }, { "||", P_OR }, { "*=", P_MULA }, { "/=", P_DIVA },
    { "%=", P_MODA }, { "+=", P_ADDA }, { "-=", P_SUBA }, { "&=", P_ANDA }, { "^=", P_XORA },
    { "|=", P_ORA }, { "##", P_HASHHASH },
};

Token *tokenize(File *f)
{
    Token head = { 0 }, *cur = &head;
    const char *p = f->text;
    int line = 1, bol = 1, space = 0;
    while (*p) {
        if (p[0] == '/' && p[1] == '/') { while (*p != '\n') p++; space = 1; continue; }
        if (p[0] == '/' && p[1] == '*') {
            const char *q = p + 2;
            for (; *q && !(q[0] == '*' && q[1] == '/'); q++) if (*q == '\n') line++;
            if (!*q) { Token t = { .file = f, .line = line, .loc = p }; error_at(&t, "unterminated comment"); }
            p = q + 2;
            space = 1;
            continue;
        }
        if (*p == '\n') { p++; line++; bol = 1; space = 0; continue; }
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\f' || *p == '\v') { p++; space = 1; continue; }

        Token *t = arena(sizeof *t);
        t->loc = p;
        t->file = f;
        t->line = line;
        t->bol = bol;
        t->space = space;
        bol = space = 0;
        cur = cur->next = t;

        if (is_digit(*p) || (*p == '.' && is_digit(p[1]))) {          /* a pp-number */
            const char *q = p++;
            for (;;) {
                if (p[0] && p[1] && strchr("eEpP", p[0]) && (p[1] == '+' || p[1] == '-')) p += 2;
                else if (is_ident2(*p) || *p == '.') p++;
                else break;
            }
            t->kind = TK_NUM;
            t->len = p - q;
            continue;
        }
        const char *q = p;                                             /* prefixes of literals */
        if (q[0] == 'u' && q[1] == '8') q += 2;
        else if (*q == 'L' || *q == 'u' || *q == 'U') q++;
        if ((*q == '"' || *q == '\'') && !(opt_asm_mode && *q == '\'')) {
            char quote = *q;
            for (q++; *q != quote; q++) {
                if (*q == '\n' || !*q) error_at(t, "unterminated %s", quote == '"' ? "string" : "character constant");
                if (*q == '\\') q++;
            }
            t->kind = quote == '"' ? TK_STR : TK_CHAR;
            t->len = q + 1 - p;
            p = q + 1;
            continue;
        }
        if (is_ident1(*p) || ucn_len(p)) {
            int ucn = 0;
            for (;;) {
                if (is_ident2(*p)) p++;
                else if (ucn_len(p)) { p += ucn_len(p); ucn = 1; }
                else break;
            }
            t->kind = TK_IDENT;
            t->len = p - t->loc;
            t->name = ucn ? ucn_name(t->loc, t->len) : intern(t->loc, t->len);
            t->kw = (int)(long)map_get(&kw_map, t->name);
            continue;
        }
        t->kind = TK_PUNCT;
        t->punct = (unsigned char)*p;
        t->len = 1;
        for (unsigned i = 0; i < sizeof puncts / sizeof puncts[0]; i++) {
            int n = strlen(puncts[i].s);
            if (!strncmp(p, puncts[i].s, n)) { t->punct = puncts[i].p; t->len = n; break; }
        }
        p += t->len;
    }
    Token *e = arena(sizeof *e);
    e->kind = TK_EOF;
    e->file = f;
    e->line = line;
    e->loc = p;
    e->bol = 1;
    cur->next = e;
    return head.next;
}
