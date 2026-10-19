/*
 * panels.c - The library sidebar, popovers (a footnote, a selection) and the
 * dialogs (a note; Library & Account), with the jobs they start.
 */
#include "app.h"
#include "font.h"
#include "linux_import.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSION "0.2"

/* ---- the sidebar --------------------------------------------------------------------------- */

void side_push(int kind, const toc_t *node, const char *title)
{
    if (A.npages == COUNT(A.pages)) return;
    page_t *p = &A.pages[A.npages++];
    memset(p, 0, sizeof *p);
    p->kind = kind;
    p->node = node;
    p->title = xstrdup(title ? title : "");
    A.dirty = 1;
}

static void side_pop(void)
{
    if (A.npages <= 1) return;
    page_t *p = &A.pages[--A.npages];
    free(p->title);
    hits_free(p->hits, p->nhits);
    A.dirty = 1;
}

void side_search(const char *q)
{
    while (A.npages > 1) side_pop();
    if (!q || !*q) return;
    char *t = xprintf("“%s”", q);
    side_push(PG_SEARCH, NULL, t);
    free(t);
    page_t *p = &A.pages[A.npages - 1];
    p->hits = store_search(A.lang, q, 300, &p->nhits);
    A.focus = NULL;
}

static int row(int x0, int x1, int *y, int clip0, int clip1, const char *title, int title_face, const char *sub,
               int sublines, int arrow, int id, int a, const void *p, int current)
{
    int px = ui_px(), sp = ui_small(), pad = U(10);
    int h = U(10) + px * 13 / 10 + (sub ? U(4) + sublines * sp * 145 / 100 : 0) + U(8);
    int top = *y;
    *y += h;
    if (top + h < clip0 || top > clip1) return h;
    int hov = hovered(x0 + U(6), MAX(top, clip0), x1 - U(6), MIN(top + h, clip1));
    if (current) draw_rect(x0 + U(6), top + U(1), x1 - U(6), top + h - U(1), U(6), C_SEL);
    else if (hov) draw_rect(x0 + U(6), top + U(1), x1 - U(6), top + h - U(1), U(6), C_BAR);
    int tw = x1 - x0 - 2 * pad - U(6) - (arrow ? U(20) : 0);
    draw_text_clipped(x0 + pad + U(6), top + U(10) + font_ascent(px), tw, title, title_face, px, C_TXT);
    if (sub) ui_wrap(x0 + pad + U(6), top + U(10) + px * 13 / 10 + U(4), tw, sub, F_REG, sp, C_DIM, sublines);
    if (arrow) draw_text(x1 - pad - U(14), top + (h - px) / 2 + font_ascent(px), "›", 3, F_BOLD, px, C_DIM);
    zone(x0 + U(6), MAX(top, clip0), x1 - U(6), MIN(top + h, clip1), id, a, p, CUR_HAND);
    return h;
}

static int is_numbered(const toc_t *n)
{
    if (n->nkids < 2) return 0;
    for (int i = 0; i < n->nkids; i++) {
        if (!n->kids[i].uri || n->kids[i].nkids) return 0;
        const char *last = strrchr(n->kids[i].uri, '/');
        if (!last || !last[1]) return 0;
        for (const char *c = last + 1; *c; c++) if (*c < '0' || *c > '9') return 0;
    }
    return 1;
}

static void wrap_lines_count(const char *s, int w, int *lines)
{
    int px = ui_small(), n = (int)strlen(s), l = 0;
    for (int i = 0; i < n && l < 4; l++) {
        int fit = text_fit(s + i, n - i, F_REG, px, w);
        if (fit <= 0) break;
        i += fit;
    }
    *lines = MAX(1, l);
}

void draw_sidebar(int x0, int y0, int x1, int y1)
{
    draw_rect(x0, y0, x1, y1, U(10), C_GLASS);
    draw_frame(x0, y0, x1, y1, U(10), 1, C_LINE);
    zone(x0, y0, x1, y1, Z_SIDE, 0, NULL, CUR_ARROW);
    if (!A.npages) side_push(PG_ROOT, NULL, "Library");
    page_t *pg = &A.pages[A.npages - 1];
    int hh = U(46), tpx = ui_title(), tx = x0 + U(16);
    if (A.npages > 1) {
        int s = U(30), bx = x0 + U(8), by = y0 + (hh - s) / 2;
        if (hovered(bx, by, bx + s, by + s)) draw_rect(bx, by, bx + s, by + s, U(6), C_BAR);
        draw_text(bx + U(9), by + U(22), "‹", 3, F_BOLD, U(20), C_TXT);
        zone(bx, by, bx + s, by + s, Z_SIDEBACK, 0, NULL, CUR_HAND);
        tx = bx + s + U(6);
    }
    draw_text_clipped(tx, y0 + (hh - tpx) / 2 + font_ascent(tpx), x1 - tx - U(12), pg->title, F_BOLD, tpx, C_ACCT);
    int y = y0 + hh;
    if (pg->kind == PG_ROOT) {
        field_draw(&A.search, x0 + U(12), y, x1 - U(12), y + U(34), "Search the scriptures (Ctrl+F)", A.focus == &A.search, Z_FIELD);
        y += U(44);
    }
    draw_rect(x0 + U(12), y - U(4), x1 - U(12), y - U(3), 0, C_LINE);
    int c0 = y, c1 = y1 - U(64);                        /* (room for the settings button below) */
    draw_clip(x0 + 1, c0, x1 - 1, c1);
    int cy = c0 - pg->scroll + U(4), start = cy;
    tab_t *t = cur_tab();
    const char *cur_uri = t ? t->uri : NULL;

    switch (pg->kind) {
    case PG_ROOT:
        if (!A.vols || !A.vols->n) {
            cy += ui_wrap(x0 + U(18), cy + U(8), x1 - x0 - U(36), "Nothing is downloaded yet. Download the scriptures to read them offline.",
                          F_REG, ui_px(), C_DIM, 0) + U(16);
            ui_button(x0 + U(18), cy, U(34), "Download…", BTN_ACCENT, Z_DOWNLOADS, 0, NULL);
            cy += U(50);
        }
        for (int i = 0; A.vols && i < A.vols->n; i++)
            row(x0, x1, &cy, c0, c1, A.vols->v[i].title, F_REG, NULL, 0, 1, Z_VOLUME, i, NULL, 0);
        cy += U(6);
        draw_rect(x0 + U(16), cy, x1 - U(16), cy + 1, 0, C_LINE);
        cy += U(6);
        row(x0, x1, &cy, c0, c1, "✎  Notes & Highlights", F_REG, NULL, 0, 1, Z_NOTESPAGE, 0, NULL, 0);
        break;
    case PG_GROUP: {
        const toc_t *n = pg->node;
        if (is_numbered(n)) {
            int bw = U(46), bh = U(36), gap = U(6), x = x0 + U(14);
            int cols = MAX(1, (x1 - x0 - U(28) + gap) / (bw + gap));
            for (int i = 0; i < n->nkids; i++) {
                int bx = x + (i % cols) * (bw + gap), by = cy + (i / cols) * (bh + gap);
                if (by + bh < c0 || by > c1) continue;
                const char *num = strrchr(n->kids[i].uri, '/') + 1;
                int on = cur_uri && !strcmp(cur_uri, n->kids[i].uri), hov = hovered(bx, by, bx + bw, by + bh);
                draw_rect(bx, by, bx + bw, by + bh, U(6), on ? C_ACC : hov ? C_BAR : C_TERM);
                draw_frame(bx, by, bx + bw, by + bh, U(6), 1, on ? C_ACC : C_LINE);
                int w = text_w(num, (int)strlen(num), F_BOLD, ui_px());
                draw_text(bx + (bw - w) / 2, by + (bh - ui_px()) / 2 + font_ascent(ui_px()) - U(1), num, (int)strlen(num), F_BOLD,
                          ui_px(), on ? C_INK : C_TXT);
                zone(bx, MAX(by, c0), bx + bw, MIN(by + bh, c1), Z_CHAPTER, 0, n->kids[i].uri, CUR_HAND);
            }
            cy += ((n->nkids + cols - 1) / cols) * (bh + gap) + U(8);
        } else
            for (int i = 0; i < n->nkids; i++) {
                const toc_t *k = &n->kids[i];
                if (k->uri) row(x0, x1, &cy, c0, c1, k->title ? k->title : k->uri, F_REG, NULL, 0, 0, Z_CHAPTER, 0, k->uri,
                                cur_uri && !strcmp(cur_uri, k->uri));
                else row(x0, x1, &cy, c0, c1, k->title && *k->title ? k->title : "Contents", F_REG, NULL, 0, 1, Z_GROUP, 0, k, 0);
            }
        break;
    }
    case PG_SEARCH:
        if (!pg->nhits)
            cy += ui_wrap(x0 + U(18), cy + U(8), x1 - x0 - U(36), "No verses found. Only downloaded volumes are searched.", F_REG, ui_px(), C_DIM, 0) + U(16);
        else {
            char head[64];
            snprintf(head, sizeof head, pg->nhits >= 300 ? "The first %d verses" : "%d verses", pg->nhits);
            ui_text(x0 + U(18), cy + U(2), head, F_REG, ui_small(), C_DIM);
            cy += U(22);
        }
        for (int i = 0; i < pg->nhits; i++) {
            char ref[256];
            snprintf(ref, sizeof ref, "%s:%d", pg->hits[i].title, pg->hits[i].num);
            int lines;
            wrap_lines_count(pg->hits[i].snippet, x1 - x0 - U(40), &lines);
            row(x0, x1, &cy, c0, c1, ref, F_BOLD, pg->hits[i].snippet, MIN(lines, 3), 0, Z_HIT, i, NULL, 0);
        }
        break;
    }
    pg->content_h = cy - start + U(8);
    int max = MAX(0, pg->content_h - (c1 - c0));
    if (pg->scroll > max || pg->scroll < 0) { pg->scroll = CLAMP(pg->scroll, 0, max); A.dirty = 1; }
    draw_unclip();
}

void side_click(const zone_t *z, int button, int new_tab)
{
    (void)button;
    page_t *pg = A.npages ? &A.pages[A.npages - 1] : NULL;
    int overlay = A.W < U(860);
    switch (z->id) {
    case Z_SIDEBACK: side_pop(); break;
    case Z_VOLUME: side_push(PG_GROUP, A.vols->v[z->a].toc, A.vols->v[z->a].title); break;
    case Z_GROUP: { const toc_t *k = z->p; side_push(PG_GROUP, k, k->title); break; }
    case Z_NOTESPAGE: open_notes_tab(); if (overlay) A.sidebar = 0; break;
    case Z_CHAPTER:
        open_place(A.lang, z->p, NULL, new_tab, 1);
        if (overlay && !new_tab) A.sidebar = 0;
        break;
    case Z_HIT:
        if (pg && z->a < pg->nhits) open_place(A.lang, pg->hits[z->a].uri, pg->hits[z->a].anchor, new_tab, 1);
        if (overlay && !new_tab) A.sidebar = 0;
        break;
    }
    A.dirty = 1;
}

/* ---- popovers ----------------------------------------------------------------------------- */

void close_popover(void)
{
    if (!A.pop) return;
    A.pop = POP_NONE;
    layout_free(A.fn_lay); A.fn_lay = NULL;
    doc_free(A.fn_doc); A.fn_doc = NULL;
    free(A.fn_id); A.fn_id = NULL;
    A.dirty = 1;
}

void open_footnote(tab_t *t, const char *id, int x, int y)
{
    close_popover();
    jnode *fn = json_get(t->ch->footnotes, id);
    const char *text = json_str(fn, "text");
    if (!text) return;
    A.fn_doc = html_doc(text);
    A.fn_lay = layout_make(A.fn_doc, MIN(U(440), A.W - U(80)), ui_px() * 108 / 100, 1);
    A.fn_id = xstrdup(id);
    A.pop = POP_FOOTNOTE;
    A.pop_x = x;
    A.pop_y = y + U(14);
    A.dirty = 1;
}

static void create_note(int color, int edit)
{
    tab_t *t = cur_tab();
    if (!t || !t->lay || t->sel0 == t->sel1) return;
    span_t sp[64];
    int n = layout_spans(t->lay, MIN(t->sel0, t->sel1), MAX(t->sel0, t->sel1), sp, 64);
    if (!n) return;
    notes_lock();
    note_t *no = note_new();
    no->lang = xstrdup(t->lang);
    no->uri = xstrdup(t->uri);
    no->anchor = xstrdup(sp[0].anchor);
    no->spans = xmalloc(n * sizeof *no->spans);
    memcpy(no->spans, sp, n * sizeof *sp);
    no->nspans = n;
    no->color = color >= 0 ? xstrdup(COLOR_NAMES[color]) : NULL;
    no->tags = xcalloc(1, sizeof *no->tags);
    char *id = xstrdup(no->id);
    notes_save();
    notes_unlock();
    t->sel0 = t->sel1 = 0;
    close_popover();
    if (edit) open_note(id);
    free(id);
    A.dirty = 1;
}

void popover_click(const zone_t *z, int ctrl)
{
    switch (z->id) {
    case Z_SWATCH: create_note(z->a, 0); break;
    case Z_POP_NOTE: create_note(-1, 1); break;
    case Z_FNLINK: {
        char *href = xstrdup(z->p);
        if (!ctrl) close_popover();
        follow_link(href, ctrl);
        free(href);
        break;
    }
    }
}

static void card(int x0, int y0, int x1, int y1)
{
    draw_shade(x0 + U(4), y0 + U(6), x1 + U(4), y1 + U(8), U(10), 1);
    draw_rect(x0, y0, x1, y1, U(10), C_GLASS);
    draw_frame(x0, y0, x1, y1, U(10), 1, C_ACC);
    zone(x0, y0, x1, y1, Z_POPUP, 0, NULL, CUR_ARROW);
}

void draw_popover(void)
{
    if (A.pop == POP_FOOTNOTE && A.fn_lay) {
        tab_t *t = cur_tab();
        jnode *fn = t && t->ch ? json_get(t->ch->footnotes, A.fn_id) : NULL;
        char head[200];
        snprintf(head, sizeof head, "%s  %s", json_str(fn, "marker") ? json_str(fn, "marker") : "",
                 json_str(fn, "context") ? json_str(fn, "context") : "");
        int pad = U(16), w = A.fn_lay->width + 2 * pad, h = A.fn_lay->height + U(40) + pad;
        int x0 = CLAMP(A.pop_x - w / 2, U(8), A.W - w - U(8)), y0 = A.pop_y;
        if (y0 + h > A.H - U(8)) y0 = MAX(U(8), A.pop_y - U(30) - h);
        card(x0, y0, x0 + w, y0 + h);
        ui_text(x0 + pad, y0 + U(12), head, F_BOLD, ui_px(), C_ACCT);
        int lx = x0 + pad, ly = y0 + U(40);
        layout_draw(A.fn_lay, lx, ly, y0, y0 + h, NULL, 0);
        for (int i = 0; i < A.fn_lay->npc; i++) {      /* its links */
            const piece_t *p = &A.fn_lay->pc[i];
            if (!(p->style & ST_LINK) || !p->w) continue;
            const line_t *ln = NULL;
            for (int k = 0; k < A.fn_lay->nln && !ln; k++)
                if (i >= A.fn_lay->ln[k].p0 && i < A.fn_lay->ln[k].p0 + A.fn_lay->ln[k].np) ln = &A.fn_lay->ln[k];
            if (ln) zone(lx + p->x, ly + ln->y, lx + p->x + p->w, ly + ln->y + ln->h, Z_FNLINK, 0,
                         A.fn_doc->b[p->blk].runs[p->run].href, CUR_HAND);
        }
    } else if (A.pop == POP_SELECTION) {
        int s = U(26), gap = U(8), pad = U(10);
        const char *nl = "✎ Note", *cl = "Copy";
        int bh = U(30), w = 8 * (s + gap) + U(10) + ui_button_w(nl, bh) + ui_button_w(cl, bh) + gap + 2 * pad;
        int h = bh + 2 * pad, x0 = CLAMP(A.pop_x - w / 2, U(8), A.W - w - U(8)), y0 = A.pop_y - h - U(10);
        if (y0 < A.top_h) y0 = A.pop_y + U(30);
        card(x0, y0, x0 + w, y0 + h);
        int x = x0 + pad, cy = y0 + (h - s) / 2;
        for (int i = 0; i < 8; i++) {
            int hov = hovered(x, cy, x + s, cy + s);
            draw_rect(x, cy, x + s, cy + s, s / 2, hl_color(i));
            draw_frame(x, cy, x + s, cy + s, s / 2, hov ? 2 : 1, hov ? C_ACC : C_LINE);
            zone(x, cy, x + s, cy + s, Z_SWATCH, i, NULL, CUR_HAND);
            x += s + gap;
        }
        x += U(10);
        x = ui_button(x, y0 + pad, bh, nl, BTN_PLAIN, Z_POP_NOTE, 0, NULL) + gap;
        ui_button(x, y0 + pad, bh, cl, BTN_PLAIN, Z_POP_COPY, 0, NULL);
    }
}

/* ---- dialogs --------------------------------------------------------------------------------- */

static int confirm_delete;
static int vol_have[16], vol_total[16], counts_ok;

void close_dialog(void)
{
    A.dlg = DLG_NONE;
    A.focus = NULL;
    confirm_delete = 0;
    A.confirm_push = 0;
    free(A.note_id); A.note_id = NULL;
    free(A.note_quote); A.note_quote = NULL;
    free(A.note_ref); A.note_ref = NULL;
    A.dirty = 1;
}

void open_note(const char *id)
{
    close_popover();
    notes_lock();
    note_t *n = note_find(id);
    if (!n) { notes_unlock(); return; }
    A.note_id = xstrdup(id);
    A.note_color = note_color_index(n->color);
    field_set(&A.note_title, n->title);
    char *tags = note_tags_str(n);
    field_set(&A.note_tags, tags);
    free(tags);
    field_set(&A.note_body, n->content);
    A.note_body.cur = A.note_body.anchor = A.note_body.len;
    tab_t *t = cur_tab();
    buf_t q = {0};
    int v0 = 0, v1 = 0;
    if (t && t->lay && t->uri && !strcmp(t->uri, n->uri))
        for (int s = 0; s < n->nspans; s++) {
            int c0, c1;
            if (!layout_span_range(t->lay, &n->spans[s], &c0, &c1)) continue;
            char *tx = layout_text(t->lay, c0, c1);
            if (q.n) buf_puts(&q, " … ");
            buf_puts(&q, tx);
            free(tx);
            int v = layout_verse_num(t->lay, n->spans[s].anchor);
            if (v && !v0) v0 = v;
            if (v) v1 = v;
        }
    char *title = *n->uri ? store_chapter_title(n->lang, n->uri) : NULL;
    A.note_ref = v0 ? (v1 != v0 ? xprintf("%s:%d–%d", title ? title : "", v0, v1) : xprintf("%s:%d", title ? title : "", v0))
                    : xstrdup(title ? title : "Journal entry");
    free(title);
    A.note_quote = buf_take(&q);
    notes_unlock();
    A.dlg = DLG_NOTE;
    A.focus = &A.note_body;
    A.dirty = 1;
}

static void counts_refresh(void)
{
    volumes_t *vs = store_volumes(A.lang);
    for (int i = 0; i < NVOLUMES; i++) {
        vol_have[i] = vol_total[i] = 0;
        for (int v = 0; v < vs->n; v++) {
            if (strcmp(vs->v[v].code, VOLUMES[i].code)) continue;
            /* every page of its contents, and whether it is here */
            const toc_t *stack[64];
            int sp = 0;
            stack[sp++] = vs->v[v].toc;
            while (sp) {
                const toc_t *t = stack[--sp];
                for (int k = 0; k < t->nkids; k++) {
                    if (t->kids[k].uri) { vol_total[i]++; vol_have[i] += store_has_chapter(A.lang, t->kids[k].uri); }
                    if (t->kids[k].nkids && sp < 64) stack[sp++] = &t->kids[k];
                }
            }
        }
    }
    store_volumes_free(vs);
    counts_ok = 1;
}

void open_settings(int section)
{
    close_popover();
    A.dlg = DLG_SETTINGS;
    A.section = section;
    A.focus = NULL;
    counts_refresh();
    for (int i = 0; i < NVOLUMES && i < 16; i++) A.vol_pick[i] = VOLUMES[i].standard && vol_have[i] < MAX(1, vol_total[i]);
    A.dirty = 1;
}

static void dialog_frame(int w, int h, const char *title, int closable)
{
    draw_shade(0, 0, A.W, A.H, 0, 2);
    zone(0, 0, A.W, A.H, Z_SHADE, 0, NULL, CUR_ARROW);
    w = MIN(w, A.W - U(24));
    h = MIN(h, A.H - U(24));
    int x0 = (A.W - w) / 2, y0 = (A.H - h) / 2 + U(14);
    A.dlg_x0 = x0; A.dlg_y0 = y0; A.dlg_x1 = x0 + w; A.dlg_y1 = y0 + h;
    /* Atlas's window: a title tab in the accent colour on the card's top left */
    int px = ui_px(), tw = text_w(title, (int)strlen(title), F_BOLD, px) + U(32) + (closable ? U(30) : 0), th = U(30);
    draw_top_round(x0, y0 - th, x0 + tw, y0 + U(2), U(8), C_ACC);
    draw_text(x0 + U(14), y0 - th + (th - px) / 2 + font_ascent(px) - U(1), title, (int)strlen(title), F_BOLD, px, C_INK);
    draw_rect(x0, y0, x0 + w, y0 + h, U(10), C_GLASS);
    draw_frame(x0, y0, x0 + w, y0 + h, U(10), 1, C_ACC);
    draw_rect(x0, y0, x0 + U(10), y0 + U(10), 0, C_GLASS);      /* (square under the tab) */
    draw_rect(x0, y0, x0 + 1, y0 + U(10), 0, C_ACC);
    zone(x0, y0 - th, x0 + w, y0 + h, Z_DLG, 0, NULL, CUR_ARROW);
    if (closable) {
        int cx = x0 + tw - U(28), cy = y0 - th + U(5);
        if (hovered(cx, cy, cx + U(20), cy + U(20))) draw_rect(cx, cy, cx + U(20), cy + U(20), U(5), C_ACCT);
        draw_text(cx + U(4), cy + U(15), "✕", 3, F_REG, U(12), C_INK);
        zone(cx, cy, cx + U(20), cy + U(20), Z_S_CLOSE, 0, NULL, CUR_HAND);
    }
}

static void label(int x, int y, const char *s) { ui_text(x, y, s, F_BOLD, ui_small(), C_ACCT); }

static void draw_note_dialog(void)
{
    dialog_frame(U(600), U(620), "Note", 0);
    int x0 = A.dlg_x0 + U(24), x1 = A.dlg_x1 - U(24), y = A.dlg_y0 + U(20), px = ui_px();
    ui_text(x0, y, A.note_ref ? A.note_ref : "", F_BOLD, ui_title(), C_TXT);
    y += U(32);
    if (A.note_quote && *A.note_quote) {
        draw_rect(x0, y, x0 + U(3), y + U(60), 0, C_LINE);
        y += MAX(U(20), ui_wrap(x0 + U(14), y, x1 - x0 - U(14), A.note_quote, F_ITALIC, px, C_DIM, 3)) + U(14);
    }
    label(x0, y, "HIGHLIGHT");
    y += U(22);
    int s = U(28), x = x0;
    for (int i = -1; i < 8; i++) {
        int on = A.note_color == i, hov = hovered(x, y, x + s, y + s);
        draw_rect(x, y, x + s, y + s, s / 2, i < 0 ? C_FIELD : hl_color(i));
        draw_frame(x, y, x + s, y + s, s / 2, on ? 3 : 1, on ? C_ACC : hov ? C_TXT : C_LINE);
        if (i < 0) draw_text(x + U(8), y + U(20), "∅", 3, F_REG, U(14), C_DIM);
        zone(x, y, x + s, y + s, Z_N_SWATCH, i, NULL, CUR_HAND);
        x += s + U(8);
    }
    y += s + U(18);
    label(x0, y, "TITLE");
    field_draw(&A.note_title, x0, y + U(20), x1, y + U(54), "A title (optional)", A.focus == &A.note_title, Z_FIELD);
    y += U(66);
    label(x0, y, "TAGS");
    field_draw(&A.note_tags, x0, y + U(20), x1, y + U(54), "faith, prayer…", A.focus == &A.note_tags, Z_FIELD);
    y += U(66);
    label(x0, y, "NOTE");
    int by = A.dlg_y1 - U(54);
    field_draw(&A.note_body, x0, y + U(20), x1, by - U(14), "Write your note…", A.focus == &A.note_body, Z_FIELD);
    int bh = U(34);
    ui_button(x0, by, bh, confirm_delete ? "Click again to delete" : "Delete", BTN_DANGER, Z_N_DELETE, 0, NULL);
    int sw = ui_button_w("Save", bh), cw = ui_button_w("Cancel", bh);
    ui_button(x1 - sw - U(10) - cw, by, bh, "Cancel", BTN_PLAIN, Z_N_CANCEL, 0, NULL);
    ui_button(x1 - sw, by, bh, "Save", BTN_ACCENT, Z_N_SAVE, 0, NULL);
}

static const struct { const char *code, *name; } LANGS[] = {
    {"eng", "English"}, {"spa", "Español"}, {"por", "Português"}, {"fra", "Français"}, {"deu", "Deutsch"},
    {"ita", "Italiano"}, {"rus", "Русский"}, {"jpn", "日本語"}, {"kor", "한국어"}, {"zho", "中文"},
};

/* Signing in: the page opens in the default browser; the session comes back by the clipboard
 * (Firefox: also noticed by itself, from its cookie store). */
static void draw_signin(int cx, int cx1, int *yp, int expired)
{
    int y = *yp, bh = U(32), sp = ui_small(), ff;
    const char *br = default_browser(&ff);
    char open_label[96], step[400];
    label(cx, y, expired ? "THE SESSION EXPIRED: SIGN IN AGAIN" : "SIGN IN");
    y += U(26);
    snprintf(open_label, sizeof open_label, "1.  Open the Sign-In Page in %s", br);
    ui_button(cx, y, bh, A.watching ? "Waiting for your sign-in…" : open_label, A.watching ? BTN_ON : BTN_ACCENT, Z_S_SIGNIN, 0, NULL);
    y += bh + U(12);
    snprintf(step, sizeof step, "2.  Sign in there and stay on the notes page. Press F12, open the Network tab, type "
             "annotations in its Filter box and press F5. Click an annotations?... request; under Request Headers find "
             "cookie (lower case), right-click it and choose Copy value.%s",
             ff ? " (Firefox: this app also notices your sign-in by itself.)" : "");
    y += ui_wrap(cx, y, cx1 - cx, step, F_REG, sp, C_TXT, 0) + U(12);
    ui_button(cx, y, bh, "3.  Paste & Connect", BTN_ACCENT, Z_S_PASTECLIP, 0, NULL);
    y += bh + U(14);
    field_draw(&A.cookie, cx, y, cx1 - ui_button_w("Connect", bh) - U(10), y + bh,
               "…or paste it here yourself", A.focus == &A.cookie, Z_FIELD);
    ui_button(cx1 - ui_button_w("Connect", bh), y, bh, "Connect", BTN_PLAIN, Z_S_PASTE, 0, NULL);
    y += bh + U(14);
    if (ff) {
        ui_button(cx, y, bh, "Use My Firefox Session", BTN_PLAIN, Z_S_FIREFOX, 0, NULL);
        y += bh + U(14);
    }
    *yp = y;
}

static void draw_settings(void)
{
    static const char *secs[NSEC] = { "Look", "Library", "Church Account", "About" };
    dialog_frame(U(860), U(640), "Library & Account", 1);
    int x0 = A.dlg_x0, y0 = A.dlg_y0, x1 = A.dlg_x1, y1 = A.dlg_y1, px = ui_px();
    int lw = U(190);
    draw_rect(x0 + lw, y0 + U(12), x0 + lw + 1, y1 - U(12), 0, C_LINE);
    for (int i = 0; i < NSEC; i++) {                        /* the sections */
        int ry = y0 + U(14) + i * U(40), on = A.section == i, hov = hovered(x0 + U(8), ry, x0 + lw - U(8), ry + U(34));
        if (on || hov) draw_rect(x0 + U(8), ry, x0 + lw - U(8), ry + U(34), U(6), on ? C_SEL : C_BAR);
        ui_text(x0 + U(20), ry + U(8), secs[i], on ? F_BOLD : F_REG, px, on ? C_TXT : C_DIM);
        zone(x0 + U(8), ry, x0 + lw - U(8), ry + U(34), Z_S_SECTION, i, NULL, CUR_HAND);
    }
    int cx = x0 + lw + U(28), cx1 = x1 - U(28), y = y0 + U(22), bh = U(32);
    draw_clip(x0 + lw + 1, y0 + 1, x1 - 1, y1 - 1);
    switch (A.section) {
    case SEC_LOOK: {
        label(cx, y, "SIZE OF EVERYTHING");
        y += U(22);
        static const int sizes[] = { 75, 90, 100, 115, 125, 150, 175, 200 };
        int x = cx;
        int cur = setting_int("size", 100);
        for (int i = 0; i < COUNT(sizes); i++) {
            char s[8];
            snprintf(s, sizeof s, "%d%%", sizes[i]);
            if (x + ui_button_w(s, bh) > cx1) { x = cx; y += bh + U(8); }
            x = ui_button(x, y, bh, s, cur == sizes[i] ? BTN_ON : BTN_PLAIN, Z_S_SIZE, sizes[i], NULL) + U(8);
        }
        y += bh + U(22);
        label(cx, y, "READING TEXT");
        y += U(22);
        x = ui_button(cx, y, bh, "A−", BTN_PLAIN, Z_S_TEXT, -1, NULL) + U(8);
        x = ui_button(x, y, bh, "A+", BTN_PLAIN, Z_S_TEXT, 1, NULL) + U(8);
        x = ui_button(x, y, bh, "Reset", BTN_PLAIN, Z_S_TEXT, 0, NULL) + U(16);
        char sz[32];
        snprintf(sz, sizeof sz, "%d px  (Ctrl + / Ctrl −)", A.reader_px);
        ui_text(x, y + U(8), sz, F_REG, ui_small(), C_DIM);
        y += bh + U(22);
        const char *sample = "1 And it came to pass that I, Nephi, said unto my father: I will go and do the things which the Lord hath commanded.";
        draw_rect(cx, y, cx1, y + U(110), U(8), C_TERM);
        ui_wrap(cx + U(16), y + U(14), cx1 - cx - U(32), sample, F_REG, A.reader_px, C_TXT, 3);
        break;
    }
    case SEC_LIBRARY: {
        label(cx, y, "LANGUAGE");
        y += U(22);
        int x = cx;
        for (int i = 0; i < COUNT(LANGS); i++) {
            if (x + ui_button_w(LANGS[i].name, bh) > cx1) { x = cx; y += bh + U(8); }
            x = ui_button(x, y, bh, LANGS[i].name, !strcmp(A.lang, LANGS[i].code) ? BTN_ON : BTN_PLAIN, Z_S_LANG, i, NULL) + U(8);
        }
        y += bh + U(10);
        int known = 0;
        for (int i = 0; i < COUNT(LANGS); i++) known |= !strcmp(A.lang, LANGS[i].code);
        char ph[64];
        snprintf(ph, sizeof ph, known ? "Another language code (e.g. nld, swe), Enter" : "Now: %s. Another code, Enter", A.lang);
        field_draw(&A.langcode, cx, y, MIN(cx1, cx + U(420)), y + bh, ph, A.focus == &A.langcode, Z_FIELD);
        y += bh + U(22);
        label(cx, y, "VOLUMES  (DOWNLOADED FROM CHURCHOFJESUSCHRIST.ORG FOR OFFLINE READING)");
        y += U(24);
        int colw = (cx1 - cx) / 2;
        if (!counts_ok) counts_refresh();
        for (int i = 0; i < NVOLUMES && i < 16; i++) {
            char sub[64];
            if (vol_total[i]) snprintf(sub, sizeof sub, "%d of %d pages", vol_have[i], vol_total[i]);
            else snprintf(sub, sizeof sub, "not downloaded");
            ui_check(cx + (i % 2) * colw + U(6), y + (i / 2) * U(44), A.vol_pick[i], VOLUMES[i].title, sub, Z_S_VOL, i, NULL, colw - U(16));
        }
        y += ((NVOLUMES + 1) / 2) * U(44) + U(12);
        pthread_mutex_lock(&A.dl.mu);
        int running = A.dl.running, done = A.dl.done, total = A.dl.total;
        char msg[256];
        snprintf(msg, sizeof msg, "%s", A.dl.message);
        pthread_mutex_unlock(&A.dl.mu);
        x = ui_button(cx, y, U(36), running ? "Downloading…" : "Download Selected", running ? BTN_PLAIN : BTN_ACCENT, running ? Z_NONE : Z_S_DOWNLOAD, 0, NULL) + U(10);
        if (running) ui_button(x, y, U(36), "Cancel", BTN_PLAIN, Z_S_CANCEL, 0, NULL);
        y += U(50);
        if (running || msg[0]) {
            draw_rect(cx, y, cx1, y + U(10), U(5), C_TERM);
            if (total) draw_rect(cx, y, cx + (int)((int64_t)(cx1 - cx) * done / total), y + U(10), U(5), C_ACC);
            ui_text(cx, y + U(18), msg, F_REG, ui_small(), C_DIM);
        }
        break;
    }
    case SEC_ACCOUNT: {
        char *cookie = account_cookie();
        y += ui_wrap(cx, y, cx1 - cx, "Bring your Gospel Library highlights, notes and tags into this app. You sign in on the "
                     "Church's own page, in your browser: this app never sees your password.", F_REG, px, C_DIM, 0) + U(18);
        if (cookie && A.sync.auth_error && !A.sync.running) {
            draw_signin(cx, cx1, &y, 1);
            y += U(8);
        }
        if (cookie) {
            label(cx, y, "CONNECTED");
            y += U(24);
            int x = ui_button(cx, y, bh, "Download Notes From Account", BTN_ACCENT, A.sync.running ? Z_NONE : Z_S_PULL, 0, NULL) + U(10);
            x = ui_button(x, y, bh, A.confirm_push ? "Click again: upload my changes" : "Two-Way Sync (experimental)", BTN_PLAIN,
                          A.sync.running ? Z_NONE : Z_S_PUSH, 0, NULL) + U(10);
            if (x + ui_button_w("Disconnect", bh) > cx1) { x = cx; y += bh + U(10); }
            ui_button(x, y, bh, "Disconnect", BTN_DANGER, Z_S_DISCONNECT, 0, NULL);
            y += bh + U(14);
            char last[200];
            snprintf(last, sizeof last, "Last sync: %s", setting("last_sync", "never"));
            ui_text(cx, y, A.sync.running ? "Syncing…" : last, F_REG, ui_small(), C_DIM);
            y += U(26);
            pthread_mutex_lock(&A.sync.mu);
            char *log = xstrdup(A.sync.log);
            pthread_mutex_unlock(&A.sync.mu);
            if (*log) {
                draw_rect(cx, y, cx1, y1 - U(20), U(8), C_TERM);
                ui_wrap(cx + U(12), y + U(10), cx1 - cx - U(24), log, F_REG, ui_small(), C_TERMFG, 0);
            }
            free(log);
            free(cookie);
            break;
        }
        draw_signin(cx, cx1, &y, 0);
        ui_wrap(cx, y, cx1 - cx, "The session is kept only on this computer (data/account, readable by you only). "
                "Anyone with it can act as you on the Church website until it expires; Disconnect removes it.",
                F_REG, ui_small(), C_DIM, 0);
        break;
    }
    case SEC_ABOUT: {
        ui_text(cx, y, "Scriptures " VERSION, F_BOLD, ui_title(), C_ACCT);
        y += U(34);
        y += ui_wrap(cx, y, cx1 - cx, "Read the scriptures offline, with footnotes, search, highlights and notes synced with your "
                     "Church Account. In C, drawn as SIEOS Atlas draws (its look), for SIEOS later.", F_REG, px, C_TXT, 0) + U(10);
        y += ui_wrap(cx, y, cx1 - cx, "Scripture text and study helps © Intellectual Reserve, Inc., downloaded from "
                     "churchofjesuschrist.org for personal study.", F_REG, ui_small(), C_DIM, 0) + U(18);
        label(cx, y, "KEYS");
        y += U(24);
        static const char *keys[][2] = {
            {"Ctrl+F", "search"}, {"Ctrl+T / Ctrl+W", "new tab / close tab"}, {"Ctrl+Tab, Ctrl+PgDn / PgUp", "next / previous tab"},
            {"Ctrl+click, middle click", "open in a new tab"}, {"Alt+↑ / Alt+↓, ← →", "previous / next chapter"},
            {"Alt+←", "back"}, {"Ctrl+C", "copy the selection with its reference"}, {"Ctrl+ + / Ctrl+ −", "reading text size"},
            {"Ctrl+L", "show or hide the library"}, {"Ctrl+Shift+N", "all notes and highlights"}, {"Ctrl+,", "this window"}, {"Esc", "close"}, {"Ctrl+Q", "quit"},
        };
        for (int i = 0; i < COUNT(keys); i++) {
            ui_text(cx, y, keys[i][0], F_BOLD, ui_small(), C_TXT);
            ui_text(cx + U(250), y, keys[i][1], F_REG, ui_small(), C_DIM);
            y += U(22);
        }
        break;
    }
    }
    draw_unclip();
}

void draw_dialog(void)
{
    if (A.dlg == DLG_NOTE) draw_note_dialog();
    else if (A.dlg == DLG_SETTINGS) draw_settings();
}

static void save_note_dialog(void)
{
    notes_lock();
    note_t *n = note_find(A.note_id);
    if (n) {
        free(n->title); n->title = xstrdup(field_str(&A.note_title));
        free(n->content); n->content = xstrdup(field_str(&A.note_body));
        note_set_tags(n, field_str(&A.note_tags));
        free(n->color); n->color = A.note_color >= 0 ? xstrdup(COLOR_NAMES[A.note_color]) : NULL;
        note_touch(n);
        notes_save();
    }
    notes_unlock();
    close_dialog();
    toast("Saved");
}

static void connect_with(const char *cookie, const char *how)
{
    char *err = NULL;
    int ok = session_check(cookie, &err);
    if (ok == 1) { account_save(cookie); A.sync.auth_error = 0; toast("Connected to your Church Account%s", how); }
    else if (ok == 0) toast("That session was refused (expired or not signed in)");
    else toast("No answer from the Church website: %s", err ? err : "?");
    free(err);
}

void dialog_click(const zone_t *z, int clicks)
{
    (void)clicks;
    switch (z->id) {
    case Z_SHADE: if (A.dlg == DLG_SETTINGS) close_dialog(); break;
    case Z_S_CLOSE: close_dialog(); break;
    case Z_N_SWATCH: A.note_color = z->a; break;
    case Z_N_SAVE: save_note_dialog(); break;
    case Z_N_CANCEL: close_dialog(); break;
    case Z_N_DELETE:
        if (!confirm_delete) { confirm_delete = 1; break; }
        notes_lock();
        { note_t *n = note_find(A.note_id); if (n) { if (n->remote) note_delete(n); else note_purge(n); notes_save(); } }
        notes_unlock();
        close_dialog();
        toast("Highlight deleted");
        break;
    case Z_FIELD:                                     /* Enter in a field */
        if (z->p == &A.note_title || z->p == &A.note_tags) save_note_dialog();
        else if (z->p == &A.langcode && *field_str(&A.langcode)) {
            set_lang(field_str(&A.langcode));
            field_set(&A.langcode, "");
            counts_refresh();
        } else if (z->p == &A.cookie && *field_str(&A.cookie)) { connect_with(field_str(&A.cookie), ""); field_set(&A.cookie, ""); }
        break;
    case Z_S_SECTION: A.section = z->a; A.focus = NULL; A.confirm_push = 0; break;
    case Z_S_SIZE: {
        int base = UI_S * 100 / MAX(1, setting_int("size", 100));
        A.reader_px = A.reader_px * z->a / MAX(1, setting_int("size", 100));
        setting_set_int("size", z->a);
        UI_S = base * z->a / 100;
        break;
    }
    case Z_S_TEXT:
        A.reader_px = z->a ? CLAMP(A.reader_px + z->a * U(1), U(11), U(32)) : U(18);
        setting_set_int("text_size", A.reader_px * 100 / UI_S);
        break;
    case Z_S_LANG: set_lang(LANGS[z->a].code); counts_refresh(); for (int i = 0; i < NVOLUMES; i++) A.vol_pick[i] = VOLUMES[i].standard && vol_have[i] < MAX(1, vol_total[i]); break;
    case Z_S_VOL: A.vol_pick[z->a] = !A.vol_pick[z->a]; break;
    case Z_S_DOWNLOAD: {
        const char *codes[16];
        int n = 0;
        for (int i = 0; i < NVOLUMES; i++) if (A.vol_pick[i]) codes[n++] = VOLUMES[i].code;
        if (!n) { toast("Choose at least one volume"); break; }
        download_start(&A.dl, A.lang, codes, n);
        A.dl_was = 1;
        break;
    }
    case Z_S_CANCEL: A.dl.cancel = 1; break;
    case Z_S_SIGNIN: signin_start(); break;
    case Z_S_FIREFOX: {
        char *profile = NULL, *c = firefox_cookie(&profile);
        if (!c) toast("No churchofjesuschrist.org session in Firefox: sign in there first, or use Sign In");
        else { char *how = xprintf(" (Firefox profile %s)", profile ? profile : "?"); connect_with(c, how); free(how); }
        free(c); free(profile);
        break;
    }
    case Z_S_PASTE: if (*field_str(&A.cookie)) { connect_with(field_str(&A.cookie), ""); field_set(&A.cookie, ""); } else A.focus = &A.cookie; break;
    case Z_S_PASTECLIP: {
        char *c = plat_clip_get();
        if (c) {                                       /* (trimmed; a "Cookie:" prefix dropped) */
            char *t = c;
            while (*t == ' ' || *t == '\n' || *t == '\t') t++;
            if (!strncasecmp(t, "cookie:", 7)) t += 7;
            while (*t == ' ') t++;
            size_t l = strlen(t);
            while (l && (t[l - 1] == '\n' || t[l - 1] == '\r' || t[l - 1] == ' ')) t[--l] = 0;
            if (strchr(t, '=') && !strchr(t, '\n')) connect_with(t, "");
            else toast("The clipboard holds no cookie: copy the Cookie header's value first (step 2)");
        } else toast("The clipboard is empty: copy the Cookie header's value first (step 2)");
        free(c);
        break;
    }
    case Z_S_DISCONNECT: account_clear(); A.watching = 0; toast("Disconnected"); break;
    case Z_S_PULL: sync_start(&A.sync, 0); A.sync_was = 1; break;
    case Z_S_PUSH:
        if (!A.confirm_push) { A.confirm_push = 1; break; }
        A.confirm_push = 0;
        sync_start(&A.sync, 1);
        A.sync_was = 1;
        break;
    }
    A.dirty = 1;
}

/* ---- jobs and signing in ----------------------------------------------------------------------- */

static volatile int watch_found;
static int sync_retried;

static void *watch_thread(void *arg)
{
    (void)arg;
    char *c = firefox_cookie(NULL);
    if (c) {
        char *err = NULL;
        if (session_check(c, &err) == 1) { account_save(c); watch_found = 1; }
        free(err);
        free(c);
    }
    A.watch_busy = 0;
    plat_wake();
    return NULL;
}

void signin_start(void)
{
    int ff;
    plat_open_url("https://www.churchofjesuschrist.org/notes?lang=eng");   /* (the default browser) */
    default_browser(&ff);
    if (!ff) return;
    A.watching = 1;
    watch_found = 0;
    A.watch_since = plat_ms();
    A.watch_next = plat_ms() + 2500;
    A.dirty = 1;
}

void jobs_tick(void)
{
    if (A.dl_was && !A.dl.running) {
        A.dl_was = 0;
        reload_volumes();
        counts_refresh();
        toast("%s", A.dl.message);
    }
    if (A.sync_was && !A.sync.running) {
        A.sync_was = 0;
        if (A.sync.auth_error && !sync_retried) {
            /* an expired session: take a fresh one from Firefox when there is one, and retry once */
            char *c = firefox_cookie(NULL), *old = account_cookie();
            if (c && (!old || strcmp(c, old)) && session_check(c, NULL) == 1) {
                account_save(c);
                sync_retried = 1;
                sync_start(&A.sync, 0);
                A.sync_was = 1;
            } else {
                toast("The session expired: sign in again");
                if (A.dlg != DLG_NOTE) open_settings(SEC_ACCOUNT);
            }
            free(c); free(old);
        } else {
            sync_retried = 0;
            toast("%s", A.sync.auth_error ? "The session expired: sign in again" : "Sync complete");
        }
        A.dirty = 1;
    }
    if (A.watching) {
        if (watch_found) { A.watching = 0; watch_found = 0; A.sync.auth_error = 0; toast("Connected to your Church Account"); A.dirty = 1; }
        else if (plat_ms() - A.watch_since > 5 * 60 * 1000) { A.watching = 0; toast("Sign-in timed out: try again"); A.dirty = 1; }
        else if (!A.watch_busy && plat_ms() >= A.watch_next) {
            A.watch_busy = 1;
            A.watch_next = plat_ms() + 2500;
            pthread_t t;
            pthread_create(&t, NULL, watch_thread, NULL);
            pthread_detach(t);
        }
    }
    if (A.dl.running || A.sync.running) A.dirty = 1;
}
