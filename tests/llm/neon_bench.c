/*
 * neon_bench.c - How fast sicc's code for the engine's NEON kernels runs
 * (llm/neon.c, plain NEON: the Pi 4's): each format's dot product over
 * 4096-element rows, repeated, timed with the generic timer (the fastest
 * of 7 runs). Under the
 * emulator only the ratios between two builds mean something (sicc's
 * optimizations); on a board, the numbers are the board's. Built and run
 * by tests/llm/neon.sh after the correctness test.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "../../llm/internal.h"
int printf(const char *, ...);

static uint64_t seed = 88172645463325252ULL;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return (uint32_t)seed; }
static uint64_t ticks(void) { uint64_t t; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t)); return t; }
static uint64_t freq(void) { uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f; }

int main(void)
{
    enum { N = 4096, REPS = 300 };
    static uint8_t w[N * 4];
    static float x[N], xq[N * 2];
    const int types[] = { T_Q8_0, T_Q4_0, T_Q4_K, T_Q6_K, T_F16 };
    const char *names[] = { "Q8_0", "Q4_0", "Q4_K", "Q6_K", "F16" };
    for (int i = 0; i < (int)sizeof w; i++) w[i] = rnd() & 0x3f;     /* (small: sane f16 scales too) */
    for (int i = 0; i < N; i++) x[i] = (rnd() % 2000) / 1000.0f - 1;
    double total = 0;
    for (int ti = 0; ti < 5; ti++) {
        int t = types[ti];
        quant_act(tinfo[t].act, x, xq, N);
        mat_t m = { .type = t };
        set_dot(&m, LLM_K_NEON);
        volatile float sink = 0;
        double us = 1e30;
        for (int trial = 0; trial < 7; trial++) {            /* the fastest of 7: the least disturbed */
            uint64_t t0 = ticks();
            for (int r = 0; r < REPS; r++) sink += m.dot(N, w, xq);
            double t = (double)(ticks() - t0) * 1e6 / (double)freq() / REPS;
            if (t < us) us = t;
        }
        total += us;
        printf("bench %-5s neon: %8.1f us per %d-element row\n", names[ti], us, N);
    }
    printf("bench total: %.1f us\n", total);
    return 0;
}
