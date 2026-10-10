/*
 * kernels.c - Unit tests of the engine's number formats: for every weight
 * format, random blocks are multiplied by a random vector three ways:
 *   1. unpack the row to floats, unpack the quantized vector, plain dot;
 *   2. the plain C kernel;  3. the AVX2 (and VNNI) kernel.
 * All three must agree (the integer kernels exactly, up to float rounding).
 * Also: half-precision conversions both ways, and activation quantization.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../../llm/internal.h"

static uint64_t seed = 88172645463325252ULL;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return (uint32_t)seed; }
static float frnd(void) { return (rnd() / 4294967296.0f) * 2 - 1; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* make random but sane blocks: random bits, with the f16 scales set to small values */
static void make_blocks(int type, uint8_t *p, int nb)
{
    int bytes = tinfo[type].bytes;
    for (int i = 0; i < nb * bytes; i++) p[i] = rnd();
    for (int b = 0; b < nb; b++, p += bytes) {
        f16 d = f32_to_f16(0.001f + 0.01f * (rnd() % 100) / 100), m = f32_to_f16(-0.005f * (rnd() % 100) / 100);
        switch (type) {
        case T_Q4_0: case T_Q5_0: case T_Q8_0: memcpy(p, &d, 2); break;
        case T_Q4_1: case T_Q5_1: case T_Q4_K: case T_Q5_K: memcpy(p, &d, 2); memcpy(p + 2, &m, 2); break;
        case T_Q6_K: memcpy(p + 208, &d, 2); break;
        }
    }
}

int main(void)
{
    /* half precision round trips, including subnormals and rounding */
    for (int h = 0; h < 0x7C00; h++) {
        float f = f16_to_f32(h);
        CHECK(f32_to_f16(f) == h, "f16 round trip %04x", h);
        CHECK(f16_to_f32(h | 0x8000) == -f, "f16 sign %04x", h);
    }
    CHECK(f16_to_f32(0x3C00) == 1.0f && f16_to_f32(0x0001) == ldexpf(1, -24), "f16 values");
    CHECK(f32_to_f16(65520.0f) == 0x7C00 && f32_to_f16(1e-9f) == 0, "f16 overflow / underflow");

    const int types[] = { T_F32, T_F16, T_Q4_0, T_Q4_1, T_Q5_0, T_Q5_1, T_Q8_0, T_Q4_K, T_Q5_K, T_Q6_K };
    const char *names[] = { "F32", "F16", "Q4_0", "Q4_1", "Q5_0", "Q5_1", "Q8_0", "Q4_K", "Q5_K", "Q6_K" };
    enum { N = 1024 };
    static uint8_t w[N * 4];
    static float x[N], wf[N], xf[N], xq[N * 2];
    int kernels[] = { LLM_K_SCALAR, pick_kernels(LLM_K_AVX2), pick_kernels(LLM_K_AUTO) };
    for (int ti = 0; ti < 10; ti++) {
        int t = types[ti];
        double worst[3] = { 0 };
        for (int rep = 0; rep < 200; rep++) {
            for (int i = 0; i < N; i++) x[i] = frnd() * (rep % 7 + 1);
            if (t == T_F32) for (int i = 0; i < N; i++) ((float *)w)[i] = frnd();
            else if (t == T_F16) for (int i = 0; i < N; i++) ((f16 *)w)[i] = f32_to_f16(frnd());
            else make_blocks(t, w, N / tinfo[t].bs);
            int act = tinfo[t].act;
            quant_act(act, x, xq, N);
            /* 1: everything as floats (the quantized vector unpacked too) */
            dequant_row(t, w, wf, N);
            if (act == A_F32) memcpy(xf, x, sizeof x);
            else if (act == A_Q8) for (int b = 0; b < N / 32; b++) for (int j = 0; j < 32; j++) xf[b * 32 + j] = ((aq8 *)xq)[b].d * ((aq8 *)xq)[b].qs[j];
            else for (int b = 0; b < N / 256; b++) for (int j = 0; j < 256; j++) xf[b * 256 + j] = ((aq8K *)xq)[b].d * ((aq8K *)xq)[b].qs[j];
            double ref = 0, mag = 0;
            for (int i = 0; i < N; i++) { ref += (double)wf[i] * xf[i]; mag += fabs((double)wf[i] * xf[i]); }
            for (int k = 0; k < 3; k++) {
                mat_t m = { .type = t };
                pick_kernels(kernels[k]);
                set_dot(&m, kernels[k]);
                double got = m.dot(N, w, xq), e = fabs(got - ref) / (mag + 1e-9);
                if (e > worst[k]) worst[k] = e;
            }
        }
        /* several vectors at once (prompts): must equal one at a time */
        {
            mat_t m = { .type = t };
            pick_kernels(LLM_K_AUTO); set_dot(&m, pick_kernels(LLM_K_AUTO));
            if (m.dotn) {
                static uint8_t xs[7][N * 2]; float y[7 * 3];
                size_t st = act_bytes(tinfo[t].act, N);
                for (int b = 0; b < 7; b++) { for (int i = 0; i < N; i++) x[i] = frnd() * (b + 1); quant_act(tinfo[t].act, x, xs[0] + b * st, N); }
                m.dotn(N, w, xs[0], st, 7, y, 3);
                for (int b = 0; b < 7; b++) {
                    float one = m.dot(N, w, xs[0] + b * st);
                    CHECK(fabsf(y[b * 3] - one) <= 1e-5f * (fabsf(one) + 1), "%s several-vector kernel, vector %d: %g vs %g", names[ti], b, y[b * 3], one);
                }
                printf("  %-5s several vectors at once: checked\n", names[ti]);
            }
        }
        printf("  %-5s relative error: scalar %.1e, avx2 %.1e, best %.1e\n", names[ti], worst[0], worst[1], worst[2]);
        for (int k = 0; k < 3; k++) CHECK(worst[k] < 1e-5, "%s kernel %d disagrees (%.2e)", names[ti], k, worst[k]);
    }
    /* activation quantization: error bounded by half a step */
    for (int i = 0; i < N; i++) x[i] = frnd() * 3;
    quant_act(A_Q8K, x, xq, N);
    for (int b = 0; b < N / 256; b++) {
        aq8K *a = (aq8K *)xq + b;
        for (int j = 0; j < 256; j++) CHECK(fabsf(a->d * a->qs[j] - x[b * 256 + j]) <= a->d * 0.5f + 1e-6f, "q8K step");
        for (int g = 0; g < 16; g++) { int s = 0; for (int j = 0; j < 16; j++) s += a->qs[g * 16 + j]; CHECK(s == a->bsums[g], "q8K bsums"); }
    }
    printf(fails ? "kernel tests: %d FAILED\n" : "kernel tests: all passed\n", fails);
    return fails != 0;
}
