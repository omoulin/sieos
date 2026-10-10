/*
 * fpu.c (arm64) - The FP/SIMD registers of user threads (q0-q31, FPCR,
 * FPSR: 528 bytes). The kernel itself never uses them (it is compiled with
 * -mgeneral-regs-only), so they only matter when a CPU goes from one thread
 * that uses them to another.
 *
 * Lazy start, eager switching, as on x86-64:
 *   - CPACR_EL1.FPEN = 01: EL0's FP/SIMD instructions trap (EL1's do not:
 *     entry.S saves and loads with them); a thread starts with no area;
 *   - its first such instruction traps (exception class 0x07): it gets an
 *     area holding the clean state (all zero), the CPU lets EL0 use the
 *     registers (FPEN = 11), and the instruction runs again;
 *   - from then on a switch away saves its registers, a switch to it loads them.
 * Threads that never compute in floating point (all servers) cost nothing.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "arm64.h"

#define AREA 528

static void fpen(cpu_t *c, int on)
{
    uint64_t v;
    asm volatile("mrs %0, cpacr_el1" : "=r"(v));
    v = (v & ~(3UL << 20)) | (on ? 3UL : 1UL) << 20;
    asm volatile("msr cpacr_el1, %0; isb" : : "r"(v));
    c->a.fpen = on;
}

void fpu_cpu_init(cpu_t *c) { fpen(c, 0); }

/* kzalloc's 1024-byte blocks are aligned to their size: 16 is enough. */
void fpu_free(task_t *t)
{
    if (t->fpu) { kfree(t->fpu); t->fpu = 0; }
}

int fpu_first_use(task_t *t)
{
    cpu_t *c = this_cpu();
    if (!t->fpu && !(t->fpu = kzalloc(AREA))) return 0;   /* zeros: the clean state */
    if (!c->a.fpen) fpen(c, 1);
    fpu_load(t->fpu);
    return 1;
}

/* A CPU leaves prev (may be 0: idle, or a thread that just ended) for next. */
void fpu_switch(cpu_t *c, task_t *prev, task_t *next)
{
    if (prev && prev->fpu && c->a.fpen) fpu_save(prev->fpu);
    if (next && next->fpu) {
        if (!c->a.fpen) fpen(c, 1);
        fpu_load(next->fpu);
    } else if (c->a.fpen) fpen(c, 0);
}
