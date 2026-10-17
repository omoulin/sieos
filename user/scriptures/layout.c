/*
 * layout.c - Lines from styled paragraphs (layout.h).
 */
#include "layout.h"
#include "draw.h"
#include "font.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int face, px, center, before, after, dim; } bstyle_t;

static bstyle_t block_style(int kind, int px, int compact)
{
    bstyle_t s = { F_REG, px, 0, 0, px * 7 / 10, 0 };
    if (compact) { s.after = px / 3; return s; }
    switch (kind) {
    case BK_TITLE: s.face = F_BOLD; s.px = px * 16 / 10; s.center = 1; s.before = px / 2; s.after = px * 6 / 10; break;
    case BK_SUBTITLE: s.face = F_ITALIC; s.center = 1; s.after = px; break;
    case BK_INTRO: s.face = F_ITALIC; s.dim = 1; s.after = px; break;
    case BK_TITLENUM: s.face = F_BOLD; s.px = px * 125 / 100; s.center = 1; s.before = px * 7 / 10; s.after = px * 7 / 10; break;
    case BK_SUMMARY: s.face = F_ITALIC; s.px = px * 94 / 100; s.dim = 1; s.after = px * 13 / 10; break;
    case BK_HEADING: s.face = F_BOLD; s.px = px * 12 / 10; s.before = px; s.after = px / 2; break;
    case BK_LABEL: s.face = F_BOLD; s.px = px * 11 / 10; s.before = px * 8 / 10; s.after = px / 2; break;
    }
    return s;
}

static void run_font(const run_t *r, const bstyle_t *bs, int *face, int *px, int *rise)
{
    *face = r->style & ST_BOLD ? F_BOLD : r->style & ST_ITALIC ? F_ITALIC : bs->face;
    *px = bs->px;
    *rise = 0;
    if (r->style & ST_VERSENUM) { *face = F_BOLD; *px = bs->px * 78 / 100; }
    else if (r->style & ST_MARKER) { *face = F_ITALIC; *px = bs->px * 62 / 100; *rise = bs->px * 42 / 100; }
    else if (r->style & ST_SUP) { *px = bs->px * 70 / 100; *rise = bs->px * 40 / 100; }
    else if (r->style & ST_SUB) { *px = bs->px * 70 / 100; *rise = -bs->px / 6; }
    else if (r->style & ST_SMALLCAPS) *px = bs->px * 82 / 100;
}

/* ---- building ---------------------------------------------------------------------- */

typedef struct {
    layout_t *L;
    int cap_pc, cap_ln;
    int x, line_start_pc, maxpx, width;
    int y;
    piece_t word[64]; int nword, wordw;   /* the word being gathered (it may span runs) */
    int pending_space;                     /* a space piece waits at word[0] */
} lb_t;

static piece_t *push_piece(lb_t *b, const piece_t *p)
{
    layout_t *L = b->L;
    if (L->npc == b->cap_pc) { b->cap_pc = b->cap_pc * 2 + 256; L->pc = xrealloc(L->pc, b->cap_pc * sizeof *L->pc); }
    L->pc[L->npc] = *p;
    return &L->pc[L->npc++];
}

static void end_line(lb_t *b, int blk)
{
    layout_t *L = b->L;
    if (L->nln == b->cap_ln) { b->cap_ln = b->cap_ln * 2 + 64; L->ln = xrealloc(L->ln, b->cap_ln * sizeof *L->ln); }
    int px = b->maxpx ? b->maxpx : L->px;
    line_t *ln = &L->ln[L->nln++];
    ln->y = b->y;
    ln->h = px * 155 / 100;
    int asc = font_ascent(px), desc = font_descent(px);
    ln->base = b->y + (ln->h - asc - desc) / 2 + asc;
    ln->p0 = b->line_start_pc;
    ln->np = L->npc - b->line_start_pc;
    ln->blk = blk;
    b->y += ln->h;
    b->x = 0;
    b->line_start_pc = L->npc;
    b->maxpx = 0;
}

static void flush_word(lb_t *b, int blk)
{
    if (!b->nword) return;
    int lead = b->pending_space && b->x == 0;     /* (a space at a line's start: dropped) */
    int w = b->wordw - (lead ? b->word[0].w : 0);
    int words_w = b->wordw - (b->pending_space ? b->word[0].w : 0);
    if (b->x > 0 && b->x + w > b->width && words_w > 0) {
        /* wrap before the word: its leading space ends this line instead */
        int i0 = 0;
        if (b->pending_space) {
            piece_t sp = b->word[0];
            sp.x = b->x;
            push_piece(b, &sp);
            i0 = 1;
        }
        end_line(b, blk);
        for (int i = i0; i < b->nword; i++) {
            b->word[i].x = b->x;
            b->x += b->word[i].w;
            b->maxpx = MAX(b->maxpx, b->word[i].px + b->word[i].rise);
            push_piece(b, &b->word[i]);
        }
    } else {
        for (int i = 0; i < b->nword; i++) {
            if (i == 0 && lead) {               /* keep it as a zero-width piece (offsets stay whole) */
                b->word[0].w = 0;
            }
            b->word[i].x = b->x;
            b->x += b->word[i].w;
            b->maxpx = MAX(b->maxpx, b->word[i].px + b->word[i].rise);
            push_piece(b, &b->word[i]);
        }
    }
    b->nword = 0;
    b->wordw = 0;
    b->pending_space = 0;
}

static void add_frag(lb_t *b, int blk, piece_t *p, int is_space)
{
    if (is_space) { flush_word(b, blk); b->pending_space = 1; }
    if (b->nword == COUNT(b->word)) flush_word(b, blk);
    b->word[b->nword++] = *p;
    b->wordw += p->w;
}

layout_t *layout_make(const doc_t *doc, int width, int px, int compact)
{
    layout_t *L = xcalloc(1, sizeof *L);
    L->doc = doc;
    L->width = width;
    L->px = px;
    L->bi = xcalloc(doc->n + 1, sizeof *L->bi);
    lb_t b = { .L = L, .width = width };
    int off = 0;
    for (int bi = 0; bi < doc->n; bi++) {
        const block_t *blk = &doc->b[bi];
        bstyle_t bs = block_style(blk->kind, px, compact);
        if (bi) b.y += bs.before;
        L->bi[bi].c0 = off;
        L->bi[bi].y0 = b.y;
        int first_line = L->nln;
        for (int ri = 0; ri < blk->nruns; ri++) {
            const run_t *r = &blk->runs[ri];
            piece_t p = { .blk = bi, .run = ri, .style = r->style };
            run_font(r, &bs, &p.face, &p.px, &p.rise);
            const char *t = r->text;
            int n = (int)strlen(t);
            for (int i = 0; i < n;) {
                if (t[i] == '\n') {                       /* <br> */
                    flush_word(&b, bi);
                    piece_t q = p;
                    q.b0 = i; q.b1 = i + 1; q.c0 = off++; q.nc = 1; q.w = 0; q.x = b.x;
                    push_piece(&b, &q);
                    end_line(&b, bi);
                    i++;
                    continue;
                }
                int sp = t[i] == ' ', dash = 0;
                int j = i, nc = 0;
                while (j < n && t[j] != '\n' && (t[j] == ' ') == sp) {
                    int k, cp = utf8_dec(t + j, &k);
                    j += k;
                    nc++;
                    if (!sp && cp == 0x2014) { dash = 1; break; }   /* a line may break after an em dash */
                }
                piece_t q = p;
                q.b0 = i; q.b1 = j; q.c0 = off; q.nc = nc;
                q.w = text_w(t + i, j - i, q.face, q.px);
                off += nc;
                add_frag(&b, bi, &q, sp);
                if (dash) flush_word(&b, bi);
                i = j;
            }
        }
        flush_word(&b, bi);
        if (b.line_start_pc < L->npc || L->nln == first_line) end_line(&b, bi);
        if (bs.center)
            for (int li = first_line; li < L->nln; li++) {
                line_t *ln = &L->ln[li];
                if (!ln->np) continue;
                piece_t *last = &L->pc[ln->p0 + ln->np - 1];
                int lw = last->x + (last->b1 - last->b0 == 1 && L->doc->b[last->blk].runs[last->run].text[last->b0] == ' ' ? 0 : last->w);
                int shift = MAX(0, (width - lw) / 2);
                for (int k = 0; k < ln->np; k++) L->pc[ln->p0 + k].x += shift;
            }
        L->bi[bi].c1 = off;
        L->bi[bi].y1 = b.y;
        b.y += bs.after;
        off++;                                          /* (between paragraphs) */

        /* its words: countable characters split at spaces */
        int cap = 16, nw = 0, inword = 0, pos = L->bi[bi].c0;
        int *ws = xmalloc(cap * sizeof *ws), *we = xmalloc(cap * sizeof *we);
        for (int ri = 0; ri < blk->nruns; ri++) {
            const run_t *r = &blk->runs[ri];
            for (int i = 0, k; r->text[i]; i += k, pos++) {
                int cp = utf8_dec(r->text + i, &k);
                if (!r->countable) continue;
                int space = cp == ' ' || cp == '\n' || cp == 0xa0;
                if (space) { inword = 0; continue; }
                if (!inword) {
                    if (nw == cap) { cap *= 2; ws = xrealloc(ws, cap * sizeof *ws); we = xrealloc(we, cap * sizeof *we); }
                    ws[nw] = pos;
                    nw++;
                    inword = 1;
                }
                we[nw - 1] = pos + 1;
            }
        }
        L->bi[bi].ws = ws; L->bi[bi].we = we; L->bi[bi].nw = nw;
    }
    L->height = b.y;
    L->nchars = off;
    return L;
}

void layout_free(layout_t *L)
{
    if (!L) return;
    for (int i = 0; i < L->doc->n; i++) { free(L->bi[i].ws); free(L->bi[i].we); }
    free(L->bi);
    free(L->pc);
    free(L->ln);
    free(L);
}

/* ---- queries ------------------------------------------------------------------------ */

static const char *ptext(const layout_t *L, const piece_t *p) { return L->doc->b[p->blk].runs[p->run].text; }

static int line_at(const layout_t *L, int y)
{
    int lo = 0, hi = L->nln - 1;
    if (hi < 0) return -1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (L->ln[mid].y <= y) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* The x where character i (counted from the piece's start) begins. */
static int char_x(const layout_t *L, const piece_t *p, int i)
{
    const char *t = ptext(L, p) + p->b0;
    int b = 0;
    for (int k, c = 0; c < i && b < p->b1 - p->b0; c++) { utf8_dec(t + b, &k); b += k; }
    return p->x + (p->w ? text_w(t, b, p->face, p->px) : 0);
}

int layout_offset_at(const layout_t *L, int x, int y)
{
    int li = line_at(L, y);
    if (li < 0) return 0;
    if (y >= L->ln[li].y + L->ln[li].h && li == L->nln - 1) return L->nchars;
    const line_t *ln = &L->ln[li];
    if (!ln->np) return L->bi[ln->blk].c0;
    const piece_t *first = &L->pc[ln->p0];
    if (x <= first->x) return first->c0;
    for (int k = 0; k < ln->np; k++) {
        const piece_t *p = &L->pc[ln->p0 + k];
        if (x >= p->x + p->w && k < ln->np - 1) continue;
        for (int c = 0; c < p->nc; c++) {
            int a = char_x(L, p, c), b2 = char_x(L, p, c + 1);
            if (x < (a + b2) / 2) return p->c0 + c;
        }
        if (k == ln->np - 1) return p->c0 + p->nc;
    }
    return first->c0;
}

const piece_t *layout_piece_at(const layout_t *L, int x, int y)
{
    int li = line_at(L, y);
    if (li < 0 || y >= L->ln[li].y + L->ln[li].h) return NULL;
    const line_t *ln = &L->ln[li];
    for (int k = 0; k < ln->np; k++) {
        const piece_t *p = &L->pc[ln->p0 + k];
        if (x >= p->x - 2 && x < p->x + p->w + 2 && p->w) return p;
    }
    return NULL;
}

void layout_pos(const layout_t *L, int off, int *x, int *y, int *h)
{
    *x = 0; *y = 0; *h = L->px;
    for (int li = 0; li < L->nln; li++) {
        const line_t *ln = &L->ln[li];
        for (int k = 0; k < ln->np; k++) {
            const piece_t *p = &L->pc[ln->p0 + k];
            if (off >= p->c0 && off <= p->c0 + p->nc) { *x = char_x(L, p, off - p->c0); *y = ln->y; *h = ln->h; return; }
        }
    }
}

int layout_anchor_y(const layout_t *L, const char *anchor)
{
    for (int i = 0; i < L->doc->n; i++)
        if (L->doc->b[i].anchor && !strcmp(L->doc->b[i].anchor, anchor)) return L->bi[i].y0;
    return -1;
}
const char *layout_anchor_at(const layout_t *L, int y)
{
    for (int i = 0; i < L->doc->n; i++)
        if (L->doc->b[i].anchor && L->bi[i].y1 > y) return L->doc->b[i].anchor;
    return NULL;
}
int layout_verse_num(const layout_t *L, const char *anchor)
{
    for (int i = 0; i < L->doc->n; i++)
        if (L->doc->b[i].anchor && !strcmp(L->doc->b[i].anchor, anchor)) return L->doc->b[i].num;
    return 0;
}

int layout_span_range(const layout_t *L, const span_t *s, int *c0, int *c1)
{
    for (int i = 0; i < L->doc->n; i++) {
        if (!L->doc->b[i].anchor || strcmp(L->doc->b[i].anchor, s->anchor)) continue;
        const blkinfo_t *b = &L->bi[i];
        if (!b->nw) return 0;
        int first = s->start < 0 ? 0 : s->start, last = s->end < 0 ? b->nw - 1 : MIN(s->end, b->nw - 1);
        if (first > last) return 0;
        *c0 = b->ws[first];
        *c1 = b->we[last];
        return 1;
    }
    return 0;
}

int layout_spans(const layout_t *L, int c0, int c1, span_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < L->doc->n && n < max; i++) {
        const blkinfo_t *b = &L->bi[i];
        if (!L->doc->b[i].anchor || b->c1 <= c0 || b->c0 >= c1) continue;
        int first = -1, last = -1;
        for (int w = 0; w < b->nw; w++)
            if (b->ws[w] < c1 && b->we[w] > c0) { if (first < 0) first = w; last = w; }
        if (first < 0) continue;
        snprintf(out[n].anchor, sizeof out[n].anchor, "%s", L->doc->b[i].anchor);
        out[n].start = first;
        out[n].end = last;
        n++;
    }
    return n;
}

char *layout_text(const layout_t *L, int c0, int c1)
{
    buf_t t = {0};
    int prev_blk = -1;
    for (int i = 0; i < L->npc; i++) {
        const piece_t *p = &L->pc[i];
        if (p->c0 + p->nc <= c0 || p->c0 >= c1) continue;
        const run_t *r = &L->doc->b[p->blk].runs[p->run];
        if (!r->countable) continue;
        if (prev_blk >= 0 && p->blk != prev_blk) buf_puts(&t, "\n");
        prev_blk = p->blk;
        const char *s = r->text + p->b0;
        for (int b = 0, k, c = p->c0; b < p->b1 - p->b0; b += k, c++) {
            utf8_dec(s + b, &k);
            if (c >= c0 && c < c1) buf_add(&t, s + b, k);
        }
    }
    char *out = buf_take(&t);
    size_t n = strlen(out);                        /* (trimmed) */
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\n')) out[--n] = 0;
    char *s = out;
    while (*s == ' ') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
    return out;
}

/* ---- drawing --------------------------------------------------------------------------- */

static uint32_t piece_color(const layout_t *L, const piece_t *p)
{
    int kind = L->doc->b[p->blk].kind;
    if (p->style & (ST_MARKER | ST_LINK)) return C_ACCT;
    if (p->style & ST_VERSENUM) return C_DIM;
    if (kind == BK_INTRO || kind == BK_SUMMARY) return C_DIM;
    return C_TXT;
}

void layout_draw(const layout_t *L, int ox, int oy, int top, int bottom, const mark_t *m, int nm)
{
    int li = line_at(L, top - oy);
    if (li < 0) return;
    for (; li < L->nln; li++) {
        const line_t *ln = &L->ln[li];
        if (oy + ln->y > bottom) break;
        /* backgrounds first (highlights, the selection), then the text over them */
        for (int k = 0; k < ln->np; k++) {
            const piece_t *p = &L->pc[ln->p0 + k];
            for (int i = 0; i < nm; i++) {
                int a = MAX(m[i].c0, p->c0), b = MIN(m[i].c1, p->c0 + p->nc);
                if (a >= b || !p->w) continue;
                int xa = char_x(L, p, a - p->c0), xb = char_x(L, p, b - p->c0);
                if (m[i].bg) draw_rect(ox + xa, oy + ln->y + ln->h / 8, ox + xb, oy + ln->y + ln->h - ln->h / 10, 0, m[i].bg);
                if (m[i].underline)
                    draw_rect(ox + xa, oy + ln->base + L->px / 5, ox + xb, oy + ln->base + L->px / 5 + MAX(1, L->px / 9), 0, C_ACC);
            }
        }
        for (int k = 0; k < ln->np; k++) {
            const piece_t *p = &L->pc[ln->p0 + k];
            if (!p->w) continue;
            const char *t = ptext(L, p);
            uint32_t c = piece_color(L, p);
            draw_text(ox + p->x, oy + ln->base - p->rise, t + p->b0, p->b1 - p->b0, p->face, p->px, c);
            if (p->style & ST_LINK)
                draw_rect(ox + p->x, oy + ln->base + 2, ox + p->x + p->w, oy + ln->base + 3, 0, C_ACCT);
            if (p->style & ST_UNDERLINE)
                draw_rect(ox + p->x, oy + ln->base + 2, ox + p->x + p->w, oy + ln->base + 3, 0, c);
        }
    }
}
