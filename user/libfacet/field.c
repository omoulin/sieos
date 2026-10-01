/*
 * field.c - The text field: one line of text to type in, with the
 * selection and the clipboard every Facet application shares.
 *
 * Mouse: a click places the cursor, a drag selects, a double-click selects
 * the word.  Keys: Left, Right, Home, End (Shift extends the selection),
 * Backspace, Delete, Ctrl+A (all), Ctrl+C, Ctrl+X, Ctrl+V (the desktop's
 * clipboard).  A masked field (a password) shows dots and is never copied.
 * Text longer than the field scrolls to keep the cursor in view.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "facet/facet.h"
#include "facet/theme.h"

#define INSET 6

static bool cont(unsigned char c) { return (c & 0xC0) == 0x80; }   /* a UTF-8 continuation byte */

static int sel_lo(const struct fct_field *f) { return f->cur < f->anchor ? f->cur : f->anchor; }
static int sel_hi(const struct fct_field *f) { return f->cur > f->anchor ? f->cur : f->anchor; }

void fct_field_set(struct fct_field *f, const char *text)
{
    snprintf(f->text, sizeof(f->text), "%s", text ? text : "");
    f->cur = f->anchor = (int)strlen(f->text);
    f->scroll = 0;
    f->dragging = false;
}

/* What is shown: the text, or one dot a character */
static void shown(const struct fct_field *f, char *out, size_t n)
{
    if (!f->masked) {
        snprintf(out, n, "%s", f->text);
        return;
    }
    size_t i = 0;
    for (const char *s = f->text; *s && i + 1 < n; s++)
        out[i++] = cont((unsigned char)*s) ? '\x01' : '*';   /* (one byte for each: offsets stay) */
    out[i] = 0;
}

/* The width of shown bytes [a, b) */
static int width_of(const char *disp, int a, int b)
{
    char tmp[sizeof(((struct fct_field *)0)->text)];
    int n = 0;
    for (int i = a; i < b && disp[i]; i++)
        if (disp[i] != '\x01')
            tmp[n++] = disp[i];
    tmp[n] = 0;
    return text_width(tmp);
}

static void keep_visible(struct fct_field *f, const char *disp, int w)
{
    if (f->cur < f->scroll)
        f->scroll = f->cur;
    while (f->scroll < f->cur && width_of(disp, f->scroll, f->cur) > w) {
        f->scroll++;
        while (cont((unsigned char)f->text[f->scroll]))
            f->scroll++;
    }
}

static void draw_part(struct surface *s, int x, int y, const char *disp, int a, int b, color_t col)
{
    char tmp[sizeof(((struct fct_field *)0)->text)];
    int n = 0;
    for (int i = a; i < b && disp[i]; i++)
        if (disp[i] != '\x01')
            tmp[n++] = disp[i];
    tmp[n] = 0;
    gfx_text(s, x, y, tmp, col);
}

void fct_field_draw(struct surface *s, struct rect r, struct fct_field *f, bool focus, const char *hint)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, C_CONTENT);
    gfx_bevel(s, r.x, r.y, r.w, r.h, 1, false, focus ? C_ACCENT : C_LINE, C_FACE_DARK);
    char disp[sizeof(f->text)];
    shown(f, disp, sizeof(disp));
    int len = (int)strlen(disp), w = r.w - 2 * INSET, ty = r.y + (r.h - FONT_H) / 2;
    if (!len && hint && !focus) {
        gfx_text(s, r.x + INSET, ty, hint, C_DIM);
        return;
    }
    keep_visible(f, disp, w);
    struct rect old = s->clip;
    s->clip = rect_intersect(old, rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2));
    int end = f->scroll;                         /* the last byte that fits */
    while (end < len && width_of(disp, f->scroll, end + 1) <= w)
        end++;
    int lo = sel_lo(f), hi = sel_hi(f);
    if (focus && lo != hi) {                     /* the selection, under the text */
        int a = lo < f->scroll ? f->scroll : lo, b = hi > end ? end : hi;
        if (a < b) {
            int x0 = r.x + INSET + width_of(disp, f->scroll, a), x1 = r.x + INSET + width_of(disp, f->scroll, b);
            gfx_fill(s, x0, r.y + 3, x1 - x0, r.h - 6, C_SELECT);
        }
    }
    draw_part(s, r.x + INSET, ty, disp, f->scroll, end, C_TEXT);
    if (focus) {
        int cx = r.x + INSET + width_of(disp, f->scroll, f->cur);
        gfx_vline(s, cx, ty, FONT_H, C_ACCENT);
    }
    s->clip = old;
}

/* The byte offset nearest to x */
static int pos_at(struct fct_field *f, struct rect r, int x)
{
    char disp[sizeof(f->text)];
    shown(f, disp, sizeof(disp));
    int len = (int)strlen(disp), rel = x - r.x - INSET;
    if (rel <= 0)
        return f->scroll > 0 ? f->scroll - 1 : 0;
    int best = f->scroll, prev_w = 0;
    for (int i = f->scroll + 1; i <= len; i++) {
        if (i < len && cont((unsigned char)f->text[i]))
            continue;
        int wi = width_of(disp, f->scroll, i);
        if (wi >= rel)
            return rel - prev_w < wi - rel ? best : i;
        best = i;
        prev_w = wi;
    }
    return len;
}

bool fct_field_mouse(struct fct_field *f, struct rect r, int x, int y, int kind)
{
    bool inside = rect_contains(r, x, y);
    if (kind == FCT_MOUSE_DOWN && inside) {
        f->cur = f->anchor = pos_at(f, r, x);
        f->dragging = true;
        return true;
    }
    if (kind == FCT_MOUSE_DOUBLE && inside) {    /* the word under the pointer (a password: all of it) */
        int p = pos_at(f, r, x), a = p, b = p, len = (int)strlen(f->text);
        if (f->masked) {
            a = 0;
            b = len;
        } else {
            while (a > 0 && f->text[a - 1] != ' ')
                a--;
            while (b < len && f->text[b] != ' ')
                b++;
        }
        f->anchor = a;
        f->cur = b;
        f->dragging = false;
        return true;
    }
    if (kind == FCT_MOUSE_MOVE && f->dragging) {
        f->cur = pos_at(f, r, x);
        return true;
    }
    if (kind == FCT_MOUSE_UP && f->dragging) {
        f->dragging = false;
        return true;
    }
    return false;
}

static void delete_range(struct fct_field *f, int a, int b)
{
    memmove(f->text + a, f->text + b, strlen(f->text + b) + 1);
    f->cur = f->anchor = a;
}

static void insert(struct fct_field *f, const char *t, size_t n)
{
    if (sel_lo(f) != sel_hi(f))
        delete_range(f, sel_lo(f), sel_hi(f));
    size_t len = strlen(f->text), room = sizeof(f->text) - 1 - len;
    if (n > room) {
        n = room;
        while (n && cont((unsigned char)t[n]))   /* (not in the middle of a character) */
            n--;
    }
    memmove(f->text + f->cur + n, f->text + f->cur, len - f->cur + 1);
    memcpy(f->text + f->cur, t, n);
    f->cur += (int)n;
    f->anchor = f->cur;
}

static int step(const struct fct_field *f, int p, int dir)
{
    int len = (int)strlen(f->text);
    do
        p += dir;
    while (p > 0 && p < len && cont((unsigned char)f->text[p]));
    return p < 0 ? 0 : p > len ? len : p;
}

static void copy_selection(const struct fct_field *f)
{
    int lo = sel_lo(f), hi = sel_hi(f);
    if (lo != hi && !f->masked)
        fct_clipboard_set(f->text + lo, hi - lo);
}

int fct_field_key(struct fct_field *f, const struct fct_key *k)
{
    if (!k->value)
        return FCT_FIELD_NONE;
    bool ctrl = k->mods & FCT_MOD_CTRL, shift = k->mods & FCT_MOD_SHIFT;
    int len = (int)strlen(f->text), lo = sel_lo(f), hi = sel_hi(f);
    if (ctrl) {
        switch (k->code) {
        case 0x1E:                               /* Ctrl+A: all */
            f->anchor = 0;
            f->cur = len;
            return FCT_FIELD_MOVED;
        case 0x2E:                               /* Ctrl+C */
            copy_selection(f);
            return FCT_FIELD_MOVED;
        case 0x2D:                               /* Ctrl+X */
            if (lo == hi || f->masked)
                return FCT_FIELD_MOVED;
            copy_selection(f);
            delete_range(f, lo, hi);
            return FCT_FIELD_CHANGED;
        case 0x2F: {                             /* Ctrl+V: one line of the clipboard */
            size_t n;
            char *clip = fct_clipboard_get(&n);
            if (!clip)
                return FCT_FIELD_MOVED;
            size_t m = 0;
            for (size_t i = 0; i < n; i++) {
                if (clip[i] == '\n' || clip[i] == '\r')
                    break;
                if ((unsigned char)clip[i] >= 32 && clip[i] != 127)
                    clip[m++] = clip[i] == '\t' ? ' ' : clip[i];
            }
            insert(f, clip, m);
            free(clip);
            return FCT_FIELD_CHANGED;
        }
        }
        return FCT_FIELD_NONE;                   /* (other Ctrl keys: the application's) */
    }
    int to = -1;
    switch (k->code) {
    case FCT_KEY_LEFT:  to = !shift && lo != hi ? lo : step(f, f->cur, -1); break;
    case FCT_KEY_RIGHT: to = !shift && lo != hi ? hi : step(f, f->cur, 1); break;
    case FCT_KEY_HOME:  to = 0; break;
    case FCT_KEY_END:   to = len; break;
    case FCT_KEY_DELETE:
        if (lo != hi)
            delete_range(f, lo, hi);
        else if (f->cur < len)
            delete_range(f, f->cur, step(f, f->cur, 1));
        return FCT_FIELD_CHANGED;
    }
    if (to >= 0) {
        f->cur = to;
        if (!shift)
            f->anchor = to;
        return FCT_FIELD_MOVED;
    }
    if (k->ascii == '\b' || k->code == 0x0E) {
        if (lo != hi)
            delete_range(f, lo, hi);
        else if (f->cur > 0)
            delete_range(f, step(f, f->cur, -1), f->cur);
        return FCT_FIELD_CHANGED;
    }
    if (k->ascii >= 32 && k->ascii < 127) {
        char c = (char)k->ascii;
        insert(f, &c, 1);
        return FCT_FIELD_CHANGED;
    }
    return FCT_FIELD_NONE;                       /* Enter, Tab, Escape, Up, Down: the application's */
}
