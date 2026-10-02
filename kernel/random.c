/*
 * random.c - Kernel random number generator (/dev/random, /dev/urandom).
 *
 * An entropy pool is stirred with interrupt timings (TSC), the real-time
 * clock and, when the CPU has them, RDSEED/RDRAND.  Output comes from a
 * ChaCha20 keystream whose key is re-derived from the pool on every
 * request and replaced after it (forward secrecy).  Reads never block.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "kernel.h"
#include "random.h"
#include "smp.h"

static uint64_t pool[8];
static unsigned pool_idx;
static uint32_t key[8];
static uint64_t counter;
static struct spinlock key_lock;                    /* the key and the counter (the pool is mixed without it) */
static bool have_rdrand, have_rdseed;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static bool hw_random(uint64_t *v)
{
    for (int tries = 0; tries < 10; tries++) {
        unsigned char ok;
        if (have_rdseed)
            __asm__ volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok));
        else if (have_rdrand)
            __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
        else
            return false;
        if (ok)
            return true;
    }
    return false;
}

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROTL32(d, 16); \
    c += d; b ^= c; b = ROTL32(b, 12); \
    a += b; d ^= a; d = ROTL32(d, 8);  \
    c += d; b ^= c; b = ROTL32(b, 7)

static void chacha20_block(const uint32_t k[8], uint64_t ctr, uint32_t out[16])
{
    uint32_t s[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                       k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7],
                       (uint32_t)ctr, (uint32_t)(ctr >> 32), 0x53494541, 0x52414e44 };
    uint32_t x[16];
    memcpy(x, s, sizeof(x));
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++)
        out[i] = x[i] + s[i];
}

/* Mix a sample into the pool (cheap: called from interrupt paths). */
void random_add_entropy(uint64_t v)
{
    uint64_t t = rdtsc();
    unsigned i = pool_idx++ & 7;
    pool[i] = ((pool[i] << 13) | (pool[i] >> 51)) ^ v ^ t;
    pool[(i + 3) & 7] += t * 0x9E3779B97F4A7C15UL;
}

/* Fold the pool into the key. */
static void reseed(void)
{
    uint64_t hw;
    if (hw_random(&hw))
        random_add_entropy(hw);
    random_add_entropy(rdtsc());
    uint32_t mixed[8];
    for (int i = 0; i < 8; i++)
        mixed[i] = key[i] ^ (uint32_t)pool[i] ^ (uint32_t)(pool[(i + 1) & 7] >> 32);
    uint32_t out[16];
    chacha20_block(mixed, ++counter, out);
    memcpy(key, out, sizeof(key));
}

/*
 * The key and a range of counters are taken under the lock (and the key
 * replaced: earlier output cannot be recomputed); the keystream is made
 * outside it, into buf (a user buffer, perhaps).
 */
void random_bytes(void *buf, size_t n)
{
    uint8_t *p = buf;
    uint32_t k[8], out[16];
    uint64_t blocks = (n + sizeof(out) - 1) / sizeof(out);
    spin_lock(&key_lock);
    reseed();
    memcpy(k, key, sizeof(k));
    uint64_t ctr = counter;
    counter += blocks;
    chacha20_block(key, ++counter, out);            /* fresh key */
    memcpy(key, out + 8, sizeof(key));
    spin_unlock(&key_lock);
    while (n) {
        chacha20_block(k, ++ctr, out);
        size_t chunk = MIN(n, sizeof(out));
        memcpy(p, out, chunk);
        p += chunk;
        n -= chunk;
    }
    memset(out, 0, sizeof(out));
    memset(k, 0, sizeof(k));
}

void random_init(void)
{
    uint32_t r[4];
    cpuid(0, 0, r);
    uint32_t max_leaf = r[0];
    cpuid(1, 0, r);
    have_rdrand = r[2] & (1u << 30);
    if (max_leaf >= 7) {
        cpuid(7, 0, r);
        have_rdseed = r[1] & (1u << 18);
    }
    random_add_entropy(rtc_unix_time());
    for (int i = 0; i < 16; i++) {
        uint64_t hw = 0;
        hw_random(&hw);
        random_add_entropy(hw ^ ((uint64_t)i << 56));
    }
    spin_lock(&key_lock);
    reseed();
    spin_unlock(&key_lock);
}

bool random_hw_available(void)
{
    return have_rdrand || have_rdseed;
}
