/* pkg-config: fontconfig harfbuzz-icu harfbuzz freetype2 */
/*
 * wtest-text (make webdeps-test): the rest of the text stack, on the DejaVu
 * fonts in /lib/fonts: Fontconfig finds "sans-serif" and "serif"; FreeType
 * loads the face and renders a glyph; HarfBuzz (with ICU's Unicode
 * functions) shapes text: the "ffi" ligature, kerning, a right-to-left word.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb-icu.h>
#include <hb.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *what, int ok) { printf("wtest text: %s: %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }

static char *match(FcConfig *cfg, const char *family)
{
    FcPattern *p = FcNameParse((const FcChar8 *)family), *m;
    FcResult r;
    FcConfigSubstitute(cfg, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    m = FcFontMatch(cfg, p, &r);
    FcChar8 *file = 0;
    char *out = 0;
    if (m && FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) out = strdup((char *)file);
    FcPatternDestroy(p);
    if (m) FcPatternDestroy(m);
    return out;
}

static unsigned shape(hb_font_t *f, const char *s, hb_direction_t dir, hb_position_t *advance)
{
    hb_buffer_t *b = hb_buffer_create();
    hb_buffer_set_unicode_funcs(b, hb_icu_get_unicode_funcs());
    hb_buffer_add_utf8(b, s, -1, 0, -1);
    hb_buffer_set_direction(b, dir);
    hb_buffer_guess_segment_properties(b);
    hb_shape(f, b, 0, 0);
    unsigned n = hb_buffer_get_length(b);
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(b, 0);
    *advance = 0;
    for (unsigned i = 0; i < n; i++) *advance += pos[i].x_advance;
    hb_buffer_destroy(b);
    return n;
}

int main(void)
{
    FcConfig *cfg = FcConfigCreate();
    const char *conf = "<?xml version=\"1.0\"?><fontconfig><dir>/lib/fonts</dir><cachedir>/tmp/fontconfig</cachedir>"
                       "<alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias>"
                       "<alias><family>serif</family><prefer><family>DejaVu Serif</family></prefer></alias></fontconfig>";
    int ok = FcConfigParseAndLoadFromMemory(cfg, (const FcChar8 *)conf, FcTrue) && FcConfigBuildFonts(cfg);
    char *sans = ok ? match(cfg, "sans-serif") : 0, *serif = ok ? match(cfg, "serif:bold") : 0;
    check("Fontconfig: sans-serif and serif found", sans && strstr(sans, "DejaVuSans.ttf") && serif && strstr(serif, "DejaVuSerif.ttf"));
    FT_Library lib;
    FT_Face face = 0;
    int ft = !FT_Init_FreeType(&lib) && sans && !FT_New_Face(lib, sans, 0, &face) && !FT_Set_Pixel_Sizes(face, 0, 32);
    ft = ft && !FT_Load_Char(face, 'A', FT_LOAD_RENDER) && face->glyph->bitmap.width > 10 && face->glyph->bitmap.rows > 15;
    int ink = 0;
    for (unsigned i = 0; ft && i < face->glyph->bitmap.rows * (unsigned)face->glyph->bitmap.pitch; i++) ink += face->glyph->bitmap.buffer[i] > 128;
    check("FreeType: a face loaded, an 'A' rendered at 32 px", ft && ink > 50);
    hb_font_t *hf = face ? hb_ft_font_create_referenced(face) : 0;
    hb_position_t w1 = 0, w2 = 0;
    unsigned n = hf ? shape(hf, "office", HB_DIRECTION_LTR, &w1) : 0;
    check("HarfBuzz: \"office\" shaped with the ffi ligature (4 glyphs)", n == 4);
    unsigned kern = hf ? shape(hf, "AVA", HB_DIRECTION_LTR, &w2) : 0;
    hb_position_t a1, v1;
    if (hf) { shape(hf, "A", HB_DIRECTION_LTR, &a1); shape(hf, "V", HB_DIRECTION_LTR, &v1); }
    check("HarfBuzz: kerning (AVA narrower than its letters)", kern == 3 && w2 < 2 * a1 + v1);
    hb_position_t w3;
    unsigned rtl = hf ? shape(hf, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", HB_DIRECTION_RTL, &w3) : 0;   /* shalom */
    check("HarfBuzz: a right-to-left word (ICU's script functions)", rtl == 4 && w3 > 0);
    if (hf) hb_font_destroy(hf);
    printf("wtest text: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
