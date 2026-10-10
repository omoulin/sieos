/*
 * kernel.h - What the kernel's portable core shares: processes, threads,
 * the per-CPU data, the lock, and every core function. The processor's own
 * types, layout and helpers come from arch_defs.h (kernel/arch/NAME/), its
 * functions from arch.h.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
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

/* A process: an address space, an identity, and its threads. */
typedef struct proc {
    int pid, ppid, uid, gid;
    mk_groups_t groups;         /* the other groups it belongs to (SYS_IDENT) */
    long child_port;            /* SYS_CHILD_PORT: told when a child ends (0: nobody) */
    int driver;                 /* may use I/O ports, interrupts, device memory */
    int super;                  /* the supervisor (init): grants device rights, starts boot
                                   modules, adopts orphans; never killed */
    int nthreads;
    int exiting, zombie, status;/* ending; ended, waiting for its parent's SYS_WAIT */
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
} proc_t;

/* A thread ("task"): what the scheduler runs. Its task_t sits at the bottom
 * of its kernel stack: one allocation for both. */
typedef struct task {
    uint64_t ksp;               /* its saved kernel stack pointer, while switched out */
    uint64_t kstack;            /* bottom of its kernel stack (= this task_t) */
    proc_t *proc;
    int tid, state, cpu;        /* state: TS_... (mk/abi.h); cpu: where it runs or ran */
    long ret;                   /* result of the system call it is sleeping in */
    char wname[16];             /* TS_LOOKUP: the port name it waits for */
    uint64_t wake;              /* TS_SLEEP: when to wake (ticks) */
    int wpid;                   /* TS_WAIT: the child it waits for (-1: any) */
    int woken;                  /* SYS_WAKE came while it was not asleep: its next sleep ends at once */
    msg_t *umsg;                /* its msg_t while in an IPC (user address) */
    struct port *wport;         /* TS_SEND/TS_RECV: the port it waits on */
    struct proc *server;        /* TS_REPLY: the process that must reply (any of its threads may) */
    struct task *next;          /* in the run queue, a port's queue, or a wait list */
    struct task *tnext;         /* in its process's list of threads */
    struct task *hnext;         /* in the thread id hash table */
    void *fpu;                  /* its x87/SSE/AVX registers, once it used them (fpu.c) */
    uint64_t fs_base;           /* its FS base: thread-local storage (SYS_SET_FS) */
} task_t;

/* One per processor. The architecture's part comes first (its entry code
 * reaches it at fixed offsets); this_cpu() (arch_defs.h) finds the running
 * CPU's. */
typedef struct cpu {
    arch_cpu_t a;
    int id;
    volatile int online;        /* set by the CPU itself once started */
    int idle;                   /* halted in idle(), waiting for work */
    volatile int tlb_req;       /* another CPU asks us to flush our TLB */
    task_t *running;            /* the running thread, 0 while idle ("cur" below) */
    task_t *dead;               /* a thread that just exited: free its stack later */
    uint64_t idle_ksp;          /* the idle loop's saved stack pointer */
    uint64_t cur_as;            /* the address space loaded now */
    uint64_t slice_end;         /* (ticks) when the running thread's time slice ends */
    task_t *handoff;            /* ready_first's thread: it continues the slice */
    uint64_t armed;             /* (ticks) when this CPU's timer is set to fire, or NEVER */
#ifdef STACKCHECK
    uint64_t *stack_lo, *stack_hi;  /* its idle stack */
#endif
} cpu_t;
#define cur (this_cpu()->running)

/* A ticket lock: each CPU takes a number and waits for its turn (fair). */
typedef struct { volatile uint32_t next, owner; } spinlock_t;
static inline void spin_lock(spinlock_t *l)
{
    uint32_t t = __atomic_fetch_add(&l->next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != t) arch_relax();
}
static inline void spin_unlock(spinlock_t *l) { __atomic_store_n(&l->owner, l->owner + 1, __ATOMIC_RELEASE); }

/* The boot modules (main.c): kept for the supervisor to start (again). */
typedef struct { uint64_t pa, len; char name[16]; } module_t;
extern module_t modules[MAXMOD];
extern int nmodules;

/* kprintf.c */
void kprintf(const char *fmt, ...);
__attribute__((noreturn)) void panic(const char *fmt, ...);
/* main.c: the processors (filled by cpu_init and smp_start), the kernel lock */
extern cpu_t *cpus[MAXCPU];
extern int ncpu;
void klock(void);
void kunlock(void);
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
void vm_unmap(uint64_t as, uint64_t va);
void vm_free(uint64_t as);
void quarantine_release(void);
int  vm_copy(uint64_t das, void *dst, uint64_t sas, const void *src, uint64_t n);
int  vm_load(uint64_t das, void *dst, uint64_t sas, const void *src, uint64_t n);
/* task.c */
extern proc_t *all_procs;
extern int nprocs, nthreads;
int  proc_spawn(uint64_t src_as, const uint8_t *elf, uint64_t len, const char *args, int uid, int gid,
                const mk_groups_t *groups, int driver, uint64_t quota);
long thread_create(uint64_t entry, uint64_t stack, uint64_t arg);
void thread_exit(void) __attribute__((noreturn));
void proc_exit(int code) __attribute__((noreturn));
long proc_kill(int pid);
extern proc_t *super;
long proc_wait(int pid, int *status, int flags);
task_t *task_find(int tid);
proc_t *proc_find(int pid);
void ready(task_t *t);
void ready_first(task_t *t);
void schedule(void);
void preempt(void);
void reap(void);
long sleep_ns(uint64_t ns);
long sleep_wake(int tid);
void wake_sleepers(void);
int  runq_empty(void);
void idle(void) __attribute__((noreturn));
void kernel_exit(void);
void timer_update(cpu_t *c);
void core_tick(cpu_t *c, int timer);
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
/* random.c */
void rand_init(void);
void rand_event(uint64_t x);
long rand_get(uint64_t ubuf, uint64_t n);
void ipc_unblock(task_t *t);
void ipc_exit(proc_t *p);
