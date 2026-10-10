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
 * Prints "ktest NAME: ok" or "... FAIL"; exit status 0 if all went well.
 * Built twice for the tests: by gcc (ktest), and by sicc (ktest-sicc,
 * without the AVX part: sicc does not encode AVX instructions yet). On
 * arm64 both are built by sicc, and there is no AVX part.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

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

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    int n = argc > 2 ? (int)strnum(argv[2]) : 16, fails;
    if (n < 1 || n > 64) n = 16;
    if (!strcmp(what, "quota-child")) return quota_child(0);
    if (!strcmp(what, "quota-grand")) return quota_child(1);
    if (!strcmp(what, "fpu")) fails = test_fpu(n);
#ifndef __SICC__
    else if (!strcmp(what, "avx")) fails = test_avx(n);
#endif
    else if (!strcmp(what, "tls")) fails = test_tls(n);
    else if (!strcmp(what, "quota")) fails = test_quota();
    else if (!strcmp(what, "ipc")) fails = test_ipc();
    else { printf("usage: ktest fpu|avx|tls|quota|ipc [threads]\n"); return 2; }
    printf("ktest %s: %s\n", what, fails ? "FAIL" : "ok");
    return fails != 0;
}
