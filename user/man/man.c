/*
 * man - The manual: a page of /usr/share/man shown on the terminal.
 *
 *   man NAME            the page (sections 1, 8, 2, 4, 5, 7, in that order)
 *   man SECTION NAME    the page of that section only
 *   man ./FILE          a page from a file (writing one: man(7))
 *   man -k WORD         the pages whose name or one-line summary has WORD
 *
 * Pages are written in a small subset of the man(7) macros (man(7) tells
 * which); this renders them: words filled to the width, bold and underline
 * through the terminal's attributes (ESC [ 1 m, ESC [ 4 m; a terminal that
 * has none ignores them). A page longer than the screen goes through a
 * small pager: space or f a page on, b a page back, Enter or j a line on,
 * k a line back, g and G the top and the end, /WORD searches (n: again),
 * q quits. /usr/share/man/whatis (made when the disk is built) holds every
 * page's names and summary: man -k reads it, and so does a NAME that is
 * not a page's file name (cd's page also tells pwd).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

#define DIR   "/usr/share/man"
#define W     78                       /* the width filled (a VGA line is 80) */
#ifndef ROWS
#define ROWS  24                       /* the screen's height (its last row: the pager's) */
#endif
#define BODY  7                        /* the text's indent, as in a classic manual */

/* ---- What is rendered: lines with their attributes (ESC sequences). */
static char **ln;
static int nln, cap;
static char cur[1024];                 /* the line being filled */
static int clen, cvis;                 /* its bytes; its visible columns */
static char cfont = 'R';               /* the attribute in effect in cur */
static int head, pad;                  /* the last line is a heading; cur ends in a tag's padding */

static void keep(const char *s, int n)
{
    if (nln == cap) { cap = cap ? cap * 2 : 256; ln = realloc(ln, cap * sizeof *ln); }
    char *p = malloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    ln[nln++] = p;
}
static void setfont(char f)
{
    if (f == cfont || clen > (int)sizeof cur - 8) return;
    const char *e = f == 'B' ? "\033[0;1m" : f == 'I' ? "\033[0;4m" : "\033[0m";
    memcpy(cur + clen, e, strlen(e));
    clen += strlen(e);
    cfont = f;
}
static void putc_(char c, char f)
{
    setfont(f);
    if (clen < (int)sizeof cur - 8) { cur[clen++] = c; cvis++; }
}
static void flush(void)                /* the line is done (if anything is in it) */
{
    if (!clen) return;
    setfont('R');
    keep(cur, clen);
    clen = cvis = head = 0;
}
static void blank(void)                /* one empty line between paragraphs, never two */
{
    flush();
    if (nln && ln[nln - 1][0] && !head) keep("", 0);
}

/* ---- Layout. */
static int left = BODY;                /* the margin (.RS moves it) */
static int ind = BODY;                 /* where lines of this paragraph start */
static int nofill, tp;                 /* .nf; tp: the next line is a .TP's tag */
static int rsstk[8], nrs;
static char font = 'R', pfont = 'R';   /* the font of plain text (\f escapes) */

typedef struct { char c, f; } cell_t;  /* a character and its font */
static cell_t seq[2048];
static int nseq;

static void add(char c, char f) { if (nseq < (int)(sizeof seq / sizeof *seq)) seq[nseq++] = (cell_t){ c, f }; }

/* Text with its escapes into seq, starting in font *f. */
static void conv(const char *s, char *f)
{
    for (; *s; s++) {
        if (*s != '\\') { add(*s, *f); continue; }
        switch (*++s) {
        case 0: return;
        case 'f':
            if (!s[1]) return;
            s++;
            { char n = *s == 'P' ? pfont : *s == 'B' ? 'B' : *s == 'I' ? 'I' : 'R'; pfont = *f; *f = n; }
            break;
        case '"': return;                              /* a comment to the end of the line */
        case '&': break;
        case 'e': add('\\', *f); break;
        case ' ': add(1, *f); break;                   /* a space that never breaks a line */
        case '(':                                      /* \(bu \(em \(mu \(dq \(lq \(rq \(aq */
            if (!s[1] || !s[2]) return;
            if (s[1] == 'b' && s[2] == 'u') add('*', *f);
            else if (s[1] == 'e' && s[2] == 'm') { add('-', *f); add('-', *f); }
            else if (s[1] == 'm' && s[2] == 'u') add('x', *f);
            else if (s[2] == 'q' && s[1] != 'a') add('"', *f);
            else add('\'', *f);
            s += 2;
            break;
        default: add(*s, *f);                          /* \- \\ and the rest: the character */
        }
    }
}

/* seq, filled into lines (or as it is, under .nf). */
static void out(void)
{
    if (nofill) {
        for (int i = 0; i < nseq; i++) {
            if (!clen) for (int k = 0; k < ind; k++) putc_(' ', 'R');
            putc_(seq[i].c == 1 ? ' ' : seq[i].c, seq[i].c == ' ' ? 'R' : seq[i].f);
        }
        nseq = 0;
        return;
    }
    for (int i = 0; i < nseq; ) {
        while (i < nseq && (seq[i].c == ' ' || seq[i].c == '\t')) i++;
        int j = i;
        while (j < nseq && seq[j].c != ' ' && seq[j].c != '\t') j++;
        if (j == i) break;
        if (clen && cvis + 1 + (j - i) > W) flush();
        if (!clen) for (int k = 0; k < ind; k++) putc_(' ', 'R');
        else if (!pad) putc_(' ', cfont == seq[i].f ? cfont : 'R');
        pad = 0;
        for (; i < j; i++) putc_(seq[i].c == 1 ? ' ' : seq[i].c, seq[i].f);
    }
    nseq = 0;
}

/* After a .TP's tag: the text goes on beside it if the tag is short, else below. */
static void tag_end(void)
{
    ind = left + BODY;
    if (clen && cvis < ind - 1) { while (cvis < ind) putc_(' ', 'R'); pad = 1; }
    else flush();
}

/* A macro line's arguments ("..." keeps spaces). */
static int args(char *p, char **av, int max)
{
    int n = 0;
    while (n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            av[n++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            av[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = 0;
    }
    return n;
}

static char title[48], next1;          /* "LS(1)"; next1: .B or .I alone: the font of the next line */
static const char *secname(char s)
{
    switch (s) {
    case '1': return "General Commands"; case '2': return "System Calls";
    case '4': return "Servers and Devices"; case '5': return "File Formats";
    case '7': return "Overviews"; case '8': return "System Administration";
    }
    return "SIEOS";
}

static void macro(char *l)
{
    char *av[16], name[4] = { 0 };
    int i = 0;
    while (i < 3 && l[i] && l[i] != ' ' && l[i] != '\t') { name[i] = l[i]; i++; }
    int ac = args(l + i, av, 16);
    char f;
    if (!strcmp(name, "TH")) {
        int k = 0;
        for (const char *s = ac ? av[0] : "?"; *s && k < 40; s++) title[k++] = *s;
        title[k++] = '(';
        title[k++] = ac > 1 ? av[1][0] : '?';
        title[k++] = ')';
        title[k] = 0;
        const char *mid = secname(ac > 1 ? av[1][0] : 0);
        int a = (int)strlen(title), b = (int)strlen(mid), gap = (W - 2 * a - b) / 2;
        for (int c = 0; title[c]; c++) putc_(title[c], 'R');
        for (int c = 0; c < gap; c++) putc_(' ', 'R');
        for (int c = 0; mid[c]; c++) putc_(mid[c], 'R');
        while (cvis < W - a) putc_(' ', 'R');
        for (int c = 0; title[c]; c++) putc_(title[c], 'R');
        flush();
    } else if (!strcmp(name, "SH") || !strcmp(name, "SS")) {
        blank();
        nrs = 0;
        left = BODY;
        ind = name[1] == 'H' ? 0 : 3;
        f = 'B';
        for (int k = 0; k < ac; k++) { if (k) add(' ', 'B'); conv(av[k], &f); }
        out();
        flush();
        head = 1;
        ind = left;
        tp = 0;
    } else if (!strcmp(name, "PP") || !strcmp(name, "LP") || !strcmp(name, "P")) {
        blank();
        ind = left;
    } else if (!strcmp(name, "TP")) {
        blank();
        ind = left;
        tp = 1;
    } else if (!strcmp(name, "IP")) {
        blank();
        ind = left;
        if (ac && av[0][0]) { f = 'R'; conv(av[0], &f); out(); tag_end(); }
        else ind = left + BODY;
    } else if (!strcmp(name, "RS")) {
        flush();
        if (nrs < 8) rsstk[nrs++] = left;
        ind = left += BODY;
    } else if (!strcmp(name, "RE")) {
        flush();
        if (nrs) left = rsstk[--nrs];
        ind = left;
    } else if (!strcmp(name, "nf")) { flush(); nofill = 1; }
    else if (!strcmp(name, "fi")) { flush(); nofill = 0; }
    else if (!strcmp(name, "br")) flush();
    else if (!strcmp(name, "sp")) { flush(); keep("", 0); }
    else if ((name[0] == 'B' || name[0] == 'I' || name[0] == 'R') && (!name[1] || ((name[1] == 'B' || name[1] == 'I' || name[1] == 'R') && !name[2]))) {
        if (!ac) { next1 = name[0]; return; }           /* .B alone: the next line in bold */
        for (int k = 0; k < ac; k++) {                  /* .B, .I: words in that font; .BR...: alternating */
            f = name[1] ? name[k & 1] : name[0];
            if (k && !name[1]) add(' ', f);
            conv(av[k], &f);
        }
        if (nofill) { out(); flush(); } else out();
    } else return;                                      /* (other macros: ignored) */
    if (tp && strcmp(name, "TP")) { tp = 0; tag_end(); }
}

static void render(char *text)
{
    for (char *l = text, *e; *l; l = e) {
        for (e = l; *e && *e != '\n'; e++) ;
        if (*e) *e++ = 0;
        if ((l[0] == '.' || l[0] == '\'') && l[1] == '\\' && l[2] == '"') continue;
        if (l[0] == '.') { macro(l + 1); continue; }
        if (!nofill && (!*l || *l == ' ')) { if (!*l) { flush(); keep("", 0); continue; } flush(); }
        if (next1) { char f = next1; next1 = 0; conv(l, &f); }
        else conv(l, &font);
        out();
        if (nofill) flush();
        if (tp) { tp = 0; tag_end(); }
    }
    flush();
}

/* ---- Finding a page. */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int has(const char *s, int n, const char *w)        /* w in s[0..n), any case */
{
    int k = (int)strlen(w);
    for (int i = 0; i + k <= n; i++) {
        int j = 0;
        while (j < k && lower(s[i + j]) == lower(w[j])) j++;
        if (j == k) return 1;
    }
    return 0;
}

/* whatis: one line per page, "SECTION<tab>FILE<tab>NAME,NAME...<tab>SUMMARY". */
static char *whatis;
static int fields(char *l, char **f)
{
    int n = 0;
    f[n++] = l;
    for (; *l && n < 4; l++) if (*l == '\t') { *l = 0; f[n++] = l + 1; }
    return n;
}

static int apropos(const char *w)
{
    int found = 0;
    for (char *l = whatis, *e; l && *l; l = e) {
        for (e = l; *e && *e != '\n'; e++) ;
        if (*e) *e++ = 0;
        char *f[4];
        if (fields(l, f) < 4 || !(has(f[2], strlen(f[2]), w) || has(f[3], strlen(f[3]), w))) continue;
        for (char *c = f[2]; *c; c++) if (*c == ',') { *c = 0; printf("%s, ", f[2]); f[2] = c + 1; }
        printf("%s (%s) - %s\n", f[2], f[0], f[3]);
        found = 1;
    }
    if (!found) printf("%s: nothing appropriate\n", w);
    return !found;
}

/* The page's file for NAME (in section sec, 0: any) -> 1, path in p. */
static int find(const char *name, char sec, char *p)
{
    static const char order[] = "182457";
    siefs_stat_t st;
    for (const char *s = order; *s; s++) {
        if (sec && *s != sec) continue;
        size_t n = strlcpy(p, DIR "/", FS_PATH);
        p[n++] = *s; p[n++] = '/';
        n += strlcpy(p + n, name, FS_PATH - n - 3);
        p[n++] = '.'; p[n++] = *s; p[n] = 0;
        if (!fs_stat(p, &st, 0)) return 1;
    }
    for (const char *s = order; *s; s++) {              /* another name of a page (its NAME line) */
        if (sec && *s != sec) continue;
        for (char *l = whatis; l && *l; ) {
            char *e = l, *f[4];
            while (*e && *e != '\n') e++;
            char save = *e;
            *e = 0;
            char line[512];
            strlcpy(line, l, sizeof line);
            *e = save;
            l = *e ? e + 1 : e;
            if (fields(line, f) < 4 || f[0][0] != *s) continue;
            for (char *c = f[2]; *c; ) {
                char *d = c;
                while (*d && *d != ',') d++;
                int last = !*d;
                *d = 0;
                if (!strcmp(c, name)) {
                    size_t n = strlcpy(p, DIR "/", FS_PATH);
                    strlcpy(p + n, f[1], FS_PATH - n);
                    return 1;
                }
                if (last) break;
                c = d + 1;
            }
        }
    }
    return 0;
}

/* ---- The pager. */
static int vislen(const char *s, char *plain, int max)     /* the line without its ESC sequences */
{
    int n = 0;
    for (; *s; s++) {
        if (*s == 033) { while (*s && *s != 'm') s++; if (!*s) break; continue; }
        if (n < max - 1) plain[n++] = *s;
    }
    plain[n] = 0;
    return n;
}
static void show(int top, const char *msg)
{
    static char b[ROWS * 160];
    int n = 0;
    memcpy(b, "\033[H\033[J", 6); n = 6;
    for (int i = top; i < top + ROWS - 1 && i < nln; i++) {
        int k = (int)strlen(ln[i]);
        if (n + k + 2 > (int)sizeof b) { con_write(b, n); n = 0; }
        memcpy(b + n, ln[i], k); n += k;
        b[n++] = '\n';
    }
    con_write(b, n);
    int pct = nln > ROWS - 1 ? (top + ROWS - 1 >= nln ? 100 : (top + ROWS - 1) * 100 / nln) : 100;
    if (msg) printf("\033[1m%s\033[0m", msg);
    else printf("\033[1m%s %d%%%s\033[0m (space b / n q) ", title, pct, pct == 100 ? " (end)" : "");
}
static int search(const char *w, int from)
{
    char plain[1024];
    for (int i = from; i < nln; i++) if (has(plain, vislen(ln[i], plain, sizeof plain), w)) return i;
    return -1;
}
static char kb[64];                    /* raw keys come as the terminal sends them, several at once */
static int kn, ki;
static int key(void)
{
    if (ki == kn) { long n = con_read(kb, sizeof kb); if (n <= 0) return -1; kn = (int)n; ki = 0; }
    return (uint8_t)kb[ki++];
}
static void pager(void)
{
    int top = 0, max = nln - (ROWS - 1), c;
    char pat[64] = "";
    const char *msg = 0;
    con_echo(CON_RAW);
    for (;;) {
        if (top > max) top = max;
        if (top < 0) top = 0;
        if (ki == kn) { show(top, msg); msg = 0; }      /* (keys already typed: those first) */
        if ((c = key()) < 0) break;
        if (c == 033 && key() == '[') { c = key(); c = c == 'A' ? 'k' : c == 'B' ? 'j' : c == 'H' ? 'g' : c == 'F' ? 'G' : 0; }
        if (c == 'q' || c == 'Q' || c == 3 || c == 4) break;
        switch (c) {
        case ' ': case 'f': top += ROWS - 1; break;
        case 'b': top -= ROWS - 1; break;
        case '\r': case '\n': case 'j': top++; break;
        case 'k': top--; break;
        case 'g': top = 0; break;
        case 'G': top = max; break;
        case '/': {                                     /* the word, typed on the last row */
            int m = 0;
            printf("\r\033[K/");
            while ((c = key()) >= 0 && c != '\r' && c != '\n' && c != 033 && c != 3) {
                if (c == 0x7F || c == '\b') { if (m) { m--; printf("\b \b"); } continue; }
                if (m < (int)sizeof pat - 1 && c >= ' ') { pat[m++] = (char)c; printf("%c", c); }
            }
            pat[m] = 0;
            if (!m) break;
            top--;                                      /* (the first match from the top line on) */
        }
        /* fall through */
        case 'n':
            if (pat[0]) {
                int r = search(pat, top + 1);
                if (r < 0) msg = "not found (q, or any key)";
                else top = r;
            }
            break;
        }
    }
    con_echo(1);
    printf("\r\033[K");
}

int main(int ac, char **av)
{
    char path[FS_PATH], sec = 0;
    whatis = file_get(DIR "/whatis", 0);
    if (ac == 3 && !strcmp(av[1], "-k")) return apropos(av[2]);
    if (ac == 3 && av[1][0] >= '1' && av[1][0] <= '9' && !av[1][1]) { sec = av[1][0]; av++; ac--; }
    if (ac != 2 || av[1][0] == '-') { printf("usage: man [SECTION] NAME | man -k WORD\n"); return 2; }
    if (strchr(av[1], '/')) strlcpy(path, av[1], sizeof path);
    else if (!find(av[1], sec, path)) {
        if (sec) printf("no entry for %s in section %c of the manual\n", av[1], sec);
        else printf("no manual entry for %s (man -k %s searches)\n", av[1], av[1]);
        return 1;
    }
    char *text = file_get(path, 0);
    if (!text) { printf("%s: cannot read it\n", path); return 1; }
    render(text);
    if (nln <= ROWS - 1) { for (int i = 0; i < nln; i++) printf("%s\n", ln[i]); return 0; }
    pager();
    return 0;
}
