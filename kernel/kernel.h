/*
 * kernel.h - What the kernel's portable core shares: processes, threads,
 * the per-CPU data, the locks, and every core function. The processor's own
 * types, layout and helpers come from arch_defs.h (kernel/arch/NAME/), its
 * functions from arch.h.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "mk/abi.h"
#include "mk/lib.h"
#include "arch_defs.h"
#include "arch.h"

#ifdef ARCH_KSTACK
#define KSTACK    ARCH_KSTACK         /* (the processor wants another size: arch_defs.h) */
#else
#define KSTACK    PAGE                /* each thread: its task_t, then its kernel stack. 4 KiB:
                                         the deepest use measured is 904 bytes (make STACKCHECK=1
                                         runs every kind of system call, fault and interrupt),
                                         under 1.5 KiB with an NMI on top */
#endif
#define MAXTASK   (1 << 20)           /* a sanity cap on threads; memory is the real limit */
#define SLICE_NS  10000000UL          /* a time slice: 10 ms, when others wait for the CPU */
#define NEVER     (~0UL)

/* A ticket lock: each CPU takes a number and waits for its turn (fair).
 * The kernel runs with interrupts off, so an interrupt never wants a lock
 * its CPU holds. While it waits, a CPU still does what its architecture
 * needs (arch_lock_wait: answering TLB shootdowns, which the holder may be
 * waiting for). Which locks exist, and their order: main.c. */
typedef struct { volatile uint32_t next, owner; } spinlock_t;
#ifndef smp_mb
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)   /* a full barrier: earlier stores before later loads */
#endif
#ifndef store_mb                     /* store v (after the earlier stores), then a full barrier */
#define store_mb(p, v) do { __atomic_store_n((p), (v), __ATOMIC_RELEASE); smp_mb(); } while (0)
#endif

/* A process: an address space, an identity, and its threads. */
typedef struct proc {
    int pid, ppid, uid, gid;    /* (ppid: under proc_lock) */
    mk_groups_t groups;         /* the other groups it belongs to (SYS_IDENT) */
    long child_port;            /* SYS_CHILD_PORT: told when a child ends (0: nobody) */
    int driver;                 /* may use I/O ports, interrupts, device memory */
    int super;                  /* the supervisor (init): grants device rights, starts boot
                                   modules, adopts orphans; never killed */
    int bg;                     /* background work: its threads prefer E-cores (SPAWN_BACKGROUND) */
    spinlock_t lock;            /* its threads, its ports, its heap and mappings, exiting */
    int nthreads;
    volatile int exiting;       /* ending: each of its threads ends on its way out of the kernel */
    int zombie, status;         /* ended, waiting for its parent's SYS_WAIT (under proc_lock) */
    uint64_t as;                /* its address space: physical address of its PML4 */
    char name[16];
    char console[16];           /* its terminal (mk_ident_t), inherited by children */
    uint64_t brk_base, brk;     /* its heap */
    uint64_t mmio;              /* where the next device mapping or DMA buffer goes */
    uint64_t pages;             /* memory it uses, in pages */
    uint64_t quota;             /* the most it may use, in pages (0: no limit; SPAWN_QUOTA) */
    struct task *threads;       /* its threads (linked by tnext) */
    struct port *ports;         /* the ports it owns */
    struct proc *hnext;         /* in the pid hash table */
    struct proc *anext, *aprev; /* in the list of all processes (pid order) */
    int pgid;                   /* its process group (SPAWN_PGROUP; 0: none) */
    uint32_t sigign, sigcatch;  /* the signals it ignores, and those it catches (SYS_SIGNAL) */
    uint64_t sigentry;          /* ... its function for those */
    int sigtid;                 /* ... the thread that runs it (the one that set it) */
    struct region *regions;     /* its anonymous memory (SYS_MMAP, vm.c; under lock) */
    struct proc *asnext;        /* in vm.c's address space table */
    uint64_t note;              /* SYS_NOTE_WORD: the word SYS_NOTE adds to (0: none) */
} proc_t;

/* A thread ("task"): what the scheduler runs. Its task_t sits at the bottom
 * of its kernel stack: one allocation for both. */
typedef struct task {
    uint64_t ksp;               /* its saved kernel stack pointer, while switched out */
    uint64_t kstack;            /* bottom of its kernel stack (= this task_t) */
    proc_t *proc;
    int tid, cpu;               /* cpu: where it runs or ran */
    volatile int state;         /* TS_... (mk/abi.h) */
    volatile int on_cpu;        /* a CPU is still on its stack (until finish_switch): no other may run it */
    int bg;                     /* background work: prefers E-cores (SYS_PLACE) */
    int move;                   /* leave this CPU at the next switch, for one that suits it better */
    long ret;                   /* result of the system call it is sleeping in */
    char wname[16];             /* TS_LOOKUP: the port name it waits for */
    uint64_t wake;              /* TS_SLEEP: when to wake (ticks) */
    int wpid;                   /* TS_WAIT: the child it waits for (-1: any) */
    int woken;                  /* SYS_WAKE came while it was not asleep: its next sleep ends at once */
    msg_t *umsg;                /* its msg_t while in an IPC (user address) */
    struct port *wport;         /* TS_SEND/TS_RECV: the port it waits on (0: taken off its queue) */
    long wport_id;              /* ... that port's number */
    struct proc *volatile server;   /* TS_REPLY: the process that must reply (any of its threads
                                   may); whoever sets it to 0 (under the tid bucket's lock)
                                   wakes the thread (ipc.c) */
    struct task *next;          /* in a run queue, a port's queue, or a wait list */
    struct task *tnext;         /* in its process's list of threads */
    struct task *hnext;         /* in the thread id hash table */
    void *fpu;                  /* its x87/SSE/AVX registers, once it used them (fpu.c) */
    uint64_t fs_base;           /* its FS base: thread-local storage (SYS_SET_FS) */
    volatile uint32_t sigpend;  /* caught signals waiting for it (one bit each) */
    uint32_t gen;               /* the calls it gave up (mod 2^31): in its reply tokens (ipc.c) */
    struct sigctx *sigctx;      /* running a signal's function: what it was doing (task.c) */
    uint64_t clear_tid;         /* SYS_THREAD_SELF: 0 written there, and a futex waiter woken, at its end */
} task_t;
/* A caught signal waits for t, and t is not already in a signal's function. */
#define sig_due(t) ((t)->sigpend && !(t)->sigctx)
/* The fast test, where a thread waits or leaves the kernel: nonzero if its
 * process may be ending or a signal may be due (stop_why then says). */
#define may_stop(t) ((t)->proc->exiting | (t)->sigpend)

/* One per processor. The architecture's part comes first (its entry code
 * reaches it at fixed offsets); this_cpu() (arch_defs.h) finds the running
 * CPU's. */
enum { CPU_P, CPU_E };          /* a core's kind: performance (or all alike), efficiency */
typedef struct cpu {
    arch_cpu_t a;
    int id;
    volatile int online;        /* set by the CPU itself once started */
    int type;                   /* CPU_P or CPU_E (sched_cpus: CPU_P for all unless hybrid) */
    volatile int idle;          /* in idle(), about to sleep or asleep: a waker may claim it (1 -> 0) */
    volatile uint32_t tlb_want, tlb_done;   /* shootdowns asked of it, and answered (smp.c) */
    task_t *running;            /* the running thread, 0 while idle ("cur" below) */
    volatile int running_bg;    /* ... it is background work (for other CPUs to see) */
    volatile int evict;         /* asked to move its running thread to a CPU that suits it */
    volatile int park;          /* suspending (task.c): 1 asked, 2 its threads gone to CPU 0, 3 stopped */
    task_t *last;               /* the thread it just switched away from (finish_switch) */
    int last_dead, last_ready;  /* ... it exited: free its stack; it moves: make it ready */
    uint64_t idle_ksp;          /* the idle loop's saved stack pointer */
    uint64_t cur_as;            /* the address space loaded now */
    uint64_t slice_end;         /* (ticks) when the running thread's time slice ends */
    task_t *handoff;            /* ready_first's thread: it runs next and continues the slice */
    uint64_t armed;             /* (ticks) when this CPU's timer is set to fire, or NEVER */
    spinlock_t rq_lock;         /* its run queue: */
    task_t *rq_head, *rq_tail;  /*   the ready threads waiting for this CPU */
    volatile int nready;        /*   how many */
    uint64_t rand_acc;          /* interrupt times, gathered for the random pool (random.c) */
    int rand_n;
#ifdef STACKCHECK
    uint64_t *stack_lo, *stack_hi;  /* its idle stack */
#endif
} cpu_t;
#define cur (this_cpu()->running)

void spin_wait(spinlock_t *l, uint32_t t);   /* (main.c: until our turn comes) */
static inline void spin_lock(spinlock_t *l)
{
    uint32_t t = __atomic_fetch_add(&l->next, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != t) spin_wait(l, t);
}
static inline void spin_unlock(spinlock_t *l) { __atomic_store_n(&l->owner, l->owner + 1, __ATOMIC_RELEASE); }

/* The boot modules (main.c): kept for the supervisor to start (again). */
typedef struct { uint64_t pa, len; char name[16]; } module_t;
extern module_t modules[MAXMOD];
extern int nmodules;

/* kprintf.c */
void kprintf(const char *fmt, ...);
long log_read(char *to, size_t size, uint64_t *pos);   /* SYS_LOG (kprintf.c) */
__attribute__((noreturn)) void panic(const char *fmt, ...);
/* main.c: the processors (filled by cpu_init and smp_start), the boot options */
extern cpu_t *cpus[MAXCPU];
extern int ncpu;
static inline int ncpus(void) { return __atomic_load_n(&ncpu, __ATOMIC_ACQUIRE); }  /* (grows while CPUs start) */
extern int opt_pcores;          /* boot option "pcores=N": CPUs 0..N-1 are P-cores, the others E-cores */
/* mem.c */
extern uint64_t pages_total, pages_free;
void mem_init(boot_info_t *bi);
void mem_boot_done(void);
uint64_t page_alloc(uint64_t n);
uint64_t page_alloc_low(uint64_t n, uint64_t limit);   /* below a physical address (DMA_LOW) */
void page_free(uint64_t pa, uint64_t n);
int  is_ram(uint64_t pa, uint64_t size);
void *kzalloc(size_t n);
void kfree(void *p);
#define kmap_dev(pa, size) kmap(pa, size, VM_UC | VM_NX)
void vm_unmap(proc_t *p, uint64_t va, uint64_t end);
void vm_free(uint64_t as);
void quarantine_release(void);
int  vm_copy(uint64_t das, void *dst, uint64_t sas, const void *src, uint64_t n);
int  vm_load(uint64_t das, void *dst, uint64_t sas, const void *src, uint64_t n);
/* task.c */
extern spinlock_t proc_lock;    /* the process table: pid hash, all_procs, parents and children */
extern proc_t *all_procs;
extern int nprocs, nthreads;
proc_t *proc_new(uint64_t src_as, const uint8_t *elf, uint64_t len, const char *args, int uid, int gid,
                 const mk_groups_t *groups, int driver, uint64_t quota, long *err);
int  proc_start(proc_t *p, int ppid);
long thread_create(uint64_t entry, uint64_t stack, uint64_t arg);
void thread_exit(void) __attribute__((noreturn));
void proc_exit(int code) __attribute__((noreturn));
long proc_kill(long pid, int sig);  /* SYS_KILL */
long sig_set(int sig, uint64_t entry);   /* SYS_SIGNAL */
long sig_return(regs_t *r);     /* SYS_SIGRETURN */
void sig_deliver(task_t *t);
extern proc_t *super;
long proc_wait(int pid, int *status, int flags);
task_t *task_lock(int tid);     /* the thread, with its bucket of the tid hash locked, or 0 */
void task_unlock(task_t *t);
proc_t *proc_find(int pid);     /* (under proc_lock) */
int  blocked(task_t *t, int state);
long stop_why(task_t *t);       /* -EPIPE: its process is ending; -EINTR: a signal is due; else 0 */
void ready(task_t *t);
void ready_first(task_t *t);
void schedule(void);
void preempt(void);
long sleep_ns(uint64_t ns);
long sleep_wake(int tid);
void thread_wake(int tid);      /* sleep_wake for any process's thread (futexes) */
void wake_sleepers(void);
void idle(void) __attribute__((noreturn));
void kernel_exit(void);
void timer_update(cpu_t *c);
void core_tick(cpu_t *c, int timer);
void sched_cpus(void);          /* every CPU has started: their kinds, the boot line */
extern int ecores;              /* how many E-cores (0: all cores alike) */
long place(int tid, int bg);    /* SYS_PLACE */
long suspend(void);             /* SYS_POWER POWER_SLEEP: the others parked, CPU 0 sleeps (arch_suspend) */
/* Measuring stack depth (make STACKCHECK=1): stacks are filled with a
 * pattern when created; the deepest point ever reached is the lowest word
 * that no longer holds it. Reported at power off. */
#ifdef STACKCHECK
void stack_paint(void *lo, void *hi);
void stack_report(void);
#endif
/* ipc.c */
long port_create(const char *name);
long port_lookup(const char *name, int flags);
long port_want(const char *name, long err);
long port_wanted(uint64_t buf, uint64_t size);
long ipc_call(long port, msg_t *um);
long ipc_recv(long port, msg_t *um);
long ipc_reply(long token, msg_t *um);
long ipc_reply_recv(long token, long port, msg_t *um);
long irq_bind(int irq, long port);
long irq_ack(int irq);
void irq_raise(int irq);
void port_notify(long port, proc_t *owner, uint64_t note);
long child_port(long port);
int  ipc_unblock(task_t *t, int state);
int  ipc_unreply(task_t *t);
void ipc_exit(proc_t *p);
/* vm.c */
int  vm_fault(uint64_t va, int access);   /* a page fault (access: PROT_READ/WRITE/EXEC) -> 1: handled */
int  vm_fault_as(uint64_t as, uint64_t va, int write);   /* the kernel's copies (mem.c) */
void vm_as_add(proc_t *p);
void vm_as_del(proc_t *p);
void vm_regions_free(proc_t *p);
void vm_thread_end(task_t *t);
long vm_mmap(uint64_t addr, uint64_t size, uint64_t flags);
long vm_munmap(uint64_t addr, uint64_t size, uint64_t flags);
long vm_mprotect(uint64_t addr, uint64_t size, uint64_t prot);
long futex(int op, uint64_t uaddr, uint64_t val, uint64_t ns);
long shm_create(uint64_t id, uint64_t size);
long shm_hold(uint64_t id, int pid);
long shm_close(uint64_t id);
long shm_map(uint64_t id, uint64_t addr, uint64_t flags);
uint64_t vm_shm_page(uint64_t va, void **o, uint64_t *off);
int  vm_shm_share(proc_t *c, uint64_t va, void *o, uint64_t off, uint64_t flags);
long note(int pid);
uint64_t note_bump(proc_t *t);          /* (under proc_lock) a process's note word, bumped -> its key */
void note_wake(uint64_t key);           /* ... its waiters woken (no lock held) */
long note_word(uint64_t uaddr);
void wall_init(void);                   /* syscall.c: the wall clock from the clock chip, at boot */
#define PN_SHARE 2                      /* proc_new's driver argument: SPAWN_SHARE */
/* random.c */
void rand_init(void);
void rand_event(uint64_t x);
long rand_get(uint64_t ubuf, uint64_t n);
