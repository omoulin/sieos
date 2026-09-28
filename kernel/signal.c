/*
 * signal.c - Signals (Solaris numbering inside the kernel, see ksig.h).
 *
 * Dispositions belong to the process; masks, LWP-directed pending signals
 * and the alternate stack belong to each LWP.  A process-directed signal is
 * taken by one LWP that does not block it.  Signals are acted upon when an
 * LWP returns to user mode (signal_deliver, called by the trap handler):
 * default actions terminate the process, stop or continue every LWP, or
 * ignore; user handlers get an ABI v1 frame (sigreturn) or, when installed
 * through ABI v2, a Solaris ucontext + siginfo and return with
 * context(SETCONTEXT).
 */
#include "proc.h"
#include "abi2.h"
#include "mm.h"
#include "sieos/syscall.h"

#define STOP_SIGS (KSIGBIT(SIGSTOP) | KSIGBIT(SIGTSTP) | KSIGBIT(SIGTTIN) | KSIGBIT(SIGTTOU))
#define UNBLOCKABLE (KSIGBIT(SIGKILL) | KSIGBIT(SIGSTOP))
#define VALID_SIGS ((KSIGBIT(KNSIG) - 1) & ~(ksigset_t)1)

enum { ACT_TERM, ACT_CORE, ACT_IGN, ACT_STOP, ACT_CONT };

static int default_action(int sig)
{
    switch (sig) {
    case SIGQUIT: case SIGILL: case SIGTRAP: case SIGABRT: case SIGEMT: case SIGFPE:
    case SIGBUS: case SIGSEGV: case SIGSYS: case SIGXCPU: case SIGXFSZ:
        return ACT_CORE;
    case SIGCHLD: case SIGPWR: case SIGWINCH: case SIGURG: case SIEOS_SIGWAITING: case SIEOS_SIGLWP:
    case SIEOS_SIGFREEZE: case SIEOS_SIGTHAW: case SIEOS_SIGCANCEL: case SIEOS_SIGXRES:
    case SIEOS_SIGJVM1: case SIEOS_SIGJVM2: case SIEOS_SIGINFO:
        return ACT_IGN;
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
        return ACT_STOP;
    case SIGCONT:
        return ACT_CONT;
    }
    return ACT_TERM;
}

static int lowest_sig(ksigset_t s)
{
    uint64_t lo = (uint64_t)s, hi = (uint64_t)(s >> 64);
    return lo ? __builtin_ctzl(lo) : hi ? 64 + __builtin_ctzl(hi) : 0;
}

/* ---------------- ABI v2 sets and siginfo ---------------- */

ksigset_t sig_set_from_v2(const sieos_sigset_t *s)
{
    ksigset_t k = 0;
    for (int sig = 1; sig < KNSIG; sig++)
        if (s->__sigbits[(sig - 1) / 32] & (1u << ((sig - 1) % 32)))
            k |= KSIGBIT(sig);
    return k;
}

void sig_set_to_v2(ksigset_t k, sieos_sigset_t *s)
{
    memset(s, 0, sizeof(*s));
    for (int sig = 1; sig < KNSIG; sig++)
        if (k & KSIGBIT(sig))
            s->__sigbits[(sig - 1) / 32] |= 1u << ((sig - 1) % 32);
}

static void siginfo_to_v2(int sig, const struct ksiginfo *k, sieos_siginfo_t *u)
{
    memset(u, 0, sizeof(*u));
    u->si_signo = sig;
    u->si_code = k->code;
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP) {
        u->__data.fault.addr = (void *)k->addr;
    } else {
        u->__data.proc.pid = k->pid;
        u->__data.proc.uid = k->uid;
        u->__data.proc.status = k->status;
        u->__data.proc.value.sival_ptr = (void *)k->value;
    }
}

/* ---------------- sending ---------------- */

bool signal_ignored_or_blocked(struct proc *p, int sig)
{
    return p->sigact[sig].handler == (uint64_t)SIG_IGN || (curlwp->sig_blocked & KSIGBIT(sig));
}

bool signal_pending(struct proc *p)
{
    (void)p;
    struct lwp *l = curlwp;
    if (l->must_exit)
        return true;
    return ((l->sig_pending | l->proc->sig_pending) & ~(l->sig_blocked & ~UNBLOCKABLE)) != 0;
}

static bool sigchld_nocldstop(struct proc *parent)
{
    struct ksigaction *ka = &parent->sigact[SIGCHLD];
    return ka->flags & SIEOS_SA_NOCLDSTOP;
}

static void notify_parent(struct proc *p, int code, int status)
{
    struct proc *parent = p->parent;
    if (!parent)
        return;
    if (!sigchld_nocldstop(parent) && !p->nosigchld) {
        struct ksiginfo info = { .code = code, .pid = p->pid, .uid = p->uid, .status = status };
        signal_send_info(parent, SIGCHLD, &info);
    }
    wakeup(parent);
}

static bool discarded(struct proc *p, int sig)
{
    if (sig == SIGKILL || sig == SIGSTOP)
        return false;
    uint64_t h = p->sigact[sig].handler;
    if (h == (uint64_t)SIG_IGN)
        return true;
    return h == (uint64_t)SIG_DFL && (default_action(sig) == ACT_IGN || default_action(sig) == ACT_CONT);
}

/* Continue a stopped process: every stopped LWP runs again. */
static void continue_proc(struct proc *p)
{
    p->stopped = false;
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->proc == p && l->state == LWP_STOPPED)
            make_runnable(l);
    }
}

/*
 * Real-time signals queue: a signal already pending keeps its first
 * instance in siginfo[] and the others here, oldest first.
 */
#define NRTQ 256
static struct rtq {
    struct proc *p;
    struct lwp *l;                 /* NULL: process-directed */
    int sig;
    uint64_t seq;
    struct ksiginfo info;
} rtq[NRTQ];
static uint64_t rtq_seq;

static bool is_rt(int sig)
{
    return sig >= SIEOS_SIGRTMIN && sig <= SIEOS_SIGRTMAX;
}

static void rtq_add(struct proc *p, struct lwp *l, int sig, const struct ksiginfo *info)
{
    for (int i = 0; i < NRTQ; i++)
        if (!rtq[i].p) {
            rtq[i] = (struct rtq){ p, l, sig, ++rtq_seq, *info };
            return;
        }
}

static bool rtq_take(struct proc *p, struct lwp *l, int sig, struct ksiginfo *info)
{
    struct rtq *best = NULL;
    for (int i = 0; i < NRTQ; i++)
        if (rtq[i].p == p && rtq[i].l == l && rtq[i].sig == sig && (!best || rtq[i].seq < best->seq))
            best = &rtq[i];
    if (!best)
        return false;
    *info = best->info;
    best->p = NULL;
    return true;
}

/* Drop queued instances: of an LWP (l), of a process (l == NULL, all), or of one signal (sig > 0). */
void signal_purge(struct proc *p, struct lwp *l, int sig)
{
    for (int i = 0; i < NRTQ; i++)
        if (rtq[i].p == p && (!l || rtq[i].l == l) && (!sig || rtq[i].sig == sig))
            rtq[i].p = NULL;
}

void signal_send_info(struct proc *p, int sig, const struct ksiginfo *info)
{
    if (!p || sig <= 0 || sig >= KNSIG || p->state != PSTATE_RUNNING)
        return;
    if (sig == SIGCONT || sig == SIGKILL) {
        p->sig_pending &= ~STOP_SIGS;
        for (int i = 0; i < NLWP; i++)
            if (lwp_table[i].proc == p)
                lwp_table[i].sig_pending &= ~STOP_SIGS;
        if (p->stopped) {
            continue_proc(p);
            if (sig == SIGCONT) {
                p->cont_pending = true;
                notify_parent(p, SIEOS_CLD_CONTINUED, SIGCONT);
            }
        }
    }
    if (KSIGBIT(sig) & STOP_SIGS)
        p->sig_pending &= ~KSIGBIT(SIGCONT);
    if (discarded(p, sig))
        return;

    struct ksiginfo k = info ? *info : (struct ksiginfo){ .code = SIEOS_SI_USER, .pid = current->pid, .uid = current->uid };
    if (is_rt(sig) && (p->sig_pending & KSIGBIT(sig))) {
        rtq_add(p, NULL, sig, &k);
    } else {
        p->sig_pending |= KSIGBIT(sig);
        p->siginfo[sig] = k;
    }

    /* wake an LWP that can take it (all of them for SIGKILL) */
    bool unblockable = KSIGBIT(sig) & UNBLOCKABLE;
    for (int i = 0; i < NLWP; i++) {
        struct lwp *l = &lwp_table[i];
        if (l->proc != p || l->state == LWP_UNUSED || l->state == LWP_ZOMBIE || l->state == LWP_EMBRYO)
            continue;
        if (!unblockable && (l->sig_blocked & KSIGBIT(sig)))
            continue;
        if (l->state == LWP_RUNNING || l->state == LWP_RUNNABLE) {
            if (sig != SIGKILL)
                return;                              /* it will see the signal soon */
            continue;
        }
        if (l->state == LWP_SLEEPING || (sig == SIGKILL && (l->state == LWP_STOPPED || l->state == LWP_SUSPENDED))) {
            make_runnable(l);
            if (sig != SIGKILL)
                return;
        }
    }
}

void signal_send(struct proc *p, int sig)
{
    signal_send_info(p, sig, NULL);
}

void signal_lwp(struct lwp *l, int sig, const struct ksiginfo *info)
{
    struct proc *p = l->proc;
    if (sig <= 0 || sig >= KNSIG || p->state != PSTATE_RUNNING || l->state == LWP_ZOMBIE)
        return;
    if (sig == SIGKILL || sig == SIGSTOP || sig == SIGCONT) {
        signal_send_info(p, sig, info);              /* these act on the whole process */
        return;
    }
    if (discarded(p, sig))
        return;
    struct ksiginfo k = info ? *info : (struct ksiginfo){ .code = SIEOS_SI_LWP, .pid = current->pid, .uid = current->uid };
    if (is_rt(sig) && (l->sig_pending & KSIGBIT(sig))) {
        rtq_add(p, l, sig, &k);
    } else {
        l->sig_pending |= KSIGBIT(sig);
        l->siginfo[sig] = k;
    }
    if (l->state == LWP_SLEEPING && !(l->sig_blocked & KSIGBIT(sig)))
        make_runnable(l);
}

int signal_pgrp(int pgid, int sig)
{
    int n = 0;
    if (pgid <= 0)
        return 0;
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &proc_table[i];
        if (p->state == PSTATE_RUNNING && p->pgid == pgid) {
            signal_send(p, sig);
            n++;
        }
    }
    return n;
}

void signal_exec_reset(struct proc *p)
{
    for (int s = 1; s < KNSIG; s++) {
        if (p->sigact[s].handler != (uint64_t)SIG_IGN) {
            p->sigact[s].handler = (uint64_t)SIG_DFL;
            p->sigact[s].flags = 0;
            p->sigact[s].mask = 0;
        }
    }
}

static bool may_signal(struct proc *target, int sig)
{
    if (current->euid == 0)
        return true;
    if (sig == SIGCONT && target->sid == current->sid)
        return true;
    return current->uid == target->uid || current->uid == target->suid ||
           current->euid == target->uid || current->euid == target->suid;
}

long kill_pids(int pid, int sig, const struct ksiginfo *info)
{
    if (sig < 0 || sig >= KNSIG)
        return -EINVAL;
    int found = 0, permitted = 0;
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &proc_table[i];
        if (p->state != PSTATE_RUNNING)
            continue;
        bool match;
        if (pid > 0)
            match = p->pid == pid;
        else if (pid == 0)
            match = p->pgid == current->pgid;
        else if (pid == -1)
            match = p->pid != 1 && p != current;
        else
            match = p->pgid == -pid;
        if (!match)
            continue;
        found++;
        if (p->pid == 1 && (sig == SIGKILL || sig == SIGSTOP || p->sigact[sig].handler == (uint64_t)SIG_DFL))
            continue;                          /* init only gets the signals it handles */
        if (!may_signal(p, sig))
            continue;
        permitted++;
        if (sig)
            signal_send_info(p, sig, info);
    }
    if (!found)
        return -ESRCH;
    return permitted ? 0 : -EPERM;
}

/* A synchronous fault in user mode (from the trap handler). */
void signal_fault(int sig, int code, uint64_t addr)
{
    struct proc *p = current;
    struct lwp *l = curlwp;
    struct ksigaction *ka = &p->sigact[sig];
    if (ka->handler <= (uint64_t)SIG_IGN || (l->sig_blocked & KSIGBIT(sig))) {
        kprintf("%s[%d/%d]: fatal signal %d (code %d) at rip %lx, address %lx\n", p->name, p->pid, l->lwpid,
                sig, code, l->tf->rip, addr);
        proc_exit(sig | (default_action(sig) == ACT_CORE && core_dump(sig, l->tf) ? 0x80 : 0));
    }
    struct ksiginfo info = { .code = code, .addr = addr, .pid = p->pid, .uid = p->uid };
    l->sig_pending |= KSIGBIT(sig);
    l->siginfo[sig] = info;
}

/* ---------------- delivery ---------------- */

static void restart_or_eintr(struct trapframe *tf, bool restart)
{
    struct lwp *l = curlwp;
    if (!l->restart_syscall)
        return;
    l->restart_syscall = false;
    if (restart) {
        tf->rax = l->orig_rax;
        tf->rip -= 2;                          /* re-execute the syscall instruction */
        tf->rflags &= ~1UL;
    } else {
        tf->rax = sieos_errno(EINTR);          /* CF + positive errno */
        tf->rflags |= 1;
    }
}

/* The mask to restore after the handler: sigsuspend's saved one, or the current one. */
static ksigset_t previous_mask(struct lwp *l)
{
    if (l->saved_mask_valid) {
        l->saved_mask_valid = false;
        return l->saved_mask;
    }
    return l->sig_blocked;
}

static void apply_handler_mask(struct lwp *l, int sig, struct ksigaction *ka, bool nodefer, bool resethand)
{
    if (!nodefer)
        l->sig_blocked |= KSIGBIT(sig);
    l->sig_blocked |= ka->mask;
    l->sig_blocked &= ~UNBLOCKABLE;
    if (resethand) {
        ka->handler = (uint64_t)SIG_DFL;
        ka->flags = 0;
    }
}

static void tf_to_gregs(const struct trapframe *tf, sieos_greg_t *g, uint64_t fsbase)
{
    g[SIEOS_REG_R15] = tf->r15; g[SIEOS_REG_R14] = tf->r14; g[SIEOS_REG_R13] = tf->r13;
    g[SIEOS_REG_R12] = tf->r12; g[SIEOS_REG_R11] = tf->r11; g[SIEOS_REG_R10] = tf->r10;
    g[SIEOS_REG_R9] = tf->r9;   g[SIEOS_REG_R8] = tf->r8;   g[SIEOS_REG_RDI] = tf->rdi;
    g[SIEOS_REG_RSI] = tf->rsi; g[SIEOS_REG_RBP] = tf->rbp; g[SIEOS_REG_RBX] = tf->rbx;
    g[SIEOS_REG_RDX] = tf->rdx; g[SIEOS_REG_RCX] = tf->rcx; g[SIEOS_REG_RAX] = tf->rax;
    g[SIEOS_REG_TRAPNO] = tf->int_no; g[SIEOS_REG_ERR] = tf->err_code;
    g[SIEOS_REG_RIP] = tf->rip; g[SIEOS_REG_CS] = tf->cs; g[SIEOS_REG_RFL] = tf->rflags;
    g[SIEOS_REG_RSP] = tf->rsp; g[SIEOS_REG_SS] = tf->ss;
    g[SIEOS_REG_FS] = g[SIEOS_REG_GS] = g[SIEOS_REG_ES] = g[SIEOS_REG_DS] = 0;
    g[SIEOS_REG_FSBASE] = fsbase;
    g[SIEOS_REG_GSBASE] = 0;
}

void sig_tf_to_gregs(const struct trapframe *tf, sieos_greg_t *g, uint64_t fsbase)
{
    tf_to_gregs(tf, g, fsbase);
}

void sig_gregs_to_tf(const sieos_greg_t *g, struct trapframe *tf)
{
    tf->r15 = g[SIEOS_REG_R15]; tf->r14 = g[SIEOS_REG_R14]; tf->r13 = g[SIEOS_REG_R13];
    tf->r12 = g[SIEOS_REG_R12]; tf->r11 = g[SIEOS_REG_R11]; tf->r10 = g[SIEOS_REG_R10];
    tf->r9 = g[SIEOS_REG_R9];   tf->r8 = g[SIEOS_REG_R8];   tf->rdi = g[SIEOS_REG_RDI];
    tf->rsi = g[SIEOS_REG_RSI]; tf->rbp = g[SIEOS_REG_RBP]; tf->rbx = g[SIEOS_REG_RBX];
    tf->rdx = g[SIEOS_REG_RDX]; tf->rcx = g[SIEOS_REG_RCX]; tf->rax = g[SIEOS_REG_RAX];
    tf->rip = g[SIEOS_REG_RIP]; tf->rsp = g[SIEOS_REG_RSP];
    /* only the user-changeable flags (CF PF AF ZF SF TF DF OF), interrupts stay on */
    tf->rflags = (g[SIEOS_REG_RFL] & 0xDD5UL) | 0x202;
    tf->cs = USER_CS;
    tf->ss = USER_DS;
}

/* Fill a ucontext with the LWP's user state (tf: the state to resume). */
static void make_ucontext(struct lwp *l, const struct trapframe *tf, ksigset_t mask, sieos_ucontext_t *uc)
{
    memset(uc, 0, sizeof(*uc));
    uc->uc_flags = SIEOS_UC_ALL;
    sig_set_to_v2(mask, &uc->uc_sigmask);
    uc->uc_stack.ss_sp = (void *)l->altstack_sp;
    uc->uc_stack.ss_size = l->altstack_size;
    uc->uc_stack.ss_flags = l->altstack_flags;
    tf_to_gregs(tf, uc->uc_mcontext.gregs, l->fsbase);
    fpu_save(&l->fpu);
    memcpy(&uc->uc_mcontext.fpregs, l->fpu.area, 512);
}

static bool on_altstack(struct lwp *l, uint64_t sp)
{
    return !(l->altstack_flags & SIEOS_SS_DISABLE) && sp >= l->altstack_sp && sp < l->altstack_sp + l->altstack_size;
}

static bool setup_frame_v2(struct trapframe *tf, int sig, struct ksigaction *ka, const struct ksiginfo *info)
{
    struct lwp *l = curlwp;
    uint64_t sp = tf->rsp - 128;                 /* red zone */
    bool alt = (ka->flags & SIEOS_SA_ONSTACK) && !(l->altstack_flags & SIEOS_SS_DISABLE) && !on_altstack(l, tf->rsp);
    if (alt)
        sp = l->altstack_sp + l->altstack_size;
    sp -= sizeof(sieos_ucontext_t);
    sp &= ~15UL;
    uint64_t ucp = sp;
    sp -= sizeof(sieos_siginfo_t);
    sp &= ~15UL;
    uint64_t sip = sp;
    sp -= 8;                                     /* return address 0: handlers must not return */
    if (!user_range_ok(current->pml4, sp, ucp + sizeof(sieos_ucontext_t) - sp, true))
        return false;
    static sieos_ucontext_t uc;                  /* under the big kernel lock */
    make_ucontext(l, tf, previous_mask(l), &uc);
    if (on_altstack(l, tf->rsp) || alt)
        uc.uc_stack.ss_flags |= SIEOS_SS_ONSTACK;
    memcpy((void *)ucp, &uc, sizeof(uc));
    sieos_siginfo_t si;
    siginfo_to_v2(sig, info, &si);
    memcpy((void *)sip, &si, sizeof(si));
    *(uint64_t *)sp = 0;
    tf->rsp = sp;
    tf->rip = ka->handler;
    tf->rdi = sig;
    tf->rsi = sip;
    tf->rdx = ucp;
    tf->rax = 0;
    tf->rflags &= ~0x400UL;                      /* clear DF */
    /* the handler starts with clean FPU/SSE state */
    l->fpu = fpu_default;
    fpu_restore(&l->fpu);
    apply_handler_mask(l, sig, ka, ka->flags & SIEOS_SA_NODEFER, ka->flags & SIEOS_SA_RESETHAND);
    return true;
}

/* Take the next deliverable signal: LWP-directed first, then process-directed. */
static int dequeue(struct lwp *l, ksigset_t allowed, struct ksiginfo *info)
{
    struct proc *p = l->proc;
    ksigset_t mine = l->sig_pending & allowed, shared = p->sig_pending & allowed;
    int sl = lowest_sig(mine), sp = lowest_sig(shared);
    if (sl && (!sp || sl <= sp)) {
        l->sig_pending &= ~KSIGBIT(sl);
        *info = l->siginfo[sl];
        if (is_rt(sl) && rtq_take(p, l, sl, &l->siginfo[sl]))
            l->sig_pending |= KSIGBIT(sl);           /* the next queued instance */
        return sl;
    }
    if (sp) {
        p->sig_pending &= ~KSIGBIT(sp);
        *info = p->siginfo[sp];
        if (is_rt(sp) && rtq_take(p, NULL, sp, &p->siginfo[sp]))
            p->sig_pending |= KSIGBIT(sp);
        return sp;
    }
    return 0;
}

void signal_deliver(struct trapframe *tf)
{
    struct lwp *l = curlwp;
    struct proc *p = current;
    for (;;) {
        if (l->must_exit)
            lwp_exit_self();
        /* job-control stop: every LWP parks here until SIGCONT or SIGKILL */
        if (p->stopped && !((p->sig_pending | l->sig_pending) & KSIGBIT(SIGKILL))) {
            l->state = LWP_STOPPED;
            schedule();
            continue;
        }
        if (l->suspend_req) {                    /* lwp_suspend */
            l->state = LWP_SUSPENDED;
            schedule();
            continue;
        }
        struct ksiginfo info;
        int sig = dequeue(l, ~(l->sig_blocked & ~UNBLOCKABLE), &info);
        if (!sig)
            break;
        struct ksigaction *ka = &p->sigact[sig];
        bool forced = sig == SIGKILL || sig == SIGSTOP;

        if (!forced && ka->handler == (uint64_t)SIG_IGN)
            continue;
        if (forced || ka->handler == (uint64_t)SIG_DFL) {
            switch (default_action(sig)) {
            case ACT_IGN:
            case ACT_CONT:
                continue;
            case ACT_STOP:
                p->stopped = true;
                p->stop_sig = sig;
                p->stop_reported = false;
                notify_parent(p, SIEOS_CLD_STOPPED, sig);
                continue;                        /* parks at the top of the loop */
            case ACT_CORE:
                proc_exit(sig | (core_dump(sig, tf) ? 0x80 : 0));
            default:
                proc_exit(sig);
            }
        }
        /* user handler */
        bool restart = ka->flags & SIEOS_SA_RESTART;
        restart_or_eintr(tf, restart);
        bool ok = setup_frame_v2(tf, sig, ka, &info);
        if (!ok) {
            kprintf("%s[%d]: cannot deliver signal %d (bad stack), killed\n", p->name, p->pid, sig);
            proc_exit(SIGSEGV | 0x80);
        }
        return;
    }
    restart_or_eintr(tf, true);
    if (l->saved_mask_valid) {                   /* sigsuspend woke without a handler */
        l->sig_blocked = l->saved_mask;
        l->saved_mask_valid = false;
    }
}

/* ---------------- ABI v2 calls ---------------- */

long sys2_sigaction(int sig, const struct sieos_sigaction *act, struct sieos_sigaction *old)
{
    if (sig <= 0 || sig >= KNSIG)
        return -EINVAL;
    struct ksigaction *ka = &current->sigact[sig];
    if (old) {
        if (!user_ok(old, sizeof(*old), true))
            return -EFAULT;
        struct sieos_sigaction o;
        memset(&o, 0, sizeof(o));
        o.__sa_u.sa_handler = (void (*)(int))ka->handler;
        o.sa_flags = (int)ka->flags;
        sig_set_to_v2(ka->mask, &o.sa_mask);
        memcpy(old, &o, sizeof(o));
    }
    if (act) {
        if (!user_ok(act, sizeof(*act), false))
            return -EFAULT;
        if (sig == SIGKILL || sig == SIGSTOP)
            return -EINVAL;
        struct sieos_sigaction a;
        memcpy(&a, act, sizeof(a));
        uint64_t h = (uint64_t)a.__sa_u.sa_handler;
        if (h == 2)                                  /* SIG_HOLD */
            return -EINVAL;
        ka->handler = h;
        ka->flags = (unsigned)a.sa_flags;
        ka->restorer = 0;
        ka->mask = sig_set_from_v2(&a.sa_mask) & ~UNBLOCKABLE;
        if (discarded(current, sig)) {
            current->sig_pending &= ~KSIGBIT(sig);
            curlwp->sig_pending &= ~KSIGBIT(sig);
            signal_purge(current, NULL, sig);
        }
    }
    return 0;
}

long sys2_sigmask(int how, const sieos_sigset_t *set, sieos_sigset_t *old)
{
    struct lwp *l = curlwp;
    if (old) {
        if (!user_ok(old, sizeof(*old), true))
            return -EFAULT;
        sieos_sigset_t o;
        sig_set_to_v2(l->sig_blocked, &o);
        memcpy(old, &o, sizeof(o));
    }
    if (set) {
        if (!user_ok(set, sizeof(*set), false))
            return -EFAULT;
        sieos_sigset_t n;
        memcpy(&n, set, sizeof(n));
        ksigset_t s = sig_set_from_v2(&n) & ~UNBLOCKABLE;
        switch (how) {
        case SIEOS_SIG_BLOCK:   l->sig_blocked |= s; break;
        case SIEOS_SIG_UNBLOCK: l->sig_blocked &= ~s; break;
        case SIEOS_SIG_SETMASK: l->sig_blocked = s; break;
        default: return -EINVAL;
        }
    }
    return 0;
}

long sys2_sigpending(int op, sieos_sigset_t *set)
{
    if (!user_ok(set, sizeof(*set), true))
        return -EFAULT;
    sieos_sigset_t o;
    if (op == SIEOS_SIGPENDING)
        sig_set_to_v2((curlwp->sig_pending | current->sig_pending) & curlwp->sig_blocked, &o);
    else if (op == SIEOS_SIGFILLSET)
        sig_set_to_v2(VALID_SIGS, &o);
    else
        return -EINVAL;
    memcpy(set, &o, sizeof(o));
    return 0;
}

long sys2_sigsuspend(const sieos_sigset_t *set)
{
    if (!user_ok(set, sizeof(*set), false))
        return -EFAULT;
    sieos_sigset_t n;
    memcpy(&n, set, sizeof(n));
    struct lwp *l = curlwp;
    l->saved_mask = l->sig_blocked;
    l->saved_mask_valid = true;
    l->sig_blocked = sig_set_from_v2(&n) & ~UNBLOCKABLE;
    static int suspend_chan;
    while (!signal_pending(current))
        sleep_on(&suspend_chan);
    return -EINTR;                                   /* the old mask comes back after delivery */
}

long sys2_sigaltstack(const sieos_stack_t *ss, sieos_stack_t *old)
{
    struct lwp *l = curlwp;
    if (old) {
        if (!user_ok(old, sizeof(*old), true))
            return -EFAULT;
        sieos_stack_t o = { (void *)l->altstack_sp, l->altstack_size, l->altstack_flags, 0 };
        if (on_altstack(l, l->tf->rsp))
            o.ss_flags |= SIEOS_SS_ONSTACK;
        memcpy(old, &o, sizeof(o));
    }
    if (ss) {
        if (!user_ok(ss, sizeof(*ss), false))
            return -EFAULT;
        if (on_altstack(l, l->tf->rsp))
            return -EPERM;
        sieos_stack_t n;
        memcpy(&n, ss, sizeof(n));
        if (n.ss_flags & ~SIEOS_SS_DISABLE)
            return -EINVAL;
        if (n.ss_flags & SIEOS_SS_DISABLE) {
            l->altstack_flags = SIEOS_SS_DISABLE;
            l->altstack_sp = l->altstack_size = 0;
        } else {
            if (n.ss_size < SIEOS_MINSIGSTKSZ)
                return -ENOMEM;
            l->altstack_sp = (uint64_t)n.ss_sp;
            l->altstack_size = n.ss_size;
            l->altstack_flags = 0;
        }
    }
    return 0;
}

long sys2_sigqueue(int pid, int sig, uint64_t value)
{
    struct ksiginfo info = { .code = SIEOS_SI_QUEUE, .pid = current->pid, .uid = current->uid, .value = value };
    return kill_pids(pid, sig, &info);
}

long sys2_sigtimedwait(const sieos_sigset_t *set, sieos_siginfo_t *uinfo, const struct sieos_timespec *timeout)
{
    if (!user_ok(set, sizeof(*set), false) || (uinfo && !user_ok(uinfo, sizeof(*uinfo), true)) ||
        (timeout && !user_ok(timeout, sizeof(*timeout), false)))
        return -EFAULT;
    sieos_sigset_t n;
    memcpy(&n, set, sizeof(n));
    ksigset_t want = sig_set_from_v2(&n) & ~UNBLOCKABLE;
    struct lwp *l = curlwp;
    uint64_t deadline = 0;
    if (timeout) {
        uint64_t ns = (uint64_t)timeout->tv_sec * 1000000000UL + timeout->tv_nsec;
        deadline = ticks + (ns * TIMER_HZ + 999999999UL) / 1000000000UL;
    }
    static int wait_chan;
    for (;;) {
        struct ksiginfo info;
        int sig = dequeue(l, want, &info);
        if (sig) {
            if (uinfo) {
                sieos_siginfo_t si;
                siginfo_to_v2(sig, &info, &si);
                memcpy(uinfo, &si, sizeof(si));
            }
            return sig;
        }
        if (timeout && ticks >= deadline)
            return -EAGAIN;
        /* another signal (not waited for, not blocked) interrupts the wait */
        if (((l->sig_pending | current->sig_pending) & ~(l->sig_blocked & ~UNBLOCKABLE) & ~want) || l->must_exit)
            return -EINTR;
        l->wake_tick = timeout ? deadline : 0;
        sleep_on(&wait_chan);
        l->wake_tick = 0;
    }
}

/* context(GETCONTEXT / SETCONTEXT, ucp) */
long sys2_context(int op, sieos_ucontext_t *ucp, struct trapframe *tf)
{
    struct lwp *l = curlwp;
    if (!user_ok(ucp, sizeof(*ucp), op == SIEOS_GETCONTEXT))
        return -EFAULT;
    static sieos_ucontext_t uc;                      /* under the big kernel lock */
    if (op == SIEOS_GETCONTEXT) {
        struct trapframe t = *tf;                    /* resumes after the call, returning 0 */
        t.rax = 0;
        t.rdx = 0;
        t.rflags &= ~1UL;
        make_ucontext(l, &t, l->sig_blocked, &uc);
        memcpy(ucp, &uc, sizeof(uc));
        return 0;
    }
    if (op != SIEOS_SETCONTEXT)
        return -EINVAL;
    memcpy(&uc, ucp, sizeof(uc));
    if (uc.uc_flags & SIEOS_UC_CPU)
        sig_gregs_to_tf(uc.uc_mcontext.gregs, tf);
    if (uc.uc_flags & SIEOS_UC_SIGMASK)
        l->sig_blocked = sig_set_from_v2(&uc.uc_sigmask) & ~UNBLOCKABLE;
    if (uc.uc_flags & SIEOS_UC_FPU) {
        memcpy(l->fpu.area, &uc.uc_mcontext.fpregs, 512);
        uint32_t *mxcsr = (uint32_t *)(l->fpu.area + 24);
        *mxcsr &= 0xFFFF;                            /* reserved MXCSR bits would fault */
        fpu_restore(&l->fpu);
    }
    if (uc.uc_flags & SIEOS_UC_STACK) {
        /* the "on stack" state follows from the restored rsp */
    }
    l->restart_syscall = false;
    return -EJUSTRETURN;                             /* tf now holds the context */
}

long sys2_lwp_kill(int lwpid, int sig)
{
    if (sig < 0 || sig >= KNSIG)
        return -EINVAL;
    struct lwp *t = lwp_find(current, lwpid);
    if (!t || t->state == LWP_ZOMBIE)
        return -ESRCH;
    if (sig)
        signal_lwp(t, sig, NULL);
    return 0;
}
