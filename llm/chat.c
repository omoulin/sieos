/*
 * chat.c - Conversations: chat templates, sessions with their attention
 * cache, and choosing the next token (sampling).
 *
 * A turn is wrapped in the model's chat format (ChatML, Llama 3 or [INST]);
 * the control tokens come from fixed strings, the user's text is read as
 * plain text, so it can never smuggle control tokens in. The cache keeps
 * every token read: the next turn only reads its own new tokens.
 *
 * llm_next() reads exactly one token per call: the one chosen by the
 * previous call (or the last token of the prompt). So the scores it samples
 * from are always this session's own, even when several sessions take turns.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "internal.h"

void llm_default_sampler(llm_sampler_t *s)
{
    *s = (llm_sampler_t){ .temperature = 0.7f, .top_k = 40, .top_p = 0.9f, .min_p = 0.05f,
                          .repeat_penalty = 1.1f, .repeat_last = 64, .seed = 0x5EED, .max_tokens = 0 };
}

llm_session_t *llm_session_new(llm_t *m, int *err)
{
    uint64_t kv = m->info.mem_kv_per_session;
    if (m->info.mem_weights + m->info.mem_scratch + m->sessions_mem + kv > m->env.budget) { if (err) *err = LLM_EBUDGET; return 0; }
    llm_session_t *s = zalloc(m, sizeof *s);
    if (!s) { if (err) *err = LLM_ENOMEM; return 0; }
    s->m = m;
    s->kc = m->env.alloc(m->env.ctx, kv / 2);
    s->vc = m->env.alloc(m->env.ctx, kv / 2);
    s->hist = zalloc(m, (size_t)m->ctx * 4);
    if (!s->kc || !s->vc || !s->hist) { llm_session_free(s); if (err) *err = LLM_ENOMEM; return 0; }
    m->sessions_mem += kv;
    s->rng = 0x9E3779B97F4A7C15ULL;
    return s;
}

void llm_session_free(llm_session_t *s)
{
    if (!s) return;
    llm_t *m = s->m;
    if (s->kc && s->vc && s->hist) m->sessions_mem -= m->info.mem_kv_per_session;
    zfree(m, s->kc); zfree(m, s->vc); zfree(m, s->hist);
    zfree(m, s);
}

void llm_session_reset(llm_session_t *s) { s->n_past = s->nhist = s->npend = s->ulen = s->state = s->ngen = 0; }
int  llm_session_tokens(llm_session_t *s) { return s->n_past; }
void llm_stats(llm_session_t *s, llm_stats_t *out) { *out = s->st; }
static uint64_t now(llm_t *m) { return m->env.now_ns ? m->env.now_ns(m->env.ctx) : 0; }

const float *llm_eval(llm_session_t *s, const int32_t *tok, int n)
{
    if (n <= 0 || s->n_past + n > s->m->ctx) return 0;
    for (int i = 0; i < n; i++) if (tok[i] < 0 || tok[i] >= s->m->n_vocab) return 0;
    memcpy(s->hist + s->nhist, tok, n * 4);
    s->nhist += n;
    return forward(s, tok, n);
}

/* ---- Building a turn's tokens. */
typedef struct { int32_t *t; int n, max; } tl_t;
static void add_special(llm_t *m, tl_t *l, const char *text)       /* fixed template text */
{
    int k = llm_tokenize(m, text, strlen(text), l->t + l->n, l->max - l->n, 1);
    l->n += k > 0 ? k : 0;
}
static void add_plain(llm_t *m, tl_t *l, const char *text)         /* the user's own text */
{
    int k = llm_tokenize(m, text, strlen(text), l->t + l->n, l->max - l->n, 0);
    l->n += k > 0 ? k : 0;
}

static void build_turn(llm_session_t *s, tl_t *l, const char *sys, const char *user)
{
    llm_t *m = s->m;
    int first = s->n_past == 0;
    for (int i = 0; i < s->npend; i++) if (l->n < l->max) l->t[l->n++] = s->pend[i];
    if (first && m->add_bos && m->bos >= 0 && l->n < l->max) l->t[l->n++] = m->bos;
    switch (m->chat) {
    case CHAT_LLAMA3:
        if (first && sys) { add_special(m, l, "<|start_header_id|>system<|end_header_id|>\n\n"); add_plain(m, l, sys); add_special(m, l, "<|eot_id|>"); }
        add_special(m, l, "<|start_header_id|>user<|end_header_id|>\n\n"); add_plain(m, l, user);
        add_special(m, l, "<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n");
        break;
    case CHAT_INST:
        add_special(m, l, "[INST] ");
        if (first && sys) { add_plain(m, l, sys); add_plain(m, l, "\n\n"); }
        add_plain(m, l, user);
        add_special(m, l, " [/INST]");
        break;
    default:                                                       /* ChatML */
        if (first && sys) { add_special(m, l, "<|im_start|>system\n"); add_plain(m, l, sys); add_special(m, l, "<|im_end|>\n"); }
        add_special(m, l, "<|im_start|>user\n"); add_plain(m, l, user);
        add_special(m, l, "<|im_end|>\n<|im_start|>assistant\n");
    }
}

int llm_ask(llm_session_t *s, const char *sys, const char *user)
{
    llm_t *m = s->m;
    if (!user) return LLM_EINVAL;
    tl_t l = { 0, 0, m->ctx };
    l.t = zalloc(m, (size_t)m->ctx * 4);
    if (!l.t) return LLM_ENOMEM;
    build_turn(s, &l, sys, user);
    if (s->n_past + l.n + 8 > m->ctx) {                            /* full: start again, this turn only */
        llm_session_reset(s);
        l.n = 0;
        build_turn(s, &l, sys, user);
        if (l.n + 8 > m->ctx) { zfree(m, l.t); return LLM_EFULL; }
    }
    uint64_t t0 = now(m);
    if (l.n > 1 && !llm_eval(s, l.t, l.n - 1)) { zfree(m, l.t); return LLM_EINVAL; }
    s->pend[0] = l.t[l.n - 1]; s->npend = 1;                       /* read by the first llm_next */
    s->st.prompt_tokens = l.n; s->st.prompt_ns = now(m) - t0;
    s->st.gen_tokens = 0; s->st.gen_ns = 0;
    s->state = 1; s->ngen = 0; s->ulen = 0;
    zfree(m, l.t);
    return 0;
}

/* Read a past exchange into the cache without answering (a conversation
 * resumed after a restart): the user's turn, then the assistant's answer as
 * it was, closed as the model would have closed it. */
int llm_feed(llm_session_t *s, const char *sys, const char *user, const char *answer)
{
    llm_t *m = s->m;
    if (!user || !answer) return LLM_EINVAL;
    tl_t l = { 0, 0, m->ctx };
    l.t = zalloc(m, (size_t)m->ctx * 4);
    if (!l.t) return LLM_ENOMEM;
    build_turn(s, &l, sys, user);
    add_plain(m, &l, answer);
    if (s->n_past + l.n + 8 > m->ctx) { zfree(m, l.t); return LLM_EFULL; }
    if (l.n && !llm_eval(s, l.t, l.n)) { zfree(m, l.t); return LLM_EINVAL; }
    zfree(m, l.t);
    s->npend = 0;
    s->state = 1;                    /* as if the answer had just been given: */
    llm_stop(s);                     /* the end-of-turn tokens wait for the next turn */
    return 0;
}

/* ---- Sampling. */
static uint64_t rnd(llm_session_t *s) { s->rng ^= s->rng >> 12; s->rng ^= s->rng << 25; s->rng ^= s->rng >> 27; return s->rng * 2685821657736338717ULL; }
static float frand(llm_session_t *s) { return (rnd(s) >> 40) / 16777216.0f; }

typedef struct { int id; float v; } cand_t;

static int sample(llm_session_t *s, const llm_sampler_t *p, float *lg)
{
    llm_t *m = s->m;
    int V = m->n_vocab;
    if (p->repeat_penalty > 0 && p->repeat_penalty != 1) {        /* each recent token, once */
        int from = s->nhist - p->repeat_last; if (from < 0) from = 0;
        for (int i = from; i < s->nhist; i++) {
            int t = s->hist[i], seen = 0;
            for (int j = from; j < i && !seen; j++) seen = s->hist[j] == t;
            if (!seen) lg[t] = lg[t] > 0 ? lg[t] / p->repeat_penalty : lg[t] * p->repeat_penalty;
        }
    }
    if (p->temperature <= 0) { int b = 0; for (int i = 1; i < V; i++) if (lg[i] > lg[b]) b = i; return b; }
    /* top k by a min-heap of the k best (k <= 256) */
    int k = p->top_k > 0 && p->top_k < 256 ? p->top_k : 256;
    cand_t h[256];
    int n = 0;
    for (int i = 0; i < V; i++) {
        float v = lg[i];
        if (n < k) {
            int c = n++;
            while (c && h[(c - 1) / 2].v > v) { h[c] = h[(c - 1) / 2]; c = (c - 1) / 2; }
            h[c] = (cand_t){ i, v };
        } else if (v > h[0].v) {
            int c = 0;
            for (;;) {
                int a = 2 * c + 1, b = a + 1, sm = c;
                float sv = v;
                if (a < n && h[a].v < sv) { sm = a; sv = h[a].v; }
                if (b < n && h[b].v < sv) sm = b;
                if (sm == c) break;
                h[c] = h[sm]; c = sm;
            }
            h[c] = (cand_t){ i, v };
        }
    }
    for (int i = 1; i < n; i++) { cand_t c = h[i]; int j = i; while (j && h[j - 1].v < c.v) { h[j] = h[j - 1]; j--; } h[j] = c; }
    float mx = h[0].v, sum = 0;
    for (int i = 0; i < n; i++) { h[i].v = m_expf((h[i].v - mx) / p->temperature); sum += h[i].v; }
    int keep = n;
    float cum = 0;
    for (int i = 0; i < n; i++) {
        h[i].v /= sum;
        if (p->min_p > 0 && h[i].v < p->min_p * h[0].v) { keep = i; break; }
        cum += h[i].v;
        if (p->top_p > 0 && p->top_p < 1 && cum >= p->top_p) { keep = i + 1; break; }
    }
    if (keep < 1) keep = 1;
    float tot = 0;
    for (int i = 0; i < keep; i++) tot += h[i].v;
    float r = frand(s) * tot;
    for (int i = 0; i < keep; i++) { r -= h[i].v; if (r <= 0) return h[i].id; }
    return h[keep - 1].id;
}

static int is_end(llm_t *m, int t)
{
    return t == m->eos || t == m->eot || (m->ttype && m->ttype[t] == 3);
}

int llm_next(llm_session_t *s, const llm_sampler_t *p, char *out, int max)
{
    llm_t *m = s->m;
    if (s->state != 1 || s->npend != 1) return -1;
    if (s->n_past + 2 > m->ctx || (p->max_tokens && s->ngen >= p->max_tokens)) { llm_stop(s); return -1; }
    uint64_t t0 = now(m);
    const float *lg = llm_eval(s, s->pend, 1);
    if (!lg) { llm_stop(s); return -1; }
    s->npend = 0;
    if (!s->ngen) { s->st.prompt_ns += now(m) - t0; t0 = now(m); }
    float *w = m->logits;                                     /* sample() may change the scores */
    int t = sample(s, p, w);
    s->ngen++;
    s->st.gen_tokens++;
    s->st.gen_ns += now(m) - t0;
    if (is_end(m, t)) {
        s->pend[0] = t; s->npend = 1;
        if (m->chat == CHAT_CHATML && m->nl >= 0) s->pend[s->npend++] = m->nl;   /* "<|im_end|>\n" */
        s->state = 0;
        if (s->ulen && s->ulen <= max) { memcpy(out, s->ubuf, s->ulen); int n = s->ulen; s->ulen = 0; return n; }
        return -1;
    }
    s->pend[0] = t; s->npend = 1;
    /* the token's bytes, holding back an unfinished UTF-8 character */
    char b[64 + 8];
    memcpy(b, s->ubuf, s->ulen);
    int n = s->ulen + llm_token_bytes(m, t, b + s->ulen, 64);
    int cut = n;
    for (int i = n - 1; i >= 0 && i >= n - 4; i--) {
        uint8_t c = b[i];
        if ((c & 0xC0) == 0x80) continue;                      /* continuation byte */
        int need = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (i + need > n) cut = i;
        break;
    }
    s->ulen = n - cut;
    memcpy(s->ubuf, b + cut, s->ulen);
    if (cut > max) cut = max;
    memcpy(out, b, cut);
    return cut;
}

void llm_stop(llm_session_t *s)
{
    llm_t *m = s->m;
    if (s->state != 1) return;
    s->state = 0;
    /* close the turn as the model would have */
    int end = m->eot >= 0 ? m->eot : m->eos;
    if (m->chat == CHAT_CHATML) { int32_t t; if (llm_tokenize(m, "<|im_end|>", 10, &t, 1, 1) == 1) end = t; }
    s->npend = 0;
    if (s->n_past < m->ctx - 2) {
        /* the token chosen last but not read yet is dropped: the answer ends here */
        if (end >= 0) s->pend[s->npend++] = end;
        if (m->chat == CHAT_CHATML && m->nl >= 0) s->pend[s->npend++] = m->nl;
    }
}
