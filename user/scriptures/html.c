/*
 * html.c - Chapter HTML into styled paragraphs; contents pages into a tree (html.h).
 *
 * A small tag scanner (no DOM): it keeps a stack of open paragraphs and of
 * inline styles, and turns text into runs carrying the styles in effect.
 */
#include "html.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the scanner ------------------------------------------------------------ */

typedef struct { char name[16]; char *attr[16][2]; int nattr, close, selfclose; } tag_t;

static const char *attr(const tag_t *t, const char *k)
{
    for (int i = 0; i < t->nattr; i++)
        if (!strcmp(t->attr[i][0], k)) return t->attr[i][1];
    return NULL;
}
static int has_class(const tag_t *t, const char *cls)
{
    const char *c = attr(t, "class");
    size_t n = strlen(cls);
    while (c && *c) {
        while (*c == ' ') c++;
        const char *e = c;
        while (*e && *e != ' ') e++;
        if ((size_t)(e - c) == n && !strncmp(c, cls, n)) return 1;
        c = e;
    }
    return 0;
}
static void tag_free(tag_t *t)
{
    for (int i = 0; i < t->nattr; i++) { free(t->attr[i][0]); free(t->attr[i][1]); }
    t->nattr = 0;
}

static const struct { const char *name; int cp; } entities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xa0},
    {"mdash", 0x2014}, {"ndash", 0x2013}, {"rsquo", 0x2019}, {"lsquo", 0x2018}, {"rdquo", 0x201d},
    {"ldquo", 0x201c}, {"hellip", 0x2026}, {"copy", 0xa9}, {"para", 0xb6}, {"sect", 0xa7},
    {"middot", 0xb7}, {"thinsp", 0x2009}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"shy", 0xad},
};

/* Text with its character references decoded. */
static void decode(buf_t *b, const char *s, size_t n)
{
    const char *e = s + n;
    while (s < e) {
        if (*s != '&') { buf_putc(b, *s++); continue; }
        const char *semi = memchr(s, ';', MIN(12, e - s));
        int cp = -1;
        if (semi) {
            if (s[1] == '#') cp = s[2] == 'x' || s[2] == 'X' ? (int)strtol(s + 3, NULL, 16) : atoi(s + 2);
            else
                for (int i = 0; i < COUNT(entities); i++)
                    if ((size_t)(semi - s - 1) == strlen(entities[i].name) && !strncmp(s + 1, entities[i].name, semi - s - 1))
                        cp = entities[i].cp;
        }
        if (cp <= 0) { buf_putc(b, *s++); continue; }
        char o[4];
        buf_add(b, o, utf8_enc(o, cp));
        s = semi + 1;
    }
}

/* Reads a tag at *p ('<'); returns 0 for a comment or a declaration (skipped). */
static int read_tag(const char **pp, tag_t *t)
{
    const char *p = *pp + 1;
    memset(t, 0, sizeof *t);
    if (!strncmp(p, "!--", 3)) {
        const char *e = strstr(p, "-->");
        *pp = e ? e + 3 : p + strlen(p);
        return 0;
    }
    if (*p == '!' || *p == '?') {
        const char *e = strchr(p, '>');
        *pp = e ? e + 1 : p + strlen(p);
        return 0;
    }
    if (*p == '/') { t->close = 1; p++; }
    int n = 0;
    while (*p && *p != '>' && *p != ' ' && *p != '/' && *p != '\n' && *p != '\t') {
        if (n < 15) t->name[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
        p++;
    }
    for (;;) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;
        if (*p == '>') { p++; break; }
        if (*p == '/') { t->selfclose = 1; p++; continue; }
        const char *k = p;
        while (*p && *p != '=' && *p != '>' && *p != ' ' && *p != '/') p++;
        char *key = xstrndup(k, p - k);
        buf_t v = {0};
        if (*p == '=') {
            p++;
            char q = *p == '"' || *p == '\'' ? *p++ : 0;
            const char *vs = p;
            while (*p && (q ? *p != q : *p != ' ' && *p != '>')) p++;
            decode(&v, vs, p - vs);
            if (q && *p) p++;
        }
        if (t->nattr < 16) { t->attr[t->nattr][0] = key; t->attr[t->nattr][1] = buf_take(&v); t->nattr++; }
        else { free(key); buf_free(&v); }
    }
    *pp = p;
    return 1;
}

/* ---- chapter bodies ---------------------------------------------------------- */

static const char *BLOCK_TAGS[] = { "p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "figcaption", "tr", "dt", "dd",
                                    "blockquote", "div", "section", "header", "figure", "table", "ul", "ol" };
static const char *SKIP_TAGS[] = { "script", "style", "video", "audio", "button", "footer", "nav" };

static int in_list(const char *name, const char **list, int n)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(name, list[i])) return 1;
    return 0;
}

typedef struct { char tag[16]; int style; char *note, *href; int countable; } inl_t;
typedef struct { char tag[16]; int kind; char *anchor; } blk_t;

typedef struct {
    doc_t *doc;
    blk_t bs[64]; int nb;
    inl_t is[64]; int ni;
    run_t *runs; int nruns, cap;
    int skip;
} parser_t;

static void emit(parser_t *ps, const char *text, size_t n, int extra, int countable)
{
    int style = extra, cnt = 1;
    char *note = NULL, *href = NULL;
    for (int i = 0; i < ps->ni; i++) {
        style |= ps->is[i].style;
        if (ps->is[i].note) note = ps->is[i].note;
        if (ps->is[i].href) href = ps->is[i].href;
        cnt &= ps->is[i].countable;
    }
    if (ps->nruns == ps->cap) { ps->cap = ps->cap * 2 + 8; ps->runs = xrealloc(ps->runs, ps->cap * sizeof *ps->runs); }
    run_t *r = &ps->runs[ps->nruns++];
    r->text = xstrndup(text, n);
    r->style = style;
    r->note = xstrdup(note);
    r->href = xstrdup(href);
    r->countable = countable >= 0 ? countable : cnt;
}

static void run_free(run_t *r) { free(r->text); free(r->note); free(r->href); }

/* Ends the paragraph being collected: whitespace collapsed as HTML lays it out. */
static void flush(parser_t *ps)
{
    int prev_space = 1, out = 0;
    for (int i = 0; i < ps->nruns; i++) {
        run_t *r = &ps->runs[i];
        char *d = r->text;
        int is_br = r->countable == 0 && !r->style && !strcmp(r->text, "\n");
        for (char *s = r->text; *s; s++) {
            if (is_br) { *d++ = '\n'; prev_space = 1; continue; }   /* <br>: a line break, kept */
            int sp = *s == ' ' || *s == '\n' || *s == '\t' || *s == '\r';
            if (sp) { if (!prev_space) *d++ = ' '; prev_space = 1; }
            else { *d++ = *s; prev_space = 0; }
        }
        *d = 0;
    }
    for (int i = ps->nruns - 1; i >= 0; i--) {          /* trailing spaces */
        size_t n = strlen(ps->runs[i].text);
        while (n && ps->runs[i].text[n - 1] == ' ') ps->runs[i].text[--n] = 0;
        if (n) break;
    }
    for (int i = 0; i < ps->nruns; i++) {
        if (*ps->runs[i].text) ps->runs[out++] = ps->runs[i];
        else run_free(&ps->runs[i]);
    }
    ps->nruns = 0;
    if (!out) return;
    doc_t *d = ps->doc;
    d->b = xrealloc(d->b, (d->n + 1) * sizeof *d->b);
    block_t *b = &d->b[d->n++];
    memset(b, 0, sizeof *b);
    b->kind = ps->nb ? ps->bs[ps->nb - 1].kind : BK_P;
    b->anchor = ps->nb ? xstrdup(ps->bs[ps->nb - 1].anchor) : NULL;
    b->runs = xmalloc(out * sizeof *b->runs);
    memcpy(b->runs, ps->runs, out * sizeof *b->runs);
    b->nruns = out;
    for (int i = 0; i < out; i++)
        if (b->runs[i].style & ST_VERSENUM) { b->num = atoi(b->runs[i].text); break; }
}

static int kind_of(const tag_t *t)
{
    const char *n = t->name;
    if (!strcmp(n, "h1")) return BK_TITLE;
    if (n[0] == 'h' && n[1] >= '2' && n[1] <= '6' && !n[2]) return BK_HEADING;
    if (has_class(t, "verse")) return BK_VERSE;
    if (has_class(t, "subtitle")) return BK_SUBTITLE;
    if (has_class(t, "intro") || has_class(t, "study-intro")) return BK_INTRO;
    if (has_class(t, "title-number")) return BK_TITLENUM;
    if (has_class(t, "study-summary")) return BK_SUMMARY;
    if (has_class(t, "label")) return BK_LABEL;
    return BK_P;
}

static void start_tag(parser_t *ps, tag_t *t)
{
    const char *n = t->name;
    if (in_list(n, SKIP_TAGS, COUNT(SKIP_TAGS))) { if (!t->selfclose) ps->skip++; return; }
    if (ps->skip || !strcmp(n, "img") || !strcmp(n, "hr") || !strcmp(n, "source")) return;
    if (!strcmp(n, "br")) { emit(ps, "\n", 1, 0, 0); return; }
    if (in_list(n, BLOCK_TAGS, COUNT(BLOCK_TAGS))) {
        flush(ps);
        if (ps->nb == COUNT(ps->bs)) return;
        blk_t *b = &ps->bs[ps->nb++];
        snprintf(b->tag, sizeof b->tag, "%s", n);
        b->kind = kind_of(t);
        const char *id = attr(t, "id");
        int anchored = (!strcmp(n, "p") || !strcmp(n, "h1") || !strcmp(n, "h2") || !strcmp(n, "h3") || !strcmp(n, "li"))
                       && attr(t, "data-aid") && id;
        b->anchor = anchored ? xstrdup(id) : NULL;
        return;
    }
    if (t->selfclose || ps->ni == COUNT(ps->is)) return;
    inl_t *in = &ps->is[ps->ni++];
    memset(in, 0, sizeof *in);
    snprintf(in->tag, sizeof in->tag, "%s", n);
    in->countable = 1;
    if (!strcmp(n, "sup")) {
        in->countable = 0;
        in->style = has_class(t, "marker") ? ST_MARKER : ST_SUP;
        const char *v = attr(t, "data-value");
        if (in->style == ST_MARKER && v) emit(ps, v, strlen(v), 0, -1);
    } else if (!strcmp(n, "span") && has_class(t, "verse-number")) {
        in->countable = 0;
        in->style = ST_VERSENUM;
    } else if (!strcmp(n, "a")) {
        const char *h = attr(t, "href");
        if (h && has_class(t, "study-note-ref") && h[0] == '#') { in->style = ST_FOOTNOTE; in->note = xstrdup(h + 1); }
        else if (h && *h && h[0] != '#') { in->style = ST_LINK; in->href = xstrdup(h); }
    } else if (!strcmp(n, "i") || !strcmp(n, "em")) in->style = ST_ITALIC;
    else if (!strcmp(n, "b") || !strcmp(n, "strong")) in->style = ST_BOLD;
    else if (!strcmp(n, "small")) in->style = ST_SMALLCAPS;
    else if (!strcmp(n, "sub")) in->style = ST_SUB;
    else if (!strcmp(n, "u")) in->style = ST_UNDERLINE;
}

static void end_tag(parser_t *ps, tag_t *t)
{
    const char *n = t->name;
    if (in_list(n, SKIP_TAGS, COUNT(SKIP_TAGS))) { if (ps->skip) ps->skip--; return; }
    if (ps->skip) return;
    if (in_list(n, BLOCK_TAGS, COUNT(BLOCK_TAGS))) {
        flush(ps);
        for (int i = ps->nb - 1; i >= 0; i--)
            if (!strcmp(ps->bs[i].tag, n)) {
                for (int k = i; k < ps->nb; k++) free(ps->bs[k].anchor);
                ps->nb = i;
                break;
            }
        return;
    }
    for (int i = ps->ni - 1; i >= 0; i--)
        if (!strcmp(ps->is[i].tag, n)) {
            for (int k = i; k < ps->ni; k++) { free(ps->is[k].note); free(ps->is[k].href); }
            ps->ni = i;
            break;
        }
}

doc_t *html_doc(const char *html)
{
    parser_t ps = {0};
    ps.doc = xcalloc(1, sizeof *ps.doc);
    const char *p = html;
    while (*p) {
        if (*p == '<') {
            tag_t t;
            if (read_tag(&p, &t)) {
                if (t.close) end_tag(&ps, &t); else start_tag(&ps, &t);
                tag_free(&t);
            }
            continue;
        }
        const char *s = p;
        while (*p && *p != '<') p++;
        if (!ps.skip) {
            buf_t b = {0};
            decode(&b, s, p - s);
            if (b.n) emit(&ps, b.p, b.n, 0, -1);
            buf_free(&b);
        }
    }
    flush(&ps);
    for (int i = 0; i < ps.nb; i++) free(ps.bs[i].anchor);
    for (int i = 0; i < ps.ni; i++) { free(ps.is[i].note); free(ps.is[i].href); }
    free(ps.runs);
    return ps.doc;
}

void doc_free(doc_t *d)
{
    if (!d) return;
    for (int i = 0; i < d->n; i++) {
        for (int k = 0; k < d->b[i].nruns; k++) run_free(&d->b[i].runs[k]);
        free(d->b[i].runs);
        free(d->b[i].anchor);
    }
    free(d->b);
    free(d);
}

char *doc_text(const block_t *b, int countable_only)
{
    buf_t t = {0};
    for (int i = 0; i < b->nruns; i++)
        if (!countable_only || b->runs[i].countable) buf_puts(&t, b->runs[i].text);
    return buf_take(&t);
}

/* ---- contents pages ------------------------------------------------------------ */

static toc_t *toc_add(toc_t *parent)
{
    parent->kids = xrealloc(parent->kids, (parent->nkids + 1) * sizeof *parent->kids);
    toc_t *k = &parent->kids[parent->nkids++];
    memset(k, 0, sizeof *k);
    return k;
}

/* The manifest: nested <ul class="doc-map"><li> with either <h2 class="label"> (a group,
 * its children in the next <ul>) or <a href> with <p class="title">. */
toc_t *html_toc(const char *html)
{
    toc_t *root = xcalloc(1, sizeof *root);
    toc_t *stack[32] = { root };       /* the open groups */
    int nstack = 1;
    int li_group[64] = {0}, nli = 0;   /* for each open <li>: whether it opened a group */
    char li_type[64][32];
    toc_t *link = NULL;                 /* the link being read */
    int capture = 0, in_label = 0;      /* 1: a title, 2: a description */
    buf_t text = {0};
    const char *p = html;
    while (*p) {
        if (*p != '<') {
            const char *s = p;
            while (*p && *p != '<') p++;
            if (capture == 1) decode(&text, s, p - s);
            continue;
        }
        tag_t t;
        if (!read_tag(&p, &t)) continue;
        const char *n = t.name;
        if (!t.close) {
            if (!strcmp(n, "li") && nli < 64) {
                const char *ty = attr(&t, "data-content-type");
                snprintf(li_type[nli], sizeof li_type[nli], "%s", ty ? ty : "");
                li_group[nli++] = 0;
            } else if (!strcmp(n, "h2") && has_class(&t, "label") && nli && nstack < 32) {
                toc_t *g = toc_add(stack[nstack - 1]);
                stack[nstack++] = g;
                li_group[nli - 1] = 1;
                in_label = 1;
            } else if (!strcmp(n, "a") && attr(&t, "href") && nli) {
                if (strcmp(li_type[nli - 1], "table-of-contents")) {
                    link = toc_add(stack[nstack - 1]);
                    link->uri = uri_normalize(attr(&t, "href"));
                }
            } else if (!strcmp(n, "p") && has_class(&t, "title") && (link || in_label)) {
                capture = 1;
                buf_free(&text);
            }
        } else {
            if (!strcmp(n, "p") && capture == 1) {
                char *s = buf_take(&text);
                toc_t *target = link ? link : stack[nstack - 1];
                if (!target->title) target->title = s; else free(s);
                capture = 0;
            } else if (!strcmp(n, "h2")) in_label = 0;
            else if (!strcmp(n, "a")) link = NULL;
            else if (!strcmp(n, "li") && nli) {
                if (li_group[--nli] && nstack > 1) nstack--;
            }
        }
        tag_free(&t);
    }
    buf_free(&text);
    return root;
}

static void toc_clear(toc_t *t)
{
    for (int i = 0; i < t->nkids; i++) toc_clear(&t->kids[i]);
    free(t->kids);
    free(t->title);
    free(t->uri);
}

void toc_free(toc_t *t)
{
    if (!t) return;
    toc_clear(t);
    free(t);
}

/* ---- links ------------------------------------------------------------------------ */

char *uri_normalize(const char *href)
{
    const char *p = href;
    if (!strncmp(p, "https://", 8) || !strncmp(p, "http://", 7)) {
        p = strchr(p + 8, '/');
        if (!p) return xstrdup("/");
    }
    if (!strncmp(p, "/study/", 7)) p += 6;
    size_t n = strcspn(p, "?#");
    while (n > 1 && p[n - 1] == '/') n--;
    return xstrndup(p, n);
}

static char *query_param(const char *q, const char *key)
{
    size_t k = strlen(key);
    while (q && *q) {
        if (!strncmp(q, key, k) && q[k] == '=') return xstrndup(q + k + 1, strcspn(q + k + 1, "&#"));
        q = strchr(q, '&');
        if (q) q++;
    }
    return NULL;
}

int link_resolve(const char *href, const char *dflt_lang, char **lang, char **uri, char **anchor, char **url)
{
    *lang = *uri = *anchor = *url = NULL;
    int local = href[0] == '/' || strstr(href, "churchofjesuschrist.org/") != NULL;
    char *path = uri_normalize(href);
    if (local && !strncmp(path, "/scriptures/", 12)) {
        const char *q = strchr(href, '?');
        char *l = q ? query_param(q + 1, "lang") : NULL;
        char *id = q ? query_param(q + 1, "id") : NULL;
        const char *hash = strchr(href, '#');
        if (!id && hash) id = xstrdup(hash + 1);
        if (id) id[strcspn(id, "-,")] = 0;
        *lang = l ? l : xstrdup(dflt_lang);
        *uri = path;
        *anchor = id;
        return 1;
    }
    free(path);
    *url = href[0] == '/' ? xprintf("https://www.churchofjesuschrist.org%s", href) : xstrdup(href);
    return 0;
}
