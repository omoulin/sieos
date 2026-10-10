/*
 * lib.c - Program start-up, the console client (printf and friends send
 * messages to the console server, port "console"), malloc, and threads.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

/* ---- Thread-local storage (_Thread_local). The template is in the
 * program (user.ld): __tls_start .. __tls_end holds the initial values
 * (zeros included). Each thread gets its own copy, placed by the
 * processor's convention around its "thread pointer" (SYS_SET_FS), from
 * which the compilers reach a variable at a fixed offset:
 *   x86-64: the copy just below the thread pointer (the FS base), and the
 *           word at the thread pointer holds its own address (%fs:0);
 *   arm64:  the copy 16 bytes above the thread pointer (TPIDR_EL0), past a
 *           16-byte control block (variables aligned to at most 16 bytes).
 * Programs without thread-local variables skip all this. */
extern char __tls_start[], __tls_end[];
#define TLS_SIZE ((size_t)(__tls_end - __tls_start))          /* a multiple of 64 (user.ld) */
#define TLS_NEED (TLS_SIZE ? TLS_SIZE + 64 + 63 : 0)           /* + the control word(s), + alignment */

static void tls_set(char *mem)                                 /* mem: TLS_NEED bytes */
{
    if (!TLS_SIZE) return;
    char *b = (char *)(((uintptr_t)mem + 63) & ~63UL), *tp;
#if defined(__aarch64__)
    b += 64;                                                   /* room for the control block below */
    tp = b - 16;
#else
    tp = b + TLS_SIZE;
    *(char **)tp = tp;
#endif
    memcpy(b, __tls_start, TLS_SIZE);
    SYS(SYS_SET_FS, tp, 0);
}

void __start(char *args)
{
    if (TLS_SIZE) {                         /* the first thread's copy */
        char *m = malloc(TLS_NEED);
        if (!m) sys_exit(-ENOMEM);
        tls_set(m);
    }
    char *argv[16];
    int argc = 0;
    while (*args && argc < 15) {            /* split at spaces, in place */
        while (*args == ' ') *args++ = 0;
        if (*args) argv[argc++] = args;
        while (*args && *args != ' ') args++;
    }
    argv[argc] = 0;
    sys_exit(main(argc, argv));
}

long call_named(long *port, const char *name, msg_t *m, int repeat)
{
    msg_t copy = *m;                        /* the reply overwrites m: keep the request */
    if (*port <= 0 && (*port = port_lookup(name)) < 0) return *port;
    long e = ipc_call(*port, m);
    /* The server ended (it crashed, or stopped for being unused): wait for
     * the new one (looking the name up starts an on-demand server again).
     * -ENOENT: the call never reached it, so sending it again is always
     * safe, even several times if the server keeps ending. */
    for (int tries = 0; (e == -ENOENT && tries < 8) || (e == -EPIPE && repeat && !tries); tries++) {
        if ((*port = port_lookup(name)) < 0) return *port;
        *m = copy;
        e = ipc_call(*port, m);
    }
    if (e == -EPIPE) *port = 0;             /* (not repeated: the next call finds the new server) */
    return e;
}

/* A server started on demand answers init's "may you stop?" (SVC_MAYSTOP):
 * yes (0) only if no request came since the last question (*seen equals
 * the server's request count) and it holds no client state (!busy); else
 * how long to wait before asking again. Only init (pid 1) may ask. */
long maystop_answer(const msg_t *m, uint64_t count, uint64_t *seen, int busy)
{
    if (m->pid != 1 || m->uid) return -EPERM;
    if (busy || count != *seen) { *seen = count; return m->w[1] ? (long)m->w[1] : 60000000000L; }
    return 0;
}

static long con;                            /* the console's port, found on first use */
static char con_name[16];                   /* its name: "console", or this process's terminal */
static long con_chan;                       /* ... and the channel ("atlas#3": 3), sent in w[3] */

/* Console requests are all safe to repeat: a write prints again at worst
 * (the restarted console has cleared the screen anyway), a read waits for
 * a new line. */
static long con_call(msg_t *m)
{
    if (!con_name[0]) {                     /* first use: which terminal is ours? */
        mk_ident_t id;
        strlcpy(con_name, sys_ident(0, &id) || !id.console[0] ? "console" : id.console, sizeof con_name);
        char *h = strchr(con_name, '#');
        if (h) { *h = 0; con_chan = strnum(h + 1); }
    }
    m->w[3] = con_chan;
    long e = call_named(&con, con_name, m, 1);
    return e < 0 ? e : (long)m->w[0];
}

long con_write(const void *buf, size_t n)
{
    msg_t m = { .w = { CON_WRITE }, .sbuf = buf, .slen = n };
    return con_call(&m);
}

long con_read(void *buf, size_t n)
{
    msg_t m = { .w = { CON_READ, n }, .rbuf = buf, .rlen = n };
    return con_call(&m);
}

long con_echo(int on)
{
    msg_t m = { .w = { CON_ECHO, on } };
    return con_call(&m);
}

/* printf collects its output and sends it in one message per 256 bytes. */
struct pbuf { char b[256]; size_t n; };

static void pput(void *ctx, char c)
{
    struct pbuf *p = ctx;
    p->b[p->n++] = c;
    if (p->n == sizeof p->b) { con_write(p->b, p->n); p->n = 0; }
}

int printf(const char *fmt, ...)
{
    struct pbuf p = { .n = 0 };
    va_list ap;
    va_start(ap, fmt);
    vformat(pput, &p, fmt, ap);
    va_end(ap);
    if (p.n) con_write(p.b, p.n);
    return 0;
}

/* ---- Memory: malloc and free. Free blocks are kept in address order and
 * merged with their neighbours when freed; when none is big enough, the
 * heap grows (SYS_BRK) by at least 64 KiB. When a free block of 256 KiB or
 * more ends at the top of the heap, the heap shrinks: the memory goes back
 * to the system (a login's 16 MiB for hashing a password, for example).
 * A lock makes it safe for threads (waiting threads yield the processor
 * instead of spinning). */
typedef struct blk { size_t size; struct blk *next; } blk_t;   /* size includes this header */
static blk_t *free_blocks;
static char heap_lock;
static uintptr_t top;                  /* the end of the heap */

static void lock(void)   { while (__atomic_test_and_set(&heap_lock, __ATOMIC_ACQUIRE)) sys_yield(); }
static void unlock(void) { __atomic_clear(&heap_lock, __ATOMIC_RELEASE); }

static void release(blk_t *b)          /* put b back in the list, merging neighbours */
{
    blk_t **p = &free_blocks, *prev = 0;
    while (*p && *p < b) { prev = *p; p = &(*p)->next; }
    b->next = *p;
    *p = b;
    if (b->next && (char *)b + b->size == (char *)b->next) { b->size += b->next->size; b->next = b->next->next; }
    if (prev && (char *)prev + prev->size == (char *)b) { prev->size += b->size; prev->next = b->next; }
}

void *malloc(size_t n)
{
    size_t need = (n + sizeof(blk_t) + 15) & ~15UL;
    if (!n || need < n) return 0;
    lock();
    for (;;) {
        for (blk_t **p = &free_blocks; *p; p = &(*p)->next) {
            blk_t *b = *p;
            if (b->size < need) continue;
            if (b->size - need >= 64) {    /* split: the rest stays free */
                blk_t *r = (blk_t *)((char *)b + need);
                r->size = b->size - need;
                r->next = b->next;
                *p = r;
                b->size = need;
            } else *p = b->next;
            unlock();
            return b + 1;
        }
        if (!top) top = sys_brk(0);
        size_t grow = need < 65536 ? 65536 : need;
        if (sys_brk(top + grow) < 0) { unlock(); return 0; }
        blk_t *b = (blk_t *)top;
        b->size = grow;
        top += grow;
        release(b);
    }
}

void free(void *ptr)
{
    if (!ptr) return;
    lock();
    release((blk_t *)ptr - 1);
    blk_t **p = &free_blocks;                          /* the last free block: at the top? */
    while (*p && (*p)->next) p = &(*p)->next;
    if (*p && (uintptr_t)*p + (*p)->size == top && (*p)->size >= (256 << 10)) {
        uintptr_t b = (uintptr_t)*p;
        *p = 0;
        if (sys_brk(b) == (long)b) top = b;
        else release((blk_t *)b);                      /* (cannot happen; keep it usable) */
    }
    unlock();
}

void *realloc(void *ptr, size_t n)
{
    void *q = malloc(n);
    if (q && ptr) {
        size_t old = ((blk_t *)ptr - 1)->size - sizeof(blk_t);
        memcpy(q, ptr, old < n ? old : n);
        free(ptr);
    }
    return q;
}

/* ---- Threads. The new thread starts in thread_entry with its function
 * and argument, stored at the top of its stack; its thread-local copy, if
 * the program has any, is at the bottom of the same block. When the
 * function returns, the thread ends. (Its stack is not freed: a later
 * version will let another thread collect it.) */
struct tstart { void (*fn)(void *); void *arg; char *tls; };

static void thread_entry(struct tstart *s)
{
    tls_set(s->tls);
    s->fn(s->arg);
    sys_thread_exit();
}

long thread_start(void (*fn)(void *), void *arg, size_t stack)
{
    char *mem = malloc(TLS_NEED + stack);
    if (!mem) return -ENOMEM;
    struct tstart *s = (struct tstart *)(((uintptr_t)mem + TLS_NEED + stack - sizeof *s) & ~15UL);
    s->fn = fn;
    s->arg = arg;
    s->tls = mem;
    return syscall4(SYS_THREAD_CREATE, (long)thread_entry, (long)s, (long)s, 0);
}
