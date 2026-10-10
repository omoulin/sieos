/*
 * siad - The assistant server, port "sia" (protocol SIA_* in mk/proto.h).
 *
 *   client ──SIA_OPEN──► siad          a session: one conversation
 *          ──SIA_ASK───►               the user's message (answered at once)
 *          ──SIA_NEXT──►  ──backend──► the next piece of the answer, again
 *          ◄── text ─────              and again until 0 (the end)
 *
 * The answer comes from a backend (backend.h): the local model (local.c,
 * our engine on this machine's processors) or a remote one with an
 * OpenAI-compatible API (remote.c), chosen in /etc/sia.conf:
 *     backend=local            model=/models/....gguf   ctx=2048  threads=N
 *     backend=remote           url=https://host/v1      api_model=NAME
 * The API key is kept apart, in /etc/sia.key (readable by root only).
 * SIA_CONFIG (root only) changes them; siad then ends and init starts it
 * again with the new settings.
 *
 * Several sessions may be open at once; they take turns, one piece of
 * answer each (each client asks for its next piece). A client that ended
 * without SIA_CLOSE leaves a session behind: reclaimed when the table is full.
 *
 * Memory (memory.c, docs/sia.md): every conversation is saved as it goes,
 * in /var/sia/UID (the caller's uid, as the kernel stamped it), so it can be
 * resumed after a restart (SIA_OPEN with SIA_RESUME). The system text given
 * to the model is built from: what sia is, the facts it remembers about the
 * user (SIA_FACTS), the conversation's own instructions (SIA_CONTEXT) and
 * the summary of its older turns. When a conversation fills compact_at % of
 * the model's context, the older turns are replaced by a summary the model
 * writes itself (compaction), right after the answer has been delivered.
 *
 * Init gives siad a memory limit (4.5 GiB): the kernel refuses it more.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "mk/crypto.h"                  /* wipe */
#include "backend.h"
#include "memory.h"

#define MAXSESS 8
#define MAXTURN 64
#define FACTS_MAX 2048                   /* what sia remembers about a user, at most */
typedef struct {
    int used, pid, uid;                  /* the client that opened it */
    void *b;                             /* the backend's state */
    int pending;                         /* a message waits to be read (SIA_ASK) */
    int answering;                       /* an answer is being produced */
    sia_turn_t turn[MAXTURN];            /* the conversation (texts malloc'd); turn[0]: the system text */
    int nturn;
    char *answer; size_t alen, acap;     /* the answer so far: added to the conversation */
    int conv;                            /* its id on the disk (0: not saved) */
    char *ctx, *digest;                  /* extra instructions; summary of the older turns */
    int compact_due;                     /* too long: compact once the answer is delivered */
    long logsize;                        /* bytes of its log */
} sess_t;
static sess_t sess[MAXSESS];
static sia_backend_t *B;
static int ready;                        /* the backend is ready (model loaded / endpoint set) */
static char conf[1024];
static int memory_on = 1, compact_at = 60, keep_turns = 4, ctx_tokens;
static char offered[MAXSESS][512];       /* "[remember: ...]" lines of the last answers, per session */
/* The model's cache of the last conversation closed, kept "warm": each
 * `sia QUESTION` is a new client, and continuing the same conversation
 * then needs no re-reading of its history. One only (it holds memory). */
static struct { void *b; int uid, conv, nturn; size_t sys; } warm;

static struct { char b[200]; size_t n; } logline;
static void lput(void *c, char ch) { (void)c; if (logline.n < sizeof logline.b) logline.b[logline.n++] = ch; }
void sia_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    logline.n = 0;
    vformat(lput, 0, fmt, ap);
    va_end(ap);
    sys_debug(logline.b, logline.n);
}

/* /etc/sia.conf, then the key from /etc/sia.key as one more "api_key=" line. */
static void load_conf(void)
{
    size_t n = 0;
    char *f = file_get("/etc/sia.conf", &n);
    if (f) { strlcpy(conf, f, sizeof conf); free(f); }
    char *k = file_get("/etc/sia.key", &n);
    if (k) {
        size_t l = strlen(conf);
        if (l && conf[l - 1] != '\n' && l + 1 < sizeof conf) conf[l++] = '\n', conf[l] = 0;
        if (l + 8 + strlen(k) < sizeof conf) { strlcpy(conf + l, "api_key=", sizeof conf - l); strlcpy(conf + l + 8, k, sizeof conf - l - 8); }
        wipe(k, strlen(k));
        free(k);
    }
}

static int conf_is(const char *key, const char *val)
{
    size_t kl = strlen(key), vl = strlen(val);
    for (const char *p = conf; *p; ) {
        if (!memcmp(p, key, kl) && p[kl] == '=' && !memcmp(p + kl + 1, val, vl) &&
            (p[kl + 1 + vl] == '\n' || !p[kl + 1 + vl])) return 1;
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return 0;
}

/* SIA_CONFIG "key=value": replace that line of /etc/sia.conf (the key goes
 * to /etc/sia.key, 0600). Then end: init starts siad again, which reads it. */
static long configure(const char *kv)
{
    const char *eq = strchr(kv, '=');
    if (!eq || eq == kv || strchr(kv, '\n') || strlen(kv) > 400) return -EINVAL;
    size_t kl = eq - kv;
    if (kl == 7 && !memcmp(kv, "api_key", 7)) {
        long h = fs_open("/etc/sia.key", FS_WRONLY | FS_CREAT | FS_TRUNC, 0600);
        if (h < 0) return h;
        fs_chmod("/etc/sia.key", 0600);
        long r = fs_write(h, 0, eq + 1, strlen(eq + 1));
        fs_close(h);
        return r < 0 ? r : 0;
    }
    static char out[1024];
    size_t n = 0;
    char *f = file_get("/etc/sia.conf", &n), *p = f;
    out[0] = 0;
    n = 0;
    while (p && *p) {                     /* every other line, as it was */
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (!(l > kl && !memcmp(p, kv, kl) && p[kl] == '=') && l && n + l + 1 < sizeof out) {
            memcpy(out + n, p, l);
            out[n += l] = '\n';
            out[++n] = 0;
        }
        p = e ? e + 1 : p + l;
    }
    free(f);
    if (n + strlen(kv) + 2 >= sizeof out) return -ENOSPC;
    strlcpy(out + n, kv, sizeof out - n);
    n += strlen(kv);
    out[n++] = '\n';
    out[n] = 0;
    long h = fs_open("/etc/sia.conf.new", FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
    if (h < 0) return h;
    long r = fs_write(h, 0, out, n);
    fs_close(h);
    if (r < 0) return r;
    return fs_rename("/etc/sia.conf.new", "/etc/sia.conf");
}

/* A number from /etc/sia.conf, or def. */
static long conf_num(const char *key, long def)
{
    size_t kl = strlen(key);
    for (const char *p = conf; *p; ) {
        if (!memcmp(p, key, kl) && p[kl] == '=') {
            char v[24];
            size_t n = 0;
            for (const char *q = p + kl + 1; *q && *q != '\n' && n < sizeof v - 1; q++) v[n++] = *q;
            v[n] = 0;
            long x = strnum(v);
            return x >= 0 ? x : def;
        }
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return def;
}

static char *dup(const char *t, size_t n)
{
    char *c = malloc(n + 1);
    if (c) { memcpy(c, t, n); c[n] = 0; }
    return c;
}

/* What sia is, and how it may ask to remember something. */
static const char *base_text =
    "You are sia, the assistant built into SIEOS, a small microkernel operating system. "
    "Answer briefly and clearly. Rarely, when the user states something lasting about "
    "themselves that would help in later conversations (their name, a preference, their "
    "project), end the answer with a last line: [remember: <that fact>]. Most answers have no "
    "such line; never write it for a fact already listed below.";

/* turn[0], the system text: what sia is + what it remembers about the user +
 * this conversation's instructions + the summary of its older turns. */
static void system_text(sess_t *s)
{
    char *facts = memory_on ? mem_facts(s->uid) : 0;
    size_t n = strlen(base_text) + (facts ? strlen(facts) + 64 : 0) + (s->ctx ? strlen(s->ctx) + 4 : 0)
             + (s->digest ? strlen(s->digest) + 64 : 0) + 1;
    char *t = malloc(n), *o = t;
    if (!t) { free(facts); return; }
    o += strlcpy(o, base_text, n);
    if (facts && *facts) { o += strlcpy(o, "\n\nWhat you remember about the user:\n", t + n - o); o += strlcpy(o, facts, t + n - o); }
    if (s->ctx) { o += strlcpy(o, "\n\n", t + n - o); o += strlcpy(o, s->ctx, t + n - o); }
    if (s->digest) { o += strlcpy(o, "\n\nSummary of the conversation so far:\n", t + n - o); strlcpy(o, s->digest, t + n - o); }
    free(facts);
    if (s->nturn) free((void *)s->turn[0].text);
    else s->nturn = 1;
    s->turn[0] = (sia_turn_t){ SIA_SYSTEM, t };
}

/* Throw away the model's cache of this conversation: the next answer reads
 * the conversation again from its turns (new system text, compaction). */
static void renew(sess_t *s)
{
    if (s->answering) return;
    void *b = B->session_new(B);
    if (!b) return;
    B->session_free(B, s->b);
    s->b = b;
}

static void drop_warm(void) { if (warm.b) B->session_free(B, warm.b); memset(&warm, 0, sizeof warm); }

static void forget(sess_t *s)
{
    if (s->conv && s->b && !s->answering && !s->pending) {   /* keep its cache for the next client */
        drop_warm();
        warm.b = s->b; warm.uid = s->uid; warm.conv = s->conv; warm.nturn = s->nturn;
        warm.sys = s->nturn ? strlen(s->turn[0].text) : 0;
        s->b = 0;
    }
    for (int i = 0; i < s->nturn; i++) free((void *)s->turn[i].text);
    free(s->answer); free(s->ctx); free(s->digest);
    if (s->b) B->session_free(B, s->b);
    offered[s - sess][0] = 0;
    memset(s, 0, sizeof *s);
}

/* A free slot, reclaiming sessions of clients that no longer exist. */
static sess_t *slot(void)
{
    for (int i = 0; i < MAXSESS; i++) if (!sess[i].used) return &sess[i];
    for (int i = 0; i < MAXSESS; i++) {
        mk_ident_t id;
        if (sys_ident(sess[i].pid, &id) < 0) { forget(&sess[i]); return &sess[i]; }
    }
    return 0;
}

static sess_t *get(msg_t *m)
{
    uint64_t i = m->w[1];
    if (i < 1 || i > MAXSESS || !sess[i - 1].used || sess[i - 1].pid != m->pid) return 0;
    return &sess[i - 1];
}

/* Add a turn (and save it). */
static int add_turn(sess_t *s, int role, const char *text, size_t n)
{
    if (s->nturn == MAXTURN) {           /* drop the oldest exchange, keep the system text */
        free((void *)s->turn[1].text); free((void *)s->turn[2].text);
        memmove(&s->turn[1], &s->turn[3], (s->nturn - 3) * sizeof s->turn[0]);
        s->nturn -= 2;
    }
    char *t = dup(text, n);
    if (!t) return -ENOMEM;
    s->turn[s->nturn++] = (sia_turn_t){ role, t };
    if (s->conv) {
        long r = mem_add(s->uid, s->conv, role == SIA_USER ? 'U' : 'A', text, n);
        if (r > 0) s->logsize = r;
    }
    return 0;
}

/* About how many tokens the whole conversation takes. */
static int tokens(sess_t *s)
{
    int n = 0;
    for (int i = 0; i < s->nturn; i++)
        n += B->tokens && ready ? B->tokens(B, s->turn[i].text) : (int)(strlen(s->turn[i].text) + 3) / 4;
    return n + 8 * s->nturn;             /* (the template's markers) */
}

static const char *find(const char *s, const char *w)   /* first w in s, or 0 */
{
    size_t n = strlen(w);
    for (; *s; s++) if (!memcmp(s, w, n)) return s;
    return 0;
}
static int has_nl(const char *s, size_t n) { while (n--) if (*s++ == '\n') return 1; return 0; }

/* Lines "[remember: ...]" of an answer: offered, kept until the user
 * decides. Small models copy the instruction literally: placeholders, facts
 * already known and repeats are dropped. */
static void offers(sess_t *s, const char *a)
{
    char *o = offered[s - sess], *known = memory_on ? mem_facts(s->uid) : 0;
    size_t n = strlen(o);
    for (const char *p = a; (p = find(p, "[remember:")); ) {
        const char *b = p + 10, *e = strchr(b, ']');
        if (!e) break;
        while (*b == ' ') b++;
        size_t l = e - b;
        while (l && (b[l - 1] == ' ' || b[l - 1] == '.')) l--;
        char f[200];
        p = e;
        if (!l || l >= sizeof f || has_nl(b, l)) continue;
        memcpy(f, b, l); f[l] = 0;
        if (strchr(f, '<') || find(f, "the fact") || find(f, "that fact") || l < 4) continue;   /* placeholders */
        if ((known && find(known, f)) || find(o, f)) continue;                                /* known, repeated */
        if (n + l + 2 < sizeof offered[0]) { memcpy(o + n, f, l); n += l; o[n++] = '\n'; o[n] = 0; }
    }
    free(known);
}

/* The "[remember: ...]" lines out of an answer before it is saved: they
 * are offers, not conversation (seen again, the model imitates them). */
static size_t strip_tags(char *a)
{
    for (char *p; (p = (char *)find(a, "[remember:")); ) {
        char *e = strchr(p, ']');
        if (!e) { *p = 0; break; }
        memmove(p, e + 1, strlen(e + 1) + 1);
    }
    size_t n = strlen(a);
    while (n && (a[n - 1] == '\n' || a[n - 1] == ' ')) a[--n] = 0;
    return n;
}

/* The next piece of s's answer into buf: bytes, 0 at the end, or -error. */
static long next_piece(sess_t *s, char *buf, int cap)
{
    if (s->pending) {
        s->pending = 0;
        long e = B->begin(B, s->b, s->turn, s->nturn);
        if (e < 0) return e;
        s->answering = 1;
        s->alen = 0;
    }
    if (!s->answering) return 0;
    long n = B->next(B, s->b, buf, cap);
    if (n > 0 && s->alen + n + 1 > s->acap) {          /* keep the answer for the conversation */
        size_t c = (s->alen + n + 1) * 2;
        char *a = realloc(s->answer, c);
        if (a) { s->answer = a; s->acap = c; }
    }
    if (n > 0 && s->alen + n < s->acap) { memcpy(s->answer + s->alen, buf, n); s->alen += n; }
    if (n <= 0) {
        s->answering = 0;
        if (s->answer) s->answer[s->alen] = 0;
        if (s->answer) { offers(s, s->answer); s->alen = strip_tags(s->answer); }
        add_turn(s, SIA_ASSISTANT, s->answer ? s->answer : "", s->alen);
        int limit = ctx_tokens * compact_at / 100;
        if (s->nturn > 1 + keep_turns && (tokens(s) > limit || s->logsize > (long)conf_num("max_log_kb", 256) << 10))
            s->compact_due = 1;
    }
    return n;
}

/* The model summarizes text: its whole answer in out (cap bytes). 0, or -error.
 * Small models tend to continue a transcript instead of summarizing it: the
 * text goes between markers, the instruction after it, and lines that still
 * look like a transcript ("User: ...") are dropped. */
static int summarize(sess_t *s, const char *text, char *out, size_t cap)
{
    static const char *ask =
        "\n<<<END\n\nWrite short notes about the conversation above, in at most 80 words: what the "
        "user said about themselves, their work and their preferences, and what was decided. "
        "Start with \"The user\". Copy names and numbers exactly. Only facts from the text.";
    size_t tl = strlen(text), al = strlen(ask);
    char *u = malloc(tl + al + 32);
    if (!u) return -ENOMEM;
    char *q = u + strlcpy(u, "Conversation:\nSTART>>>\n", 32);
    memcpy(q, text, tl); memcpy(q + tl, ask, al + 1);
    sia_turn_t req[2] = { { SIA_SYSTEM, "You write short, exact notes about conversations." }, { SIA_USER, u } };
    size_t n = 0;
    renew(s);                                          /* (the cache is renewed afterwards anyway) */
    int e = B->begin(B, s->b, req, 2);
    for (long k; !e && n < cap - 1 && (k = B->next(B, s->b, out + n, (int)(cap - 1 - n))) > 0; ) n += k;
    free(u);
    out[n] = 0;
    strip_tags(out);
    char *w = out;                                     /* keep the lines that are notes */
    for (char *r = out, *eol; *r; r = eol) {
        eol = strchr(r, '\n');
        eol = eol ? eol + 1 : r + strlen(r);
        if (!memcmp(r, "User:", 5) || !memcmp(r, "Assistant:", 10) || !memcmp(r, "<<<", 3) || !memcmp(r, "START", 5)) continue;
        memmove(w, r, eol - r);
        w += eol - r;
    }
    *w = 0;
    n = strip_tags(out);
    return e ? -EIO : n ? 0 : -EIO;
}

/* Compaction: the turns before the last keep_turns are summarized by the
 * model and the notes appended to the conversation's summary; those turns
 * are dropped. Only the dropped turns are summarized (summarizing summaries
 * again and again drifts); the summary itself is condensed only when it
 * outgrows DIGEST_MAX. Done after the answer was delivered. */
#define DIGEST_MAX 1200
static void compact(sess_t *s)
{
    s->compact_due = 0;
    int old = s->nturn - 1 - keep_turns;  /* turns 1..old are summarized */
    if (old < 2) return;
    if (s->turn[1 + old].role == SIA_ASSISTANT) old++;   /* (the kept part starts with a question) */
    uint64_t t0 = sys_clock();
    int before = tokens(s);
    size_t n = 64;
    for (int i = 1; i <= old; i++) n += strlen(s->turn[i].text) + 16;
    char *tr = malloc(n), *o = tr;
    if (!tr) return;
    *o = 0;
    for (int i = 1; i <= old; i++) {
        o += strlcpy(o, s->turn[i].role == SIA_USER ? "User: " : "Assistant: ", tr + n - o);
        o += strlcpy(o, s->turn[i].text, tr + n - o);
        o += strlcpy(o, "\n", tr + n - o);
    }
    static char notes[1024], d[DIGEST_MAX * 2 + 2];
    int e = summarize(s, tr, notes, sizeof notes);
    free(tr);
    if (e) { sia_log("siad: compaction failed (no summary); the conversation is unchanged\n"); renew(s); return; }
    size_t dl = 0;
    d[0] = 0;
    if (s->digest) dl = strlcpy(d, s->digest, sizeof d);
    if (dl && dl < sizeof d - 2) { d[dl++] = '\n'; d[dl] = 0; }
    if (dl < sizeof d) dl += strlcpy(d + dl, notes, sizeof d - dl);
    if (dl > DIGEST_MAX && summarize(s, d, notes, sizeof notes) == 0) dl = strlcpy(d, notes, sizeof d);
    if (dl >= sizeof d) dl = sizeof d - 1;
    free(s->digest);
    s->digest = dup(d, dl);
    for (int i = 1; i <= old; i++) free((void *)s->turn[i].text);
    memmove(&s->turn[1], &s->turn[1 + old], (s->nturn - 1 - old) * sizeof s->turn[0]);
    s->nturn -= old;
    system_text(s);
    renew(s);
    if (s->conv) {
        conv_t c = { .ctx = s->ctx, .digest = s->digest, .nturn = s->nturn - 1 };
        conv_t prev;
        if (mem_load(s->uid, s->conv, &prev) == 0) { c.title = prev.title; prev.title = 0; mem_free(&prev); }
        memcpy(c.turn, &s->turn[1], (s->nturn - 1) * sizeof s->turn[0]);
        if (mem_rewrite(s->uid, s->conv, &c) < 0) sia_log("siad: compaction: the log could not be rewritten\n");
        free(c.title);
        s->logsize = 0;
    }
    sia_log("siad: compacted conversation %d: %d -> %d tokens (%d turns summarized) in %lu ms\n",
            s->conv, before, tokens(s), old, (sys_clock() - t0) / 1000000);
}

/* SIA_OPEN: a new conversation, or (SIA_RESUME) a saved one read back. */
static long open_session(msg_t *m, long *conv_out, long *resumed)
{
    sess_t *s = slot();
    if (!s) return -EBUSY;
    if (!(s->b = B->session_new(B))) { drop_warm(); s->b = B->session_new(B); }
    if (!s->b) return -ENOMEM;                         /* (over the memory budget) */
    s->used = 1; s->pid = m->pid; s->uid = m->uid;
    *resumed = 0;
    if (memory_on) {
        int id = (m->w[1] & SIA_RESUME) ? (m->w[2] ? (int)m->w[2] : mem_last(s->uid)) : 0;
        for (int i = 0; id && i < MAXSESS; i++)            /* open in another session: a new one */
            if (&sess[i] != s && sess[i].used && sess[i].uid == s->uid && sess[i].conv == id) id = 0;
        conv_t c;
        if (id > 0 && mem_load(s->uid, id, &c) == 0) {
            s->conv = id;
            s->ctx = c.ctx; s->digest = c.digest; c.ctx = c.digest = 0;
            system_text(s);
            for (int i = 0; i < c.nturn && s->nturn < MAXTURN; i++) { s->turn[s->nturn++] = c.turn[i]; c.turn[i].text = 0; }
            c.nturn = 0;
            mem_free(&c);
            *resumed = 1;
        } else if (m->w[1] & SIA_RESUME && m->w[2]) {
            forget(s);
            return -ENOENT;
        } else {
            int n = mem_new(s->uid);
            s->conv = n > 0 ? n : 0;
        }
    }
    if (!s->nturn) system_text(s);
    if (warm.b && warm.uid == s->uid && warm.conv == s->conv && *resumed) {
        if (warm.nturn == s->nturn && warm.sys == strlen(s->turn[0].text)) {   /* the same conversation, */
            B->session_free(B, s->b);                  /* unchanged: its cache is still right */
            s->b = warm.b;
            warm.b = 0;
            sia_log("siad: conversation %d: the model's cache is still warm\n", s->conv);
        }
        drop_warm();
    }
    *conv_out = s->conv;
    if (memory_on) sia_log("siad: uid %d: conversation %d %s, %d turns%s, system text %lu bytes\n", s->uid, s->conv,
                           *resumed ? "resumed" : "new", s->nturn - 1, s->digest ? " + summary" : "",
                           strlen(s->turn[0].text));
    return s - sess + 1;
}

/* SIA_FACTS for the caller's uid. */
static long facts(msg_t *m, const char *req, char *out, size_t cap, size_t *olen)
{
    char *f = mem_facts(m->uid);
    size_t n = f ? strlen(f) : 0;
    long r = 0;
    switch (m->w[1]) {
    case SIA_F_GET:
        *olen = n < cap ? n : cap;
        if (f) memcpy(out, f, *olen);
        break;
    case SIA_F_ADD: {
        size_t l = strlen(req);
        while (l && (req[l - 1] == '\n' || req[l - 1] == ' ')) l--;
        if (!l || l > 200 || has_nl(req, l)) { r = -EINVAL; break; }
        if (n + l + 1 > FACTS_MAX) { r = -ENOSPC; break; }
        char *t = malloc(n + l + 2);
        if (!t) { r = -ENOMEM; break; }
        memcpy(t, f ? f : "", n);
        memcpy(t + n, req, l);
        t[n + l] = '\n'; t[n + l + 1] = 0;
        r = mem_facts_put(m->uid, t);
        free(t);
        break;
    }
    case SIA_F_DEL: {
        long line = (long)m->w[2], k = 1;
        char *t = malloc(n + 1), *o = t;
        if (!t) { r = -ENOMEM; break; }
        r = -ENOENT;
        for (char *p = f; p && *p; k++) {
            char *e = strchr(p, '\n');
            size_t l = e ? (size_t)(e - p + 1) : strlen(p);
            if (k == line) r = 0;
            else { memcpy(o, p, l); o += l; }
            p += l;
        }
        *o = 0;
        if (!r) r = mem_facts_put(m->uid, t);
        free(t);
        break;
    }
    case SIA_F_CLEAR:
        r = mem_facts_put(m->uid, "");
        break;
    case SIA_F_OFFERED:                  /* the offers of this client's sessions, then forgotten */
        for (int i = 0; i < MAXSESS; i++)
            if (sess[i].used && sess[i].pid == m->pid) {
                size_t l = strlen(offered[i]);
                if (*olen + l < cap) { memcpy(out + *olen, offered[i], l); *olen += l; }
                offered[i][0] = 0;
            }
        break;
    default:
        r = -EINVAL;
    }
    free(f);
    if (r == 0 && warm.uid == (int)m->uid) drop_warm();
    /* the sessions of this user see the new facts at their next answer */
    if (m->w[1] != SIA_F_GET && m->w[1] != SIA_F_OFFERED && r == 0)
        for (int i = 0; i < MAXSESS; i++)
            if (sess[i].used && sess[i].uid == (int)m->uid) { system_text(&sess[i]); renew(&sess[i]); }
    return r;
}

static int list_tokens(const conv_t *c)  /* (an estimate: 4 bytes a token) */
{
    size_t n = c->digest ? strlen(c->digest) : 0;
    for (int i = 0; i < c->nturn; i++) n += strlen(c->turn[i].text);
    return (int)(n / 4);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    load_conf();
    B = conf_is("backend", "remote") ? &sia_remote : &sia_local;
    long e = B->init(B, conf);
    ready = e == 0;
    if (!ready) sia_log("siad: the %s backend is not ready (error %ld): %s\n", B->name, -e,
                        B == &sia_local ? "no model? put a .gguf file in /models, or: sia config backend=remote"
                                        : "check the url and key: sia config url=...");
    memory_on = !conf_is("memory", "off");
    compact_at = (int)conf_num("compact_at", 60);
    keep_turns = (int)conf_num("keep_turns", 4);
    if (compact_at < 10 || compact_at > 95) compact_at = 60;
    if (keep_turns < 2) keep_turns = 2;
    mem_setup((int)conf_num("max_convs", 32), (int)conf_num("max_log_kb", 256));
    if (ready) {
        sia_info_t in = { 0 };
        B->info(B, in.model, sizeof in.model, &in.mem, &in.ctx, &in.tok_s_x100);
        ctx_tokens = in.ctx;
    }
    if (ctx_tokens <= 0) ctx_tokens = (int)conf_num("remote_ctx", 8192);
    long port = port_create("sia");
    static char req[SIA_MAX + 1], piece[SIA_MAX];
    msg_t m = { .rbuf = req, .rlen = SIA_MAX };
    long from = ipc_recv(port, &m);
    uint64_t asked = 0, seen = 0;         /* requests served; at init's last question */
    for (;;) {
        if (from <= 0) { m = (msg_t){ .rbuf = req, .rlen = SIA_MAX }; from = ipc_recv(port, &m); continue; }
        req[m.rlen < SIA_MAX ? m.rlen : SIA_MAX] = 0;
        asked++;
        long r = 0, w1 = 0, w2 = 0;
        sess_t *s = 0, *later = 0;        /* later: compact this one after replying */
        const void *out = 0;
        size_t olen = 0;
        int end_after = 0;
        static sia_info_t info;
        switch (m.w[0]) {
        case SIA_OPEN:
            if (!ready) { r = B == &sia_local ? -ENOENT : -EIO; break; }
            r = open_session(&m, &w1, &w2);
            break;
        case SIA_ASK:
            if (!(s = get(&m))) { r = -EINVAL; break; }
            if (s->answering) { B->stop(B, s->b); while (next_piece(s, piece, sizeof piece) > 0) ; }
            if (s->conv && s->nturn == 1) {           /* the first question: the title */
                conv_t c;
                int fresh = mem_load(s->uid, s->conv, &c) == 0 && !c.title;
                if (fresh) mem_add(s->uid, s->conv, 'T', req, strlen(req) < 60 ? strlen(req) : 60);
                mem_free(&c);
            }
            r = add_turn(s, SIA_USER, req, strlen(req));
            if (!r) s->pending = 1;
            break;
        case SIA_NEXT:
            if (!(s = get(&m))) { r = -EINVAL; break; }
            r = next_piece(s, piece, m.w[2] && m.w[2] < sizeof piece ? (int)m.w[2] : (int)sizeof piece);
            if (r > 0) { out = piece; olen = r; }
            if (r == 0 && s->compact_due) later = s;
            break;
        case SIA_STOP:
            if (!(s = get(&m))) { r = -EINVAL; break; }
            if (s->answering) B->stop(B, s->b);
            break;
        case SIA_CLOSE:
            if (!(s = get(&m))) { r = -EINVAL; break; }
            forget(s);
            break;
        case SIA_CONTEXT:
            if (!(s = get(&m))) { r = -EINVAL; break; }
            if (s->ctx && !strcmp(s->ctx, req)) break;  /* (the same again: nothing to do) */
            free(s->ctx);
            s->ctx = *req ? dup(req, strlen(req)) : 0;
            if (s->conv) mem_add(s->uid, s->conv, 'C', req, strlen(req));
            system_text(s);
            renew(s);
            break;
        case SIA_LIST:
            olen = memory_on ? (size_t)mem_list(m.uid, piece, sizeof piece, list_tokens) : 0;
            out = piece;
            break;
        case SIA_FORGET:
            for (int i = 0; i < MAXSESS; i++)          /* open conversations stop being saved */
                if (sess[i].used && sess[i].uid == (int)m.uid && (!m.w[1] || sess[i].conv == (int)m.w[1])) sess[i].conv = 0;
            if (warm.uid == (int)m.uid && (!m.w[1] || warm.conv == (int)m.w[1])) drop_warm();
            r = mem_forget(m.uid, (int)m.w[1]);
            break;
        case SIA_FACTS:
            r = facts(&m, req, piece, sizeof piece, &olen);
            out = piece;
            break;
        case SIA_INFO: {
            memset(&info, 0, sizeof info);
            info.backend = B == &sia_local ? SIA_LOCAL : SIA_REMOTE;
            info.ready = ready;
            for (int i = 0; i < MAXSESS; i++) info.sessions += sess[i].used;
            if (ready) B->info(B, info.model, sizeof info.model, &info.mem, &info.ctx, &info.tok_s_x100);
            out = &info; olen = sizeof info;
            break;
        }
        case SIA_CONFIG:
            if (m.uid) { r = -EPERM; break; }
            r = configure(req);
            end_after = r == 0;           /* init starts us again, with the new settings */
            break;
        case SVC_MAYSTOP: {               /* started on demand: unused for a while, we end and
                                             the model's memory goes back (conversations are
                                             on the disk: a client resumes them) */
            int busy = 0;
            asked--;                      /* (init's question is not a use) */
            for (int i = 0; i < MAXSESS; i++) busy |= sess[i].pending | sess[i].answering | sess[i].compact_due;
            if ((r = maystop_answer(&m, asked, &seen, busy)) == 0) {
                reply_val(from, 0);
                sia_log("siad: unused: stopping\n");
                return 0;
            }
            break;
        }
        default:
            r = -ENOSYS;
        }
        if (end_after) { msg_t a = { .w = { 0 } }; ipc_reply(from, &a); sia_log("siad: new settings, restarting\n"); return 0; }
        m = (msg_t){ .w = { r, w1, w2 }, .sbuf = out, .slen = olen, .rbuf = req, .rlen = SIA_MAX };
        if (later) {                      /* answer first, then compact */
            ipc_reply(from, &m);
            compact(later);
            from = 0;
            continue;
        }
        from = ipc_reply_recv(from, port, &m);
    }
}
