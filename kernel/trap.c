/*
 * trap.c - IDT, 8259 PIC and the central trap dispatcher.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "power.h"
#include "arch.h"
#include "proc.h"
#include "mm.h"
#include "smp.h"
#include "random.h"
#include "abi2.h"
#include "vm.h"
#include "jbd2.h"
#include "net.h"
#include "blkdev.h"
#include "sieos/syscall.h"
#include "sieos/time.h"

struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  type;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static irq_handler_t irq_handlers[16];
#define IRQ_SHARE 4
static struct {
    irq_shared_t fn;
    void *arg;
} irq_shared[16][IRQ_SHARE];

extern uint64_t isr_table[];
extern void isr64(void), isr65(void), isr66(void), isr255(void);
static struct idt_ptr idtr;

long syscall_dispatch(struct trapframe *tf);
long syscall_dispatch_v2(struct trapframe *tf);

static const char *exc_names[] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range",
    "Invalid opcode", "Device not available", "Double fault", "Coprocessor overrun",
    "Invalid TSS", "Segment not present", "Stack fault", "General protection",
    "Page fault", "Reserved", "x87 FPU error", "Alignment check", "Machine check",
    "SIMD exception", "Virtualization", "Control protection",
};

static void idt_set(int n, uint64_t handler, uint8_t type)
{
    idt[n].off_lo = handler & 0xFFFF;
    idt[n].sel = KERNEL_CS;
    idt[n].ist = 0;
    idt[n].type = type;
    idt[n].off_mid = (handler >> 16) & 0xFFFF;
    idt[n].off_hi = handler >> 32;
    idt[n].zero = 0;
}

static void pic_init(void)
{
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, IRQ_BASE); io_wait();
    outb(0xA1, IRQ_BASE + 8); io_wait();
    outb(0x21, 4); io_wait();
    outb(0xA1, 2); io_wait();
    outb(0x21, 1); io_wait();
    outb(0xA1, 1); io_wait();
    outb(0x21, 0xFB);            /* everything masked except cascade */
    outb(0xA1, 0xFF);
}

void pic_unmask(int irq)
{
    uint16_t port = irq < 8 ? 0x21 : 0xA1;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}

static void pic_eoi(int irq)
{
    if (irq >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

static void irq_route(int irq)
{
    if (ioapic_ok)
        ioapic_route(irq, cpus[0].apic_id);
    else
        pic_unmask(irq);
}

bool irq_register_shared(int irq, irq_shared_t h, void *arg)
{
    if (irq < 0 || irq >= 16)
        return false;
    for (int i = 0; i < IRQ_SHARE; i++)
        if (!irq_shared[irq][i].fn) {
            irq_shared[irq][i].arg = arg;
            irq_shared[irq][i].fn = h;
            irq_route(irq);
            return true;
        }
    return false;
}

void irq_register(int irq, irq_handler_t h)
{
    irq_handlers[irq] = h;
    if (ioapic_ok)
        ioapic_route(irq, cpus[0].apic_id);   /* device interrupts: the boot CPU */
    else
        pic_unmask(irq);
}

/* Move the interrupts registered so far to the I/O APIC (at boot, once it is found). */
void irq_use_ioapic(void)
{
    if (!ioapic_init())
        return;
    for (int irq = 0; irq < 16; irq++)
        if (irq_handlers[irq] || irq_shared[irq][0].fn)
            ioapic_route(irq, cpus[0].apic_id);
}

void idt_init(void)
{
    pic_init();
    for (int i = 0; i < 48; i++)
        idt_set(i, isr_table[i], 0x8E);           /* interrupt gate, DPL0 */
    idt_set(T_LAPIC_TIMER, (uint64_t)isr64, 0x8E);
    idt_set(T_IPI_RESCHED, (uint64_t)isr65, 0x8E);
    idt_set(T_IPI_TLB, (uint64_t)isr66, 0x8E);
    idt_set(T_SPURIOUS, (uint64_t)isr255, 0x8E);
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)idt;
    idt_load();
}

void idt_load(void)
{
    __asm__ volatile("lidt %0" :: "m"(idtr));
}

static void dump_frame(struct trapframe *tf)
{
    kprintf("  rip=%lx cs=%lx rflags=%lx rsp=%lx err=%lx\n",
            tf->rip, tf->cs, tf->rflags, tf->rsp, tf->err_code);
    kprintf("  rax=%lx rbx=%lx rcx=%lx rdx=%lx\n", tf->rax, tf->rbx, tf->rcx, tf->rdx);
    kprintf("  rsi=%lx rdi=%lx rbp=%lx cr2=%lx\n", tf->rsi, tf->rdi, tf->rbp, read_cr2());
}

/* Signal, si_code and address for a user-mode exception. */
static int fault_signal(struct trapframe *tf, int *code, uint64_t *addr)
{
    *addr = tf->rip;
    switch (tf->int_no) {
    case 0:  *code = SIEOS_FPE_INTDIV; return SIGFPE;          /* divide error */
    case 16:
    case 19: *code = SIEOS_FPE_FLTDIV; return SIGFPE;
    case 3:  *code = SIEOS_TRAP_BRKPT; return SIGTRAP;
    case 6:  *code = SIEOS_ILL_ILLOPC; return SIGILL;
    case 17: *code = SIEOS_BUS_ADRALN; return SIGBUS;
    case 14:
        *addr = read_cr2();
        *code = (tf->err_code & 1) ? SIEOS_SEGV_ACCERR : SIEOS_SEGV_MAPERR;
        return SIGSEGV;
    default: *code = SIEOS_SEGV_MAPERR; *addr = 0; return SIGSEGV;
    }
}

/*
 * System calls that read only the caller's own state or the clocks run
 * without the kernel lock: false for everything else.
 */
static bool fast_syscall(struct trapframe *tf)
{
    struct lwp *l = curlwp;
    struct proc *p = l->proc;
    long r;
    switch (tf->rax) {
    case SIEOS_SYS_getpid:
        tf->rdx = p->parent ? p->parent->pid : 0;       /* (proc_table entries never go away) */
        r = p->pid;
        break;
    case SIEOS_SYS_getuid:  tf->rdx = p->euid; r = p->uid; break;
    case SIEOS_SYS_geteuid: r = p->euid; break;
    case SIEOS_SYS_getgid:  tf->rdx = p->egid; r = p->gid; break;
    case SIEOS_SYS_getegid: r = p->egid; break;
    case SIEOS_SYS_lwp_self: r = l->lwpid; break;
    case SIEOS_SYS_gethrtime: r = (long)hrtime(); break;
    case SIEOS_SYS_gethrvtime: r = (long)(l->ticks * (1000000000UL / TIMER_HZ)); break;
    case SIEOS_SYS_clock_gettime: {
        struct sieos_timespec ts, *u = (struct sieos_timespec *)tf->rsi;
        int64_t ns;
        if (tf->rdi == SIEOS_CLOCK_REALTIME)
            ns = realtime_ns();
        else if (tf->rdi == SIEOS_CLOCK_MONOTONIC)
            ns = (int64_t)hrtime();
        else
            return false;
        if (!user_range_ok(p->pml4, (uint64_t)u, sizeof(*u), true))
            return false;                               /* (the slow path says EFAULT) */
        ts.tv_sec = ns / 1000000000L;
        ts.tv_nsec = ns % 1000000000L;
        memcpy(u, &ts, sizeof(ts));                     /* a fault here takes the kernel lock */
        r = 0;
        break;
    }
    default:
        return false;
    }
    l->orig_rax = tf->rax;
    l->restart_syscall = false;
    tf->rax = r;
    tf->rflags &= ~1UL;
    return true;
}

/* Nothing to do on the way back to user mode but return? */
static bool nothing_pending(void)
{
    struct lwp *l = curlwp;
    struct proc *p = l->proc;
    return !mycpu()->need_resched && !l->must_exit && !l->suspend_req && !p->stopped &&
           !((l->sig_pending | p->sig_pending) & ~l->sig_blocked);
}

void trap_handler(struct trapframe *tf)
{
    bool from_user = (tf->cs & 3) == 3;

    if (tf->int_no == T_SPURIOUS)
        return;                               /* no EOI for spurious interrupts */
    if (tf->int_no == T_IPI_TLB) {            /* no kernel lock: the sender holds it */
        write_cr3(read_cr3());
        mycpu()->tlb_flush = 0;
        lapic_eoi();
        return;
    }

    /* A page fault from user mode needs only the address space's own lock
     * (vm.c): demand-zero and copy-on-write faults of different LWPs and
     * processes run in parallel; so do the system calls of fast_syscall.
     * Anything else to do on the way back to
     * user mode (rescheduling, signals, exit) takes the slow path. */
    if (from_user && ((tf->int_no == 14 && vm_fault(read_cr2(), tf->err_code, true) && ++curlwp->minflt) ||
                      (tf->int_no == T_SYSCALL2 && fast_syscall(tf)))) {
        if (nothing_pending())
            return;
        bkl_lock();
        goto to_user;
    }

    /* Kernel code runs under the big kernel lock.  A trap from user mode
     * always needs it; a trap from kernel mode only interrupts the idle
     * loop's hlt (or a boot-time wait) and takes it if not already held. */
    bool took = false;
    if (!bkl_held()) {
        bkl_lock();
        took = true;
    }

    if (tf->int_no == T_LAPIC_TIMER) {
        lapic_eoi();
        lapic_timer_irq(tf);
    } else if (tf->int_no == T_IPI_RESCHED) {
        lapic_eoi();                          /* idle loop reschedules after hlt */
    } else if (tf->int_no == T_SYSCALL2) {
        /* ABI v2: carry flag + positive errno on failure (see syscall2.c) */
        curlwp->orig_rax = tf->rax;
        long r = syscall_dispatch_v2(tf);
        curlwp->restart_syscall = (r == -ERESTART);
        if (r == -EJUSTRETURN) {
            /* context(SETCONTEXT): the registers are already set */
        } else if (r < 0 && r >= -4095) {
            tf->rax = sieos_errno(-r);
            tf->rflags |= 1;
        } else {
            tf->rax = r;
            tf->rflags &= ~1UL;
        }
        if (fs_commit_deadline && ticks >= fs_commit_deadline)
            ext4_journal_tick(false);        /* an old ext4 transaction: commit it */
        if (net_loop_pending)
            net_loop_drain();                /* packets this call sent to ourselves */
        if (blk_late_pending)
            blk_scan_late();                 /* a disk that came after boot: its partitions */
    } else if (tf->int_no >= IRQ_BASE && tf->int_no < IRQ_BASE + 16) {
        int irq = tf->int_no - IRQ_BASE;
        if (!ioapic_ok)
            pic_eoi(irq);
        random_add_entropy(tf->rip ^ ((uint64_t)irq << 48));
        if (irq_handlers[irq])
            irq_handlers[irq](tf);
        for (int i = 0; i < IRQ_SHARE && irq_shared[irq][i].fn; i++)
            irq_shared[irq][i].fn(tf, irq_shared[irq][i].arg);
        if (ioapic_ok)
            lapic_eoi();                      /* after the handler: level lines are quiet now */
    } else if (tf->int_no == 14 && vm_fault(read_cr2(), tf->err_code, from_user)) {
        curlwp->minflt++;
        /* resolved: demand paging or copy-on-write */
    } else if (tf->int_no == 13 && !from_user && msr_fixup(tf)) {
        /* an MSR that is not there (power.c probes them) */
    } else {
        const char *name = tf->int_no < ARRAY_SIZE(exc_names) ? exc_names[tf->int_no] : "Unknown";
        if (from_user && current) {
            int code;
            uint64_t addr;
            int sig = fault_signal(tf, &code, &addr);
            signal_fault(sig, code, addr);
        } else {
            kprintf("\nException %lu: %s in kernel mode\n", tf->int_no, name);
            dump_frame(tf);
            panic("unrecoverable kernel exception");
        }
    }

    if (from_user) {
to_user:
        if (mycpu()->need_resched)
            schedule();                  /* a higher-priority LWP became runnable */
        signal_deliver(tf);
        bkl_unlock();
    } else if (took) {
        bkl_unlock();
    }
}
