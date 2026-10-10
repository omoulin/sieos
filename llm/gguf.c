/*
 * gguf.c - Reading a model file (GGUF): its key/value metadata, its list of
 * tensors, then the tensor data itself.
 *
 * Layout: "GGUF", version, tensor count, key count; the keys (name, type,
 * value); the tensor descriptions (name, dimensions, type, offset); then,
 * aligned, the data. Every length and offset is checked against the file
 * size before use: a damaged file is refused, never trusted.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "internal.h"

/* ---- A buffered reader over env.read. */
typedef struct {
    llm_t *m;
    uint64_t pos, size, bpos;          /* bpos: file offset of buf[0] */
    uint32_t blen;
    int bad;
    uint8_t buf[1 << 16];
} rd_t;

static int fill(rd_t *r, uint32_t need)
{
    if (r->pos >= r->bpos && r->pos + need <= r->bpos + r->blen) return 0;
    if (need > sizeof r->buf || r->pos + need > r->size) return r->bad = 1, -1;
    uint64_t n = r->size - r->pos < sizeof r->buf ? r->size - r->pos : sizeof r->buf;
    if (r->m->env.read(r->m->env.ctx, r->buf, r->pos, n)) return r->bad = 1, -1;
    r->bpos = r->pos; r->blen = n;
    return 0;
}
static void get(rd_t *r, void *dst, uint32_t n)
{
    if (r->bad || fill(r, n)) { memset(dst, 0, n); return; }
    memcpy(dst, r->buf + (r->pos - r->bpos), n);
    r->pos += n;
}
static uint32_t u32(rd_t *r) { uint32_t v; get(r, &v, 4); return v; }
static uint64_t u64(rd_t *r) { uint64_t v; get(r, &v, 8); return v; }
static void skip(rd_t *r, uint64_t n) { if (n > r->size - r->pos) r->bad = 1; else r->pos += n; }

/* ---- A bump allocator for the metadata kept (strings, tables). */
typedef struct arena { struct arena *next; size_t used, cap; } arena_t;
static void *aalloc(llm_t *m, size_t n)
{
    n = (n + 15) & ~(size_t)15;
    arena_t *a = m->arena;
    if (!a || a->used + n > a->cap) {
        size_t cap = n > (1 << 20) ? n : (1 << 20);
        arena_t *b = m->env.alloc(m->env.ctx, sizeof *b + 64 + cap);
        if (!b) return 0;
        b->next = a; b->used = 0; b->cap = cap;
        m->arena = a = b;
    }
    void *p = (uint8_t *)(a + 1) + 64 + a->used;
    a->used += n;
    return p;
}

/* A string: length (u64) then bytes. Copied (0-terminated) if keep. */
static char *str(rd_t *r, int keep, uint32_t *len)
{
    uint64_t n = u64(r);
    if (n > (1 << 20) || n > r->size - r->pos) { r->bad = 1; return 0; }
    if (len) *len = n;
    if (!keep) { skip(r, n); return 0; }
    char *s = aalloc(r->m, n + 1);
    if (!s) { r->bad = 1; return 0; }
    for (uint64_t done = 0; done < n; ) {                 /* in pieces: may exceed the buffer */
        uint32_t c = n - done > 4096 ? 4096 : n - done;
        get(r, s + done, c);
        done += c;
    }
    s[n] = 0;
    return s;
}

enum { G_U8, G_I8, G_U16, G_I16, G_U32, G_I32, G_F32, G_BOOL, G_STR, G_ARR, G_U64, G_I64, G_F64 };
static const uint8_t gsize[] = { 1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8 };

/* A scalar value as a number (whatever its type). */
static double num(rd_t *r, uint32_t t)
{
    uint8_t b[8] = { 0 };
    if (t >= sizeof gsize || !gsize[t]) { r->bad = 1; return 0; }
    get(r, b, gsize[t]);
    switch (t) {
    case G_U8: case G_BOOL: return b[0];
    case G_I8: return (int8_t)b[0];
    case G_U16: { uint16_t v; memcpy(&v, b, 2); return v; }
    case G_I16: { int16_t v; memcpy(&v, b, 2); return v; }
    case G_U32: { uint32_t v; memcpy(&v, b, 4); return v; }
    case G_I32: { int32_t v; memcpy(&v, b, 4); return v; }
    case G_F32: { float v; memcpy(&v, b, 4); return v; }
    case G_U64: { uint64_t v; memcpy(&v, b, 8); return (double)v; }
    case G_I64: { int64_t v; memcpy(&v, b, 8); return (double)v; }
    default: { double v; memcpy(&v, b, 8); return v; }
    }
}

static void skip_value(rd_t *r, uint32_t t, int depth)
{
    if (t == G_STR) { str(r, 0, 0); return; }
    if (t == G_ARR) {
        uint32_t et = u32(r); uint64_t n = u64(r);
        if (depth > 2 || et > G_F64) { r->bad = 1; return; }
        if (et == G_STR || et == G_ARR) { for (uint64_t i = 0; i < n && !r->bad; i++) skip_value(r, et, depth + 1); }
        else if (n > r->size / gsize[et]) r->bad = 1;
        else skip(r, n * gsize[et]);
        return;
    }
    if (t >= sizeof gsize || !gsize[t]) { r->bad = 1; return; }
    skip(r, gsize[t]);
}

static int ends(const char *k, const char *suffix)
{
    size_t a = strlen(k), b = strlen(suffix);
    return a > b && !strcmp(k + a - b, suffix) && k[a - b - 1] == '.';
}

typedef struct { char *name; uint32_t type, ndim; uint64_t ne[4], off; } tdesc_t;

static void say(char *err, size_t n, const char *msg) { if (err && n) strlcpy(err, msg, n); }

int gguf_load(llm_t *m, uint64_t size, char *err, size_t errlen)
{
    rd_t *r = m->env.alloc(m->env.ctx, sizeof *r);
    if (!r) return say(err, errlen, "out of memory"), LLM_ENOMEM;
    memset(r, 0, offsetof(rd_t, buf));
    r->m = m; r->size = size;
    int ret = LLM_EFORMAT;
    tdesc_t *td = 0;

    uint32_t magic = u32(r), ver = u32(r);
    uint64_t ntens = u64(r), nkv = u64(r);
    if (magic != 0x46554747 || ver < 2 || ver > 3 || ntens > 100000 || nkv > 100000) { say(err, errlen, "not a GGUF v2/v3 file"); goto out; }

    /* ---- keys */
    char *arch = 0, *name = 0, *tmodel = 0, *tmpl = 0;
    uint64_t align = 32;
    double ctx = 0, embd = 0, layers = 0, ff = 0, heads = 0, kvheads = 0, theta = 10000, eps = 1e-5;
    m->bos = m->eos = m->eot = -1; m->add_bos = -1;
    for (uint64_t i = 0; i < nkv && !r->bad; i++) {
        char *k = str(r, 1, 0);
        uint32_t t = u32(r);
        if (r->bad || !k) break;
        if (t == G_STR && (!strcmp(k, "general.architecture") || !strcmp(k, "general.name") ||
                           !strcmp(k, "tokenizer.ggml.model") || !strcmp(k, "tokenizer.chat_template"))) {
            char *v = str(r, 1, 0);
            if (!strcmp(k, "general.architecture")) arch = v;
            else if (!strcmp(k, "general.name")) name = v;
            else if (!strcmp(k, "tokenizer.ggml.model")) tmodel = v;
            else tmpl = v;
        } else if (t == G_ARR && (!strcmp(k, "tokenizer.ggml.tokens") || !strcmp(k, "tokenizer.ggml.scores") ||
                                  !strcmp(k, "tokenizer.ggml.token_type") || !strcmp(k, "tokenizer.ggml.merges"))) {
            uint32_t et = u32(r); uint64_t n = u64(r);
            if (n > (1 << 22)) { r->bad = 1; break; }
            if (!strcmp(k, "tokenizer.ggml.tokens") || !strcmp(k, "tokenizer.ggml.merges")) {
                if (et != G_STR) { r->bad = 1; break; }
                str_t *a = aalloc(m, (n + 1) * sizeof *a);
                if (!a) { r->bad = 1; break; }
                for (uint64_t j = 0; j < n && !r->bad; j++) { uint32_t len; a[j].s = str(r, 1, &len); a[j].len = len; }
                a[n].s = 0;
                if (!strcmp(k, "tokenizer.ggml.tokens")) { m->vocab = a; m->n_vocab = n; }
                else { m->merges = a; m->nmerges = n; }
            } else {
                if (et >= sizeof gsize || !gsize[et]) { r->bad = 1; break; }
                void *a = aalloc(m, n * 4 + 4);
                if (!a) { r->bad = 1; break; }
                for (uint64_t j = 0; j < n && !r->bad; j++) {
                    double v = num(r, et);
                    if (k[15] == 's') ((float *)a)[j] = v; else ((int32_t *)a)[j] = (int32_t)v;
                }
                if (k[15] == 's') m->scores = a; else m->ttype = a;
            }
        } else if (t != G_STR && t != G_ARR) {
            double v = num(r, t);
            if (!strcmp(k, "general.alignment")) align = (uint64_t)v;
            else if (str_prefix(k, "general.")) ;
            else if (!strcmp(k, "tokenizer.ggml.bos_token_id")) m->bos = v;
            else if (!strcmp(k, "tokenizer.ggml.eos_token_id")) m->eos = v;
            else if (!strcmp(k, "tokenizer.ggml.eot_token_id")) m->eot = v;
            else if (!strcmp(k, "tokenizer.ggml.add_bos_token")) m->add_bos = v != 0;
            else if (ends(k, "context_length")) ctx = v;
            else if (ends(k, "embedding_length")) embd = v;
            else if (ends(k, "block_count")) layers = v;
            else if (ends(k, "feed_forward_length")) ff = v;
            else if (ends(k, "attention.head_count")) heads = v;
            else if (ends(k, "attention.head_count_kv")) kvheads = v;
            else if (ends(k, "rope.freq_base")) theta = v;
            else if (ends(k, "attention.layer_norm_rms_epsilon")) eps = v;
        } else skip_value(r, t, 0);
    }
    if (r->bad) { say(err, errlen, "damaged metadata"); goto out; }
    if (!arch || !m->vocab || !tmodel) { say(err, errlen, "missing architecture or vocabulary"); goto out; }
    if (strcmp(arch, "llama") && strcmp(arch, "qwen2") && strcmp(arch, "mistral")) {
        say(err, errlen, "unsupported architecture (llama, mistral, qwen2 only)"); ret = LLM_EUNSUP; goto out;
    }
    if (!kvheads) kvheads = heads;
    if (embd < 64 || embd > 65536 || layers < 1 || layers > 512 || heads < 1 || heads > 1024 || ff < 1 || ff > 262144 ||
        kvheads < 1 || (int)heads % (int)kvheads || (int)embd % (int)heads || align < 8 || align > 65536 || (align & (align - 1)) ||
        m->n_vocab < 16) { say(err, errlen, "impossible model dimensions"); goto out; }
    m->n_embd = embd; m->n_layer = layers; m->n_ff = ff; m->n_head = heads; m->n_kv = kvheads;
    m->hd = m->n_embd / m->n_head; m->theta = theta; m->eps = eps;
    m->rope_neox = !strcmp(arch, "qwen2");
    m->tok_kind = !strcmp(tmodel, "gpt2") ? TOK_GPT2 : !strcmp(tmodel, "llama") ? TOK_SPM : 0;
    if (!m->tok_kind) { say(err, errlen, "unsupported tokenizer"); ret = LLM_EUNSUP; goto out; }
    m->chat = !tmpl ? CHAT_CHATML : strstr_simple(tmpl, "<|start_header_id|>") ? CHAT_LLAMA3
            : strstr_simple(tmpl, "<|im_start|>") ? CHAT_CHATML : strstr_simple(tmpl, "[INST]") ? CHAT_INST : CHAT_CHATML;
    strlcpy(m->info.arch, arch, sizeof m->info.arch);
    strlcpy(m->info.name, name ? name : arch, sizeof m->info.name);
    m->info.ctx_train = ctx;

    /* ---- tensor descriptions */
    td = m->env.alloc(m->env.ctx, ntens * sizeof *td);
    if (!td) { ret = LLM_ENOMEM; say(err, errlen, "out of memory"); goto out; }
    for (uint64_t i = 0; i < ntens && !r->bad; i++) {
        td[i].name = str(r, 1, 0);
        td[i].ndim = u32(r);
        if (td[i].ndim < 1 || td[i].ndim > 4) { r->bad = 1; break; }
        for (int d = 0; d < 4; d++) td[i].ne[d] = d < (int)td[i].ndim ? u64(r) : 1;
        td[i].type = u32(r);
        td[i].off = u64(r);
    }
    if (r->bad) { say(err, errlen, "damaged tensor list"); goto out; }
    uint64_t data = (r->pos + align - 1) & ~(align - 1);

    /* ---- check every tensor: known type, sizes that fit, inside the file */
    uint64_t total = 0;
    for (uint64_t i = 0; i < ntens; i++) {
        tdesc_t *t = &td[i];
        if (t->type >= T_NTYPES || !tinfo[t->type].bs) { say(err, errlen, "unsupported tensor type"); ret = LLM_EUNSUP; goto out; }
        uint64_t n = 1;
        for (int d = 0; d < 4; d++) { if (!t->ne[d] || t->ne[d] > (1ULL << 32) || n > (1ULL << 40) / t->ne[d]) { say(err, errlen, "bad tensor shape"); goto out; } n *= t->ne[d]; }
        if (t->ne[0] % tinfo[t->type].bs) { say(err, errlen, "tensor rows not whole blocks"); goto out; }
        uint64_t bytes = n / tinfo[t->type].bs * tinfo[t->type].bytes;
        if (t->off > size || data + t->off > size || bytes > size - data - t->off || t->off % align) { say(err, errlen, "tensor outside the file"); goto out; }
        if (t->off + bytes > total) total = t->off + bytes;
    }

    /* ---- the data: one block of memory, if the budget allows */
    m->wbytes = total;
    if (total > m->env.budget) { say(err, errlen, "model larger than the memory budget"); ret = LLM_EBUDGET; goto out; }
    m->weights = m->env.alloc(m->env.ctx, total + 64);
    if (!m->weights) { ret = LLM_ENOMEM; say(err, errlen, "out of memory for the weights"); goto out; }
    for (uint64_t done = 0; done < total; ) {
        uint64_t c = total - done > (64u << 20) ? (64u << 20) : total - done;
        if (m->env.read(m->env.ctx, m->weights + done, data + done, c)) { ret = LLM_EIO; say(err, errlen, "read error"); goto out; }
        done += c;
    }

    /* ---- find the tensors the model needs */
    m->L = zalloc(m, m->n_layer * sizeof *m->L);
    if (!m->L) { ret = LLM_ENOMEM; goto out; }
    ret = LLM_EFORMAT;
    for (uint64_t i = 0; i < ntens; i++) {
        tdesc_t *t = &td[i];
        const uint8_t *p = m->weights + t->off;
        mat_t mt = { (int)t->type, (int)t->ne[1], (int)t->ne[0], t->ne[0] / tinfo[t->type].bs * tinfo[t->type].bytes, p, 0, 0 };
        const char *n = t->name;
        int is_vec = t->ndim == 1, isf32 = t->type == T_F32;
        if (!strcmp(n, "token_embd.weight")) m->embd = mt;
        else if (!strcmp(n, "output.weight")) m->out = mt;
        else if (!strcmp(n, "output_norm.weight") && is_vec && isf32 && t->ne[0] == (uint64_t)m->n_embd) m->out_norm = (const float *)p;
        else if (!strcmp(n, "rope_freqs.weight") && is_vec && isf32 && t->ne[0] == (uint64_t)m->hd / 2) m->rope_factors = (const float *)p;
        else if (str_prefix(n, "blk.")) {
            long l = strnum_prefix(n + 4);
            const char *d = strchr(n + 4, '.');
            if (l < 0 || l >= m->n_layer || !d) continue;
            layer_t *L = &m->L[l];
            d++;
            if (!strcmp(d, "attn_q.weight")) L->wq = mt;
            else if (!strcmp(d, "attn_k.weight")) L->wk = mt;
            else if (!strcmp(d, "attn_v.weight")) L->wv = mt;
            else if (!strcmp(d, "attn_output.weight")) L->wo = mt;
            else if (!strcmp(d, "ffn_gate.weight")) L->wg = mt;
            else if (!strcmp(d, "ffn_up.weight")) L->wu = mt;
            else if (!strcmp(d, "ffn_down.weight")) L->wd = mt;
            else if (is_vec && isf32 && !strcmp(d, "attn_norm.weight") && t->ne[0] == (uint64_t)m->n_embd) L->attn_norm = (const float *)p;
            else if (is_vec && isf32 && !strcmp(d, "ffn_norm.weight") && t->ne[0] == (uint64_t)m->n_embd) L->ffn_norm = (const float *)p;
            else if (is_vec && isf32 && !strcmp(d, "attn_q.bias") && t->ne[0] == (uint64_t)m->n_embd) L->bq = (const float *)p;
            else if (is_vec && isf32 && !strcmp(d, "attn_k.bias") && t->ne[0] == (uint64_t)(m->n_kv * m->hd)) L->bk = (const float *)p;
            else if (is_vec && isf32 && !strcmp(d, "attn_v.bias") && t->ne[0] == (uint64_t)(m->n_kv * m->hd)) L->bv = (const float *)p;
        }
    }
    if (!m->out.data) m->out = m->embd;                    /* tied embeddings */
    int E = m->n_embd, KV = m->n_kv * m->hd, F = m->n_ff;
#define SHAPE(mt, R, C) ((mt).data && (mt).rows == (R) && (mt).cols == (C))
    if (!SHAPE(m->embd, m->n_vocab, E) || !SHAPE(m->out, m->n_vocab, E) || !m->out_norm) { say(err, errlen, "missing or misshapen embeddings"); goto out; }
    for (int l = 0; l < m->n_layer; l++) {
        layer_t *L = &m->L[l];
        if (!SHAPE(L->wq, E, E) || !SHAPE(L->wk, KV, E) || !SHAPE(L->wv, KV, E) || !SHAPE(L->wo, E, E) ||
            !SHAPE(L->wg, F, E) || !SHAPE(L->wu, F, E) || !SHAPE(L->wd, E, F) || !L->attn_norm || !L->ffn_norm) {
            say(err, errlen, "missing or misshapen layer tensor"); goto out;
        }
    }
    ret = 0;
out:
    if (td) m->env.free(m->env.ctx, td);
    m->env.free(m->env.ctx, r);
    return ret;
}
