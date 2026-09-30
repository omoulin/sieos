/*
 * gdt.c - Per-CPU Global Descriptor Table and Task State Segment.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "arch.h"
#include "smp.h"

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void gdt_init(struct cpu *c)
{
    uint64_t *gdt = c->gdt;
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFUL;   /* 0x08 kernel code, 64-bit, DPL0 */
    gdt[2] = 0x00CF92000000FFFFUL;   /* 0x10 kernel data */
    gdt[3] = 0x00CFF2000000FFFFUL;   /* 0x18 user data, DPL3 */
    gdt[4] = 0x00AFFA000000FFFFUL;   /* 0x20 user code, 64-bit, DPL3 */

    uint64_t base = (uint64_t)&c->tss;
    uint64_t limit = sizeof(c->tss) - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89UL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;
    c->tss.iomap_base = sizeof(c->tss);

    struct gdt_ptr p = { sizeof(c->gdt) - 1, (uint64_t)gdt };
    __asm__ volatile(
        "lgdt %0\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "xor %%ax, %%ax\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "pushq $0x08\n"
        "lea 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "mov $0x28, %%ax\n"
        "ltr %%ax\n"
        :: "m"(p) : "rax", "memory");

    /* Kernel per-CPU pointer lives in GS_BASE; user GS base (0) is kept in
     * KERNEL_GS_BASE and exchanged with swapgs on every user/kernel switch. */
    c->self = c;
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void tss_set_rsp0(uint64_t rsp0)
{
    mycpu()->tss.rsp0 = rsp0;
    mycpu()->syscall_rsp0 = rsp0;
}
