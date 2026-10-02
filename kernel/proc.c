/*
 * proc.c - Processes and LWPs: the round-robin scheduler, fork/exec/exit,
 * waiting, process groups and sessions.
 *
 * A process (struct proc) owns the address space and the other shared
 * resources; its LWPs (struct lwp) are what the scheduler runs.  Kernel code
 * runs under the big kernel lock with interrupts disabled; an LWP gives up
 * its CPU by sleeping, or when the timer fires while it is in user mode.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "power.h"
#include "proc.h"
#include "mm.h"
#include "vm.h"
#include "abi2.h"
#include "fs.h"
#include "elf.h"
#include "tty.h"
#include "smp.h"
#include "poll.h"
#include "display.h"
#include "random.h"
#include "sieos/auxv.h"
#include "sieos/mman.h"
#include "sieos/wait.h"
#include "sieos/time.h"

struct proc proc_table[NPROC];
struct lwp lwp_table[NLWP];
static struct proc idle_proc;
static struct lwp idle_lwps[NCPU];
static int next_pid = 1;
static int sleep_chan;


void proc_init_cpu(struct cpu *c)
{
    if (!idle_proc.pml4) {
        idle_proc.pml4 = kernel_pml4_phys;
        idle_proc.state = PSTATE_RUNNING;
        strcpy(idle_proc.name, "idle");
    }
    struct lwp *idle = &idle_lwps[c->id];
    memset(idle, 0, sizeof(*idle));
    idle->state = LWP_RUNNING;
    idle->is_idle = true;
    idle->cpu = c->id;
    idle->proc = &idle_proc;
    idle->kstack = c->kstack_top - KSTACK_SIZE;
    idle->fpu = fpu_default;
    snprintf(idle->name, sizeof(idle->name), "idle/%d", c->id);
    c->idle = c->lwp = idle;
}

struct proc *proc_find(int pid)
{
    for (int i = 1; i < NPROC; i++)
        if (proc_table[i].state != PSTATE_UNUSED && proc_table[i].pid == pid)
            return &proc_table[i];
    return NULL;
}

struct lwp *lwp_find(struct proc *p, int lwpid)
{
    for (int i = 0; i < NLWP; i++)
        if (lwp_table[i].state != LWP_UNUSED && lwp_table[i].proc == p && lwp_table[i].lwpid == lwpid)
            return &lwp_table[i];
    return NULL;
}

/* PSTATE_* for ps: the most active state of the process's LWPs. */
int proc_state(struct proc *p)
{
    if (p->state != PSTATE_RUNNING)
        return p->state;
    if (p->stopped)
        return PSTATE_STOPPED;
    int best = PSTATE_SLEEPING;
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->proc != p)
            continue;
        if (l->state == LWP_RUNNING)
            return PSTATE_RUNNING;
        if (l->state == LWP_RUNNABLE)
            best = PSTATE_RUNNABLE;
    }
    return best;
}

static int alloc_pid(void)
{
    for (;;) {
        int pid = next_pid++;
        if (next_pid > 99999)
            next_pid = 2;
        bool used = false;
        for (int i = 1; i < NPROC; i++) {
            struct proc *p = &proc_table[i];
            if (p->state != PSTATE_UNUSED && (p->pid == pid || p->pgid == pid || p->sid == pid))
                used = true;
        }
        if (!used)
            return pid;
    }
}

static void default_rlimits(struct proc *p)
{
    for (int i = 0; i < 16; i++)
        p->rlim_cur[i] = p->rlim_max[i] = SIEOS_RLIM_INFINITY;
    p->rlim_cur[SIEOS_RLIMIT_NOFILE] = p->rlim_max[SIEOS_RLIMIT_NOFILE] = NOFILE;
    p->rlim_cur[SIEOS_RLIMIT_STACK] = USER_STACK_SIZE;
    p->rlim_max[SIEOS_RLIMIT_STACK] = USER_STACK_SIZE;
    p->rlim_cur[SIEOS_RLIMIT_CORE] = 0;
}

static struct proc *alloc_proc(void)
{
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &proc_table[i];
        if (p->state != PSTATE_UNUSED)
            continue;
        memset(p, 0, sizeof(*p));
        p->state = PSTATE_EMBRYO;
        p->pid = alloc_pid();
        p->start_tick = ticks;
        default_rlimits(p);
        return p;
    }
    return NULL;
}

struct lwp *lwp_alloc(struct proc *p)
{
    struct lwp *l = NULL;
    for (int i = 0; i < NLWP; i++)
        if (lwp_table[i].state == LWP_UNUSED) {
            l = &lwp_table[i];
            break;
        }
    if (!l)
        return NULL;
    uint64_t kstack_pa = pmm_alloc_contig(KSTACK_SIZE / PAGE_SIZE);
    if (!kstack_pa)
        return NULL;
    memset(l, 0, sizeof(*l));
    l->state = LWP_EMBRYO;
    l->proc = p;
    l->lwpid = ++p->next_lwpid;
    l->cpu = -1;
    l->kstack = (uint64_t)P2V(kstack_pa);
    l->fpu = fpu_default;
    l->altstack_flags = SIEOS_SS_DISABLE;
    strlcpy(l->name, p->name, sizeof(l->name));
    sched_init_lwp(l, curlwp);
    p->nlwp++;

    /* Trap frame at the top; below it a context that "returns" to forkret,
     * which releases the big kernel lock and enters user mode via trapret. */
    uint64_t sp = l->kstack + KSTACK_SIZE;
    sp -= sizeof(struct trapframe);
    l->tf = (struct trapframe *)sp;
    memset(l->tf, 0, sizeof(*l->tf));
    sp -= 8;
    *(uint64_t *)sp = (uint64_t)forkret;
    for (int i = 0; i < 6; i++) {      /* rbp rbx r12 r13 r14 r15 */
        sp -= 8;
        *(uint64_t *)sp = 0;
    }
    l->ctx_rsp = sp;
    return l;
}

static void lwp_free(struct lwp *l)
{
    pmm_free_contig(V2P(l->kstack), KSTACK_SIZE / PAGE_SIZE);
    memset(l, 0, sizeof(*l));
    l->state = LWP_UNUSED;
}

/* Free a process slot and every LWP left of it. */
static void proc_free(struct proc *p)
{
    for (int i = 0; i < NLWP; i++)
        if (lwp_table[i].state != LWP_UNUSED && lwp_table[i].proc == p)
            lwp_free(&lwp_table[i]);
    memset(p, 0, sizeof(*p));
    p->state = PSTATE_UNUSED;
}

/* First entry of a new LWP (from forkret): it may have been told to exit already. */
void lwp_first_run(void)
{
    if (curlwp->must_exit)
        lwp_exit_self();
}

/* Detached LWPs that have exited are freed here (never their own stack). */
static void reap_detached(void)
{
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->state == LWP_ZOMBIE && l->detached && l != curlwp && l->proc->state == PSTATE_RUNNING)
            lwp_free(l);
    }
}

/* ------------------------------------------------------------------ */
/* Scheduling (called with the big kernel lock held)                   */
/* ------------------------------------------------------------------ */

void schedule(void)
{
    struct cpu *c = mycpu();
    struct lwp *cur = c->lwp, *next = NULL;
    int best = -1;
    c->need_resched = false;
    for (int i = 1; i <= NLWP && !c->offline; i++) {   /* highest priority first, round robin among equals */
        int idx = (c->rr + i) % NLWP;
        struct lwp *l = &lwp_table[idx];
        if (l->state == LWP_RUNNABLE && (!l->bound || l->bound == c->id + 1) &&
            (!l->affinity || (l->affinity & (1ULL << c->id)))) {
            int pri = sched_gpri(l);
            if (pri > best) {
                best = pri;
                next = l;
            }
        }
    }
    bool cur_ok = cur->state == LWP_RUNNING && !cur->is_idle && !c->offline && (!cur->bound || cur->bound == c->id + 1) &&
                  (!cur->affinity || (cur->affinity & (1ULL << c->id)));
    if (cur_ok && (!next || sched_gpri(cur) > best || (sched_gpri(cur) == best && !c->slice_expired))) {
        c->slice_expired = false;
        return;                         /* it keeps the CPU: nobody ranks higher */
    }
    c->slice_expired = false;
    if (next)
        c->rr = next - lwp_table;
    else
        next = c->idle;
    if (next == cur)
        return;
    if (!cur->is_idle) {
        if (cur->state == LWP_RUNNING)
            cur->nivcsw++;
        else
            cur->nvcsw++;
    }
    if (cur->state == LWP_RUNNING)
        cur->state = LWP_RUNNABLE;
    next->state = LWP_RUNNING;
    next->cpu = c->id;
    if (!next->is_idle)
        tss_set_rsp0(next->kstack + KSTACK_SIZE);
    uint64_t cr3 = next->proc->pml4 ? next->proc->pml4 : kernel_pml4_phys;
    if (read_cr3() != cr3)
        write_cr3(cr3);
    fpu_save(&cur->fpu);                /* user FPU/SSE state follows the LWP */
    fpu_restore(&next->fpu);
    wrmsr(MSR_FS_BASE, next->fsbase);   /* its TLS pointer */
    c->lwp = next;
    c->slice_start = ticks;
    next->last_run = ticks;
    switch_context(&cur->ctx_rsp, next->ctx_rsp);
    /* Note: we may now be running on a different CPU than before. */
}

/* Per-CPU idle loop; entered with the big kernel lock held. */
void cpu_idle(void)
{
    struct lwp *idle = mycpu()->idle;
    for (;;) {
        idle->state = LWP_RUNNABLE;
        schedule();
        idle->state = LWP_RUNNING;
        bkl_unlock();
        power_idle();                        /* HLT, or MWAIT into a deep C-state */
        cli();
        bkl_lock();
    }
}

void sleep_on(void *chan)
{
    struct lwp *l = curlwp;
    l->chan = chan;
    l->state = LWP_SLEEPING;
    schedule();
    l->chan = NULL;
}

void make_runnable(struct lwp *l)
{
    l->state = LWP_RUNNABLE;
    sched_woke(l);
    smp_kick_idle();
}

void wakeup(void *chan)
{
    bool woke = false;
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->state == LWP_SLEEPING && l->chan == chan) {
            l->state = LWP_RUNNABLE;
            sched_woke(l);
            woke = true;
        }
    }
    if (woke)
        smp_kick_idle();
}

unsigned long loadavg[3];

/* Every 5 s: exponentially decayed run-queue length over 1, 5 and 15 minutes. */
static void update_loadavg(void)
{
    static const unsigned long e[3] = { 1884, 2014, 2037 };      /* 2048 * exp(-5/60), (-5/300), (-5/900) */
    unsigned long n = 0;
    for (int i = 0; i < NLWP; i++)
        if (lwp_table[i].state == LWP_RUNNABLE || lwp_table[i].state == LWP_RUNNING)
            n++;
    for (int k = 0; k < 3; k++)
        loadavg[k] = (loadavg[k] * e[k] + (n << 11) * (2048 - e[k])) >> 11;
}

/* n ticks have passed (usually 1). */
void timerfd_tick(void);                         /* fdext.c */

void clock_tick(uint64_t n)
{
    bool woke = false;
    timerfd_tick();                              /* (expired timerfds wake their waiters) */
    if (ticks / (5 * TIMER_HZ) != (ticks - n) / (5 * TIMER_HZ))
        update_loadavg();
    if (ticks / TIMER_HZ != (ticks - n) / TIMER_HZ)
        sched_second();
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->state == LWP_SLEEPING && l->wake_tick && l->wake_tick <= ticks) {
            l->state = LWP_RUNNABLE;
            sched_woke(l);
            woke = true;
        }
    }
    /* ITIMER_REAL counts wall-clock ticks */
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &proc_table[i];
        if (p->state != PSTATE_RUNNING || !p->itimer_value[0])
            continue;
        if (p->itimer_value[0] <= n) {
            p->itimer_value[0] = p->itimer_interval[0];
            signal_send(p, SIGALRM);
        } else {
            p->itimer_value[0] -= n;
        }
    }
    if (woke)
        smp_kick_idle();
}

void sched_tick(struct trapframe *tf)
{
    struct cpu *c = mycpu();
    struct lwp *l = c->lwp;
    if (l->is_idle) {
        c->idle_ticks++;
    } else {
        c->busy_ticks++;
        l->ticks++;
        if ((tf->cs & 3) != 3)
            l->sticks++;
        struct proc *pp = l->proc;
        pp->cpu_total++;
        uint64_t lim = pp->rlim_cur[SIEOS_RLIMIT_CPU];
        if (lim != SIEOS_RLIM_INFINITY && pp->cpu_total % TIMER_HZ == 0) {
            uint64_t secs = pp->cpu_total / TIMER_HZ;
            if (secs >= pp->rlim_max[SIEOS_RLIMIT_CPU])
                signal_send(pp, SIGKILL);
            else if (secs >= lim && (int)secs > pp->cpu_limit_sent) {
                pp->cpu_limit_sent = secs;
                signal_send(pp, SIGXCPU);
            }
        }
        /* ITIMER_VIRTUAL (user time) and ITIMER_PROF (all CPU time) */
        struct proc *p = l->proc;
        bool user = (tf->cs & 3) == 3;
        if (user && p->itimer_value[1] && --p->itimer_value[1] == 0) {
            p->itimer_value[1] = p->itimer_interval[1];
            signal_send(p, SIGVTALRM);
        }
        if (p->itimer_value[2] && --p->itimer_value[2] == 0) {
            p->itimer_value[2] = p->itimer_interval[2];
            signal_send(p, SIGPROF);
        }
    }
    uint32_t q = sched_quantum(l);
    if ((tf->cs & 3) == 3 && !l->is_idle && q && ticks - c->slice_start >= q) {
        sched_expired(l);                /* its quantum is used up */
        c->slice_expired = true;
        schedule();
    }
}

/* Wake the high-resolution sleepers that are due; returns the earliest deadline left. */
uint64_t hr_wake(uint64_t now)
{
    uint64_t next = ~0UL;
    bool woke = false;
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (!l->wake_ns)
            continue;
        if (l->wake_ns <= now) {
            if (l->state == LWP_SLEEPING) {
                l->state = LWP_RUNNABLE;
                sched_woke(l);
                woke = true;
            }
        } else if (l->wake_ns < next) {
            next = l->wake_ns;
        }
    }
    if (woke)
        smp_kick_idle();
    return next;
}

/* Sleep until hrtime reaches when; -EINTR if a signal arrives first. */
long proc_sleep_until_ns(uint64_t when)
{
    struct lwp *l = curlwp;
    l->wake_ns = when;
    lapic_timer_hint(when);
    while (hrtime() < when) {
        if (signal_pending(current)) {
            l->wake_ns = 0;
            return -EINTR;
        }
        sleep_on(&sleep_chan);
    }
    l->wake_ns = 0;
    return 0;
}

/* Sleep until the given tick; -EINTR if a signal arrives first. */
long proc_sleep_until(uint64_t wake)
{
    struct lwp *l = curlwp;
    l->wake_tick = wake;
    while (ticks < wake) {
        if (signal_pending(current)) {
            l->wake_tick = 0;
            return -EINTR;
        }
        sleep_on(&sleep_chan);
    }
    l->wake_tick = 0;
    return 0;
}


/* ------------------------------------------------------------------ */
/* Program loading                                                     */
/* ------------------------------------------------------------------ */

/* Copy bytes into a (possibly inactive) address space. */
static int copy_to_space(uint64_t pml4, uint64_t va, const void *src, size_t n)
{
    const uint8_t *s = src;
    while (n) {
        uint64_t pa = vmm_translate(pml4, va, NULL);
        if (!pa)
            return -EFAULT;
        size_t chunk = MIN(n, PAGE_SIZE - (va & 0xFFF));
        memcpy(P2V(pa), s, chunk);
        s += chunk;
        va += chunk;
        n -= chunk;
    }
    return 0;
}

#define PIE_BASE 0x400000UL                /* where static-PIE executables are loaded */

static int load_segment(uint64_t pml4, struct inode *ip, Elf64_Phdr *ph, uint64_t bias)
{
    uint64_t start = ph->p_vaddr + bias, end = start + ph->p_memsz;
    if (ph->p_memsz < ph->p_filesz || start < USER_BASE || end > USER_STACK_TOP - USER_STACK_SIZE || end < start)
        return -ENOEXEC;
    bool w = ph->p_flags & PF_W, x = ph->p_flags & PF_X;
    uint64_t flags = PTE_U | (w ? PTE_W : 0) | (x ? 0 : pte_nx);
    /* pages already mapped by a previous segment get the union of both permissions */
    for (uint64_t va = PAGE_ALIGN_DOWN(start); va < end; va += PAGE_SIZE)
        if (vmm_translate(pml4, va, NULL))
            vmm_widen(pml4, va, w, x);
    int r = vmm_alloc_range(pml4, start, end, flags);
    if (r < 0)
        return r;
    uint64_t va = start, off = ph->p_offset, left = ph->p_filesz;
    while (left) {
        uint64_t pa = vmm_translate(pml4, va, NULL);
        size_t chunk = MIN(left, PAGE_SIZE - (va & 0xFFF));
        if (readi(ip, P2V(pa), off, chunk) != (long)chunk)
            return -EIO;
        va += chunk;
        off += chunk;
        left -= chunk;
    }
    return 0;
}

static inline void cpuid1(uint32_t *ecx, uint32_t *edx)
{
    uint32_t a, b;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(*ecx), "=d"(*edx) : "a"(1), "c"(0));
}

/* Push bytes below *sp in the new address space; returns their address. */
static uint64_t push_bytes(uint64_t pml4, uint64_t *sp, const void *data, size_t n)
{
    *sp -= n;
    copy_to_space(pml4, *sp, data, n);
    return *sp;
}

/* The dynamic linker of a program (PT_INTERP): an ET_DYN loaded just below the mmap area top. */
struct interp_image {
    uint64_t base;                                   /* load bias: AT_BASE */
    uint64_t entry;
    uint64_t start, end;                             /* the range it occupies */
};

static int load_interp(uint64_t pml4, const char *path, struct interp_image *out)
{
    int err;
    struct inode *ip = namei(path, &err);
    if (!ip)
        return err == -ENOENT ? -ENOENT : err;
    Elf64_Ehdr eh;
    Elf64_Phdr phs[32];
    int r = 0;
    if (!S_ISREG(inode_mode(ip)) || inode_permission(ip, X_OK) < 0)
        r = -EACCES;
    else if (readi(ip, &eh, 0, sizeof(eh)) != sizeof(eh) || eh.e_magic != ELF_MAGIC || eh.e_class != ELFCLASS64 ||
             eh.e_type != ET_DYN || eh.e_machine != EM_X86_64 || eh.e_phentsize != sizeof(Elf64_Phdr) ||
             eh.e_phnum > 32 ||
             readi(ip, phs, eh.e_phoff, eh.e_phnum * sizeof(Elf64_Phdr)) != (long)(eh.e_phnum * sizeof(Elf64_Phdr)))
        r = -ELIBBAD_K;
    uint64_t lo = ~0UL, hi = 0;
    for (int i = 0; r == 0 && i < eh.e_phnum; i++) {
        if (phs[i].p_type == PT_INTERP)
            r = -ELIBBAD_K;                          /* no chains of interpreters */
        if (phs[i].p_type != PT_LOAD)
            continue;
        lo = MIN(lo, PAGE_ALIGN_DOWN(phs[i].p_vaddr));
        hi = MAX(hi, PAGE_ALIGN_UP(phs[i].p_vaddr + phs[i].p_memsz));
    }
    if (r == 0 && (hi <= lo || hi - lo > (256UL << 20)))
        r = -ELIBBAD_K;
    if (r == 0) {
        uint64_t start = (USER_MMAP_TOP - (hi - lo)) & ~0xFFFFFUL;    /* 1 MiB aligned */
        uint64_t bias = start - lo;
        for (int i = 0; r == 0 && i < eh.e_phnum; i++)
            if (phs[i].p_type == PT_LOAD)
                r = load_segment(pml4, ip, &phs[i], bias);
        out->base = bias;
        out->entry = eh.e_entry + bias;
        out->start = start;
        out->end = start + (hi - lo);
    }
    iput(ip);
    return r;
}

static int exec_file(struct lwp *l, const char *path, char *const argv[], char *const envp[], int depth);

/*
 * A script ("#!interpreter [arg]" on its first line, up to 255 bytes): run
 * the interpreter with argv interpreter [arg] path argv[1]...  Interpreters
 * may be scripts themselves, 4 deep.  (The script's set-id bits are ignored:
 * the interpreter's count.)
 */
static int exec_script(struct lwp *l, struct inode *ip, const char *path, char *const argv[], char *const envp[],
                       int depth)
{
    char line[256];
    long n = readi(ip, line, 0, sizeof(line) - 1);
    iput(ip);
    if (n < 3)
        return -ENOEXEC;
    line[n] = 0;
    char *nl = strchr(line, '\n');
    if (!nl && n == (long)sizeof(line) - 1)
        return -ENOEXEC;                             /* the line is too long */
    if (nl)
        *nl = 0;
    char *interp = line + 2, *arg = NULL;
    while (*interp == ' ' || *interp == '\t')
        interp++;
    char *e = interp;
    while (*e && *e != ' ' && *e != '\t')
        e++;
    if (*e) {
        *e++ = 0;
        while (*e == ' ' || *e == '\t')
            e++;
        char *end = e + strlen(e);
        while (end > e && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
            *--end = 0;
        if (*e)
            arg = e;                                 /* the rest of the line: one argument */
    }
    if (!*interp)
        return -ENOEXEC;
    if (depth >= 4)
        return -ELOOP;
    int argc = 0;
    while (argv && argv[argc])
        argc++;
    char **nargv = kmalloc((argc + 3) * sizeof(char *));
    if (!nargv)
        return -ENOMEM;
    int k = 0;
    nargv[k++] = interp;
    if (arg)
        nargv[k++] = arg;
    nargv[k++] = (char *)path;
    for (int i = 1; i < argc; i++)
        nargv[k++] = argv[i];
    nargv[k] = NULL;
    int r = exec_file(l, interp, nargv, envp, depth + 1);
    kfree(nargv);
    return r;
}

static void single_lwp(void);

static int exec_into(struct lwp *l, const char *path, char *const argv[], char *const envp[])
{
    return exec_file(l, path, argv, envp, 0);
}

static int exec_file(struct lwp *l, const char *path, char *const argv[], char *const envp[], int depth)
{
    struct proc *p = l->proc;
    int err;
    struct inode *ip = namei(path, &err);
    if (!ip)
        return err;
    if (!S_ISREG(inode_mode(ip))) {
        iput(ip);
        return -EACCES;
    }
    if ((err = inode_permission(ip, X_OK)) < 0) {
        iput(ip);
        return err;
    }
    char magic[2];
    if (readi(ip, magic, 0, 2) == 2 && magic[0] == '#' && magic[1] == '!')
        return exec_script(l, ip, path, argv, envp, depth);
    Elf64_Ehdr eh;
    if (readi(ip, &eh, 0, sizeof(eh)) != sizeof(eh) || eh.e_magic != ELF_MAGIC ||
        eh.e_class != ELFCLASS64 || (eh.e_type != ET_EXEC && eh.e_type != ET_DYN) ||
        eh.e_machine != EM_X86_64 || eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum > 32) {
        iput(ip);
        return -ENOEXEC;
    }
    Elf64_Phdr phs[32];
    if (readi(ip, phs, eh.e_phoff, eh.e_phnum * sizeof(Elf64_Phdr)) != (long)(eh.e_phnum * sizeof(Elf64_Phdr))) {
        iput(ip);
        return -ENOEXEC;
    }
    /* PIE: relocate so that the lowest segment starts at PIE_BASE */
    uint64_t bias = 0, lowest = ~0UL;
    char interp[256];                            /* (PT_INTERP: a short absolute path) */
    interp[0] = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (phs[i].p_type == PT_INTERP) {            /* the dynamic linker to run first */
            if (phs[i].p_filesz < 2 || phs[i].p_filesz > sizeof(interp) ||
                readi(ip, interp, phs[i].p_offset, phs[i].p_filesz) != (long)phs[i].p_filesz ||
                interp[phs[i].p_filesz - 1]) {
                iput(ip);
                return -ENOEXEC;
            }
        }
        if (phs[i].p_type == PT_LOAD && phs[i].p_vaddr < lowest)
            lowest = phs[i].p_vaddr;
    }
    if (eh.e_type == ET_DYN && lowest != ~0UL)
        bias = PIE_BASE - PAGE_ALIGN_DOWN(lowest);

    uint64_t pml4 = vmm_new_space();
    if (!pml4) {
        iput(ip);
        return -ENOMEM;
    }
    uint64_t max_end = 0, phdr_va = 0;
    int r = 0;
    for (int i = 0; i < eh.e_phnum && r == 0; i++) {
        Elf64_Phdr *ph = &phs[i];
        if (ph->p_type == PT_PHDR)
            phdr_va = ph->p_vaddr + bias;
        if (ph->p_type != PT_LOAD)
            continue;
        if ((r = load_segment(pml4, ip, ph, bias)) < 0)
            break;
        max_end = MAX(max_end, ph->p_vaddr + bias + ph->p_memsz);
        if (!phdr_va && eh.e_phoff >= ph->p_offset && eh.e_phoff < ph->p_offset + ph->p_filesz)
            phdr_va = ph->p_vaddr + bias + (eh.e_phoff - ph->p_offset);
    }
    uint16_t mode = inode_mode(ip);
    int fuid = inode_uid(ip), fgid = inode_gid(ip);
    if (ip->fs->nosuid)
        mode &= ~(S_ISUID | S_ISGID);                /* MS_NOSUID */
    iput(ip);
    if (r == 0 && !max_end)
        r = -ENOEXEC;
    struct interp_image in = { 0 };
    if (r == 0 && interp[0])
        r = load_interp(pml4, interp, &in);
    /* the stack pages the arguments, environment and auxv go in, and some room */
    int argc = 0, envc = 0;
    size_t strbytes = strlen(path) + 1 + 64;
    while (argv && argv[argc] && argc < MAXARGS)
        strbytes += strlen(argv[argc++]) + 1;
    while (envp && envp[envc] && envc < MAXENV)
        strbytes += strlen(envp[envc++]) + 1;
    uint64_t stack_init = PAGE_ALIGN_UP(strbytes + (argc + envc + 64) * 8 + USER_STACK_INIT);
    if (stack_init > USER_STACK_SIZE / 2)
        r = r ? r : -E2BIG;
    if (r == 0)
        r = vmm_alloc_range(pml4, USER_STACK_TOP - stack_init, USER_STACK_TOP, PTE_U | PTE_W | pte_nx);
    if (r < 0) {
        vmm_free_space(pml4);
        return r;
    }

    /* credentials the program will run with */
    int new_euid = (mode & S_ISUID) ? fuid : p->euid;
    int new_egid = (mode & S_ISGID) ? fgid : p->egid;
    bool setid = new_euid != p->uid || new_egid != p->gid;

    /*
     * The SysV amd64 initial stack (see abi/include/sieos/auxv.h):
     *   argc, argv[], NULL, envp[], NULL, auxv pairs, AT_NULL
     * followed by the strings, the platform name and 16 random bytes.
     */
    uint64_t sp = USER_STACK_TOP;
    uint64_t execfn = push_bytes(pml4, &sp, path, strlen(path) + 1);
    uint64_t platform = push_bytes(pml4, &sp, SIEOS_PLATFORM, sizeof(SIEOS_PLATFORM));
    uint8_t rnd[16];
    random_bytes(rnd, sizeof(rnd));
    sp &= ~15UL;
    uint64_t random = push_bytes(pml4, &sp, rnd, sizeof(rnd));
    uint64_t *strs = kmalloc((argc + envc + 1) * sizeof(uint64_t));
    if (!strs) {
        vmm_free_space(pml4);
        return -ENOMEM;
    }
    for (int i = envc - 1; i >= 0; i--)
        strs[argc + i] = push_bytes(pml4, &sp, envp[i], strlen(envp[i]) + 1);
    for (int i = argc - 1; i >= 0; i--)
        strs[i] = push_bytes(pml4, &sp, argv[i], strlen(argv[i]) + 1);

    uint32_t hw_ecx, hw_edx;
    cpuid1(&hw_ecx, &hw_edx);
    uint64_t aux[][2] = {
        { SIEOS_AT_PHDR, phdr_va },         { SIEOS_AT_PHENT, sizeof(Elf64_Phdr) },
        { SIEOS_AT_PHNUM, eh.e_phnum },     { SIEOS_AT_PAGESZ, PAGE_SIZE },
        { SIEOS_AT_BASE, in.base },         { SIEOS_AT_FLAGS, 0 },
        { SIEOS_AT_ENTRY, eh.e_entry + bias }, { SIEOS_AT_HWCAP, hw_edx },
        { SIEOS_AT_HWCAP2, hw_ecx },        { SIEOS_AT_CLKTCK, 100 },
        { SIEOS_AT_UID, (uint64_t)p->uid }, { SIEOS_AT_EUID, (uint64_t)new_euid },
        { SIEOS_AT_GID, (uint64_t)p->gid }, { SIEOS_AT_EGID, (uint64_t)new_egid },
        { SIEOS_AT_SECURE, setid },         { SIEOS_AT_RANDOM, random },
        { SIEOS_AT_EXECFN, execfn },        { SIEOS_AT_SUN_UID, (uint64_t)new_euid },
        { SIEOS_AT_SUN_RUID, (uint64_t)p->uid }, { SIEOS_AT_SUN_GID, (uint64_t)new_egid },
        { SIEOS_AT_SUN_RGID, (uint64_t)p->gid }, { SIEOS_AT_SUN_PLATFORM, platform },
        { SIEOS_AT_SUN_HWCAP, hw_edx },     { SIEOS_AT_SUN_EXECNAME, execfn },
        { SIEOS_AT_SUN_AUXFLAGS, setid ? SIEOS_AF_SUN_SETUGID : 0 },
        { SIEOS_AT_NULL, 0 },
    };
    size_t naux = sizeof(aux) / sizeof(aux[0]);
    size_t words = 1 + argc + 1 + envc + 1 + 2 * naux;
    sp &= ~15UL;
    if (words & 1)
        sp -= 8;                                     /* rsp must be 16-byte aligned at argc */
    sp -= words * 8;
    uint64_t v = sp;
    uint64_t w = argc;
    copy_to_space(pml4, v, &w, 8), v += 8;
    p->argc = argc;
    p->argv_addr = v;
    p->envp_addr = v + 8 * (argc + 1);
    for (int i = 0; i < argc; i++)
        copy_to_space(pml4, v, &strs[i], 8), v += 8;
    w = 0;
    copy_to_space(pml4, v, &w, 8), v += 8;
    for (int i = 0; i < envc; i++)
        copy_to_space(pml4, v, &strs[argc + i], 8), v += 8;
    copy_to_space(pml4, v, &w, 8), v += 8;
    copy_to_space(pml4, v, aux, sizeof(aux));
    kfree(strs);

    /* The point of no return: only now do the other LWPs go (a failed exec leaves them be). */
    if (l == curlwp) {
        single_lwp();
        if (l->must_exit) {                          /* another LWP's exec (or an exit) won */
            vmm_free_space(pml4);
            return -EINTR;
        }
    }

    /* Commit: replace the old address space. */
    uint64_t old = p->pml4;
    p->pml4 = pml4;
    if (p == current)
        write_cr3(pml4);
    if (old && old != kernel_pml4_phys)
        vm_space_free(p, old);

    p->euid = new_euid;
    p->egid = new_egid;
    p->suid = p->euid;
    p->sgid = p->egid;
    signal_exec_reset(p);

    p->heap_start = p->brk = PAGE_ALIGN_UP(max_end);
    (void)0;
    const char *base = strrchr(path, '/');
    strlcpy(p->name, base ? base + 1 : path, sizeof(p->name));
    p->psargs[0] = 0;
    for (int i = 0; i < argc; i++) {
        if (i)
            strlcat(p->psargs, " ", sizeof(p->psargs));
        strlcat(p->psargs, argv[i], sizeof(p->psargs));
    }

    /* descriptors marked close-on-exec */
    for (int i = 0; i < NOFILE; i++)
        if (p->ofile[i] && (p->fdflags[i] & FD_CLOEXEC))
            fd_close(p, i);
    vm_exec_reset(p);
    if (in.end)
        vm_add_area(p, in.start, in.end, SIEOS_PROT_READ | SIEOS_PROT_WRITE | SIEOS_PROT_EXEC, SIEOS_MAP_PRIVATE);

    struct trapframe *tf = l->tf;
    memset(tf, 0, sizeof(*tf));                      /* every register 0, rdx = 0 (no rtld fini) */
    l->fpu = fpu_default;
    l->fsbase = 0;
    l->exit_word = 0;
    l->robust_list = 0;
    l->altstack_sp = l->altstack_size = 0;
    l->altstack_flags = SIEOS_SS_DISABLE;
    if (l == curlwp) {
        fpu_restore(&l->fpu);
        wrmsr(MSR_FS_BASE, 0);
    }
    strlcpy(l->name, p->name, sizeof(l->name));
    tf->rip = in.entry ? in.entry : eh.e_entry + bias;
    tf->cs = USER_CS;
    tf->ss = USER_DS;
    tf->rflags = 0x202;              /* IF */
    tf->rsp = sp;
    return 0;
}

/* Terminate every other LWP of the calling process and wait until they are gone. */
static void single_lwp(void)
{
    struct proc *p = current;
    while (!curlwp->must_exit) {
        int others = 0;
        for (int i = 0; i < NLWP; i++) {
            struct lwp *l = &lwp_table[i];
            if (l->state == LWP_UNUSED || l->proc != p || l == curlwp)
                continue;
            if (l->state == LWP_ZOMBIE) {
                lwp_free(l);
                continue;
            }
            others++;
            l->must_exit = true;
            if (l->state == LWP_SLEEPING || l->state == LWP_STOPPED || l->state == LWP_SUSPENDED ||
                l->state == LWP_EMBRYO)
                make_runnable(l);
        }
        if (!others)
            return;
        sleep_on(&p->nlwp);
    }
}

long proc_exec(const char *path, char *const argv[], char *const envp[])
{
    return exec_into(curlwp, path, argv, envp);
}

int proc_spawn_init(const char *path, const char *cmdline)
{
    struct proc *p = alloc_proc();
    if (!p)
        return -ENOMEM;
    struct lwp *l = lwp_alloc(p);
    if (!l) {
        proc_free(p);
        return -ENOMEM;
    }
    p->pml4 = 0;
    p->pgid = p->sid = p->pid;
    p->umask = 022;
    p->cwd = vfs_root();
    p->root = vfs_root();
    struct file *con = file_alloc();
    con->type = FD_TTY;
    con->tty = &console_tty;
    con->flags = O_RDWR;
    p->ofile[0] = con;
    p->ofile[1] = file_dup(con);
    p->ofile[2] = file_dup(con);
    /* argv: path followed by the words of the boot command line */
    static char words[256];
    char *argv[16] = { (char *)path, NULL };
    int argc = 1;
    strlcpy(words, cmdline ? cmdline : "", sizeof(words));
    for (char *w = words; *w && argc < 15;) {
        while (*w == ' ')
            *w++ = 0;
        if (!*w)
            break;
        argv[argc++] = w;
        while (*w && *w != ' ')
            w++;
    }
    argv[argc] = NULL;
    struct lwp *saved = curlwp;
    mycpu()->lwp = l;                /* permission checks use current */
    char *envp[] = { "PATH=/bin:/sbin", "HOME=/", "TERM=sieos", NULL };
    int r = exec_into(l, path, argv, envp);
    mycpu()->lwp = saved;
    if (r < 0) {
        for (int i = 0; i < 3; i++)
            file_close(p->ofile[i]);
        iput(p->cwd);
        iput(p->root);
        proc_free(p);
        return r;
    }
    p->parent = NULL;
    p->state = PSTATE_RUNNING;
    make_runnable(l);
    return p->pid;
}

/* ------------------------------------------------------------------ */
/* fork / exit / wait                                                  */
/* ------------------------------------------------------------------ */

/* The processes of a real user, for RLIMIT_NPROC. */
static uint64_t user_procs(int uid)
{
    uint64_t n = 0;
    for (int i = 1; i < NPROC; i++)
        if (proc_table[i].state != PSTATE_UNUSED && proc_table[i].uid == uid)
            n++;
    return n;
}

long proc_fork(int flags)
{
    uint64_t lim = current->rlim_cur[SIEOS_RLIMIT_NPROC];
    if (lim != SIEOS_RLIM_INFINITY && user_procs(current->uid) >= lim)
        return -EAGAIN;                                /* (enforced for root too: it lowered its own limit) */
    struct proc *np = alloc_proc();
    if (!np)
        return -EAGAIN;
    strcpy(np->name, current->name);
    memcpy(np->psargs, current->psargs, sizeof(np->psargs));
    np->argc = current->argc;
    np->argv_addr = current->argv_addr;
    np->envp_addr = current->envp_addr;
    struct lwp *nl = lwp_alloc(np);
    if (!nl) {
        proc_free(np);
        return -EAGAIN;
    }
    struct lwp *cl = curlwp;
    struct proc *cp = current;
    np->pml4 = vm_space_copy(np, cp);
    if (!np->pml4) {
        proc_free(np);
        return -ENOMEM;
    }
    *nl->tf = *cl->tf;
    fpu_save(&cl->fpu);              /* the child starts with the parent's registers */
    nl->fpu = cl->fpu;
    nl->fsbase = cl->fsbase;
    nl->sig_blocked = cl->sig_blocked;
    nl->altstack_sp = cl->altstack_sp;
    nl->altstack_size = cl->altstack_size;
    nl->altstack_flags = cl->altstack_flags;
    strcpy(nl->name, cl->name);
    nl->tf->rax = 0;                 /* child sees 0 */
    nl->tf->rdx = 0;
    nl->tf->rflags &= ~1UL;          /* ABI v2: success (carry clear) */
    for (int i = 0; i < NOFILE; i++)
        if (cp->ofile[i]) {
            np->ofile[i] = file_dup(cp->ofile[i]);
            np->fdflags[i] = cp->fdflags[i];
        }
    np->cwd = idup(cp->cwd);
    np->root = cp->root ? idup(cp->root) : NULL;
    np->heap_start = cp->heap_start;
    np->brk = cp->brk;

    np->uid = cp->uid;
    np->euid = cp->euid;
    np->suid = cp->suid;
    np->gid = cp->gid;
    np->egid = cp->egid;
    np->sgid = cp->sgid;
    np->ngroups = cp->ngroups;
    memcpy(np->groups, cp->groups, sizeof(np->groups));
    np->umask = cp->umask;
    np->pgid = cp->pgid;
    np->sid = cp->sid;
    memcpy(np->sigact, cp->sigact, sizeof(np->sigact));
    memcpy(np->rlim_cur, cp->rlim_cur, sizeof(np->rlim_cur));
    memcpy(np->rlim_max, cp->rlim_max, sizeof(np->rlim_max));
    np->nosigchld = flags & 1;       /* SIEOS_FORK_NOSIGCHLD */
    np->waitpid_only = flags & 2;    /* SIEOS_FORK_WAITPID */

    np->parent = cp;
    np->state = PSTATE_RUNNING;
    nl->bound = curlwp->bound;       /* processor bindings and affinities are inherited */
    nl->affinity = curlwp->affinity;
    nl->exit_word = 0;
    nl->robust_list = 0;
    make_runnable(nl);
    return np->pid;
}

bool pgrp_exists_in_session(int pgid, int sid)
{
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &proc_table[i];
        if (p->state == PSTATE_RUNNING && p->pgid == pgid && p->sid == sid)
            return true;
    }
    return false;
}

long proc_setpgid(int pid, int pgid)
{
    struct proc *p = pid == 0 ? current : proc_find(pid);
    if (!p || p->state != PSTATE_RUNNING)
        return -ESRCH;
    if (p != current && p->parent != current)
        return -ESRCH;
    if (pgid < 0)
        return -EINVAL;
    if (pgid == 0)
        pgid = p->pid;
    if (p->sid != current->sid)
        return -EPERM;
    if (p->pid == p->sid)
        return -EPERM;                        /* session leader */
    if (pgid != p->pid && !pgrp_exists_in_session(pgid, current->sid))
        return -EPERM;
    p->pgid = pgid;
    return 0;
}

long proc_setsid(void)
{
    struct proc *p = current;
    for (int i = 1; i < NPROC; i++) {
        struct proc *q = &proc_table[i];
        if (q->state != PSTATE_UNUSED && q != p && q->pgid == p->pid)
            return -EPERM;
    }
    if (p->pgid == p->pid)
        return -EPERM;                        /* already a group leader */
    p->sid = p->pgid = p->pid;
    return p->sid;
}

/* The last LWP of a process is gone: release its resources, become a zombie. */
static void proc_teardown(struct proc *p)
{
    ipc_proc_exit(p);
    signal_purge(p, NULL, 0);
    for (int i = 0; i < NOFILE; i++)
        if (p->ofile[i])
            fd_close(p, i);
    iput(p->cwd);
    p->cwd = NULL;
    if (p->root)
        iput(p->root);
    p->root = NULL;
    fb_release_owner(p->pid);

    /* A dying session leader hangs up its terminal and its session. */
    if (p->sid == p->pid) {
        tty_session_exit(p->sid);
        for (int i = 1; i < NPROC; i++) {
            struct proc *q = &proc_table[i];
            if (q != p && q->state == PSTATE_RUNNING && q->sid == p->sid) {
                signal_send(q, SIGHUP);
                signal_send(q, SIGCONT);
            }
        }
    }

    struct proc *init = proc_find(1);
    for (int i = 1; i < NPROC; i++) {
        struct proc *c = &proc_table[i];
        if (c->state != PSTATE_UNUSED && c->parent == p) {
            c->parent = init;
            if (c->state == PSTATE_ZOMBIE && init)
                wakeup(init);
        }
    }

    /* The address space goes before freeing it: writing its shared mappings
     * back may sleep, and a dispatch meanwhile loads p->pml4 into CR3. */
    uint64_t pml4 = p->pml4;
    p->pml4 = 0;
    write_cr3(kernel_pml4_phys);
    vm_space_free(p, pml4);
    p->itimer_value[0] = p->itimer_value[1] = p->itimer_value[2] = 0;
    p->state = PSTATE_ZOMBIE;
    poll_wakeup();                               /* (its pidfds are readable now) */
    if (p->parent) {
        if (!p->nosigchld) {
            struct ksiginfo info = { 0 };
            int st = p->exit_status;
            info.code = (st & 0x7f) == 0 ? SIEOS_CLD_EXITED : (st & 0x80) ? SIEOS_CLD_DUMPED : SIEOS_CLD_KILLED;
            info.status = (st & 0x7f) == 0 ? (st >> 8) & 0xff : st & 0x7f;
            info.pid = p->pid;
            info.uid = p->uid;
            signal_send_info(p->parent, SIGCHLD, &info);
        }
        wakeup(p->parent);
    }
}

/* The calling LWP ends; the last one ends the process. */
void lwp_exit_self(void)
{
    struct lwp *l = curlwp;
    struct proc *p = l->proc;
    if (l->is_idle)
        panic("idle LWP tried to exit");
    lwp_exit_word(l);
    signal_purge(p, l, 0);
    p->ticks += l->ticks;
    p->ru.sticks += l->sticks;
    p->ru.nvcsw += l->nvcsw;
    p->ru.nivcsw += l->nivcsw;
    p->ru.minflt += l->minflt;
    p->nlwp--;
    if (p->nlwp == 0) {
        if (p->pid == 1)
            panic("init exited with status %x", p->exit_status);
        proc_teardown(p);                /* (closing files may sleep in disk I/O: still running) */
    } else {
        wakeup(&p->nlwp);                /* lwp_wait, exec (they run once we have switched away) */
    }
    l->state = LWP_ZOMBIE;               /* only now: a wake-up must not make us runnable again */
    reap_detached();
    schedule();
    panic("exited LWP %d/%d was scheduled", p->pid, l->lwpid);
}

void proc_exit(int status)
{
    struct proc *p = current;
    if (p->pid == 1)
        panic("init exited with status %x", status);
    if (!p->exiting) {
        p->exiting = true;
        p->exit_status = status;
        for (int i = 0; i < NLWP; i++) {
            struct lwp *l = &lwp_table[i];
            if (l->state == LWP_UNUSED || l->state == LWP_ZOMBIE || l->proc != p || l == curlwp)
                continue;
            l->must_exit = true;
            if (l->state == LWP_SLEEPING || l->state == LWP_STOPPED || l->state == LWP_SUSPENDED ||
                l->state == LWP_EMBRYO)
                make_runnable(l);
        }
    }
    lwp_exit_self();
}

/* Does child c match waitid(idtype, id)? */
static bool wait_match(struct proc *c, int idtype, long id)
{
    switch (idtype) {
    case SIEOS_P_PID:  return c->pid == id;
    case SIEOS_P_PGID: return !c->waitpid_only && c->pgid == id;
    case SIEOS_P_SID:  return !c->waitpid_only && c->sid == id;
    case SIEOS_P_ALL:  return !c->waitpid_only;
    default:           return false;
    }
}

/*
 * waitid(): report a child's state change in info (kernel signal numbers).
 * Returns the child's pid, 0 for WNOHANG with nothing to report, or -errno.
 */
long proc_waitid(int idtype, long id, int options, struct ksiginfo *info, int *status_word)
{
    if (!(options & (SIEOS_WEXITED | SIEOS_WSTOPPED | SIEOS_WCONTINUED)))
        return -EINVAL;
    if (idtype != SIEOS_P_PID && idtype != SIEOS_P_PGID && idtype != SIEOS_P_SID && idtype != SIEOS_P_ALL)
        return -EINVAL;
    for (;;) {
        bool have_kids = false;
        for (int i = 1; i < NPROC; i++) {
            struct proc *c = &proc_table[i];
            if (c->state == PSTATE_UNUSED || c->state == PSTATE_EMBRYO || c->parent != current)
                continue;
            if (!wait_match(c, idtype, id))
                continue;
            have_kids = true;
            memset(info, 0, sizeof(*info));
            info->pid = c->pid;
            info->uid = c->uid;
            if (c->state == PSTATE_ZOMBIE && (options & SIEOS_WEXITED)) {
                int st = c->exit_status;
                *status_word = st;
                if ((st & 0x7f) == 0) {
                    info->code = SIEOS_CLD_EXITED;
                    info->status = (st >> 8) & 0xff;
                } else {
                    info->code = (st & 0x80) ? SIEOS_CLD_DUMPED : SIEOS_CLD_KILLED;
                    info->status = st & 0x7f;
                }
                int cpid = c->pid;
                if (!(options & SIEOS_WNOWAIT)) {
                    current->child_ticks += c->ticks + c->child_ticks;
                    current->cru.sticks += c->ru.sticks + c->cru.sticks;
                    current->cru.nvcsw += c->ru.nvcsw + c->cru.nvcsw;
                    current->cru.nivcsw += c->ru.nivcsw + c->cru.nivcsw;
                    current->cru.minflt += c->ru.minflt + c->cru.minflt;
                    proc_free(c);
                }
                return cpid;
            }
            if ((options & SIEOS_WSTOPPED) && c->state == PSTATE_RUNNING && c->stopped && !c->stop_reported) {
                if (!(options & SIEOS_WNOWAIT))
                    c->stop_reported = true;
                info->code = SIEOS_CLD_STOPPED;
                info->status = c->stop_sig;
                *status_word = 0x7F | (c->stop_sig << 8);
                return c->pid;
            }
            if ((options & SIEOS_WCONTINUED) && c->cont_pending) {
                if (!(options & SIEOS_WNOWAIT))
                    c->cont_pending = false;
                info->code = SIEOS_CLD_CONTINUED;
                info->status = SIGCONT;
                *status_word = 0xFFFF;
                return c->pid;
            }
        }
        if (!have_kids)
            return -ECHILD;
        if (options & SIEOS_WNOHANG) {
            memset(info, 0, sizeof(*info));
            return 0;
        }
        if (signal_pending(current))
            return -ERESTART;
        sleep_on(current);
    }
}

bool proc_table_uses(struct fs *fs)
{
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &proc_table[i];
        if (p->state == PSTATE_UNUSED)
            continue;
        if ((p->cwd && p->cwd->fs == fs) || (p->root && p->root->fs == fs))
            return true;
        for (struct vm_area *a = p->areas; a; a = a->next)
            if (a->ip && a->ip->fs == fs)
                return true;
    }
    return false;
}
