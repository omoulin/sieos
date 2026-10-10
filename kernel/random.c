/*
 * random.c - Unpredictable numbers for programs (SYS_RANDOM): salts for
 * passwords, keys later.
 *
 * It lives in the kernel because the kernel is the only place that sees
 * every interrupt on every CPU (their exact timing is a source of
 * unpredictability), and every program needs it without depending on a
 * server.
 *
 *   sources ──► pool (BLAKE2b, hashes everything)
 *                 │  reseed: key = hash(pool, old key)
 *                 ▼
 *   key ──► ChaCha20 ──► output; then a fresh key from the same stream
 *
 * Sources: the CPU's own generator when present (arch_hw_random: RDSEED,
 * else RDRAND, on x86-64);
 * the timing jitter of a small memory-touching loop at boot; and the time
 * of interrupts. After every request the key is replaced ("fast key
 * erasure"), so a later look at the kernel's memory cannot recover earlier
 * output. Until enough is gathered (256 bits, counted cautiously), the
 * call fails with -EAGAIN rather than give guessable bytes.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "mk/crypto.h"

#ifndef ARCH_FINE_TICKS
#define ARCH_FINE_TICKS() ticks()    /* the finest clock for timing samples */
#endif
#ifndef ARCH_JITTER_LOOP
#define ARCH_JITTER_LOOP 64          /* steps per timing sample: enough for a GHz clock (x86-64's TSC) */
#endif

static blake2b_t pool;
static uint32_t key[8];
static uint64_t gen;                 /* one ChaCha20 nonce per request */
static int bits, seeded;
static uint64_t acc, nacc, last_reseed;

static void reseed(void)
{
    blake2b_t s = pool;              /* the pool keeps gathering; we hash a copy */
    uint8_t k[32];
    uint64_t v;
    for (int i = 0; i < 4; i++) if (arch_hw_random(&v)) blake2b_update(&s, &v, 8);
    blake2b_update(&s, key, sizeof key);
    blake2b_final(&s, k);
    memcpy(key, k, sizeof key);
    wipe(k, sizeof k); wipe(&s, sizeof s);
    last_reseed = ticks();
}

void rand_init(void)
{
    const char *hw = arch_hw_random_init();   /* the CPU's generator, if any */
    blake2b_init(&pool, 32, 0, 0);

    uint64_t v;                      /* 1. the CPU's generator: 512 bits, credited 256 */
    int got = 0;
    for (int i = 0; i < 8; i++) if (arch_hw_random(&v)) { blake2b_update(&pool, &v, 8); got++; }
    if (got == 8) bits += 256;

    /* 2. jitter: how long a small walk through memory takes varies with
     * caches and the machine's activity. Credited 1 bit per 16 samples,
     * and only if the timings really vary. */
    uint64_t pa = page_alloc(1), seen[4] = { 0 }, x = ticks();
    uint8_t *buf = P2V(pa);          /* a scratch page, given back below */
    if (!pa) panic("random: no memory\n");
    int distinct = 0;
    for (int i = 0; i < 4096; i++) {
        uint64_t t0 = ARCH_FINE_TICKS();
        for (int j = 0; j < ARCH_JITTER_LOOP; j++) { x = x * 6364136223846793005UL + 1; buf[x >> 52] += (uint8_t)j; }
        uint64_t d = ARCH_FINE_TICKS() - t0;
        blake2b_update(&pool, &d, sizeof d);
        if (!(seen[d >> 6 & 3] >> (d & 63) & 1)) { seen[d >> 6 & 3] |= 1UL << (d & 63); distinct++; }
    }
    page_free(pa, 1);
    if (distinct >= 16) bits += 4096 / 16;
    reseed();
    seeded = bits >= 256;
    kprintf("mk: random: %s%s, %d timing values: %s\n", hw ? hw : "no CPU generator",
            !hw || got == 8 ? "" : " (failed)", distinct, seeded ? "ready" : "waiting for more interrupts");
}

/* Every interrupt: its time (and kind), folded 32 at a time into the pool,
 * credited 1 bit per 32 (caution: in a virtual machine they are regular). */
void rand_event(uint64_t x)
{
    acc = (acc << 13 | acc >> 51) ^ x ^ ticks();
    if (++nacc % 32) return;
    blake2b_update(&pool, &acc, sizeof acc);
    if (bits < 4096) bits++;
    if (!seeded && bits >= 256) { reseed(); seeded = 1; }
}

/* SYS_RANDOM: n bytes (at most 4096) to the caller's buffer. */
long rand_get(uint64_t ubuf, uint64_t n)
{
    if (!seeded) return -EAGAIN;
    if (n > 4096) return -EINVAL;
    if (ticks() - last_reseed > ns_to_ticks(1000000000)) reseed();   /* at most every second */
    uint32_t nonce[3] = { (uint32_t)gen, (uint32_t)(gen >> 32), 0 };
    gen++;
    uint8_t blk[64];
    long ret = (long)n;
    uint32_t ctr = 1;                /* block 0 becomes the next key, below */
    for (uint64_t off = 0; off < n; off += 64) {
        chacha20_block(key, ctr++, nonce, blk);
        uint64_t c = n - off < 64 ? n - off : 64;
        if (vm_copy(cur->proc->as, (void *)(ubuf + off), 0, blk, c)) { ret = -EFAULT; break; }
    }
    chacha20_block(key, 0, nonce, blk);
    memcpy(key, blk, sizeof key);    /* fast key erasure */
    wipe(blk, sizeof blk);
    return ret;
}
