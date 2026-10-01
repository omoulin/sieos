/*
 * cpu.h - CPU features: FPU/SSE state, NX, syscall entry.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_CPU_H
#define SIEOS_CPU_H

#include "kernel.h"

/* The FPU/SSE/AVX registers of an LWP: an xsave image (standard form: the
 * fxsave layout's 512 bytes, the 64-byte header, then AVX's and AVX-512's
 * areas), or the fxsave image alone on a processor without XSAVE. */
#define FPU_AREA 3072
struct fpu_state {
    uint8_t area[FPU_AREA];
} __attribute__((aligned(64)));

extern struct fpu_state fpu_default;       /* after fninit, MXCSR = 0x1F80 */
extern bool cpu_xsave;                     /* XSAVE in use (CR4.OSXSAVE) */
extern uint64_t cpu_xcr0;                  /* the state components enabled: x87, SSE, AVX, AVX-512 */
extern uint32_t cpu_xsave_size;            /* bytes of an xsave image for them (512 without XSAVE) */
extern uint64_t pte_nx;                    /* PTE_NX if supported, else 0 */
extern bool pat_wc;                        /* PWT alone selects write-combining (PAT entry 1) */

void cpu_features_init(bool bsp);
void fpu_save(struct fpu_state *f);
void fpu_restore(const struct fpu_state *f);

#endif
