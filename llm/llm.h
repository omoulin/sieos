/*
 * llm.h - SIEOS's language-model engine: the interface.
 *
 * The engine runs "llama-family" transformer models (Llama, SmolLM2,
 * Mistral, Qwen2) stored in GGUF files, on the CPU. It is portable C: it
 * uses no C library, only what its caller hands it in an llm_env_t (memory,
 * file reading, threads, a clock). The same code builds into the SIEOS
 * assistant server and into host tools (tools/llm).
 *
 * Use:
 *   llm_t *m;   llm_open(&m, &env, file_size, err, sizeof err);
 *   llm_session_t *s = llm_session_new(m);
 *   llm_ask(s, "You are helpful.", "Hello!");          // reads the prompt
 *   while ((n = llm_next(s, &samp, piece, sizeof piece)) >= 0) show(piece, n);
 *   llm_ask(s, NULL, "And then?");                       // next turn, same cache
 *
 * Errors are negative numbers (LLM_E...); err[] gets a sentence.
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct llm llm_t;
typedef struct llm_session llm_session_t;

/* What the caller provides. All callbacks get ctx as their first argument. */
typedef struct {
    void *ctx;
    void *(*alloc)(void *ctx, size_t n);                 /* 64-byte aligned, or 0 */
    void  (*free)(void *ctx, void *p);
    /* read n bytes of the model file at offset off into buf: 0 or -1 */
    int   (*read)(void *ctx, void *buf, uint64_t off, size_t n);
    /* run fn(arg, i, n) for i = 0..n-1, each on its own thread, and wait for
     * all; 0 means "no threads": the engine then runs everything itself */
    void  (*parallel)(void *ctx, void (*fn)(void *arg, int i, int n), void *arg, int n);
    int   threads;                                       /* n for parallel(); 1 if none */
    uint64_t (*now_ns)(void *ctx);                       /* a monotonic clock, or 0 */
    uint64_t budget;                                     /* bytes; 0 = 4 GiB */
    int   ctx_len;                                       /* tokens per session; 0 = min(model, 4096) */
    int   kernel;                                        /* force LLM_K_...; 0 = best the CPU has */
    int   cpu_dotprod;                                   /* AArch64: the CPU has the dot product instructions
                                                            (ID_AA64ISAR0_EL1.DP: the Pi 5 yes, the Pi 4 no), as
                                                            the system says; 0 = no or unknown (plain NEON) */
} llm_env_t;

enum { LLM_K_AUTO, LLM_K_SCALAR, LLM_K_AVX2, LLM_K_VNNI, LLM_K_NEON, LLM_K_DOT };  /* matrix kernels (DOT: NEON + sdot) */
const char *llm_kernel_name(int k);   /* "scalar", "avx2", "avx2+vnni", "neon", "neon+dot" */
enum { LLM_EINVAL = -1, LLM_ENOMEM = -2, LLM_EIO = -3, LLM_EFORMAT = -4,
       LLM_EUNSUP = -5, LLM_EBUDGET = -6, LLM_EFULL = -7 };

typedef struct {
    char name[64], arch[16];
    int n_layer, n_embd, n_head, n_kv_head, n_ff, n_vocab, ctx_train, ctx;
    int kernel;                       /* LLM_K_... in use */
    uint64_t mem_weights, mem_kv_per_session, mem_scratch, mem_total;
    uint64_t file_size;
} llm_info_t;

typedef struct {
    float temperature;                /* 0 = greedy (always the most likely token) */
    int   top_k;                      /* 0 = off */
    float top_p, min_p;               /* 1, 0 = off */
    float repeat_penalty;             /* 1 = off */
    int   repeat_last;                /* how many recent tokens it looks at */
    uint64_t seed;
    int   max_tokens;                 /* per answer; 0 = until the end or the context is full */
} llm_sampler_t;

typedef struct {                      /* timings of the last answer */
    int prompt_tokens, gen_tokens;
    uint64_t prompt_ns, gen_ns;
} llm_stats_t;

int  llm_open(llm_t **out, const llm_env_t *env, uint64_t file_size, char *err, size_t errlen);
void llm_close(llm_t *m);
void llm_info(llm_t *m, llm_info_t *out);
void llm_default_sampler(llm_sampler_t *s);

/* Tokens. tokenize returns the count (or -needed if max is too small);
 * special = 1 turns "<|im_start|>" etc. in the text into their tokens. */
int  llm_tokenize(llm_t *m, const char *text, size_t len, int32_t *out, int max, int special);
int  llm_token_bytes(llm_t *m, int32_t tok, char *buf, int max);   /* raw bytes of a token */

/* Sessions: a conversation with its own attention cache. */
llm_session_t *llm_session_new(llm_t *m, int *err);
void llm_session_free(llm_session_t *s);
void llm_session_reset(llm_session_t *s);
/* Add a turn: an optional system text (first turn only), the user's text;
 * the model reads it. Then llm_next gives the answer piece by piece:
 * returns the bytes written (0: nothing printable yet), -1 at the end. */
int  llm_ask(llm_session_t *s, const char *system, const char *user);
int  llm_next(llm_session_t *s, const llm_sampler_t *p, char *out, int max);
void llm_stop(llm_session_t *s);                /* end the answer now */
/* A past exchange (user's turn + the answer given) read into the cache
 * without generating: to resume a saved conversation. LLM_EFULL if it
 * does not fit. */
int  llm_feed(llm_session_t *s, const char *system, const char *user, const char *answer);
void llm_stats(llm_session_t *s, llm_stats_t *out);
int  llm_session_tokens(llm_session_t *s);      /* tokens in its cache */

/* Low level (tests, tools): read tokens at the end of the cache and get the
 * logits of the last one (n_vocab floats, valid until the next call). */
const float *llm_eval(llm_session_t *s, const int32_t *tok, int n);
/* exact = 1: every matrix product in plain float (slow; to check the fast kernels) */
void llm_set_exact(llm_t *m, int exact);
