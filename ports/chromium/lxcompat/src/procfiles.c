/*
 * procfiles.c - Linux's /proc and /sys text files, made from SIEOS's
 * Solaris interfaces when a program opens them.
 *
 *   /proc/cpuinfo            cpuid, processor_info(2), sysconf
 *   /proc/meminfo            sysinfo (the C library's, from sysconfig)
 *   /proc/stat, uptime, loadavg, version
 *   /proc/sys/kernel/random/boot_id, uuid; /proc/sys/fs/inotify/max_user_*
 *   /proc/PID/stat, statm, status, comm, cmdline
 *                            /proc/PID/psinfo, status and usage (Solaris's)
 *   /proc/self/environ, auxv the process's own initial stack (psinfo's
 *                            pr_envp: the environment, then the aux vector)
 *   /proc/self/maps          the loaded objects (dl_iterate_phdr), the heap
 *                            and the stack (pstatus): not anonymous mappings
 *   /proc/PID/exe            getexecname (the process's own only)
 *   /proc/PID/task/          /proc/PID/lwp/: the LWP ids, which gettid gives
 *   /sys/devices/system/cpu/ online, possible, present, kernel_max, and
 *                            cpuN/cpufreq/{cpuinfo_max_freq,scaling_cur_freq}
 *
 * PID is "self", "thread-self" or a number; /proc/PID/task/TID/ has the
 * process's stat, status and comm.  Everything else under /proc is SIEOS's
 * own (/proc/PID/fd/ is there).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "lx.h"
#include <cpuid.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdbool.h>
#include <link.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/processor.h>
#include <sys/procfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

void *lx_real(const char *name)
{
    void *f = dlsym(RTLD_NEXT, name);
    if (!f) {
        static const char msg[] = "liblxcompat: a C library function is missing\n";
        write(2, msg, sizeof msg - 1);
        abort();
    }
    return f;
}

void bmem(struct buf *b, const void *p, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->n + n + 1)
            cap *= 2;
        char *s = realloc(b->s, cap);
        if (!s)
            return;
        b->s = s;
        b->cap = cap;
    }
    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = 0;
}

void bput(struct buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof tmp) {
        bmem(b, tmp, n > 0 ? n : 0);
        return;
    }
    char *big = malloc(n + 1);
    if (!big)
        return;
    va_start(ap, fmt);
    vsnprintf(big, n + 1, fmt, ap);
    va_end(ap);
    bmem(b, big, n);
    free(big);
}

int lx_open_text(const struct buf *b, int flags)
{
    if ((flags & O_ACCMODE) != O_RDONLY) {
        errno = EACCES;
        return -1;
    }
    int fd = memfd_create("lxcompat", (flags & O_CLOEXEC) ? MFD_CLOEXEC : 0);
    if (fd < 0)
        return -1;
    for (size_t off = 0; off < b->n;) {
        ssize_t w = write(fd, b->s + off, b->n - off);
        if (w <= 0) {
            close(fd);
            errno = EIO;
            return -1;
        }
        off += w;
    }
    lseek(fd, 0, SEEK_SET);
    if (flags & O_NONBLOCK)
        fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

/* ---------------- the system ---------------- */

static int ncpu(void)
{
    long n = sysconf(_SC_NPROCESSORS_CONF);
    return n > 0 ? (int)n : 1;
}

static int ncpu_online(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

static int cpu_mhz(int cpu)
{
    processor_info_t pi;
    return processor_info(cpu, &pi) == 0 ? pi.pi_clock : 0;
}

static double uptime_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static time_t boot_time(void)
{
    return time(0) - (time_t)uptime_s();
}

static const char *const flags1_edx[32] = {
    "fpu", "vme", "de", "pse", "tsc", "msr", "pae", "mce", "cx8", "apic", 0, "sep", "mtrr", "pge", "mca",
    "cmov", "pat", "pse36", "pn", "clflush", 0, "dts", "acpi", "mmx", "fxsr", "sse", "sse2", "ss", "ht",
    "tm", "ia64", "pbe" };
static const char *const flags1_ecx[32] = {
    "pni", "pclmulqdq", "dtes64", "monitor", "ds_cpl", "vmx", "smx", "est", "tm2", "ssse3", "cid",
    "sdbg", "fma", "cx16", "xtpr", "pdcm", 0, "pcid", "dca", "sse4_1", "sse4_2", "x2apic", "movbe",
    "popcnt", "tsc_deadline_timer", "aes", "xsave", 0, "avx", "f16c", "rdrand", "hypervisor" };
static const char *const flags7_ebx[32] = {
    "fsgsbase", "tsc_adjust", "sgx", "bmi1", "hle", "avx2", 0, "smep", "bmi2", "erms", "invpcid", "rtm",
    0, 0, "mpx", 0, "avx512f", "avx512dq", "rdseed", "adx", "smap", "avx512ifma", 0, "clflushopt",
    "clwb", 0, "avx512pf", "avx512er", "avx512cd", "sha_ni", "avx512bw", "avx512vl" };
static const char *const flags7_ecx[32] = {
    0, "avx512vbmi", "umip", "pku", "ospke", "waitpkg", "avx512_vbmi2", 0, "gfni", "vaes",
    "vpclmulqdq", "avx512_vnni", "avx512_bitalg", 0, "avx512_vpopcntdq", 0, 0, 0, 0, 0, 0, 0,
    "rdpid", 0, 0, "cldemote", 0, "movdiri", "movdir64b", 0, 0, 0 };
static const char *const flags81_edx[32] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "syscall", 0, 0, 0, 0, 0, 0, 0, 0, "nx", 0, "mmxext", 0, 0,
    "fxsr_opt", "pdpe1gb", "rdtscp", 0, "lm", "3dnowext", "3dnow" };
static const char *const flags81_ecx[32] = {
    "lahf_lm", "cmp_legacy", "svm", "extapic", "cr8_legacy", "abm", "sse4a", "misalignsse",
    "3dnowprefetch", "osvw", "ibs", "xop", "skinit", "wdt", 0, "lwp", "fma4", "tce", 0, "nodeid_msr",
    0, "tbm", "topoext", 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static void put_flags(struct buf *b, unsigned v, const char *const names[32])
{
    for (int i = 0; i < 32; i++)
        if ((v >> i) & 1 && names[i])
            bput(b, " %s", names[i]);
}

static void cpuinfo(struct buf *b)
{
    unsigned a, bx, c, d, max, maxext;
    char vendor[13] = { 0 };
    __cpuid(0, max, bx, c, d);
    memcpy(vendor, &bx, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    __cpuid(0x80000000, maxext, bx, c, d);
    unsigned sig = 0, e1 = 0, c1 = 0, b7 = 0, c7 = 0, e81 = 0, c81 = 0, clflush = 64;
    __cpuid(1, sig, bx, c1, e1);
    clflush = ((bx >> 8) & 0xff) * 8;
    if (max >= 7)
        __cpuid_count(7, 0, a, b7, c7, d);
    if (maxext >= 0x80000001)
        __cpuid(0x80000001, a, bx, c81, e81);
    char brand[49] = { 0 };
    if (maxext >= 0x80000004) {
        unsigned *w = (unsigned *)brand;
        for (unsigned l = 0; l < 3; l++)
            __cpuid(0x80000002 + l, w[l * 4], w[l * 4 + 1], w[l * 4 + 2], w[l * 4 + 3]);
    }
    char *bp = brand;
    while (*bp == ' ')
        bp++;
    unsigned fam = (sig >> 8) & 0xf, model = (sig >> 4) & 0xf, step = sig & 0xf;
    if (fam == 0xf)
        fam += (sig >> 20) & 0xff;
    if (fam == 0x6 || fam >= 0xf)
        model |= ((sig >> 16) & 0xf) << 4;
    unsigned l2kb = 0, phys = 36, virt = 48;
    if (maxext >= 0x80000006) {
        __cpuid(0x80000006, a, bx, c, d);
        l2kb = c >> 16;
    }
    if (maxext >= 0x80000008) {
        __cpuid(0x80000008, a, bx, c, d);
        phys = a & 0xff;
        virt = (a >> 8) & 0xff;
    }
    int n = ncpu_online();
    for (int i = 0; i < n; i++) {
        int mhz = cpu_mhz(i);
        bput(b, "processor\t: %d\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\nmodel name\t: %s\n"
                "stepping\t: %u\ncpu MHz\t\t: %d.000\ncache size\t: %u KB\nphysical id\t: 0\nsiblings\t: %d\n"
                "core id\t\t: %d\ncpu cores\t: %d\napicid\t\t: %d\ninitial apicid\t: %d\nfpu\t\t: yes\n"
                "fpu_exception\t: yes\ncpuid level\t: %u\nwp\t\t: yes\nflags\t\t:",
             i, vendor, fam, model, bp, step, mhz, l2kb, n, i, n, i, i, max);
        put_flags(b, e1, flags1_edx);
        put_flags(b, c1, flags1_ecx);
        put_flags(b, e81, flags81_edx);
        put_flags(b, c81, flags81_ecx);
        put_flags(b, b7, flags7_ebx);
        put_flags(b, c7, flags7_ecx);
        bput(b, "\nbogomips\t: %d.00\nclflush size\t: %u\ncache_alignment\t: %u\n"
                "address sizes\t: %u bits physical, %u bits virtual\npower management:\n\n",
             mhz * 2, clflush, clflush, phys, virt);
    }
}

static void meminfo(struct buf *b)
{
    struct sysinfo si;
    sysinfo(&si);
    unsigned long unit = si.mem_unit ? si.mem_unit : 1;
    unsigned long total = si.totalram * unit / 1024, avail = si.freeram * unit / 1024;
    bput(b, "MemTotal:       %8lu kB\nMemFree:        %8lu kB\nMemAvailable:   %8lu kB\n"
            "Buffers:               0 kB\nCached:                0 kB\nSwapCached:            0 kB\n"
            "Active:         %8lu kB\nInactive:              0 kB\nActive(anon):   %8lu kB\n"
            "Inactive(anon):        0 kB\nActive(file):          0 kB\nInactive(file):        0 kB\n"
            "Unevictable:           0 kB\nMlocked:               0 kB\nSwapTotal:             0 kB\n"
            "SwapFree:              0 kB\nDirty:                 0 kB\nWriteback:             0 kB\n"
            "AnonPages:      %8lu kB\nMapped:                0 kB\nShmem:                 0 kB\n"
            "Slab:                  0 kB\nCommitLimit:    %8lu kB\nCommitted_AS:   %8lu kB\n",
         total, avail, avail, total - avail, total - avail, total - avail, total, total - avail);
}

static void stat_sys(struct buf *b)
{
    long hz = sysconf(_SC_CLK_TCK);
    double up = uptime_s();
    int n = ncpu_online();
    double avg[3] = { 0 };
    getloadavg(avg, 3);
    /* (no per-processor accounting: busy time from the load average) */
    unsigned long per = (unsigned long)(up * hz), busy = (unsigned long)(per * (avg[2] < n ? avg[2] / n : 1));
    bput(b, "cpu  %lu 0 0 %lu 0 0 0 0 0 0\n", busy * n, (per - busy) * n);
    for (int i = 0; i < n; i++)
        bput(b, "cpu%d %lu 0 0 %lu 0 0 0 0 0 0\n", i, busy, per - busy);
    bput(b, "intr 0\nctxt 0\nbtime %ld\nprocesses 0\nprocs_running %d\nprocs_blocked 0\nsoftirq 0\n",
         (long)boot_time(), (int)avg[0] + 1);
}

static void boot_id(struct buf *b)
{
    /* the same for every process until the next boot: the boot time's hash */
    uint64_t h = 0xcbf29ce484222325ULL ^ (uint64_t)boot_time();
    unsigned char u[16];
    for (int i = 0; i < 16; i++) {
        h ^= i;
        h *= 0x100000001b3ULL;
        u[i] = h >> 56;
    }
    u[6] = (u[6] & 0x0f) | 0x40;
    u[8] = (u[8] & 0x3f) | 0x80;
    bput(b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", u[0], u[1], u[2], u[3],
         u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

static void uuid(struct buf *b)
{
    unsigned char u[16];
    arc4random_buf(u, sizeof u);
    u[6] = (u[6] & 0x0f) | 0x40;
    u[8] = (u[8] & 0x3f) | 0x80;
    bput(b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", u[0], u[1], u[2], u[3],
         u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

/* ---------------- processes ---------------- */

static int read_rec(int pid, const char *name, void *rec, size_t size)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/%s", pid, name);
    int fd = REAL(open)(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = pread(fd, rec, size, 0);
    close(fd);
    if (n != (ssize_t)size) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

static unsigned long ticks(const struct timespec *t)
{
    long hz = sysconf(_SC_CLK_TCK);
    return (unsigned long)t->tv_sec * hz + (unsigned long)t->tv_nsec / (1000000000L / hz);
}

static char state_of(const psinfo_t *ps)
{
    char s = ps->pr_lwp.pr_sname;
    return s == 'O' ? 'R' : s == 'T' ? 'T' : s == 'Z' ? 'Z' : s == 'R' ? 'R' : s ? 'S' : 'S';
}

static const char *state_name(char s)
{
    switch (s) {
    case 'R': return "R (running)";
    case 'T': return "T (stopped)";
    case 'Z': return "Z (zombie)";
    default:  return "S (sleeping)";
    }
}

static int proc_stat(int pid, struct buf *b)
{
    psinfo_t ps;
    if (read_rec(pid, "psinfo", &ps, sizeof ps) < 0)
        return -1;
    pstatus_t st;
    prusage_t us;
    bool own = read_rec(pid, "status", &st, sizeof st) == 0;
    bool use = read_rec(pid, "usage", &us, sizeof us) == 0;
    if (!own)
        memset(&st, 0, sizeof st);
    if (!use)
        memset(&us, 0, sizeof us);
    long hz = sysconf(_SC_CLK_TCK);
    struct timespec bt = { boot_time(), 0 };
    long start = (ps.pr_start.tv_sec - bt.tv_sec) * hz + ps.pr_start.tv_nsec / (1000000000L / hz);
    long pg = sysconf(_SC_PAGESIZE);
    int tty = ps.pr_ttydev == (dev_t)~0 ? 0 : (int)ps.pr_ttydev;
    bput(b, "%d (%.15s) %c %d %d %d %d %d 4194304 %lu 0 %lu 0 %lu %lu %lu %lu %d %d %d 0 %ld %lu %lu "
            "18446744073709551615 0 0 %lu 0 0 0 0 0 0 0 0 17 %d 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
         ps.pr_pid, ps.pr_fname, state_of(&ps), ps.pr_ppid, ps.pr_pgid, ps.pr_sid, tty, -1, us.pr_minf,
         us.pr_majf, ticks(&st.pr_utime), ticks(&st.pr_stime), ticks(&st.pr_cutime), ticks(&st.pr_cstime),
         20 - ps.pr_lwp.pr_nice + 20, ps.pr_lwp.pr_nice - 20 > 19 ? 19 : 0, ps.pr_nlwp, start,
         (unsigned long)ps.pr_size * 1024, (unsigned long)ps.pr_rssize * 1024 / pg,
         (unsigned long)st.pr_stkbase + st.pr_stksize, sched_getcpu() > 0 ? sched_getcpu() : 0);
    return 0;
}

static int proc_statm(int pid, struct buf *b)
{
    psinfo_t ps;
    if (read_rec(pid, "psinfo", &ps, sizeof ps) < 0)
        return -1;
    long kpg = sysconf(_SC_PAGESIZE) / 1024;
    bput(b, "%lu %lu 0 0 0 %lu 0\n", (unsigned long)ps.pr_size / kpg, (unsigned long)ps.pr_rssize / kpg,
         (unsigned long)ps.pr_size / kpg);
    return 0;
}

static int proc_status(int pid, struct buf *b)
{
    psinfo_t ps;
    if (read_rec(pid, "psinfo", &ps, sizeof ps) < 0)
        return -1;
    prcred_t cr;
    if (read_rec(pid, "cred", &cr, sizeof cr) < 0) {
        memset(&cr, 0, sizeof cr);
        cr.pr_ruid = cr.pr_euid = cr.pr_suid = ps.pr_uid;
        cr.pr_rgid = cr.pr_egid = cr.pr_sgid = ps.pr_gid;
    }
    pstatus_t st;
    if (read_rec(pid, "status", &st, sizeof st) < 0)
        memset(&st, 0, sizeof st);
    bput(b, "Name:\t%.15s\nUmask:\t0022\nState:\t%s\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\n"
            "Uid:\t%d\t%d\t%d\t%d\nGid:\t%d\t%d\t%d\t%d\nFDSize:\t256\nGroups:\t",
         ps.pr_fname, state_name(state_of(&ps)), ps.pr_pid, ps.pr_pid, ps.pr_ppid, cr.pr_ruid, cr.pr_euid,
         cr.pr_suid, cr.pr_euid, cr.pr_rgid, cr.pr_egid, cr.pr_sgid, cr.pr_egid);
    for (int i = 0; i < cr.pr_ngroups && i < 16; i++)
        bput(b, "%d ", cr.pr_groups[i]);
    unsigned long stk = st.pr_stksize / 1024;
    bput(b, "\nVmPeak:\t%8lu kB\nVmSize:\t%8lu kB\nVmLck:\t       0 kB\nVmPin:\t       0 kB\n"
            "VmHWM:\t%8lu kB\nVmRSS:\t%8lu kB\nRssAnon:\t%8lu kB\nRssFile:\t       0 kB\nRssShmem:\t       0 kB\n"
            "VmData:\t%8lu kB\nVmStk:\t%8lu kB\nVmExe:\t       0 kB\nVmLib:\t       0 kB\nVmPTE:\t       0 kB\n"
            "VmSwap:\t       0 kB\nThreads:\t%d\nSigQ:\t0/0\nCpus_allowed_list:\t0-%d\n"
            "voluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n",
         (unsigned long)ps.pr_size, (unsigned long)ps.pr_size, (unsigned long)ps.pr_rssize,
         (unsigned long)ps.pr_rssize, (unsigned long)ps.pr_rssize, (unsigned long)ps.pr_size, stk, ps.pr_nlwp,
         ncpu() - 1);
    return 0;
}

static int proc_comm(int pid, struct buf *b)
{
    psinfo_t ps;
    if (read_rec(pid, "psinfo", &ps, sizeof ps) < 0)
        return -1;
    bput(b, "%.15s\n", ps.pr_fname);
    return 0;
}

static int proc_cmdline(int pid, struct buf *b)
{
    psinfo_t ps;
    if (read_rec(pid, "psinfo", &ps, sizeof ps) < 0)
        return -1;
    if (pid == getpid() && ps.pr_argv) {
        char **argv = (char **)ps.pr_argv;
        for (int i = 0; i < ps.pr_argc && argv[i]; i++)
            bmem(b, argv[i], strlen(argv[i]) + 1);
        return 0;
    }
    /* another process: its argument string (Solaris's, 80 bytes), split at blanks */
    for (char *s = ps.pr_psargs; *s; s++)
        bmem(b, *s == ' ' ? "" : s, 1);
    if (ps.pr_psargs[0])
        bmem(b, "", 1);
    return 0;
}

static char **initial_envp(void)
{
    psinfo_t ps;
    if (read_rec(getpid(), "psinfo", &ps, sizeof ps) < 0)
        return 0;
    return (char **)ps.pr_envp;
}

static int self_environ(struct buf *b)
{
    char **env = initial_envp();
    for (; env && *env; env++)
        bmem(b, *env, strlen(*env) + 1);
    return 0;
}

static int self_auxv(struct buf *b)
{
    char **env = initial_envp();
    if (!env) {
        errno = ENOENT;
        return -1;
    }
    while (*env)
        env++;
    for (Elf64_auxv_t *a = (Elf64_auxv_t *)(env + 1);; a++) {
        bmem(b, a, sizeof *a);
        if (a->a_type == AT_NULL)
            break;
    }
    return 0;
}

static const char *exe_path(char *out, size_t size)
{
    const char *e = getexecname();
    if (!e)
        return 0;
    if (e[0] == '/') {
        snprintf(out, size, "%s", e);
        return out;
    }
    char tmp[PATH_MAX];
    if (!realpath(e, tmp))
        return 0;
    snprintf(out, size, "%s", tmp);
    return out;
}

struct maps_ctx {
    struct buf *b;
};

static int maps_object(struct dl_phdr_info *info, size_t size, void *arg)
{
    (void)size;
    struct buf *b = ((struct maps_ctx *)arg)->b;
    char exe[PATH_MAX];
    const char *name = info->dlpi_name && info->dlpi_name[0] ? info->dlpi_name : exe_path(exe, sizeof exe);
    long pg = sysconf(_SC_PAGESIZE);
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD)
            continue;
        uintptr_t s = (info->dlpi_addr + ph->p_vaddr) & ~(uintptr_t)(pg - 1);
        uintptr_t e = (info->dlpi_addr + ph->p_vaddr + ph->p_memsz + pg - 1) & ~(uintptr_t)(pg - 1);
        bput(b, "%012lx-%012lx %c%c%cp %08lx 00:00 0          %s\n", (unsigned long)s, (unsigned long)e,
             ph->p_flags & PF_R ? 'r' : '-', ph->p_flags & PF_W ? 'w' : '-', ph->p_flags & PF_X ? 'x' : '-',
             (unsigned long)(ph->p_offset & ~(uint64_t)(pg - 1)), name ? name : "");
    }
    return 0;
}

static int self_maps(struct buf *b)
{
    struct maps_ctx c = { b };
    dl_iterate_phdr(maps_object, &c);
    pstatus_t st;
    if (read_rec(getpid(), "status", &st, sizeof st) == 0) {
        if (st.pr_brksize)
            bput(b, "%012lx-%012lx rw-p 00000000 00:00 0          [heap]\n", (unsigned long)st.pr_brkbase,
                 (unsigned long)(st.pr_brkbase + st.pr_brksize));
        bput(b, "%012lx-%012lx rw-p 00000000 00:00 0          [stack]\n", (unsigned long)st.pr_stkbase,
             (unsigned long)(st.pr_stkbase + st.pr_stksize));
    }
    return 0;
}

/* "/proc/PID/rest": the pid (self's for "self", "thread-self"), and rest after "PID/" ("" for the directory). */
static int parse_pid(const char *path, const char **rest)
{
    if (strncmp(path, "/proc/", 6))
        return -1;
    const char *p = path + 6, *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    int pid;
    if ((len == 4 && !strncmp(p, "self", 4)) || (len == 11 && !strncmp(p, "thread-self", 11))) {
        pid = getpid();
    } else {
        if (!len || len > 9)
            return -1;
        pid = 0;
        for (size_t i = 0; i < len; i++) {
            if (p[i] < '0' || p[i] > '9')
                return -1;
            pid = pid * 10 + (p[i] - '0');
        }
    }
    *rest = slash ? slash + 1 : "";
    if (len == 11 && !strncmp(p, "thread-self", 11)) {
        /* thread-self/X is the process's X, as task/TID/X is */
        static __thread char tbuf[64];
        snprintf(tbuf, sizeof tbuf, "task/%d/%s", (int)gettid(), *rest);
        *rest = tbuf;
    }
    return pid;
}

/* task/TID/X: X for the process; the LWP must exist. */
static const char *task_entry(int pid, const char *rest, bool *bad)
{
    *bad = false;
    if (strncmp(rest, "task/", 5))
        return rest;
    const char *t = rest + 5, *slash = strchr(t, '/');
    if (!slash)
        return rest;
    char lwp[64];
    snprintf(lwp, sizeof lwp, "/proc/%d/lwp/%.*s", pid, (int)(slash - t), t);
    if (access(lwp, F_OK) < 0)
        *bad = true;
    return slash + 1;
}

static int sys_cpu(const char *rest, struct buf *b)
{
    int n = ncpu();
    if (!strcmp(rest, "online"))
        bput(b, n > 1 ? "0-%d\n" : "0\n", ncpu_online() - 1);
    else if (!strcmp(rest, "possible") || !strcmp(rest, "present"))
        bput(b, n > 1 ? "0-%d\n" : "0\n", n - 1);
    else if (!strcmp(rest, "kernel_max"))
        bput(b, "%d\n", n - 1);
    else {
        int cpu, used = 0;
        char leaf[64];
        if (sscanf(rest, "cpu%d/cpufreq/%63s%n", &cpu, leaf, &used) != 2 || rest[used] || cpu < 0 || cpu >= n)
            return 0;
        int mhz = cpu_mhz(cpu);
        if (!mhz || (strcmp(leaf, "cpuinfo_max_freq") && strcmp(leaf, "scaling_cur_freq") &&
                     strcmp(leaf, "scaling_max_freq") && strcmp(leaf, "cpuinfo_min_freq")))
            return 0;
        bput(b, "%d\n", mhz * 1000);
    }
    return 1;
}

int lx_text(const char *path, struct buf *b)
{
    if (!strncmp(path, "/sys/devices/system/cpu/", 24))
        return sys_cpu(path + 24, b);
    if (strncmp(path, "/proc/", 6))
        return 0;
    const char *p = path + 6;
    if (!strcmp(p, "cpuinfo")) cpuinfo(b);
    else if (!strcmp(p, "meminfo")) meminfo(b);
    else if (!strcmp(p, "stat")) stat_sys(b);
    else if (!strcmp(p, "uptime")) bput(b, "%.2f %.2f\n", uptime_s(), uptime_s() * ncpu_online() / 2);
    else if (!strcmp(p, "loadavg")) {
        double a[3] = { 0 };
        getloadavg(a, 3);
        bput(b, "%.2f %.2f %.2f 1/1 %d\n", a[0], a[1], a[2], getpid());
    } else if (!strcmp(p, "version")) {
        struct utsname u;
        uname(&u);
        bput(b, "%s version %s (%s) %s\n", u.sysname, u.release, u.machine, u.version);
    } else if (!strcmp(p, "sys/kernel/random/boot_id")) boot_id(b);
    else if (!strcmp(p, "sys/kernel/random/uuid")) uuid(b);
    else if (!strcmp(p, "sys/fs/inotify/max_user_watches")) bput(b, "65536\n");
    else if (!strcmp(p, "sys/fs/inotify/max_user_instances")) bput(b, "128\n");
    else {
        const char *rest;
        int pid = parse_pid(path, &rest);
        if (pid < 0)
            return 0;
        bool bad;
        const char *e = task_entry(pid, rest, &bad);
        int r;
        if (!strcmp(e, "stat")) r = proc_stat(pid, b);
        else if (!strcmp(e, "statm")) r = proc_statm(pid, b);
        else if (!strcmp(e, "status")) {
            if (e == rest)
                return 0;                        /* (/proc/PID/status: SIEOS's own, Solaris's pstatus) */
            r = proc_status(pid, b);
        } else if (!strcmp(e, "comm")) r = proc_comm(pid, b);
        else if (!strcmp(e, "cmdline")) r = proc_cmdline(pid, b);
        else if (pid == getpid() && !strcmp(e, "environ")) r = self_environ(b);
        else if (pid == getpid() && !strcmp(e, "auxv")) r = self_auxv(b);
        else if (pid == getpid() && !strcmp(e, "maps")) r = self_maps(b);
        else return 0;
        if (bad) {
            errno = ENOENT;
            return -1;
        }
        if (r < 0) {
            errno = ENOENT;
            return -1;
        }
    }
    return 1;
}

int lx_link(const char *path, char *out, size_t size)
{
    const char *rest;
    int pid = parse_pid(path, &rest);
    if (pid < 0 || strcmp(rest, "exe"))
        return 0;
    if (pid != getpid()) {
        errno = EACCES;
        return -1;
    }
    if (!exe_path(out, size)) {
        errno = ENOENT;
        return -1;
    }
    return 1;
}

int lx_dir(const char *path, struct buf *names)
{
    if (!strcmp(path, "/sys/devices/system/cpu")) {
        int n = ncpu();
        for (int i = 0; i < n; i++) {
            char s[16];
            int k = snprintf(s, sizeof s, "cpu%d", i);
            bmem(names, s, k + 1);
        }
        bmem(names, "online", 7);
        bmem(names, "possible", 9);
        bmem(names, "present", 8);
        bmem(names, "kernel_max", 11);
        return 1;
    }
    const char *rest;
    int pid = parse_pid(path, &rest);
    if (pid < 0 || strcmp(rest, "task"))
        return 0;
    char lwp[64];
    snprintf(lwp, sizeof lwp, "/proc/%d/lwp", pid);
    DIR *d = REAL(opendir)(lwp);
    if (!d)
        return -1;
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.')
            bmem(names, e->d_name, strlen(e->d_name) + 1);
    closedir(d);
    return 1;
}
