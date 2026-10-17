/*
 * ui.c - Zones, buttons, check boxes, text fields (ui.h).
 */
#include "ui.h"
#include "draw.h"
#include "font.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

int UI_S = 100;
int mouse_x = -1, mouse_y = -1;

/* ---- zones ------------------------------------------------------------------------- */

static zone_t zones[2048];
static int nzones;

void zones_reset(void) { nzones = 0; }
void zone(int x0, int y0, int x1, int y1, int id, int a, const void *p, int cursor)
{
    if (nzones < COUNT(zones)) zones[nzones++] = (zone_t){ x0, y0, x1, y1, id, a, p, cursor };
}
const zone_t *zone_at(int x, int y)
{
    for (int i = nzones - 1; i >= 0; i--)                 /* (the last drawn is on top) */
        if (x >= zones[i].x0 && x < zones[i].x1 && y >= zones[i].y0 && y < zones[i].y1) return &zones[i];
    return NULL;
}
int hovered(int x0, int y0, int x1, int y1) { return mouse_x >= x0 && mouse_x < x1 && mouse_y >= y0 && mouse_y < y1; }

/* ---- text ---------------------------------------------------------------------------- */

int ui_px(void) { return U(14); }
int ui_small(void) { return U(12); }
int ui_title(void) { return U(17); }

int ui_text(int x, int y, const char *s, int face, int px, uint32_t c)
{
    return draw_text(x, y + font_ascent(px), s, (int)strlen(s), face, px, c);
}

static int measuring;
int ui_wrap_h(int w, const char *s, int face, int px, int maxlines)
{
    measuring = 1;
    int h = ui_wrap(0, 0, w, s, face, px, 0, maxlines);
    measuring = 0;
    return h;
}

int ui_wrap(int x, int y, int w, const char *s, int face, int px, uint32_t c, int maxlines)
{
    int lh = px * 145 / 100, lines = 0, n = (int)strlen(s);
    for (int i = 0; i < n && (!maxlines || lines < maxlines);) {
        if (s[i] == '\n') { i++; lines++; continue; }
        int fit = text_fit(s + i, n - i, face, px, w), end = i + fit;
        const char *nl = memchr(s + i, '\n', fit);
        if (nl) end = (int)(nl - s);
        else if (end < n) {
            int sp = end;
            while (sp > i && s[sp] != ' ') sp--;
            if (sp > i) end = sp;
        }
        if (end == i) end = i + 1;
        int last = maxlines && lines == maxlines - 1 && end < n;
        if (measuring) ;
        else if (last) draw_text_clipped(x, y + lines * lh + font_ascent(px), w, s + i, face, px, c);
        else draw_text(x, y + lines * lh + font_ascent(px), s + i, end - i, face, px, c);
        lines++;
        i = end;
        if (nl && i < n && s[i] == '\n') i++;          /* (the line's own break: not another, empty line) */
        else while (i < n && s[i] == ' ') i++;
    }
    return lines * lh;
}

/* ---- buttons --------------------------------------------------------------------------- */

int ui_button_w(const char *label, int h) { return text_w(label, (int)strlen(label), F_BOLD, ui_px()) + h; }

int ui_button(int x, int y, int h, const char *label, int style, int id, int a, const void *p)
{
    int w = ui_button_w(label, h), hov = hovered(x, y, x + w, y + h);
    uint32_t bg, fg;
    switch (style) {
    case BTN_ACCENT: bg = C_ACC; fg = C_INK; break;
    case BTN_ON: bg = C_SEL; fg = C_TXT; break;
    case BTN_FLAT: bg = hov ? C_BAR : 0xffffffff; fg = C_TXT; break;
    case BTN_DANGER: bg = hov ? C_BAR : C_GLASS; fg = C_ERR; break;
    default: bg = hov ? C_BAR : C_GLASS; fg = C_TXT;
    }
    if (bg != 0xffffffff) draw_rect(x, y, x + w, y + h, h / 2, bg);
    if (style == BTN_PLAIN || style == BTN_DANGER) draw_frame(x, y, x + w, y + h, h / 2, 1, hov ? C_ACC : C_LINE);
    if (style == BTN_ACCENT && hov) draw_frame(x, y, x + w, y + h, h / 2, 1, C_TXT);
    int px = ui_px();
    draw_text(x + h / 2, y + (h - px) / 2 + font_ascent(px) - U(1), label, (int)strlen(label), F_BOLD, px, fg);
    zone(x, y, x + w, y + h, id, a, p, CUR_HAND);
    return x + w;
}

void ui_check(int x, int y, int on, const char *label, const char *sub, int id, int a, const void *p, int w)
{
    int s = U(18), px = ui_px();
    int h = sub ? U(40) : U(28);
    if (hovered(x - U(6), y, x + w, y + h)) draw_rect(x - U(6), y, x + w, y + h, U(6), C_BAR);
    int by = y + (h - s) / 2;
    draw_rect(x, by, x + s, by + s, U(4), on ? C_ACC : C_FIELD);
    draw_frame(x, by, x + s, by + s, U(4), 1, on ? C_ACC : C_LINE);
    if (on) draw_text(x + U(3), by + s - U(4), "✓", 3, F_BOLD, U(14), C_INK);
    int tx = x + s + U(10);
    if (sub) {
        ui_text(tx, y + U(3), label, F_REG, px, C_TXT);
        ui_text(tx, y + U(22), sub, F_REG, ui_small(), C_DIM);
    } else ui_text(tx, y + (h - px) / 2 - U(1), label, F_REG, px, C_TXT);
    zone(x - U(6), y, x + w, y + h, id, a, p, CUR_HAND);
}

/* ---- text fields --------------------------------------------------------------------------- */

static void grow(field_t *f, int n)
{
    if (f->len + n + 1 > f->cap) {
        f->cap = MAX(f->cap * 2, f->len + n + 64);
        f->s = xrealloc(f->s, f->cap);
    }
}

void field_set(field_t *f, const char *s)
{
    f->len = 0;
    grow(f, (int)strlen(s ? s : ""));
    strcpy(f->s, s ? s : "");
    f->len = (int)strlen(f->s);
    f->cur = f->anchor = f->len;
    f->scroll = 0;
}
const char *field_str(field_t *f) { if (!f->s) field_set(f, ""); return f->s; }

static void del_range(field_t *f, int a, int b)
{
    if (a > b) { int t = a; a = b; b = t; }
    memmove(f->s + a, f->s + b, f->len - b + 1);
    f->len -= b - a;
    f->cur = f->anchor = a;
}
static int has_sel(const field_t *f) { return f->cur != f->anchor; }

void field_insert(field_t *f, const char *s)
{
    field_str(f);
    if (has_sel(f)) del_range(f, f->cur, f->anchor);
    int n = (int)strlen(s);
    char *clean = xstrndup(s, n);
    if (!f->multiline)                                   /* (one line: newlines become spaces) */
        for (char *c = clean; *c; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    grow(f, n);
    memmove(f->s + f->cur + n, f->s + f->cur, f->len - f->cur + 1);
    memcpy(f->s + f->cur, clean, n);
    free(clean);
    f->len += n;
    f->cur = f->anchor = f->cur + n;
}

void field_select_all(field_t *f) { field_str(f); f->anchor = 0; f->cur = f->len; }

static int next_char(const field_t *f, int i) { int k; if (i >= f->len) return f->len; utf8_dec(f->s + i, &k); return i + k; }
static int is_word(int c) { return c == '_' || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z') || c >= 0x80; }
static int word_left(const field_t *f, int i)
{
    while (i > 0 && f->s[i - 1] == ' ') i = utf8_prev(f->s, i);
    while (i > 0) { int p = utf8_prev(f->s, i), k; if (!is_word(utf8_dec(f->s + p, &k))) break; i = p; }
    return i;
}
static int word_right(const field_t *f, int i)
{
    while (i < f->len && f->s[i] == ' ') i++;
    while (i < f->len) { int k; if (!is_word(utf8_dec(f->s + i, &k))) break; i += k; }
    return i;
}

/* The drawn lines (multi-line): which line holds byte i. */
static int line_of(const field_t *f, int i)
{
    int l = 0;
    for (int k = 0; k < f->nls; k++) if (f->ls[k] <= i) l = k;
    return l;
}
static int line_end(const field_t *f, int l) { return l + 1 < f->nls ? f->ls[l + 1] : f->len; }
static int x_in_line(const field_t *f, int l, int i) { return text_w(f->s + f->ls[l], i - f->ls[l], F_REG, ui_px()); }
static int at_x(const field_t *f, int l, int x)
{
    int a = f->ls[l], e = line_end(f, l);
    if (e > a && l + 1 < f->nls && (f->s[e - 1] == '\n' || f->s[e - 1] == ' ')) e--;
    return a + text_fit(f->s + a, e - a, F_REG, ui_px(), x + text_w("n", 1, F_REG, ui_px()) / 2);
}

int field_key(field_t *f, const ev_t *e)
{
    field_str(f);
    int shift = e->mod & M_SHIFT, ctrl = e->mod & M_CTRL, old = f->cur;
    switch (e->key) {
    case K_LEFT: f->cur = has_sel(f) && !shift ? MIN(f->cur, f->anchor) : ctrl ? word_left(f, f->cur) : utf8_prev(f->s, f->cur); break;
    case K_RIGHT: f->cur = has_sel(f) && !shift ? MAX(f->cur, f->anchor) : ctrl ? word_right(f, f->cur) : next_char(f, f->cur); break;
    case K_HOME: f->cur = f->multiline && !ctrl && f->nls ? f->ls[line_of(f, f->cur)] : 0; break;
    case K_END: {
        if (f->multiline && !ctrl && f->nls) {
            int l = line_of(f, f->cur), en = line_end(f, l);
            if (l + 1 < f->nls && en > 0 && (f->s[en - 1] == '\n' || f->s[en - 1] == ' ')) en--;
            f->cur = en;
        } else f->cur = f->len;
        break;
    }
    case K_UP: case K_DOWN: {
        if (!f->multiline || !f->nls) return 0;
        int l = line_of(f, f->cur), nl = l + (e->key == K_DOWN ? 1 : -1);
        if (nl < 0) { f->cur = 0; break; }
        if (nl >= f->nls) { f->cur = f->len; break; }
        f->cur = at_x(f, nl, x_in_line(f, l, f->cur));
        break;
    }
    case K_BACKSPACE:
        if (has_sel(f)) del_range(f, f->cur, f->anchor);
        else if (f->cur) del_range(f, ctrl ? word_left(f, f->cur) : utf8_prev(f->s, f->cur), f->cur);
        return 1;
    case K_DELETE:
        if (has_sel(f)) del_range(f, f->cur, f->anchor);
        else if (f->cur < f->len) del_range(f, f->cur, ctrl ? word_right(f, f->cur) : next_char(f, f->cur));
        return 1;
    case K_ENTER:
        if (!f->multiline) return 2;
        field_insert(f, "\n");
        return 1;
    case 'a': if (!ctrl) return 0; field_select_all(f); return 1;
    case 'c': case 'x':
        if (!ctrl) return 0;
        if (has_sel(f) && !f->password) {
            int a = MIN(f->cur, f->anchor), b = MAX(f->cur, f->anchor);
            char *t = xstrndup(f->s + a, b - a);
            plat_clip_set(t);
            free(t);
            if (e->key == 'x') del_range(f, a, b);
        }
        return 1;
    case 'v': {
        if (!ctrl) return 0;
        char *t = plat_clip_get();
        if (t) { field_insert(f, t); free(t); }
        return 1;
    }
    default: return 0;
    }
    if (!shift) f->anchor = f->cur;
    (void)old;
    return 1;
}

void field_click(field_t *f, int x, int y, int extend)
{
    field_str(f);
    int pad = U(10);
    if (f->multiline && f->nls) {
        int lh = ui_px() * 145 / 100;
        int l = CLAMP((y - f->y0 - pad + f->scroll) / lh, 0, f->nls - 1);
        f->cur = at_x(f, l, x - f->x0 - pad);
    } else {
        int px = ui_px(), dx = x - f->x0 - pad + f->scroll;
        const char *shown = f->s;
        if (f->password) { f->cur = f->len; }
        else f->cur = text_fit(shown, f->len, F_REG, px, dx + text_w("n", 1, F_REG, px) / 2);
    }
    if (!extend) f->anchor = f->cur;
}

void field_draw(field_t *f, int x0, int y0, int x1, int y1, const char *ph, int focused, int id)
{
    field_str(f);
    f->x0 = x0; f->y0 = y0; f->x1 = x1; f->y1 = y1;
    int px = ui_px(), pad = U(10), r = f->multiline ? U(8) : (y1 - y0) / 2;
    draw_rect(x0, y0, x1, y1, r, C_FIELD);
    draw_frame(x0, y0, x1, y1, r, focused ? 2 : 1, focused ? C_ACC : C_LINE);
    zone(x0, y0, x1, y1, id, 0, f, CUR_TEXT);
    draw_clip(x0 + U(4), y0 + U(2), x1 - U(4), y1 - U(2));
    int iw = x1 - x0 - 2 * pad;
    if (!f->len && ph) ui_text(x0 + pad, f->multiline ? y0 + pad : y0 + (y1 - y0 - px) / 2 - U(1), ph, F_ITALIC, px, C_DIM);
    int sa = MIN(f->cur, f->anchor), sb = MAX(f->cur, f->anchor);
    if (!f->multiline) {
        char *shown = f->s;
        buf_t dots = {0};
        if (f->password) { for (int i = 0; i < utf8_len(f->s, f->len); i++) buf_puts(&dots, "•"); shown = dots.p ? dots.p : ""; }
        int curx = f->password ? text_w(shown, (int)strlen(shown), F_REG, px) : text_w(f->s, f->cur, F_REG, px);
        if (curx - f->scroll > iw) f->scroll = curx - iw;
        if (curx - f->scroll < 0) f->scroll = curx;
        int bx = x0 + pad - f->scroll, base = y0 + (y1 - y0 - px) / 2 + font_ascent(px) - U(1);
        if (focused && sb > sa && !f->password)
            draw_rect(bx + text_w(f->s, sa, F_REG, px), y0 + U(5), bx + text_w(f->s, sb, F_REG, px), y1 - U(5), 0, C_SEL);
        draw_text(bx, base, shown, (int)strlen(shown), F_REG, px, C_TXT);
        if (focused) draw_rect(bx + curx, y0 + U(6), bx + curx + MAX(1, U(2)), y1 - U(6), 0, C_ACC);
        buf_free(&dots);
    } else {
        /* wrap at words; remember the line starts for the keys and the mouse */
        int lh = px * 145 / 100;
        f->nls = 0;
        for (int i = 0;;) {
            if (f->nls == f->cls) { f->cls = f->cls * 2 + 32; f->ls = xrealloc(f->ls, f->cls * sizeof *f->ls); }
            f->ls[f->nls++] = i;
            if (i >= f->len) break;
            const char *nl = memchr(f->s + i, '\n', f->len - i);
            int lim = nl ? (int)(nl - f->s) : f->len;
            int fit = text_fit(f->s + i, lim - i, F_REG, px, iw), end = i + fit;
            if (end < lim) {
                int sp = end;
                while (sp > i && f->s[sp - 1] != ' ') sp--;
                if (sp > i) end = sp;
                else if (end == i) end = next_char(f, i);
            } else if (nl) end = lim + 1;
            else { i = end; break; }
            i = end;
            if (i >= f->len && !(f->len && f->s[f->len - 1] == '\n')) break;
        }
        int cl = line_of(f, f->cur), cy = cl * lh;
        int vis = y1 - y0 - 2 * pad;
        if (cy - f->scroll + lh > vis) f->scroll = cy + lh - vis;
        if (cy < f->scroll) f->scroll = cy;
        for (int l = 0; l < f->nls; l++) {
            int a = f->ls[l], b = line_end(f, l), y = y0 + pad + l * lh - f->scroll;
            if (y + lh < y0 || y > y1) continue;
            int shown_b = b > a && (f->s[b - 1] == '\n') ? b - 1 : b;
            if (focused && sb > sa && sa < b && sb > a) {
                int xa = text_w(f->s + a, MAX(sa, a) - a, F_REG, px), xb = text_w(f->s + a, MIN(sb, shown_b) - a, F_REG, px);
                draw_rect(x0 + pad + xa, y, x0 + pad + MAX(xb, xa + U(4)), y + lh, 0, C_SEL);
            }
            draw_text(x0 + pad, y + (lh - px) / 2 + font_ascent(px), f->s + a, shown_b - a, F_REG, px, C_TXT);
        }
        if (focused) {
            int x = x0 + pad + x_in_line(f, cl, f->cur), y = y0 + pad + cy - f->scroll;
            draw_rect(x, y + U(3), x + MAX(1, U(2)), y + lh - U(3), 0, C_ACC);
        }
    }
    draw_unclip();
}
