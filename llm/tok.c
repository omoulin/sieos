/*
 * tok.c - Text <-> tokens.
 *
 * Two families, both read from the model file:
 *  - byte-level BPE ("gpt2": SmolLM2, Llama 3, Qwen2). The text is cut
 *    into pieces (words, numbers, punctuation, spaces), each byte is mapped
 *    to a printable character, and pairs are merged in the order of the
 *    file's merge list until none applies.
 *  - SentencePiece ("llama": Llama 2, Mistral). Spaces become U+2581, and
 *    the pair whose merge has the best score is merged first; characters
 *    the vocabulary lacks are spelled as byte tokens <0xNN>.
 * Special tokens ("<|im_start|>") are recognised in the text on request.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "internal.h"

static uint32_t fnv(const char *s, int n) { uint32_t h = 2166136261u; while (n--) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static uint32_t pairhash(int a, int b) { return (uint32_t)a * 2654435761u ^ (uint32_t)b * 40503u; }

static int vocab_find(llm_t *m, const char *s, int n)
{
    for (uint32_t i = fnv(s, n) & m->vhmask; ; i = (i + 1) & m->vhmask) {
        int id = m->vhash[i];
        if (id < 0) return -1;
        if (m->vocab[id].len == n && !memcmp(m->vocab[id].s, s, n)) return id;
    }
}

static const merge_t *merge_find(llm_t *m, int a, int b)
{
    if (!m->mtab) return 0;
    for (uint32_t i = pairhash(a, b) & m->mmask; ; i = (i + 1) & m->mmask) {
        const merge_t *e = &m->mtab[i];
        if (e->rank < 0) return 0;
        if (e->a == a && e->b == b) return e;
    }
}

/* ---- UTF-8 */
static int utf8_len(uint8_t c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }
static uint32_t utf8_get(const char *s, size_t n, size_t i, int *len)
{
    uint8_t c = s[i];
    int l = utf8_len(c);
    if (c >= 0x80 && c < 0xC0) l = 1;                      /* stray continuation byte */
    if (i + l > n) l = 1;
    uint32_t cp = l == 1 ? c : l == 2 ? c & 0x1F : l == 3 ? c & 0x0F : c & 0x07;
    for (int k = 1; k < l; k++) cp = cp << 6 | (s[i + k] & 0x3F);
    *len = l;
    return cp;
}
static int utf8_put(uint32_t cp, char *o)
{
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | cp >> 6; o[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) { o[0] = 0xE0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 0x3F); o[2] = 0x80 | (cp & 0x3F); return 3; }
    o[0] = 0xF0 | cp >> 18; o[1] = 0x80 | (cp >> 12 & 0x3F); o[2] = 0x80 | (cp >> 6 & 0x3F); o[3] = 0x80 | (cp & 0x3F); return 4;
}

/* Character classes, enough for the pre-tokenizer: letters, digits, spaces.
 * Outside ASCII: the common punctuation and symbol blocks are "other";
 * everything else counts as a letter (documented approximation). */
static int is_space(uint32_t c)
{
    return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
static int is_digit(uint32_t c) { return c >= '0' && c <= '9'; }
static int is_letter(uint32_t c)
{
    if (c < 0x80) return (c | 32) >= 'a' && (c | 32) <= 'z';
    if (c < 0xC0) return c == 0xAA || c == 0xB5 || c == 0xBA;
    if (c == 0xD7 || c == 0xF7 || is_space(c)) return 0;
    if ((c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFE30 && c <= 0xFE4F) ||
        (c >= 0xFF00 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) ||
        (c >= 0xFF5B && c <= 0xFF65) || (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0xE000 && c <= 0xF8FF)) return 0;
    return 1;
}

/* ---- GPT-2 pre-tokenizer: the end of the piece starting at i, following
 *   's|'t|'re|'ve|'m|'ll|'d | ?L+ | ?N+ | ?[^sLN]+ | s+(?!S) | s+
 * digits1: every digit is a piece of its own (SmolLM2's variant). */
static size_t piece_end(const char *s, size_t n, size_t i, int digits1)
{
    int l, l2;
    uint32_t c = utf8_get(s, n, i, &l);
    if (c == '\'' && i + 1 < n) {
        const char *ct[] = { "s", "t", "re", "ve", "m", "ll", "d" };
        for (int k = 0; k < 7; k++) {
            size_t cl = strlen(ct[k]);
            if (i + 1 + cl <= n && !memcmp(s + i + 1, ct[k], cl)) return i + 1 + cl;
        }
    }
    size_t j = i;
    uint32_t c2 = c;
    if (c == ' ' && i + 1 < n) { c2 = utf8_get(s, n, i + 1, &l2); if (!is_space(c2) && !(digits1 && is_digit(c2))) j = i + 1; }
    if (j > i || !is_space(c)) {                         /* optional space, then a run of one class */
        uint32_t f = utf8_get(s, n, j, &l);
        int cls = is_letter(f) ? 1 : is_digit(f) ? 2 : 3;
        if (cls == 2 && digits1) return j + l;
        size_t k = j + l;
        while (k < n) {
            uint32_t g = utf8_get(s, n, k, &l2);
            int gc = is_letter(g) ? 1 : is_digit(g) ? 2 : is_space(g) ? 0 : 3;
            if (gc != cls || (cls == 2 && digits1)) break;
            k += l2;
        }
        return k;
    }
    size_t k = i, last = i;                              /* whitespace run */
    uint32_t g = 0;
    while (k < n) { g = utf8_get(s, n, k, &l2); if (!is_space(g)) break; last = k; k += l2; }
    /* leave one space for the next word, unless a digit follows (digits1:
     * digits cut the text first, so the spaces end their own segment) */
    if (k < n && last > i && !(digits1 && is_digit(g))) return last;
    return k;
}

/* GPT-2's map from bytes to printable characters (and back). */
static uint32_t byte2cp[256];
static int16_t cp2byte[324];
static void bytemap(void)
{
    int n = 0;
    for (int b = 0; b < 256; b++) {
        int keep = (b >= '!' && b <= '~') || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF);
        byte2cp[b] = keep ? (uint32_t)b : (uint32_t)(256 + n++);
    }
    for (int i = 0; i < 324; i++) cp2byte[i] = -1;
    for (int b = 0; b < 256; b++) cp2byte[byte2cp[b]] = b;
}

/* BPE on one piece (GPT-2): sym[] starts as one token per mapped byte. */
static int bpe_gpt2(llm_t *m, const char *p, size_t n, int32_t *out, int max, int cnt)
{
    int32_t small[256], *sym = n <= 256 ? small : zalloc(m, n * 4);   /* long pieces: on the heap */
    int ns = 0;
    char buf[8];
    if (!sym) return cnt;
    for (size_t i = 0; i < n; i++) {
        int id = vocab_find(m, buf, utf8_put(byte2cp[(uint8_t)p[i]], buf));
        sym[ns++] = id < 0 ? 0 : id;
    }
    for (;;) {                                             /* merge the best-ranked pair */
        int best = -1, bi = -1, bo = 0;
        for (int i = 0; i + 1 < ns; i++) {
            const merge_t *e = merge_find(m, sym[i], sym[i + 1]);
            if (e && (best < 0 || e->rank < best)) { best = e->rank; bi = i; bo = e->out; }
        }
        if (bi < 0) break;
        sym[bi] = bo;
        memmove(sym + bi + 1, sym + bi + 2, (ns - bi - 2) * 4);
        ns--;
    }
    for (int k = 0; k < ns; k++) { if (cnt < max) out[cnt] = sym[k]; cnt++; }
    if (sym != small) zfree(m, sym);
    return cnt;
}

/* SentencePiece BPE on a run of text (spaces already U+2581). */
static int bpe_spm(llm_t *m, const char *p, size_t n, int32_t *out, int max, int cnt)
{
    enum { MAXS = 512 };
    struct { const char *s; int len; } sym[MAXS];
    int ns = 0;
    for (size_t i = 0; i < n; ) {
        int l; utf8_get(p, n, i, &l);
        if (ns == MAXS) break;
        sym[ns].s = p + i; sym[ns].len = l; ns++;
        i += l;
    }
    for (;;) {
        float best = -1e30f; int bi = -1;
        for (int i = 0; i + 1 < ns; i++) {
            if (sym[i].s + sym[i].len != sym[i + 1].s) continue;
            int id = vocab_find(m, sym[i].s, sym[i].len + sym[i + 1].len);
            if (id >= 0 && m->scores && m->scores[id] > best) { best = m->scores[id]; bi = i; }
        }
        if (bi < 0) break;
        sym[bi].len += sym[bi + 1].len;
        memmove(sym + bi + 1, sym + bi + 2, (ns - bi - 2) * sizeof sym[0]);
        ns--;
    }
    for (int k = 0; k < ns; k++) {
        int id = vocab_find(m, sym[k].s, sym[k].len);
        if (id >= 0) { if (cnt < max) out[cnt] = id; cnt++; continue; }
        for (int b = 0; b < sym[k].len; b++) { if (cnt < max) out[cnt] = m->byte_tok[(uint8_t)sym[k].s[b]]; cnt++; }
    }
    return cnt;
}

/* Plain text (no special tokens) to tokens. */
static int encode_text(llm_t *m, const char *s, size_t n, int32_t *out, int max, int cnt, int *first)
{
    if (m->tok_kind == TOK_GPT2) {
        for (size_t i = 0; i < n; ) {
            size_t e = piece_end(s, n, i, 1);
            if (e <= i) e = i + 1;
            cnt = bpe_gpt2(m, s + i, e - i, out, max, cnt);
            i = e;
        }
        return cnt;
    }
    /* SentencePiece: a leading space, spaces as U+2581, in runs of <= 400 bytes */
    char buf[512];
    size_t bl = 0;
    if (*first) { memcpy(buf, "\xE2\x96\x81", 3); bl = 3; *first = 0; }
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ' ') { memcpy(buf + bl, "\xE2\x96\x81", 3); bl += 3; } else buf[bl++] = s[i];
        if (bl > 400 && (i + 1 == n || (s[i + 1] & 0xC0) != 0x80)) { cnt = bpe_spm(m, buf, bl, out, max, cnt); bl = 0; }
    }
    if (bl) cnt = bpe_spm(m, buf, bl, out, max, cnt);
    return cnt;
}

int llm_tokenize(llm_t *m, const char *s, size_t n, int32_t *out, int max, int special)
{
    int cnt = 0, first = 1;
    size_t start = 0;
    if (special) for (size_t i = 0; i < n; ) {            /* cut at special tokens */
        int hit = -1;
        if (s[i] == '<' || s[i] == '[')
            for (int k = 0; k < m->nspecial && hit < 0; k++) {
                const str_t *v = &m->vocab[m->special[k]];
                if (v->len && (size_t)v->len <= n - i && !memcmp(s + i, v->s, v->len)) hit = m->special[k];
            }
        if (hit < 0) { i++; continue; }
        cnt = encode_text(m, s + start, i - start, out, max, cnt, &first);
        if (cnt < max) out[cnt] = hit;
        cnt++;
        i += m->vocab[hit].len;
        start = i;
    }
    cnt = encode_text(m, s + start, n - start, out, max, cnt, &first);
    return cnt <= max ? cnt : -cnt;
}

/* One token's bytes. Special tokens give their text; byte tokens one byte. */
int llm_token_bytes(llm_t *m, int32_t tok, char *buf, int max)
{
    if (tok < 0 || tok >= m->n_vocab) return 0;
    const str_t *v = &m->vocab[tok];
    int n = 0;
    if (m->ttype && (m->ttype[tok] == 3 || m->ttype[tok] == 4)) {
        n = v->len < max ? v->len : max;
        memcpy(buf, v->s, n);
        return n;
    }
    if (m->tok_kind == TOK_SPM) {
        if (m->ttype && m->ttype[tok] == 6 && v->len == 6) {          /* <0xNN> */
            int hi = v->s[3], lo = v->s[4];
            hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10; lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
            if (max > 0) buf[n++] = hi << 4 | lo;
            return n;
        }
        for (int i = 0; i < v->len && n < max; ) {
            if (i + 3 <= v->len && !memcmp(v->s + i, "\xE2\x96\x81", 3)) { buf[n++] = ' '; i += 3; }
            else buf[n++] = v->s[i++];
        }
        return n;
    }
    for (size_t i = 0; i < (size_t)v->len && n < max; ) {             /* undo the byte map */
        int l; uint32_t cp = utf8_get(v->s, v->len, i, &l);
        i += l;
        if (cp < 324 && cp2byte[cp] >= 0) buf[n++] = cp2byte[cp];
    }
    return n;
}

/* Build the lookup tables after loading. */
int tok_init(llm_t *m)
{
    bytemap();
    uint32_t sz = 1;
    while (sz < (uint32_t)m->n_vocab * 2) sz <<= 1;
    m->vhash = zalloc(m, sz * 4);
    if (!m->vhash) return LLM_ENOMEM;
    m->vhmask = sz - 1;
    for (uint32_t i = 0; i < sz; i++) m->vhash[i] = -1;
    for (int id = 0; id < m->n_vocab; id++) {
        if (!m->vocab[id].s) return LLM_EFORMAT;
        uint32_t i = fnv(m->vocab[id].s, m->vocab[id].len) & m->vhmask;
        while (m->vhash[i] >= 0) i = (i + 1) & m->vhmask;
        m->vhash[i] = id;
    }
    if (m->tok_kind == TOK_GPT2) {
        if (!m->merges) return LLM_EFORMAT;
        sz = 1;
        while (sz < (uint32_t)m->nmerges * 2) sz <<= 1;
        m->mtab = zalloc(m, sz * sizeof *m->mtab);
        if (!m->mtab) return LLM_ENOMEM;
        m->mmask = sz - 1;
        for (uint32_t i = 0; i < sz; i++) m->mtab[i].rank = -1;
        for (int r = 0; r < m->nmerges; r++) {               /* "a b": a, b and a+b must be tokens */
            const char *s = m->merges[r].s, *sp = s ? strchr(s, ' ') : 0;
            if (!sp) continue;
            int la = sp - s, lb = m->merges[r].len - la - 1;
            int a = vocab_find(m, s, la), b = vocab_find(m, sp + 1, lb);
            char cat[512];
            if (a < 0 || b < 0 || la + lb > (int)sizeof cat) continue;
            memcpy(cat, s, la); memcpy(cat + la, sp + 1, lb);
            int o = vocab_find(m, cat, la + lb);
            if (o < 0) continue;
            uint32_t i = pairhash(a, b) & m->mmask;
            while (m->mtab[i].rank >= 0) { if (m->mtab[i].a == a && m->mtab[i].b == b) goto next; i = (i + 1) & m->mmask; }
            m->mtab[i] = (merge_t){ 0, a, b, r, o };
        next:;
        }
    } else {
        for (int b = 0; b < 256; b++) {
            char h[7] = "<0x00>";
            h[3] = "0123456789ABCDEF"[b >> 4]; h[4] = "0123456789ABCDEF"[b & 15];
            m->byte_tok[b] = vocab_find(m, h, 6);
            if (m->byte_tok[b] < 0) m->byte_tok[b] = 0;
        }
    }
    /* special tokens, longest first, for llm_tokenize(special = 1) */
    int ns = 0;
    for (int id = 0; id < m->n_vocab; id++) if (m->ttype && (m->ttype[id] == 3 || m->ttype[id] == 4) && m->vocab[id].len > 1) ns++;
    m->special = zalloc(m, (ns + 1) * 4);
    if (!m->special) return LLM_ENOMEM;
    for (int id = 0; id < m->n_vocab; id++)
        if (m->ttype && (m->ttype[id] == 3 || m->ttype[id] == 4) && m->vocab[id].len > 1) {
            int k = m->nspecial++;
            while (k > 0 && m->vocab[m->special[k - 1]].len < m->vocab[id].len) { m->special[k] = m->special[k - 1]; k--; }
            m->special[k] = id;
        }
    if (m->add_bos < 0) m->add_bos = m->tok_kind == TOK_SPM;
    char nl = '\n';
    int32_t t;
    m->nl = llm_tokenize(m, &nl, 1, &t, 1, 0) == 1 ? t : -1;
    return 0;
}
