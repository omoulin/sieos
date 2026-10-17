/*
 * layout.h - Paragraphs of styled runs (html.h) laid out in lines, and drawn.
 *
 * Every character has an offset (code points, in paragraph order, +1 between
 * paragraphs): selections, highlights and the pointer are expressed in them.
 * A paragraph's words (for highlight offsets) are its countable characters
 * split at spaces, as Gospel Library counts them.
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#include "html.h"
#include "store.h"
#include <stdint.h>

typedef struct {
    int x, w;                    /* in the layout */
    int blk, run, b0, b1;        /* its text: doc->b[blk].runs[run].text[b0..b1) */
    int c0, nc;                  /* its characters' offsets */
    int face, px, rise, style;
} piece_t;

typedef struct { int y, h, base, p0, np, blk; } line_t;
typedef struct { int c0, c1, y0, y1; int *ws, *we, nw; } blkinfo_t;

typedef struct {
    const doc_t *doc;
    int width, px;
    piece_t *pc; int npc;
    line_t *ln; int nln;
    blkinfo_t *bi;
    int height, nchars;
} layout_t;

typedef struct { int c0, c1; uint32_t bg; int underline; } mark_t;

layout_t *layout_make(const doc_t *doc, int width, int px, int compact);
void layout_free(layout_t *L);

int  layout_offset_at(const layout_t *L, int x, int y);          /* the nearest caret offset */
const piece_t *layout_piece_at(const layout_t *L, int x, int y);  /* the piece under (x, y), or NULL */
void layout_pos(const layout_t *L, int off, int *x, int *y, int *h);
void layout_draw(const layout_t *L, int ox, int oy, int top, int bottom, const mark_t *m, int nm);

int  layout_anchor_y(const layout_t *L, const char *anchor);      /* -1: not here */
const char *layout_anchor_at(const layout_t *L, int y);           /* the first paragraph at or below y */
int  layout_span_range(const layout_t *L, const span_t *s, int *c0, int *c1);   /* a note's span -> offsets */
int  layout_spans(const layout_t *L, int c0, int c1, span_t *out, int max);     /* offsets -> spans */
char *layout_text(const layout_t *L, int c0, int c1);            /* plain, without verse numbers or letters */
int  layout_verse_num(const layout_t *L, const char *anchor);

#endif
