/*
 * fuzz.c - Damaged model files must be refused, never crash the engine.
 * The model is read into memory once; each round changes a few bytes of its
 * header and metadata (or cuts the file short), opens it, and if it still
 * opens, tokenizes and runs a few tokens. Built with the address and
 * undefined-behaviour checkers by `make llm-test`.
 *   fuzz model.gguf [rounds]
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../llm/llm.h"

static unsigned char *img;
static uint64_t len, cut;
static void *a(void *c, size_t n) { (void)c; return aligned_alloc(64, (n + 63) & ~(size_t)63); }
static void f(void *c, void *p) { (void)c; free(p); }
static int rd(void *c, void *buf, uint64_t off, size_t n) { (void)c; if (off > cut || n > cut - off) return -1; memcpy(buf, img + off, n); return 0; }

int main(int argc, char **argv)
{
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) return 2;
    fseek(fp, 0, SEEK_END); len = ftell(fp); fseek(fp, 0, SEEK_SET);
    img = malloc(len);
    if (fread(img, 1, len, fp) != len) return 2;
    fclose(fp);
    int rounds = argc > 2 ? atoi(argv[2]) : 300, opened = 0;
    llm_env_t env = { .alloc = a, .free = f, .read = rd, .threads = 1, .budget = 1ULL << 30, .ctx_len = 256 };
    uint64_t meta = len < (4u << 20) ? len : (4u << 20);       /* the header and metadata live here */
    srand(1234);
    for (int r = 0; r < rounds; r++) {
        unsigned char saved[16]; uint64_t at[16]; int nmut = 1 + rand() % 8;
        cut = len;
        if (r % 10 == 9) cut = (uint64_t)rand() * rand() % len;  /* a truncated file */
        for (int k = 0; k < nmut; k++) {
            at[k] = r % 3 == 0 ? (uint64_t)rand() % 64 : (uint64_t)rand() * rand() % meta;
            saved[k] = img[at[k]];
            img[at[k]] = r % 2 ? (unsigned char)rand() : img[at[k]] ^ (1 << rand() % 8);
        }
        llm_t *m; char err[128];
        if (!llm_open(&m, &env, cut, err, sizeof err)) {
            opened++;
            int e; llm_session_t *s = llm_session_new(m, &e);
            int32_t t[64];
            int n = llm_tokenize(m, "fuzzing <|im_start|> text 123", 29, t, 64, 1);
            if (s && n > 0) llm_eval(s, t, n);
            llm_session_free(s);
            llm_close(m);
        }
        for (int k = nmut - 1; k >= 0; k--) img[at[k]] = saved[k];
    }
    printf("fuzz: %d damaged files, %d still opened (harmless changes), no crash\n", rounds, opened);
    return 0;
}
