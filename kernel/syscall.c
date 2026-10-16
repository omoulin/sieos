/*
 * syscall.c - The system calls: the architecture's entry code saves the
 * caller's registers in r and calls syscall(r); SC_NR, SC_ARGn and SC_RET
 * (arch_defs.h) name the number, arguments and result in them. Any pointer
 * from a program is only used through vm_copy, which checks it.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
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
static long brk_locked(proc_t *p, uint64_t end)
{
    if (!end) return p->brk;
    if (end < p->brk_base || end > p->brk_base + HEAP_MAX) return -ENOMEM;
    uint64_t old = (p->brk + PAGE - 1) & ~(PAGE - 1), new = (end + PAGE - 1) & ~(PAGE - 1);
    if (new > old && !fits(p, (new - old) / PAGE)) return -ENOMEM;
    for (uint64_t va = old; va < new; va += PAGE) {
        uint64_t pa = page_alloc(1);
        if (!pa || vm_map(p->as, va, pa, VM_U | VM_W | VM_NX)) {
            if (pa) page_free(pa, 1);
            vm_unmap(p, old, va);
            p->pages -= (va - old) / PAGE;
            return -ENOMEM;
        }
        p->pages++;
    }
    if (new < old) { vm_unmap(p, new, old); p->pages -= (old - new) / PAGE; }
    return p->brk = end;
}
static long brk(uint64_t end)            /* (the process's lock: its threads may all call it at once) */
{
    proc_t *p = cur->proc;
    spin_lock(&p->lock);
    long r = brk_locked(p, end);
    spin_unlock(&p->lock);
    return r;
}

/* SYS_PCI: one PCI configuration access (x86-64: index to port 0xCF8, data
 * at 0xCFC), under a lock, so the pair cannot be split by another driver's
 * access. */
static long pci_cfg(uint64_t addr, uint64_t val, uint64_t write)
{
    if (!cur->proc->driver) return -EPERM;
#if defined(__x86_64__)
    static spinlock_t pci_lock;
    uint32_t idx = 0x80000000u | (uint32_t)(addr & 0xFFFFFC), v = 0;
    spin_lock(&pci_lock);
    asm volatile("outl %0, %1" : : "a"(idx), "Nd"((uint16_t)0xCF8));
    if (write) asm volatile("outl %0, %1" : : "a"((uint32_t)val), "Nd"((uint16_t)0xCFC));
    else asm volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    spin_unlock(&pci_lock);
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
    spin_lock(&p->lock);
    uint64_t va = p->mmio, first = pa & ~(PAGE - 1);
    long r = va + pa % PAGE;
    for (uint64_t a = first; a < pa + size; a += PAGE, p->mmio += PAGE)
        if (vm_map(p->as, p->mmio, a, VM_U | VM_W | VM_UC | wc | VM_DEV | VM_NX)) { r = -ENOMEM; break; }
    spin_unlock(&p->lock);
    if (r > 0) arch_phys_mapped(pa, size);
    return r;
}

/* SYS_LOG: the console's log (anyone may read it: it is what the serial
 * wire shows). */
static long sys_log(uint64_t ubuf, uint64_t size, uint64_t upos)
{
    static char buf[1024];
    static spinlock_t lock;                  /* (buf: one caller at a time) */
    uint64_t pos;
    long r;
    if ((r = vm_copy(0, &pos, cur->proc->as, (void *)upos, sizeof pos))) return r;
    spin_lock(&lock);
    long n = log_read(buf, size < sizeof buf ? size : sizeof buf, &pos);
    r = n ? vm_copy(cur->proc->as, (void *)ubuf, 0, buf, n) : 0;
    spin_unlock(&lock);
    if (r) return r;
    if ((r = vm_copy(cur->proc->as, (void *)upos, 0, &pos, sizeof pos))) return r;
    return n;
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
    spin_lock(&p->lock);
    uint64_t va = p->mmio;
    for (uint64_t i = 0; i < n; i++, p->mmio += PAGE)
        vm_map(p->as, p->mmio, pa + i * PAGE, VM_U | VM_W | VM_NX | VM_DMA);
    p->pages += n;
    spin_unlock(&p->lock);
    return va;
}

static long dma_free(uint64_t va, uint64_t size)
{
    proc_t *p = cur->proc;
    if (!p->driver) return -EPERM;
    spin_lock(&p->lock);
    long r = va % PAGE || va < MMIO_BASE || va + size > p->mmio ? -EINVAL : 0;
    if (!r) { vm_unmap(p, va, va + size); p->pages -= (size + PAGE - 1) / PAGE; }
    spin_unlock(&p->lock);
    return r;
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
    int ppid = p->pid;
    char console[16];                                  /* its terminal: chosen, or its parent's */
    memcpy(console, p->console, sizeof console);
    if (o.flags & SPAWN_PARENT) {                      /* (if that parent ends meanwhile: proc_start) */
        spin_lock(&proc_lock);
        proc_t *parent = proc_find(o.parent);
        int ok = parent && !parent->zombie && !parent->exiting;
        if (ok) { ppid = parent->pid; memcpy(console, parent->console, sizeof console); }
        spin_unlock(&proc_lock);
        if (!ok) return -ESRCH;
    }
    o.console[sizeof o.console - 1] = 0;
    if (o.flags & SPAWN_CONSOLE) memcpy(console, o.console, sizeof console);
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
    proc_t *c = proc_new(src, (const uint8_t *)img, len, args, uid, gid, &g, !!(o.flags & SPAWN_DRIVER), quota, &e);
    if (!c) return e;
    memcpy(c->console, console, sizeof c->console);
    c->bg = p->bg || (o.flags & SPAWN_BACKGROUND);
    c->pgid = o.flags & SPAWN_PGROUP ? c->pid : p->pgid;
    c->threads->bg = c->bg;
    return proc_start(c, ppid);
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
    mk_ident_t id;
    spin_lock(&proc_lock);
    proc_t *p = pid ? proc_find(pid) : cur->proc;
    int ok = p && !p->zombie;
    if (ok) {
        id = (mk_ident_t){ .pid = p->pid, .uid = p->uid, .gid = p->gid, .groups = p->groups };
        memcpy(id.console, p->console, sizeof id.console);
    }
    spin_unlock(&proc_lock);
    if (!ok) return -ESRCH;
    return vm_copy(cur->proc->as, (void *)dst, 0, &id, sizeof id);
}

static long info(uint64_t dst)
{
    cpu_t *c = this_cpu();
    mk_info_t in = { .uptime_ns = now_ns(), .pages_total = pages_total, .pages_free = pages_free,
                     .self_pid = cur->proc->pid, .self_uid = cur->proc->uid,
                     .nprocs = nprocs, .nthreads = nthreads, .ncpus = ncpus(), .hwcap = arch_hwcap(),
                     .ecores = ecores, .cpu = c->id, .cpu_e = c->type == CPU_E };
    return vm_copy(cur->proc->as, (void *)dst, 0, &in, sizeof in);
}

/* SYS_TASKS: up to max processes with a pid above `after`, in pid order
 * (the list of all processes is in pid order), so a program can list any
 * number of them in chunks. A process's state is its most active thread's. */
static long tasks(uint64_t dst, uint64_t max, int after)
{
    long n = 0, r = 0;
    spin_lock(&proc_lock);
    for (proc_t *p = all_procs; p && (uint64_t)n < max; p = p->anext) {
        if (p->pid <= after) continue;
        spin_lock(&p->lock);
        task_t *best = p->threads;
        for (task_t *t = p->threads; t; t = t->tnext)
            if (t->state == TS_RUN || (t->state == TS_READY && best->state != TS_RUN)) best = t;
        mk_task_t o = { .pid = p->pid, .uid = p->uid, .state = best ? best->state : TS_DEAD,
                        .cpu = best ? best->cpu : 0, .threads = p->nthreads, .ppid = p->ppid, .pages = p->pages,
                        .quota = p->quota };
        spin_unlock(&p->lock);
        memcpy(o.name, p->name, sizeof o.name);
        if ((r = vm_copy(cur->proc->as, (mk_task_t *)dst + n, 0, &o, sizeof o))) break;
        n++;
    }
    spin_unlock(&proc_lock);
    return r ? r : n;
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
    case SYS_YIELD:         schedule(); ret = 0; break;        /* (still TS_RUN: back in the queue) */
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
    case SYS_KILL:          ret = proc_kill((int)a, (int)b); break;
    case SYS_SIGNAL:        ret = sig_set((int)a, b); break;
    case SYS_SIGRETURN:     ret = sig_return(r); break;
    case SYS_MODULES:       ret = module_names(a, b); break;
    case SYS_SET_FS:        ret = set_fs(a); break;
    case SYS_FDT:           ret = arch_fdt(a, b); break;
    case SYS_SCREEN:        ret = arch_screen(a); break;
    case SYS_PCI:           ret = pci_cfg(a, b, c); break;
    case SYS_LOG:           ret = sys_log(a, b, c); break;
    case SYS_PLACE:         ret = place((int)a, (int)b); break;
    default:               ret = -ENOSYS;
    }
    SC_RET(r) = ret;
}
