/*
 * notesview.c - Every note and highlight, in a tab of its own: a card each (the
 * reference, the highlighted words, the title, the note, the tags), filtered by
 * text or tag, newest first or in book order.
 */
#include "app.h"
#include "font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char *id, *lang, *uri, *anchor, *ref, *quote, *title, *content;
    char **tags; int ntags;
    char *hay;                         /* lower-case text, for the filter */
    int color, order, verse;
    double updated;
} nv_t;

static nv_t *nv;
static int nnv, built = -1;
static char *tags[256]; static int tag_count[256], ntags;
static char *tag_on;                   /* the tag chosen, or NULL */
static int by_book;
static int *shown; static int nshown;

int is_notes_tab(const tab_t *t) { return t && t->uri && !strcmp(t->uri, NOTES_URI); }

void open_notes_tab(void)
{
    for (int i = 0; i < A.ntabs; i++)
        if (is_notes_tab(A.tabs[i])) { A.cur = i; A.dirty = 1; save_session(); return; }
    tab_t *t = cur_tab();
    if (!t || t->uri) { tab_new(1); t = cur_tab(); }
    free(t->uri);
    t->uri = xstrdup(NOTES_URI);
    free(t->title);
    t->title = xstrdup("Notes & Highlights");
    t->scroll = 0;
    save_session();
    A.dirty = 1;
}

/* ---- the list (rebuilt when the notes change) ------------------------------------------- */

static void clear(void)
{
    for (int i = 0; i < nnv; i++) {
        nv_t *n = &nv[i];
        free(n->id); free(n->lang); free(n->uri); free(n->anchor); free(n->ref); free(n->quote);
        free(n->title); free(n->content); free(n->hay);
        for (int k = 0; k < n->ntags; k++) free(n->tags[k]);
        free(n->tags);
    }
    free(nv); nv = NULL; nnv = 0;
    for (int i = 0; i < ntags; i++) free(tags[i]);
    ntags = 0;
}

/* The highlighted words of a span, from the verse's text (its countable words). */
static void span_words(buf_t *q, const char *verse, const span_t *s)
{
    char *copy = xstrdup(verse), *rest = copy;
    int i = 0, any = 0;
    for (char *w; (w = tok(&rest, " \t\n")); i++) {
        if ((s->start >= 0 && i < s->start) || (s->end >= 0 && i > s->end)) continue;
        if (any++) buf_putc(q, ' ');
        buf_puts(q, w);
    }
    free(copy);
}

/* A verse's text: in the note's language, else in a library that has it (the paragraph ids
 * are the same in every language). *used: the language found. */
static const char *verse_any(const char *lang, const char *uri, const char *anchor, int *num, const char **used)
{
    const char *langs[3] = { lang, A.lang, "eng" };
    for (int i = 0; i < 3; i++) {
        const char *t = store_verse_text(langs[i], uri, anchor, num);
        if (t) { if (used) *used = langs[i]; return t; }
    }
    return NULL;
}

static void lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }

/* A page outside the library (a manual's lesson...): a readable name from its address,
 * "/manual/endowed-from-on-high/lesson-7" -> "Endowed From On High · Lesson 7". */
static char *pretty_uri(const char *uri)
{
    buf_t b = {0};
    const char *p = uri[0] == '/' ? uri + 1 : uri;
    const char *first = strchr(p, '/');
    if (first) p = first + 1;                          /* (the collection: "manual", "general-conference"...) */
    int cap = 1;
    for (; *p; p++) {
        if (*p == '/') { buf_puts(&b, " · "); cap = 1; continue; }
        if (*p == '-' || *p == '_') { buf_putc(&b, ' '); cap = 1; continue; }
        buf_putc(&b, cap && *p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        cap = 0;
    }
    return buf_take(&b);
}

static void build(void)
{
    clear();
    notes_lock();
    int n;
    note_t **all = notes_all(&n);
    nv = xcalloc(n + 1, sizeof *nv);
    for (int i = 0; i < n; i++) {
        note_t *no = all[i];
        if (no->deleted) continue;
        nv_t *v = &nv[nnv++];
        v->id = xstrdup(no->id);
        v->lang = xstrdup(no->lang);
        v->uri = xstrdup(no->uri);
        v->anchor = xstrdup(no->anchor);
        v->title = xstrdup(no->title);
        v->content = xstrdup(no->content);
        v->color = note_color_index(no->color);
        v->updated = no->updated;
        v->order = order_of(no->uri);
        buf_t q = {0};
        int v0 = 0, v1 = 0;
        for (int s = 0; s < no->nspans; s++) {
            int num = 0;
            const char *used = NULL;
            const char *verse = verse_any(no->lang, no->uri, no->spans[s].anchor, &num, &used);
            if (num && !v0) v0 = num;
            if (num) v1 = num;
            if (!verse) continue;
            if (q.n) buf_puts(&q, " … ");
            /* word positions count in the note's language: in another, the whole verse */
            span_t whole = no->spans[s];
            if (strcmp(used, no->lang)) whole.start = whole.end = -1;
            span_words(&q, verse, &whole);
        }
        v->quote = buf_take(&q);
        v->verse = v0;
        const char *t = *no->uri ? leaf_title(no->uri) : NULL;
        char *ct = t ? NULL : *no->uri ? store_chapter_title(no->lang, no->uri) : NULL;
        if (!t && !ct && *no->uri) ct = pretty_uri(no->uri);
        const char *name = t ? t : ct ? ct : *no->uri ? no->uri : "Journal entry";
        v->ref = v0 ? (v1 != v0 ? xprintf("%s:%d–%d", name, v0, v1) : xprintf("%s:%d", name, v0)) : xstrdup(name);
        free(ct);
        v->tags = xcalloc(no->ntags + 1, sizeof *v->tags);
        for (int k = 0; k < no->ntags; k++) {
            v->tags[v->ntags++] = xstrdup(no->tags[k]);
            int j;
            for (j = 0; j < ntags && strcmp(tags[j], no->tags[k]); j++) ;
            if (j == ntags && ntags < 256) { tags[ntags] = xstrdup(no->tags[k]); tag_count[ntags++] = 0; }
            if (j < 256) tag_count[j]++;
        }
        char *tagstr = note_tags_str(no);
        v->hay = xprintf("%s\n%s\n%s\n%s\n%s", v->ref, v->quote, v->title, v->content, tagstr);
        free(tagstr);
        lower(v->hay);
    }
    notes_unlock();
    for (int i = 1; i < ntags; i++)                     /* (tags: alphabetical) */
        for (int j = i; j > 0 && strcmp(tags[j], tags[j - 1]) < 0; j--) {
            char *t = tags[j]; tags[j] = tags[j - 1]; tags[j - 1] = t;
            int c = tag_count[j]; tag_count[j] = tag_count[j - 1]; tag_count[j - 1] = c;
        }
    if (tag_on) {                                        /* (a tag no note has any more) */
        int k = 0;
        for (int i = 0; i < ntags; i++) k |= !strcmp(tags[i], tag_on);
        if (!k) { free(tag_on); tag_on = NULL; }
    }
    built = notes_version();
}

static int cmp_newest(const void *a, const void *b)
{
    double d = nv[*(const int *)b].updated - nv[*(const int *)a].updated;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}
static int cmp_book(const void *a, const void *b)
{
    const nv_t *x = &nv[*(const int *)a], *y = &nv[*(const int *)b];
    int ox = x->order < 0 ? 1 << 30 : x->order, oy = y->order < 0 ? 1 << 30 : y->order;
    if (ox != oy) return ox < oy ? -1 : 1;
    if (x->verse != y->verse) return x->verse < y->verse ? -1 : 1;
    return 0;
}

static void select_shown(void)
{
    free(shown);
    shown = xmalloc((nnv + 1) * sizeof *shown);
    nshown = 0;
    char *f = xstrdup(field_str(&A.nv_filter));
    lower(f);
    for (int i = 0; i < nnv; i++) {
        if (*f && !strstr(nv[i].hay, f)) continue;
        if (tag_on) {
            int has = 0;
            for (int k = 0; k < nv[i].ntags; k++) has |= !strcmp(nv[i].tags[k], tag_on);
            if (!has) continue;
        }
        shown[nshown++] = i;
    }
    free(f);
    qsort(shown, nshown, sizeof *shown, by_book ? cmp_book : cmp_newest);
}

/* ---- drawing ------------------------------------------------------------------------------- */

static int chip(int x, int y, const char *label, int on, int id, int a, const void *p)
{
    int px = ui_small(), h = U(26), w = text_w(label, (int)strlen(label), F_REG, px) + U(22);
    int hov = hovered(x, y, x + w, y + h);
    draw_rect(x, y, x + w, y + h, h / 2, on ? C_ACC : hov ? C_BAR : C_GLASS);
    draw_frame(x, y, x + w, y + h, h / 2, 1, on ? C_ACC : C_LINE);
    draw_text(x + U(11), y + (h - px) / 2 + font_ascent(px) - U(1), label, (int)strlen(label), F_REG, px, on ? C_INK : C_TXT);
    zone(x, y, x + w, y + h, id, a, p, CUR_HAND);
    return x + w;
}

void draw_notes_view(tab_t *t, int x0, int y0, int x1, int y1)
{
    if (built != notes_version()) build();
    select_shown();
    int w = MIN(x1 - x0 - U(60), U(880)), cx = x0 + (x1 - x0 - w) / 2, cx1 = cx + w;
    int px = ui_px(), sp = ui_small(), rp = A.reader_px * 9 / 10;
    draw_clip(x0 + 1, y0 + 1, x1 - 1, y1 - 1);
    zone(x0, y0, x1, y1, Z_READER, 0, NULL, CUR_ARROW);      /* (first, so under the cards: the wheel scrolls) */
    int y = y0 + U(24) - t->scroll, top = y;

    /* the header: the title and count, the filter, the order, the tags */
    char head[64];
    snprintf(head, sizeof head, nshown == nnv ? "%d notes and highlights" : "%d of %d notes and highlights", nshown, nnv);
    ui_text(cx, y, "Notes & Highlights", F_BOLD, ui_title() * 13 / 10, C_ACCT);
    ui_text(cx, y + U(32), head, F_REG, sp, C_DIM);
    y += U(60);
    int bh = U(32), fw = MIN(U(420), w - U(260));
    field_draw(&A.nv_filter, cx, y, cx + fw, y + bh, "Filter: words, references, tags…", A.focus == &A.nv_filter, Z_FIELD);
    int bx = cx + fw + U(14);
    bx = ui_button(bx, y, bh, "Newest", by_book ? BTN_PLAIN : BTN_ON, Z_NV_SORT, 0, NULL) + U(8);
    ui_button(bx, y, bh, "Book order", by_book ? BTN_ON : BTN_PLAIN, Z_NV_SORT, 1, NULL);
    y += bh + U(14);
    if (ntags) {
        int x = cx;
        x = chip(x, y, "All", !tag_on, Z_NV_CLEAR, 0, NULL) + U(6);
        for (int i = 0; i < ntags; i++) {
            char l[160];
            snprintf(l, sizeof l, "#%s  %d", tags[i], tag_count[i]);
            int cw = text_w(l, (int)strlen(l), F_REG, sp) + U(22);
            if (x + cw > cx1) { x = cx; y += U(32); }
            x = chip(x, y, l, tag_on && !strcmp(tag_on, tags[i]), Z_NV_TAG, i, NULL) + U(6);
        }
        y += U(40);
    }
    y += U(6);

    if (!nnv)
        y += ui_wrap(cx, y, w, "No notes yet. Select words while reading to highlight them, add a note, or bring your "
                     "Gospel Library notes in from ⚙ → Church Account.", F_REG, px, C_DIM, 0);
    else if (!nshown)
        y += ui_wrap(cx, y, w, "No note matches.", F_REG, px, C_DIM, 0);

    /* the cards */
    int pad = U(16), iw = w - 2 * pad - U(8);
    for (int s = 0; s < nshown; s++) {
        nv_t *n = &nv[shown[s]];
        char date[32];
        time_t tt = (time_t)n->updated;
        struct tm tm;
        localtime_r(&tt, &tm);
        strftime(date, sizeof date, "%Y-%m-%d", &tm);
        char *tagline = NULL;
        if (n->ntags) {
            buf_t b = {0};
            for (int k = 0; k < n->ntags; k++) buf_printf(&b, "%s#%s", k ? "  " : "", n->tags[k]);
            tagline = buf_take(&b);
        }
        int h = pad + U(30);
        if (*n->quote) h += ui_wrap_h(iw, n->quote, F_ITALIC, rp, 6) + U(8);
        if (*n->title) h += ui_wrap_h(iw, n->title, F_BOLD, px, 0) + U(6);
        if (*n->content) h += ui_wrap_h(iw, n->content, F_REG, px, 0) + U(6);
        if (tagline) h += sp * 145 / 100 + U(4);
        h += pad - U(4);
        if (y + h >= y0 && y <= y1) {
            int hov = hovered(cx, y, cx1, y + h);
            draw_rect(cx, y, cx1, y + h, U(10), hov ? C_BAR : C_GLASS);
            draw_frame(cx, y, cx1, y + h, U(10), 1, hov ? C_ACC : C_LINE);
            zone(cx, MAX(y, y0), cx1, MIN(y + h, y1), Z_NV_CARD, shown[s], NULL, CUR_HAND);
            if (n->color >= 0) draw_rect(cx + U(6), y + U(10), cx + U(11), y + h - U(10), U(2), hl_color(n->color));
            int tx = cx + pad + U(8), ty = y + pad;
            int rw = text_w(n->ref, (int)strlen(n->ref), F_BOLD, px);
            int rhov = hovered(tx, ty - U(2), tx + rw, ty + px + U(4));
            ui_text(tx, ty - U(2), n->ref, F_BOLD, px, C_ACCT);
            if (rhov) draw_rect(tx, ty + px + U(1), tx + rw, ty + px + U(2), 0, C_ACCT);
            if (*n->uri) zone(tx, MAX(ty - U(2), y0), tx + rw, MIN(ty + px + U(4), y1), Z_NV_REF, shown[s], NULL, CUR_HAND);
            int dw = text_w(date, (int)strlen(date), F_REG, sp), dx = cx1 - pad - dw;
            ui_text(dx, ty, date, F_REG, sp, C_DIM);
            if (*n->uri) {                                   /* the way to the verse */
                const char *go = "Go to verse →";
                int gh = U(28), gw = ui_button_w(go, gh);
                ui_button(dx - U(14) - gw, ty - U(6), gh, go, BTN_PLAIN, Z_NV_GO, shown[s], NULL);
            }
            ty += U(30);
            if (*n->quote) ty += ui_wrap(tx, ty, iw, n->quote, F_ITALIC, rp, C_TXT, 6) + U(8);
            if (*n->title) ty += ui_wrap(tx, ty, iw, n->title, F_BOLD, px, C_TXT, 0) + U(6);
            if (*n->content) ty += ui_wrap(tx, ty, iw, n->content, F_REG, px, C_TERMFG, 0) + U(6);
            if (tagline) ui_text(tx, ty, tagline, F_REG, sp, C_ACCT);
        }
        free(tagline);
        y += h + U(12);
    }
    int content = y - top + U(40), view = y1 - y0;
    int max = MAX(0, content - view);
    if (t->scroll > max || t->scroll < 0) { t->scroll = CLAMP(t->scroll, 0, max); A.dirty = 1; }
    draw_unclip();
}

void notes_view_click(const zone_t *z, int ctrl)
{
    switch (z->id) {
    case Z_NV_REF:
    case Z_NV_GO:
        if (z->a < nnv && *nv[z->a].uri) {
            /* in the note's language when that chapter is here, else in the reading language
             * (when it is there), else the note's (fetched) */
            const char *lang = nv[z->a].lang;
            if (!store_has_chapter(lang, nv[z->a].uri) && store_has_chapter(A.lang, nv[z->a].uri)) lang = A.lang;
            jump_to(lang, nv[z->a].uri, nv[z->a].anchor, ctrl);
        }
        break;
    case Z_NV_CARD: if (z->a < nnv) open_note(nv[z->a].id); break;
    case Z_NV_TAG:
        if (z->a < ntags) {
            int same = tag_on && !strcmp(tag_on, tags[z->a]);
            free(tag_on);
            tag_on = same ? NULL : xstrdup(tags[z->a]);
        }
        break;
    case Z_NV_CLEAR: free(tag_on); tag_on = NULL; break;
    case Z_NV_SORT: by_book = z->a; break;
    }
    A.dirty = 1;
}
