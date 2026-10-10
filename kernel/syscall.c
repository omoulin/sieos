/*
 * syscall.c - The system calls: the architecture's entry code saves the
 * caller's registers in r and calls syscall(r); SC_NR, SC_ARGn and SC_RET
 * (arch_defs.h) name the number, arguments and result in them. Any pointer
 * from a program is only used through vm_copy, which checks it.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"

/* Copy a string (at most max - 1 characters) from the calling program. */
static long get_str(char *dst, uint64_t src, int max)
{
    for (int i = 0; i < max; i++) {
        if (vm_copy(0, dst + i, cur->proc->as, (void *)(src + i), 1)) return -EFAULT;
        if (!dst[i]) return 0;
    }
    return -EINVAL;
}

/* A process may take n more pages: within its quota (SPAWN_QUOTA), if any. */
static int fits(proc_t *p, uint64_t n) { return !p->quota || p->pages + n <= p->quota; }

/* SYS_BRK: move the end of the heap, adding or removing pages. The heap
 * may grow up to 64 GiB above its start (a language model's weights and
 * cache take a few GiB); the real limits are the quota and free memory. */
#define HEAP_MAX (64UL << 30)
static long brk(uint64_t end)
{
    proc_t *p = cur->proc;
    if (!end) return p->brk;
    if (end < p->brk_base || end > p->brk_base + HEAP_MAX) return -ENOMEM;
    uint64_t old = (p->brk + PAGE - 1) & ~(PAGE - 1), new = (end + PAGE - 1) & ~(PAGE - 1);
    if (new > old && !fits(p, (new - old) / PAGE)) return -ENOMEM;
    for (uint64_t va = old; va < new; va += PAGE) {
        uint64_t pa = page_alloc(1);
        if (!pa || vm_map(p->as, va, pa, VM_U | VM_W | VM_NX)) {
            if (pa) page_free(pa, 1);
            for (uint64_t v = old; v < va; v += PAGE) vm_unmap(p->as, v), p->pages--;
            return -ENOMEM;
        }
        p->pages++;
    }
    for (uint64_t va = new; va < old; va += PAGE) vm_unmap(p->as, va), p->pages--;
    if (new < old) tlb_shootdown(p);     /* other threads' CPUs must forget those pages */
    return p->brk = end;
}

/* SYS_PCI: one PCI configuration access (x86-64: index to port 0xCF8, data
 * at 0xCFC). Every system call holds the kernel lock, so the pair cannot be
 * split by another driver's access. */
static long pci_cfg(uint64_t addr, uint64_t val, uint64_t write)
{
    if (!cur->proc->driver) return -EPERM;
#if defined(__x86_64__)
    uint32_t idx = 0x80000000u | (uint32_t)(addr & 0xFFFFFC);
    asm volatile("outl %0, %1" : : "a"(idx), "Nd"((uint16_t)0xCF8));
    if (write) { asm volatile("outl %0, %1" : : "a"((uint32_t)val), "Nd"((uint16_t)0xCFC)); return 0; }
    uint32_t v;
    asm volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    return v;
#else
    (void)addr; (void)val; (void)write;
    return -ENOSYS;
#endif
}

/* SYS_MAP_PHYS: give a driver a device's registers or memory (e.g. the
 * screen). Never RAM (which would let it read anyone's memory), never the
 * interrupt controllers (the kernel's). Mapped uncached. */
static long map_phys(uint64_t pa, uint64_t size)
{
    proc_t *p = cur->proc;
    if (!p->driver) return -EPERM;
    uint64_t wc = size & MAP_WC ? VM_WC : 0;
    size &= ~MAP_WC;
    if (!size || size > (1UL << 30) || pa + size < pa) return -EINVAL;
    if ((is_ram(pa, size) && !arch_phys_screen(pa, size)) || arch_phys_forbidden(pa, size)) return -EPERM;
    uint64_t va = p->mmio, first = pa & ~(PAGE - 1);
    for (uint64_t a = first; a < pa + size; a += PAGE, p->mmio += PAGE)
        if (vm_map(p->as, p->mmio, a, VM_U | VM_W | VM_UC | wc | VM_DEV | VM_NX)) return -ENOMEM;
    return va + pa % PAGE;
}

/* SYS_DMA_ALLOC: memory a device reads or writes by itself ("DMA"): it
 * must be physically contiguous, and the driver needs its physical address
 * to give to the device. It is the process's own memory, freed with it. */
static long dma_alloc(uint64_t size, uint64_t upa)
{
    proc_t *p = cur->proc;
    if (!p->driver) return -EPERM;
    int low = !!(size & DMA_LOW);
    size &= ~DMA_LOW;
    uint64_t n = (size + PAGE - 1) / PAGE, pa;
    quarantine_release();                /* dead drivers' buffers, if their time is up */
    if (!n || n > 4096 || !fits(p, n) || !(pa = low ? page_alloc_low(n, 1UL << 30) : page_alloc(n))) return -ENOMEM;
    arch_dma_clean(pa, n * PAGE);        /* (the zeroes, written to memory: a device may not see caches) */
    if (vm_copy(p->as, (void *)upa, 0, &pa, sizeof pa)) { page_free(pa, n); return -EFAULT; }
    uint64_t va = p->mmio;
    for (uint64_t i = 0; i < n; i++, p->mmio += PAGE)
        vm_map(p->as, p->mmio, pa + i * PAGE, VM_U | VM_W | VM_NX | VM_DMA);
    p->pages += n;
    return va;
}

static long dma_free(uint64_t va, uint64_t size)
{
    proc_t *p = cur->proc;
    if (!p->driver) return -EPERM;
    if (va % PAGE || va < MMIO_BASE || va + size > p->mmio) return -EINVAL;
    for (uint64_t a = va; a < va + size; a += PAGE, p->pages--) vm_unmap(p->as, a);
    tlb_shootdown(p);
    return 0;
}

/* SYS_SPAWN: start a program from an ELF image in the caller's memory. Its
 * identity: the caller's, unless the caller is root, who may choose (that
 * is how accounts start a user's shell). Options (mk_spawn_t): root may
 * give groups and another parent; only the supervisor may give device
 * rights or start a boot module (whose image the kernel keeps). */
static long spawn(uint64_t img, uint64_t len, uint64_t uargs, int uid, int gid, uint64_t uopt)
{
    char args[256];
    long e = get_str(args, uargs, sizeof args);
    if (e) return e;
    proc_t *p = cur->proc;
    mk_spawn_t o = { 0 };
    if (uopt && vm_copy(0, &o, p->as, (void *)uopt, sizeof o)) return -EFAULT;
    mk_groups_t g = p->groups;                         /* the caller's groups, unless root chooses */
    if (p->uid || uid < 0) uid = p->uid;
    if (p->uid || gid < 0) gid = p->gid;
    if (p->uid && (o.flags & (SPAWN_GROUPS | SPAWN_PARENT))) return -EPERM;
    if (!p->super && (o.flags & (SPAWN_DRIVER | SPAWN_MODULE))) return -EPERM;
    if (o.flags & SPAWN_GROUPS) {
        if (o.groups.n < 0 || o.groups.n > NGROUPS) return -EINVAL;
        g = o.groups;
    } else if (!p->uid && uid != p->uid) g.n = 0;     /* root starting someone else: no groups unless given */
    proc_t *parent = 0;
    if ((o.flags & SPAWN_PARENT) && (!(parent = proc_find(o.parent)) || parent->zombie || parent->exiting)) return -ESRCH;
    uint64_t src = p->as;
    if (o.flags & SPAWN_MODULE) {                      /* len: the module's number */
        if (len >= (uint64_t)nmodules || !len) return -ENOENT;   /* (0 is init itself) */
        img = (uint64_t)P2V(modules[len].pa);
        len = modules[len].len;
        src = 0;                                       /* a kernel address */
    }
    if (len > (256UL << 20)) return -EINVAL;
    /* Its memory limit: the one asked for, never above the caller's own. */
    uint64_t quota = o.flags & SPAWN_QUOTA ? (o.quota + PAGE - 1) / PAGE : p->quota;
    if (!quota && (o.flags & SPAWN_QUOTA)) return -EINVAL;
    if (p->quota && (!quota || quota > p->quota)) quota = p->quota;
    long pid = proc_spawn(src, (const uint8_t *)img, len, args, uid, gid, &g, !!(o.flags & SPAWN_DRIVER), quota);
    if (pid <= 0) return pid;
    proc_t *c = proc_find(pid);
    if (parent) c->ppid = parent->pid;
    o.console[sizeof o.console - 1] = 0;             /* its terminal: chosen, or its parent's */
    memcpy(c->console, o.flags & SPAWN_CONSOLE ? o.console : (parent ? parent : p)->console, sizeof c->console);
    return pid;
}

/* SYS_MODULES: the boot modules' names, for the supervisor. */
static long module_names(uint64_t dst, uint64_t size)
{
    if (!cur->proc->super) return -EPERM;
    uint64_t n = 0;
    for (int i = 0; i < nmodules; i++) {
        uint64_t l = strlen(modules[i].name) + 1;
        if (n + l > size) return -EINVAL;
        if (vm_copy(cur->proc->as, (char *)dst + n, 0, modules[i].name, l)) return -EFAULT;
        n += l;
    }
    return n;
}

/* SYS_IDENT: who process pid is (0: the caller). A process's identity never
 * changes, so servers may keep the answer. */
static long ident(int pid, uint64_t dst)
{
    proc_t *p = pid ? proc_find(pid) : cur->proc;
    if (!p || p->zombie) return -ESRCH;
    mk_ident_t id = { .pid = p->pid, .uid = p->uid, .gid = p->gid, .groups = p->groups };
    memcpy(id.console, p->console, sizeof id.console);
    return vm_copy(cur->proc->as, (void *)dst, 0, &id, sizeof id);
}

static long info(uint64_t dst)
{
    mk_info_t in = { .uptime_ns = now_ns(), .pages_total = pages_total, .pages_free = pages_free,
                     .self_pid = cur->proc->pid, .self_uid = cur->proc->uid,
                     .nprocs = nprocs, .nthreads = nthreads, .ncpus = ncpu, .hwcap = arch_hwcap() };
    return vm_copy(cur->proc->as, (void *)dst, 0, &in, sizeof in);
}

/* SYS_TASKS: up to max processes with a pid above `after`, in pid order
 * (the list of all processes is in pid order), so a program can list any
 * number of them in chunks. A process's state is its most active thread's. */
static long tasks(uint64_t dst, uint64_t max, int after)
{
    long n = 0;
    for (proc_t *p = all_procs; p && (uint64_t)n < max; p = p->anext) {
        if (p->pid <= after) continue;
        task_t *best = p->threads;
        for (task_t *t = p->threads; t; t = t->tnext)
            if (t->state == TS_RUN || (t->state == TS_READY && best->state != TS_RUN)) best = t;
        mk_task_t o = { .pid = p->pid, .uid = p->uid, .state = best ? best->state : TS_DEAD,
                        .cpu = best ? best->cpu : 0, .threads = p->nthreads, .ppid = p->ppid, .pages = p->pages,
                        .quota = p->quota };
        memcpy(o.name, p->name, sizeof o.name);
        if (vm_copy(cur->proc->as, (mk_task_t *)dst + n, 0, &o, sizeof o)) return -EFAULT;
        n++;
    }
    return n;
}

/* SYS_SET_FS: this thread's thread-local storage pointer (on x86-64 the FS
 * base: compilers address thread-local variables %fs-relative). Loaded on
 * every switch to it. */
static long set_fs(uint64_t base)
{
    if (base >= USER_TOP) return -EINVAL;
    cur->fs_base = base;
    arch_set_tls(this_cpu(), base);
    return 0;
}

static long power(int restart)
{
    if (cur->proc->uid) return -EPERM;
#ifdef STACKCHECK
    stack_report();
#endif
    kprintf("mk: %s\n", restart ? "restarting" : "power off");
    halt_others();                       /* every other CPU stops first */
    arch_power(restart);
}

void syscall(regs_t *r)
{
    klock();                        /* released in kernel_exit (entry.S, task.c) */
    if (cur->proc->exiting) thread_exit();   /* its process is ending: so does this thread */
    uint64_t a = SC_ARG0(r), b = SC_ARG1(r), c = SC_ARG2(r);
    char name[16];
    long ret;
    switch (SC_NR(r)) {
    case SYS_DEBUG: {
        char buf[200];
        if (b > sizeof buf - 1) b = sizeof buf - 1;
        if ((ret = vm_copy(0, buf, cur->proc->as, (void *)a, b))) break;
        buf[b] = 0;
        kprintf("%s", buf);
        ret = b;
        break;
    }
    case SYS_EXIT:          proc_exit((int)a);
    case SYS_YIELD:         ready(cur); schedule(); ret = 0; break;
    case SYS_PORT_CREATE:   ret = a ? get_str(name, a, 16) : (name[0] = 0); if (!ret) ret = port_create(name); break;
    case SYS_PORT_LOOKUP:   ret = get_str(name, a, 16); if (!ret) ret = port_lookup(name, (int)b); break;
    case SYS_WANT:          ret = get_str(name, a, 16); if (!ret) ret = port_want(name, (long)b); break;
    case SYS_WANTED:        ret = port_wanted(a, b); break;
    case SYS_CALL:          ret = ipc_call(a, (msg_t *)b); break;
    case SYS_RECV:          ret = ipc_recv(a, (msg_t *)b); break;
    case SYS_REPLY:         ret = ipc_reply(a, (msg_t *)b); break;
    case SYS_IRQ_BIND:      ret = irq_bind(a, b); break;
    case SYS_IRQ_ACK:       ret = irq_ack(a); break;
    case SYS_MAP_PHYS:      ret = map_phys(a, b); break;
    case SYS_BRK:           ret = brk(a); break;
    case SYS_INFO:          ret = info(a); break;
    case SYS_POWER:         ret = power(a); break;
    case SYS_TASKS:         ret = tasks(a, b, c); break;
    case SYS_CLOCK:         ret = now_ns(); break;
    case SYS_SLEEP:         ret = sleep_ns(a); break;
    case SYS_WAKE:          ret = sleep_wake((int)a); break;
    case SYS_THREAD_CREATE: ret = thread_create(a, b, c); break;
    case SYS_THREAD_EXIT:   thread_exit();
    case SYS_DMA_ALLOC:     ret = dma_alloc(a, b); break;
    case SYS_DMA_FREE:      ret = dma_free(a, b); break;
    case SYS_SPAWN:         ret = spawn(a, b, c, (int)SC_ARG3(r), (int)SC_ARG4(r), SC_ARG5(r)); break;
    case SYS_WAIT:          ret = proc_wait((int)a, (int *)b, (int)c); break;
    case SYS_REPLY_RECV:    ret = ipc_reply_recv(a, b, (msg_t *)c); break;
    case SYS_RANDOM:        ret = rand_get(a, b); break;
    case SYS_IDENT:         ret = ident((int)a, b); break;
    case SYS_CHILD_PORT:    ret = child_port(a); break;
    case SYS_KILL:          ret = proc_kill((int)a); break;
    case SYS_MODULES:       ret = module_names(a, b); break;
    case SYS_SET_FS:        ret = set_fs(a); break;
    case SYS_FDT:           ret = arch_fdt(a, b); break;
    case SYS_SCREEN:        ret = arch_screen(a); break;
    case SYS_PCI:           ret = pci_cfg(a, b, c); break;
    default:                ret = -ENOSYS;
    }
    SC_RET(r) = ret;
}
