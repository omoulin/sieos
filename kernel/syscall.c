/*
 * syscall.c - Process, credential and information calls used by the ABI v2
 * dispatcher (syscall2.c), and helpers shared with it.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "power.h"
#include "proc.h"
#include "mm.h"
#include "vm.h"
#include "fs.h"
#include "tty.h"
#include "poll.h"
#include "display.h"
#include "net.h"

char sys_hostname[65] = "sieos";

static bool uok(const void *p, size_t n, bool write)
{
    return user_range_ok(current->pml4, (uint64_t)p, n ? n : 1, write);
}

static int fetch_str(const char *u, char *k, size_t max)
{
    long len = user_strlen(current->pml4, u, max);
    if (len < 0)
        return len;
    memcpy(k, u, len + 1);
    return 0;
}

static struct file *getfile(int fd)
{
    if (fd < 0 || fd >= NOFILE)
        return NULL;
    return current->ofile[fd];
}


static bool is_root(void)
{
    return current->euid == 0;
}

/* ------------------------------------------------------------------ */
/* Processes                                                           */
/* ------------------------------------------------------------------ */

static long sys_exit(long code)
{
    proc_exit((code & 0xFF) << 8);
}

/* The number of entries of a NULL-terminated user vector; -E2BIG past max. */
static long count_strvec(char *const *uvec, long max)
{
    long n = 0;
    if (!uvec)
        return 0;
    for (;; n++) {
        if (!uok(&uvec[n], sizeof(char *), false))
            return -EFAULT;
        if (!uvec[n])
            return n;
        if (n == max)
            return -E2BIG;
    }
}

/* Copy a NULL-terminated user string vector into kernel memory. */
static int copy_strvec(char *const *uvec, char **kvec, int max, size_t *budget)
{
    int n = 0;
    if (!uvec) {
        kvec[0] = NULL;
        return 0;
    }
    for (;; n++) {
        if (!uok(&uvec[n], sizeof(char *), false))
            return -EFAULT;
        const char *s = uvec[n];
        if (!s)
            break;
        if (n == max)
            return -E2BIG;
        long len = user_strlen(current->pml4, s, MAXARGSTR);
        if (len < 0)
            return len == -ENAMETOOLONG ? -E2BIG : len;
        if ((size_t)len + 1 + sizeof(char *) > *budget)
            return -E2BIG;
        *budget -= len + 1 + sizeof(char *);
        kvec[n] = kmalloc(len + 1);
        if (!kvec[n])
            return -ENOMEM;
        memcpy(kvec[n], s, len + 1);
        kvec[n + 1] = NULL;
    }
    kvec[n] = NULL;
    return n;
}

static void free_strvec(char **kvec)
{
    for (int i = 0; kvec[i]; i++)
        kfree(kvec[i]);
}

static long sys_exec(const char *upath, char *const *uargv, char *const *uenvp)
{
    char *path = path_get();
    size_t budget = MAXARGSTR;
    if (!path)
        return -ENOMEM;
    long na = count_strvec(uargv, MAXARGS), ne = count_strvec(uenvp, MAXENV);
    if (na < 0 || ne < 0) {
        path_put(path);
        return na < 0 ? na : ne;
    }
    char **kargv = kmalloc((na + 1) * sizeof(char *)), **kenvp = kmalloc((ne + 1) * sizeof(char *));
    int r = -ENOMEM;
    if (kargv && kenvp) {
        kargv[0] = kenvp[0] = NULL;
        if ((r = fetch_str(upath, path, MAXPATH)) >= 0 &&
            (r = copy_strvec(uargv, kargv, na, &budget)) >= 0 &&
            (r = copy_strvec(uenvp, kenvp, ne, &budget)) >= 0)
            r = proc_exec(path, kargv, kenvp);
        free_strvec(kargv);
        free_strvec(kenvp);
    }
    kfree(kargv);
    kfree(kenvp);
    path_put(path);
    return r;
}





/* ------------------------------------------------------------------ */
/* Credentials                                                         */
/* ------------------------------------------------------------------ */

static long sys_setuid(int uid)
{
    if (uid < 0)
        return -EINVAL;
    if (is_root()) {
        current->uid = current->euid = current->suid = uid;
        return 0;
    }
    if (uid != current->uid && uid != current->suid)
        return -EPERM;
    current->euid = uid;
    return 0;
}

static long sys_seteuid(int uid)
{
    if (uid < 0)
        return -EINVAL;
    if (!is_root() && uid != current->uid && uid != current->suid)
        return -EPERM;
    current->euid = uid;
    return 0;
}

static long sys_setgid(int gid)
{
    if (gid < 0)
        return -EINVAL;
    if (is_root()) {
        current->gid = current->egid = current->sgid = gid;
        return 0;
    }
    if (gid != current->gid && gid != current->sgid)
        return -EPERM;
    current->egid = gid;
    return 0;
}

static long sys_setegid(int gid)
{
    if (gid < 0)
        return -EINVAL;
    if (!is_root() && gid != current->gid && gid != current->sgid)
        return -EPERM;
    current->egid = gid;
    return 0;
}

static long sys_getgroups(int n, int *list)
{
    if (n == 0)
        return current->ngroups;
    if (n < current->ngroups)
        return -EINVAL;
    if (!uok(list, current->ngroups * sizeof(int), true))
        return -EFAULT;
    memcpy(list, current->groups, current->ngroups * sizeof(int));
    return current->ngroups;
}

static long sys_setgroups(int n, const int *list)
{
    if (!is_root())
        return -EPERM;
    if (n < 0 || n > NGROUPS_MAX)
        return -EINVAL;
    if (n && !uok(list, n * sizeof(int), false))
        return -EFAULT;
    memcpy(current->groups, list, n * sizeof(int));
    current->ngroups = n;
    return 0;
}

static long sys_umask(int mask)
{
    int old = current->umask;
    current->umask = mask & 0777;
    return old;
}












static long sys_cpuinfo(struct cpuinfo *buf, int max)
{
    if (max < 0 || !uok(buf, max * sizeof(*buf), true))
        return -EFAULT;
    int n = 0;
    for (int i = 0; i < ncpu && n < max; i++, n++) {
        struct cpu *c = &cpus[i];
        buf[n].id = c->id;
        buf[n].apic_id = c->apic_id;
        buf[n].online = c->online;
        buf[n].pid = c->lwp && !c->lwp->is_idle ? c->lwp->proc->pid : 0;
        buf[n].busy_ticks = c->busy_ticks;
        buf[n].idle_ticks = c->idle_ticks;
    }
    return n;
}

static long sys_sbrk(long incr)
{
    uint64_t old = current->brk, nbrk = old + incr;
    if (nbrk < current->heap_start || nbrk >= USER_MMAP_TOP - (64UL << 20))
        return -ENOMEM;
    uint64_t dlim = current->rlim_cur[2];            /* SIEOS_RLIMIT_DATA */
    if (incr > 0 && dlim != (uint64_t)-3 && nbrk - current->heap_start > dlim)
        return -ENOMEM;
    if (incr > 0 && (!vm_brk_ok(current, old, nbrk) || !vm_vmem_ok(current, incr)))
        return -ENOMEM;
    if (incr > 0 && (uint64_t)incr / PAGE_SIZE > pmm_total_pages())
        return -ENOMEM;                              /* heuristic overcommit, as for mmap */
    if (incr > 0) {
        vm_space_lock(current);
        int r = vmm_alloc_range(current->pml4, old, nbrk, PTE_U | PTE_W | pte_nx);
        vm_space_unlock(current);
        if (r < 0)
            return -ENOMEM;
    }
    current->brk = nbrk;
    return old;
}

/* ------------------------------------------------------------------ */
/* System information                                                  */
/* ------------------------------------------------------------------ */

static long sys_procinfo(struct procinfo *buf, int max)
{
    if (max < 0 || !uok(buf, max * sizeof(*buf), true))
        return -EFAULT;
    int n = 0, fg = tty_foreground_pgrp();
    for (int i = 0; i < ncpu && n < max; i++) {         /* one idle entry per CPU */
        struct lwp *l = cpus[i].idle;
        if (!l)
            continue;
        struct procinfo *pi = &buf[n++];
        memset(pi, 0, sizeof(*pi));
        pi->state = cpus[i].lwp == l ? PSTATE_RUNNING : PSTATE_RUNNABLE;
        pi->cpu = cpus[i].lwp == l ? i : -1;
        pi->ticks = cpus[i].idle_ticks;
        strlcpy(pi->name, l->name, sizeof(pi->name));
    }
    for (int i = 1; i < NPROC && n < max; i++) {
        struct proc *p = &proc_table[i];
        if (p->state == PSTATE_UNUSED || p->state == PSTATE_EMBRYO)
            continue;
        struct procinfo *pi = &buf[n++];
        pi->pid = p->pid;
        pi->ppid = p->parent ? p->parent->pid : 0;
        pi->pgid = p->pgid;
        pi->sid = p->sid;
        pi->uid = p->uid;
        pi->euid = p->euid;
        pi->state = proc_state(p);
        pi->tty_fg = fg && p->pgid == fg;
        pi->cpu = -1;
        pi->ticks = p->ticks;
        for (int k = 0; k < NLWP; k++) {
            struct lwp *l = &lwp_table[k];
            if (l->proc != p || l->state == LWP_UNUSED)
                continue;
            pi->ticks += l->ticks;
            if (l->state == LWP_RUNNING)
                pi->cpu = l->cpu;
        }
        pi->mem_kb = (p->pml4 && p->pml4 != kernel_pml4_phys) ? vmm_user_pages(p->pml4) * 4 : 0;
        strlcpy(pi->name, p->name, sizeof(pi->name));
    }
    return n;
}

static long sys_meminfo(struct meminfo *mi)
{
    if (!uok(mi, sizeof(*mi), true))
        return -EFAULT;
    mi->total_kb = pmm_total_pages() * 4;
    mi->free_kb = pmm_free_pages() * 4;
    mi->kernel_kb = pmm_kernel_pages() * 4;
    mi->heap_kb = kheap_used() / 1024;
    return 0;
}


/* Halt (power off) or restart the machine. */
long system_halt(bool restart)
{
    vfs_sync();
    if (restart) {
        kprintf("Restarting system...\n");
        power_reset();           /* ACPI reset register, 0xCF9, the i8042, a triple fault */
    } else {
        kprintf("Powering off...\n");
        power_off();             /* ACPI S5 */
        kprintf("System halted. It is now safe to turn off the machine.\n");
    }
    cli();
    for (;;)
        hlt();
    __builtin_unreachable();
}



/*
 * The kernel's internal calls behind some ABI v2 system calls (syscall2.c
 * calls them with a copy of the trap frame).  There is no user entry to
 * this table: the ABI v1 int $0x80 gate was removed in milestone 10.
 */
long syscall_dispatch(struct trapframe *tf)
{
    uint64_t a1 = tf->rdi, a2 = tf->rsi, a3 = tf->rdx;
    switch (tf->rax) {
    case SYS_exit:        return sys_exit(a1);
    case SYS_exec:        return sys_exec((const char *)a1, (char *const *)a2, (char *const *)a3);
    case SYS_sbrk:        return sys_sbrk(a1);
    case SYS_procinfo:    return sys_procinfo((struct procinfo *)a1, a2);
    case SYS_meminfo:     return sys_meminfo((struct meminfo *)a1);
    case SYS_setuid:      return sys_setuid(a1);
    case SYS_setgid:      return sys_setgid(a1);
    case SYS_seteuid:     return sys_seteuid(a1);
    case SYS_setegid:     return sys_setegid(a1);
    case SYS_getgroups:   return sys_getgroups(a1, (int *)a2);
    case SYS_setgroups:   return sys_setgroups(a1, (const int *)a2);
    case SYS_umask:       return sys_umask(a1);
    case SYS_cpuinfo:     return sys_cpuinfo((struct cpuinfo *)a1, a2);
    case SYS_fbmap:       return sys_fbmap(a1);
    }
    return -ENOSYS;
}

/* ------------------------------------------------------------------ */
/* Helpers shared with the ABI v2 dispatcher (syscall2.c)              */
/* ------------------------------------------------------------------ */

bool user_ok(const void *p, size_t n, bool write) { return uok(p, n, write); }
int  user_fetch_str(const char *u, char *k, size_t max) { return fetch_str(u, k, max); }
struct file *fd_file(int fd) { return getfile(fd); }
