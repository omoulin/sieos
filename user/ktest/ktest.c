/*
 * ktest - Checks of the kernel's thread services that programs rely on:
 *   ktest fpu [threads]   floating point: many threads compute at once
 *                         (more than CPUs, so they are preempted mid-
 *                         computation); each result must equal the same
 *                         computation done again alone, bit for bit
 *   ktest avx [threads]   the 16 AVX registers hold a pattern through a
 *                         long busy loop (preempted): nothing may change them
 *   ktest tls [threads]   thread-local variables: each thread its own copy
 *   ktest quota           a child limited to 16 MiB gets about that much,
 *                         and cannot give its own child more
 *   ktest ipc             message round trip between two threads: neither
 *                         uses floating point, both do, both use AVX (what
 *                         saving and restoring their registers costs)
 *   ktest ipcmany [n]     n client threads (default 40) calling one server
 *                         thread at once, on every CPU: the time per call
 *                         over all of them (how the kernel's locks scale)
 *   ktest place           P-cores and E-cores (booted with pcores=N where
 *                         all cores are alike): normal threads run on
 *                         P-cores, background ones on E-cores; more work
 *                         than cores of one kind uses the other kind
 *   ktest sig             signals: caught, ignored, default; waits they
 *                         interrupt (sleep, receive, lookup, wait, call: its
 *                         late reply dropped); a process group; registers
 *                         (floating point, AVX) and IPC through a storm of
 *                         them; the thread that set the function takes them
 *   ktest sigwait         (for tools/sigtest.py) SIGINT caught in a busy loop,
 *                         a sleep and a console read, then a clean end
 *   ktest hang spin|sleep|read|net   wait forever (for Ctrl-C)
 * Prints "ktest NAME: ok" or "... FAIL"; exit status 0 if all went well.
 * Built twice for the tests: by gcc (ktest), and by sicc (ktest-sicc,
 * without the AVX part: sicc does not encode AVX instructions yet). On
 * arm64 both are built by sicc, and there is no AVX part.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "net.h"

static volatile int done, bad;

static void wait_all(int n) { while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) < n) sys_sleep(2000000); }

/* ---- floating point */
static double series(int k)
{
    double x = 1.0 + k * 1e-3, y = 0.5;
    for (int i = 0; i < 25000000; i++) {   /* ~40 ms: preempted several times */
        x = x * 1.0000001 + 1e-9 * (i & 7);
        y = y * 0.999999 + x * 1e-7;
    }
    return x + y;
}
static double fpu_res[64];
static void fpu_thread(void *a)
{
    int k = (int)(intptr_t)a;
    fpu_res[k] = series(k);
    __atomic_fetch_add(&done, 1, __ATOMIC_RELEASE);
}
static int test_fpu(int n)
{
    for (int k = 0; k < n; k++) thread_start(fpu_thread, (void *)(intptr_t)k, 16384);
    wait_all(n);
    int fails = 0;
    for (int k = 0; k < n; k++) {
        double again = series(k);
        if (memcmp(&again, &fpu_res[k], sizeof again)) fails++;
    }
    return fails;
}

/* ---- AVX: ymm0-15 loaded with a pattern, a busy loop long enough to be
 * preempted many times, then stored and compared. */
#ifndef __SICC__
#define LD(i) "vmovdqu " #i "*32(%0), %%ymm" #i "\n"
#define ST(i) "vmovdqu %%ymm" #i ", " #i "*32(%1)\n"
static int avx_hold(uint32_t seed)
{
    uint32_t in[128], out[128];
    for (int i = 0; i < 128; i++) in[i] = seed * 2654435761u + i * 40503u;
    asm volatile(LD(0) LD(1) LD(2) LD(3) LD(4) LD(5) LD(6) LD(7)
                 LD(8) LD(9) LD(10) LD(11) LD(12) LD(13) LD(14) LD(15)
                 "mov $400000000, %%rcx\n1: dec %%rcx\n jnz 1b\n"
                 ST(0) ST(1) ST(2) ST(3) ST(4) ST(5) ST(6) ST(7)
                 ST(8) ST(9) ST(10) ST(11) ST(12) ST(13) ST(14) ST(15)
                 "vzeroupper\n"
                 : : "r"(in), "r"(out)
                 : "rcx", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                   "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    return memcmp(in, out, sizeof in) != 0;
}
static void avx_thread(void *a)
{
    if (avx_hold((uint32_t)(intptr_t)a + 1)) __atomic_fetch_add(&bad, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&done, 1, __ATOMIC_RELEASE);
}
static int test_avx(int n)
{
    uint32_t r[4];
    asm volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(1), "c"(0));
    if (!(r[2] >> 28 & 1)) { printf("ktest avx: no AVX on this CPU, skipped\n"); return 0; }
    for (int k = 0; k < n; k++) thread_start(avx_thread, (void *)(intptr_t)k, 16384);
    wait_all(n);
    return bad;
}
#endif

/* ---- thread-local variables */
static _Thread_local int tv = 42;
static _Thread_local char tarr[100];
static void tls_thread(void *a)
{
    int id = (int)(intptr_t)a + 1;
    if (tv != 42) __atomic_fetch_add(&bad, 1, __ATOMIC_RELAXED);   /* the initial value */
    tv = id;
    memset(tarr, id, sizeof tarr);
    for (int round = 0; round < 20; round++) {
        sys_sleep(1000000);
        for (volatile int i = 0; i < 200000; i++) ;
        int ok = tv == id;
        for (size_t i = 0; i < sizeof tarr; i++) ok &= tarr[i] == (char)id;
        if (!ok) { __atomic_fetch_add(&bad, 1, __ATOMIC_RELAXED); break; }
    }
    __atomic_fetch_add(&done, 1, __ATOMIC_RELEASE);
}
static int test_tls(int n)
{
    for (int k = 0; k < n; k++) thread_start(tls_thread, (void *)(intptr_t)k, 16384);
    wait_all(n);
    return bad + (tv != 42);                 /* the first thread's copy was never touched */
}

/* ---- quota: MiB blocks until refused */
static int grab(void)
{
    int n = 0;
    while (malloc(1 << 20)) n++;
    return n;
}
static int test_quota(void)
{
    mk_spawn_t o = { .flags = SPAWN_QUOTA, .quota = 16 << 20 };
    long pid = spawn_file("/bin/ktest", "ktest quota-child", -1, -1, &o);
    int st = -1;
    if (pid < 0 || sys_wait(pid, &st, 0) < 0) return 1;
    int child = st / 100, grand = st % 100;
    printf("ktest quota: a child limited to 16 MiB got %d MiB; its child, asking for 64 MiB, got %d MiB\n", child, grand);
    return !(child >= 12 && child < 16 && grand >= 12 && grand < 16);
}
static int quota_child(int grandchild)
{
    int g = 0;
    if (grandchild) return grab();
    {                       /* ask more for our own child: it must stay within ours */
        mk_spawn_t o = { .flags = SPAWN_QUOTA, .quota = 64 << 20 };
        long pid = spawn_file("/bin/ktest", "ktest quota-grand", -1, -1, &o);
        int st = 0;
        if (pid > 0 && sys_wait(pid, &st, 0) > 0) g = st;
    }
    return grab() * 100 + g;
}

/* ---- IPC round trip, with and without floating-point state to switch */
static long ipc_port;
static int use_fp;                           /* 0 none, 1 x87/SSE (arm64: FP/SIMD), 2 AVX too */
static uint64_t ipc_ns;
static void touch_fp(void)
{
    if (!use_fp) return;
    volatile double d = 1.0;
    d = d * 1.5;
#ifndef __SICC__
    if (use_fp > 1) asm volatile("vpcmpeqd %%ymm1, %%ymm1, %%ymm1" : : : "xmm1");   /* upper halves in use */
#endif
}
static void ipc_server(void *a)
{
    (void)a;
    touch_fp();
    msg_t m = { 0 };
    long t = ipc_recv(ipc_port, &m);
    for (;;) {
        m = (msg_t){ .w = { m.w[0] + 1 } };
        t = ipc_reply_recv(t, ipc_port, &m);
    }
}
static void ipc_client(void *a)
{
    (void)a;
    touch_fp();
    enum { N = 200000 };
    uint64_t t0 = sys_clock();
    for (int i = 0; i < N; i++) {
        msg_t m = { .w = { i } };
        if (ipc_call(ipc_port, &m) < 0 || m.w[0] != (uint64_t)i + 1) __atomic_fetch_add(&bad, 1, __ATOMIC_RELAXED);
    }
    ipc_ns = (sys_clock() - t0) / N;
    __atomic_fetch_add(&done, 1, __ATOMIC_RELEASE);
}
static int test_ipc(void)
{
    uint64_t ns[3];
    for (use_fp = 0; use_fp < 3; use_fp++) {
#ifdef __SICC__
        if (use_fp == 2) { ns[2] = 0; break; }
#endif
        ipc_port = port_create(0);
        thread_start(ipc_server, 0, 16384);
        done = 0;
        thread_start(ipc_client, 0, 16384);
        wait_all(1);
        ns[use_fp] = ipc_ns;
    }
    #ifdef __aarch64__
    printf("ktest ipc: round trip %lu ns (no floating point), %lu ns (FP/SIMD)\n", ns[0], ns[1]);
#else
    printf("ktest ipc: round trip %lu ns (no floating point), %lu ns (x87/SSE), %lu ns (AVX)\n", ns[0], ns[1], ns[2]);
#endif
    return bad;
}

/* ---- Many clients, one server: every client calls as fast as it can;
 * the time from the first call to the last answer, per call. */
static volatile uint64_t many_end;
static void many_client(void *a)
{
    enum { N = 20000 };
    (void)a;
    for (int i = 0; i < N; i++) {
        msg_t m = { .w = { i } };
        if (ipc_call(ipc_port, &m) < 0 || m.w[0] != (uint64_t)i + 1) __atomic_fetch_add(&bad, 1, __ATOMIC_RELAXED);
    }
    uint64_t t = sys_clock();
    if (__atomic_add_fetch(&done, 1, __ATOMIC_ACQ_REL) == 1000) many_end = t;   /* (main sets done to 1000 - n) */
}
static int test_ipcmany(int n)
{
    mk_info_t in;
    sys_info(&in);
    use_fp = 0;
    ipc_port = port_create(0);
    thread_start(ipc_server, 0, 16384);
    done = 1000 - n;
    uint64_t t0 = sys_clock();
    for (int k = 0; k < n; k++) thread_start(many_client, 0, 16384);
    wait_all(1000);
    uint64_t ns = (many_end - t0) / (20000UL * n);
    printf("ktest ipcmany: %d clients, 1 server, %d CPUs: %lu ns per call\n", n, in.ncpus, ns);
    return bad;
}

/* ---- P-cores and E-cores (boot with pcores=N where all cores are alike):
 * busy threads, normal or background, each counting where it runs. */
static volatile int on_e[64];
static int place_ms[64], place_from;     /* each thread's time; samples count from place_from ms */
static int place_wait;                   /* normal threads sleep this long (ms) first */
/* Busy for ms, counting where it runs (from `from` ms on): -> the
 * percentage of that time on E-cores. */
static int busy_place(int bg, int ms, int from)
{
    if (bg) sys_place(0, PLACE_BACKGROUND);
    else if (place_wait) sys_sleep(place_wait * 1000000UL);
    int e = 0, p = 0;
    for (uint64_t t0 = sys_clock(), t; (t = sys_clock() - t0) < ms * 1000000UL; ) {
        for (volatile int i = 0; i < 20000; i++) ;
        mk_info_t in;
        sys_info(&in);
        if (t < from * 1000000UL) continue;
        if (in.cpu_e) e++; else p++;
    }
    return e + p ? e * 100 / (e + p) : 0;
}
static void place_thread(void *a)
{
    int k = (int)(intptr_t)a & 63, bg = (int)(intptr_t)a >> 6;
    on_e[k] = busy_place(bg, place_ms[k], place_from);
    __atomic_fetch_add(&done, 1, __ATOMIC_RELEASE);
}
/* n threads: the first nn normal, the others background, 300 ms each
 * unless set; -> the average percentage on E-cores of each kind (*e_normal
 * over the normal ones from first on, *e_bg). */
static void place_run(int n, int nn, int first, int *e_normal, int *e_bg)
{
    done = 0;
    for (int k = 0; k < n; k++) {
        if (!place_ms[k]) place_ms[k] = 300;
        thread_start(place_thread, (void *)(intptr_t)(k | (k >= nn) << 6), 16384);
    }
    wait_all(n);
    int sn = 0, sb = 0;
    for (int k = 0; k < n; k++) { if (k < nn) sn += k >= first ? on_e[k] : 0; else sb += on_e[k]; place_ms[k] = 0; }
    *e_normal = nn > first ? sn / (nn - first) : 0;
    *e_bg = n > nn ? sb / (n - nn) : 0;
    place_from = 0;
}
static int test_place(void)
{
    mk_info_t in;
    sys_info(&in);
    if (!in.ecores) { printf("ktest place: all cores alike, skipped (boot with pcores=N to test)\n"); return 0; }
    if (in.ncpus > 63) { printf("ktest place: more than 63 CPUs, skipped\n"); return 0; }
    int np = in.ncpus - in.ecores, ne = in.ecores, fails = 0, a, b;
    printf("ktest place: %d CPUs, %d P-cores, %d E-cores\n", in.ncpus, np, ne);
    place_run(np + ne, np, 0, &a, &b);       /* one per CPU: each kind on its own */
    printf("ktest place: %d normal and %d background threads: normal %d%% on E-cores, background %d%%\n", np, ne, a, b);
    fails += a > 15 || b < 85;
    place_run(1, 0, 0, &a, &b);              /* alone: background still prefers E-cores */
    printf("ktest place: 1 background thread alone: %d%% on E-cores\n", b);
    fails += b < 85;
    place_run(np + ne, np + ne, 0, &a, &b);  /* more normal work than P-cores: overflows to E-cores */
    printf("ktest place: %d normal threads: %d%% on E-cores\n", np + ne, a);
    fails += a < 100 * ne / (np + ne) / 2;
    for (int k = 0; k < ne; k++) place_ms[k] = 100;   /* ... then fewer: P-cores take them back */
    place_from = 150;
    place_run(np + ne, np + ne, ne, &a, &b);
    printf("ktest place: %d of them end after 100 ms: the other %d, from 150 ms on: %d%% on E-cores\n", ne, np, a);
    fails += a > 15;
    place_run(np + ne, 0, 0, &a, &b);        /* idle P-cores take background work */
    printf("ktest place: %d background threads: %d%% on E-cores\n", np + ne, b);
    fails += b > 100 - 100 * np / (np + ne) / 2;
    place_wait = 50;                         /* ... until normal work comes: it takes a P-core back */
    place_run(np + ne + 1, 1, 0, &a, &b);
    place_wait = 0;
    printf("ktest place: then a normal thread wakes up: %d%% on E-cores\n", a);
    fails += a > 15;
    mk_spawn_t o = { .flags = SPAWN_BACKGROUND };   /* a background process (init's services) */
    long pid = spawn_file("/bin/ktest", "ktest place-child", -1, -1, &o);
    int st = -1;
    if (pid < 0 || sys_wait(pid, &st, 0) < 0) st = -1;
    printf("ktest place: a process started background: %d%% on E-cores\n", st);
    fails += st < 85;
    return fails;
}

/* SYS_LOG: a line printed is in the log; reading moves the position; the
 * oldest kept is at most LOG_KEEP bytes back. */
static int test_log(void)
{
    static char buf[1024], tail[64];
    char mark[48];
    int fails = 0, found = 0, tl = 0;
    uint64_t t = (uint64_t)sys_clock(), start = 0, pos = 0;
    int n = 0;
    memcpy(mark, "ktest log mark ", 15);
    for (int i = 0; i < 12; i++) mark[15 + i] = (char)('a' + (t >> (4 * i)) % 26);
    mark[27] = '\n'; mark[28] = 0;
    sys_debug(mark, 28);
    long r = sys_log(buf, 1, &start);                 /* from 0: moved up to the oldest kept */
    if (r != 1) { printf("ktest log: a first byte: %ld\n", r); fails++; }
    pos = start - 1;
    for (;;) {
        long k = sys_log(buf, sizeof buf, &pos);
        if (k < 0) { printf("ktest log: error %ld\n", k); fails++; break; }
        if (!k) break;
        for (long i = 0; i < k; i++) {              /* (the mark may straddle two reads) */
            if (tl == sizeof tail) { memmove(tail, tail + 1, --tl); }
            tail[tl++] = buf[i];
            if (tl >= 28 && !memcmp(tail + tl - 28, mark, 28)) found = 1;
        }
        n += (int)k;
    }
    if (!found) { printf("ktest log: the mark is not in the log (%d bytes read)\n", n); fails++; }
    if (n > LOG_KEEP) { printf("ktest log: %d bytes, more than LOG_KEEP\n", n); fails++; }
    uint64_t again = pos;
    if (sys_log(buf, sizeof buf, &again) != 0 || again != pos) { printf("ktest log: something after the end\n"); fails++; }
    return fails;
}

/* ---- Signals. A function for SIGUSR1 that computes in floating point
 * itself (the interrupted code's registers must not change), a thread that
 * sends one after a while ("kick"), or many ("storm"). */
static volatile int sig_n[NSIG], sig_stop;
static int self_pid;
static void on_sig(int s)
{
    sig_n[s]++;
    volatile double z = 1.5;
    for (int i = 0; i < 50; i++) z = z * 1.01 + 0.25;
}
static void kick_thread(void *a)
{
    sys_sleep((uint64_t)(intptr_t)a * 1000000);
    kill(self_pid, SIGUSR1);
}
static void kick(int ms) { thread_start(kick_thread, (void *)(intptr_t)ms, 16384); }
static void storm_thread(void *a)
{
    (void)a;
    while (!sig_stop) { kill(self_pid, SIGUSR1); sys_sleep(200000); }
    __atomic_fetch_add(&done, 1, __ATOMIC_RELEASE);
}
/* A server that answers its first call late (after the client was
 * interrupted: the answer must go nowhere), the second at once. */
static volatile long slow_r = 1;
static void slow_server(void *a)
{
    (void)a;
    msg_t m = { 0 };
    long t = ipc_recv(ipc_port, &m);
    sys_sleep(1500000000);
    m = (msg_t){ .w = { 111 } };
    long r = ipc_reply(t, &m);
    t = ipc_recv(ipc_port, &m);
    m = (msg_t){ .w = { 222 } };
    ipc_reply(t, &m);
    slow_r = r;
}
static void usr2_thread(void *a)        /* (the last to set a function takes the signal) */
{
    (void)a;
    signal(SIGUSR2, on_sig);
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    long r = sys_sleep(20000000000UL);
    __atomic_store_n(&done, r == -EINTR ? 2 : 3, __ATOMIC_RELEASE);
}
static int sig_check(const char *what, int ok)
{
    printf("ktest sig: %s: %s\n", what, ok ? "ok" : "FAIL");
    return !ok;
}
static int has_child(int pid)           /* a process whose parent is pid */
{
    static mk_task_t t[32];
    long n;
    int after = 0;
    do {
        n = sys_tasks(t, 32, after);
        for (long i = 0; i < n; i++) if (t[i].ppid == pid) return 1;
        if (n > 0) after = t[n - 1].pid;
    } while (n == 32);
    return 0;
}
static int test_sig(void)
{
    mk_ident_t id;
    int fails = 0, st;
    sys_ident(0, &id);
    self_pid = id.pid;
    signal(SIGUSR1, on_sig);
    raise(SIGUSR1);
    fails += sig_check("raise runs the function at once", sig_n[SIGUSR1] == 1);
    signal(SIGUSR2, SIG_IGN);
    raise(SIGUSR2);
    fails += sig_check("ignored", sig_n[SIGUSR2] == 0);
    fails += sig_check("SIGKILL cannot be caught", signal(SIGKILL, on_sig) == SIG_ERR && SYS(SYS_SIGNAL, SIGKILL, 1) == -EINVAL);
    fails += sig_check("not the supervisor", kill(1, SIGTERM) == -EPERM);

    uint64_t t0 = sys_clock();
    kick(100);
    long r = sys_sleep(20000000000UL);
    fails += sig_check("a sleep ends with -EINTR", r == -EINTR && sys_clock() - t0 < 10000000000UL);
    long p = port_create(0);
    kick(100);
    msg_t m = { 0 };
    fails += sig_check("a receive ends with -EINTR", ipc_recv(p, &m) == -EINTR);
    kick(100);
    fails += sig_check("a lookup ends with -EINTR", port_lookup("ktest-none") == -EINTR);

    long pid = spawn_file("/bin/ktest", "ktest spin", -1, -1, 0);
    kick(100);
    r = sys_wait((int)pid, &st, 0);
    fails += sig_check("a wait ends with -EINTR", r == -EINTR);
    fails += sig_check("kill -0 finds it", kill((int)pid, 0) == 0);
    kill((int)pid, SIGTERM);
    r = sys_wait((int)pid, &st, 0);
    fails += sig_check("SIGTERM ends a busy loop", r == pid && st == EXIT_SIGNAL(SIGTERM));
    fails += sig_check("then it is gone", kill((int)pid, 0) == -ESRCH);

    /* a call interrupted: the late answer goes nowhere, the next call gets its own */
    ipc_port = port_create(0);
    thread_start(slow_server, 0, 16384);
    kick(200);
    m = (msg_t){ .w = { 1 } };
    long e1 = ipc_call(ipc_port, &m);
    m = (msg_t){ .w = { 2 } };
    long e2 = ipc_call(ipc_port, &m);
    while (slow_r == 1) sys_sleep(10000000);
    fails += sig_check("a call ends with -EINTR; its late reply is dropped", e1 == -EINTR && e2 == 0 && m.w[0] == 222 && slow_r == -ESRCH);

    /* a process group: a child (its own group) and its child, ended together */
    mk_spawn_t o = { .flags = SPAWN_PGROUP };
    pid = spawn_file("/bin/ktest", "ktest sig-group", -1, -1, &o);
    for (int i = 0; i < 1000 && !has_child((int)pid); i++) sys_sleep(10000000);
    r = kill((int)-pid, SIGINT);
    sys_wait((int)pid, &st, 0);
    int gone = 0;
    for (int i = 0; i < 1000 && !(gone = kill((int)-pid, 0) == -ESRCH); i++) sys_sleep(10000000);
    fails += sig_check("SIGINT to a group ends all of it", r == 0 && st == EXIT_SIGNAL(SIGINT) && gone);

    /* floating point through signals: bit for bit */
    double ref[4], again[4];
    for (int k = 0; k < 4; k++) ref[k] = series(k);
    int before = sig_n[SIGUSR1];
    done = 0; sig_stop = 0;
    thread_start(storm_thread, 0, 16384);
    for (int k = 0; k < 4; k++) again[k] = series(k);
#if !defined(__SICC__) && defined(__x86_64__)
    uint32_t c[4];
    asm volatile("cpuid" : "=a"(c[0]), "=b"(c[1]), "=c"(c[2]), "=d"(c[3]) : "a"(1), "c"(0));
    int avx_bad = c[2] >> 28 & 1 ? avx_hold(7) : 0;
#else
    int avx_bad = 0;
#endif
    /* IPC through signals: interrupted calls are made again; every answer must be its own */
    ipc_port = port_create(0);
    use_fp = 0;
    thread_start(ipc_server, 0, 16384);
    int calls = 0, intr = 0, wrong = 0;
    for (int i = 0; i < 100000; i++) {
        m = (msg_t){ .w = { i } };
        long e;
        while ((e = ipc_call(ipc_port, &m)) == -EINTR) { intr++; m = (msg_t){ .w = { i } }; }
        calls++;
        wrong += e < 0 || m.w[0] != (uint64_t)i + 1;
    }
    sig_stop = 1;
    wait_all(1);
    int got = sig_n[SIGUSR1] - before;
    printf("ktest sig: %d signals taken during the computation and %d calls (%d calls interrupted)\n", got, calls, intr);
    fails += sig_check("floating point unchanged by signals", !memcmp(ref, again, sizeof ref) && !avx_bad && got > 3);
    fails += sig_check("no answer to the wrong call", !wrong);

    /* the thread that sets the function takes the signal */
    done = 0;
    thread_start(usr2_thread, 0, 16384);
    while (done != 1) sys_sleep(1000000);
    raise(SIGUSR2);
    while (done == 1) sys_sleep(1000000);
    fails += sig_check("the signal goes to the thread that set the function", done == 2 && sig_n[SIGUSR2] == 1);
    return fails;
}

static int hang(const char *how);
/* ktest sig-group: a child of its own (same group), then busy */
static int sig_group(void)
{
    spawn_file("/bin/ktest", "ktest spin", -1, -1, 0);
    return hang("spin");
}

/* ktest sigwait: for the console tests (tools/sigtest.py), each step after
 * a Ctrl-C: SIGINT caught in a busy loop, in a sleep, in a console read;
 * then it cleans up and ends normally. */
static volatile int intr_n;
static void on_int(int s) { (void)s; intr_n++; }
static int sig_wait(void)
{
    char line[64];
    signal(SIGINT, on_int);
    printf("ktest sigwait: ready\n");
    while (!intr_n) ;
    printf("ktest sigwait: caught in a busy loop\n");
    long r = sys_sleep(600000000000UL);
    printf("ktest sigwait: caught in a sleep (%s)\n", r == -EINTR ? "interrupted" : "not interrupted");
    r = con_read(line, sizeof line);
    printf("ktest sigwait: caught in a read (%s)\n", r == -EINTR ? "interrupted" : "not interrupted");
    printf("ktest sigwait: cleaned up, %d signals\n", intr_n);
    return intr_n == 3 ? 0 : 1;
}

/* ktest hang spin|sleep|read|net: wait forever, for Ctrl-C (no function) */
static int hang(const char *how)
{
    char b[64];
    printf("ktest hang: %s\n", how);
    if (!strcmp(how, "sleep")) sys_sleep(600000000000UL);
    else if (!strcmp(how, "read")) con_read(b, sizeof b);
    else if (!strcmp(how, "net")) { long s = net_udp(0); net_recvfrom(s, b, sizeof b, 0, 0, 600000); }
    else for (;;) ;
    printf("ktest hang: not interrupted\n");
    return 1;
}

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    int n = argc > 2 ? (int)strnum(argv[2]) : 16, fails;
    if (n < 1 || n > 64) n = 16;
    if (!strcmp(what, "quota-child")) return quota_child(0);
    if (!strcmp(what, "quota-grand")) return quota_child(1);
    if (!strcmp(what, "place-child")) return busy_place(0, 300, 0);   /* (background: its parent said so) */
    if (!strcmp(what, "sig-group")) return sig_group();
    if (!strcmp(what, "sigwait")) return sig_wait();
    if (!strcmp(what, "spin")) return hang("spin");
    if (!strcmp(what, "hang")) return hang(argc > 2 ? argv[2] : "spin");
    if (!strcmp(what, "fpu")) fails = test_fpu(n);
#ifndef __SICC__
    else if (!strcmp(what, "avx")) fails = test_avx(n);
#endif
    else if (!strcmp(what, "tls")) fails = test_tls(n);
    else if (!strcmp(what, "quota")) fails = test_quota();
    else if (!strcmp(what, "ipc")) fails = test_ipc();
    else if (!strcmp(what, "ipcmany")) fails = test_ipcmany(argc > 2 ? n : 40);
    else if (!strcmp(what, "place")) fails = test_place();
    else if (!strcmp(what, "log")) fails = test_log();
    else if (!strcmp(what, "sig")) fails = test_sig();
    else { printf("usage: ktest fpu|avx|tls|quota|ipc|ipcmany|place|log|sig [threads]\n"); return 2; }
    printf("ktest %s: %s\n", what, fails ? "FAIL" : "ok");
    return fails != 0;
}
