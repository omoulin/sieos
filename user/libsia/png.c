/*
 * png.c - PNG images (for models that see them): screenshots of
 * applications, the vision test's picture.  RGB, 8 bits a channel; each
 * row filtered with None, Sub or Up (the smallest), then deflated with
 * the fixed Huffman codes and runs of repeated bytes - flat interfaces
 * shrink to a few percent.  And base64, for data: URLs.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "libsia.h"

static uint32_t crc_table[256];

static uint32_t crc32_of(const unsigned char *p, size_t n, uint32_t crc)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}

static void put32(struct sbuf *b, uint32_t v)
{
    char x[4] = { (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v };
    sb_putn(b, x, 4);
}

static void chunk(struct sbuf *out, const char *type, const unsigned char *data, size_t n)
{
    put32(out, (uint32_t)n);
    size_t at = out->len;
    sb_putn(out, type, 4);
    if (n)
        sb_putn(out, (const char *)data, n);
    put32(out, crc32_of((const unsigned char *)out->s + at, n + 4, 0));
}

/* ---------------- deflate (fixed codes, runs) ---------------- */

struct bits {
    struct sbuf *out;
    uint32_t acc;
    int n;
};

static void put_bits(struct bits *b, uint32_t v, int n)        /* least significant bit first */
{
    b->acc |= v << b->n;
    b->n += n;
    while (b->n >= 8) {
        sb_putc(b->out, (char)(b->acc & 255));
        b->acc >>= 8;
        b->n -= 8;
    }
}

static void put_code(struct bits *b, uint32_t code, int n)     /* Huffman codes: most significant first */
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++)
        r |= ((code >> i) & 1) << (n - 1 - i);
    put_bits(b, r, n);
}

static void literal(struct bits *b, int v)
{
    if (v < 144)
        put_code(b, 0x30 + v, 8);
    else if (v < 256)
        put_code(b, 0x190 + v - 144, 9);
    else if (v < 280)
        put_code(b, v - 256, 7);
    else
        put_code(b, 0xC0 + v - 280, 8);
}

static const int len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                  67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const int len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                   4, 4, 4, 4, 5, 5, 5, 5, 0 };

/* A copy of len (3..258) bytes from dist back (1 or 3 here: distance codes 0 and 2) */
static void match(struct bits *b, int len, int dist)
{
    int i = 28;
    while (len_base[i] > len)
        i--;
    literal(b, 257 + i);
    if (len_extra[i])
        put_bits(b, len - len_base[i], len_extra[i]);
    put_code(b, dist == 1 ? 0 : 2, 5);
}

static void deflate_fixed(struct sbuf *out, const unsigned char *d, size_t n)
{
    struct bits b = { out, 0, 0 };
    put_bits(&b, 1, 1);                                 /* the last block */
    put_bits(&b, 1, 2);                                 /* fixed codes */
    size_t i = 0;
    while (i < n) {
        int best = 0, dist = 0;
        for (int k = 1; k <= 3; k += 2) {                /* repeats of the last byte, or of the last pixel */
            if (i < (size_t)k)
                continue;
            int r = 0;
            while (i + r < n && r < 258 && d[i + r] == d[i + r - k])
                r++;
            if (r > best)
                best = r, dist = k;
        }
        if (best >= 3) {
            match(&b, best, dist);
            i += best;
        } else {
            literal(&b, d[i++]);
        }
    }
    literal(&b, 256);                                   /* end of block */
    if (b.n)
        put_bits(&b, 0, 8 - b.n);
}

/* ---------------- the image ---------------- */

void sia_png(const uint32_t *px, int w, int h, int stride, struct sbuf *out)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    sb_putn(out, (const char *)sig, 8);
    unsigned char ihdr[13] = { (unsigned char)(w >> 24), (unsigned char)(w >> 16), (unsigned char)(w >> 8),
                               (unsigned char)w, (unsigned char)(h >> 24), (unsigned char)(h >> 16),
                               (unsigned char)(h >> 8), (unsigned char)h, 8, 2, 0, 0, 0 };
    chunk(out, "IHDR", ihdr, 13);
    size_t row = (size_t)w * 3, raw_n = (row + 1) * h;
    unsigned char *raw = malloc(raw_n), *cur = malloc(row), *prev = calloc(1, row), *trial = malloc(row);
    if (!raw || !cur || !prev || !trial) {
        free(raw), free(cur), free(prev), free(trial);
        return;
    }
    unsigned char *o = raw;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * stride + x];
            cur[x * 3] = p >> 16, cur[x * 3 + 1] = p >> 8, cur[x * 3 + 2] = p;
        }
        int best_f = 0;
        unsigned long best_sum = ~0ul;
        for (int f = 0; f < 3; f++) {                     /* None, Sub, Up: the smallest sum wins */
            unsigned long sum = 0;
            for (size_t i = 0; i < row; i++) {
                unsigned char v = f == 0 ? cur[i] : f == 1 ? (unsigned char)(cur[i] - (i >= 3 ? cur[i - 3] : 0))
                                                           : (unsigned char)(cur[i] - prev[i]);
                sum += v < 128 ? v : 256 - v;
            }
            if (sum < best_sum)
                best_sum = sum, best_f = f;
        }
        *o++ = (unsigned char)best_f;
        for (size_t i = 0; i < row; i++)
            *o++ = best_f == 0 ? cur[i] : best_f == 1 ? (unsigned char)(cur[i] - (i >= 3 ? cur[i - 3] : 0))
                                                      : (unsigned char)(cur[i] - prev[i]);
        unsigned char *t = prev;
        prev = cur;
        cur = t;
    }
    struct sbuf z;
    sb_init(&z);
    sb_putc(&z, 0x78);                                  /* zlib: deflate, 32K window */
    sb_putc(&z, 0x01);
    deflate_fixed(&z, raw, raw_n);
    uint32_t a = 1, bsum = 0;                           /* Adler-32 */
    for (size_t i = 0; i < raw_n; i++) {
        a = (a + raw[i]) % 65521;
        bsum = (bsum + a) % 65521;
    }
    put32(&z, bsum << 16 | a);
    chunk(out, "IDAT", (const unsigned char *)z.s, z.len);
    chunk(out, "IEND", NULL, 0);
    sb_free(&z);
    free(raw), free(cur), free(prev), free(trial);
}

void sb_base64(struct sbuf *out, const unsigned char *d, size_t n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        char q[4] = { t[v >> 18 & 63], t[v >> 12 & 63], i + 1 < n ? t[v >> 6 & 63] : '=', i + 2 < n ? t[v & 63] : '=' };
        sb_putn(out, q, 4);
    }
}
