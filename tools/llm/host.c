/*
 * host.c - The engine's environment on the development machine, in ISO C
 * only: aligned memory, reading the model file, a pool of threads (C11
 * <threads.h>) and a clock (timespec_get).
 *
 * The thread pool: the caller is worker 0; the others wait for a job, spin
 * a little first (a job usually follows quickly), then sleep.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <stdatomic.h>
#include <time.h>
#include "host.h"

static void *h_alloc(void *c, size_t n) { (void)c; return aligned_alloc(64, (n + 63) & ~(size_t)63); }
static void h_free(void *c, void *p) { (void)c; free(p); }

static int h_read(void *c, void *buf, uint64_t off, size_t n)
{
    host_t *h = c;
    /* fseek takes a long: fine for files up to 2^63 on this host */
    if (fseek(h->f, (long)off, SEEK_SET) || fread(buf, 1, n, h->f) != n) return -1;
    return 0;
}

static uint64_t h_now(void *c)
{
    (void)c;
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}

/* ---- the thread pool */
static struct {
    int n;
    thrd_t th[256];
    mtx_t mu; cnd_t cv;
    atomic_uint gen, done;
    void (*fn)(void *, int, int); void *arg; int nt;
} P;

static int worker(void *a)
{
    int i = (int)(intptr_t)a;
    unsigned seen = 0;
    for (;;) {
        unsigned g;
        for (int spin = 0; (g = atomic_load(&P.gen)) == seen && spin < 20000; spin++) ;
        if (g == seen) {
            mtx_lock(&P.mu);
            while ((g = atomic_load(&P.gen)) == seen) cnd_wait(&P.cv, &P.mu);
            mtx_unlock(&P.mu);
        }
        seen = g;
        P.fn(P.arg, i, P.nt);
        atomic_fetch_add(&P.done, 1);
    }
    return 0;
}

static void h_parallel(void *c, void (*fn)(void *, int, int), void *arg, int n)
{
    (void)c;
    P.fn = fn; P.arg = arg; P.nt = n;
    atomic_store(&P.done, 0);
    mtx_lock(&P.mu);
    atomic_fetch_add(&P.gen, 1);
    cnd_broadcast(&P.cv);
    mtx_unlock(&P.mu);
    fn(arg, 0, n);
    while (atomic_load(&P.done) != (unsigned)(n - 1)) ;
}

int host_env(host_t *h, const char *path, int threads)
{
    memset(h, 0, sizeof *h);
    if (!(h->f = fopen(path, "rb"))) return -1;
    fseek(h->f, 0, SEEK_END);
    h->size = (uint64_t)ftell(h->f);
    if (threads < 1) threads = 1;
    if (threads > 256) threads = 256;
    if (threads > 1 && !P.n) {
        mtx_init(&P.mu, mtx_plain); cnd_init(&P.cv);
        for (int i = 1; i < threads; i++) thrd_create(&P.th[i], worker, (void *)(intptr_t)i);
        P.n = threads;
    }
    h->env = (llm_env_t){ .ctx = h, .alloc = h_alloc, .free = h_free, .read = h_read,
                          .parallel = threads > 1 ? h_parallel : 0, .threads = threads, .now_ns = h_now };
    return 0;
}
