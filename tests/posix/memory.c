/*
 * ptest-memory (make posix-test): memory as Linux programs use it: mmap
 * reservations that cost nothing until touched, giving pages back
 * (madvise), guard pages, mprotect, generated code run, a deep recursion
 * on the 8 MiB main stack, the main thread's stack as pthread_getattr_np
 * finds it, and a thread with a 4 MiB stack.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int fails;
static void check(const char *what, int ok)
{
    printf("ptest memory: %s: %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

static int deep(int n) { volatile char pad[1000]; pad[0] = (char)n; return n ? deep(n - 1) + pad[0] - (char)n + 1 : 0; }

static void *big_stack(void *a)
{
    (void)a;
    volatile char buf[3 << 20];                         /* 3 MiB of a 4 MiB stack */
    buf[0] = 1; buf[sizeof buf - 1] = 2;
    return (void *)(long)(buf[0] + buf[sizeof buf - 1]);
}

int main(void)
{
    char *p = mmap(0, 1UL << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    int ok = p != MAP_FAILED;
    if (ok) { p[0] = 1; p[(1UL << 32) - 1] = 2; ok = p[0] == 1 && p[12345] == 0; }
    check("4 GiB reserved, two pages touched", ok);
    ok = ok && madvise(p, 4096, MADV_DONTNEED) == 0 && p[0] == 0;
    check("madvise(MADV_DONTNEED): zero again", ok);
    check("munmap", p != MAP_FAILED && munmap(p, 1UL << 32) == 0);
    unsigned char *code = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#if defined(__x86_64__)
    static const unsigned char ret7[] = { 0xb8, 7, 0, 0, 0, 0xc3 };                 /* mov eax, 7; ret */
#else
    static const unsigned char ret7[] = { 0xe0, 0x00, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6 };   /* mov w0, #7; ret */
#endif
    memcpy(code, ret7, sizeof ret7);
    __builtin___clear_cache((char *)code, (char *)code + sizeof ret7);
    int mp = mprotect(code, 4096, PROT_READ | PROT_EXEC);
    check("code written, made executable (W^X), run", mp == 0 && ((int (*)(void))(uintptr_t)code)() == 7);
    check("a 4 MiB recursion on the main stack", deep(4000) == 4000);
    pthread_attr_t at;
    void *base = 0;
    size_t size = 0;
    ok = pthread_getattr_np(pthread_self(), &at) == 0 && pthread_attr_getstack(&at, &base, &size) == 0;
    check("the main thread's stack found", ok && size >= (1 << 20) && (char *)&at > (char *)base && (char *)&at < (char *)base + size);
    pthread_t t;
    void *r = 0;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4 << 20);
    check("a thread with a 4 MiB stack", pthread_create(&t, &at, big_stack, 0) == 0 && pthread_join(t, &r) == 0 && (long)r == 3);
    check("sysconf: page size, processors", sysconf(_SC_PAGESIZE) == 4096 && sysconf(_SC_NPROCESSORS_ONLN) >= 1);
    printf("ptest memory: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
