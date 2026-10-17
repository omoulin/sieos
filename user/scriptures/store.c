/*
 * store.c - The library on disk (store.h).
 */
#include "store.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *DIR_;
static pthread_mutex_t notes_mu = PTHREAD_MUTEX_INITIALIZER, idx_mu = PTHREAD_MUTEX_INITIALIZER,
                       set_mu = PTHREAD_MUTEX_INITIALIZER;

void store_init(const char *dir)
{
    DIR_ = xstrdup(dir);
    mkdirs(dir, 0700);
}
const char *store_dir(void) { return DIR_; }

static char *lpath(const char *lang, const char *rest) { return xprintf("%s/lib/%s/%s", DIR_, lang, rest); }

static char *ch_path(const char *lang, const char *uri)
{
    char *u = xstrdup(uri[0] == '/' ? uri + 1 : uri);
    for (char *p = u; *p; p++)
        if (*p == '/') *p = '~';
    char *p = xprintf("%s/lib/%s/ch/%s.json", DIR_, lang, u);
    free(u);
    return p;
}

/* ---- volumes ---------------------------------------------------------------------- */

static void toc_write(buf_t *b, const toc_t *t, int depth)
{
    for (int i = 0; i < t->nkids; i++) {
        const toc_t *k = &t->kids[i];
        buf_printf(b, "%d\t%s\t", depth, k->uri ? k->uri : "-");
        for (const char *s = k->title ? k->title : ""; *s; s++) buf_putc(b, *s == '\t' || *s == '\n' ? ' ' : *s);
        buf_putc(b, '\n');
        toc_write(b, k, depth + 1);
    }
}

/* The contents file, back into a tree: a flat list first, then each level built
 * whole before the next (so no pointer into a growing array is kept). */
typedef struct { int depth; char *uri, *title; } tent_t;

static void toc_build(toc_t *parent, int depth, tent_t *e, int n, int *pos)
{
    int count = 0;
    for (int i = *pos; i < n && e[i].depth >= depth; i++) count += e[i].depth == depth;
    parent->kids = count ? xcalloc(count, sizeof *parent->kids) : NULL;
    parent->nkids = 0;
    while (*pos < n && e[*pos].depth >= depth) {
        if (e[*pos].depth > depth) { free(e[*pos].uri); free(e[*pos].title); (*pos)++; continue; }   /* (malformed) */
        toc_t *k = &parent->kids[parent->nkids++];
        k->uri = e[*pos].uri;
        k->title = e[*pos].title;
        (*pos)++;
        if (*pos < n && e[*pos].depth > depth) toc_build(k, depth + 1, e, n, pos);
    }
}

static toc_t *toc_load(const char *text)
{
    tent_t *e = NULL;
    int n = 0;
    for (const char *line = text; line && *line;) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char *l = xstrndup(line, len), *t1 = strchr(l, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t1 && t2) {
            *t1 = *t2 = 0;
            e = xrealloc(e, (n + 1) * sizeof *e);
            e[n].depth = CLAMP(atoi(l), 0, 30);
            e[n].uri = strcmp(t1 + 1, "-") ? xstrdup(t1 + 1) : NULL;
            e[n].title = xstrdup(t2 + 1);
            n++;
        }
        free(l);
        line = nl ? nl + 1 : NULL;
    }
    toc_t *root = xcalloc(1, sizeof *root);
    int pos = 0;
    toc_build(root, 0, e, n, &pos);
    free(e);
    return root;
}

volumes_t *store_volumes(const char *lang)
{
    volumes_t *vs = xcalloc(1, sizeof *vs);
    char *p = lpath(lang, "volumes.txt");
    char *text = read_file(p, NULL);
    free(p);
    for (char *line = text; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            char *tp = xprintf("%s/lib/%s/toc/%s.txt", DIR_, lang, line);
            char *tt = read_file(tp, NULL);
            free(tp);
            if (tt) {
                vs->v = xrealloc(vs->v, (vs->n + 1) * sizeof *vs->v);
                volume_t *v = &vs->v[vs->n++];
                snprintf(v->code, sizeof v->code, "%s", line);
                v->title = xstrdup(tab + 1);
                v->toc = toc_load(tt);
                free(tt);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(text);
    return vs;
}

void store_volumes_free(volumes_t *vs)
{
    if (!vs) return;
    for (int i = 0; i < vs->n; i++) { free(vs->v[i].title); toc_free(vs->v[i].toc); }
    free(vs->v);
    free(vs);
}

static int rank_of(const char **order, int norder, const char *code)
{
    for (int i = 0; i < norder; i++)
        if (!strcmp(order[i], code)) return i;
    return norder;
}

/* Saves a volume's contents and lists it in volumes.txt, kept in the given order. */
int store_save_volume(const char *lang, const char *code, const char *title, const toc_t *toc,
                      const char **order, int norder)
{
    buf_t b = {0};
    toc_write(&b, toc, 0);
    char *p = xprintf("%s/lib/%s/toc/%s.txt", DIR_, lang, code);
    int r = write_file(p, b.p ? b.p : "", b.n, 0644);
    free(p);
    buf_free(&b);
    if (r) return r;

    /* volumes.txt: the existing entries plus this one, in `order` */
    char *vp = lpath(lang, "volumes.txt");
    char *old = read_file(vp, NULL);
    char *codes[64], *titles[64];
    int n = 0;
    for (char *line = old; line && *line && n < 63;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *tab = strchr(line, '\t');
        if (tab && strncmp(line, code, tab - line)) {
            *tab = 0;
            codes[n] = xstrdup(line);
            titles[n++] = xstrdup(tab + 1);
        }
        line = nl ? nl + 1 : NULL;
    }
    free(old);
    codes[n] = xstrdup(code);
    titles[n++] = xstrdup(title);
    for (int i = 1; i < n; i++)                     /* (insertion sort by rank) */
        for (int j = i; j > 0 && rank_of(order, norder, codes[j]) < rank_of(order, norder, codes[j - 1]); j--) {
            char *t = codes[j]; codes[j] = codes[j - 1]; codes[j - 1] = t;
            t = titles[j]; titles[j] = titles[j - 1]; titles[j - 1] = t;
        }
    for (int i = 0; i < n; i++) { buf_printf(&b, "%s\t%s\n", codes[i], titles[i]); free(codes[i]); free(titles[i]); }
    r = write_file(vp, b.p, b.n, 0644);
    free(vp);
    buf_free(&b);
    return r;
}

char **store_languages(int *n)
{
    char *p = xprintf("%s/lib", DIR_);
    DIR *d = opendir(p);
    free(p);
    char **out = NULL;
    *n = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char *vp = lpath(e->d_name, "volumes.txt");
        if (file_exists(vp)) { out = xrealloc(out, (*n + 1) * sizeof *out); out[(*n)++] = xstrdup(e->d_name); }
        free(vp);
    }
    if (d) closedir(d);
    return out;
}

/* ---- chapters ------------------------------------------------------------------------- */

int store_has_chapter(const char *lang, const char *uri)
{
    char *p = ch_path(lang, uri);
    int r = file_exists(p);
    free(p);
    return r;
}

int store_save_chapter(const char *lang, const char *uri, const char *json, size_t n)
{
    char *p = ch_path(lang, uri);
    int r = write_file(p, json, n, 0644);
    free(p);
    return r;
}

chapter_t *store_chapter(const char *lang, const char *uri)
{
    char *p = ch_path(lang, uri);
    size_t n;
    char *text = read_file(p, &n);
    free(p);
    if (!text) return NULL;
    jnode *j = json_parse(text, n);
    free(text);
    if (!j) return NULL;
    chapter_t *c = xcalloc(1, sizeof *c);
    c->lang = xstrdup(lang);
    c->uri = xstrdup(uri);
    c->json = j;
    const char *title = json_str(json_get(j, "meta"), "title");
    c->title = xstrdup(title ? title : uri);
    jnode *audio = json_path(j, "meta.audio");
    if (audio && audio->kid) c->audio = xstrdup(json_str(audio->kid, "mediaUrl"));
    c->footnotes = json_path(j, "content.footnotes");
    const char *body = json_str(json_get(j, "content"), "body");
    c->doc = html_doc(body ? body : "");
    return c;
}

void chapter_free(chapter_t *c)
{
    if (!c) return;
    free(c->lang); free(c->uri); free(c->title); free(c->audio);
    json_free(c->json);
    doc_free(c->doc);
    free(c);
}

char *store_chapter_title(const char *lang, const char *uri)
{
    chapter_t *c = store_chapter(lang, uri);
    char *t = c ? xstrdup(c->title) : NULL;
    chapter_free(c);
    return t;
}

/* ---- search ----------------------------------------------------------------------------- */

typedef struct { char *uri, *anchor, *title, *text, *folded; int num; } ent_t;
typedef struct { char lang[16]; ent_t *e; int n; char *raw; } index_t;
static index_t idx;                      /* the last language searched */

static char *fold(const char *s)
{
    buf_t b = {0};
    for (int i = 0, k; s[i]; i += k) {
        int c = fold_char(utf8_dec(s + i, &k));
        char o[4];
        buf_add(&b, o, utf8_enc(o, c));
    }
    return buf_take(&b);
}

static void index_clear(void)
{
    for (int i = 0; i < idx.n; i++) free(idx.e[i].folded);
    free(idx.e);
    free(idx.raw);
    memset(&idx, 0, sizeof idx);
}

static void index_load(const char *lang)
{
    if (!strcmp(idx.lang, lang) && idx.e) return;
    index_clear();
    snprintf(idx.lang, sizeof idx.lang, "%s", lang);
    char *p = lpath(lang, "search.txt");
    idx.raw = read_file(p, NULL);
    free(p);
    int cap = 0;
    for (char *line = idx.raw; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *f[5] = { line };
        int k = 1;
        for (char *s = line; *s && k < 5; s++)
            if (*s == '\t') { *s = 0; f[k++] = s + 1; }
        if (k == 5) {
            if (idx.n == cap) { cap = cap * 2 + 1024; idx.e = xrealloc(idx.e, cap * sizeof *idx.e); }
            ent_t *e = &idx.e[idx.n++];
            e->uri = f[0]; e->anchor = f[1]; e->num = atoi(f[2]); e->title = f[3]; e->text = f[4];
            e->folded = fold(f[4]);
        }
        line = nl ? nl + 1 : NULL;
    }
}

static void index_walk(buf_t *b, const char *lang, const toc_t *t)
{
    for (int i = 0; i < t->nkids; i++) {
        const toc_t *k = &t->kids[i];
        if (k->uri) {
            chapter_t *c = store_chapter(lang, k->uri);
            for (int j = 0; c && j < c->doc->n; j++) {
                block_t *bl = &c->doc->b[j];
                if (bl->kind != BK_VERSE || !bl->anchor) continue;
                char *text = doc_text(bl, 1);
                for (char *s = text; *s; s++)
                    if (*s == '\t' || *s == '\n') *s = ' ';
                char *s = text;
                while (*s == ' ') s++;
                buf_printf(b, "%s\t%s\t%d\t%s\t%s\n", k->uri, bl->anchor, bl->num, c->title, s);
                free(text);
            }
            chapter_free(c);
        }
        index_walk(b, lang, k);
    }
}

int store_rebuild_index(const char *lang)
{
    volumes_t *vs = store_volumes(lang);
    buf_t b = {0};
    for (int i = 0; i < vs->n; i++) index_walk(&b, lang, vs->v[i].toc);
    store_volumes_free(vs);
    char *p = lpath(lang, "search.txt");
    int r = write_file(p, b.p ? b.p : "", b.n, 0644);
    free(p);
    buf_free(&b);
    pthread_mutex_lock(&idx_mu);
    if (!strcmp(idx.lang, lang)) index_clear();
    pthread_mutex_unlock(&idx_mu);
    return r;
}

/* Every word of the query must appear (accents and case ignored); in library order. */
hit_t *store_search(const char *lang, const char *query, int max, int *n)
{
    pthread_mutex_lock(&idx_mu);
    index_load(lang);
    char *q = fold(query), *words[16];
    int nw = 0;
    char *rest = q;
    for (char *s; nw < 16 && (s = tok(&rest, " \t\"'")); ) words[nw++] = s;
    hit_t *h = NULL;
    *n = 0;
    for (int i = 0; nw && i < idx.n && *n < max; i++) {
        ent_t *e = &idx.e[i];
        char *first = NULL;
        int ok = 1;
        for (int w = 0; w < nw && ok; w++) {
            char *m = strstr(e->folded, words[w]);
            if (!m) ok = 0;
            else if (!first || m < first) first = m;
        }
        if (!ok) continue;
        /* the snippet: around the first match, in the original text (same byte offsets
         * whenever folding kept lengths; else from the start) */
        int off = (int)(first - e->folded), tl = (int)strlen(e->text);
        int same = (int)strlen(e->folded) == tl;
        int s0 = same ? MAX(0, off - 50) : 0;
        while (s0 > 0 && ((unsigned char)e->text[s0] & 0xc0) == 0x80) s0--;
        if (s0) { while (s0 < tl && e->text[s0] != ' ') s0++; }
        int s1 = MIN(tl, s0 + 170);
        while (s1 < tl && ((unsigned char)e->text[s1] & 0xc0) == 0x80) s1++;
        h = xrealloc(h, (*n + 1) * sizeof *h);
        hit_t *r = &h[(*n)++];
        r->uri = xstrdup(e->uri);
        r->anchor = xstrdup(e->anchor);
        r->title = xstrdup(e->title);
        r->num = e->num;
        r->snippet = xprintf("%s%.*s%s", s0 ? "…" : "", s1 - s0, e->text + s0, s1 < tl ? "…" : "");
        r->hl0 = same ? off - s0 + (s0 ? 3 : 0) : -1;
        r->hl1 = same ? r->hl0 + (int)strlen(words[0]) : -1;
        if (same && strncmp(e->folded + off, words[0], strlen(words[0]))) r->hl0 = r->hl1 = -1;
    }
    free(q);
    pthread_mutex_unlock(&idx_mu);
    return h;
}

void hits_free(hit_t *h, int n)
{
    for (int i = 0; i < n; i++) { free(h[i].uri); free(h[i].anchor); free(h[i].title); free(h[i].snippet); }
    free(h);
}

const char *store_verse_text(const char *lang, const char *uri, const char *anchor, int *num)
{
    pthread_mutex_lock(&idx_mu);
    index_load(lang);
    const char *t = NULL;
    for (int i = 0; i < idx.n; i++)
        if (!strcmp(idx.e[i].uri, uri) && !strcmp(idx.e[i].anchor, anchor)) {
            t = idx.e[i].text;
            if (num) *num = idx.e[i].num;
            break;
        }
    pthread_mutex_unlock(&idx_mu);
    return t;
}

/* ---- notes ------------------------------------------------------------------------------- */

static note_t **notes;
static int nnotes, notes_loaded, notes_ver;

void notes_lock(void) { pthread_mutex_lock(&notes_mu); }
void notes_unlock(void) { pthread_mutex_unlock(&notes_mu); }

static void note_free(note_t *n)
{
    free(n->id); free(n->lang); free(n->uri); free(n->anchor); free(n->color); free(n->style);
    free(n->title); free(n->content); free(n->spans);
    for (int i = 0; i < n->ntags; i++) free(n->tags[i]);
    free(n->tags);
    json_free(n->remote);
    free(n);
}

static note_t *note_from_json(const jnode *j)
{
    note_t *n = xcalloc(1, sizeof *n);
    n->id = xstrdup(json_str(j, "id"));
    n->lang = xstrdup(json_str(j, "lang") ? json_str(j, "lang") : "eng");
    n->uri = xstrdup(json_str(j, "uri") ? json_str(j, "uri") : "");
    n->anchor = xstrdup(json_str(j, "anchor"));
    n->color = xstrdup(json_str(j, "color"));
    n->style = xstrdup(json_str(j, "style"));
    n->title = xstrdup(json_str(j, "title") ? json_str(j, "title") : "");
    n->content = xstrdup(json_str(j, "content") ? json_str(j, "content") : "");
    jnode *sp = json_get(j, "spans");
    n->spans = xcalloc(json_count(sp) + 1, sizeof *n->spans);
    for (jnode *k = sp ? sp->kid : NULL; k; k = k->next) {
        span_t *s = &n->spans[n->nspans++];
        snprintf(s->anchor, sizeof s->anchor, "%s", json_str(k, "anchor") ? json_str(k, "anchor") : "");
        s->start = (int)json_num(k, "start", -1);
        s->end = (int)json_num(k, "end", -1);
    }
    jnode *tg = json_get(j, "tags");
    n->tags = xcalloc(json_count(tg) + 1, sizeof *n->tags);
    for (jnode *k = tg ? tg->kid : NULL; k; k = k->next)
        if (k->t == J_STR) n->tags[n->ntags++] = xstrdup(k->s);
    n->created = json_num(j, "created_at", now_s());
    n->updated = json_num(j, "updated_at", n->created);
    n->deleted = (int)json_num(j, "deleted", 0);
    n->dirty = (int)json_num(j, "dirty", 1);
    jnode *r = json_get(j, "remote");
    n->remote = r && r->t == J_OBJ ? json_clone(r) : NULL;
    if (!n->id) n->id = xstrdup("?");
    return n;
}

static void note_to_json(buf_t *b, const note_t *n)
{
    buf_puts(b, "{\"id\":"); json_write_str(b, n->id);
    buf_puts(b, ",\"lang\":"); json_write_str(b, n->lang);
    buf_puts(b, ",\"uri\":"); json_write_str(b, n->uri);
    if (n->anchor) { buf_puts(b, ",\"anchor\":"); json_write_str(b, n->anchor); }
    buf_puts(b, ",\"spans\":[");
    for (int i = 0; i < n->nspans; i++)
        buf_printf(b, "%s{\"anchor\":\"%s\",\"start\":%d,\"end\":%d}", i ? "," : "", n->spans[i].anchor,
                   n->spans[i].start, n->spans[i].end);
    buf_puts(b, "]");
    if (n->color) { buf_puts(b, ",\"color\":"); json_write_str(b, n->color); }
    if (n->style) { buf_puts(b, ",\"style\":"); json_write_str(b, n->style); }
    buf_puts(b, ",\"title\":"); json_write_str(b, n->title);
    buf_puts(b, ",\"content\":"); json_write_str(b, n->content);
    buf_puts(b, ",\"tags\":[");
    for (int i = 0; i < n->ntags; i++) { if (i) buf_putc(b, ','); json_write_str(b, n->tags[i]); }
    buf_printf(b, "],\"created_at\":%.3f,\"updated_at\":%.3f,\"deleted\":%d,\"dirty\":%d",
               n->created, n->updated, n->deleted, n->dirty);
    if (n->remote) { buf_puts(b, ",\"remote\":"); json_write(b, n->remote); }
    buf_puts(b, "}");
}

static void notes_load(void)
{
    if (notes_loaded) return;
    notes_loaded = 1;
    char *p = xprintf("%s/notes.json", DIR_);
    size_t len;
    char *text = read_file(p, &len);
    free(p);
    jnode *j = text ? json_parse(text, len) : NULL;
    free(text);
    for (jnode *k = j ? j->kid : NULL; k; k = k->next) {
        notes = xrealloc(notes, (nnotes + 1) * sizeof *notes);
        notes[nnotes++] = note_from_json(k);
    }
    json_free(j);
}

note_t **notes_all(int *n) { notes_load(); *n = nnotes; return notes; }

note_t *note_find(const char *id)
{
    notes_load();
    for (int i = 0; i < nnotes; i++)
        if (!strcmp(notes[i]->id, id)) return notes[i];
    return NULL;
}

static char *uuid4(void)
{
    unsigned char r[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(r, 1, 16, f) != 16) for (int i = 0; i < 16; i++) r[i] = (unsigned char)(rand() ^ (int)(now_s() * 1000));
    if (f) fclose(f);
    r[6] = (r[6] & 0x0f) | 0x40;
    r[8] = (r[8] & 0x3f) | 0x80;
    return xprintf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                   r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

note_t *note_new(void)
{
    notes_load();
    note_t *n = xcalloc(1, sizeof *n);
    n->id = uuid4();
    n->title = xstrdup("");
    n->content = xstrdup("");
    n->created = n->updated = now_s();
    n->dirty = 1;
    notes = xrealloc(notes, (nnotes + 1) * sizeof *notes);
    notes[nnotes++] = n;
    return n;
}

note_t *note_add_json(const jnode *j)
{
    notes_load();
    note_t *n = note_from_json(j);
    notes = xrealloc(notes, (nnotes + 1) * sizeof *notes);
    notes[nnotes++] = n;
    return n;
}

void note_touch(note_t *n) { n->updated = now_s(); n->dirty = 1; }
void note_delete(note_t *n) { n->deleted = 1; note_touch(n); }

void note_purge(note_t *n)
{
    for (int i = 0; i < nnotes; i++)
        if (notes[i] == n) {
            memmove(notes + i, notes + i + 1, (nnotes - i - 1) * sizeof *notes);
            nnotes--;
            note_free(n);
            return;
        }
}

void note_set_tags(note_t *n, const char *list)
{
    for (int i = 0; i < n->ntags; i++) free(n->tags[i]);
    n->ntags = 0;
    n->tags = xrealloc(n->tags, (strlen(list) / 2 + 2) * sizeof *n->tags);
    char *copy = xstrdup(list);
    char *rest = copy;
    for (char *t; (t = tok(&rest, ",")); ) {
        while (*t == ' ') t++;
        size_t l = strlen(t);
        while (l && t[l - 1] == ' ') t[--l] = 0;
        if (l) n->tags[n->ntags++] = xstrdup(t);
    }
    free(copy);
}

char *note_tags_str(const note_t *n)
{
    buf_t b = {0};
    for (int i = 0; i < n->ntags; i++) { if (i) buf_puts(&b, ", "); buf_puts(&b, n->tags[i]); }
    return buf_take(&b);
}

int notes_version(void) { return notes_ver; }

int notes_save(void)
{
    notes_ver++;
    buf_t b = {0};
    buf_puts(&b, "[\n");
    for (int i = 0; i < nnotes; i++) {
        if (i) buf_puts(&b, ",\n");
        note_to_json(&b, notes[i]);
    }
    buf_puts(&b, "\n]\n");
    char *p = xprintf("%s/notes.json", DIR_);
    int r = write_file(p, b.p, b.n, 0600);
    free(p);
    buf_free(&b);
    return r;
}

int notes_count_for(const char *uri)
{
    int c = 0;
    notes_load();
    for (int i = 0; i < nnotes; i++) c += !notes[i]->deleted && !strcmp(notes[i]->uri, uri);
    return c;
}

/* ---- settings ------------------------------------------------------------------------------ */

static char **skeys, **svals;
static int nset, set_loaded;

static void settings_load(void)
{
    if (set_loaded) return;
    set_loaded = 1;
    char *p = xprintf("%s/settings.txt", DIR_);
    char *t = read_file(p, NULL);
    free(p);
    for (char *line = t; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            skeys = xrealloc(skeys, (nset + 1) * sizeof *skeys);
            svals = xrealloc(svals, (nset + 1) * sizeof *svals);
            skeys[nset] = xstrdup(line);
            svals[nset++] = xstrdup(eq + 1);
        }
        line = nl ? nl + 1 : NULL;
    }
    free(t);
}

const char *setting(const char *key, const char *dflt)
{
    pthread_mutex_lock(&set_mu);
    settings_load();
    const char *v = dflt;
    for (int i = 0; i < nset; i++)
        if (!strcmp(skeys[i], key)) { v = svals[i]; break; }
    pthread_mutex_unlock(&set_mu);
    return v;
}
int setting_int(const char *key, int dflt) { const char *v = setting(key, NULL); return v ? atoi(v) : dflt; }

void setting_set(const char *key, const char *value)
{
    pthread_mutex_lock(&set_mu);
    settings_load();
    int i;
    for (i = 0; i < nset; i++)
        if (!strcmp(skeys[i], key)) break;
    if (i == nset) {
        skeys = xrealloc(skeys, (nset + 1) * sizeof *skeys);
        svals = xrealloc(svals, (nset + 1) * sizeof *svals);
        skeys[nset] = xstrdup(key);
        svals[nset++] = NULL;
    }
    char *old = svals[i];
    svals[i] = xstrdup(value);
    buf_t b = {0};
    for (int k = 0; k < nset; k++) {
        buf_printf(&b, "%s=", skeys[k]);
        for (const char *s = svals[k]; *s; s++) buf_putc(&b, *s == '\n' ? ' ' : *s);
        buf_putc(&b, '\n');
    }
    char *p = xprintf("%s/settings.txt", DIR_);
    write_file(p, b.p, b.n, 0644);
    free(p);
    buf_free(&b);
    pthread_mutex_unlock(&set_mu);
    (void)old;   /* (kept: a caller may still hold the previous value) */
}
void setting_set_int(const char *key, int v) { char s[24]; snprintf(s, sizeof s, "%d", v); setting_set(key, s); }

/* ---- account ---------------------------------------------------------------------------------- */

char *account_cookie(void)
{
    char *p = xprintf("%s/account", DIR_);
    char *c = read_file(p, NULL);
    free(p);
    if (c) {
        size_t l = strlen(c);
        while (l && (c[l - 1] == '\n' || c[l - 1] == ' ')) c[--l] = 0;
        if (!l) { free(c); c = NULL; }
    }
    return c;
}
int account_save(const char *cookie)
{
    const char *c = cookie;
    if (!strncasecmp(c, "cookie:", 7)) c += 7;
    while (*c == ' ') c++;
    char *p = xprintf("%s/account", DIR_);
    int r = write_file(p, c, strlen(c), 0600);
    free(p);
    return r;
}
void account_clear(void)
{
    char *p = xprintf("%s/account", DIR_);
    unlink(p);
    free(p);
}
