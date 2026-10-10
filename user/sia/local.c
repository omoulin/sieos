/*
 * local.c - sia's local backend: our own language-model engine (llm/,
 * docs/llm.md) running on this machine's processors.
 *
 * The engine asks its environment for memory, file reads and threads:
 *   - memory: malloc, 64-byte aligned (the weights stay in this process's
 *     heap; the kernel's quota for sia, set by init, caps it);
 *   - the model file: read from SieFS in large requests (128 KiB each);
 *   - threads: a pool, one thread per processor. Between answers the
 *     workers sleep in ipc_recv on their own port (no CPU used); during an
 *     answer they spin briefly between two pieces of work, so the next one
 *     needs no wake-up message.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "backend.h"
#include "../../llm/llm.h"

/* ---- memory: 64-byte aligned blocks; the original pointer just before */
static void *a_alloc(void *c, size_t n)
{
    (void)c;
    char *m = malloc(n + 64 + sizeof(void *));
    if (!m) return 0;
    char *p = (char *)(((uintptr_t)m + sizeof(void *) + 63) & ~63UL);
    ((void **)p)[-1] = m;
    return p;
}
static void a_free(void *c, void *p) { (void)c; if (p) free(((void **)p)[-1]); }

/* ---- the model file */
static long model_fd = -1;
static int f_read(void *c, void *buf, uint64_t off, size_t n)
{
    (void)c;
    for (size_t done = 0; done < n; ) {
        size_t k = n - done < FS_MAX ? n - done : FS_MAX;
        long r = fs_read(model_fd, off + done, (char *)buf + done, k);
        if (r <= 0) return -1;
        done += r;
    }
    return 0;
}
static uint64_t now(void *c) { (void)c; return sys_clock(); }

/* ---- the thread pool. job = generation number of the current job; each
 * worker runs its part, then counts itself done. A worker that has
 * waited "long enough" (SPIN_NS) without a new job goes to sleep in
 * ipc_recv; the caller wakes sleepers with a message (any reply will do). */
#define MAXW 64
#define SPIN_NS 200000                   /* 0.2 ms */
static struct {
    int n;                               /* threads, the caller included */
    long port[MAXW];
    volatile int sleeping[MAXW];
    volatile uint32_t job, done;
    void (*fn)(void *, int, int);
    void *arg;
} P;

static void worker(void *a)
{
    int i = (int)(intptr_t)a;
    uint32_t seen = 0;
    for (;;) {
        uint64_t t0 = sys_clock();
        while (__atomic_load_n(&P.job, __ATOMIC_ACQUIRE) == seen)
            if (sys_clock() - t0 > SPIN_NS) {        /* idle: sleep until woken */
                __atomic_store_n(&P.sleeping[i], 1, __ATOMIC_SEQ_CST);
                if (__atomic_load_n(&P.job, __ATOMIC_SEQ_CST) == seen) {
                    msg_t m = { 0 };
                    long t = ipc_recv(P.port[i], &m);
                    if (t > 0) reply_val(t, 0);
                }
                __atomic_store_n(&P.sleeping[i], 0, __ATOMIC_SEQ_CST);
                t0 = sys_clock();
            }
        seen = __atomic_load_n(&P.job, __ATOMIC_ACQUIRE);
        P.fn(P.arg, i, P.n);
        __atomic_fetch_add(&P.done, 1, __ATOMIC_ACQ_REL);
    }
}

static void p_parallel(void *c, void (*fn)(void *, int, int), void *arg, int n)
{
    (void)c;
    P.fn = fn;
    P.arg = arg;
    __atomic_store_n(&P.done, 0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&P.job, 1, __ATOMIC_SEQ_CST);
    for (int i = 1; i < n; i++)
        if (__atomic_load_n(&P.sleeping[i], __ATOMIC_SEQ_CST)) { msg_t m = { 0 }; ipc_call(P.port[i], &m); }
    fn(arg, 0, n);
    while (__atomic_load_n(&P.done, __ATOMIC_ACQUIRE) != (uint32_t)(n - 1)) ;
}

/* ---- the backend */
typedef struct { llm_t *m; llm_info_t in; llm_sampler_t samp; char model[64]; uint32_t speed; } local_t;
static local_t L;
static const char *sys_text = "You are sia, the assistant built into SIEOS, a small microkernel operating "
                              "system. Answer briefly and clearly.";

static const char *conf_get(const char *conf, const char *key, char *buf, size_t size)
{
    size_t kl = strlen(key);
    for (const char *p = conf; p && *p; ) {
        const char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        if ((size_t)(e - p) > kl && !memcmp(p, key, kl) && p[kl] == '=') {
            size_t n = e - p - kl - 1 < size - 1 ? (size_t)(e - p - kl - 1) : size - 1;
            memcpy(buf, p + kl + 1, n);
            buf[n] = 0;
            return buf;
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

static int l_init(sia_backend_t *b, const char *conf)
{
    (void)b;
    char path[FS_PATH], v[32];
    if (!conf_get(conf, "model", path, sizeof path)) strlcpy(path, "/models/smollm2-1.7b-instruct-q4_k_m.gguf", sizeof path);
    siefs_stat_t st;
    if (fs_stat(path, &st, 0) < 0 || (model_fd = fs_open(path, FS_RDONLY, 0)) < 0) return -ENOENT;
    mk_info_t in;
    sys_info(&in);
    P.n = in.ncpus < MAXW ? in.ncpus : MAXW;
    if (conf_get(conf, "threads", v, sizeof v) && strnum(v) > 0 && strnum(v) <= MAXW) P.n = strnum(v);
    for (int i = 1; i < P.n; i++) {
        P.port[i] = port_create(0);
        thread_start(worker, (void *)(intptr_t)i, 32768);
    }
    llm_env_t env = { .alloc = a_alloc, .free = a_free, .read = f_read, .now_ns = now,
                      .parallel = P.n > 1 ? p_parallel : 0, .threads = P.n,
                      .budget = 4096ULL << 20, .ctx_len = 2048 };
    env.cpu_dotprod = (in.hwcap & HWCAP_DOTPROD) != 0;   /* AArch64: Pi 5 yes, Pi 4 no (the kernel knows) */
    if (conf_get(conf, "ctx", v, sizeof v) && strnum(v) >= 256) env.ctx_len = strnum(v);
    char err[128];
    uint64_t t0 = sys_clock();
    int r = llm_open(&L.m, &env, st.size, err, sizeof err);
    if (r) { sia_log("siad: %s: %s\n", path, err); return -EINVAL; }
    llm_info(L.m, &L.in);
    const char *base = path;
    for (const char *q = path; *q; q++) if (*q == '/') base = q + 1;
    strlcpy(L.model, base, sizeof L.model);
    llm_default_sampler(&L.samp);
    L.samp.max_tokens = 512;
    sia_log("siad: %s loaded in %lu ms: %lu MiB of weights, %lu MiB per conversation, %d threads, %s kernels\n",
           L.model, (sys_clock() - t0) / 1000000, L.in.mem_weights >> 20, L.in.mem_kv_per_session >> 20,
           P.n, llm_kernel_name(L.in.kernel));
    return 0;
}

static void *l_new(sia_backend_t *b) { (void)b; int e; return llm_session_new(L.m, &e); }
static void l_free(sia_backend_t *b, void *s) { (void)b; llm_session_free(s); }

/* The engine keeps the conversation in its cache: only the new message is
 * read. When the cache is empty but the conversation is not (a new session
 * on a saved conversation: after a restart, or after compaction), the past
 * exchanges are read first, without answering them (llm_feed). */
static int l_begin(sia_backend_t *b, void *s, const sia_turn_t *t, int n)
{
    (void)b;
    if (n < 1 || t[n - 1].role != SIA_USER) return -EINVAL;
    if (llm_session_tokens(s)) return llm_ask(s, 0, t[n - 1].text) < 0 ? -EIO : 0;
    const char *sys = t[0].role == SIA_SYSTEM ? t[0].text : sys_text;
    int i = t[0].role == SIA_SYSTEM;
    uint64_t t0 = sys_clock();
    int fed = 0;
    for (; i + 1 < n - 1; i += 2) {      /* user, assistant pairs before the new message */
        if (t[i].role != SIA_USER || t[i + 1].role != SIA_ASSISTANT) continue;
        if (llm_feed(s, sys, t[i].text, t[i + 1].text) < 0) { llm_session_reset(s); fed = -1; break; }
        sys = 0;
        fed++;
    }
    if (fed < 0) sys = t[0].role == SIA_SYSTEM ? t[0].text : sys_text;   /* (too long: the new message alone) */
    if (fed > 0) sia_log("siad: resumed %d exchanges: %d tokens read in %lu ms\n", fed,
                         llm_session_tokens(s), (sys_clock() - t0) / 1000000);
    return llm_ask(s, sys, t[n - 1].text) < 0 ? -EIO : 0;
}

static int l_tokens(sia_backend_t *b, const char *text)
{
    (void)b;
    static int32_t tok[4096];
    size_t n = strlen(text), k = 0;
    int total = 0;
    while (k < n) {                      /* in slices, so any length counts */
        size_t c = n - k > 4096 ? 4096 : n - k;   /* (at most one token per byte) */
        while (c < n - k && c && (text[k + c] & 0xC0) == 0x80) c--;   /* (not inside a character) */
        int r = llm_tokenize(L.m, text + k, c, tok, 4096, 0);
        total += r > 0 ? r : (int)(c / 3);
        k += c;
    }
    return total;
}

static int l_next(sia_backend_t *b, void *s, char *buf, int cap)
{
    (void)b;
    for (;;) {
        int r = llm_next(s, &L.samp, buf, cap);
        if (r < 0) {                     /* the answer is complete: note its speed */
            llm_stats_t st;
            llm_stats(s, &st);
            if (st.gen_ns) L.speed = (uint32_t)(st.gen_tokens * 100000000000ULL / st.gen_ns);
            return 0;
        }
        if (r > 0) return r;             /* (0: a piece of a character, nothing to show yet) */
    }
}

static void l_stop(sia_backend_t *b, void *s) { (void)b; llm_stop(s); }

static void l_info(sia_backend_t *b, char *model, int cap, uint64_t *mem, int *ctx, uint32_t *speed)
{
    (void)b;
    strlcpy(model, L.model, cap);
    *mem = L.in.mem_weights + L.in.mem_scratch;
    *ctx = L.in.ctx;
    *speed = L.speed;
}

sia_backend_t sia_local = { "local", l_init, l_new, l_free, l_begin, l_next, l_stop, l_info, 0, l_tokens };
