/*
 * model.c - Opening a model, and the transformer's forward pass: from tokens
 * to the scores (logits) of the next token.
 *
 *   x = embedding(token)
 *   for each layer:                                   (24 to 32 of them)
 *       h = rmsnorm(x);  q, k, v = Wq h, Wk h, Wv h;  rotate q, k by position
 *       store k, v in the session's cache; attention of q over all cached k, v
 *       x += Wo (attention output)
 *       h = rmsnorm(x);  x += Wdown (silu(Wgate h) * (Wup h))
 *   logits = Wout rmsnorm(x)
 *
 * Almost all the time goes into the matrix-vector products (mm): each
 * thread takes a share of the rows. Up to BMAX tokens are read together,
 * so each weight row, once loaded, serves all of them.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "internal.h"

void *zalloc(llm_t *m, size_t n)
{
    void *p = m->env.alloc(m->env.ctx, n ? n : 1);
    if (p) memset(p, 0, n);
    return p;
}
void zfree(llm_t *m, void *p) { if (p) m->env.free(m->env.ctx, p); }

static void par(llm_t *m, void (*fn)(void *, int, int), void *arg)
{
    if (m->env.parallel && m->env.threads > 1) m->env.parallel(m->env.ctx, fn, arg, m->env.threads);
    else fn(arg, 0, 1);
}

/* ---- Matrix x activations, for up to 3 matrices sharing the same input. */
typedef struct {
    llm_t *m;
    const mat_t *w[3];
    float *y[3];
    int nw, B, chunk, nchunk[3], total;  /* chunks of rows, per matrix and in all */
    int next;                            /* next chunk to hand out */
    const void *xa[3]; size_t xs[3];     /* activation (by format) and its stride per token */
} mm_t;

/* Threads take rows in small chunks as they go, so fast and slow cores
 * (performance and efficiency cores) all finish at about the same time. */
static void mm_part(void *arg, int t, int nt)
{
    mm_t *j = arg;
    llm_t *m = j->m;
    (void)nt;
    float *tmp = m->tmp + (size_t)t * (m->n_ff > m->n_embd ? m->n_ff : m->n_embd);
    for (;;) {
        int c = __atomic_fetch_add(&j->next, 1, __ATOMIC_RELAXED);
        if (c >= j->total) break;
        int k = 0;
        while (c >= j->nchunk[k]) c -= j->nchunk[k++];   /* which matrix, which chunk of it */
        const mat_t *w = j->w[k];
        int r0 = c * j->chunk, r1 = r0 + j->chunk < w->rows ? r0 + j->chunk : w->rows;
        int act = m->exact ? A_F32 : tinfo[w->type].act;
        const uint8_t *X = j->xa[act];
        for (int r = r0; r < r1; r++) {
            const uint8_t *row = w->data + (size_t)r * w->row_bytes;
            if (m->exact) dequant_row(w->type, row, tmp, w->cols);
            else if (j->B > 1 && w->dotn) { w->dotn(w->cols, row, X, j->xs[act], j->B, &j->y[k][r], w->rows); continue; }
            for (int b = 0; b < j->B; b++) {
                const void *x = X + b * j->xs[act];
                j->y[k][(size_t)b * w->rows + r] = m->exact ? dot_f32(w->cols, tmp, x) : w->dot(w->cols, row, x);
            }
        }
    }
}

/* y[k] = w[k] * x for the B vectors x (each n = w->cols long, stride ldx). */
static void mm(llm_t *m, int nw, const mat_t **w, float **y, const float *x, int ldx, int B)
{
    mm_t j = { .m = m, .nw = nw, .B = B };
    int n = w[0]->cols, need[3] = { 1, 0, 0 };
    /* chunks of about 16 KiB of weights per token read, at least 4 rows */
    j.chunk = (int)(16384 / (w[0]->row_bytes * B + 1));
    if (j.chunk < 4) j.chunk = 4;
    if (j.chunk > 256) j.chunk = 256;
    for (int k = 0; k < nw; k++) {
        j.w[k] = w[k]; j.y[k] = y[k]; need[tinfo[w[k]->type].act] = 1;
        j.nchunk[k] = (w[k]->rows + j.chunk - 1) / j.chunk;
        j.total += j.nchunk[k];
    }
    if (m->exact) need[A_Q8] = need[A_Q8K] = 0;
    j.xa[A_F32] = x; j.xs[A_F32] = (size_t)ldx * 4;
    uint8_t *q = m->xq;
    for (int a = A_Q8; a <= A_Q8K; a++) {
        if (!need[a]) continue;
        j.xs[a] = act_bytes(a, n); j.xa[a] = q;
        for (int b = 0; b < B; b++) quant_act(a, x + (size_t)b * ldx, q + b * j.xs[a], n);
        q += B * j.xs[a];
    }
    par(m, mm_part, &j);
}

static void rmsnorm(float *y, const float *x, const float *w, int n, float eps)
{
    float ss = 0;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    float r = 1 / m_sqrtf(ss / n + eps);
    for (int i = 0; i < n; i++) y[i] = x[i] * r * w[i];
}

/* Rotary embedding: rotate pairs of q/k values by an angle that grows with
 * the position, a different speed per pair. */
static void rope(llm_t *m, float *v, int nheads, int pos)
{
    int hd = m->hd, half = hd / 2;
    for (int i = 0; i < half; i++) {
        double s, c;
        m_sincos(pos * m->inv_freq[i], &s, &c);
        for (int h = 0; h < nheads; h++) {
            float *p = v + h * hd;
            int a = m->rope_neox ? i : 2 * i, b = m->rope_neox ? i + half : 2 * i + 1;
            float x0 = p[a], x1 = p[b];
            p[a] = x0 * c - x1 * s;
            p[b] = x0 * s + x1 * c;
        }
    }
}

/* ---- Attention, one head per thread share. */
typedef struct { llm_session_t *s; int l, B, pos0; } att_t;

static void att_part(void *arg, int t, int nt)
{
    att_t *a = arg;
    llm_session_t *s = a->s;
    llm_t *m = s->m;
    int hd = m->hd, KV = m->n_kv * hd, grp = m->n_head / m->n_kv;
    float *sc = m->att + (size_t)t * m->ctx, scale = 1 / m_sqrtf(hd);
    const f16 *K = s->kc + (size_t)a->l * m->ctx * KV, *V = s->vc + (size_t)a->l * m->ctx * KV;
    for (int h = m->n_head * t / nt; h < m->n_head * (t + 1) / nt; h++) {
        int kh = h / grp;
        for (int b = 0; b < a->B; b++) {
            int pos = a->pos0 + b;
            const float *q = m->q + (size_t)b * m->n_embd + h * hd;
            float mx = -1e30f, sum = 0;
            for (int p = 0; p <= pos; p++) {
                float v = dot_f32_f16(hd, q, K + (size_t)p * KV + kh * hd) * scale;
                sc[p] = v;
                if (v > mx) mx = v;
            }
            for (int p = 0; p <= pos; p++) { sc[p] = m_expf(sc[p] - mx); sum += sc[p]; }
            float *o = m->xb + (size_t)b * m->n_embd + h * hd;
            memset(o, 0, hd * 4);
            for (int p = 0; p <= pos; p++) axpy_f16(hd, sc[p] / sum, V + (size_t)p * KV + kh * hd, o);
        }
    }
}

/* ---- SwiGLU: hb = silu(gate) * up. */
typedef struct { llm_t *m; int B; } ffn_t;
static void silu_part(void *arg, int t, int nt)
{
    ffn_t *f = arg;
    size_t n = (size_t)f->B * f->m->n_ff, i0 = n * t / nt, i1 = n * (t + 1) / nt;
    float *g = f->m->hb, *u = f->m->hb2;
    for (size_t i = i0; i < i1; i++) g[i] = g[i] / (1 + m_expf(-g[i])) * u[i];
}

/* Read B (<= BMAX) tokens at positions n_past.. ; logits of the last one. */
static void step(llm_session_t *s, const int32_t *tok, int B, int want_logits)
{
    llm_t *m = s->m;
    int E = m->n_embd, KV = m->n_kv * m->hd, F = m->n_ff, pos0 = s->n_past;
    for (int b = 0; b < B; b++) dequant_row(m->embd.type, m->embd.data + (size_t)tok[b] * m->embd.row_bytes, m->x + (size_t)b * E, E);
    for (int l = 0; l < m->n_layer; l++) {
        layer_t *L = &m->L[l];
        for (int b = 0; b < B; b++) rmsnorm(m->xb + (size_t)b * E, m->x + (size_t)b * E, L->attn_norm, E, m->eps);
        const mat_t *w3[3] = { &L->wq, &L->wk, &L->wv };
        float *y3[3] = { m->q, m->k, m->v };
        mm(m, 3, w3, y3, m->xb, E, B);
        for (int b = 0; b < B; b++) {
            float *q = m->q + (size_t)b * E, *k = m->k + (size_t)b * KV, *v = m->v + (size_t)b * KV;
            if (L->bq) for (int i = 0; i < E; i++) q[i] += L->bq[i];
            if (L->bk) for (int i = 0; i < KV; i++) k[i] += L->bk[i];
            if (L->bv) for (int i = 0; i < KV; i++) v[i] += L->bv[i];
            rope(m, q, m->n_head, pos0 + b);
            rope(m, k, m->n_kv, pos0 + b);
            f16 *kc = s->kc + ((size_t)l * m->ctx + pos0 + b) * KV, *vc = s->vc + ((size_t)l * m->ctx + pos0 + b) * KV;
            to_f16(KV, k, kc); to_f16(KV, v, vc);
        }
        att_t a = { s, l, B, pos0 };
        par(m, att_part, &a);
        const mat_t *wo = &L->wo;
        float *o = m->hb;                                     /* Wo output, then added to x */
        mm(m, 1, &wo, &o, m->xb, E, B);
        for (size_t i = 0; i < (size_t)B * E; i++) m->x[i] += o[i];

        for (int b = 0; b < B; b++) rmsnorm(m->xb + (size_t)b * E, m->x + (size_t)b * E, L->ffn_norm, E, m->eps);
        const mat_t *w2[2] = { &L->wg, &L->wu };
        float *y2[2] = { m->hb, m->hb2 };
        mm(m, 2, w2, y2, m->xb, E, B);
        ffn_t f = { m, B };
        par(m, silu_part, &f);
        const mat_t *wd = &L->wd;
        float *d = m->xb;
        mm(m, 1, &wd, &d, m->hb, F, B);
        for (size_t i = 0; i < (size_t)B * E; i++) m->x[i] += m->xb[i];
    }
    s->n_past += B;
    if (!want_logits) return;
    rmsnorm(m->xb, m->x + (size_t)(B - 1) * E, m->out_norm, E, m->eps);
    const mat_t *wo = &m->out;
    mm(m, 1, &wo, &m->logits, m->xb, E, 1);
}

const float *forward(llm_session_t *s, const int32_t *tok, int n)
{
    for (int i = 0; i < n; i += BMAX) {
        int b = n - i < BMAX ? n - i : BMAX;
        step(s, tok + i, b, i + b == n);
    }
    return s->m->logits;
}

/* ---- Opening. */
int llm_open(llm_t **out, const llm_env_t *env, uint64_t size, char *err, size_t errlen)
{
    *out = 0;
    if (!env || !env->alloc || !env->free || !env->read) return LLM_EINVAL;
    llm_t *m = env->alloc(env->ctx, sizeof *m);
    if (!m) return LLM_ENOMEM;
    memset(m, 0, sizeof *m);
    m->env = *env;
    if (!m->env.budget) m->env.budget = 4ULL << 30;
    if (m->env.threads < 1) m->env.threads = 1;
    int r = gguf_load(m, size, err, errlen);
    if (!r) r = tok_init(m);
    if (r) { if (err && errlen && !err[0]) strlcpy(err, "cannot read the tokenizer", errlen); llm_close(m); return r; }

    int ctx = m->env.ctx_len ? m->env.ctx_len : m->info.ctx_train > 0 && m->info.ctx_train < 4096 ? m->info.ctx_train : 4096;
    m->ctx = ctx;
    if (!(m->inv_freq = zalloc(m, m->hd / 2 * sizeof(double)))) { llm_close(m); return LLM_ENOMEM; }
    for (int i = 0; i < m->hd / 2; i++) {                 /* theta^(-2i/hd), divided by any rope factor */
        m->inv_freq[i] = m_exp(-2.0 * i / m->hd * m_log(m->theta));
        if (m->rope_factors) m->inv_freq[i] /= m->rope_factors[i];
    }
    cpu_dotprod = m->env.cpu_dotprod;
    m->kernel = pick_kernels(m->env.kernel);
    set_dot(&m->embd, m->kernel); set_dot(&m->out, m->kernel);
    for (int l = 0; l < m->n_layer; l++) {
        layer_t *L = &m->L[l];
        mat_t *all[7] = { &L->wq, &L->wk, &L->wv, &L->wo, &L->wg, &L->wu, &L->wd };
        for (int i = 0; i < 7; i++) set_dot(all[i], m->kernel);
    }

    /* scratch for BMAX tokens */
    size_t E = m->n_embd, KV = (size_t)m->n_kv * m->hd, F = m->n_ff, T = m->env.threads, mx = F > E ? F : E;
    m->xq_stride = act_bytes(A_Q8, mx) + act_bytes(A_Q8K, mx) + 64;
    size_t need[] = { BMAX * E, BMAX * E, BMAX * E, BMAX * KV, BMAX * KV, T * (size_t)ctx, BMAX * F, BMAX * F, (size_t)m->n_vocab, T * mx };
    float **ptr[] = { &m->x, &m->xb, &m->q, &m->k, &m->v, &m->att, &m->hb, &m->hb2, &m->logits, &m->tmp };
    uint64_t scratch = 0;
    for (int i = 0; i < 10; i++) {
        if (!(*ptr[i] = zalloc(m, need[i] * 4))) { llm_close(m); return LLM_ENOMEM; }
        scratch += need[i] * 4;
    }
    if (!(m->xq = zalloc(m, BMAX * m->xq_stride * 2))) { llm_close(m); return LLM_ENOMEM; }
    scratch += BMAX * m->xq_stride * 2;

    llm_info_t *in = &m->info;
    in->n_layer = m->n_layer; in->n_embd = m->n_embd; in->n_head = m->n_head; in->n_kv_head = m->n_kv;
    in->n_ff = m->n_ff; in->n_vocab = m->n_vocab; in->ctx = ctx; in->kernel = m->kernel; in->file_size = size;
    in->mem_weights = m->wbytes; in->mem_scratch = scratch;
    in->mem_kv_per_session = 2ULL * m->n_layer * ctx * KV * sizeof(f16);
    in->mem_total = in->mem_weights + in->mem_scratch;
    if (in->mem_total + in->mem_kv_per_session > m->env.budget) {
        if (err && errlen) strlcpy(err, "model and one session do not fit in the memory budget", errlen);
        llm_close(m); return LLM_EBUDGET;
    }
    *out = m;
    return 0;
}

void llm_close(llm_t *m)
{
    if (!m) return;
    float *bufs[] = { m->x, m->xb, m->q, m->k, m->v, m->att, m->hb, m->hb2, m->logits, m->tmp };
    for (int i = 0; i < 10; i++) zfree(m, bufs[i]);
    zfree(m, m->xq); zfree(m, m->L); zfree(m, m->inv_freq); zfree(m, m->weights);
    zfree(m, m->vhash); zfree(m, m->mtab); zfree(m, m->special);
    for (void *a = m->arena; a; ) { void *n = *(void **)a; m->env.free(m->env.ctx, a); a = n; }
    m->env.free(m->env.ctx, m);
}

void llm_info(llm_t *m, llm_info_t *out) { *out = m->info; out->mem_total = m->info.mem_weights + m->info.mem_scratch + m->sessions_mem; }
void llm_set_exact(llm_t *m, int exact) { m->exact = exact; }
