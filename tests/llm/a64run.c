/*
 * a64run.c - The engine on AArch64 under the emulator, for tests: load a
 * model, then print the logits of a text, a greedy decoding, or the speed.
 *   a64run MODEL KERNEL logits TEXT OUT | greedy N TEXT | bench
 * KERNEL: scalar, neon or dot. One thread. Test infrastructure only (it uses
 * the emulator's clock system call).
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stddef.h>
#include <stdint.h>
#include "../../llm/llm.h"
typedef struct FILE FILE;
FILE *fopen(const char *, const char *); size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *); int fclose(FILE *);
int printf(const char *, ...); void *malloc(size_t); int strcmp(const char *, const char *); size_t strlen(const char *);
void *memcpy(void *, const void *, size_t);
static int num(const char *p) { int n = 0; while (*p >= '0' && *p <= '9') n = n * 10 + *p++ - '0'; return n; }

size_t strlcpy(char *d, const char *s, size_t n)       /* (the engine's one function the test C library lacks) */
{
    size_t l = strlen(s);
    if (n) { size_t c = l < n ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
static uint8_t *file; static uint64_t size;
static void *e_alloc(void *c, size_t n) { (void)c; uint8_t *p = malloc(n + 64); return p ? (void *)(((uintptr_t)p + 63) & ~(uintptr_t)63) : 0; }
static void e_free(void *c, void *p) { (void)c; (void)p; }
static int e_read(void *c, void *b, uint64_t off, size_t n) { (void)c; if (off + n > size) return -1; memcpy(b, file + off, n); return 0; }
static uint64_t e_now(void *c)
{
    (void)c;
    struct { long s, ns; } t;
    register long x8 __asm__("x8") = 113, x0 __asm__("x0") = 1, x1 __asm__("x1") = (long)&t;   /* clock_gettime(MONOTONIC) */
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return (uint64_t)t.s * 1000000000u + t.ns;
}

int main(int argc, char **argv)
{
    if (argc < 4) { printf("usage: a64run MODEL scalar|neon|dot logits TEXT OUT | greedy N TEXT | bench\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("a64run: cannot open %s\n", argv[1]); return 1; }
    size_t cap = 1 << 20;
    file = malloc(cap);
    for (size_t r; (r = fread(file + size, 1, cap - size, f)) > 0; ) {
        size += r;
        if (size == cap) { uint8_t *g = malloc(cap * 2); memcpy(g, file, size); file = g; cap *= 2; }
    }
    fclose(f);
    llm_env_t env = { 0 };
    env.alloc = e_alloc; env.free = e_free; env.read = e_read; env.threads = 1; env.now_ns = e_now;
    env.kernel = !strcmp(argv[2], "scalar") ? LLM_K_SCALAR : !strcmp(argv[2], "dot") ? LLM_K_DOT : LLM_K_NEON;
    env.ctx_len = 512;
    llm_t *m; char err[128];
    if (llm_open(&m, &env, size, err, sizeof err)) { printf("a64run: %s\n", err); return 1; }
    llm_info_t in; llm_info(m, &in);
    llm_session_t *s = llm_session_new(m, 0);
    int32_t t[512];
    if (!strcmp(argv[3], "logits") && argc > 5) {
        int n = llm_tokenize(m, argv[4], strlen(argv[4]), t, 512, 1);
        const float *lg = llm_eval(s, t, n);
        FILE *o = fopen(argv[5], "wb");
        fwrite(lg, 4, in.n_vocab, o); fclose(o);
    } else if (!strcmp(argv[3], "greedy") && argc > 5) {
        int n = llm_tokenize(m, argv[5], strlen(argv[5]), t, 512, 1), k = num(argv[4]);
        const float *lg = llm_eval(s, t, n);
        for (int j = 0; j < k; j++) {
            int32_t b = 0;
            for (int i = 1; i < in.n_vocab; i++) if (lg[i] > lg[b]) b = i;
            printf("%d%c", b, j + 1 < k ? ' ' : '\n');
            lg = llm_eval(s, &b, 1);
        }
    } else {
        for (int i = 0; i < 96; i++) t[i] = 100 + (i * 37) % 1000;
        uint64_t a = e_now(0); llm_eval(s, t, 64);
        uint64_t b = e_now(0); for (int i = 0; i < 16; i++) llm_eval(s, t + 64 + i, 1);
        uint64_t c = e_now(0);
        printf("%s: prompt %.2f tok/s, generation %.2f tok/s (emulated, 1 thread)\n", llm_kernel_name(in.kernel), 64e9 / (b - a), 16e9 / (c - b));
    }
    return 0;
}
