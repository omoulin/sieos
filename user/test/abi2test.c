/*
 * abi2test - ABI v2 self-test (milestones 2-7).
 *
 * A freestanding static-PIE program: no libc, only the syscall
 * instruction (the only system call entry).  It checks the SysV start-up stack and
 * auxv, self-relocation at the PIE load address, the carry-flag error
 * convention, second return values, stat/dirent layouts, clocks,
 * sysinfo, brk, FPU/SSE state across context switches, and NX.
 */
#include "sieos/abi.h"

typedef unsigned long u64;

struct sc { long val, val2; int err; };

static struct sc sys6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    struct sc r;
    register long r10 __asm__("r10") = a4, r8 __asm__("r8") = a5, r9 __asm__("r9") = a6;
    unsigned char cf;
    long rdx = a3;
    __asm__ volatile("syscall; setc %2"
                     : "=a"(r.val), "+d"(rdx), "=r"(cf)
                     : "a"(nr), "D"(a1), "S"(a2), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    r.val2 = rdx;
    r.err = cf ? (int)r.val : 0;
    return r;
}
static struct sc sys0(long nr) { return sys6(nr, 0, 0, 0, 0, 0, 0); }
static struct sc sys1(long nr, long a) { return sys6(nr, a, 0, 0, 0, 0, 0); }
static struct sc sys2(long nr, long a, long b) { return sys6(nr, a, b, 0, 0, 0, 0); }
static struct sc sys3(long nr, long a, long b, long c) { return sys6(nr, a, b, c, 0, 0, 0); }
static struct sc sys4(long nr, long a, long b, long c, long d) { return sys6(nr, a, b, c, d, 0, 0); }

/* Wait for a child; *status in the wait(2) encoding (exit code << 8, or the signal). */
static long wait_child(long pid, int *status)
{
    sieos_siginfo_t si;
    struct sc r = sys6(SIEOS_SYS_waitid, SIEOS_P_PID, pid, (long)&si, SIEOS_WEXITED, 0, 0);
    if (r.err)
        return -r.err;
    if (si.si_code == SIEOS_CLD_EXITED)
        *status = (si.sieos_si_status & 0xff) << 8;
    else
        *status = si.sieos_si_status | (si.si_code == SIEOS_CLD_DUMPED ? 0x80 : 0);
    return pid;
}

/* ---------------- tiny output ---------------- */

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts_(const char *s) { sys3(SIEOS_SYS_write, 1, (long)s, slen(s)); }
static int seq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static void putnum(long v)
{
    char b[24];
    int i = 23;
    b[i] = 0;
    int neg = v < 0;
    unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    do { b[--i] = '0' + u % 10; u /= 10; } while (u);
    if (neg) b[--i] = '-';
    puts_(b + i);
}

static int passed, failed;
static void check(const char *name, int ok, long detail)
{
    puts_(ok ? "  PASS  " : "  FAIL  ");
    puts_(name);
    if (!ok) {
        puts_("  (");
        putnum(detail);
        puts_(")");
    }
    puts_("\n");
    ok ? passed++ : failed++;
}

/* ---------------- start-up: self-relocation for static-PIE ---------------- */

extern char __ehdr_start[] __attribute__((visibility("hidden")));
extern const u64 _DYNAMIC[] __attribute__((visibility("hidden")));
static long *stack0;

/* Pointers in data that must be relocated at load time. */
const char *reloc_table[] = { "alpha", "beta", "gamma" };

static void relocate(u64 base)
{
    const u64 *rela = 0;
    u64 relasz = 0;
    for (const u64 *d = _DYNAMIC; d[0]; d += 2) {
        if (d[0] == 7)            /* DT_RELA */
            rela = (const u64 *)(base + d[1]);
        else if (d[0] == 8)       /* DT_RELASZ */
            relasz = d[1];
    }
    for (u64 i = 0; rela && i < relasz / 24; i++) {
        const u64 *r = rela + 3 * i;
        if ((r[1] & 0xffffffff) == 8)        /* R_X86_64_RELATIVE */
            *(u64 *)(base + r[0]) = base + r[2];
    }
}

void cstart(long *sp);
__asm__(".text\n.global _start\n_start:\n"
        "  xor %rbp, %rbp\n"
        "  mov %rsp, %rdi\n"
        "  mov %rdx, %rsi\n"            /* the kernel must give rdx = 0 */
        "  call cstart\n"
        "  hlt\n");

static long rdx_at_entry;

static int test_main(void);

void cstart(long *sp)
{
    register long rdx __asm__("rsi");
    rdx_at_entry = rdx;
    stack0 = sp;
    /* before relocation only PC-relative code may run */
    u64 base = (u64)__ehdr_start;
    relocate(base);
    sys1(SIEOS_SYS_exit, test_main());
}

/* ---------------- tests ---------------- */

static long auxval(long type)
{
    long argc = stack0[0];
    long *p = stack0 + 1 + argc + 1;
    while (*p) p++;
    for (p++; p[0] != SIEOS_AT_NULL; p += 2)
        if (p[0] == type)
            return p[1];
    return -1;
}

static void test_startup(void)
{
    puts_("start-up\n");
    check("rsp 16-byte aligned at entry", ((u64)stack0 & 15) == 0, (u64)stack0 & 15);
    check("rdx is 0 at entry", rdx_at_entry == 0, rdx_at_entry);
    long argc = stack0[0];
    char **argv = (char **)(stack0 + 1);
    check("argc/argv on the stack", argc >= 1 && argv[argc] == 0 && slen(argv[0]) > 0, argc);
    check("AT_PAGESZ = 4096", auxval(SIEOS_AT_PAGESZ) == 4096, auxval(SIEOS_AT_PAGESZ));
    extern char _start[] __attribute__((visibility("hidden")));
    check("AT_ENTRY = our _start", auxval(SIEOS_AT_ENTRY) == (long)_start, auxval(SIEOS_AT_ENTRY));
    check("AT_PHDR points at our program headers", auxval(SIEOS_AT_PHDR) == (long)__ehdr_start + 64,
          auxval(SIEOS_AT_PHDR));
    check("loaded at the PIE base 0x400000", (u64)__ehdr_start == 0x400000, (u64)__ehdr_start);
    check("relocated pointers", seq(((const char *volatile *)reloc_table)[1], "beta"), 0);
    const char *plat = (const char *)auxval(SIEOS_AT_SUN_PLATFORM);
    check("AT_SUN_PLATFORM = i86pc", plat != (const char *)-1 && seq(plat, "i86pc"), 0);
    const char *fn = (const char *)auxval(SIEOS_AT_EXECFN);
    check("AT_EXECFN names the program", fn != (const char *)-1 && fn[0] == '/', 0);
    const unsigned char *rnd = (const unsigned char *)auxval(SIEOS_AT_RANDOM);
    int nz = 0;
    for (int i = 0; rnd != (const unsigned char *)-1 && i < 16; i++)
        nz |= rnd[i];
    check("AT_RANDOM gives 16 random bytes", nz != 0, 0);
    struct sc u = sys0(SIEOS_SYS_getuid);
    check("AT_SUN_UID/RUID match getuid", auxval(SIEOS_AT_SUN_RUID) == u.val && auxval(SIEOS_AT_SUN_UID) == u.val2,
          auxval(SIEOS_AT_SUN_UID));
    check("AT_SECURE = 0", auxval(SIEOS_AT_SECURE) == 0, auxval(SIEOS_AT_SECURE));
}

static void test_convention(void)
{
    puts_("system call convention\n");
    struct sc r = sys3(SIEOS_SYS_write, 1, (long)"  (write via syscall)\n", 22);
    check("write returns the byte count, CF clear", !r.err && r.val == 22, r.val);
    r = sys4(SIEOS_SYS_openat, SIEOS_AT_FDCWD, (long)"/no/such/file", SIEOS_O_RDONLY, 0);
    check("openat missing file: CF set, errno ENOENT", r.err == SIEOS_ENOENT, r.err);
    r = sys1(SIEOS_SYS_close, 999);
    check("close(999): EBADF", r.err == SIEOS_EBADF, r.err);
    r = sys2(SIEOS_SYS_kill, 1, 1000);
    check("kill with a bad signal: EINVAL", r.err == SIEOS_EINVAL, r.err);
    r = sys0(250);
    check("unknown call: ENOSYS", r.err == SIEOS_ENOSYS, r.err);
    r = sys0(SIEOS_SYS_getpid);
    check("getpid rv2: parent pid in rdx", !r.err && r.val > 1 && r.val2 >= 1 && r.val2 != r.val, r.val2);
    r = sys0(SIEOS_SYS_getgid);
    check("getgid rv2: effective gid in rdx", !r.err && r.val == r.val2, r.val2);
}

static void test_files(void)
{
    puts_("files\n");
    struct sieos_stat st;
    struct sc r = sys4(SIEOS_SYS_fstatat, SIEOS_AT_FDCWD, (long)"/etc/passwd", (long)&st, 0);
    check("fstatat /etc/passwd", !r.err && (st.st_mode & SIEOS_S_IFMT) == SIEOS_S_IFREG && st.st_size > 0 &&
          seq(st.st_fstype, "ext4"), r.err);
    r = sys4(SIEOS_SYS_openat, SIEOS_AT_FDCWD, (long)"/etc/passwd", SIEOS_O_RDONLY | SIEOS_O_LARGEFILE, 0);
    long fd = r.val;
    check("openat /etc/passwd", !r.err && fd >= 0, r.err);
    struct sieos_stat st2;
    r = sys4(SIEOS_SYS_fstatat, fd, 0, (long)&st2, 0);
    check("fstat (NULL path) matches", !r.err && st2.st_ino == st.st_ino && st2.st_size == st.st_size, r.err);
    char buf[16];
    r = sys3(SIEOS_SYS_read, fd, (long)buf, 5);
    check("read", !r.err && r.val == 5 && buf[0] == 'r' && buf[4] == ':', r.val);
    r = sys3(SIEOS_SYS_lseek, fd, 0, 2);
    check("lseek(SEEK_END) = size", !r.err && r.val == st.st_size, r.val);
    sys1(SIEOS_SYS_close, fd);

    r = sys4(SIEOS_SYS_openat, SIEOS_AT_FDCWD, (long)"/", SIEOS_O_RDONLY | SIEOS_O_DIRECTORY, 0);
    fd = r.val;
    static char dbuf[4096] __attribute__((aligned(8)));
    int found_etc = 0, found_bin = 0, aligned = 1;
    for (;;) {
        r = sys3(SIEOS_SYS_getdents, fd, (long)dbuf, sizeof(dbuf));
        if (r.err || r.val <= 0)
            break;
        for (long off = 0; off < r.val;) {
            struct sieos_dirent *d = (struct sieos_dirent *)(dbuf + off);
            if (d->d_reclen & 7)
                aligned = 0;
            if (seq(d->d_name, "etc")) found_etc = 1;
            if (seq(d->d_name, "bin")) found_bin = 1;
            off += d->d_reclen;
        }
    }
    sys1(SIEOS_SYS_close, fd);
    check("getdents: Solaris dirent records, 8-byte aligned", found_etc && found_bin && aligned, found_etc);
    const char *t = "/tmp/abi2test.tmp";
    r = sys4(SIEOS_SYS_openat, SIEOS_AT_FDCWD, (long)t, SIEOS_O_WRONLY | SIEOS_O_CREAT | SIEOS_O_TRUNC, 0644);
    fd = r.val;
    sys3(SIEOS_SYS_write, fd, (long)"hello", 5);
    sys1(SIEOS_SYS_close, fd);
    r = sys4(SIEOS_SYS_fstatat, SIEOS_AT_FDCWD, (long)t, (long)&st, 0);
    check("O_CREAT|O_TRUNC translated", !r.err && st.st_size == 5, st.st_size);
    r = sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)t, 0);
    check("unlinkat", !r.err, r.err);
}

static void test_time_info(void)
{
    puts_("time and system information\n");
    struct sieos_timespec ts, ts2;
    struct sc r = sys2(SIEOS_SYS_clock_gettime, SIEOS_CLOCK_REALTIME, (long)&ts);
    check("clock_gettime(REALTIME) is after 2024", !r.err && ts.tv_sec > 1704067200, ts.tv_sec);
    long h0 = sys0(SIEOS_SYS_gethrtime).val;
    struct sieos_timespec req = { 0, 50000000 };
    r = sys2(SIEOS_SYS_nanosleep, (long)&req, 0);
    long h1 = sys0(SIEOS_SYS_gethrtime).val;
    check("nanosleep 50 ms (gethrtime)", !r.err && h1 - h0 >= 40000000 && h1 - h0 < 500000000, h1 - h0);
    r = sys2(SIEOS_SYS_clock_gettime, SIEOS_CLOCK_MONOTONIC, (long)&ts2);
    check("clock_gettime(MONOTONIC)", !r.err && ts2.tv_nsec >= 0 && ts2.tv_nsec < 1000000000, ts2.tv_nsec);
    char name[32];
    r = sys3(SIEOS_SYS_sysinfo, SIEOS_SI_SYSNAME, (long)name, sizeof(name));
    check("sysinfo(SI_SYSNAME) = SIEOS", !r.err && r.val == 6 && seq(name, "SIEOS"), r.val);
    r = sys3(SIEOS_SYS_sysinfo, SIEOS_SI_ARCHITECTURE_64, (long)name, sizeof(name));
    check("sysinfo(SI_ARCHITECTURE_64) = amd64", !r.err && seq(name, "amd64"), r.err);
}

static void test_brk(void)
{
    puts_("memory\n");
    struct sc r = sys1(SIEOS_SYS_brk, 0);
    long cur = r.val;
    r = sys1(SIEOS_SYS_brk, cur + 16384);
    check("brk grows the heap", !r.err && r.val == cur + 16384, r.val);
    volatile char *p = (volatile char *)cur;
    p[0] = 1;
    p[16383] = 2;
    check("new heap is writable", p[0] == 1 && p[16383] == 2, 0);
}

static void set_xmm(double v)
{
    __asm__ volatile("movsd %0, %%xmm7" :: "m"(v) : "xmm7");
}

static double get_xmm(void)
{
    double v;
    __asm__ volatile("movsd %%xmm7, %0" : "=m"(v));
    return v;
}

static void test_fpu(void)
{
    puts_("FPU/SSE state\n");
    struct sc r = sys1(SIEOS_SYS_forkx, 0);
    if (r.val == 0 && !r.err) {                       /* child: hammer xmm7 */
        for (int i = 0; i < 20; i++) {
            set_xmm(-1.0 * i);
            struct sieos_timespec q = { 0, 5000000 };
            sys2(SIEOS_SYS_nanosleep, (long)&q, 0);
        }
        sys1(SIEOS_SYS_exit, get_xmm() == -19.0 ? 0 : 1);
    }
    set_xmm(3.25);
    double acc = 0;
    for (int i = 0; i < 10; i++) {
        struct sieos_timespec q = { 0, 10000000 };
        sys2(SIEOS_SYS_nanosleep, (long)&q, 0);
        acc += get_xmm();
    }
    int status = 0;
    wait_child(r.val, &status);
    check("xmm7 survives context switches", acc == 32.5, (long)(acc * 100));
    check("the child kept its own xmm7", status == 0, status);
    unsigned int mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    check("MXCSR default 0x1F80", (mxcsr & 0xFFBF) == 0x1F80, mxcsr);
}

static void test_nx(void)
{
    puts_("page protection (NX)\n");
    struct sc r = sys1(SIEOS_SYS_forkx, 0);
    if (r.val == 0 && !r.err) {                       /* child: execute code on the stack */
        unsigned char code[16] = { 0xC3 };            /* ret */
        ((void (*)(void))code)();
        sys1(SIEOS_SYS_exit, 42);                     /* NX not enforced */
    }
    int status = 0;
    wait_child(r.val, &status);
    check("executing the stack faults (SIGSEGV)", (status & 0x7f) == 11, status);
    r = sys1(SIEOS_SYS_forkx, 0);
    if (r.val == 0 && !r.err) {                       /* child: write to its own text */
        extern char _start[] __attribute__((visibility("hidden")));
        *(volatile char *)_start = (char)0x90;
        sys1(SIEOS_SYS_exit, 42);
    }
    wait_child(r.val, &status);
    check("writing the text segment faults (SIGSEGV)", (status & 0x7f) == 11, status);
}


/* ================= milestone 3: LWPs, signals, waitid ================= */

static long gettid(void) { return sys0(SIEOS_SYS_lwp_self).val; }
static void msleep(long ms)
{
    struct sieos_timespec q = { ms / 1000, (ms % 1000) * 1000000L };
    sys2(SIEOS_SYS_nanosleep, (long)&q, 0);
}

/* a mutex on lwp_umtx_wait/wake: 0 free, 1 locked, 2 locked with waiters */
static void mutex_lock(volatile int *m)
{
    int c = __sync_val_compare_and_swap(m, 0, 1);
    if (c == 0)
        return;
    if (c != 2)
        c = __atomic_exchange_n(m, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        sys4(SIEOS_SYS_lwp_umtx_wait, (long)m, 2, 0, SIEOS_UMTX_PRIVATE);
        c = __atomic_exchange_n(m, 2, __ATOMIC_ACQUIRE);
    }
}

static void mutex_unlock(volatile int *m)
{
    if (__atomic_fetch_sub(m, 1, __ATOMIC_RELEASE) != 1) {
        *m = 0;
        sys3(SIEOS_SYS_lwp_umtx_wake, (long)m, 1, SIEOS_UMTX_PRIVATE);
    }
}

struct tcb { struct tcb *self; long id; long ok; };
static struct tcb tcbs[8];
static volatile int counter_lock;
static volatile long counter;
static volatile int parked_flag, early_flag;

static char *stack_alloc(long size)
{
    long cur = sys1(SIEOS_SYS_brk, 0).val;
    sys1(SIEOS_SYS_brk, cur + size);
    return (char *)cur;
}

static long start_lwp(void (*fn)(long), long arg, struct tcb *tcb, int flags)
{
    static sieos_ucontext_t uc;
    for (unsigned i = 0; i < sizeof(uc); i++)
        ((char *)&uc)[i] = 0;
    char *stk = stack_alloc(65536);
    uc.uc_mcontext.gregs[SIEOS_REG_RIP] = (long)fn;
    uc.uc_mcontext.gregs[SIEOS_REG_RSP] = ((long)(stk + 65536) & ~15L) - 8;
    uc.uc_mcontext.gregs[SIEOS_REG_RDI] = arg;
    uc.uc_mcontext.gregs[SIEOS_REG_FSBASE] = (long)tcb;
    sieos_lwpid_t id = 0;
    struct sc r = sys3(SIEOS_SYS_lwp_create, (long)&uc, flags, (long)&id);
    return r.err ? -(long)r.err : (long)id;
}

static void counter_thread(long idx)
{
    struct tcb *self;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(self));
    tcbs[idx].ok = self == &tcbs[idx] && sys3(SIEOS_SYS_lwp_private, SIEOS_LWP_GETPRIVATE, SIEOS_LWP_FSBASE, 0).val ==
                                             (long)&tcbs[idx];
    for (int i = 0; i < 2000; i++) {
        mutex_lock(&counter_lock);
        long v = counter;
        if ((i & 63) == 0)
            sys0(SIEOS_SYS_yield);
        counter = v + 1;
        mutex_unlock(&counter_lock);
    }
    sys0(SIEOS_SYS_lwp_exit);
}

static void park_thread(long arg)
{
    (void)arg;
    msleep(80);                                     /* main unparks us before we park */
    struct sc r = sys2(SIEOS_SYS_lwp_park, 0, 0);
    early_flag = !r.err;
    r = sys2(SIEOS_SYS_lwp_park, 0, 0);             /* now wait for a real unpark */
    parked_flag = r.err ? -1 : 1;
    sys0(SIEOS_SYS_lwp_exit);
}

static volatile int got_sig, got_code, got_pid, got_onstack;
static volatile long got_value;
static char altstack[16384] __attribute__((aligned(16)));

static void handler(int sig, sieos_siginfo_t *si, sieos_ucontext_t *uc)
{
    got_sig = sig;
    got_code = si->si_code;
    got_pid = si->__data.proc.pid;
    got_value = (long)si->__data.proc.value.sival_ptr;
    char here;
    got_onstack = &here >= altstack && &here < altstack + sizeof(altstack);
    sys2(SIEOS_SYS_context, SIEOS_SETCONTEXT, (long)uc);   /* handlers never return */
}

static void set_handler(int sig, int flags)
{
    struct sieos_sigaction sa;
    for (unsigned i = 0; i < sizeof(sa); i++)
        ((char *)&sa)[i] = 0;
    sa.__sa_u.sa_sigaction = (void (*)(int, sieos_siginfo_t *, void *))handler;
    sa.sa_flags = SIEOS_SA_SIGINFO | flags;
    sys3(SIEOS_SYS_sigaction, sig, (long)&sa, 0);
}

static void sigset_one(sieos_sigset_t *s, int sig)
{
    for (int i = 0; i < 4; i++)
        s->__sigbits[i] = 0;
    s->__sigbits[(sig - 1) / 32] = 1u << ((sig - 1) % 32);
}

static volatile long waiter_result;

static void sigwait_thread(long arg)
{
    (void)arg;
    sieos_sigset_t set;
    sigset_one(&set, SIEOS_SIGUSR2);
    sys3(SIEOS_SYS_lwp_sigmask, SIEOS_SIG_BLOCK, (long)&set, 0);
    sieos_siginfo_t si;
    struct sieos_timespec to = { 5, 0 };
    struct sc r = sys3(SIEOS_SYS_sigtimedwait, (long)&set, (long)&si, (long)&to);
    waiter_result = !r.err && r.val == SIEOS_SIGUSR2 && si.si_code == SIEOS_SI_LWP ? 1 : -1;
    sys0(SIEOS_SYS_lwp_exit);
}

static void test_lwps(void)
{
    puts_("LWPs\n");
    check("lwp_self of the first LWP is 1", gettid() == 1, gettid());
    long ids[4];
    for (int i = 0; i < 4; i++) {
        tcbs[i].self = &tcbs[i];
        tcbs[i].id = i;
        ids[i] = start_lwp(counter_thread, i, &tcbs[i], 0);
    }
    check("lwp_create x4", ids[0] > 1 && ids[3] > ids[0], ids[0]);
    int joined = 0;
    for (int i = 0; i < 4; i++) {
        sieos_lwpid_t who = 0;
        struct sc r = sys2(SIEOS_SYS_lwp_wait, ids[i], (long)&who);
        joined += !r.err && who == ids[i];
    }
    check("lwp_wait joins every LWP", joined == 4, joined);
    check("umtx mutex: 4 x 2000 increments", counter == 8000, counter);
    check("each LWP has its own TLS (%fs:0, lwp_private)", tcbs[0].ok && tcbs[1].ok && tcbs[2].ok && tcbs[3].ok, 0);
    struct sc r = sys2(SIEOS_SYS_lwp_wait, 0, 0);
    check("lwp_wait with nothing left: ESRCH", r.err == SIEOS_ESRCH, r.err);

    tcbs[4].self = &tcbs[4];
    long pt = start_lwp(park_thread, 0, &tcbs[4], 0);
    sys1(SIEOS_SYS_lwp_unpark, pt);                 /* before it parks: remembered */
    msleep(200);
    check("an unpark before the park is remembered", early_flag == 1, early_flag);
    sys1(SIEOS_SYS_lwp_unpark, pt);
    sys2(SIEOS_SYS_lwp_wait, pt, 0);
    check("lwp_park returns on lwp_unpark", parked_flag == 1, parked_flag);
    struct sieos_timespec to = { 0, 30000000 };
    r = sys2(SIEOS_SYS_lwp_park, (long)&to, 0);
    check("lwp_park times out (ETIME)", r.err == SIEOS_ETIME, r.err);
    volatile int w = 5;
    r = sys4(SIEOS_SYS_lwp_umtx_wait, (long)&w, 4, 0, 0);
    check("lwp_umtx_wait with a changed value: EAGAIN", r.err == SIEOS_EAGAIN, r.err);
}

static void test_signals(void)
{
    puts_("signals (ABI v2)\n");
    long pid = sys0(SIEOS_SYS_getpid).val;
    set_handler(SIEOS_SIGUSR1, 0);
    struct sc r = sys2(SIEOS_SYS_kill, pid, SIEOS_SIGUSR1);
    check("handler runs, returns through context(SETCONTEXT)", !r.err && got_sig == SIEOS_SIGUSR1, got_sig);
    check("siginfo: SI_USER and the sender's pid", got_code == SIEOS_SI_USER && got_pid == pid, got_code);
    got_sig = 0;
    r = sys3(SIEOS_SYS_sigqueue, pid, SIEOS_SIGUSR1, 12345);
    check("sigqueue: SI_QUEUE with the value", got_sig == SIEOS_SIGUSR1 && got_code == SIEOS_SI_QUEUE &&
          got_value == 12345, got_value);

    sieos_stack_t ss = { altstack, sizeof(altstack), 0, 0 };
    sys2(SIEOS_SYS_sigaltstack, (long)&ss, 0);
    set_handler(SIEOS_SIGUSR2, SIEOS_SA_ONSTACK);
    got_sig = 0;
    sys2(SIEOS_SYS_kill, pid, SIEOS_SIGUSR2);
    check("SA_ONSTACK runs on the alternate stack", got_sig == SIEOS_SIGUSR2 && got_onstack, got_onstack);

    sieos_sigset_t set, pend;
    sigset_one(&set, SIEOS_SIGUSR1);
    sys3(SIEOS_SYS_lwp_sigmask, SIEOS_SIG_BLOCK, (long)&set, 0);
    got_sig = 0;
    sys2(SIEOS_SYS_kill, pid, SIEOS_SIGUSR1);
    sys2(SIEOS_SYS_sigpending, SIEOS_SIGPENDING, (long)&pend);
    check("a blocked signal stays pending", got_sig == 0 && (pend.__sigbits[0] & (1u << (SIEOS_SIGUSR1 - 1))),
          pend.__sigbits[0]);
    sieos_siginfo_t si;
    struct sieos_timespec to = { 1, 0 };
    r = sys3(SIEOS_SYS_sigtimedwait, (long)&set, (long)&si, (long)&to);
    check("sigtimedwait takes it", !r.err && r.val == SIEOS_SIGUSR1 && si.si_signo == SIEOS_SIGUSR1, r.val);
    to.tv_sec = 0;
    to.tv_nsec = 20000000;
    r = sys3(SIEOS_SYS_sigtimedwait, (long)&set, (long)&si, (long)&to);
    check("sigtimedwait times out: EAGAIN", r.err == SIEOS_EAGAIN, r.err);

    /* sigsuspend: another LWP sends the signal a little later */
    got_sig = 0;
    sieos_sigset_t none;
    for (int i = 0; i < 4; i++)
        none.__sigbits[i] = 0;
    tcbs[5].self = &tcbs[5];
    long t = start_lwp((void (*)(long))0, 0, &tcbs[5], SIEOS_LWP_SUSPENDED);
    (void)t;                                        /* created suspended, never continued: no effect */
    struct sc s = sys1(SIEOS_SYS_forkx, 0);
    if (s.val == 0 && !s.err) {
        msleep(50);
        sys2(SIEOS_SYS_kill, pid, SIEOS_SIGUSR1);
        sys1(SIEOS_SYS_exit, 0);
    }
    r = sys1(SIEOS_SYS_sigsuspend, (long)&none);
    sieos_sigset_t now;
    sys3(SIEOS_SYS_lwp_sigmask, SIEOS_SIG_BLOCK, 0, (long)&now);
    check("sigsuspend: EINTR after the handler, mask restored", r.err == SIEOS_EINTR && got_sig == SIEOS_SIGUSR1 &&
          (now.__sigbits[0] & (1u << (SIEOS_SIGUSR1 - 1))), r.err);
    sieos_siginfo_t wsi;
    sys4(SIEOS_SYS_waitid, SIEOS_P_PID, s.val, (long)&wsi, SIEOS_WEXITED);
    sys3(SIEOS_SYS_lwp_sigmask, SIEOS_SIG_UNBLOCK, (long)&set, 0);

    tcbs[6].self = &tcbs[6];
    long wt = start_lwp(sigwait_thread, 0, &tcbs[6], 0);
    msleep(50);
    sys2(SIEOS_SYS_lwp_kill, wt, SIEOS_SIGUSR2);
    sys2(SIEOS_SYS_lwp_wait, wt, 0);
    check("lwp_kill reaches that LWP (SI_LWP)", waiter_result == 1, waiter_result);

    static sieos_ucontext_t uc;
    static volatile int passes;
    passes = 0;
    sys2(SIEOS_SYS_context, SIEOS_GETCONTEXT, (long)&uc);
    passes++;
    if (passes == 1)
        sys2(SIEOS_SYS_context, SIEOS_SETCONTEXT, (long)&uc);
    check("getcontext / setcontext resume after getcontext", passes == 2, passes);
}

static void test_wait(void)
{
    puts_("waitid and process groups\n");
    sieos_siginfo_t si;
    struct sc c = sys1(SIEOS_SYS_forkx, 0);
    if (c.val == 0 && !c.err)
        sys1(SIEOS_SYS_exit, 7);
    struct sc r = sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WEXITED);
    check("waitid: CLD_EXITED with status 7", !r.err && si.si_code == SIEOS_CLD_EXITED && si.__data.proc.status == 7 &&
          si.__data.proc.pid == c.val, si.si_code);
    c = sys1(SIEOS_SYS_forkx, 0);
    if (c.val == 0 && !c.err)
        for (;;) msleep(1000);
    r = sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WEXITED | SIEOS_WNOHANG);
    check("WNOHANG with a running child: 0, si_pid 0", !r.err && si.__data.proc.pid == 0, si.__data.proc.pid);
    sys2(SIEOS_SYS_kill, c.val, SIEOS_SIGSTOP);
    r = sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WSTOPPED);
    check("waitid: CLD_STOPPED by SIGSTOP", !r.err && si.si_code == SIEOS_CLD_STOPPED &&
          si.__data.proc.status == SIEOS_SIGSTOP, si.si_code);
    sys2(SIEOS_SYS_kill, c.val, SIEOS_SIGCONT);
    r = sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WCONTINUED);
    check("waitid: CLD_CONTINUED", !r.err && si.si_code == SIEOS_CLD_CONTINUED, si.si_code);
    sys2(SIEOS_SYS_kill, c.val, SIEOS_SIGTERM);
    r = sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WEXITED);
    check("waitid: CLD_KILLED by SIGTERM", !r.err && si.si_code == SIEOS_CLD_KILLED &&
          si.__data.proc.status == SIEOS_SIGTERM, si.__data.proc.status);

    long pgrp = sys1(SIEOS_SYS_pgrpsys, SIEOS_PGRP_GETPGRP).val;
    long pgid = sys2(SIEOS_SYS_pgrpsys, SIEOS_PGRP_GETPGID, 0).val;
    long sid = sys2(SIEOS_SYS_pgrpsys, SIEOS_PGRP_GETSID, 0).val;
    check("pgrpsys: getpgrp = getpgid(0), getsid(0)", pgrp == pgid && pgrp > 0 && sid > 0, pgrp);
    c = sys1(SIEOS_SYS_forkx, SIEOS_FORK_WAITPID);
    if (c.val == 0 && !c.err) {
        long me = sys0(SIEOS_SYS_getpid).val;
        long ns = sys1(SIEOS_SYS_pgrpsys, SIEOS_PGRP_SETSID).val;
        sys1(SIEOS_SYS_exit, ns == me ? 3 : 4);
    }
    msleep(50);
    r = sys4(SIEOS_SYS_waitid, SIEOS_P_ALL, 0, (long)&si, SIEOS_WEXITED | SIEOS_WNOHANG);
    check("FORK_WAITPID child is not seen by waitid(P_ALL)", r.err == SIEOS_ECHILD, r.err);
    r = sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WEXITED);
    check("setsid in the child makes it a session leader", !r.err && si.__data.proc.status == 3, si.__data.proc.status);

    /* exit() from a second LWP ends the whole process */
    c = sys1(SIEOS_SYS_forkx, 0);
    if (c.val == 0 && !c.err) {
        tcbs[7].self = &tcbs[7];
        start_lwp((void (*)(long))(void *)0, 0, &tcbs[7], SIEOS_LWP_SUSPENDED | SIEOS_LWP_DETACHED);
        extern void exit9_thread(long);
        start_lwp(exit9_thread, 0, &tcbs[7], 0);
        for (;;) msleep(1000);
    }
    r = sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WEXITED);
    check("exit() in one LWP ends every LWP (status 9)", !r.err && si.si_code == SIEOS_CLD_EXITED &&
          si.__data.proc.status == 9, si.__data.proc.status);

    /* execve() from a threaded process */
    c = sys1(SIEOS_SYS_forkx, 0);
    if (c.val == 0 && !c.err) {
        extern void sleeper_thread(long);
        start_lwp(sleeper_thread, 0, &tcbs[7], 0);
        char *argv[] = { "true", 0 };
        char *envp[] = { 0 };
        sys3(SIEOS_SYS_execve, (long)"/bin/true", (long)argv, (long)envp);
        sys1(SIEOS_SYS_exit, 99);
    }
    r = sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WEXITED);
    check("execve ends the other LWPs first", !r.err && si.si_code == SIEOS_CLD_EXITED && si.__data.proc.status == 0,
          si.__data.proc.status);
}

void exit9_thread(long arg)
{
    (void)arg;
    msleep(30);
    sys1(SIEOS_SYS_exit, 9);
}

void sleeper_thread(long arg)
{
    (void)arg;
    for (;;) msleep(1000);
}

/* ================= milestone 4: memory ================= */

static long mmap_(long addr, long len, int prot, int flags, int fd, long off)
{
    struct sc r = sys6(SIEOS_SYS_mmap, addr, len, prot, flags, fd, off);
    return r.err ? -r.err : r.val;
}

/* run fn in a child; return its waitid code and status */
static int in_child(void (*fn)(long), long arg, int *status)
{
    struct sc c = sys1(SIEOS_SYS_forkx, 0);
    if (c.val == 0 && !c.err) {
        fn(arg);
        sys1(SIEOS_SYS_exit, 0);
    }
    sieos_siginfo_t si;
    sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WEXITED);
    *status = si.__data.proc.status;
    return si.si_code;
}

static void touch_write(long addr) { *(volatile char *)addr = 1; }
static void touch_read(long addr) { (void)*(volatile char *)addr; }
static void call_code(long addr) { ((void (*)(void))addr)(); }
static long deep(long n) { volatile char buf[4096]; buf[0] = (char)n; return n ? deep(n - 1) + buf[0] : 0; }
static void deep_child(long n) { deep(n); }

static void test_memory(void)
{
    puts_("memory (mmap)\n");
    int st, code;
    long len = 1 << 20;
    long a = mmap_(0, len, SIEOS_PROT_READ | SIEOS_PROT_WRITE, SIEOS_MAP_PRIVATE | SIEOS_MAP_ANON, -1, 0);
    check("mmap anonymous 1 MiB", a > 0 && (a & 4095) == 0 && a > 0x10000000, a);
    char vec[256];
    sys3(SIEOS_SYS_mincore, a, len, (long)vec);
    int resident = 0;
    for (int i = 0; i < 256; i++)
        resident += vec[i];
    check("pages are not allocated before they are touched", resident == 0, resident);
    volatile long *w = (volatile long *)a;
    int zero = w[0] == 0 && w[len / 8 - 1] == 0;
    w[0] = 111;
    w[512] = 222;
    sys3(SIEOS_SYS_mincore, a, len, (long)vec);
    check("demand-zero pages, resident once touched", zero && vec[0] == 1 && vec[1] == 1 && vec[2] == 0, vec[2]);

    long sh = mmap_(0, 4096, SIEOS_PROT_READ | SIEOS_PROT_WRITE, SIEOS_MAP_SHARED | SIEOS_MAP_ANON, -1, 0);
    volatile long *shp = (volatile long *)sh;
    shp[0] = 5;
    struct sc c = sys1(SIEOS_SYS_forkx, 0);
    if (c.val == 0 && !c.err) {
        int ok = w[0] == 111 && w[512] == 222;           /* inherited */
        w[0] = 999;                                       /* private: the parent keeps 111 */
        shp[0] = 77;                                      /* shared: the parent sees 77 */
        sys1(SIEOS_SYS_exit, ok ? 0 : 1);
    }
    w[512] = 333;                                         /* after fork: the child keeps 222 */
    sieos_siginfo_t si;
    sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WEXITED);
    check("fork: the child inherits the data", si.__data.proc.status == 0, si.__data.proc.status);
    check("copy-on-write: the child's write stays in the child", w[0] == 111 && w[512] == 333, w[0]);
    check("MAP_SHARED memory stays shared across fork", shp[0] == 77, shp[0]);

    sys2(SIEOS_SYS_munmap, a + 4096, 4096);
    code = in_child(touch_read, a + 4096, &st);
    check("access after munmap: SIGSEGV", (code == SIEOS_CLD_DUMPED || code == SIEOS_CLD_KILLED) && st == SIEOS_SIGSEGV, st);
    check("the rest of the mapping is still there", w[0] == 111, w[0]);
    sys3(SIEOS_SYS_mprotect, a, 4096, SIEOS_PROT_READ);
    code = in_child(touch_write, a, &st);
    check("write to a PROT_READ page: SIGSEGV", (code == SIEOS_CLD_DUMPED || code == SIEOS_CLD_KILLED) && st == SIEOS_SIGSEGV, st);
    sys3(SIEOS_SYS_mprotect, a, 4096, SIEOS_PROT_READ | SIEOS_PROT_WRITE);
    w[1] = 5;
    check("mprotect back to read/write", w[1] == 5 && w[0] == 111, w[1]);

    long f = mmap_(a + 8192, 8192, SIEOS_PROT_READ | SIEOS_PROT_WRITE,
                   SIEOS_MAP_PRIVATE | SIEOS_MAP_ANON | SIEOS_MAP_FIXED, -1, 0);
    check("MAP_FIXED replaces the pages there", f == a + 8192 && ((volatile long *)f)[0] == 0, f);
    long al = mmap_(0x200000, 4096, SIEOS_PROT_READ, SIEOS_MAP_PRIVATE | SIEOS_MAP_ANON | SIEOS_MAP_ALIGN, -1, 0);
    check("MAP_ALIGN 2 MiB", al > 0 && (al & 0x1fffff) == 0, al);

    struct sc o = sys4(SIEOS_SYS_openat, SIEOS_AT_FDCWD, (long)"/etc/passwd", SIEOS_O_RDONLY, 0);
    long fm = mmap_(0, 4096, SIEOS_PROT_READ | SIEOS_PROT_WRITE, SIEOS_MAP_PRIVATE, o.val, 0);
    char *fp = (char *)fm;
    int looks = fm > 0 && fp[0] == 'r' && fp[1] == 'o' && fp[4] == ':';
    fp[0] = 'X';
    char first;
    sys3(SIEOS_SYS_lseek, o.val, 0, 0);
    sys3(SIEOS_SYS_read, o.val, (long)&first, 1);
    sys1(SIEOS_SYS_close, o.val);
    check("MAP_PRIVATE file mapping; writes stay private", looks && first == 'r', fp[0]);

    w[2] = 42;
    sys4(SIEOS_SYS_memcntl, a, 4096, SIEOS_MC_ADVISE, SIEOS_MADV_DONTNEED);
    check("madvise(DONTNEED) gives zero-filled pages back", w[2] == 0, w[2]);

    code = in_child(deep_child, 512, &st);                 /* 2 MiB of stack */
    check("the stack grows on demand (2 MiB)", code == SIEOS_CLD_EXITED && st == 0, st);
    code = in_child(deep_child, 2400, &st);                /* more than 8 MiB */
    check("beyond 8 MiB of stack: SIGSEGV", (code == SIEOS_CLD_DUMPED || code == SIEOS_CLD_KILLED) && st == SIEOS_SIGSEGV, st);

    long x = mmap_(0, 4096, SIEOS_PROT_READ | SIEOS_PROT_WRITE, SIEOS_MAP_PRIVATE | SIEOS_MAP_ANON, -1, 0);
    *(volatile unsigned char *)x = 0xC3;                   /* ret */
    code = in_child(call_code, x, &st);
    check("data pages are not executable (NX)", (code == SIEOS_CLD_DUMPED || code == SIEOS_CLD_KILLED) && st == SIEOS_SIGSEGV, st);
    sys3(SIEOS_SYS_mprotect, x, 4096, SIEOS_PROT_READ | SIEOS_PROT_EXEC);
    call_code(x);
    check("mprotect(PROT_EXEC) makes them executable", 1, 0);
    long bad = mmap_(0, 4096, SIEOS_PROT_READ, SIEOS_MAP_SHARED | SIEOS_MAP_PRIVATE, -1, 0);
    check("mmap with a bad type: EINVAL", bad == -SIEOS_EINVAL, bad);
}

/* ---------------- milestone 5: file systems ---------------- */

static long open_(long dfd, const char *path, long flags, long mode)
{
    struct sc r = sys4(SIEOS_SYS_openat, dfd, (long)path, flags, mode);
    return r.err ? -r.err : r.val;
}
static long stat_(long dfd, const char *path, struct sieos_stat *st, long flag)
{
    struct sc r = sys4(SIEOS_SYS_fstatat, dfd, (long)path, (long)st, flag);
    return r.err ? -r.err : 0;
}
static long wr(long fd, const void *b, long n) { struct sc r = sys3(SIEOS_SYS_write, fd, (long)b, n); return r.err ? -r.err : r.val; }
static long rd(long fd, void *b, long n) { struct sc r = sys3(SIEOS_SYS_read, fd, (long)b, n); return r.err ? -r.err : r.val; }
static int meq(const void *a, const void *b, long n)
{
    const char *x = a, *y = b;
    for (long i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}
static void numstr(long v, char *out)
{
    char b[24];
    int i = 23;
    b[i] = 0;
    do { b[--i] = '0' + v % 10; v /= 10; } while (v);
    for (int k = 0; (out[k] = b[i + k]); k++)
        ;
}
static long readlink_(long dfd, const char *path, char *buf, long n)
{
    struct sc r = sys4(SIEOS_SYS_readlinkat, dfd, (long)path, (long)buf, n - 1);
    if (r.err)
        return -r.err;
    buf[r.val] = 0;
    return r.val;
}

static void truncate_checks(const char *label, const char *path)
{
    static char big[10000];
    for (int i = 0; i < 10000; i++)
        big[i] = 'a' + i % 26;
    long fd = open_(SIEOS_AT_FDCWD, path, SIEOS_O_RDWR | SIEOS_O_CREAT | SIEOS_O_TRUNC, 0644);
    wr(fd, big, 10000);
    struct sieos_stat st;
    struct sc r = sys2(SIEOS_SYS_ftruncate, fd, 5000);
    stat_(fd, 0, &st, 0);
    char c[4] = { 0 };
    sys4(SIEOS_SYS_pread, fd, (long)c, 1, 4999);
    int ok1 = !r.err && st.st_size == 5000 && c[0] == big[4999];
    r = sys2(SIEOS_SYS_ftruncate, fd, 20000);
    stat_(fd, 0, &st, 0);
    char z[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    struct sc r2 = sys4(SIEOS_SYS_pread, fd, (long)z, 8, 5000);
    int ok2 = !r.err && st.st_size == 20000 && r2.val == 8 && z[0] == 0 && z[7] == 0;
    sys2(SIEOS_SYS_ftruncate, fd, 3);
    stat_(fd, 0, &st, 0);
    sys3(SIEOS_SYS_lseek, fd, 0, 0);
    char h[8] = { 0 };
    long n = rd(fd, h, 8);
    sys1(SIEOS_SYS_close, fd);
    sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)path, 0);
    puts_("  ");
    puts_(label);
    puts_(":\n");
    check("  ftruncate shrink keeps the head", ok1, st.st_size);
    check("  ftruncate grow reads back zeros", ok2, r2.val);
    check("  ftruncate to 3 bytes", n == 3 && meq(h, "abc", 3) && st.st_size == 3, n);
}

static void test_vfs(void)
{
    puts_("file systems (milestone 5)\n");
    struct sieos_stat st, st2;
    struct sc r;
    long pid = sys0(SIEOS_SYS_getpid).val;

    /* mounts */
    check("/tmp is tmpfs", stat_(SIEOS_AT_FDCWD, "/tmp", &st, 0) == 0 && seq(st.st_fstype, "tmpfs"), 0);
    check("/proc is proc", stat_(SIEOS_AT_FDCWD, "/proc", &st, 0) == 0 && seq(st.st_fstype, "proc"), 0);
    stat_(SIEOS_AT_FDCWD, "/", &st2, 0);
    stat_(SIEOS_AT_FDCWD, "/tmp", &st, 0);
    check("st_dev differs across mounts", st.st_dev != st2.st_dev, 0);
    stat_(SIEOS_AT_FDCWD, "/tmp/..", &st, 0);
    check("/tmp/.. is /", st.st_ino == st2.st_ino && st.st_dev == st2.st_dev, st.st_ino);
    r = sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)"/tmp", SIEOS_AT_REMOVEDIR);
    check("rmdir of a mount point: EBUSY", r.err == SIEOS_EBUSY, r.err);

    /* *at with a directory descriptor */
    sys3(SIEOS_SYS_mkdirat, SIEOS_AT_FDCWD, (long)"/tmp/d", 0755);
    long dfd = open_(SIEOS_AT_FDCWD, "/tmp/d", SIEOS_O_RDONLY | SIEOS_O_DIRECTORY, 0);
    long fd = open_(dfd, "f", SIEOS_O_RDWR | SIEOS_O_CREAT | SIEOS_O_EXCL, 0600);
    check("openat relative to a dirfd", dfd >= 0 && fd >= 0, fd);
    wr(fd, "tmpfs data", 10);
    sys1(SIEOS_SYS_close, fd);
    check("fstatat relative to a dirfd", stat_(dfd, "f", &st, 0) == 0 && st.st_size == 10 &&
          (st.st_mode & 07777) == 0600, st.st_size);
    r = sys4(SIEOS_SYS_renameat, dfd, (long)"f", dfd, (long)"g");
    check("renameat", !r.err && stat_(dfd, "f", &st, 0) == -SIEOS_ENOENT && stat_(dfd, "g", &st, 0) == 0, r.err);

    /* symbolic links */
    r = sys3(SIEOS_SYS_symlinkat, (long)"g", dfd, (long)"l");
    char buf[256];
    long n = readlink_(dfd, "l", buf, sizeof(buf));
    check("symlinkat + readlinkat", !r.err && n == 1 && buf[0] == 'g', n);
    check("fstatat follows the link", stat_(dfd, "l", &st, 0) == 0 && (st.st_mode & SIEOS_S_IFMT) == SIEOS_S_IFREG, 0);
    check("AT_SYMLINK_NOFOLLOW sees the link", stat_(dfd, "l", &st, SIEOS_AT_SYMLINK_NOFOLLOW) == 0 &&
          (st.st_mode & SIEOS_S_IFMT) == SIEOS_S_IFLNK && st.st_size == 1, st.st_mode);
    fd = open_(SIEOS_AT_FDCWD, "/tmp/d/l", SIEOS_O_RDONLY, 0);
    n = rd(fd, buf, sizeof(buf));
    sys1(SIEOS_SYS_close, fd);
    check("open through a symlink", n == 10 && meq(buf, "tmpfs data", 10), n);
    check("O_NOFOLLOW on a symlink: ELOOP",
          open_(dfd, "l", SIEOS_O_RDONLY | SIEOS_O_NOFOLLOW, 0) == -SIEOS_ELOOP, 0);
    sys3(SIEOS_SYS_symlinkat, (long)"loop", dfd, (long)"loop");
    check("symlink loop: ELOOP", open_(dfd, "loop", SIEOS_O_RDONLY, 0) == -SIEOS_ELOOP, 0);
    sys3(SIEOS_SYS_symlinkat, (long)"/tmp/d", SIEOS_AT_FDCWD, (long)"/tmp/dl");
    check("directory symlink in the middle of a path", stat_(SIEOS_AT_FDCWD, "/tmp/dl/g", &st, 0) == 0, 0);
    sys3(SIEOS_SYS_symlinkat, (long)"../etc", SIEOS_AT_FDCWD, (long)"/tmp/up");
    check("relative symlink crossing a mount upwards", stat_(SIEOS_AT_FDCWD, "/tmp/up/passwd", &st, 0) == 0 &&
          seq(st.st_fstype, "ext4"), 0);

    /* hard links */
    r = sys6(SIEOS_SYS_linkat, dfd, (long)"g", dfd, (long)"h", 0, 0);
    stat_(dfd, "h", &st, 0);
    check("linkat: nlink 2", !r.err && st.st_nlink == 2, st.st_nlink);
    sys3(SIEOS_SYS_unlinkat, dfd, (long)"g", 0);
    fd = open_(dfd, "h", SIEOS_O_RDONLY, 0);
    n = rd(fd, buf, sizeof(buf));
    stat_(fd, 0, &st, 0);
    sys1(SIEOS_SYS_close, fd);
    check("data survives unlinking the other name", n == 10 && st.st_nlink == 1, n);
    r = sys6(SIEOS_SYS_linkat, SIEOS_AT_FDCWD, (long)"/etc/passwd", SIEOS_AT_FDCWD, (long)"/tmp/x", 0, 0);
    check("link across file systems: EXDEV", r.err == SIEOS_EXDEV, r.err);
    r = sys4(SIEOS_SYS_renameat, SIEOS_AT_FDCWD, (long)"/tmp/d/h", SIEOS_AT_FDCWD, (long)"/root/h");
    check("rename across file systems: EXDEV", r.err == SIEOS_EXDEV, r.err);

    /* ext4 links */
    r = sys3(SIEOS_SYS_symlinkat, (long)"/etc/passwd", SIEOS_AT_FDCWD, (long)"/root/pw");
    fd = open_(SIEOS_AT_FDCWD, "/root/pw", SIEOS_O_RDONLY, 0);
    n = rd(fd, buf, 5);
    sys1(SIEOS_SYS_close, fd);
    check("ext4 fast symlink", !r.err && n == 5 && meq(buf, "root:", 5), r.err);
    const char *longt = "/etc/../etc/./../etc/../etc/../etc/../etc/../etc/../etc/../etc/passwd";
    sys3(SIEOS_SYS_symlinkat, (long)longt, SIEOS_AT_FDCWD, (long)"/root/pwlong");
    n = readlink_(SIEOS_AT_FDCWD, "/root/pwlong", buf, sizeof(buf));
    check("ext4 slow symlink (> 60 bytes)", n == slen(longt) && seq(buf, longt) &&
          stat_(SIEOS_AT_FDCWD, "/root/pwlong", &st, 0) == 0 && st.st_size > 0, n);
    fd = open_(SIEOS_AT_FDCWD, "/root/hl1", SIEOS_O_WRONLY | SIEOS_O_CREAT | SIEOS_O_TRUNC, 0644);
    wr(fd, "x", 1);
    sys1(SIEOS_SYS_close, fd);
    r = sys6(SIEOS_SYS_linkat, SIEOS_AT_FDCWD, (long)"/root/hl1", SIEOS_AT_FDCWD, (long)"/root/hl2", 0, 0);
    stat_(SIEOS_AT_FDCWD, "/root/hl2", &st, 0);
    stat_(SIEOS_AT_FDCWD, "/root/hl1", &st2, 0);
    check("ext4 hard link", !r.err && st.st_nlink == 2 && st.st_ino == st2.st_ino, r.err);
    sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)"/root/hl1", 0);
    sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)"/root/hl2", 0);
    sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)"/root/pw", 0);
    sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)"/root/pwlong", 0);
    check("unlinking symlinks", stat_(SIEOS_AT_FDCWD, "/root/pwlong", &st, SIEOS_AT_SYMLINK_NOFOLLOW) == -SIEOS_ENOENT, 0);

    /* truncate */
    truncate_checks("ext4", "/root/trunc.tst");
    truncate_checks("tmpfs", "/tmp/trunc.tst");
    {
        /* interleaved writes fragment A into many extents (a depth-1 tree), then cut it mid-extent */
        static char blk[4096];
        long fa = open_(SIEOS_AT_FDCWD, "/root/fragA", SIEOS_O_RDWR | SIEOS_O_CREAT | SIEOS_O_TRUNC, 0644);
        long fb = open_(SIEOS_AT_FDCWD, "/root/fragB", SIEOS_O_RDWR | SIEOS_O_CREAT | SIEOS_O_TRUNC, 0644);
        for (int i = 0; i < 40; i++) {
            for (int j = 0; j < 4096; j++)
                blk[j] = (char)(i + 1);
            wr(fa, blk, 4096);
            wr(fb, blk, 4096);
        }
        struct sieos_stat sa;
        sys2(SIEOS_SYS_ftruncate, fa, 17 * 4096 + 100);
        stat_(fa, 0, &sa, 0);
        char t2[2] = { 0 };
        sys4(SIEOS_SYS_pread, fa, (long)t2, 2, 17 * 4096 + 99);
        long blocks_after = sa.st_blocks;
        check("ext4: truncate a fragmented file (depth-1 extent tree)",
              sa.st_size == 17 * 4096 + 100 && t2[0] == 18 && blocks_after <= 19 * 8, blocks_after);
        sys1(SIEOS_SYS_close, fa);
        sys1(SIEOS_SYS_close, fb);
        sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)"/root/fragA", 0);
        sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)"/root/fragB", 0);
    }

    /* positioned and vector I/O */
    fd = open_(SIEOS_AT_FDCWD, "/tmp/io", SIEOS_O_RDWR | SIEOS_O_CREAT, 0644);
    wr(fd, "0123456789", 10);
    r = sys4(SIEOS_SYS_pwrite, fd, (long)"AB", 2, 3);
    struct sc cur = sys3(SIEOS_SYS_lseek, fd, 0, SIEOS_SEEK_CUR);
    char pb[4] = { 0 };
    sys4(SIEOS_SYS_pread, fd, (long)pb, 3, 2);
    check("pwrite/pread keep the offset", !r.err && cur.val == 10 && meq(pb, "2AB", 3), cur.val);
    struct sieos_iovec iov[2] = { { "xy", 2 }, { "zw", 2 } };
    r = sys3(SIEOS_SYS_writev, fd, (long)iov, 2);
    sys3(SIEOS_SYS_lseek, fd, 8, SIEOS_SEEK_SET);
    char v1[3] = { 0 }, v2[4] = { 0 };
    struct sieos_iovec riov[2] = { { v1, 3 }, { v2, 3 } };
    struct sc rv = sys3(SIEOS_SYS_readv, fd, (long)riov, 2);
    check("writev/readv", r.val == 4 && rv.val == 6 && meq(v1, "89x", 3) && meq(v2, "yzw", 3), rv.val);

    /* timestamps */
    struct sieos_timespec ts[2] = { { 1000000000, 5 }, { 1234567890, 123456789 } };
    r = sys4(SIEOS_SYS_utimensat, SIEOS_AT_FDCWD, (long)"/tmp/io", (long)ts, 0);
    stat_(fd, 0, &st, 0);
    check("utimensat sets ns timestamps", !r.err && st.st_mtim.tv_sec == 1234567890 &&
          st.st_mtim.tv_nsec == 123456789 && st.st_atim.tv_sec == 1000000000, st.st_mtim.tv_nsec);
    struct sieos_timespec ts2[2] = { { 0, SIEOS_UTIME_OMIT }, { 0, SIEOS_UTIME_NOW } };
    sys4(SIEOS_SYS_utimensat, fd, 0, (long)ts2, 0);
    stat_(fd, 0, &st, 0);
    check("UTIME_OMIT / UTIME_NOW (fd)", st.st_atim.tv_sec == 1000000000 && st.st_mtim.tv_sec > 1600000000, 0);
    fd = open_(SIEOS_AT_FDCWD, "/root/ts", SIEOS_O_RDWR | SIEOS_O_CREAT, 0644);
    r = sys4(SIEOS_SYS_utimensat, fd, 0, (long)ts, 0);
    stat_(fd, 0, &st, 0);
    check("utimensat on ext4", !r.err && st.st_mtim.tv_sec == 1234567890 && st.st_mtim.tv_nsec == 123456789,
          st.st_mtim.tv_nsec);
    sys1(SIEOS_SYS_close, fd);
    sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)"/root/ts", 0);

    /* fcntl */
    fd = open_(SIEOS_AT_FDCWD, "/tmp/io", SIEOS_O_RDWR | SIEOS_O_CLOEXEC, 0);
    struct sc g = sys3(SIEOS_SYS_fcntl, fd, SIEOS_F_GETFD, 0);
    struct sc d = sys3(SIEOS_SYS_fcntl, fd, SIEOS_F_DUPFD, 20);
    struct sc g2 = sys3(SIEOS_SYS_fcntl, d.val, SIEOS_F_GETFD, 0);
    check("O_CLOEXEC, F_GETFD, F_DUPFD", g.val == SIEOS_FD_CLOEXEC && d.val >= 20 && g2.val == 0, d.val);
    sys3(SIEOS_SYS_fcntl, fd, SIEOS_F_SETFL, SIEOS_O_APPEND | SIEOS_O_NONBLOCK);
    g = sys3(SIEOS_SYS_fcntl, d.val, SIEOS_F_GETFL, 0);
    check("F_SETFL/F_GETFL (shared by dups)", (g.val & SIEOS_O_APPEND) && (g.val & SIEOS_O_NONBLOCK) &&
          (g.val & 3) == SIEOS_O_RDWR, g.val);
    d = sys3(SIEOS_SYS_fcntl, fd, SIEOS_F_DUP2FD_CLOEXEC, 30);
    g = sys3(SIEOS_SYS_fcntl, 30, SIEOS_F_GETFD, 0);
    check("F_DUP2FD_CLOEXEC", d.val == 30 && g.val == SIEOS_FD_CLOEXEC, d.val);
    sys1(SIEOS_SYS_close, 30);
    sys1(SIEOS_SYS_close, 20);
    struct sieos_flock fl = { 0 };
    fl.l_whence = SIEOS_SEEK_SET;
    fl.l_start = 4;
    fl.l_len = 0;
    r = sys3(SIEOS_SYS_fcntl, fd, SIEOS_F_FREESP, (long)&fl);
    stat_(fd, 0, &st, 0);
    check("F_FREESP truncates", !r.err && st.st_size == 4, st.st_size);

    /* record locks */
    fl.l_type = SIEOS_F_WRLCK;
    fl.l_start = 0;
    fl.l_len = 100;
    r = sys3(SIEOS_SYS_fcntl, fd, SIEOS_F_SETLK, (long)&fl);
    check("F_SETLK write lock", !r.err, r.err);
    struct sc c = sys1(SIEOS_SYS_forkx, 0);
    if (c.val == 0 && !c.err) {
        struct sieos_flock l = { 0 };
        l.l_type = SIEOS_F_WRLCK;
        l.l_start = 50;
        l.l_len = 10;
        int code = 0;
        if (sys3(SIEOS_SYS_fcntl, fd, SIEOS_F_SETLK, (long)&l).err != SIEOS_EAGAIN)
            code |= 1;
        sys3(SIEOS_SYS_fcntl, fd, SIEOS_F_GETLK, (long)&l);
        if (l.l_type != SIEOS_F_WRLCK || l.l_pid != pid || l.l_start != 0 || l.l_len != 100)
            code |= 2;
        l.l_type = SIEOS_F_RDLCK;
        l.l_start = 100;
        l.l_len = 10;
        if (sys3(SIEOS_SYS_fcntl, fd, SIEOS_F_SETLK, (long)&l).err)
            code |= 4;
        l.l_type = SIEOS_F_WRLCK;
        l.l_start = 0;
        l.l_len = 1;
        if (sys3(SIEOS_SYS_fcntl, fd, SIEOS_F_SETLKW, (long)&l).err)   /* waits for the parent */
            code |= 8;
        sys1(SIEOS_SYS_exit, code);
    }
    msleep(150);
    fl.l_type = SIEOS_F_UNLCK;
    fl.l_start = 0;
    fl.l_len = 0;
    sys3(SIEOS_SYS_fcntl, fd, SIEOS_F_SETLK, (long)&fl);
    sieos_siginfo_t si;
    sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WEXITED);
    check("locks: conflict, F_GETLK owner, SETLKW wakes on unlock", si.__data.proc.status == 0,
          si.__data.proc.status);
    fl.l_type = SIEOS_F_WRLCK;
    fl.l_start = 0;
    fl.l_len = 0;
    r = sys3(SIEOS_SYS_fcntl, fd, SIEOS_F_SETLK, (long)&fl);
    check("child's locks released at exit", !r.err, r.err);
    sys1(SIEOS_SYS_close, fd);

    /* directory cookies */
    fd = open_(SIEOS_AT_FDCWD, "/bin", SIEOS_O_RDONLY | SIEOS_O_DIRECTORY, 0);
    static char db[4096] __attribute__((aligned(8)));
    r = sys3(SIEOS_SYS_getdents, fd, (long)db, sizeof(db));
    struct sieos_dirent *e0 = (struct sieos_dirent *)db;
    struct sieos_dirent *e1 = (struct sieos_dirent *)(db + e0->d_reclen);
    struct sieos_dirent *e2 = (struct sieos_dirent *)((char *)e1 + e1->d_reclen);
    char want[64];
    int k = 0;
    for (; e2->d_name[k] && k < 63; k++)
        want[k] = e2->d_name[k];
    want[k] = 0;
    sys3(SIEOS_SYS_lseek, fd, e1->d_off, SIEOS_SEEK_SET);
    r = sys3(SIEOS_SYS_getdents, fd, (long)db, sizeof(db));
    check("lseek to a d_off cookie resumes getdents", !r.err && r.val > 0 &&
          seq(((struct sieos_dirent *)db)->d_name, want), r.err);
    sys1(SIEOS_SYS_close, fd);
    r = sys3(SIEOS_SYS_getdents, dfd, (long)db, 8);
    check("getdents with a tiny buffer: EINVAL", r.err == SIEOS_EINVAL, r.err);

    /* directories */
    r = sys1(SIEOS_SYS_fchdir, dfd);
    sys2(SIEOS_SYS_getcwd, (long)buf, sizeof(buf));
    check("fchdir + getcwd across a mount", !r.err && seq(buf, "/tmp/d"), r.err);
    sys1(SIEOS_SYS_chdir, (long)"/proc/self");
    sys2(SIEOS_SYS_getcwd, (long)buf, sizeof(buf));
    char pidpath[32] = "/proc/";
    numstr(pid, pidpath + 6);
    check("getcwd inside /proc", seq(buf, pidpath), 0);
    sys1(SIEOS_SYS_chdir, (long)"/");
    struct sieos_statvfs sv;
    r = sys2(SIEOS_SYS_statvfs, (long)"/tmp", (long)&sv);
    struct sieos_statvfs sv2;
    struct sc r3 = sys2(SIEOS_SYS_fstatvfs, 1, (long)&sv2);
    check("statvfs / fstatvfs", !r.err && seq(sv.f_basetype, "tmpfs") && sv.f_blocks > 0 && !r3.err, r.err);
    r = sys4(SIEOS_SYS_mknodat, dfd, (long)"reg", SIEOS_S_IFREG | 0600, 0);
    check("mknodat (regular file)", !r.err && stat_(dfd, "reg", &st, 0) == 0, r.err);
    r = sys4(SIEOS_SYS_faccessat, SIEOS_AT_FDCWD, (long)"/etc/passwd", SIEOS_R_OK, SIEOS_AT_EACCESS);
    check("faccessat AT_EACCESS", !r.err, r.err);

    /* /proc */
    fd = open_(SIEOS_AT_FDCWD, "/proc/self/psinfo", SIEOS_O_RDONLY, 0);
    sieos_psinfo_t ps;
    r = sys4(SIEOS_SYS_pread, fd, (long)&ps, sizeof(ps), 0);
    sys1(SIEOS_SYS_close, fd);
    check("/proc/self/psinfo", r.val == sizeof(ps) && ps.pr_pid == pid && seq(ps.pr_fname, "abi2test") &&
          ps.pr_nlwp >= 1 && ps.pr_dmodel == 2 && ps.pr_lwp.pr_lwpid >= 1, r.val);
    char path[64] = "/proc/";
    numstr(pid, path + 6);
    long plen = slen(path);
    const char *tail = "/status";
    for (k = 0; tail[k]; k++)
        path[plen + k] = tail[k];
    path[plen + k] = 0;
    fd = open_(SIEOS_AT_FDCWD, path, SIEOS_O_RDONLY, 0);
    sieos_pstatus_t pst;
    r = sys4(SIEOS_SYS_pread, fd, (long)&pst, sizeof(pst), 0);
    sys1(SIEOS_SYS_close, fd);
    check("/proc/<pid>/status", r.val == sizeof(pst) && pst.pr_pid == pid && pst.pr_nlwp >= 1, r.val);
    fd = open_(SIEOS_AT_FDCWD, "/proc/self/cred", SIEOS_O_RDONLY, 0);
    sieos_prcred_t cr;
    r = sys3(SIEOS_SYS_read, fd, (long)&cr, sizeof(cr));
    sys1(SIEOS_SYS_close, fd);
    check("/proc/self/cred", r.val == sizeof(cr) && cr.pr_euid == sys0(SIEOS_SYS_geteuid).val, r.val);
    fd = open_(SIEOS_AT_FDCWD, "/proc/self/lwp/1/lwpsinfo", SIEOS_O_RDONLY, 0);
    sieos_lwpsinfo_t li;
    r = sys3(SIEOS_SYS_read, fd, (long)&li, sizeof(li));
    sys1(SIEOS_SYS_close, fd);
    check("/proc/self/lwp/1/lwpsinfo", r.val == sizeof(li) && li.pr_lwpid == 1 && li.pr_sname == 'O', r.val);
    fd = open_(SIEOS_AT_FDCWD, "/proc/self/usage", SIEOS_O_RDONLY, 0);
    sieos_prusage_t us;
    r = sys3(SIEOS_SYS_read, fd, (long)&us, sizeof(us));
    sys1(SIEOS_SYS_close, fd);
    check("/proc/self/usage", r.val == sizeof(us) && us.pr_count >= 1, r.val);
    n = readlink_(SIEOS_AT_FDCWD, "/proc/self", buf, sizeof(buf));
    check("/proc/self -> pid", n > 0 && seq(buf, pidpath + 6), n);
    n = readlink_(SIEOS_AT_FDCWD, "/proc/self/cwd", buf, sizeof(buf));
    check("/proc/self/cwd", n == 1 && buf[0] == '/', n);
    fd = open_(dfd, "reg", SIEOS_O_RDONLY, 0);
    char fdp[48] = "/proc/self/fd/";
    numstr(fd, fdp + 14);
    n = readlink_(SIEOS_AT_FDCWD, fdp, buf, sizeof(buf));
    sys1(SIEOS_SYS_close, fd);
    check("/proc/self/fd/<n> names the file", n > 0 && seq(buf, "/tmp/d/reg"), n);
    check("/proc is read-only", open_(SIEOS_AT_FDCWD, "/proc/self/psinfo", SIEOS_O_WRONLY, 0) < 0, 0);
    int seen_self = 0, seen_init = 0;
    fd = open_(SIEOS_AT_FDCWD, "/proc", SIEOS_O_RDONLY, 0);
    r = sys3(SIEOS_SYS_getdents, fd, (long)db, sizeof(db));
    for (long off = 0; !r.err && off < r.val;) {
        struct sieos_dirent *e = (struct sieos_dirent *)(db + off);
        if (seq(e->d_name, pidpath + 6)) seen_self = 1;
        if (seq(e->d_name, "1")) seen_init = 1;
        off += e->d_reclen;
    }
    sys1(SIEOS_SYS_close, fd);
    check("/proc lists processes", seen_self && seen_init, r.err);

    /* pseudo-terminals */
    long m = open_(SIEOS_AT_FDCWD, "/dev/ptmx", SIEOS_O_RDWR | SIEOS_O_NOCTTY, 0);
    r = sys3(SIEOS_SYS_ioctl, m, SIEOS_ISPTM, 0);
    char pts[32] = { 0 };
    struct sc r4 = sys3(SIEOS_SYS_ioctl, m, SIEOS_PTSNAME, (long)pts);
    check("/dev/ptmx: ISPTM, PTSNAME", m >= 0 && !r.err && !r4.err && meq(pts, "/dev/pts/", 9), m);
    check("slave locked until UNLKPT", open_(SIEOS_AT_FDCWD, pts, SIEOS_O_RDWR | SIEOS_O_NOCTTY, 0) == -SIEOS_EIO, 0);
    sys3(SIEOS_SYS_ioctl, m, SIEOS_UNLKPT, 0);
    long sfd = open_(SIEOS_AT_FDCWD, pts, SIEOS_O_RDWR | SIEOS_O_NOCTTY, 0);
    check("open /dev/pts/N", sfd >= 0 && stat_(SIEOS_AT_FDCWD, pts, &st, 0) == 0 &&
          (st.st_mode & SIEOS_S_IFMT) == SIEOS_S_IFCHR, sfd);
    wr(m, "hi\n", 3);
    char tb[16] = { 0 };
    n = rd(sfd, tb, sizeof(tb));
    check("master -> slave (line discipline)", n == 3 && meq(tb, "hi\n", 3), n);
    struct sieos_termios tio;
    r = sys3(SIEOS_SYS_ioctl, sfd, SIEOS_TCGETS, (long)&tio);
    check("TCGETS (Solaris termios)", !r.err && (tio.c_lflag & SIEOS_ICANON) && (tio.c_lflag & SIEOS_ECHO), r.err);
    tio.c_lflag &= ~(SIEOS_ICANON | SIEOS_ECHO);
    tio.c_cc[SIEOS_VMIN] = 1;
    tio.c_cc[SIEOS_VTIME] = 0;
    sys3(SIEOS_SYS_ioctl, sfd, SIEOS_TCSETS, (long)&tio);
    sys3(SIEOS_SYS_ioctl, sfd, SIEOS_TCGETS, (long)&tio);
    check("TCSETS raw mode", !(tio.c_lflag & SIEOS_ICANON) && tio.c_cc[SIEOS_VMIN] == 1, tio.c_lflag);
    while (1) {                                          /* drain the echo of "hi" */
        struct sieos_pollfd pf = { m, SIEOS_POLLIN, 0 };
        (void)pf;
        break;
    }
    wr(sfd, "out", 3);
    msleep(20);
    char mb[64] = { 0 };
    n = rd(m, mb, sizeof(mb));
    int has_out = 0;
    for (int i = 0; i + 2 < n; i++)
        if (mb[i] == 'o' && mb[i + 1] == 'u' && mb[i + 2] == 't')
            has_out = 1;
    check("slave -> master", has_out, n);
    struct sieos_winsize ws = { 30, 100, 0, 0 }, ws2 = { 0 };
    sys3(SIEOS_SYS_ioctl, m, SIEOS_TIOCSWINSZ, (long)&ws);
    sys3(SIEOS_SYS_ioctl, sfd, SIEOS_TIOCGWINSZ, (long)&ws2);
    check("window size master -> slave", ws2.ws_row == 30 && ws2.ws_col == 100, ws2.ws_col);
    sys1(SIEOS_SYS_close, sfd);
    sys1(SIEOS_SYS_close, m);
    check("/dev/pts/N goes with the master", stat_(SIEOS_AT_FDCWD, pts, &st, 0) == -SIEOS_ENOENT, 0);

    /* clean up */
    sys3(SIEOS_SYS_unlinkat, dfd, (long)"reg", 0);
    sys3(SIEOS_SYS_unlinkat, dfd, (long)"h", 0);
    sys3(SIEOS_SYS_unlinkat, dfd, (long)"l", 0);
    sys3(SIEOS_SYS_unlinkat, dfd, (long)"loop", 0);
    sys1(SIEOS_SYS_close, dfd);
    sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)"/tmp/dl", 0);
    sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)"/tmp/up", 0);
    sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)"/tmp/io", 0);
    r = sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)"/tmp/d", SIEOS_AT_REMOVEDIR);
    check("cleanup: rmdir", !r.err, r.err);
}

/* ---------------- milestone 6: time, resources, processors, sockets ---------------- */

static long hrt(void) { return sys0(SIEOS_SYS_gethrtime).val; }
static void spin_ms(long ms)
{
    long end = hrt() + ms * 1000000L;
    while (hrt() < end)
        ;
}
static void set_ign(int sig)
{
    struct sieos_sigaction sa;
    for (unsigned i = 0; i < sizeof(sa); i++)
        ((char *)&sa)[i] = 0;
    sa.__sa_u.sa_handler = SIEOS_SIG_IGN;
    sys3(SIEOS_SYS_sigaction, sig, (long)&sa, 0);
}
static void rlimit_(int res, unsigned long cur, unsigned long max)
{
    struct sieos_rlimit rl = { cur, max };
    sys2(SIEOS_SYS_setrlimit, res, (long)&rl);
}

static void fsize_child(long arg)
{
    (void)arg;
    set_ign(SIEOS_SIGXFSZ);
    rlimit_(SIEOS_RLIMIT_FSIZE, 100, 100);
    long fd = open_(SIEOS_AT_FDCWD, "/tmp/fsz", SIEOS_O_WRONLY | SIEOS_O_CREAT | SIEOS_O_TRUNC, 0644);
    static char b[200];
    long w1 = wr(fd, b, 200), w2 = wr(fd, b, 10);
    sys1(SIEOS_SYS_exit, w1 == 100 && w2 == -SIEOS_EFBIG ? 0 : 1);
}

static void cpu_child(long arg)
{
    (void)arg;
    set_handler(SIEOS_SIGXCPU, 0);
    got_sig = 0;
    rlimit_(SIEOS_RLIMIT_CPU, 1, 5);
    long end = hrt() + 4000000000L;
    while (!got_sig && hrt() < end)
        ;
    sys1(SIEOS_SYS_exit, got_sig == SIEOS_SIGXCPU ? 0 : 1);
}

static void busy_child(long arg)
{
    spin_ms(arg);
}

static unsigned short htons_(unsigned short v) { return (unsigned short)((v >> 8) | (v << 8)); }

static void test_m6(void)
{
    puts_("time, resources, processors, sockets (milestone 6)\n");
    struct sc r;

    /* high-resolution time */
    long t1 = 0, t2 = 0, best = 1L << 60;
    int mono_ok = 1;
    for (int i = 0; i < 20; i++) {                   /* the smallest gap: no preemption in between */
        t1 = hrt();
        t2 = hrt();
        if (t2 < t1)
            mono_ok = 0;
        if (t2 - t1 < best)
            best = t2 - t1;
    }
    msleep(50);
    long t3 = hrt();
    check("gethrtime is monotonic with sub-ms resolution", mono_ok && best < 1000000, best);
    check("gethrtime across a 50 ms sleep", t3 - t2 >= 40000000L && t3 - t2 < 500000000L, t3 - t2);
    struct sieos_timespec ts, res;
    sys2(SIEOS_SYS_clock_gettime, SIEOS_CLOCK_HIGHRES, (long)&ts);
    long mono = ts.tv_sec * 1000000000L + ts.tv_nsec, h = hrt();
    check("CLOCK_HIGHRES = gethrtime", h >= mono && h - mono < 10000000L, h - mono);
    r = sys2(SIEOS_SYS_clock_getres, SIEOS_CLOCK_MONOTONIC, (long)&res);
    check("clock_getres(MONOTONIC) = 1 ns", !r.err && res.tv_sec == 0 && res.tv_nsec == 1, res.tv_nsec);
    sys2(SIEOS_SYS_clock_gettime, SIEOS_CLOCK_REALTIME, (long)&ts);
    struct sieos_timespec ts2;
    msleep(20);
    sys2(SIEOS_SYS_clock_gettime, SIEOS_CLOCK_REALTIME, (long)&ts2);
    long dr = (ts2.tv_sec - ts.tv_sec) * 1000000000L + ts2.tv_nsec - ts.tv_nsec;
    check("CLOCK_REALTIME advances with ns precision", ts.tv_sec > 1600000000L && dr > 10000000L && dr < 400000000L, dr);
    r = sys2(SIEOS_SYS_clock_settime, SIEOS_CLOCK_REALTIME, (long)&ts2);
    check("clock_settime(REALTIME) as root", !r.err, r.err);
    struct sieos_timespec c1, c2;
    sys2(SIEOS_SYS_clock_gettime, SIEOS_CLOCK_PROCESS_CPUTIME_ID, (long)&c1);
    spin_ms(60);
    sys2(SIEOS_SYS_clock_gettime, SIEOS_CLOCK_PROCESS_CPUTIME_ID, (long)&c2);
    long dc = (c2.tv_sec - c1.tv_sec) * 1000000000L + c2.tv_nsec - c1.tv_nsec;
    check("CLOCK_PROCESS_CPUTIME_ID counts CPU time", dc >= 20000000L, dc);
    struct sieos_timeval d = { 0, 100000 }, od = { 1, 1 };
    sys2(SIEOS_SYS_adjtime, (long)&d, 0);
    r = sys2(SIEOS_SYS_adjtime, 0, (long)&od);
    check("adjtime: pending adjustment reported", !r.err && od.tv_sec == 0 && od.tv_usec > 50000 && od.tv_usec <= 100000,
          od.tv_usec);
    struct sieos_timeval zero = { 0, 0 };
    sys2(SIEOS_SYS_adjtime, (long)&zero, 0);

    /* interval timers */
    set_handler(SIEOS_SIGALRM, 0);
    got_sig = 0;
    struct sieos_itimerval it = { { 0, 0 }, { 0, 50000 } }, cur;
    r = sys3(SIEOS_SYS_setitimer, SIEOS_ITIMER_REAL, (long)&it, 0);
    sys2(SIEOS_SYS_getitimer, SIEOS_ITIMER_REAL, (long)&cur);
    int pending = cur.it_value.tv_usec > 0 && cur.it_value.tv_usec <= 50000;
    long start = hrt();
    while (!got_sig && hrt() - start < 1000000000L)
        msleep(5);
    long took = hrt() - start;
    check("ITIMER_REAL: SIGALRM after 50 ms", !r.err && pending && got_sig == SIEOS_SIGALRM &&
          took >= 30000000L && took < 500000000L, took);
    set_handler(SIEOS_SIGVTALRM, 0);
    got_sig = 0;
    struct sieos_itimerval vt = { { 0, 0 }, { 0, 30000 } };
    sys3(SIEOS_SYS_setitimer, SIEOS_ITIMER_VIRTUAL, (long)&vt, 0);
    start = hrt();
    while (!got_sig && hrt() - start < 2000000000L)
        ;
    check("ITIMER_VIRTUAL: SIGVTALRM after user CPU time", got_sig == SIEOS_SIGVTALRM, got_sig);
    set_handler(SIEOS_SIGPROF, 0);
    got_sig = 0;
    int nprof = 0;
    struct sieos_itimerval pt = { { 0, 20000 }, { 0, 20000 } };
    sys3(SIEOS_SYS_setitimer, SIEOS_ITIMER_PROF, (long)&pt, 0);
    start = hrt();
    while (nprof < 3 && hrt() - start < 3000000000L)
        if (got_sig) {
            nprof++;
            got_sig = 0;
        }
    struct sieos_itimerval off = { { 0, 0 }, { 0, 0 } };
    sys3(SIEOS_SYS_setitimer, SIEOS_ITIMER_PROF, (long)&off, 0);
    check("ITIMER_PROF with a reload interval", nprof == 3, nprof);
    struct sieos_tms tm;
    r = sys1(SIEOS_SYS_times, (long)&tm);
    check("times", r.val > 0 && tm.tms_utime > 0, tm.tms_utime);

    /* resource limits */
    struct sieos_rlimit rl;
    r = sys2(SIEOS_SYS_getrlimit, SIEOS_RLIMIT_NOFILE, (long)&rl);
    check("getrlimit(NOFILE)", !r.err && rl.rlim_cur == 64, rl.rlim_cur);
    struct sieos_rlimit bad = { 10, 5 };
    r = sys2(SIEOS_SYS_setrlimit, SIEOS_RLIMIT_NOFILE, (long)&bad);
    check("setrlimit cur > max: EINVAL", r.err == SIEOS_EINVAL, r.err);
    rlimit_(SIEOS_RLIMIT_NOFILE, 8, 64);
    long fds[10], nf = 0, lastfd = 0;
    for (int i = 0; i < 10; i++) {
        long f = open_(SIEOS_AT_FDCWD, "/etc/passwd", SIEOS_O_RDONLY, 0);
        if (f < 0) {
            lastfd = f;
            break;
        }
        fds[nf++] = f;
    }
    for (int i = 0; i < nf; i++)
        sys1(SIEOS_SYS_close, fds[i]);
    rlimit_(SIEOS_RLIMIT_NOFILE, 64, 64);
    check("RLIMIT_NOFILE limits descriptors", lastfd == -SIEOS_EMFILE && nf < 8, nf);
    check("sysconfig(OPEN_FILES) follows RLIMIT_NOFILE", sys1(SIEOS_SYS_sysconfig, SIEOS_CONFIG_OPEN_FILES).val == 64, 0);
    int st, code = in_child(fsize_child, 0, &st);
    check("RLIMIT_FSIZE: short write, then EFBIG", code == SIEOS_CLD_EXITED && st == 0, st);
    sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)"/tmp/fsz", 0);
    code = in_child(cpu_child, 0, &st);
    check("RLIMIT_CPU: SIGXCPU", code == SIEOS_CLD_EXITED && st == 0, st);
    long brk0 = sys1(SIEOS_SYS_brk, 0).val;
    rlimit_(SIEOS_RLIMIT_DATA, 1 << 20, SIEOS_RLIM_INFINITY);
    r = sys1(SIEOS_SYS_brk, brk0 + (4L << 20));
    rlimit_(SIEOS_RLIMIT_DATA, SIEOS_RLIM_INFINITY, SIEOS_RLIM_INFINITY);
    check("RLIMIT_DATA limits brk", r.err == SIEOS_ENOMEM, r.err);

    /* usage */
    struct sieos_rusage ru;
    r = sys2(SIEOS_SYS_getrusage, SIEOS_RUSAGE_SELF, (long)&ru);
    check("getrusage(SELF)", !r.err && (ru.ru_utime.tv_sec > 0 || ru.ru_utime.tv_usec > 0) && ru.ru_maxrss > 0 &&
          ru.ru_minflt > 0 && ru.ru_nvcsw > 0, ru.ru_utime.tv_usec);
    struct sieos_rusage rc0, rc1;
    sys2(SIEOS_SYS_getrusage, SIEOS_RUSAGE_CHILDREN, (long)&rc0);
    in_child(busy_child, 80, &st);
    sys2(SIEOS_SYS_getrusage, SIEOS_RUSAGE_CHILDREN, (long)&rc1);
    long cu0 = rc0.ru_utime.tv_sec * 1000000 + rc0.ru_utime.tv_usec + rc0.ru_stime.tv_sec * 1000000 + rc0.ru_stime.tv_usec;
    long cu1 = rc1.ru_utime.tv_sec * 1000000 + rc1.ru_utime.tv_usec + rc1.ru_stime.tv_sec * 1000000 + rc1.ru_stime.tv_usec;
    check("getrusage(CHILDREN) includes a waited-for child", cu1 - cu0 >= 40000, cu1 - cu0);
    r = sys2(SIEOS_SYS_getrusage, SIEOS_RUSAGE_LWP, (long)&ru);
    check("getrusage(LWP)", !r.err && (ru.ru_utime.tv_sec || ru.ru_utime.tv_usec), r.err);

    /* pollsys */
    int pfd[2];
    sys2(SIEOS_SYS_pipe2, (long)pfd, 0);
    struct sieos_pollfd pf = { pfd[0], SIEOS_POLLIN, 0 };
    struct sieos_timespec pto = { 0, 50000000 };
    start = hrt();
    r = sys4(SIEOS_SYS_pollsys, (long)&pf, 1, (long)&pto, 0);
    took = hrt() - start;
    check("pollsys times out", !r.err && r.val == 0 && took >= 40000000L, took);
    wr(pfd[1], "x", 1);
    r = sys4(SIEOS_SYS_pollsys, (long)&pf, 1, (long)&pto, 0);
    check("pollsys: POLLIN", r.val == 1 && (pf.revents & SIEOS_POLLIN), pf.revents);
    set_handler(SIEOS_SIGUSR1, 0);
    got_sig = 0;
    sieos_sigset_t blk, empty, now;
    sigset_one(&blk, SIEOS_SIGUSR1);
    for (int i = 0; i < 4; i++)
        empty.__sigbits[i] = 0;
    sys3(SIEOS_SYS_lwp_sigmask, SIEOS_SIG_BLOCK, (long)&blk, 0);
    sys2(SIEOS_SYS_kill, sys0(SIEOS_SYS_getpid).val, SIEOS_SIGUSR1);
    int still_pending = !got_sig;
    rd(pfd[0], &st, 1);
    struct sieos_timespec longto = { 2, 0 };
    r = sys4(SIEOS_SYS_pollsys, (long)&pf, 1, (long)&longto, (long)&empty);
    sys3(SIEOS_SYS_lwp_sigmask, SIEOS_SIG_BLOCK, 0, (long)&now);
    check("pollsys: signal mask applied during the wait, then restored",
          still_pending && r.err == SIEOS_EINTR && got_sig == SIEOS_SIGUSR1 && (now.__sigbits[0] & blk.__sigbits[0]),
          r.err);
    sys3(SIEOS_SYS_lwp_sigmask, SIEOS_SIG_SETMASK, (long)&empty, 0);
    sys1(SIEOS_SYS_close, pfd[0]);
    sys1(SIEOS_SYS_close, pfd[1]);

    /* system configuration */
    check("sysconfig(PAGESIZE, CLK_TCK, NPROC_ONLN, PHYS_PAGES)",
          sys1(SIEOS_SYS_sysconfig, SIEOS_CONFIG_PAGESIZE).val == 4096 &&
          sys1(SIEOS_SYS_sysconfig, SIEOS_CONFIG_CLK_TCK).val == 100 &&
          sys1(SIEOS_SYS_sysconfig, SIEOS_CONFIG_NPROC_ONLN).val >= 1 &&
          sys1(SIEOS_SYS_sysconfig, SIEOS_CONFIG_PHYS_PAGES).val > 1000, 0);
    check("sysconfig(unknown): EINVAL", sys1(SIEOS_SYS_sysconfig, 9999).err == SIEOS_EINVAL, 0);
    char hn[64];
    r = sys3(SIEOS_SYS_sysinfo, SIEOS_SI_SET_HOSTNAME, (long)"testhost", 0);
    sys3(SIEOS_SYS_sysinfo, SIEOS_SI_HOSTNAME, (long)hn, sizeof(hn));
    check("sysinfo(SI_SET_HOSTNAME)", !r.err && seq(hn, "testhost"), r.err);
    sys3(SIEOS_SYS_sysinfo, SIEOS_SI_SET_HOSTNAME, (long)"sieos", 0);
    check("uadmin(unknown cmd): EINVAL", sys3(SIEOS_SYS_uadmin, 99, 0, 0).err == SIEOS_EINVAL, 0);

    /* processors */
    sieos_processor_info_t pi;
    r = sys2(SIEOS_SYS_processor_info, 0, (long)&pi);
    check("processor_info(0)", !r.err && pi.pi_state == SIEOS_P_ONLINE && pi.pi_clock > 0 &&
          seq(pi.pi_processor_type, "i386"), pi.pi_clock);
    check("p_online(P_STATUS)", sys2(SIEOS_SYS_p_online, 0, SIEOS_P_STATUS).val == SIEOS_P_ONLINE, 0);
    long ncpu = sys1(SIEOS_SYS_sysconfig, SIEOS_CONFIG_NPROC_CONF).val;
    if (ncpu >= 2) {
        r = sys2(SIEOS_SYS_p_online, 1, SIEOS_P_OFFLINE);
        long stat1 = sys2(SIEOS_SYS_p_online, 1, SIEOS_P_STATUS).val;
        long onl = sys1(SIEOS_SYS_sysconfig, SIEOS_CONFIG_NPROC_ONLN).val;
        int ob;
        struct sc b = sys4(SIEOS_SYS_processor_bind, SIEOS_P_LWPID, SIEOS_P_MYID, 1, (long)&ob);
        sys2(SIEOS_SYS_p_online, 1, SIEOS_P_ONLINE);
        check("p_online(P_OFFLINE / P_ONLINE)", r.val == SIEOS_P_ONLINE && stat1 == SIEOS_P_OFFLINE &&
              onl == ncpu - 1 && b.err == SIEOS_EINVAL, stat1);
        /* CPU 0 goes offline, then all but the last of the others; the last cannot */
        r = sys2(SIEOS_SYS_p_online, 0, SIEOS_P_OFFLINE);
        int bad = 0;
        for (long c = 1; c < ncpu - 1; c++)
            bad |= sys2(SIEOS_SYS_p_online, c, SIEOS_P_OFFLINE).err;
        struct sc r2 = sys2(SIEOS_SYS_p_online, ncpu - 1, SIEOS_P_OFFLINE);   /* would leave no CPU */
        for (long c = 0; c < ncpu; c++)
            sys2(SIEOS_SYS_p_online, c, SIEOS_P_ONLINE);
        check("the last online processor cannot go offline", !r.err && !bad && r2.err == SIEOS_EBUSY, r2.err);
    }
    int ob = 7, q = 7;
    long target = ncpu >= 2 ? 1 : 0;
    r = sys4(SIEOS_SYS_processor_bind, SIEOS_P_LWPID, SIEOS_P_MYID, target, (long)&ob);
    sys4(SIEOS_SYS_processor_bind, SIEOS_P_PID, SIEOS_P_MYID, SIEOS_PBIND_QUERY, (long)&q);
    spin_ms(30);
    sieos_lwpsinfo_t li;
    long lfd = open_(SIEOS_AT_FDCWD, "/proc/self/lwp/1/lwpsinfo", SIEOS_O_RDONLY, 0);
    rd(lfd, &li, sizeof(li));
    sys1(SIEOS_SYS_close, lfd);
    sys4(SIEOS_SYS_processor_bind, SIEOS_P_LWPID, SIEOS_P_MYID, SIEOS_PBIND_NONE, 0);
    check("processor_bind runs the LWP there", !r.err && ob == SIEOS_PBIND_NONE && q == target &&
          li.pr_onpro == target && li.pr_bindpro == target, li.pr_onpro);
    long avg[3] = { -1, -1, -1 };
    r = sys2(SIEOS_SYS_getloadavg, (long)avg, 3);
    check("getloadavg", r.val == 3 && avg[0] >= 0 && avg[2] >= 0, r.val);

    /* sockets */
    long ls = sys3(SIEOS_SYS_so_socket, SIEOS_AF_INET, SIEOS_SOCK_STREAM | SIEOS_SOCK_CLOEXEC, 0).val;
    check("so_socket(SOCK_STREAM | SOCK_CLOEXEC)", ls >= 0 && sys3(SIEOS_SYS_fcntl, ls, SIEOS_F_GETFD, 0).val == 1, ls);
    struct sieos_sockaddr_in sa = { 0 };
    sa.sin_family = SIEOS_AF_INET;
    sa.sin_port = htons_(4321);
    sa.sin_addr.s_addr = 0x0100007f;                       /* 127.0.0.1, network order */
    r = sys3(SIEOS_SYS_bind, ls, (long)&sa, sizeof(sa));
    struct sc lr = sys2(SIEOS_SYS_listen, ls, 4);
    int ival = 0;
    unsigned int ilen = sizeof(ival);
    sys6(SIEOS_SYS_getsockopt, ls, SIEOS_SOL_SOCKET, SIEOS_SO_TYPE, (long)&ival, (long)&ilen, 0);
    int acc = 0;
    ilen = sizeof(acc);
    sys6(SIEOS_SYS_getsockopt, ls, SIEOS_SOL_SOCKET, SIEOS_SO_ACCEPTCONN, (long)&acc, (long)&ilen, 0);
    check("bind, listen, SO_TYPE, SO_ACCEPTCONN", !r.err && !lr.err && ival == SIEOS_SOCK_STREAM && acc == 1, ival);
    sys3(SIEOS_SYS_fcntl, ls, SIEOS_F_SETFL, SIEOS_O_NONBLOCK);
    r = sys4(SIEOS_SYS_accept, ls, 0, 0, 0);
    check("non-blocking accept: EAGAIN", r.err == SIEOS_EAGAIN, r.err);
    sys3(SIEOS_SYS_fcntl, ls, SIEOS_F_SETFL, 0);
    long cs = sys3(SIEOS_SYS_so_socket, SIEOS_AF_INET, SIEOS_SOCK_STREAM, 0).val;
    r = sys3(SIEOS_SYS_connect, cs, (long)&sa, sizeof(sa));
    struct sieos_sockaddr_in peer;
    unsigned int plen = sizeof(peer);
    long as = sys4(SIEOS_SYS_accept, ls, (long)&peer, (long)&plen, SIEOS_SOCK_NONBLOCK).val;
    check("connect + accept over loopback", !r.err && as >= 0 && peer.sin_family == SIEOS_AF_INET, as);
    r = sys6(SIEOS_SYS_sendto, cs, (long)"ping", 4, 0, 0, 0);
    char sb[16] = { 0 };
    msleep(20);
    struct sc rr = sys6(SIEOS_SYS_recvfrom, as, (long)sb, sizeof(sb), 0, 0, 0);
    check("sendto / recvfrom (stream)", r.val == 4 && rr.val == 4 && meq(sb, "ping", 4), rr.val);
    struct sieos_iovec siov[2] = { { "ab", 2 }, { "cd", 2 } };
    struct sieos_msghdr mh = { 0 };
    mh.msg_iov = siov;
    mh.msg_iovlen = 2;
    r = sys3(SIEOS_SYS_sendmsg, as, (long)&mh, 0);
    char m1[2], m2[8] = { 0 };
    struct sieos_iovec riov[2] = { { m1, 2 }, { m2, 8 } };
    struct sieos_msghdr rh = { 0 };
    rh.msg_iov = riov;
    rh.msg_iovlen = 2;
    msleep(20);
    rr = sys3(SIEOS_SYS_recvmsg, cs, (long)&rh, SIEOS_MSG_WAITALL * 0);
    check("sendmsg / recvmsg with two buffers", r.val == 4 && rr.val == 4 && meq(m1, "ab", 2) && meq(m2, "cd", 2), rr.val);
    rr = sys6(SIEOS_SYS_recvfrom, as, (long)sb, sizeof(sb), 0, 0, 0);
    check("SOCK_NONBLOCK from accept: EAGAIN", rr.err == SIEOS_EAGAIN, rr.err);
    struct sieos_timeval rto = { 0, 100000 }, rto2 = { 0, 0 };
    sys6(SIEOS_SYS_setsockopt, cs, SIEOS_SOL_SOCKET, SIEOS_SO_RCVTIMEO, (long)&rto, sizeof(rto), 0);
    ilen = sizeof(rto2);
    sys6(SIEOS_SYS_getsockopt, cs, SIEOS_SOL_SOCKET, SIEOS_SO_RCVTIMEO, (long)&rto2, (long)&ilen, 0);
    start = hrt();
    rr = sys6(SIEOS_SYS_recvfrom, cs, (long)sb, sizeof(sb), 0, 0, 0);
    took = hrt() - start;
    check("SO_RCVTIMEO (timeval)", rto2.tv_usec == 100000 && rr.err && took >= 80000000L && took < 1000000000L, took);
    rr = sys6(SIEOS_SYS_recvfrom, cs, (long)sb, sizeof(sb), SIEOS_MSG_DONTWAIT, 0, 0);
    check("MSG_DONTWAIT: EAGAIN", rr.err == SIEOS_EAGAIN, rr.err);
    sys1(SIEOS_SYS_close, as);
    sys1(SIEOS_SYS_close, cs);
    sys1(SIEOS_SYS_close, ls);
    long us = sys3(SIEOS_SYS_so_socket, SIEOS_AF_INET, SIEOS_SOCK_DGRAM, 0).val;
    sa.sin_port = htons_(4322);
    sys3(SIEOS_SYS_bind, us, (long)&sa, sizeof(sa));
    r = sys6(SIEOS_SYS_sendto, us, (long)"dgram", 5, 0, (long)&sa, sizeof(sa));
    struct sieos_sockaddr_in from;
    unsigned int flen = sizeof(from);
    msleep(20);
    rr = sys6(SIEOS_SYS_recvfrom, us, (long)sb, sizeof(sb), 0, (long)&from, (long)&flen);
    check("SOCK_DGRAM sendto / recvfrom with address", r.val == 5 && rr.val == 5 && meq(sb, "dgram", 5) &&
          from.sin_port == htons_(4322), rr.val);
    sys1(SIEOS_SYS_close, us);
    {                                                /* AF_UNIX (milestone 10 prerequisite) */
        int sv[2] = { -1, -1 };
        struct sc sp = sys4(SIEOS_SYS_so_socketpair, SIEOS_AF_UNIX, SIEOS_SOCK_STREAM, 0, (long)sv);
        struct sc w = sys3(SIEOS_SYS_write, sv[0], (long)"unix", 4);
        char ub[8];
        struct sc rd = sys3(SIEOS_SYS_read, sv[1], (long)ub, sizeof(ub));
        check("AF_UNIX: so_socketpair, write, read", !sp.err && w.val == 4 && rd.val == 4 && meq(ub, "unix", 4),
              sp.err);
        sys1(SIEOS_SYS_close, sv[0]);
        sys1(SIEOS_SYS_close, sv[1]);
        /* AF_INET6 (milestone 19): a 32-byte sockaddr_in6; a socket bound to :: reports it */
        struct sc s6 = sys3(SIEOS_SYS_so_socket, SIEOS_AF_INET6, SIEOS_SOCK_DGRAM, 0);
        struct sieos_sockaddr_in6 a6 = { .sin6_family = SIEOS_AF_INET6, .sin6_port = 0x3930 }, g6;   /* :: port 12345 */
        struct sc b6 = sys3(SIEOS_SYS_bind, s6.val, (long)&a6, sizeof(a6));
        unsigned int gl = sizeof(g6);
        struct sc n6 = sys3(SIEOS_SYS_getsockname, s6.val, (long)&g6, (long)&gl);
        check("AF_INET6: socket, bind ::, getsockname", !s6.err && !b6.err && !n6.err && gl == 32 &&
              g6.sin6_family == SIEOS_AF_INET6 && g6.sin6_port == 0x3930, b6.err);
        sys1(SIEOS_SYS_close, s6.val);
        check("AF_INET6: SOCK_RAW needs IPPROTO_ICMPV6",
              sys3(SIEOS_SYS_so_socket, SIEOS_AF_INET6, SIEOS_SOCK_RAW, SIEOS_IPPROTO_ICMP).err == SIEOS_EPROTONOSUPPORT, 0);
    }
}

/* ---------------- milestone 7 kernel prerequisites ---------------- */

static long ipc_(long nr, long op, long a, long b, long c, long d, long e)
{
    struct sc r = sys6(nr, op, a, b, c, d, e);
    return r.err ? -r.err : r.val;
}

static void shared_file_checks(const char *label, const char *path)
{
    long fd = open_(SIEOS_AT_FDCWD, path, SIEOS_O_RDWR | SIEOS_O_CREAT | SIEOS_O_TRUNC, 0644);
    sys2(SIEOS_SYS_ftruncate, fd, 8192);
    long m = mmap_(0, 8192, SIEOS_PROT_READ | SIEOS_PROT_WRITE, SIEOS_MAP_SHARED, fd, 0);
    char *p = (char *)m;
    for (int i = 0; i < 5; i++)
        p[i] = "hello"[i];
    char b[8] = { 0 };
    sys4(SIEOS_SYS_pread, fd, (long)b, 5, 0);
    int ok1 = m > 0 && meq(b, "hello", 5);
    sys4(SIEOS_SYS_pwrite, fd, (long)"XYZ", 3, 200);
    int ok2 = meq(p + 200, "XYZ", 3);
    struct sc c = sys1(SIEOS_SYS_forkx, 0);
    if (c.val == 0 && !c.err) {
        for (int i = 0; i < 5; i++)
            p[4096 + i] = "child"[i];
        sys1(SIEOS_SYS_exit, 0);
    }
    sieos_siginfo_t si;
    sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WEXITED);
    int ok3 = meq(p + 4096, "child", 5);
    sys2(SIEOS_SYS_munmap, m, 8192);
    sys1(SIEOS_SYS_close, fd);
    fd = open_(SIEOS_AT_FDCWD, path, SIEOS_O_RDONLY, 0);
    char b2[8] = { 0 };
    sys4(SIEOS_SYS_pread, fd, (long)b2, 5, 4096);
    sys4(SIEOS_SYS_pread, fd, (long)b, 5, 0);
    sys1(SIEOS_SYS_close, fd);
    sys3(SIEOS_SYS_unlinkat, SIEOS_AT_FDCWD, (long)path, 0);
    puts_("  ");
    puts_(label);
    puts_(":\n");
    check("  MAP_SHARED store seen by read()", ok1, 0);
    check("  write() seen through the mapping", ok2, 0);
    check("  child's store seen by the parent", ok3, 0);
    check("  stores reach the file after munmap", meq(b2, "child", 5) && meq(b, "hello", 5), 0);
}

static void test_m7_kernel(void)
{
    puts_("shared mappings and System V IPC (milestone 7)\n");
    struct sieos_stat st;
    check("/dev/shm is tmpfs", stat_(SIEOS_AT_FDCWD, "/dev/shm", &st, 0) == 0 && seq(st.st_fstype, "tmpfs") &&
          (st.st_mode & 07777) == 01777, st.st_mode);
    shared_file_checks("ext4", "/root/shared.dat");
    shared_file_checks("tmpfs (/dev/shm)", "/dev/shm/shared.dat");
    long rfd = open_(SIEOS_AT_FDCWD, "/etc/passwd", SIEOS_O_RDONLY, 0);
    check("MAP_SHARED|PROT_WRITE on a read-only descriptor: EACCES",
          mmap_(0, 4096, SIEOS_PROT_READ | SIEOS_PROT_WRITE, SIEOS_MAP_SHARED, rfd, 0) == -SIEOS_EACCES, 0);
    sys1(SIEOS_SYS_close, rfd);

    /* message queues */
    struct { long type; char text[16]; } msg;
    long q = ipc_(SIEOS_SYS_msgsys, SIEOS_MSGGET, SIEOS_IPC_PRIVATE, 0600, 0, 0, 0);
    msg.type = 2; msg.text[0] = 'b';
    long s1 = ipc_(SIEOS_SYS_msgsys, SIEOS_MSGSND, q, (long)&msg, 1, 0, 0);
    msg.type = 1; msg.text[0] = 'a';
    ipc_(SIEOS_SYS_msgsys, SIEOS_MSGSND, q, (long)&msg, 1, 0, 0);
    struct sieos_msqid_ds qd;
    ipc_(SIEOS_SYS_msgsys, SIEOS_MSGCTL, q, SIEOS_IPC_STAT, (long)&qd, 0, 0);
    check("msgget / msgsnd / IPC_STAT", q >= 0 && s1 == 0 && qd.msg_qnum == 2 && qd.msg_cbytes == 2, qd.msg_qnum);
    long n = ipc_(SIEOS_SYS_msgsys, SIEOS_MSGRCV, q, (long)&msg, sizeof(msg.text), 1, 0);
    check("msgrcv by type", n == 1 && msg.type == 1 && msg.text[0] == 'a', n);
    n = ipc_(SIEOS_SYS_msgsys, SIEOS_MSGRCV, q, (long)&msg, sizeof(msg.text), 0, 0);
    long n2 = ipc_(SIEOS_SYS_msgsys, SIEOS_MSGRCV, q, (long)&msg, sizeof(msg.text), 0, SIEOS_IPC_NOWAIT);
    check("msgrcv FIFO, then ENOMSG", n == 1 && msg.text[0] == 'b' && n2 == -SIEOS_ENOMSG, n2);
    msg.type = 5;
    for (int i = 0; i < 10; i++)
        msg.text[i] = '0' + i;
    ipc_(SIEOS_SYS_msgsys, SIEOS_MSGSND, q, (long)&msg, 10, 0, 0);
    n = ipc_(SIEOS_SYS_msgsys, SIEOS_MSGRCV, q, (long)&msg, 4, 0, 0);
    n2 = ipc_(SIEOS_SYS_msgsys, SIEOS_MSGRCV, q, (long)&msg, 4, 0, SIEOS_MSG_NOERROR);
    check("E2BIG, then MSG_NOERROR truncates", n == -SIEOS_E2BIG && n2 == 4, n);
    struct sc c = sys1(SIEOS_SYS_forkx, 0);
    if (c.val == 0 && !c.err) {
        long r = ipc_(SIEOS_SYS_msgsys, SIEOS_MSGRCV, q, (long)&msg, sizeof(msg.text), 7, 0);
        sys1(SIEOS_SYS_exit, r == 3 && msg.text[0] == 'x' ? 0 : 1);
    }
    msleep(50);
    msg.type = 7; msg.text[0] = 'x';
    ipc_(SIEOS_SYS_msgsys, SIEOS_MSGSND, q, (long)&msg, 3, 0, 0);
    sieos_siginfo_t si;
    sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WEXITED);
    check("a blocked msgrcv wakes on msgsnd", si.__data.proc.status == 0, si.__data.proc.status);
    ipc_(SIEOS_SYS_msgsys, SIEOS_MSGCTL, q, SIEOS_IPC_RMID, 0, 0, 0);
    check("IPC_RMID: the id is gone", ipc_(SIEOS_SYS_msgsys, SIEOS_MSGSND, q, (long)&msg, 1, 0, 0) == -SIEOS_EINVAL, 0);
    long k1 = ipc_(SIEOS_SYS_msgsys, SIEOS_MSGGET, 0x5eed, SIEOS_IPC_CREAT | 0600, 0, 0, 0);
    long k2 = ipc_(SIEOS_SYS_msgsys, SIEOS_MSGGET, 0x5eed, 0600, 0, 0, 0);
    long k3 = ipc_(SIEOS_SYS_msgsys, SIEOS_MSGGET, 0x5eed, SIEOS_IPC_CREAT | SIEOS_IPC_EXCL | 0600, 0, 0, 0);
    ipc_(SIEOS_SYS_msgsys, SIEOS_MSGCTL, k1, SIEOS_IPC_RMID, 0, 0, 0);
    check("keys: IPC_CREAT, lookup, IPC_EXCL", k1 >= 0 && k2 == k1 && k3 == -SIEOS_EEXIST, k3);

    /* semaphores */
    long sm = ipc_(SIEOS_SYS_semsys, SIEOS_SEMGET, SIEOS_IPC_PRIVATE, 2, 0600, 0, 0);
    ipc_(SIEOS_SYS_semsys, SIEOS_SEMCTL, sm, 0, SIEOS_SETVAL, 1, 0);
    struct sieos_sembuf dn = { 0, -1, 0 }, dnw = { 0, -1, SIEOS_IPC_NOWAIT }, up = { 0, 1, 0 };
    long r1 = ipc_(SIEOS_SYS_semsys, SIEOS_SEMOP, sm, (long)&dn, 1, 0, 0);
    long r2 = ipc_(SIEOS_SYS_semsys, SIEOS_SEMOP, sm, (long)&dnw, 1, 0, 0);
    check("semop: P, then EAGAIN with IPC_NOWAIT", sm >= 0 && r1 == 0 && r2 == -SIEOS_EAGAIN &&
          ipc_(SIEOS_SYS_semsys, SIEOS_SEMCTL, sm, 0, SIEOS_GETVAL, 0, 0) == 0, r2);
    struct sieos_timespec to = { 0, 50000000 };
    long t0 = hrt();
    r1 = ipc_(SIEOS_SYS_semsys, SIEOS_SEMTIMEDOP, sm, (long)&dn, 1, (long)&to, 0);
    long took = hrt() - t0;
    check("semtimedop times out", r1 == -SIEOS_EAGAIN && took >= 40000000L, took);
    c = sys1(SIEOS_SYS_forkx, 0);
    if (c.val == 0 && !c.err) {
        long r = ipc_(SIEOS_SYS_semsys, SIEOS_SEMOP, sm, (long)&dn, 1, 0, 0);    /* waits for the V */
        struct sieos_sembuf u2 = { 1, 5, SIEOS_SEM_UNDO };
        ipc_(SIEOS_SYS_semsys, SIEOS_SEMOP, sm, (long)&u2, 1, 0, 0);
        sys1(SIEOS_SYS_exit, r == 0 ? 0 : 1);
    }
    msleep(50);
    long ncnt = ipc_(SIEOS_SYS_semsys, SIEOS_SEMCTL, sm, 0, SIEOS_GETNCNT, 0, 0);
    ipc_(SIEOS_SYS_semsys, SIEOS_SEMOP, sm, (long)&up, 1, 0, 0);
    sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WEXITED);
    check("a blocked semop wakes on V; GETNCNT", si.__data.proc.status == 0 && ncnt == 1, ncnt);
    check("SEM_UNDO is applied at exit", ipc_(SIEOS_SYS_semsys, SIEOS_SEMCTL, sm, 1, SIEOS_GETVAL, 0, 0) == 0, 0);
    unsigned short all[2] = { 3, 4 }, got[2] = { 0, 0 };
    ipc_(SIEOS_SYS_semsys, SIEOS_SEMCTL, sm, 0, SIEOS_SETALL, (long)all, 0);
    ipc_(SIEOS_SYS_semsys, SIEOS_SEMCTL, sm, 0, SIEOS_GETALL, (long)got, 0);
    struct sieos_semid_ds sd;
    ipc_(SIEOS_SYS_semsys, SIEOS_SEMCTL, sm, 0, SIEOS_IPC_STAT, (long)&sd, 0);
    check("SETALL / GETALL / IPC_STAT", got[0] == 3 && got[1] == 4 && sd.sem_nsems == 2, got[0]);
    ipc_(SIEOS_SYS_semsys, SIEOS_SEMCTL, sm, 0, SIEOS_IPC_RMID, 0, 0);

    /* shared memory */
    long sh = ipc_(SIEOS_SYS_shmsys, SIEOS_SHMGET, SIEOS_IPC_PRIVATE, 8192, 0600, 0, 0);
    long at = ipc_(SIEOS_SYS_shmsys, SIEOS_SHMAT, sh, 0, 0, 0, 0);
    char *sp = (char *)at;
    sp[0] = 'P';
    c = sys1(SIEOS_SYS_forkx, 0);
    if (c.val == 0 && !c.err) {
        sp[4096] = 'C';
        sys1(SIEOS_SYS_exit, sp[0] == 'P' ? 0 : 1);
    }
    sys4(SIEOS_SYS_waitid, SIEOS_P_PID, c.val, (long)&si, SIEOS_WEXITED);
    struct sieos_shmid_ds shd;
    ipc_(SIEOS_SYS_shmsys, SIEOS_SHMCTL, sh, SIEOS_IPC_STAT, (long)&shd, 0, 0);
    check("shmget / shmat shared across fork", sh >= 0 && at > 0 && sp[4096] == 'C' && si.__data.proc.status == 0 &&
          shd.shm_nattch == 1 && shd.shm_segsz == 8192, shd.shm_nattch);
    long at2 = ipc_(SIEOS_SYS_shmsys, SIEOS_SHMAT, sh, 0, SIEOS_SHM_RDONLY, 0, 0);
    check("second attach sees the data", at2 > 0 && at2 != at && ((char *)at2)[4096] == 'C', at2);
    ipc_(SIEOS_SYS_shmsys, SIEOS_SHMCTL, sh, SIEOS_IPC_RMID, 0, 0, 0);
    sp[1] = 'x';                                        /* still attached: still usable */
    ipc_(SIEOS_SYS_shmsys, SIEOS_SHMDT, at, 0, 0, 0, 0);
    ipc_(SIEOS_SYS_shmsys, SIEOS_SHMDT, at2, 0, 0, 0, 0);
    check("IPC_RMID destroys at the last shmdt", ipc_(SIEOS_SYS_shmsys, SIEOS_SHMAT, sh, 0, 0, 0, 0) == -SIEOS_EINVAL, 0);
}

static int test_main(void)
{
    puts_("ABI v2 self-test (milestones 2-7)\n");
    test_startup();
    test_convention();
    test_files();
    test_time_info();
    test_brk();
    test_fpu();
    test_nx();
    test_lwps();
    test_signals();
    test_wait();
    test_memory();
    test_vfs();
    test_m6();
    test_m7_kernel();
    puts_(failed ? "FAILED: " : "all tests passed: ");
    putnum(passed);
    puts_(" passed, ");
    putnum(failed);
    puts_(" failed\n");
    return failed ? 1 : 0;
}
