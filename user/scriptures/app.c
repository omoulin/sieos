/*
 * app.c - Scriptures: the window, its tabs, the reader, input, the main loop.
 *
 *   ┌──────────────────────────────────────────────────────────────────────┐
 *   │ [☰ Library] [1 Nephi 3  ×][John 3  ×] [+]                  ← ↑ ↓ ⚙  │ the top bar
 *   │ ┌─────────────┐ ┌───────────────────────────────────────────────────┐ │
 *   │ │ the library │ │                 Chapter 3                         │ │
 *   │ │ (sidebar)   │ │  1 And it came to pass that I, Nephi, returned... │ │ the reader
 *   │ └─────────────┘ └───────────────────────────────────────────────────┘ │
 *   └──────────────────────────────────────────────────────────────────────┘
 *
 * Drawn in Atlas's looks (draw.c); Phosphor by default.
 */
#include "app.h"
#include "font.h"
#include "linux_import.h"
#include "net.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

app_t A;
static uint32_t *pic;                 /* the picture (palette colours), W x H */
static int pic_n;

/* ---- small things ------------------------------------------------------------------- */

void toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.toast, sizeof A.toast, fmt, ap);
    va_end(ap);
    A.toast_until = plat_ms() + 3200;
    A.dirty = 1;
}

tab_t *cur_tab(void) { return A.ntabs ? A.tabs[A.cur] : NULL; }

static void tab_unload(tab_t *t)
{
    layout_free(t->lay); t->lay = NULL;              /* (first: it reads the chapter's paragraphs) */
    chapter_free(t->ch); t->ch = NULL;
    free(t->error); t->error = NULL;
    t->sel0 = t->sel1 = 0;
}

static void tab_free(tab_t *t)
{
    tab_unload(t);
    free(t->uri); free(t->title); free(t->goto_anchor); free(t->flash_anchor);
    for (int i = 0; i < t->nhist; i++) { free(t->hist[i].uri); free(t->hist[i].anchor); }
    free(t);
}

static void set_str(char **p, const char *s) { char *n = s ? xstrdup(s) : NULL; free(*p); *p = n; }

/* The flat reading order (for previous / next chapter). */
static const char **order; static int norder, order_cap;
static void order_walk(const toc_t *t)
{
    for (int i = 0; i < t->nkids; i++) {
        if (t->kids[i].uri) {
            if (norder == order_cap) { order_cap = order_cap * 2 + 512; order = xrealloc(order, order_cap * sizeof *order); }
            order[norder++] = t->kids[i].uri;
        }
        order_walk(&t->kids[i]);
    }
}
static int order_index(const char *uri)
{
    for (int i = 0; uri && i < norder; i++) if (!strcmp(order[i], uri)) return i;
    return -1;
}
int order_of(const char *uri) { return order_index(uri); }

static const char *find_title(const toc_t *t, const char *uri)
{
    for (int i = 0; i < t->nkids; i++) {
        if (t->kids[i].uri && !strcmp(t->kids[i].uri, uri)) return t->kids[i].title;
        const char *r = find_title(&t->kids[i], uri);
        if (r) return r;
    }
    return NULL;
}
const char *leaf_title(const char *uri)
{
    for (int v = 0; uri && A.vols && v < A.vols->n; v++) {
        const char *r = find_title(A.vols->v[v].toc, uri);
        if (r) return r;
    }
    return NULL;
}

void reload_volumes(void)
{
    store_volumes_free(A.vols);
    A.vols = store_volumes(A.lang);
    norder = 0;
    for (int v = 0; v < A.vols->n; v++) order_walk(A.vols->v[v].toc);
    for (int i = 0; i < A.npages; i++) { free(A.pages[i].title); hits_free(A.pages[i].hits, A.pages[i].nhits); }
    A.npages = 0;
    side_push(PG_ROOT, NULL, "Library");
    A.dirty = 1;
}

void set_lang(const char *lang)
{
    if (!lang || !*lang || !strcmp(lang, A.lang)) return;
    snprintf(A.lang, sizeof A.lang, "%s", lang);
    setting_set("lang", lang);
    reload_volumes();
}

/* ---- loading chapters (a page not downloaded: fetched on a thread) ------------------------ */

typedef struct { char lang[16]; char *uri; int ok; char *err; } fetched_t;
static fetched_t done_q[32];
static int ndone;
static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;

static void *fetch_thread(void *arg)
{
    fetched_t *f = arg;
    f->ok = fetch_chapter(f->lang, f->uri, &f->err) == 0;
    pthread_mutex_lock(&q_mu);
    if (ndone < COUNT(done_q)) done_q[ndone++] = *f; else { free(f->uri); free(f->err); }
    pthread_mutex_unlock(&q_mu);
    free(f);
    plat_wake();
    return NULL;
}

static void tab_load(tab_t *t)
{
    tab_unload(t);
    if (!t->uri || is_notes_tab(t)) return;
    t->ch = store_chapter(t->lang, t->uri);
    if (t->ch) {
        set_str(&t->title, t->ch->title);
        t->loading = 0;
        return;
    }
    t->loading = 1;
    fetched_t *f = xcalloc(1, sizeof *f);
    snprintf(f->lang, sizeof f->lang, "%s", t->lang);
    f->uri = xstrdup(t->uri);
    pthread_t th;
    pthread_create(&th, NULL, fetch_thread, f);
    pthread_detach(th);
}

static void fetched(void)
{
    pthread_mutex_lock(&q_mu);
    for (int i = 0; i < ndone; i++) {
        for (int k = 0; k < A.ntabs; k++) {
            tab_t *t = A.tabs[k];
            if (!t->loading || !t->uri || strcmp(t->uri, done_q[i].uri) || strcmp(t->lang, done_q[i].lang)) continue;
            t->loading = 0;
            if (done_q[i].ok) tab_load(t);
            else { free(t->error); t->error = xstrdup(done_q[i].err ? done_q[i].err : "not available"); }
        }
        free(done_q[i].uri); free(done_q[i].err);
    }
    ndone = 0;
    pthread_mutex_unlock(&q_mu);
    A.dirty = 1;
}

/* ---- tabs and places ------------------------------------------------------------------------ */

void tab_new(int select)
{
    if (A.ntabs == MAXTABS) { toast("Too many tabs"); return; }
    tab_t *t = xcalloc(1, sizeof *t);
    snprintf(t->lang, sizeof t->lang, "%s", A.lang);
    t->title = xstrdup("New Tab");
    A.tabs[A.ntabs++] = t;
    if (select) A.cur = A.ntabs - 1;
    A.dirty = 1;
}

static void tab_close(int i)
{
    if (i < 0 || i >= A.ntabs) return;
    if (A.ntabs == 1) {                                  /* the last one: emptied, not closed */
        tab_t *t = A.tabs[0];
        tab_unload(t);
        set_str(&t->uri, NULL);
        set_str(&t->title, "New Tab");
        t->nhist = 0;
    } else {
        tab_free(A.tabs[i]);
        memmove(A.tabs + i, A.tabs + i + 1, (A.ntabs - i - 1) * sizeof *A.tabs);
        A.ntabs--;
        if (A.cur > i || A.cur >= A.ntabs) A.cur = MAX(0, A.cur - 1);
    }
    close_popover();
    save_session();
    A.dirty = 1;
}

static const char *top_anchor(tab_t *t) { return t->lay ? layout_anchor_at(t->lay, t->scroll) : NULL; }

void open_place(const char *lang_in, const char *uri, const char *anchor, int new_tab, int history)
{
    if (!uri) return;
    char lang[16];                                       /* (lang may be a tab's own) */
    snprintf(lang, sizeof lang, "%s", lang_in);
    char *u = uri_normalize(uri);
    if (strcmp(lang, A.lang)) set_lang(lang);
    if (new_tab) {
        tab_new(0);
        tab_t *t = A.tabs[A.ntabs - 1];
        snprintf(t->lang, sizeof t->lang, "%s", lang);
        t->uri = u;
        set_str(&t->goto_anchor, anchor);
        char *title = store_chapter_title(lang, u);
        set_str(&t->title, title ? title : "…");
        free(title);
        toast("Opened in a new tab");
        save_session();
        return;
    }
    tab_t *t = cur_tab();
    close_popover();
    if (t->uri && !strcmp(t->uri, u) && !strcmp(t->lang, lang) && t->ch) {   /* the same page: scroll */
        if (anchor) { set_str(&t->goto_anchor, anchor); set_str(&t->flash_anchor, anchor); t->flash_until = plat_ms() + 1600; }
        free(u);
        A.dirty = 1;
        return;
    }
    if (history && t->uri) {
        if (t->nhist == MAXHIST) { free(t->hist[0].uri); free(t->hist[0].anchor); memmove(t->hist, t->hist + 1, (MAXHIST - 1) * sizeof *t->hist); t->nhist--; }
        place_t *p = &t->hist[t->nhist++];
        memcpy(p->lang, t->lang, sizeof p->lang);
        p->uri = xstrdup(t->uri);
        p->anchor = xstrdup(top_anchor(t));
    }
    snprintf(t->lang, sizeof t->lang, "%s", lang);
    free(t->uri);
    t->uri = u;
    t->scroll = 0;
    set_str(&t->goto_anchor, anchor);
    set_str(&t->flash_anchor, history ? anchor : NULL);
    t->flash_until = plat_ms() + 1600;
    tab_load(t);
    setting_set("last", t->uri);
    save_session();
    A.dirty = 1;
}

/* Show a verse: the tab already open on its chapter, else a new tab (behind: not switched to). */
void jump_to(const char *lang, const char *uri, const char *anchor, int background)
{
    for (int i = 0; !background && i < A.ntabs; i++) {
        tab_t *t = A.tabs[i];
        if (!t->uri || is_notes_tab(t) || strcmp(t->uri, uri) || strcmp(t->lang, lang)) continue;
        A.cur = i;
        set_str(&t->goto_anchor, anchor);
        set_str(&t->flash_anchor, anchor);
        t->flash_until = plat_ms() + 1600;
        save_session();
        A.dirty = 1;
        return;
    }
    open_place(lang, uri, anchor, 1, 1);
    if (!background) {
        tab_t *t = A.tabs[A.ntabs - 1];
        A.cur = A.ntabs - 1;
        set_str(&t->flash_anchor, anchor);
        t->flash_until = plat_ms() + 1600;
        A.toast[0] = 0;                                  /* (no "opened in a new tab": it is showing) */
        save_session();
    }
}

static void go_back(void)
{
    tab_t *t = cur_tab();
    if (!t || !t->nhist) return;
    place_t p = t->hist[--t->nhist];
    open_place(p.lang, p.uri, p.anchor, 0, 0);
    free(p.uri); free(p.anchor);
}

static void step_chapter(int d)
{
    tab_t *t = cur_tab();
    int i = t ? order_index(t->uri) : -1;
    if (i >= 0 && i + d >= 0 && i + d < norder) open_place(t->lang, order[i + d], NULL, 0, 0);
}

void follow_link(const char *href, int new_tab)
{
    char *lang, *uri, *anchor, *url;
    if (link_resolve(href, A.lang, &lang, &uri, &anchor, &url)) open_place(lang, uri, anchor, new_tab, 1);
    else plat_open_url(url);
    free(lang); free(uri); free(anchor); free(url);
}

void save_session(void)
{
    buf_t b = {0};
    for (int i = 0; i < A.ntabs; i++) {
        tab_t *t = A.tabs[i];
        if (!t->uri) continue;
        const char *a = t->lay ? top_anchor(t) : t->goto_anchor;
        buf_printf(&b, "%s%s|%s|%s", b.n ? ";" : "", t->lang, t->uri, a ? a : "");
    }
    setting_set("session", b.p ? b.p : "");
    setting_set_int("session_cur", A.cur);
    buf_free(&b);
}

static void restore_session(void)
{
    const char *s = setting("session", "");
    char *copy = xstrdup(s);
    char *rest = copy;
    for (char *e; (e = tok(&rest, ";")); ) {
        char *p1 = strchr(e, '|'), *p2 = p1 ? strchr(p1 + 1, '|') : NULL;
        if (!p2) continue;
        *p1 = *p2 = 0;
        if (!strcmp(p1 + 1, NOTES_URI)) {
            tab_new(0);
            tab_t *t = A.tabs[A.ntabs - 1];
            t->uri = xstrdup(NOTES_URI);
            free(t->title);
            t->title = xstrdup("Notes & Highlights");
            continue;
        }
        if (!store_has_chapter(e, p1 + 1)) continue;
        tab_new(0);
        tab_t *t = A.tabs[A.ntabs - 1];
        snprintf(t->lang, sizeof t->lang, "%s", e);
        t->uri = xstrdup(p1 + 1);
        if (p2[1]) t->goto_anchor = xstrdup(p2 + 1);
        char *title = store_chapter_title(t->lang, t->uri);
        set_str(&t->title, title ? title : t->uri);
        free(title);
    }
    free(copy);
    if (!A.ntabs) tab_new(1);
    A.cur = CLAMP(setting_int("session_cur", 0), 0, A.ntabs - 1);
}

/* ---- notes on the page --------------------------------------------------------------------- */

typedef struct { int c0, c1; char *id; } nreg_t;
static nreg_t regs[512];
static int nregs;

const char *COLOR_NAMES[8] = { "yellow", "green", "blue", "red", "purple", "orange", "pink", "gray" };
int note_color_index(const char *c)
{
    if (!c) return -1;
    static const struct { const char *alias; int i; } al[] = { {"dark_blue", 2}, {"brown", 5}, {"grey", 7} };
    for (int i = 0; i < 8; i++) if (!strcmp(c, COLOR_NAMES[i])) return i;
    for (int i = 0; i < COUNT(al); i++) if (!strcmp(c, al[i].alias)) return al[i].i;
    return -1;
}

static int page_marks(tab_t *t, mark_t *m, int max)
{
    int nm = 0;
    for (int i = 0; i < nregs; i++) free(regs[i].id);
    nregs = 0;
    if (!t->lay || !t->uri) return 0;
    if (t->flash_anchor && plat_ms() < t->flash_until)
        for (int b = 0; b < t->lay->doc->n && nm < max; b++)
            if (t->lay->doc->b[b].anchor && !strcmp(t->lay->doc->b[b].anchor, t->flash_anchor))
                m[nm++] = (mark_t){ t->lay->bi[b].c0, t->lay->bi[b].c1, C_BAR, 0 };
    notes_lock();
    int n;
    note_t **all = notes_all(&n);
    for (int i = 0; i < n; i++) {
        note_t *no = all[i];
        if (no->deleted || strcmp(no->uri, t->uri)) continue;
        int ci = note_color_index(no->color);
        int marked = *no->content || *no->title || no->ntags || ci < 0;
        for (int s = 0; s < no->nspans && nm < max; s++) {
            int c0, c1;
            span_t sp = no->spans[s];
            if (strcmp(no->lang, t->lang)) sp.start = sp.end = -1;   /* (words count in the note's language) */
            if (!layout_span_range(t->lay, &sp, &c0, &c1)) continue;
            m[nm++] = (mark_t){ c0, c1, ci >= 0 ? hl_color(ci) : 0, marked };
            if (nregs < COUNT(regs)) regs[nregs++] = (nreg_t){ c0, c1, xstrdup(no->id) };
        }
    }
    notes_unlock();
    if (t->sel0 != t->sel1 && nm < max) m[nm++] = (mark_t){ MIN(t->sel0, t->sel1), MAX(t->sel0, t->sel1), C_SEL, 0 };
    return nm;
}

static const char *note_at(int off)
{
    for (int i = nregs - 1; i >= 0; i--) if (off >= regs[i].c0 && off < regs[i].c1) return regs[i].id;
    return NULL;
}

void refresh_notes(void) { A.dirty = 1; }

/* ---- drawing --------------------------------------------------------------------------------- */

static int icon_button(int x, int y, int s, const char *glyph, int id, int a, int enabled)
{
    int hov = enabled && hovered(x, y, x + s, y + s);
    if (hov) draw_rect(x, y, x + s, y + s, U(6), C_BAR);
    int px = U(16), w = text_w(glyph, (int)strlen(glyph), F_REG, px);
    draw_text(x + (s - w) / 2, y + (s - px) / 2 + font_ascent(px) - U(1), glyph, (int)strlen(glyph), F_REG, px,
              enabled ? (hov ? C_TXT : C_DIM) : C_LINE);
    if (enabled) zone(x, y, x + s, y + s, id, a, NULL, CUR_HAND);
    return x + s;
}

static void draw_topbar(void)
{
    int th = U(32), y = A.top_h - th, px = ui_px(), x = U(8);
    int narrow = A.W < U(720);
    /* the app's own tab: the library */
    const char *app = narrow ? "☰" : "☰  Library";
    int aw = text_w(app, (int)strlen(app), F_BOLD, px) + U(28), hov = hovered(x, y, x + aw, y + th);
    draw_top_round(x, y, x + aw, A.top_h, U(8), A.sidebar ? C_GLASS : hov ? C_BAR : C_GLASS);
    draw_text(x + U(14), y + (th - px) / 2 + font_ascent(px) - U(1), app, (int)strlen(app), F_BOLD, px, A.sidebar ? C_ACCT : C_TXT);
    zone(x, y, x + aw, A.top_h, Z_APPTAB, 0, NULL, CUR_HAND);
    x += aw + U(10);

    int s = U(30), ry = y + (th - s) / 2;
    int controls_x = A.W - U(8);
    /* the tabs: as wide as their titles, all narrower when crowded */
    int avail = controls_x - U(16) - x - s;     /* (the tabs may use the whole bar) */
    int minw = narrow ? U(44) : U(70);
    int want = U(230), tw = MAX(minw, MIN(want, A.ntabs ? (avail - U(4) * A.ntabs) / A.ntabs : want));
    for (int i = 0; i < A.ntabs; i++) {
        tab_t *k = A.tabs[i];
        const char *title = k->title ? k->title : "New Tab";
        int w = MIN(tw, text_w(title, (int)strlen(title), F_BOLD, px) + U(56));
        w = MAX(w, minw);
        int on = i == A.cur, h = hovered(x, y, x + w, A.top_h);
        draw_top_round(x, y + (on ? 0 : U(3)), x + w, A.top_h, U(8), on ? C_ACC : h ? C_BAR : C_GLASS);
        char shown[256];
        snprintf(shown, sizeof shown, "%s%s", k->loading ? "… " : "", title);
        draw_text_clipped(x + U(14), y + (th - px) / 2 + font_ascent(px) - U(1) + (on ? 0 : U(2)), w - U(44), shown,
                          on ? F_BOLD : F_REG, px, on ? C_INK : C_DIM);
        zone(x, y, x + w, A.top_h, Z_TAB, i, NULL, CUR_HAND);
        if (on || h) {
            int cx = x + w - U(26), cy = y + (th - U(20)) / 2 + (on ? 0 : U(2));
            int ch = hovered(cx, cy, cx + U(20), cy + U(20));
            if (ch) draw_rect(cx, cy, cx + U(20), cy + U(20), U(5), on ? C_ACCT : C_LINE);
            draw_text(cx + U(4), cy + U(15), "✕", 3, F_REG, U(12), on ? C_INK : C_TXT);
            zone(cx, cy, cx + U(20), cy + U(20), Z_TABCLOSE, i, NULL, CUR_HAND);
        }
        x += w + U(4);
    }
    draw_rect(x + U(2), ry, x + U(2) + s, ry + s, U(8), C_GLASS);
    icon_button(x + U(2), ry, s, "+", Z_NEWTAB, 0, A.ntabs < MAXTABS);
}

/* A round button on the page: an arrow (glyph), dim until the pointer comes. */
static void round_button(int x, int y, int d, const char *glyph, int id)
{
    int hov = hovered(x, y, x + d, y + d), px = d * 45 / 100;
    draw_rect(x, y, x + d, y + d, d / 2, hov ? C_BAR : C_GLASS);
    draw_frame(x, y, x + d, y + d, d / 2, 1, hov ? C_ACC : C_LINE);
    int w = text_w(glyph, (int)strlen(glyph), F_BOLD, px);
    draw_text(x + (d - w) / 2, y + (d - px) / 2 + font_ascent(px) - U(1), glyph, (int)strlen(glyph), F_BOLD, px, hov ? C_ACCT : C_DIM);
    zone(x, y, x + d, y + d, id, 0, NULL, CUR_HAND);
}

/* The page's own navigation: previous and next chapter at its sides, back (history) at its
 * top left, and up to the top once scrolled down. */
static void page_controls(tab_t *t, int x0, int y0, int x1, int y1, int max_scroll)
{
    int d = U(44), cy = (y0 + y1) / 2 - d / 2, ci = order_index(t->uri);
    if (ci > 0) round_button(x0 + U(12), cy, d, "‹", Z_PREV);
    if (ci >= 0 && ci < norder - 1) round_button(x1 - U(24) - d, cy, d, "›", Z_NEXT);
    if (t->nhist) {
        const char *b = "← Back";
        ui_button(x0 + U(12), y0 + U(10), U(30), b, BTN_PLAIN, Z_BACK, 0, NULL);
    }
    if (max_scroll > 0 && t->scroll > (y1 - y0) / 2) round_button(x1 - U(24) - d, y1 - U(16) - d, d, "↑", Z_TOP);
}

static void draw_reader(int x0, int y0, int x1, int y1)
{
    tab_t *t = cur_tab();
    draw_rect(x0, y0, x1, y1, U(10), C_TERM);
    draw_frame(x0, y0, x1, y1, U(10), 1, C_LINE);
    draw_rect(x0 + U(10), y0, x1 - U(10), y0 + 1, 0, C_ACC);       /* (the current tab's colour runs on) */
    A.read_x0 = x0; A.read_x1 = x1; A.read_y0 = y0; A.read_y1 = y1;
    if (!t) return;
    int pad = U(30), px = A.reader_px;
    int cw = x1 - x0 - 2 * U(40);
    int maxw = text_w("n", 1, F_REG, px) * 78;
    int w = MAX(U(200), MIN(cw, maxw));
    A.text_w = w;
    A.text_x0 = x0 + (x1 - x0 - w) / 2;
    if (is_notes_tab(t)) { draw_notes_view(t, x0, y0, x1, y1); return; }
    draw_clip(x0 + 1, y0 + 1, x1 - 1, y1 - 1);
    if (t->uri && !t->ch && !t->loading && !t->error) tab_load(t);   /* (a restored tab, first shown) */
    if (!t->uri || t->loading || t->error) {
        int cx = (x0 + x1) / 2, cy = y0 + (y1 - y0) / 3, tpx = ui_title();
        const char *head = t->loading ? "Loading…" : t->error ? "Not available offline" : "Scriptures";
        int hw = text_w(head, (int)strlen(head), F_BOLD, tpx);
        draw_text(cx - hw / 2, cy, head, (int)strlen(head), F_BOLD, tpx, C_ACCT);
        const char *sub = t->error ? t->error : !t->uri ? (A.vols && A.vols->n
                          ? "Choose a chapter in the library. Ctrl+click opens it in a new tab."
                          : "Nothing downloaded yet.") : "";
        int sw = text_w(sub, (int)strlen(sub), F_REG, ui_px());
        draw_text(cx - sw / 2, cy + U(34), sub, (int)strlen(sub), F_REG, ui_px(), C_DIM);
        if (!t->loading && (t->error || !(A.vols && A.vols->n))) {
            const char *b = "Download scriptures…";
            ui_button(cx - ui_button_w(b, U(34)) / 2, cy + U(64), U(34), b, BTN_ACCENT, Z_DOWNLOADS, 0, NULL);
        }
        draw_unclip();
        return;
    }
    if (!t->lay || t->lay_w != w || t->lay_px != px) {
        const char *keep = t->lay ? top_anchor(t) : NULL;
        char *k = keep && !t->goto_anchor ? xstrdup(keep) : NULL;
        layout_free(t->lay);
        t->lay = layout_make(t->ch->doc, w, px, 0);
        t->lay_w = w; t->lay_px = px;
        if (k) { t->goto_anchor = k; }
    }
    int view_h = y1 - y0 - pad;
    if (t->goto_anchor) {
        int ay = layout_anchor_y(t->lay, t->goto_anchor);
        if (ay >= 0) t->scroll = ay - U(18);
        free(t->goto_anchor); t->goto_anchor = NULL;
    }
    int max_scroll = MAX(0, t->lay->height + U(80) - view_h);
    t->scroll = CLAMP(t->scroll, 0, max_scroll);
    mark_t marks[600];
    int nm = page_marks(t, marks, COUNT(marks));
    layout_draw(t->lay, A.text_x0, y0 + pad - t->scroll, y0, y1, marks, nm);
    zone(x0, y0, x1 - U(12), y1, Z_READER, 0, NULL, CUR_TEXT);
    if (max_scroll > 0) {                                   /* the scroll bar */
        int tx0 = x1 - U(10), tx1 = x1 - U(4), ty0 = y0 + U(10), ty1 = y1 - U(10), th = ty1 - ty0;
        int kh = MAX(U(30), th * view_h / (t->lay->height + U(80)));
        int ky = ty0 + (int)((int64_t)(th - kh) * t->scroll / max_scroll);
        draw_rect(tx0, ky, tx1, ky + kh, U(3), hovered(tx0 - U(6), ty0, x1, ty1) ? C_DIM : C_LINE);
        zone(tx0 - U(6), ty0, x1, ty1, Z_SCROLLBAR, 0, NULL, CUR_ARROW);
    }
    if (t->flash_anchor && plat_ms() >= t->flash_until) { free(t->flash_anchor); t->flash_anchor = NULL; }
    page_controls(t, x0, y0, x1, y1, max_scroll);
    draw_unclip();
}

static void draw_toast(void)
{
    if (!A.toast[0] || plat_ms() > A.toast_until) { A.toast[0] = 0; return; }
    int px = ui_px(), w = text_w(A.toast, (int)strlen(A.toast), F_REG, px) + U(36), h = U(36);
    int x = (A.W - w) / 2, y = A.H - U(28) - h;
    draw_rect(x, y, x + w, y + h, h / 2, C_GLASS);
    draw_frame(x, y, x + w, y + h, h / 2, 1, C_ACC);
    draw_text(x + U(18), y + (h - px) / 2 + font_ascent(px) - U(1), A.toast, (int)strlen(A.toast), F_REG, px, C_TXT);
}

static void render(void)
{
    plat_size(&A.W, &A.H);
    if (A.W * A.H > pic_n) { pic_n = A.W * A.H; free(pic); pic = xmalloc((size_t)pic_n * 4); }
    int pitch;
    uint32_t *out = plat_begin(&pitch);
    if (!out) return;
    draw_target(pic, A.W, A.H);
    zones_reset();
    draw_desk(0, 0, A.W, A.H, U(32));
    A.top_h = U(44);
    int gap = U(8), y0 = A.top_h, y1 = A.H - gap;
    int sw = MIN(U(300), A.W * 2 / 5), overlay = A.W < U(860);
    A.side_x0 = gap; A.side_x1 = gap + sw;
    int rx0 = A.sidebar && !overlay ? A.side_x1 + gap : gap;
    draw_reader(rx0, y0, A.W - gap, y1);
    if (A.sidebar) {
        if (overlay) draw_shade(gap, y0, A.W - gap, y1, U(10), 1);
        draw_sidebar(A.side_x0, y0, A.side_x1, y1);
    }
    draw_topbar();
    {                                                        /* settings: the bottom left */
        int d = U(44), sx = U(16), sy = A.H - U(16) - d;
        round_button(sx, sy, d, "⚙", Z_SETTINGS);
    }
    draw_popover();
    draw_dialog();
    if (A.busy) {
        draw_shade(0, 0, A.W, A.H, 0, 2);
        const char *m = "Importing your library…";
        int w = text_w(m, (int)strlen(m), F_BOLD, ui_title());
        draw_text((A.W - w) / 2, A.H / 2, m, (int)strlen(m), F_BOLD, ui_title(), C_ACCT);
    }
    draw_toast();
    draw_present(out, pitch);
    plat_end();
}

/* ---- input ------------------------------------------------------------------------------------ */

static int dragging_sel, dragging_bar, dragging_field, press_off, pressed_tab = -1;
static field_t *drag_field;

static void reader_scroll(int dy) { tab_t *t = cur_tab(); if (t) { t->scroll += dy; A.dirty = 1; } }

static int reader_offset(int x, int y)
{
    tab_t *t = cur_tab();
    return layout_offset_at(t->lay, x - A.text_x0, y - (A.read_y0 + U(30)) + t->scroll);
}
static const piece_t *reader_piece(int x, int y)
{
    tab_t *t = cur_tab();
    return t && t->lay ? layout_piece_at(t->lay, x - A.text_x0, y - (A.read_y0 + U(30)) + t->scroll) : NULL;
}

static void selection_menu(tab_t *t)
{
    int a = MIN(t->sel0, t->sel1), b = MAX(t->sel0, t->sel1);
    span_t sp[2];
    if (!layout_spans(t->lay, a, b, sp, 1)) { t->sel0 = t->sel1 = 0; return; }
    int x, y, h, xe, ye, he;
    layout_pos(t->lay, a, &x, &y, &h);
    layout_pos(t->lay, b, &xe, &ye, &he);
    A.pop = POP_SELECTION;
    A.pop_x = A.text_x0 + (ye == y ? (x + xe) / 2 : x + U(120));   /* (over the selection's first line) */
    A.pop_y = A.read_y0 + U(30) + y - t->scroll;
}

static void reader_down(const ev_t *e)
{
    tab_t *t = cur_tab();
    if (!t || !t->lay) return;
    if (e->button == 2) {                                  /* middle click: a link in a new tab */
        const piece_t *p = reader_piece(e->x, e->y);
        if (p && p->style & ST_LINK) follow_link(t->lay->doc->b[p->blk].runs[p->run].href, 1);
        return;
    }
    if (e->button != 1) return;
    int off = reader_offset(e->x, e->y);
    if (e->clicks == 2) {                                  /* double click: the word */
        for (int b = 0; b < t->lay->doc->n; b++)
            for (int w = 0; w < t->lay->bi[b].nw; w++)
                if (off >= t->lay->bi[b].ws[w] && off < t->lay->bi[b].we[w]) {
                    t->sel0 = t->lay->bi[b].ws[w]; t->sel1 = t->lay->bi[b].we[w];
                    selection_menu(t);
                    A.dirty = 1;
                    return;
                }
    }
    if ((e->mod & M_SHIFT) && t->sel0 != t->sel1) t->sel1 = off;
    else t->sel0 = t->sel1 = off;
    press_off = off;
    dragging_sel = 1;
    A.dirty = 1;
}

static void reader_up(const ev_t *e)
{
    tab_t *t = cur_tab();
    dragging_sel = 0;
    if (!t || !t->lay) return;
    if (t->sel0 != t->sel1) { selection_menu(t); A.dirty = 1; return; }
    /* a click: a footnote letter, a highlight, a footnoted word, a link */
    const piece_t *p = reader_piece(e->x, e->y);
    const run_t *r = p ? &t->lay->doc->b[p->blk].runs[p->run] : NULL;
    if (r && (p->style & ST_MARKER) && r->note) { open_footnote(t, r->note, e->x, e->y); return; }
    const char *nid = p ? note_at(press_off) : NULL;
    if (nid) { open_note(nid); return; }
    if (r && (p->style & ST_FOOTNOTE) && r->note) { open_footnote(t, r->note, e->x, e->y); return; }
    if (r && (p->style & ST_LINK) && r->href) follow_link(r->href, (e->mod & M_CTRL) != 0);
}

static void copy_selection(void)
{
    tab_t *t = cur_tab();
    if (!t || !t->lay || t->sel0 == t->sel1) return;
    int a = MIN(t->sel0, t->sel1), b = MAX(t->sel0, t->sel1);
    char *text = layout_text(t->lay, a, b);
    span_t sp[64];
    int n = layout_spans(t->lay, a, b, sp, 64);
    int v0 = n ? layout_verse_num(t->lay, sp[0].anchor) : 0, v1 = n ? layout_verse_num(t->lay, sp[n - 1].anchor) : 0;
    char *s = v0 ? (v1 != v0 ? xprintf("%s (%s:%d–%d)", text, t->ch->title, v0, v1) : xprintf("%s (%s:%d)", text, t->ch->title, v0))
                 : xprintf("%s (%s)", text, t->ch->title);
    plat_clip_set(s);
    free(s);
    free(text);
    toast("Copied");
}

static void focus(field_t *f) { A.focus = f; A.dirty = 1; }

static void on_down(const ev_t *e)
{
    const zone_t *z = zone_at(e->x, e->y);
    int ctrl = (e->mod & M_CTRL) != 0;
    if (A.pop && (!z || z->id != Z_POPUP) && !(z && (z->id == Z_SWATCH || z->id == Z_POP_NOTE || z->id == Z_POP_COPY || z->id == Z_FNLINK))) {
        int was_sel = A.pop == POP_SELECTION;
        close_popover();
        tab_t *t = cur_tab();
        if (was_sel && t) t->sel0 = t->sel1 = 0;
        if (z && z->id == Z_READER) { A.dirty = 1; return; }   /* (the click only closed it) */
    }
    if (!z) { if (A.focus && !A.dlg) A.focus = NULL; A.dirty = 1; return; }
    int nv_zone = z->id >= Z_NV_REF && z->id <= Z_NV_CLEAR;
    if (A.dlg && (nv_zone || !(z->id >= Z_DLG || z->id == Z_FIELD || z->id == Z_SHADE))) return;   /* (modal) */
    if (z->id != Z_FIELD && !(A.dlg && z->id == Z_DLG)) A.focus = NULL;
    switch (z->id) {
    case Z_FIELD: {
        field_t *f = (field_t *)z->p;
        focus(f);
        field_click(f, e->x, e->y, (e->mod & M_SHIFT) != 0);
        if (e->clicks >= 2) field_select_all(f);
        dragging_field = 1;
        drag_field = f;
        break;
    }
    case Z_APPTAB: A.sidebar = !A.sidebar; setting_set_int("sidebar", A.sidebar); break;
    case Z_TAB:
        if (e->button == 2) { tab_close(z->a); break; }
        A.cur = z->a; close_popover(); pressed_tab = z->a; save_session();
        break;
    case Z_TABCLOSE: tab_close(z->a); break;
    case Z_NEWTAB: tab_new(1); if (A.W < U(860)) A.sidebar = 1; save_session(); break;
    case Z_BACK: go_back(); break;
    case Z_PREV: step_chapter(-1); break;
    case Z_TOP: { tab_t *t = cur_tab(); if (t) t->scroll = 0; break; }
    case Z_NEXT: step_chapter(1); break;
    case Z_SETTINGS: open_settings(SEC_LOOK); break;
    case Z_DOWNLOADS: open_settings(SEC_LIBRARY); break;
    case Z_READER: reader_down(e); break;
    case Z_SCROLLBAR: dragging_bar = 1; break;
    case Z_SWATCH: case Z_POP_NOTE: case Z_POP_COPY: case Z_FNLINK: case Z_POPUP:
        if (z->id == Z_POP_COPY) { copy_selection(); close_popover(); tab_t *t = cur_tab(); if (t) t->sel0 = t->sel1 = 0; }
        else popover_click(z, ctrl);
        break;
    case Z_NV_REF: case Z_NV_GO: case Z_NV_CARD: case Z_NV_TAG: case Z_NV_SORT: case Z_NV_CLEAR: notes_view_click(z, ctrl || e->button == 2); break;
    default:
        if (z->id >= Z_DLG || z->id == Z_SHADE) dialog_click(z, e->clicks);
        else side_click(z, e->button, ctrl || e->button == 2);
    }
    A.dirty = 1;
}

static void on_move(const ev_t *e)
{
    mouse_x = e->x; mouse_y = e->y;
    tab_t *t = cur_tab();
    if (dragging_sel && t && t->lay && e->button) {
        int off = reader_offset(e->x, e->y);
        if (off != t->sel1) { t->sel1 = off; A.dirty = 1; }
        if (e->y > A.read_y1 - U(20)) reader_scroll(U(12));
        if (e->y < A.read_y0 + U(20)) reader_scroll(-U(12));
        return;
    }
    if (dragging_bar && t && t->lay) {
        int th = A.read_y1 - A.read_y0 - U(20), view = A.read_y1 - A.read_y0 - U(30);
        int max_scroll = MAX(1, t->lay->height + U(80) - view);
        t->scroll = (int)((int64_t)(e->y - A.read_y0 - U(10)) * max_scroll / MAX(1, th));
        A.dirty = 1;
        return;
    }
    if (dragging_field && drag_field && e->button) { field_click(drag_field, e->x, e->y, 1); A.dirty = 1; return; }
    const zone_t *z = zone_at(e->x, e->y);
    int cur = z ? z->cursor : CUR_ARROW;
    if (z && z->id == Z_READER) {
        const piece_t *p = reader_piece(e->x, e->y);
        int off = t && t->lay ? reader_offset(e->x, e->y) : 0;
        if (p && (p->style & (ST_MARKER | ST_FOOTNOTE | ST_LINK))) cur = CUR_HAND;
        else if (p && note_at(off)) cur = CUR_HAND;
    }
    plat_cursor(cur);
    static int last_id = -1, last_a = -1;
    static const void *last_p;
    int id = z ? z->id : -1, a = z ? z->a : -1;
    const void *pp = z ? z->p : NULL;
    if (id != last_id || a != last_a || pp != last_p || (z && (z->id == Z_TAB || z->id == Z_SCROLLBAR))) A.dirty = 1;
    last_id = id; last_a = a; last_p = pp;
}

static void on_wheel(const ev_t *e)
{
    const zone_t *z = zone_at(e->x, e->y);
    int dy = -e->wheel * U(48) / 120;
    if (A.pop) close_popover();
    if (z && (z->id == Z_READER || z->id == Z_SCROLLBAR)) { reader_scroll(dy); return; }
    if (z && z->id == Z_FIELD && ((field_t *)z->p)->multiline) { ((field_t *)z->p)->scroll += dy; A.dirty = 1; return; }
    if (A.sidebar && e->x >= A.side_x0 && e->x < A.side_x1 && !A.dlg && A.npages) {
        A.pages[A.npages - 1].scroll += dy;
        A.dirty = 1;
        return;
    }
    if (!A.dlg) reader_scroll(dy);
}

static void zoom_text(int d)
{
    A.reader_px = d ? CLAMP(A.reader_px + d * U(1), U(11), U(32)) : U(17);
    setting_set_int("text_size", A.reader_px * 100 / UI_S);
    A.dirty = 1;
}

static int on_key(const ev_t *e)
{
    int ctrl = e->mod & M_CTRL, shift = e->mod & M_SHIFT, alt = e->mod & M_ALT;
    tab_t *t = cur_tab();
    if (e->key == K_ESC) {
        if (A.pop) { close_popover(); if (t) t->sel0 = t->sel1 = 0; }
        else if (A.dlg) close_dialog();
        else if (A.focus) A.focus = NULL;
        else if (t) t->sel0 = t->sel1 = 0;
        A.dirty = 1;
        return 1;
    }
    if (ctrl && !alt) {
        switch (e->key) {
        case 'q': save_session(); exit(0);
        case 't': if (!A.dlg) { tab_new(1); if (A.W < U(860)) A.sidebar = 1; save_session(); } return 1;
        case 'w': if (!A.dlg) tab_close(A.cur); return 1;
        case K_TAB: case K_PGDN: case K_PGUP:
            if (A.dlg || !A.ntabs) return 1;
            A.cur = (A.cur + (e->key == K_PGUP || (e->key == K_TAB && shift) ? A.ntabs - 1 : 1)) % A.ntabs;
            close_popover(); save_session(); A.dirty = 1;
            return 1;
        case 'f': if (!A.dlg) { A.sidebar = 1; while (A.npages > 1) side_click(&(zone_t){ .id = Z_SIDEBACK }, 1, 0); focus(&A.search); } return 1;
        case 'l': A.sidebar = !A.sidebar; A.dirty = 1; return 1;
        case 'n': if (shift && !A.dlg) open_notes_tab(); return 1;
        case '+': case '=': zoom_text(1); return 1;
        case '-': zoom_text(-1); return 1;
        case '0': zoom_text(0); return 1;
        case ',': open_settings(SEC_LOOK); return 1;
        case 'c': if (!A.focus) { copy_selection(); return 1; } break;
        }
        if (e->key >= '1' && e->key <= '9' && !A.dlg) { int i = e->key - '1'; if (i < A.ntabs) { A.cur = i; A.dirty = 1; } return 1; }
    }
    if (alt && !A.dlg) {
        if (e->key == K_LEFT) { go_back(); return 1; }
        if (e->key == K_UP) { step_chapter(-1); return 1; }
        if (e->key == K_DOWN) { step_chapter(1); return 1; }
    }
    if (A.focus) {
        int r = field_key(A.focus, e);
        if (r == 2 && A.focus == &A.search) side_search(field_str(&A.search));
        else if (r == 2) dialog_click(&(zone_t){ .id = Z_FIELD, .p = A.focus }, 1);   /* Enter in a dialog's field */
        A.dirty = 1;
        if (r) return 1;
    }
    if (A.dlg) return 0;
    int view = A.read_y1 - A.read_y0 - U(60);
    switch (e->key) {
    case K_DOWN: reader_scroll(U(40)); return 1;
    case K_UP: reader_scroll(-U(40)); return 1;
    case K_PGDN: case ' ': reader_scroll(shift ? -view : view); return 1;
    case K_PGUP: reader_scroll(-view); return 1;
    case K_HOME: if (t) { t->scroll = 0; A.dirty = 1; } return 1;
    case K_END: if (t) { t->scroll = INT_MAX / 2; A.dirty = 1; } return 1;
    case K_RIGHT: step_chapter(1); return 1;
    case K_LEFT: step_chapter(-1); return 1;
    }
    return 0;
}

/* ---- start ---------------------------------------------------------------------------------- */

static void import_progress(const char *m, void *ctx) { (void)m; (void)ctx; }
static void *import_thread(void *arg)
{
    (void)arg;
    import_python_library(import_progress, NULL);
    setting_set("imported_py", "1");
    A.busy = 0;
    plat_wake();
    return NULL;
}

static char *data_dir(void)
{
    const char *env = getenv("SCRIPTURES_DATA");
    if (env && *env) return xstrdup(env);
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            char *d = xprintf("%s/../data", exe);              /* (build/scriptures -> data/) */
            if (file_exists(d)) return d;
            free(d);
        }
    }
    const char *home = getenv("XDG_DATA_HOME");
    return home && *home ? xprintf("%s/scriptures", home) : xprintf("%s/.local/share/scriptures", getenv("HOME") ? getenv("HOME") : ".");
}

static void wake(void) { plat_wake(); }

/* ---- a test script (--script FILE): synthetic input through the same handlers, and PNG
 * snapshots of the screen (with SDL_VIDEODRIVER=offscreen, no display is needed) ---------- */

static uint32_t crc_tab[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320 ^ v >> 1 : v >> 1; crc_tab[i] = v; }
    c ^= 0xffffffff;
    while (n--) c = crc_tab[(c ^ *p++) & 255] ^ c >> 8;
    return c ^ 0xffffffff;
}
static void be32(buf_t *b, uint32_t v) { uint8_t x[4] = { v >> 24, v >> 16 & 255, v >> 8 & 255, v & 255 }; buf_add(b, x, 4); }
static void chunk(buf_t *png, const char *type, const uint8_t *d, size_t n)
{
    be32(png, (uint32_t)n);
    size_t at = png->n;
    buf_add(png, type, 4);
    if (n) buf_add(png, d, n);
    be32(png, crc(0, (uint8_t *)png->p + at, n + 4));
}
static void snap(const char *path)
{
    uint32_t *out = xmalloc((size_t)A.W * A.H * 4);
    draw_present(out, A.W);
    buf_t raw = {0}, z = {0}, png = {0};
    for (int y = 0; y < A.H; y++) {
        buf_putc(&raw, 0);
        for (int x = 0; x < A.W; x++) { uint32_t c = out[(size_t)y * A.W + x]; uint8_t px[3] = { c >> 16, c >> 8 & 255, c & 255 }; buf_add(&raw, px, 3); }
    }
    free(out);
    buf_add(&z, "\x78\x01", 2);                                 /* zlib, stored blocks */
    uint32_t a = 1, b2 = 0;
    for (size_t i = 0; i < raw.n; i++) { a = (a + (uint8_t)raw.p[i]) % 65521; b2 = (b2 + a) % 65521; }
    for (size_t off = 0; off < raw.n || !off;) {
        size_t n = MIN(raw.n - off, 65535);
        uint8_t h[5] = { off + n >= raw.n, n & 255, n >> 8, ~n & 255, (~n >> 8) & 255 };
        buf_add(&z, h, 5);
        buf_add(&z, raw.p + off, n);
        off += n;
        if (!n) break;
    }
    be32(&z, b2 << 16 | a);
    buf_add(&png, "\x89PNG\r\n\x1a\n", 8);
    uint8_t ihdr[13];
    uint32_t w = A.W, h = A.H;
    ihdr[0] = w >> 24; ihdr[1] = w >> 16; ihdr[2] = w >> 8; ihdr[3] = w;
    ihdr[4] = h >> 24; ihdr[5] = h >> 16; ihdr[6] = h >> 8; ihdr[7] = h;
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(&png, "IHDR", ihdr, 13);
    chunk(&png, "IDAT", (uint8_t *)z.p, z.n);
    chunk(&png, "IEND", NULL, 0);
    write_file(path, png.p, png.n, 0644);
    buf_free(&raw); buf_free(&z); buf_free(&png);
}

static void run_script(const char *path, const char *outdir)
{
    char *text = read_file(path, NULL);
    if (!text) { fprintf(stderr, "no script %s\n", path); exit(2); }
    char *rest = text;
    for (char *line; (line = tok(&rest, "\n")); ) {
        while (A.busy) { ev_t w; if (plat_wait(&w, 100) && w.type == EV_WAKE) fetched(); }
        for (int i = 0; i < 40; i++) {                             /* (let background work land) */
            ev_t w;
            if (!plat_wait(&w, 5)) break;
            if (w.type == EV_WAKE) { fetched(); jobs_tick(); }
        }
        render();
        char cmd[32] = "", a1[512] = "", a2[512] = "", a3[512] = "", a4[64] = "";
        int n = sscanf(line, "%31s %511s %511s %511s %63s", cmd, a1, a2, a3, a4);
        if (n < 1 || cmd[0] == '#') continue;
        ev_t e = {0};
        if (!strcmp(cmd, "open")) open_place(a1, a2, n > 3 ? a3 : NULL, 0, 1);
        else if (!strcmp(cmd, "newtab")) open_place(a1, a2, n > 3 ? a3 : NULL, 1, 1);
        else if (!strcmp(cmd, "click") || !strcmp(cmd, "dclick") || !strcmp(cmd, "mclick")) {
            e.type = EV_DOWN; e.x = atoi(a1); e.y = atoi(a2); e.button = cmd[0] == 'm' ? 2 : 1;
            e.clicks = cmd[0] == 'd' ? 2 : 1; e.mod = !strcmp(a3, "ctrl") ? M_CTRL : 0;
            mouse_x = e.x; mouse_y = e.y;
            on_down(&e);
            render();
            e.type = EV_UP;
            if (dragging_sel) reader_up(&e);
            dragging_sel = dragging_bar = dragging_field = 0;
        } else if (!strcmp(cmd, "drag")) {
            e.type = EV_DOWN; e.x = atoi(a1); e.y = atoi(a2); e.button = 1; e.clicks = 1;
            mouse_x = e.x; mouse_y = e.y;
            on_down(&e);
            e.type = EV_MOVE; e.x = atoi(a3); e.y = atoi(a4); e.button = 1;
            on_move(&e);
            e.type = EV_UP;
            if (dragging_sel) reader_up(&e);
            dragging_sel = dragging_bar = dragging_field = 0;
        } else if (!strcmp(cmd, "move")) { e.type = EV_MOVE; e.x = atoi(a1); e.y = atoi(a2); on_move(&e); }
        else if (!strcmp(cmd, "wheel")) { e.type = EV_WHEEL; e.x = atoi(a1); e.y = atoi(a2); e.wheel = atoi(a3) * 120; on_wheel(&e); }
        else if (!strcmp(cmd, "key")) {
            char *k = a1;
            for (;;) {
                if (!strncmp(k, "ctrl+", 5)) { e.mod |= M_CTRL; k += 5; }
                else if (!strncmp(k, "alt+", 4)) { e.mod |= M_ALT; k += 4; }
                else if (!strncmp(k, "shift+", 6)) { e.mod |= M_SHIFT; k += 6; }
                else break;
            }
            static const struct { const char *n; int k; } names[] = {
                {"esc", K_ESC}, {"enter", K_ENTER}, {"tab", K_TAB}, {"up", K_UP}, {"down", K_DOWN}, {"left", K_LEFT},
                {"right", K_RIGHT}, {"pgup", K_PGUP}, {"pgdn", K_PGDN}, {"home", K_HOME}, {"end", K_END}, {"backspace", K_BACKSPACE} };
            e.type = EV_KEY;
            e.key = (unsigned char)k[0];
            for (int i = 0; i < COUNT(names); i++) if (!strcmp(k, names[i].n)) e.key = names[i].k;
            on_key(&e);
        } else if (!strcmp(cmd, "type")) {
            const char *t = strchr(line, ' ');
            if (t && A.focus) field_insert(A.focus, t + 1);
        } else if (!strcmp(cmd, "wait")) {
            uint32_t until = plat_ms() + (uint32_t)atoi(a1);
            while (plat_ms() < until) { ev_t w; if (plat_wait(&w, 20) && w.type == EV_WAKE) { fetched(); jobs_tick(); } }
        } else if (!strcmp(cmd, "snap")) {
            render();
            char *p = xprintf("%s/%s.png", outdir, a1);
            snap(p);
            free(p);
        } else if (!strcmp(cmd, "dump")) {
            tab_t *t = cur_tab();
            int nn;
            notes_lock(); notes_all(&nn); notes_unlock();
            printf("tabs=%d cur=%d uri=%s title=%s sel=%d-%d scroll=%d hist=%d pop=%d dlg=%d notes=%d sidebar=%d pages=%d focus=%s\n",
                   A.ntabs, A.cur, t && t->uri ? t->uri : "-", t && t->title ? t->title : "-", t ? t->sel0 : 0, t ? t->sel1 : 0,
                   t ? t->scroll : 0, t ? t->nhist : 0, A.pop, A.dlg, nn, A.sidebar, A.npages, A.focus ? field_str(A.focus) : "-");
            fflush(stdout);
        } else if (!strcmp(cmd, "clicktext")) {        /* click the n-th (a2) piece of text starting with a1 */
            tab_t *t = cur_tab();
            int want = n > 2 ? atoi(a2) : 1, seen = 0;
            for (int i = 0; t && t->lay && i < t->lay->npc; i++) {
                const piece_t *p = &t->lay->pc[i];
                const char *txt = t->lay->doc->b[p->blk].runs[p->run].text + p->b0;
                if (strncmp(txt, a1, strlen(a1)) || ++seen < want) continue;
                int x, y, h;
                layout_pos(t->lay, p->c0 + 1, &x, &y, &h);
                e.x = A.text_x0 + x; e.y = A.read_y0 + U(30) + y + h / 2 - t->scroll;
                e.type = EV_DOWN; e.button = 1; e.clicks = 1; e.mod = !strcmp(a3, "ctrl") ? M_CTRL : 0;
                mouse_x = e.x; mouse_y = e.y;
                printf("clicktext %s at %d,%d\n", a1, e.x, e.y);
                on_down(&e);
                render();
                e.type = EV_UP;
                if (dragging_sel) reader_up(&e);
                dragging_sel = 0;
                break;
            }
        } else if (!strcmp(cmd, "clickword")) {        /* click word a2 of paragraph a1 */
            tab_t *t = cur_tab();
            for (int bi = 0; t && t->lay && bi < t->lay->doc->n; bi++) {
                if (!t->lay->doc->b[bi].anchor || strcmp(t->lay->doc->b[bi].anchor, a1)) continue;
                int w = atoi(a2);
                if (w >= t->lay->bi[bi].nw) break;
                int x, y, h, x2, y2, h2;
                layout_pos(t->lay, t->lay->bi[bi].ws[w], &x, &y, &h);
                layout_pos(t->lay, t->lay->bi[bi].we[w], &x2, &y2, &h2);
                e.x = A.text_x0 + (y2 == y ? (x + x2) / 2 : x + U(4)); e.y = A.read_y0 + U(30) + y + h / 2 - t->scroll;
                e.type = EV_DOWN; e.button = 1; e.clicks = 1; e.mod = !strcmp(a3, "ctrl") ? M_CTRL : 0;
                mouse_x = e.x; mouse_y = e.y;
                on_down(&e);
                render();
                e.type = EV_UP;
                if (dragging_sel) reader_up(&e);
                dragging_sel = 0;
                break;
            }
        } else if (!strcmp(cmd, "clip")) { const char *t = strchr(line, ' '); plat_clip_set(t ? t + 1 : ""); }
        else if (!strcmp(cmd, "toast")) { printf("toast: %s\n", A.toast); fflush(stdout); }
        else if (!strcmp(cmd, "look")) { draw_look(atoi(a1)); A.dirty = 1; }
        else if (!strcmp(cmd, "size")) { plat_resize(atoi(a1), atoi(a2)); ev_t w; while (plat_wait(&w, 50)) ; }
        else if (!strcmp(cmd, "quit")) break;
    }
    free(text);
    save_session();
    exit(0);
}

int main(int argc, char **argv)
{
    char *dir = data_dir();
    store_init(dir);
    free(dir);
    if (font_init()) return 1;
    if (plat_open("Scriptures", 1240, 860)) return 1;
    int screen = plat_scale();
    UI_S = screen * setting_int("size", 100) / 100;
    if (screen > 100) plat_resize(1240 * screen / 100, 860 * screen / 100);
    draw_look(setting_int("look", LOOK_PHOSPHOR));
    snprintf(A.lang, sizeof A.lang, "%s", setting("lang", "eng"));
    A.reader_px = setting_int("text_size", 18) * UI_S / 100;
    A.sidebar = setting_int("sidebar", 1);
    A.dl.wake = A.sync.wake = wake;
    pthread_mutex_init(&A.dl.mu, NULL);
    pthread_mutex_init(&A.sync.mu, NULL);
    A.search.multiline = A.langcode.multiline = 0;
    A.cookie.password = 1;
    A.note_body.multiline = 1;
    A.dirty = 1;

    char *old = xprintf("%s/library.db", store_dir());
    if (strcmp(setting("imported_py", "0"), "1") && file_exists(old)) {
        A.busy = 1;
        pthread_t th;
        pthread_create(&th, NULL, import_thread, NULL);
        pthread_detach(th);
    }
    free(old);
    reload_volumes();
    int fixed = notes_repair();
    if (fixed) toast("%d notes linked back to their verses", fixed);
    restore_session();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--script") && i + 2 < argc) run_script(argv[i + 1], argv[i + 2]);
        else if (argv[i][0] == '/') open_place(A.lang, argv[i], NULL, 0, 0);   /* (a page: /scriptures/...) */
    }

    for (;;) {
        if (A.dirty) { A.dirty = 0; render(); }
        int busy = A.busy || A.dl.running || A.sync.running || A.watching || A.toast[0] ||
                   (cur_tab() && cur_tab()->flash_anchor);
        ev_t e;
        int got = plat_wait(&e, busy ? 250 : -1);
        jobs_tick();
        if (!got) { if (A.toast[0] || (cur_tab() && cur_tab()->flash_anchor)) A.dirty = 1; continue; }
        switch (e.type) {
        case EV_QUIT: save_session(); return 0;
        case EV_RESIZE: A.dirty = 1; break;
        case EV_LEAVE: mouse_x = mouse_y = -1; A.dirty = 1; break;
        case EV_DOWN: mouse_x = e.x; mouse_y = e.y; on_down(&e); break;
        case EV_UP:
            if (dragging_sel) reader_up(&e);
            dragging_sel = dragging_bar = dragging_field = 0;
            drag_field = NULL;
            pressed_tab = -1;
            A.dirty = 1;
            break;
        case EV_MOVE: on_move(&e); break;
        case EV_WHEEL: on_wheel(&e); break;
        case EV_KEY: if (on_key(&e)) A.dirty = 1; break;
        case EV_TEXT: if (A.focus) { field_insert(A.focus, e.text); A.dirty = 1; } break;
        case EV_WAKE:
            fetched();
            if (!A.busy && A.vols && !A.vols->n) { reload_volumes(); }
            A.dirty = 1;
            break;
        }
    }
}
