/*
 * arch.h - x86_64 CPU structures: GDT, TSS, IDT, trap frames.
 */
#ifndef SIEOS_ARCH_H
#define SIEOS_ARCH_H

#include "kernel.h"

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   (0x18 | 3)
#define USER_CS   (0x20 | 3)
#define TSS_SEL   0x28

#define IRQ_BASE  32
#define IRQ_TIMER 0
#define IRQ_KBD   1
#define IRQ_COM1  4
#define IRQ_ATA   14
#define T_SYSCALL 0x80        /* ABI v1: int $0x80 */
#define T_SYSCALL2 0x81       /* ABI v2: the syscall instruction (not an IDT vector) */

struct trapframe {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_t)(struct trapframe *tf);

struct cpu;
void gdt_init(struct cpu *c);
void tss_set_rsp0(uint64_t rsp0);
void idt_init(void);
void idt_load(void);
void irq_register(int irq, irq_handler_t h);
void pic_unmask(int irq);

/* isr.S */
extern void trapret(void);
extern void forkret(void);
void switch_context(uint64_t *old_rsp, uint64_t new_rsp);

#endif
