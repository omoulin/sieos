/*
 * mk.h - What an SIEOS program can use: system calls, threads, the
 * console, printf, memory allocation, strings. There is no other C library (yet).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "mk/abi.h"
#include "mk/proto.h"
#include "mk/lib.h"

#if defined(__x86_64__)
/* A system call: number in rax, arguments in rdi, rsi, rdx, r10, r8, r9;
 * result in rax. The SYSCALL instruction itself overwrites rcx and r11. */
static inline long syscall6(long n, long a, long b, long c, long d, long e, long f)
{
    register long r10 asm("r10") = d;
    register long r8 asm("r8") = e;
    register long r9 asm("r9") = f;
    asm volatile("syscall" : "+a"(n) : "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return n;
}
#elif defined(__aarch64__)
/* A system call: "svc #0", number in x8, arguments in x0-x5, result in x0
 * (the kernel restores every other register). */
static inline long syscall6(long n, long a, long b, long c, long d, long e, long f)
{
    register long x8 asm("x8") = n;
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    register long x3 asm("x3") = d;
    register long x4 asm("x4") = e;
    register long x5 asm("x5") = f;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
#endif
#define syscall5(n, a, b, c, d, e) syscall6(n, a, b, c, d, e, 0)
#define syscall4(n, a, b, c, d) syscall6(n, a, b, c, d, 0, 0)
#define SYS(n, a, b) syscall5(n, (long)(a), (long)(b), 0, 0, 0)

static inline long sys_debug(const char *s, size_t n)   { return SYS(SYS_DEBUG, s, n); }
static inline __attribute__((noreturn)) void sys_exit(int code) { SYS(SYS_EXIT, code, 0); __builtin_unreachable(); }
static inline long sys_yield(void)                      { return SYS(SYS_YIELD, 0, 0); }
static inline long port_create(const char *name)        { return SYS(SYS_PORT_CREATE, name, 0); }
static inline long port_lookup(const char *name)        { return SYS(SYS_PORT_LOOKUP, name, 0); }
static inline long port_find(const char *name)          { return SYS(SYS_PORT_LOOKUP, name, LOOKUP_NOWAIT); } /* no wait */
static inline long ipc_call(long port, msg_t *m)        { return SYS(SYS_CALL, port, m); }
static inline long ipc_recv(long port, msg_t *m)        { return SYS(SYS_RECV, port, m); }
static inline long ipc_reply(long token, msg_t *m)      { return SYS(SYS_REPLY, token, m); }
static inline long ipc_reply_recv(long token, long port, msg_t *m) { return syscall4(SYS_REPLY_RECV, token, port, (long)m, 0); }
static inline long irq_bind(int irq, long port)         { return SYS(SYS_IRQ_BIND, irq, port); }
static inline long irq_ack(int irq)                     { return SYS(SYS_IRQ_ACK, irq, 0); }
static inline void *map_phys(uint64_t pa, size_t size)  { return (void *)SYS(SYS_MAP_PHYS, pa, size); }
static inline long sys_brk(uintptr_t end)               { return SYS(SYS_BRK, end, 0); }
static inline long sys_info(mk_info_t *in)              { return SYS(SYS_INFO, in, 0); }
static inline long sys_power(int restart)               { return SYS(SYS_POWER, restart, 0); }
static inline long sys_tasks(mk_task_t *t, long max, int after) { return syscall4(SYS_TASKS, (long)t, max, after, 0); }
static inline long sys_clock(void)                      { return SYS(SYS_CLOCK, 0, 0); }      /* ns since boot */
static inline long sys_sleep(uint64_t ns)               { return SYS(SYS_SLEEP, ns, 0); }
static inline __attribute__((noreturn)) void sys_thread_exit(void) { SYS(SYS_THREAD_EXIT, 0, 0); __builtin_unreachable(); }
static inline void *dma_alloc(size_t size, uint64_t *pa) { return (void *)SYS(SYS_DMA_ALLOC, size, pa); }   /* size | DMA_LOW: in the first GiB */
static inline long dma_free(void *va, size_t size)      { return SYS(SYS_DMA_FREE, va, size); }
static inline long sys_spawn(const void *elf, size_t len, const char *args, int uid, int gid, const mk_spawn_t *o)
{ return syscall6(SYS_SPAWN, (long)elf, len, (long)args, uid, gid, (long)o); }
static inline long sys_kill(int pid)                    { return SYS(SYS_KILL, pid, 0); }
static inline long sys_modules(char *buf, size_t size)  { return SYS(SYS_MODULES, buf, size); }
static inline long sys_want(const char *name, long err) { return SYS(SYS_WANT, name, err); }
static inline long sys_wanted(char *buf, size_t size)   { return SYS(SYS_WANTED, buf, size); }
static inline long sys_wake(int tid)                    { return SYS(SYS_WAKE, tid, 0); }
static inline long sys_wait(int pid, int *status, int flags) { return syscall4(SYS_WAIT, pid, (long)status, flags, 0); }
static inline long sys_random(void *buf, size_t n)      { return SYS(SYS_RANDOM, buf, n); }
static inline long sys_ident(int pid, mk_ident_t *id)   { return SYS(SYS_IDENT, pid, id); }
static inline long sys_child_port(long port)            { return SYS(SYS_CHILD_PORT, port, 0); }

/* Answer a message with just a result in w[0]. */
static inline long reply_val(long token, long v)        { msg_t m = { .w = { v } }; return ipc_reply(token, &m); }

#if defined(__x86_64__)
/* I/O ports (drivers only: the kernel refuses them to other programs). */
static inline void outb(uint16_t p, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(uint16_t p, uint16_t v) { asm volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint16_t inw(uint16_t p) { uint16_t v; asm volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl(uint16_t p, uint32_t v) { asm volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
/* x86 keeps stores in order, also toward devices: only the compiler must not reorder. */
#define dma_wmb() asm volatile("" : : : "memory")
/* DMA memory and the caches: x86 devices see the caches, nothing to do. */
static inline void dma_sync(volatile void *p, size_t n) { (void)p; (void)n; asm volatile("" : : : "memory"); }
#elif defined(__aarch64__)
/* Before telling a device to look at memory (a "notify" register): our
 * writes to that memory must be visible to it first. */
#define dma_wmb() asm volatile("dmb sy" : : : "memory")
/* For devices that do not see the caches (the Raspberry Pis' SD
 * controllers, their firmware): write the buffer's cache lines to memory
 * and drop them ("dc civac", allowed to programs by the kernel). Call it
 * before the device reads the buffer (it sees what we wrote), and after it
 * wrote (we see what it wrote, not old cached lines); the CPU must not touch
 * the buffer in between. On coherent devices it is only a little slower. */
static inline void dma_sync(volatile void *p, size_t n)
{
    uint64_t ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4UL << (ctr >> 16 & 15);
    for (uint64_t a = (uint64_t)p & ~(line - 1); a < (uint64_t)p + n; a += line)
        asm volatile("dc civac, %0" : : "r"(a) : "memory");
    asm volatile("dsb sy" : : : "memory");
}
#endif
/* Device registers mapped with map_phys (uncached). */
static inline uint32_t mmio_r32(volatile void *a)            { return *(volatile uint32_t *)a; }
static inline void     mmio_w32(volatile void *a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* SYS_FDT: the firmware's device tree (arm64; -ENOSYS where there is none),
 * where drivers find their devices (lib/fdt.c reads it). */
static inline long sys_fdt(void *buf, size_t size)      { return SYS(SYS_FDT, buf, size); }
/* SYS_SCREEN: the frame buffer a UEFI loader left (-ENOSYS: none). */
static inline long sys_screen(mk_screen_t *s)            { return SYS(SYS_SCREEN, s, 0); }
/* SYS_PCI (x86-64, drivers): a PCI configuration dword at bus << 16 | dev << 11 | fn << 8 | reg. */
static inline uint32_t pci_read(uint32_t a)              { return (uint32_t)syscall4(SYS_PCI, a, 0, 0, 0); }
static inline void pci_write(uint32_t a, uint32_t v)     { syscall4(SYS_PCI, a, v, 1, 0); }

/* lib.c */
int  main(int argc, char **argv);
/* Calls to a named server that survive its restart (init restarts crashed
 * servers). *port caches the port number. If it is stale (-ENOENT: the
 * server ended before our call), the port is found again by name (this
 * waits until the new server is up) and the call sent again: it never
 * reached the old one. If the server died during the call (-EPIPE), it is
 * sent again only if `repeat` (requests that are safe to do twice). */
long call_named(long *port, const char *name, msg_t *m, int repeat);
/* A server started on demand: its answer to init's SVC_MAYSTOP (0: stop now). */
long maystop_answer(const msg_t *m, uint64_t count, uint64_t *seen, int busy);
/* The console requests go to this process's terminal (mk_ident_t.console:
 * "port" or "port#channel", the channel sent in w[3]), else to "console". */
long con_write(const void *buf, size_t n);
long con_read(void *buf, size_t n);
long con_echo(int on);
int  printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void *malloc(size_t n);
void *realloc(void *p, size_t n);
void  free(void *p);
long thread_start(void (*fn)(void *), void *arg, size_t stack);   /* -> thread id */

/* tty.c: typed lines, for servers that are terminals (con, atlas). */
typedef struct tty {
    char line[256], done[256];         /* the line being typed; a complete one, not yet read */
    int len, dlen, echo;               /* dlen -1: no complete line */
    long reader;                       /* the client waiting in CON_READ */
    size_t want;                       /* ... and how much it can take */
    unsigned lines;                    /* lines completed so far */
    void (*out)(struct tty *, const char *, size_t);   /* shows echoed characters */
    void *ctx;
} tty_t;
void tty_init(tty_t *t, void (*out)(tty_t *, const char *, size_t), void *ctx);
void tty_input(tty_t *t, char c);
long tty_read(tty_t *t, long from, size_t want);   /* 0, or -EBUSY */
void tty_deliver(tty_t *t);

/* fs.c: files, through the file server (port "fs"). Errors are -E... */
#include "siefs.h"                     /* siefs_stat_t, siefs_statfs_t, the mode bits */
#define FS_PATH 512                    /* longest path a program may use */
long fs_open(const char *path, int flags, int mode);  /* -> handle; flags: FS_RDONLY... (mk/proto.h) */
long fs_close(long h);
long fs_read(long h, uint64_t off, void *buf, size_t n);
long fs_write(long h, uint64_t off, const void *buf, size_t n);
long fs_truncate(long h, uint64_t size);
long fs_stat(const char *path, siefs_stat_t *st, int nofollow);
long fs_fstat(long h, siefs_stat_t *st);
long fs_readdir(long h, uint64_t *cursor, void *buf, size_t size);
const char *fs_dirent(const char **p, const char *end, uint64_t *ino, int *type, char *name);
long fs_mkdir(const char *path, int mode);
long fs_unlink(const char *path);
long fs_rmdir(const char *path);
long fs_rename(const char *from, const char *to);
long fs_link(const char *from, const char *to);
long fs_symlink(const char *target, const char *path);
long fs_readlink(const char *path, char *buf, size_t size);
long fs_chmod(const char *path, int mode);
long fs_chown(const char *path, int uid, int gid);
long fs_getxattr(const char *path, const char *name, void *buf, size_t size);
long fs_setxattr(const char *path, const char *name, const void *val, size_t len);
long fs_listxattr(const char *path, char *buf, size_t size);
long fs_rmxattr(const char *path, const char *name);
long fs_find(const char *name, const char *value, uint64_t *cursor, char *buf, size_t size);
long fs_statfs(siefs_statfs_t *st);
long fs_sync(void);
long fs_chdir(const char *path);
const char *fs_getcwd(void);
long run(const char *path, const char *args);         /* start a program from the disk, wait -> its status */
long spawn_file(const char *path, const char *args, int uid, int gid, const mk_spawn_t *o);  /* -> pid */

/* acct.c: the account files (docs/accounts.md). */
typedef struct { char name[32], home[64], shell[64], full[64]; int uid, gid; } acct_user_t;
char *file_get(const char *path, size_t *len);            /* whole file, 0-terminated, malloc'd */
int  acct_next(char **p, char **fields, int nf);          /* next record's fields, or -1 */
int  acct_user(const char *name, int uid, acct_user_t *u);/* by name, or (name 0) by uid */
int  acct_groups(const char *name, mk_groups_t *g);      /* groups listing `name` as a member */
const char *acct_group_name(int gid, char *buf, size_t size);
