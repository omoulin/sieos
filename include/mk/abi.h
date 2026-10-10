/*
 * mk/abi.h - The contract between the SIEOS kernel and user programs.
 *
 * A program asks the kernel for something with the SYSCALL instruction:
 *   rax = the service number (SYS_...), arguments in rdi, rsi, rdx, r10, r8, r9;
 *   the result comes back in rax: >= 0 on success, -E... on error.
 *
 * The kernel offers very little: processes and threads, memory, time,
 * message passing (IPC) and, for drivers, access to devices. Everything
 * else (the terminal, the disk, files, users) is done by ordinary programs
 * ("servers") that other programs talk to with messages.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

enum {
    SYS_DEBUG,         /* (str, len): print to the kernel log (serial port)              */
    SYS_EXIT,          /* (code): end the calling process, all its threads               */
    SYS_YIELD,         /* (): let another thread run                                     */
    SYS_PORT_CREATE,   /* (name or 0) -> port: create a port to receive messages         */
    SYS_PORT_LOOKUP,   /* (name, flags) -> port: find a named port (waits until it exists;
                          LOOKUP_NOWAIT: -ENOENT at once instead)                       */
    SYS_CALL,          /* (port, msg*): send a message, sleep until the reply            */
    SYS_RECV,          /* (port, msg*) -> sender token, or 0 for interrupts              */
    SYS_REPLY,         /* (token, msg*): answer the message received from token          */
    SYS_IRQ_BIND,      /* (irq, port): deliver this interrupt to port (drivers)          */
    SYS_IRQ_ACK,       /* (irq): the interrupt is handled, re-enable it (drivers)        */
    SYS_MAP_PHYS,      /* (phys, size) -> address: map device memory, never RAM (drivers)*/
    SYS_BRK,           /* (end) -> end: grow/shrink the heap (0: just ask)               */
    SYS_INFO,          /* (mk_info_t*): system information                               */
    SYS_POWER,         /* (0 = power off, 1 = restart): root only                        */
    SYS_TASKS,         /* (mk_task_t*, max, after) -> n: the processes with pid > after,
                          in pid order; call again with the last pid to get more         */
    SYS_CLOCK,         /* () -> nanoseconds since boot                                   */
    SYS_SLEEP,         /* (ns): sleep at least this long                                 */
    SYS_THREAD_CREATE, /* (entry, stack top, arg) -> thread id: entry(arg) runs in a new
                          thread of this process, on that stack                          */
    SYS_THREAD_EXIT,   /* (): end this thread (the process ends with its last one)       */
    SYS_DMA_ALLOC,     /* (size, uint64_t *phys) -> address: physically contiguous,
                          zeroed memory for a device to read or write (drivers)          */
    SYS_DMA_FREE,      /* (address, size): give it back (drivers)                        */
    SYS_SPAWN,         /* (elf, len, args, uid, gid, mk_spawn_t * or 0) -> pid: start a
                          program from an ELF image in memory; only root may choose
                          another uid/gid (-1, -1: the caller's); options below          */
    SYS_WAIT,          /* (pid or -1, int *status, flags) -> pid: wait for a child to end;
                          WAIT_NOHANG: return 0 at once if none has ended yet            */
    SYS_REPLY_RECV,    /* (token, port, msg*) -> next token: SYS_REPLY then SYS_RECV in one
                          call, with the same msg_t (sbuf: the reply; rbuf: the next
                          message); the fast path for servers                            */
    SYS_RANDOM,        /* (buf, len) -> len: unpredictable bytes (at most 4096 per call);
                          -EAGAIN until the kernel has gathered enough entropy           */
    SYS_IDENT,         /* (pid or 0 for oneself, mk_ident_t *): who a process is         */
    SYS_CHILD_PORT,    /* (port): when a child ends, the kernel sends a "token 0"
                          message on this port (w[1] has NOTE_CHILD), so a server learns
                          of it in its own ipc_recv loop (then SYS_WAIT with WAIT_NOHANG)*/
    SYS_KILL,          /* (pid): end a process (its exit status: EXIT_KILLED). Root may end
                          any but the supervisor; others only their own processes        */
    SYS_MODULES,       /* (buf, size) -> bytes: the boot modules' names, "con\0vblk\0...",
                          in order (their numbers for SPAWN_MODULE); supervisor only     */
    SYS_SET_FS,        /* (address): this thread's thread pointer, for thread-local storage
                          (x86-64: the FS base; arm64: TPIDR_EL0)                       */
    SYS_WANT,          /* (name, error): supervisor only. error 0: the name's server is
                          started on demand: a SYS_PORT_LOOKUP that finds no such port
                          tells the supervisor (NOTE_WANT on its child port) instead of
                          waiting for nobody. error < 0: the threads waiting for the name
                          stop waiting and get that error (the server cannot start)    */
    SYS_WANTED,        /* (buf, size) -> bytes: the on-demand names some thread waits
                          for right now, "net\0sia\0"; supervisor only                  */
    SYS_WAKE,          /* (thread id): end the SYS_SLEEP of another thread of this
                          process early (it returns 1); if that thread is not asleep,
                          its next sleep ends at once: a wake-up is never lost          */
    SYS_FDT,           /* (buf, size) -> bytes: a copy of the firmware's device tree (on
                          machines that have one: arm64), where drivers find their
                          devices (lib/fdt.c); -ENOSYS without one                       */    SYS_SCREEN,        /* (mk_screen_t *) -> 0: the screen the firmware had set up when
                          SIEOS started (a UEFI loader's frame buffer); -ENOSYS: none    */
    SYS_PCI,           /* (address, value, write) -> the dword read, or 0: PCI configuration
                          through ports 0xCF8/0xCFC (x86-64), drivers only. The kernel does
                          the two accesses together, so drivers scanning the bus at the
                          same time never mix their index and data. address = bus << 16 |
                          device << 11 | function << 8 | register. -ENOSYS: not x86-64     */
};
/* SYS_SCREEN's answer. pitch: pixels per row; format: 0 = bytes R,G,B,x in
 * memory, 1 = bytes B,G,R,x (0x00RRGGBB words). */
typedef struct { uint64_t pa, size; uint32_t width, height, pitch, format; } mk_screen_t;
#define LOOKUP_NOWAIT 1              /* SYS_PORT_LOOKUP: do not wait (and wake nobody) */
#define EXIT_KILLED (-9)             /* the exit status of a process ended by SYS_KILL */
#define WAIT_NOHANG 1
/* A "token 0" message (from the kernel, not a client): w[0] = the interrupts
 * that occurred (one bit each), w[1] = other events: */
#define NOTE_CHILD  1                /* a child process ended */
#define NOTE_WANT   2                /* someone waits for an on-demand server (SYS_WANT) */

/* SYS_IRQ_BIND: or-ed with the irq number for a level-triggered, active-high
 * line: a PCI device's interrupt as the chipset routes it to an ISA IRQ. */
/* SYS_DMA_ALLOC's size may carry DMA_LOW: the memory must be in the first
 * GiB of RAM (the Raspberry Pis' firmware and older devices see no more). */
#define DMA_LOW (1UL << 62)
/* SYS_MAP_PHYS's size may carry MAP_WC: memory a device only reads, like a
 * frame buffer: writes may be gathered and reordered (much faster drawing;
 * arm64; x86 maps it uncached as usual). */
#define MAP_WC  (1UL << 62)
#define IRQ_LEVEL 0x100
/* Interrupt numbers: on x86-64, ISA IRQs 0-15 or I/O APIC lines ("GSIs") 16+;
 * on arm64, the GIC's shared lines ("SPI" n: what a device tree's
 * "interrupts = <0 n flags>" says). w[0] of a token 0 message has bit
 * (irq % 64) set for each interrupt that occurred. */
#define IRQ_BIT(irq) (1UL << ((irq) & 63))
/* A line may be shared by up to 4 drivers (PCI lines are; a fifth gets
 * -EBUSY). Each bound driver gets every interrupt of the line, checks its
 * own device, and must answer SYS_IRQ_ACK at once, even when the interrupt
 * was not its device's and even while idle: the kernel unmasks the line
 * only when all of them have. */

/* Errors (the POSIX numbers, returned negated). */
enum { EPERM = 1, ENOENT = 2, ESRCH = 3, EIO = 5, EBADF = 9, ECHILD = 10, EAGAIN = 11, ENOMEM = 12, EACCES = 13,
       EFAULT = 14, EBUSY = 16, EEXIST = 17, ENOTDIR = 20, EISDIR = 21, EINVAL = 22, EMFILE = 24,
       ENOSPC = 28, EPIPE = 32, ENAMETOOLONG = 36, ENOSYS = 38, ENOTEMPTY = 39, ELOOP = 40 };

/*
 * A message. Small values travel in w[]; larger data is copied by the
 * kernel straight from the sender's sbuf into the receiver's rbuf (one copy,
 * no buffering in the kernel).
 *
 *   SYS_CALL:  w[] and sbuf/slen are sent; the reply's w[] and data come
 *              back into the same msg_t (data into rbuf, its size in rlen).
 *   SYS_RECV:  fills w[], copies the data into rbuf (size in rlen) and tells
 *              who sent it: pid, uid and gid are set by the kernel, so a
 *              server can trust them to decide what the caller may do.
 *   SYS_REPLY: w[] and sbuf/slen go back to the caller.
 */
typedef struct {
    uint64_t w[4];               /* by convention w[0] = operation, or result */
    const void *sbuf; uint64_t slen;
    void *rbuf;       uint64_t rlen;
    int32_t pid, uid, gid, _pad; /* sender identity, filled in by the kernel */
} msg_t;

/* Identity. The kernel stamps uid and gid (the primary group) on every
 * message; the other groups a process belongs to are asked with SYS_IDENT
 * (a server does so only when they matter: a process's identity never
 * changes, so the answer may be kept). */
#define NGROUPS 16
typedef struct { int32_t n, g[NGROUPS]; } mk_groups_t;
/* console: the process's terminal, "port" or "port#channel" (empty: the
 * system console, port "console"). Children inherit it; the client library
 * sends its console requests there, the channel in w[3]. That is how a
 * shell in a window of the graphical desktop talks to that window. */
typedef struct { int32_t pid, uid, gid; mk_groups_t groups; char console[16]; } mk_ident_t;

/* SYS_SPAWN's options. The first process the kernel starts (init, from the
 * first boot module) is the "supervisor": it alone may give device rights
 * and start the other boot modules; it also adopts every process whose
 * parent ended, so it learns when they end. */
typedef struct {
    int32_t flags, parent;           /* parent: with SPAWN_PARENT */
    mk_groups_t groups;              /* with SPAWN_GROUPS */
    char console[16];                /* with SPAWN_CONSOLE (see mk_ident_t) */
    uint64_t quota;                  /* with SPAWN_QUOTA: the most memory it may use, bytes */
} mk_spawn_t;
#define SPAWN_GROUPS 1               /* root: the child's groups (else the caller's, or none
                                        when root starts another uid) */
#define SPAWN_PARENT 2               /* root: the child's parent is `parent` (it gets the
                                        child's end), not the caller */
#define SPAWN_DRIVER 4               /* supervisor: device rights (I/O ports, interrupts,
                                        device memory, DMA) */
#define SPAWN_MODULE 8               /* supervisor: elf = 0, len = a boot module's number */
#define SPAWN_CONSOLE 16             /* the child's console (else its parent's) */
#define SPAWN_QUOTA 32               /* a memory limit for the child (pages it uses, DMA
                                        included). Without it, a child gets its parent's
                                        limit; nobody may give a child more than its own */

/* Thread states (and a process's: its most active thread's). */
enum { TS_FREE, TS_READY, TS_RUN, TS_SEND, TS_RECV, TS_REPLY, TS_LOOKUP, TS_DEAD,
       TS_SLEEP, TS_WAIT };

/* What SYS_INFO and SYS_TASKS return. */
typedef struct {
    uint64_t uptime_ns;              /* time since boot */
    uint64_t pages_total, pages_free;/* memory, in 4 KiB pages */
    int32_t self_pid, self_uid;
    int32_t nprocs, nthreads, ncpus;
    uint32_t hwcap;                  /* what the processor can do, HWCAP_* below */
} mk_info_t;
/* Processor features a program may use (the kernel reads them once at boot;
 * some, like the AArch64 ID registers, are not readable from user mode). */
enum {
    HWCAP_AVX2    = 1 << 0,          /* x86-64: AVX2 (and the kernel saves AVX state) */
    HWCAP_FMA     = 1 << 1,          /* x86-64: fused multiply-add */
    HWCAP_AVXVNNI = 1 << 2,          /* x86-64: AVX-VNNI int8 dot products */
    HWCAP_DOTPROD = 1 << 8,          /* AArch64: SDOT/UDOT (ARMv8.2; Pi 5 yes, Pi 4 no) */
    HWCAP_LSE     = 1 << 9,          /* AArch64: LSE atomics (ARMv8.1) */
};
typedef struct {
    int32_t pid, uid, state, cpu;    /* cpu: where its first thread runs (or last ran) */
    int32_t threads, ppid;           /* ppid: its parent (0: started by the kernel) */
    uint64_t pages;
    char name[16];
    uint64_t quota;                  /* its memory limit in pages (0: none) */
} mk_task_t;
