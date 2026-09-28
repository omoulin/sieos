/*
 * cpu.h - CPU features: FPU/SSE state, NX, syscall entry.
 */
#ifndef SIEOS_CPU_H
#define SIEOS_CPU_H

#include "kernel.h"

struct fpu_state {
    uint8_t area[512];                     /* fxsave image */
} __attribute__((aligned(16)));

extern struct fpu_state fpu_default;       /* after fninit, MXCSR = 0x1F80 */
extern uint64_t pte_nx;                    /* PTE_NX if supported, else 0 */

void cpu_features_init(bool bsp);
void fpu_save(struct fpu_state *f);
void fpu_restore(const struct fpu_state *f);

#endif
