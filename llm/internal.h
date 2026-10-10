/*
 * internal.h - What the engine's files share: quantized block formats, the
 * model, its tokenizer, and sessions.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "llm.h"
#include "mk/lib.h"

typedef uint16_t f16;                        /* IEEE half precision, as stored */
#define BMAX 32                               /* tokens read together (prompt) */

/* Tensor types, numbered as in GGUF files. */
enum { T_F32 = 0, T_F16 = 1, T_Q4_0 = 2, T_Q4_1 = 3, T_Q5_0 = 6, T_Q5_1 = 7, T_Q8_0 = 8,
       T_Q4_K = 12, T_Q5_K = 13, T_Q6_K = 14, T_BF16 = 30, T_NTYPES = 31 };

/* ---- Quantized weights. Values come in blocks that share a scale:
 * 32 values per block for the simple formats, 256 for the "K" ones, which
 * add one 6-bit (or 8-bit) scale per sub-block of 32 (or 16) values. */
typedef struct { f16 d; uint8_t qs[16]; } bq4_0;                      /* x = d*(q-8)       */
typedef struct { f16 d, m; uint8_t qs[16]; } bq4_1;                   /* x = d*q + m       */
typedef struct { f16 d; uint8_t qh[4], qs[16]; } bq5_0;               /* x = d*(q-16), q 5 bits */
typedef struct { f16 d, m; uint8_t qh[4], qs[16]; } bq5_1;            /* x = d*q + m       */
typedef struct { f16 d; int8_t qs[32]; } bq8_0;                       /* x = d*q           */
typedef struct { f16 d, dmin; uint8_t sc[12], qs[128]; } bq4_K;       /* x = d*s*q - dmin*m */
typedef struct { f16 d, dmin; uint8_t sc[12], qh[32], qs[128]; } bq5_K;
typedef struct { uint8_t ql[128], qh[64]; int8_t sc[16]; f16 d; } bq6_K; /* x = d*s*(q-32) */

/* Activations (the vector a matrix multiplies) are quantized to 8 bits too,
 * in the block size of the weights, so dot products run on integers. */
typedef struct { float d, s; int8_t qs[32]; } aq8;                    /* s = d * sum(qs)   */
typedef struct { float d; int8_t qs[256]; int16_t bsums[16]; } aq8K;  /* sums of 16 values */

/* Per type: values per block, bytes per block, activation format. */
typedef struct { int bs, bytes, act; } tinfo_t;
enum { A_F32, A_Q8, A_Q8K };                    /* activation formats */
extern const tinfo_t tinfo[T_NTYPES];

/* A matrix: rows of `cols` values, each row a run of blocks. */
typedef struct mat {
    int type, rows, cols;
    size_t row_bytes;
    const uint8_t *data;
    float (*dot)(int n, const void *row, const void *x);   /* chosen kernel */
    /* several vectors at once: y[b*ys] = dot(row, x + b*xs), b < B; or 0 */
    void (*dotn)(int n, const void *row, const void *x, size_t xs, int B, float *y, size_t ys);
} mat_t;

/* quant.c */
float f16_to_f32(f16 h);
f16   f32_to_f16(float f);
void  dequant_row(int type, const void *row, float *y, int n);
void  quant_act(int act, const float *x, void *out, int n);
size_t act_bytes(int act, int n);
int   pick_kernels(int want);                 /* returns the LLM_K_ in use */
extern int cpu_dotprod;                      /* (AArch64) from the env: may LLM_K_DOT be used? */
/* AArch64 NEON kernels (neon.c; neon_dot.c: the same with sdot, ARMv8.2) */
typedef struct {
    float (*q8_0)(int, const void *, const void *), (*q4_0)(int, const void *, const void *);
    float (*q4_K)(int, const void *, const void *), (*q6_K)(int, const void *, const void *);
    float (*f16)(int, const void *, const void *), (*f32)(int, const void *, const void *);
    void (*q4_K_n)(int, const void *, const void *, size_t, int, float *, size_t);
    void (*q6_K_n)(int, const void *, const void *, size_t, int, float *, size_t);
    float (*dot_f16)(int, const float *, const f16 *);
    void (*axpy_f16)(int, float, const f16 *, float *);
    void (*to_f16)(int, const float *, f16 *);
} neon_kernels_t;
extern const neon_kernels_t neon_plain, neon_sdot;
void  set_dot(mat_t *m, int kernel);
float dot_f32(int n, const float *a, const float *b);
float dot_f32_f16(int n, const float *a, const f16 *b);
void  axpy_f16(int n, float a, const f16 *x, float *y);      /* y += a*x */
void  to_f16(int n, const float *x, f16 *y);

/* math.c: what a C library's libm would give, for the few functions used */
float m_expf(float x);
double m_exp(double x);
double m_log(double x);
void  m_sincos(double a, double *s, double *c);
float m_sqrtf(float x);

/* ---- The model. */
typedef struct {
    mat_t wq, wk, wv, wo, wg, wu, wd;
    const float *attn_norm, *ffn_norm, *bq, *bk, *bv;
} layer_t;

enum { TOK_GPT2 = 1, TOK_SPM };               /* byte-level BPE, SentencePiece */
enum { CHAT_CHATML = 1, CHAT_LLAMA3, CHAT_INST };

typedef struct { uint32_t key; int32_t a, b, rank, out; } merge_t;   /* a+b -> out */
typedef struct { const char *s; int len; } str_t;


struct llm {
    llm_env_t env;
    llm_info_t info;
    int n_layer, n_embd, n_head, n_kv, hd, n_ff, n_vocab, ctx;
    float eps, theta;
    int rope_neox;                            /* rotate (i, i+hd/2) pairs, not (2i, 2i+1) */
    const float *rope_factors;                /* per-frequency divisors, or 0 */
    double *inv_freq;                         /* rotation speed of each pair */
    uint8_t *weights;                         /* all tensor data */
    uint64_t wbytes;
    mat_t embd, out;
    const float *out_norm;
    layer_t *L;
    int kernel, exact;

    /* tokenizer */
    int tok_kind, chat;
    str_t *vocab;                             /* each token's text (as stored in the file) */
    float *scores;
    int32_t *ttype;                           /* 1 normal, 3 control, 4 user-defined, 6 byte */
    int32_t *vhash; uint32_t vhmask;          /* text -> token id */
    str_t *merges; int nmerges;               /* "a b" pairs, as in the file */
    merge_t *mtab; uint32_t mmask;            /* (a, b) -> merge */
    int32_t *special; int nspecial;           /* control tokens, longest first */
    int bos, eos, eot, add_bos, nl;
    int32_t byte_tok[256];                    /* SentencePiece byte fallback */

    /* scratch for one read of up to BMAX tokens */
    float *x, *xb, *q, *k, *v, *att, *hb, *hb2, *logits, *tmp;
    void *xq;                                 /* quantized activations */
    size_t xq_stride;
    uint64_t sessions_mem;
    void *arena;                              /* metadata (strings, tables) */
};

struct llm_session {
    llm_t *m;
    f16 *kc, *vc;                             /* [layer][pos][n_kv*hd] */
    int n_past;
    int32_t *hist; int nhist;                 /* tokens in the cache */
    int32_t pend[4]; int npend;               /* tokens to read before the next turn */
    char ubuf[8]; int ulen;                   /* bytes of an unfinished UTF-8 character */
    int state, ngen;                          /* 0 idle, 1 answering */
    uint64_t rng, t0;
    llm_stats_t st;
};

/* model.c */
const float *forward(llm_session_t *s, const int32_t *tok, int n);
/* tok.c */
int  tok_init(llm_t *m);
/* gguf.c */
int  gguf_load(llm_t *m, uint64_t size, char *err, size_t errlen);
/* memory helpers */
void *zalloc(llm_t *m, size_t n);
void  zfree(llm_t *m, void *p);

/* small string helpers the C library would have */
static inline int str_prefix(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static inline const char *strstr_simple(const char *h, const char *n)
{
    for (; *h; h++) if (str_prefix(h, n)) return h;
    return 0;
}
static inline long strnum_prefix(const char *s)       /* leading decimal digits, or -1 */
{
    long v = -1;
    for (; *s >= '0' && *s <= '9' && v < 100000000; s++) v = (v < 0 ? 0 : v * 10) + (*s - '0');
    return v;
}
