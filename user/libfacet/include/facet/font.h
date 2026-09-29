/*
 * facet/font.h - TrueType fonts (libfacet).
 *
 * A font is a .ttf file; a face is a font at a pixel size (the em square),
 * with its own glyph cache.  Glyphs are anti-aliased, without hinting.
 * Text is UTF-8 (a byte that does not start a valid sequence stands for
 * itself, as Latin-1).
 *
 *     struct fct_font *f = fct_font_load("/usr/share/fonts/dejavu/DejaVuSans.ttf");
 *     struct fct_face *big = fct_face_new(f, 32);
 *     fct_face_draw(s, big, x, y + fct_face_ascent(big), "Hello", C_TEXT);
 *
 * The desktop's own faces (fct_ui_face) are what gfx_text, gfx_text_bold,
 * text_width and gfx_char draw with: DejaVu Sans, Sans Bold and Sans Mono
 * at 13 px, which fill the 8x16 cells of the older bitmap font.  They are
 * loaded on first use.  FACET_FONT=bitmap keeps the bitmap font, and
 * FACET_FONT_SIZE=N sets the size in pixels.
 */
#ifndef FACET_FONT_H
#define FACET_FONT_H

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fct_font;
struct fct_face;

struct fct_font *fct_font_load(const char *path);         /* NULL if it is not a usable TrueType font */
void fct_font_free(struct fct_font *f);

struct fct_face *fct_face_new(struct fct_font *f, int px);
void fct_face_free(struct fct_face *fc);
int  fct_face_ascent(const struct fct_face *fc);           /* pixels above the baseline */
int  fct_face_descent(const struct fct_face *fc);          /* pixels below it */
int  fct_face_height(const struct fct_face *fc);           /* ascent + descent */

/* Draw text with its baseline at y; returns the advance in pixels. */
int  fct_face_draw(struct surface *s, struct fct_face *fc, int x, int y, const char *utf8, color_t fg);
int  fct_face_draw_cp(struct surface *s, struct fct_face *fc, int x, int y, unsigned cp, color_t fg);
int  fct_face_width(struct fct_face *fc, const char *utf8);
/* How many bytes of utf8 fit in maxw pixels (whole characters). */
size_t fct_face_fit(struct fct_face *fc, const char *utf8, int maxw);
float fct_face_advance(struct fct_face *fc, unsigned cp);
bool fct_face_has(struct fct_face *fc, unsigned cp);      /* the font has a glyph for cp */

enum { FCT_FONT_SANS, FCT_FONT_BOLD, FCT_FONT_MONO, FCT_FONT_MONO_BOLD, FCT_FONT_NFACES };
struct fct_face *fct_ui_face(int which);                  /* NULL: no TrueType fonts (bitmap font) */
struct fct_face *fct_ui_face_px(int which, int px);       /* the same font at another size */

#ifdef __cplusplus
}
#endif

#endif
