/*
 * fpu.c (x86-64) - The floating-point and vector registers of user threads: x87,
 * SSE and AVX (the "extended state"). The kernel itself never uses them
 * (it is compiled with -mgeneral-regs-only), so they only matter when a
 * CPU goes from one thread that uses them to another.
 *
 * Lazy start, eager switching:
 *   - a thread starts with no save area, and its CPU runs it with CR0.TS
 *     set: its first floating-point or vector instruction traps (#NM,
 *     "device not available");
 *   - fpu_first_use() then gives it an area holding the registers' clean
 *     state (so nothing can leak from another thread or process), loads it
 *     and clears TS: the instruction runs again, this time for real;
 *   - from then on, a switch away from the thread saves its registers
 *     (XSAVEOPT / XSAVE), and a switch to it restores them (XRSTOR).
 * A thread that never computes in floating point (all of SIEOS's servers)
 * costs nothing: no area, no saving; its CPU only keeps TS set.
 *
 * The instructions are written as bytes (".byte"), with the pointer in
 * %rdi: both gcc's assembler and sicc's accept them that way.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "x86.h"

static uint32_t area_size;          /* bytes per thread (CPUID 0xD, for what XCR0 enables) */
static int xsave, xsaveopt;         /* the CPU has XSAVE (else FXSAVE: x87 + SSE only) / XSAVEOPT */

static inline uint64_t read_cr0(void) { uint64_t v; asm volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr4(void) { uint64_t v; asm volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint64_t v) { asm volatile("mov %0, %%cr0" : : "r"(v)); }
static inline void write_cr4(uint64_t v) { asm volatile("mov %0, %%cr4" : : "r"(v)); }
static inline void clts(void) { asm volatile(".byte 0x0f, 0x06"); }           /* clear CR0.TS */
static inline void stts(void) { write_cr0(read_cr0() | 8); }                   /* set CR0.TS */

/* XSAVE-family instructions take a mask in edx:eax: -1 = every component
 * the OS enabled in XCR0. */
static void save(void *a)
{
    if (xsaveopt) asm volatile(".byte 0x48, 0x0f, 0xae, 0x37" : : "D"(a), "a"(-1), "d"(-1) : "memory");  /* xsaveopt64 (%rdi) */
    else if (xsave) asm volatile(".byte 0x48, 0x0f, 0xae, 0x27" : : "D"(a), "a"(-1), "d"(-1) : "memory");  /* xsave64 (%rdi) */
    else asm volatile(".byte 0x48, 0x0f, 0xae, 0x07" : : "D"(a) : "memory");                           /* fxsave64 (%rdi) */
}
static void restore(void *a)
{
    if (xsave) asm volatile(".byte 0x48, 0x0f, 0xae, 0x2f" : : "D"(a), "a"(-1), "d"(-1) : "memory");   /* xrstor64 (%rdi) */
    else asm volatile(".byte 0x48, 0x0f, 0xae, 0x0f" : : "D"(a) : "memory");                         /* fxrstor64 (%rdi) */
}

/* On every CPU at start: turn the registers on for user mode. CR0: MP and
 * NE (report errors as exceptions), not EM (no emulation), TS set (lazy
 * start). CR4: OSFXSR, OSXMMEXCPT (SSE allowed), OSXSAVE; XCR0: which
 * components XSAVE manages (x87, SSE, and AVX if the CPU has it). */
void fpu_cpu_init(cpu_t *c)
{
    uint32_t r[4];
    cpuid(1, r);
    xsave = r[2] >> 26 & 1;
    int avx = r[2] >> 28 & 1;
    write_cr0((read_cr0() & ~4UL) | 2 | 32 | 8);
    write_cr4(read_cr4() | 1 << 9 | 1 << 10 | (xsave ? 1 << 18 : 0));
    c->a.ts = 1;
    if (!xsave) { area_size = 512; return; }
    uint64_t xcr0 = 3 | (avx ? 4 : 0);
    asm volatile(".byte 0x0f, 0x01, 0xd1" : : "c"(0), "a"((uint32_t)xcr0), "d"((uint32_t)(xcr0 >> 32)));  /* xsetbv */
    asm volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0xD), "c"(1));
    xsaveopt = r[0] & 1;
    asm volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0xD), "c"(0));
    area_size = r[1];                    /* EBX: the size for the components XCR0 enables */
}

/* An area: 64-byte aligned. kzalloc's blocks of 1024 and 2048 bytes are
 * aligned to their size; a CPU with bigger state (AVX-512) gets a page. */
static void *area_new(void)
{
    if (area_size <= 2048) return kzalloc(area_size);
    uint64_t pa = page_alloc(1);
    return pa ? P2V(pa) : 0;
}
void fpu_free(task_t *t)
{
    if (!t->fpu) return;
    if (area_size <= 2048) kfree(t->fpu); else page_free(V2P(t->fpu), 1);
    t->fpu = 0;
}

/* #NM in user mode: thread t's first floating-point instruction. Returns 1
 * if handled (the instruction is retried), 0 if out of memory. */
int fpu_first_use(task_t *t)
{
    cpu_t *c = this_cpu();
    if (!t->fpu) {
        uint8_t *a = area_new();
        if (!a) return 0;
        *(uint16_t *)a = 0x37F;          /* the legacy area's clean x87 control word ... */
        *(uint32_t *)(a + 24) = 0x1F80;  /* ... and SSE control (MXCSR); XSAVE's header
                                            (offset 512) stays zero: every other component
                                            is loaded in its initial state */
        t->fpu = a;
    }
    clts();
    c->a.ts = 0;
    restore(t->fpu);
    return 1;
}

/* A CPU leaves prev (may be 0: idle, or a thread that just ended) for next. */
void fpu_switch(cpu_t *c, task_t *prev, task_t *next)
{
    if (prev && prev->fpu && !c->a.ts) save(prev->fpu);
    if (next && next->fpu) {
        if (c->a.ts) { clts(); c->a.ts = 0; }
        restore(next->fpu);
    } else if (!c->a.ts) { stts(); c->a.ts = 1; }
}

uint32_t fpu_area_size(void) { return area_size; }
