/*
 * font_tt.c - Glyphs from TrueType files with stb_truetype (font.h). Linux;
 * on SIEOS (the parts marked "SIEOS"): its font files, a slanted oblique.
 */
#include "font.h"
#include "util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

/* SIEOS: /lib/fonts holds DejaVu Sans Mono and its bold (scriptures.mk); the
 * oblique is the regular face slanted here (render). DejaVu Sans, when the web
 * browser brought it, gives the few symbols the mono face lacks. */
#define DEJAVU "/lib/fonts/"
static const char *FACE_FILES[NFACE][3] = {
    { DEJAVU "DejaVuSansMono.ttf" }, { DEJAVU "DejaVuSansMono-Bold.ttf" }, { DEJAVU "DejaVuSansMono-Oblique.ttf" },
};
static const char *FALLBACKS[] = { DEJAVU "DejaVuSans.ttf" };

typedef struct { unsigned char *data; stbtt_fontinfo info; int ok; } face_t;
static face_t faces[NFACE], fallbacks[COUNT(FALLBACKS)];
static face_t *mono;              /* metrics come from the regular face */
static int slant;                 /* no oblique face: the regular one, slanted */

static int load(face_t *f, const char *path)
{
    size_t n;
    f->data = (unsigned char *)read_file(path, &n);
    if (!f->data) return 0;
    int off = stbtt_GetFontOffsetForIndex(f->data, 0);
    f->ok = off >= 0 && stbtt_InitFont(&f->info, f->data, off);
    if (!f->ok) { free(f->data); f->data = NULL; }
    return f->ok;
}

int font_init(void)
{
    for (int i = 0; i < NFACE; i++)
        for (int k = 0; k < 3 && FACE_FILES[i][k] && !faces[i].ok; k++) load(&faces[i], FACE_FILES[i][k]);
    for (int i = 0; i < COUNT(FALLBACKS); i++) load(&fallbacks[i], FALLBACKS[i]);
    if (!faces[F_REG].ok) {
        for (int i = 0; i < COUNT(FALLBACKS) && !mono; i++) if (fallbacks[i].ok) mono = &fallbacks[i];
        if (!mono) { fprintf(stderr, "scriptures: no font found (" DEJAVU "DejaVuSansMono.ttf)\n"); return -1; }
        faces[F_REG] = *mono;
    }
    mono = &faces[F_REG];
    for (int i = 0; i < NFACE; i++) if (!faces[i].ok) { faces[i] = faces[F_REG]; if (i == F_ITALIC) slant = 1; }
    return 0;
}

int font_ascent(int px)
{
    int a, d, g;
    stbtt_GetFontVMetrics(&mono->info, &a, &d, &g);
    return (int)ceilf(a * stbtt_ScaleForPixelHeight(&mono->info, (float)px));
}
int font_descent(int px)
{
    int a, d, g;
    stbtt_GetFontVMetrics(&mono->info, &a, &d, &g);
    return (int)ceilf(-d * stbtt_ScaleForPixelHeight(&mono->info, (float)px));
}

/* The cache: open addressing on (face, px, code point). */
typedef struct { int key_face, key_px, key_cp, used; glyph_t g; } slot_t;
static slot_t *cache;
static int cap, used;

static uint32_t hkey(int face, int px, int cp) { return (uint32_t)cp * 2654435761u ^ (uint32_t)px * 40503u ^ (uint32_t)face * 97u; }

/* SIEOS: a slant of 1/5 for the oblique face (each row moved right by its
 * height above the baseline / 5, in 1/256 pixels, the coverage shared out). */
static void oblique(glyph_t *g)
{
    int extra = (-g->y0 + 4) / 5 + 1, w = g->w + extra;
    uint8_t *a = xcalloc((size_t)w * g->h, 1);
    for (int y = 0; y < g->h; y++) {
        int sh = (-(g->y0 + y)) * 256 / 5, whole = sh >> 8, frac = sh & 255;
        if (whole < 0) { whole = 0; frac = 0; }
        for (int x = 0; x < g->w; x++) {
            int v = g->a[y * g->w + x], X = x + whole;
            if (X < w) a[y * w + X] += (uint8_t)(v * (256 - frac) >> 8);
            if (X + 1 < w) a[y * w + X + 1] += (uint8_t)(v * frac >> 8);
        }
    }
    free(g->a);
    g->a = a; g->w = w;
}

/* SIEOS: \u2630 (the menu's three bars) when no font has it: drawn. */
static void trigram(glyph_t *g, int px)
{
    int t = px / 10 > 1 ? px / 10 : 1, w = px * 6 / 10, gap = px / 5;
    free(g->a);
    g->w = w; g->h = 2 * gap + t; g->x0 = px / 20; g->y0 = -(px * 7 / 10);
    g->a = xcalloc((size_t)g->w * g->h, 1);
    for (int b = 0; b < 3; b++)
        for (int y = b * gap; y < b * gap + t; y++) memset(g->a + y * g->w, 255, (size_t)g->w);
}

static void render(glyph_t *g, int face, int px, int cp)
{
    face_t *f = &faces[face];
    int gi = stbtt_FindGlyphIndex(&f->info, cp);
    if (!gi)
        for (int i = 0; i < COUNT(fallbacks) && !gi; i++)
            if (fallbacks[i].ok && (gi = stbtt_FindGlyphIndex(&fallbacks[i].info, cp))) f = &fallbacks[i];
    int missing = !gi;
    if (!gi) { f = &faces[face]; gi = stbtt_FindGlyphIndex(&f->info, 0xfffd); if (!gi) gi = stbtt_FindGlyphIndex(&f->info, '?'); }
    /* Every face is scaled to the mono face's line, so mixed text shares a baseline. */
    float s = stbtt_ScaleForPixelHeight(&f->info, (float)px);
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
    g->adv = (int)lroundf(adv * s);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&f->info, gi, s, s, &x0, &y0, &x1, &y1);
    g->w = x1 - x0;
    g->h = y1 - y0;
    g->x0 = x0;
    g->y0 = y0;
    g->a = NULL;
    if (g->w > 0 && g->h > 0) {
        g->a = xcalloc(g->w * g->h, 1);
        stbtt_MakeGlyphBitmap(&f->info, g->a, g->w, g->h, g->w, s, s, gi);
        if (face == F_ITALIC && slant) oblique(g);
    } else g->w = g->h = 0;
    if (cp == 0x2630 && missing) trigram(g, px);
}

const glyph_t *font_glyph(int face, int px, int cp)
{
    if (face < 0 || face >= NFACE) face = F_REG;
    if (px < 4) px = 4;
    if (px > 200) px = 200;
    if (used * 2 >= cap) {                          /* grow (and rehash) */
        int ocap = cap;
        slot_t *old = cache;
        cap = cap ? cap * 2 : 4096;
        cache = xcalloc(cap, sizeof *cache);
        used = 0;
        for (int i = 0; i < ocap; i++)
            if (old[i].used) {
                uint32_t h = hkey(old[i].key_face, old[i].key_px, old[i].key_cp) & (cap - 1);
                while (cache[h].used) h = (h + 1) & (cap - 1);
                cache[h] = old[i];
                used++;
            }
        free(old);
    }
    uint32_t h = hkey(face, px, cp) & (cap - 1);
    while (cache[h].used) {
        slot_t *s = &cache[h];
        if (s->key_face == face && s->key_px == px && s->key_cp == cp) return &s->g;
        h = (h + 1) & (cap - 1);
    }
    slot_t *s = &cache[h];
    s->used = 1;
    s->key_face = face; s->key_px = px; s->key_cp = cp;
    render(&s->g, face, px, cp);
    used++;
    return &s->g;
}
