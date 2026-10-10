/* tls.c - thread-local storage linked by sicc's own linker, no C library: _start
 * builds the thread block (the template, then the thread control block whose first
 * word points to itself), points %fs at it, and checks the variables.
 * Prints "tls ok" and exits 0, or names the first wrong value and exits 1.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
extern char __tdata_start[], __tdata_end[], __tbss_size[];

_Thread_local int a = 0x1234;
_Thread_local long b[3] = { 7, 8, 9 };
_Thread_local char zero[40];
_Thread_local _Alignas(64) int aligned = 99;
static _Thread_local short s = -5;
long plain = 42;                         /* .bss/.data next to .tbss: must not overlap its copy */
long cleared[4];
extern _Thread_local int other;          /* in tls_other.c */
extern _Thread_local long other_zero[2];

static long sys(long n, long a1, long a2, long a3)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return r;
}

static void say(const char *m) { long n = 0; while (m[n]) n++; sys(1, 1, (long)m, n); }

static _Alignas(64) char block[4096];

static int check(void)
{
    if (a != 0x1234) { say("a\n"); return 1; }
    if (b[0] + b[1] + b[2] != 24) { say("b\n"); return 1; }
    for (int i = 0; i < 40; i++) if (zero[i]) { say("zero\n"); return 1; }
    if (aligned != 99 || ((long)&aligned & 63)) { say("aligned\n"); return 1; }
    if (s != -5) { say("s\n"); return 1; }
    if (plain != 42 || cleared[0] | cleared[1] | cleared[2] | cleared[3]) { say("plain\n"); return 1; }
    if (other != 77 || other_zero[1]) { say("other\n"); return 1; }
    other += other_zero[0] + 1;
    if (other != 78) { say("other store\n"); return 1; }
    a++; b[2] = 10; zero[39] = 1; s *= 3;
    int *p = &a;                          /* the address of a thread-local: %fs:0 + offset */
    if (*p != 0x1235 || b[2] != 10 || zero[39] != 1 || s != -15) { say("store\n"); return 1; }
    say("tls ok\n");
    return 0;
}

void _start(void)
{
    long init = __tdata_end - __tdata_start, size = init + (long)__tbss_size;
    long total = (size + 63) & ~63L;      /* the block ends where the thread pointer is */
    char *tp = block + 1024 + total;      /* (1024: room below; tp stays 64-aligned) */
    for (long i = 0; i < init; i++) tp[i - total] = __tdata_start[i];
    for (long i = init; i < total; i++) tp[i - total] = 0;
    *(char **)tp = tp;
    sys(158, 0x1002, (long)tp, 0);        /* arch_prctl(ARCH_SET_FS) */
    sys(60, check(), 0, 0);
}
