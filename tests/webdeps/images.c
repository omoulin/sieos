/* pkg-config: libpng libjpeg libwebp libwebpdemux */
/*
 * wtest-images (make webdeps-test): the image libraries WebKit decodes with:
 * a picture (a colour gradient) written to PNG, JPEG and WebP (lossy and
 * lossless) in memory and read back: sizes, and pixels close to (JPEG,
 * lossy WebP) or equal to (PNG, lossless WebP) the original; the WebP demuxer.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>                                 /* (before jpeglib.h, which needs FILE) */
#include <jpeglib.h>
#include <png.h>
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>

enum { W = 96, H = 64 };
static unsigned char img[W * H * 3];
static int fails;
static void check(const char *what, int ok) { printf("wtest images: %s: %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }
static int diff(const unsigned char *a, const unsigned char *b, int n)     /* the largest difference */
{
    int m = 0;
    for (int i = 0; i < n; i++) { int d = abs(a[i] - b[i]); if (d > m) m = d; }
    return m;
}

/* PNG, through memory */
typedef struct { unsigned char *p; size_t n, cap, at; } mem_t;
static void png_put(png_structp ps, png_bytep d, png_size_t n)
{
    mem_t *m = png_get_io_ptr(ps);
    if (m->n + n > m->cap) { m->cap = (m->n + n) * 2; m->p = realloc(m->p, m->cap); }
    memcpy(m->p + m->n, d, n);
    m->n += n;
}
static void png_get(png_structp ps, png_bytep d, png_size_t n) { mem_t *m = png_get_io_ptr(ps); memcpy(d, m->p + m->at, n); m->at += n; }
static void png_nop(png_structp ps) { (void)ps; }

int main(void)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) { unsigned char *p = img + 3 * (y * W + x); p[0] = x * 255 / W; p[1] = y * 255 / H; p[2] = 128; }
    /* PNG */
    mem_t m = { 0 };
    png_structp w = png_create_write_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    png_infop wi = png_create_info_struct(w);
    png_set_write_fn(w, &m, png_put, png_nop);
    png_set_IHDR(w, wi, W, H, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(w, wi);
    for (int y = 0; y < H; y++) png_write_row(w, img + 3 * W * y);
    png_write_end(w, wi);
    png_destroy_write_struct(&w, &wi);
    png_structp r = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    png_infop ri = png_create_info_struct(r);
    png_set_read_fn(r, &m, png_get);
    png_read_info(r, ri);
    unsigned char back[W * H * 3];
    int pw = png_get_image_width(r, ri), ph = png_get_image_height(r, ri);
    for (int y = 0; y < ph && pw == W && ph == H; y++) png_read_row(r, back + 3 * W * y, 0);
    png_destroy_read_struct(&r, &ri, 0);
    check("PNG: written and read back, the same pixels", pw == W && ph == H && diff(img, back, sizeof img) == 0);
    free(m.p);
    /* JPEG */
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr err;
    unsigned char *jbuf = 0;
    unsigned long jlen = 0;
    c.err = jpeg_std_error(&err);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &jbuf, &jlen);
    c.image_width = W; c.image_height = H; c.input_components = 3; c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 90, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < H) { JSAMPROW row = img + 3 * W * c.next_scanline; jpeg_write_scanlines(&c, &row, 1); }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    struct jpeg_decompress_struct d;
    d.err = jpeg_std_error(&err);
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, jbuf, jlen);
    jpeg_read_header(&d, TRUE);
    jpeg_start_decompress(&d);
    int jw = d.output_width, jh = d.output_height;
    while (d.output_scanline < d.output_height && jw == W) { JSAMPROW row = back + 3 * W * d.output_scanline; jpeg_read_scanlines(&d, &row, 1); }
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
    int jd = diff(img, back, sizeof img);
    check("JPEG: written and read back, pixels close", jw == W && jh == H && jd < 24);
    free(jbuf);
    /* WebP, lossy and lossless; the demuxer */
    uint8_t *wp = 0;
    size_t wl = WebPEncodeRGB(img, W, H, 3 * W, 90, &wp);
    int ww = 0, wh = 0;
    uint8_t *wd = wl ? WebPDecodeRGB(wp, wl, &ww, &wh) : 0;
    check("WebP (lossy): written and read back, pixels close", wd && ww == W && wh == H && diff(img, wd, sizeof img) < 24);
    WebPData data = { wp, wl };
    WebPDemuxer *dm = WebPDemux(&data);
    check("WebP: the demuxer sees one frame", dm && WebPDemuxGetI(dm, WEBP_FF_FRAME_COUNT) == 1 && WebPDemuxGetI(dm, WEBP_FF_CANVAS_WIDTH) == W);
    WebPDemuxDelete(dm);
    WebPFree(wd); WebPFree(wp);
    wl = WebPEncodeLosslessRGB(img, W, H, 3 * W, &wp);
    wd = wl ? WebPDecodeRGB(wp, wl, &ww, &wh) : 0;
    check("WebP (lossless): the same pixels", wd && diff(img, wd, sizeof img) == 0);
    WebPFree(wd); WebPFree(wp);
    printf("wtest images: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
