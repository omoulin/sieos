/*
 * zlib.c - zlib streams (RFC 1950) of deflate data (RFC 1951), and CRC-32:
 * what git compresses its objects and packs with.
 *
 * Deflate data is a series of blocks: stored (raw bytes), or Huffman coded
 * with the standard ("fixed") codes or codes sent before the block
 * ("dynamic"). The symbols are bytes, the end of the block, or a match: a
 * length (3..258) and a distance back (1..32768) into what came before,
 * copied. Inflating reads all three kinds; the codes are canonical, so a
 * table of how many codes each length has, and the symbols in order, is
 * enough to decode them a bit at a time. The whole output is in memory:
 * it is also the window matches copy from.
 *
 * Deflating writes one fixed-code block: matches are found with a hash of
 * the next 3 bytes, which remembers the last place each hash was seen.
 * zlib wraps it all: a 2-byte header, the data, an Adler-32 checksum.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk/zlib.h"
#include "mk/lib.h"

/* The lengths and distances of matches: code i is base[i] + extra bits. */
static uint16_t lbase[29], dbase[30];
static uint8_t lext[29], dext[30];
static void tables(void)
{
    if (lbase[0]) return;
    for (int i = 0, b = 3; i < 28; b += 1 << lext[i], i++) { lext[i] = (uint8_t)(i < 8 ? 0 : (i - 4) / 4); lbase[i] = (uint16_t)b; }
    lbase[28] = 258;
    for (int i = 0, b = 1; i < 30; b += 1 << dext[i], i++) { dext[i] = (uint8_t)(i < 4 ? 0 : (i - 2) / 2); dbase[i] = (uint16_t)b; }
}

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        size_t c = n < 5552 ? n : 5552;          /* (the most before b could overflow) */
        for (n -= c; c--; ) { a += *p++; b += a; }
        a %= 65521; b %= 65521;
    }
    return b << 16 | a;
}

/* ---- Inflate */
typedef struct {
    const uint8_t *in; size_t n, pos;
    uint32_t bb; int bc;                         /* bits read ahead, how many */
    uint8_t *out; size_t cap, len;
    int err;                                     /* the input ended (or bad data) */
} zin_t;
typedef struct { uint16_t count[16], sym[288]; } huff_t;

static uint32_t bits(zin_t *s, int need)
{
    while (s->bc < need) {
        if (s->pos == s->n) { s->err = 1; return 0; }
        s->bb |= (uint32_t)s->in[s->pos++] << s->bc;
        s->bc += 8;
    }
    uint32_t v = s->bb & ((1U << need) - 1);
    s->bb >>= need; s->bc -= need;
    return v;
}

/* Codes from their lengths -> 0 (a few unused codes are allowed), -1 if too many. */
static int build(huff_t *h, const uint8_t *len, int n)
{
    uint16_t off[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    int left = 1;
    for (int l = 1; l < 16; l++) if ((left = 2 * left - h->count[l]) < 0) return -1;
    off[1] = 0;
    for (int l = 1; l < 15; l++) off[l + 1] = off[l] + h->count[l];
    for (int i = 0; i < n; i++) if (len[i]) h->sym[off[len[i]]++] = (uint16_t)i;
    return 0;
}

static int decode(zin_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)bits(s, 1);
        int c = h->count[l];
        if (code - c < first) return h->sym[index + code - first];
        index += c; first = (first + c) << 1; code <<= 1;
        if (s->err) break;
    }
    s->err = 1;
    return -1;
}

/* One Huffman-coded block -> 0, 1: out is full, -1: bad data. */
static int codes(zin_t *s, const huff_t *lit, const huff_t *dist)
{
    for (;;) {
        int sym = decode(s, lit);
        if (s->err) return -1;
        if (sym < 256) {
            if (s->len == s->cap) return 1;
            s->out[s->len++] = (uint8_t)sym;
        } else if (sym == 256) return 0;
        else {
            if ((sym -= 257) >= 29) return -1;
            size_t l = lbase[sym] + bits(s, lext[sym]);
            int d = decode(s, dist);
            if (d < 0 || d >= 30) return -1;
            size_t dd = dbase[d] + bits(s, dext[d]);
            if (s->err || dd > s->len) return -1;
            for (; l; l--) {
                if (s->len == s->cap) return 1;
                s->out[s->len] = s->out[s->len - dd];
                s->len++;
            }
        }
    }
}

static int dynamic(zin_t *s, huff_t *lit, huff_t *dist)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t len[320];
    int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) return -1;
    memset(len, 0, 19);
    for (int i = 0; i < ncode; i++) len[order[i]] = (uint8_t)bits(s, 3);
    if (build(lit, len, 19)) return -1;
    for (int i = 0; i < nlen + ndist; ) {
        int sym = decode(s, lit), rep = 0, v = 0;
        if (s->err) return -1;
        if (sym < 16) { len[i++] = (uint8_t)sym; continue; }
        if (sym == 16) { if (!i) return -1; v = len[i - 1]; rep = 3 + (int)bits(s, 2); }
        else if (sym == 17) rep = 3 + (int)bits(s, 3);
        else rep = 11 + (int)bits(s, 7);
        if (i + rep > nlen + ndist) return -1;
        while (rep--) len[i++] = (uint8_t)v;
    }
    if (!len[256] || build(lit, len, nlen) || build(dist, len + nlen, ndist)) return -1;
    return 0;
}

long zinflate(const void *in, size_t n, void *out, size_t cap, size_t *used)
{
    static huff_t lit, dist;                     /* (1.2 KB: not on the stack) */
    zin_t s = { .in = in, .n = n, .out = out, .cap = cap };
    const uint8_t *p = in;
    *used = 0;
    tables();
    if (n < 6 || (p[0] & 15) != 8 || (p[0] << 8 | p[1]) % 31 || p[1] & 32) return -1;
    s.pos = 2;
    for (int last = 0; !last; ) {
        last = (int)bits(&s, 1);
        int type = (int)bits(&s, 2), r = 0;
        if (type == 0) {                             /* stored: the length, its complement, the bytes */
            s.bb = 0; s.bc = 0;
            if (s.pos + 4 > n) return -1;
            size_t l = p[s.pos] | p[s.pos + 1] << 8;
            if ((l ^ (p[s.pos + 2] | p[s.pos + 3] << 8)) != 0xffff) return -1;
            s.pos += 4;
            if (s.pos + l > n) return -1;
            size_t c = l < cap - s.len ? l : cap - s.len;
            memcpy(s.out + s.len, p + s.pos, c);
            s.len += c; s.pos += l;
            if (c < l) return (long)s.len;
        } else if (type == 1) {                      /* the fixed codes */
            uint8_t len[318];
            memset(len, 8, 144); memset(len + 144, 9, 112); memset(len + 256, 7, 24); memset(len + 280, 8, 8);
            memset(len + 288, 5, 30);
            build(&lit, len, 288); build(&dist, len + 288, 30);
            r = codes(&s, &lit, &dist);
        } else if (type == 2) {
            if (dynamic(&s, &lit, &dist)) return -1;
            r = codes(&s, &lit, &dist);
        } else return -1;
        if (r < 0 || s.err) return -1;
        if (r) return (long)s.len;                   /* out is full: no *used */
    }
    s.pos -= (size_t)s.bc / 8;                       /* (bytes read ahead, unused) */
    if (s.pos + 4 > n) return -1;
    const uint8_t *a = p + s.pos;
    if (((uint32_t)a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3]) != adler32(s.out, s.len)) return -1;
    *used = s.pos + 4;
    return (long)s.len;
}

/* ---- Deflate */
typedef struct { uint8_t *o; size_t n; uint32_t bb; int bc; } zout_t;

static void put(zout_t *z, uint32_t v, int nb)            /* nb bits of v, low bit first */
{
    z->bb |= v << z->bc;
    for (z->bc += nb; z->bc >= 8; z->bc -= 8) { z->o[z->n++] = (uint8_t)z->bb; z->bb >>= 8; }
}
static void huff(zout_t *z, uint32_t code, int nb)        /* a Huffman code: high bit first */
{
    uint32_t r = 0;
    for (int i = 0; i < nb; i++) r |= (code >> i & 1) << (nb - 1 - i);
    put(z, r, nb);
}
static void sym(zout_t *z, int c)                         /* a literal/length symbol, fixed code */
{
    if (c < 144) huff(z, 0x30 + c, 8);
    else if (c < 256) huff(z, 0x190 + c - 144, 9);
    else if (c < 280) huff(z, c - 256, 7);
    else huff(z, 0xc0 + c - 280, 8);
}

#define HBITS 14
size_t zdeflate(const void *in, size_t n, void *out)
{
    static int32_t head[1 << HBITS];             /* a hash's last place + 1 (0: none) */
    const uint8_t *p = in;
    zout_t z = { .o = out };
    tables();
    memset(head, 0, sizeof head);
    z.o[0] = 0x78; z.o[1] = 0x01; z.n = 2;       /* deflate, 32 KiB window, "fastest" */
    put(&z, 1, 1); put(&z, 1, 2);                /* the last block, fixed codes */
    for (size_t i = 0; i < n; ) {
        size_t best = 0, dist = 0;
        if (i + 3 <= n) {
            uint32_t h = (uint32_t)(p[i] << 16 | p[i + 1] << 8 | p[i + 2]) * 2654435761U >> (32 - HBITS);
            size_t c = (size_t)head[h];
            head[h] = (int32_t)(i + 1);
            if (c-- && i - c <= 32768) {
                size_t max = n - i < 258 ? n - i : 258, l = 0;
                while (l < max && p[c + l] == p[i + l]) l++;
                if (l >= 3) { best = l; dist = i - c; }
            }
        }
        if (!best) { sym(&z, p[i++]); continue; }
        int k = 28;
        while (lbase[k] > best) k--;
        sym(&z, 257 + k); put(&z, (uint32_t)(best - lbase[k]), lext[k]);
        int d = 29;
        while (dbase[d] > dist) d--;
        huff(&z, (uint32_t)d, 5); put(&z, (uint32_t)(dist - dbase[d]), dext[d]);
        for (size_t e = i + best, j = i + 1; j < e && j + 3 <= n; j++)   /* what the match covered: hashed too */
            head[(uint32_t)(p[j] << 16 | p[j + 1] << 8 | p[j + 2]) * 2654435761U >> (32 - HBITS)] = (int32_t)(j + 1);
        i += best;
    }
    sym(&z, 256);
    if (z.bc) put(&z, 0, 8 - z.bc);
    uint32_t a = adler32(p, n);
    for (int i = 3; i >= 0; i--) z.o[z.n++] = (uint8_t)(a >> (8 * i));
    return z.n;
}

/* ---- CRC-32: the reflected polynomial 0xedb88320, a table built on first use. */
uint32_t crc32(uint32_t crc, const void *p, size_t n)
{
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ c >> 1 : c >> 1;
            t[i] = c;
        }
    const uint8_t *b = p;
    crc = ~crc;
    while (n--) crc = t[(crc ^ *b++) & 255] ^ crc >> 8;
    return ~crc;
}
