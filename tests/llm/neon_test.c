/*
 * neon_test.c - The AArch64 NEON kernels (llm/neon.c, plain and sdot)
 * against the plain C ones, on random blocks: every format, the
 * several-vectors kernels, and the attention's half-precision helpers.
 * Built by sicc for AArch64 and run under the emulator (tests/llm/neon.sh).
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "../../llm/internal.h"
int printf(const char *, ...);

static uint64_t seed = 88172645463325252ULL;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return (uint32_t)seed; }
static float frnd(void) { return (rnd() / 4294967296.0f) * 2 - 1; }
static double absd(double x) { return x < 0 ? -x : x; }
static int fails;

static void make_blocks(int type, uint8_t *p, int nb)          /* random bits, sane f16 scales */
{
    int bytes = tinfo[type].bytes;
    for (int i = 0; i < nb * bytes; i++) p[i] = rnd();
    for (int b = 0; b < nb; b++, p += bytes) {
        f16 d = f32_to_f16(0.001f + 0.01f * (rnd() % 100) / 100), m = f32_to_f16(-0.005f * (rnd() % 100) / 100);
        if (type == T_Q4_0 || type == T_Q8_0) memcpy(p, &d, 2);
        if (type == T_Q4_K) { memcpy(p, &d, 2); memcpy(p + 2, &m, 2); }
        if (type == T_Q6_K) memcpy(p + 208, &d, 2);
    }
}

int main(void)
{
    enum { N = 1024, B = 5 };
    static uint8_t w[N * 4];
    static float x[B][N], xq[B][N * 2], y[B];
    const int types[] = { T_F32, T_F16, T_Q8_0, T_Q4_0, T_Q4_K, T_Q6_K };
    const char *names[] = { "F32", "F16", "Q8_0", "Q4_0", "Q4_K", "Q6_K" };
    const char *kn[] = { "neon", "neon+dot" };
    for (int ti = 0; ti < 6; ti++) {
        int t = types[ti], act = tinfo[t].act;
        double worst[2] = { 0 }, worstn[2] = { 0 };
        for (int rep = 0; rep < 40; rep++) {
            for (int b = 0; b < B; b++) for (int i = 0; i < N; i++) x[b][i] = frnd() * (rep % 7 + 1);
            if (t == T_F32) for (int i = 0; i < N; i++) ((float *)w)[i] = frnd();
            else if (t == T_F16) for (int i = 0; i < N; i++) ((f16 *)w)[i] = f32_to_f16(frnd());
            else make_blocks(t, w, N / tinfo[t].bs);
            for (int b = 0; b < B; b++) quant_act(act, x[b], xq[b], N);
            mat_t m = { .type = t };
            set_dot(&m, LLM_K_SCALAR);
            float ref = m.dot(N, w, xq[0]), mag = 0;
            for (int i = 0; i < N; i++) mag += absd(x[0][i]);
            for (int k = 0; k < 2; k++) {
                set_dot(&m, k ? LLM_K_DOT : LLM_K_NEON);
                double e = absd(m.dot(N, w, xq[0]) - ref) / (mag + 1e-9);
                if (e > worst[k]) worst[k] = e;
                if (m.dotn) {                          /* several vectors = one at a time */
                    m.dotn(N, w, xq, sizeof xq[0], B, y, 1);
                    for (int b = 0; b < B; b++) { double d = absd(y[b] - m.dot(N, w, xq[b])) / (mag + 1e-9); if (d > worstn[k]) worstn[k] = d; }
                }
            }
        }
        for (int k = 0; k < 2; k++) {
            int ok = worst[k] < 1e-6 && worstn[k] < 1e-6;
            fails += !ok;
            printf("%-5s %-8s: worst relative difference %.1e (several vectors: %.1e) %s\n", names[ti], kn[k], worst[k], worstn[k], ok ? "ok" : "FAIL");
        }
    }
    /* the attention's helpers: half precision dot, y += a*x, float -> half */
    static float a[333], yv[333], yr[333]; static f16 h[333], h2[333], h3[333];
    for (int i = 0; i < 333; i++) { a[i] = frnd() * 10; h[i] = f32_to_f16(frnd() * 3); yv[i] = yr[i] = frnd(); }
    for (int k = 0; k < 2; k++) {
        pick_kernels(LLM_K_SCALAR);
        float rd = dot_f32_f16(333, a, h);
        to_f16(333, a, h2);
        for (int i = 0; i < 333; i++) yr[i] = yv[i];
        axpy_f16(333, 1.5f, h, yr);
        pick_kernels(k ? LLM_K_DOT : LLM_K_NEON);
        float nd = dot_f32_f16(333, a, h);
        to_f16(333, a, h3);
        static float yn[333];
        for (int i = 0; i < 333; i++) yn[i] = yv[i];
        axpy_f16(333, 1.5f, h, yn);
        int same = 1, close = 1;
        for (int i = 0; i < 333; i++) { same &= h2[i] == h3[i]; close &= absd(yn[i] - yr[i]) <= 1e-5 * (absd(yr[i]) + 1); }
        int ok = absd(nd - rd) <= 1e-4 * (absd(rd) + 1) && same && close;
        fails += !ok;
        printf("f16 helpers %-8s: dot %s, to_f16 %s, axpy %s\n", kn[k], absd(nd - rd) <= 1e-4 * (absd(rd) + 1) ? "ok" : "FAIL",
               same ? "ok" : "FAIL", close ? "ok" : "FAIL");
    }
    printf("neon kernels: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
