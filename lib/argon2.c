/*
 * argon2.c - Argon2 password hashing (RFC 9106), version 0x13.
 *
 * Memory is m 1-KiB blocks, in p lanes. The first two blocks of each lane
 * come from a hash of everything (H0); every following block mixes the
 * previous one with an earlier, pseudo-randomly chosen one, so the whole
 * memory must be kept: that is what makes guessing slow and costly. Each
 * lane is cut into 4 slices; t passes go over all of it. The tag is a hash
 * of the last blocks.
 *
 * Which earlier block: in Argon2d it depends on the data (strong against
 * memory-saving attacks), in Argon2i on a counter only (no timing leaks);
 * Argon2id, the one SIEOS uses, does Argon2i for the first half pass, then
 * Argon2d. Lanes are computed one after the other (SIEOS uses p = 1).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk/crypto.h"
#include "mk/lib.h"

typedef struct { uint64_t v[128]; } block;           /* 1 KiB */

static void le32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* H' (RFC 9106, 3.3): a hash of any length, from 64-byte BLAKE2b chained. */
static void hprime(void *out, uint32_t len, const void *in, size_t n)
{
    uint8_t l[4], v[64], *o = out;
    blake2b_t s;
    le32(l, len);
    blake2b_init(&s, len <= 64 ? len : 64, 0, 0);
    blake2b_update(&s, l, 4);
    blake2b_update(&s, in, n);
    blake2b_final(&s, len <= 64 ? out : v);
    if (len <= 64) return;
    for (;;) {                                      /* 32 bytes from each, then a last whole one */
        memcpy(o, v, 32); o += 32; len -= 32;
        if (len <= 64) break;
        blake2b(v, 64, v, 64);
    }
    blake2b(o, len, v, 64);
}

/* The compression function G: R = X ^ Y; the permutation P (BLAKE2b's round
 * with multiplications added, "BlaMka") on its 8 rows, then on its 8
 * columns; the result is R ^ P(R) (^ the old block, from the 2nd pass on). */
#define R64(x, n) ((x) >> (n) | (x) << (64 - (n)))
#define FBM(x, y) ((x) + (y) + 2 * (uint64_t)(uint32_t)(x) * (uint32_t)(y))
#define GB(a, b, c, d) do { a = FBM(a, b); d = R64(d ^ a, 32); c = FBM(c, d); b = R64(b ^ c, 24); \
                            a = FBM(a, b); d = R64(d ^ a, 16); c = FBM(c, d); b = R64(b ^ c, 63); } while (0)
#define P(r, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) do { \
    GB(r[a], r[e], r[i], r[m]); GB(r[b], r[f], r[j], r[n]); GB(r[c], r[g], r[k], r[o]); GB(r[d], r[h], r[l], r[p]); \
    GB(r[a], r[f], r[k], r[p]); GB(r[b], r[g], r[l], r[m]); GB(r[c], r[h], r[i], r[n]); GB(r[d], r[e], r[j], r[o]); } while (0)

static void fill(const block *x, const block *y, block *next, int xor)
{
    block r, t;
    for (int i = 0; i < 128; i++) t.v[i] = r.v[i] = x->v[i] ^ y->v[i];
    if (xor) for (int i = 0; i < 128; i++) t.v[i] ^= next->v[i];
    for (int i = 0; i < 8; i++) { uint64_t *q = r.v + 16 * i; P(q, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); }
    for (int i = 0; i < 8; i++) { uint64_t *q = r.v + 2 * i; P(q, 0, 1, 16, 17, 32, 33, 48, 49, 64, 65, 80, 81, 96, 97, 112, 113); }
    for (int i = 0; i < 128; i++) next->v[i] = t.v[i] ^ r.v[i];
}

/* Argon2i's block indexes: 128 at a time, G(0, G(0, counter block)). */
static void next_addresses(block *addr, block *input, const block *zero)
{
    input->v[6]++;
    fill(zero, input, addr, 0);
    fill(zero, addr, addr, 0);
}

int argon2(const argon2_t *a, void *out, uint32_t outlen, void *mem)
{
    uint32_t lanes = a->p;
    if (!lanes || lanes > 255 || !a->t || a->m_kib < 8 * lanes || outlen < 4 || a->saltlen < 8 || a->type > 2) return -1;
    uint32_t m = a->m_kib / (4 * lanes) * 4 * lanes;   /* rounded to whole slices */
    uint32_t lane_len = m / lanes, seg = lane_len / 4;
    block *B = mem;

    /* H0: a hash of every parameter and input, each length-prefixed. */
    uint8_t h0[72], w[4];
    blake2b_t s;
    blake2b_init(&s, 64, 0, 0);
    uint32_t prm[6] = { lanes, outlen, a->m_kib, a->t, 0x13, a->type };
    for (int i = 0; i < 6; i++) { le32(w, prm[i]); blake2b_update(&s, w, 4); }
    const void *in[4] = { a->pwd, a->salt, a->secret, a->ad };
    uint32_t len[4] = { a->pwdlen, a->saltlen, a->secretlen, a->adlen };
    for (int i = 0; i < 4; i++) { le32(w, len[i]); blake2b_update(&s, w, 4); if (len[i]) blake2b_update(&s, in[i], len[i]); }
    blake2b_final(&s, h0);

    for (uint32_t l = 0; l < lanes; l++)            /* each lane's first two blocks */
        for (uint32_t j = 0; j < 2; j++) {
            le32(h0 + 64, j);
            le32(h0 + 68, l);
            hprime(&B[l * lane_len + j], 1024, h0, 72);
        }

    block zero, input, addr;
    memset(&zero, 0, sizeof zero);
    for (uint32_t r = 0; r < a->t; r++)
        for (uint32_t sl = 0; sl < 4; sl++)
            for (uint32_t l = 0; l < lanes; l++) {
                int indep = a->type == ARGON2_I || (a->type == ARGON2_ID && r == 0 && sl < 2);
                uint32_t i = r == 0 && sl == 0 ? 2 : 0;
                if (indep) {
                    memset(&input, 0, sizeof input);
                    input.v[0] = r; input.v[1] = l; input.v[2] = sl;
                    input.v[3] = m; input.v[4] = a->t; input.v[5] = a->type;
                    if (i) next_addresses(&addr, &input, &zero);
                }
                for (; i < seg; i++) {
                    uint32_t cur = l * lane_len + sl * seg + i;
                    uint32_t prev = cur % lane_len ? cur - 1 : cur + lane_len - 1;
                    uint64_t rnd;
                    if (indep) { if (i % 128 == 0) next_addresses(&addr, &input, &zero); rnd = addr.v[i % 128]; }
                    else rnd = B[prev].v[0];
                    /* the reference lane, and how much of it may be used */
                    uint32_t rl = r == 0 && sl == 0 ? l : (uint32_t)(rnd >> 32) % lanes;
                    uint64_t area;
                    if (r == 0) area = sl == 0 ? i - 1 : rl == l ? sl * seg + i - 1 : sl * seg - (i == 0);
                    else area = rl == l ? lane_len - seg + i - 1 : lane_len - seg - (i == 0);
                    uint64_t x = rnd & 0xFFFFFFFF;
                    x = x * x >> 32;                /* favour recent blocks */
                    uint64_t start = r && sl != 3 ? (sl + 1) * seg : 0;
                    uint32_t ref = (uint32_t)((start + area - 1 - (area * x >> 32)) % lane_len);
                    fill(&B[prev], &B[rl * lane_len + ref], &B[cur], r != 0);
                }
            }

    block c = B[lane_len - 1];                      /* xor of each lane's last block */
    for (uint32_t l = 1; l < lanes; l++)
        for (int k = 0; k < 128; k++) c.v[k] ^= B[l * lane_len + lane_len - 1].v[k];
    hprime(out, outlen, &c, sizeof c);
    wipe(&c, sizeof c); wipe(&input, sizeof input); wipe(&addr, sizeof addr); wipe(h0, sizeof h0); wipe(&s, sizeof s);
    return 0;
}
