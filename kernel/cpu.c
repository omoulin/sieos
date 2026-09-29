/*
 * cpu.c - Per-CPU feature setup: x87/SSE state, NX pages, the syscall
 * instruction (ABI v2).
 *
 * FPU/SSE registers belong to user processes only (the kernel is compiled
 * with -mgeneral-regs-only), so they are saved and restored eagerly at every
 * context switch with fxsave/fxrstor.
 */
#include "kernel.h"
#include "arch.h"
#include "cpu.h"
#include "mm.h"
#include "smp.h"

#define MSR_EFER  0xC0000080
#define MSR_STAR  0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084
#define EFER_SCE  (1UL << 0)
#define EFER_NXE  (1UL << 11)

uint64_t pte_nx;                                   /* PTE_NX when the CPU supports it, else 0 */
struct fpu_state fpu_default __attribute__((aligned(16)));
bool cpu_has_sse = true;

extern void syscall_entry(void);                   /* isr.S */

static inline void cpuid(uint32_t leaf, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

static inline uint64_t read_cr0(void) { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint64_t v) { __asm__ volatile("mov %0, %%cr0" :: "r"(v)); }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr4(uint64_t v) { __asm__ volatile("mov %0, %%cr4" :: "r"(v)); }

void cpu_features_init(bool bsp)
{
    uint32_t r[4];
    /* x87 + SSE: CR0.MP=1 EM=0 TS=0 NE=1, CR4.OSFXSR, CR4.OSXMMEXCPT */
    write_cr0((read_cr0() | (1UL << 1) | (1UL << 5)) & ~((1UL << 2) | (1UL << 3)));
    write_cr4(read_cr4() | (1UL << 9) | (1UL << 10));
    __asm__ volatile("fninit");
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
    if (bsp) {
        __asm__ volatile("fxsave %0" : "=m"(fpu_default));
        cpuid(0x80000000, r);
        uint32_t max_ext = r[0];
        bool nx = false;
        if (max_ext >= 0x80000001) {
            cpuid(0x80000001, r);
            nx = r[3] & (1u << 20);
        }
        pte_nx = nx ? PTE_NX : 0;
    }

    uint64_t efer = rdmsr(MSR_EFER) | EFER_SCE;
    if (pte_nx)
        efer |= EFER_NXE;
    wrmsr(MSR_EFER, efer);
    /* syscall: CS = KERNEL_CS, SS = KERNEL_CS + 8; the user selectors (for
     * sysret) are not used: every return goes through iretq. */
    wrmsr(MSR_STAR, (uint64_t)KERNEL_CS << 32);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x47700);                     /* clear IF, TF, DF, AC, NT on entry */
    /* PAT entry 1 (the PWT bit alone) becomes write-combining, for framebuffers */
    uint32_t f[4];
    cpuid(1, f);
    if (f[3] & (1u << 16)) {
        uint64_t pat = rdmsr(0x277);
        pat = (pat & ~(0xFFULL << 8)) | (0x01ULL << 8);
        __asm__ volatile("wbinvd" ::: "memory");
        wrmsr(0x277, pat);
        __asm__ volatile("wbinvd" ::: "memory");
        if (bsp)
            pat_wc = true;
    }
}

bool pat_wc;

void fpu_save(struct fpu_state *f)
{
    __asm__ volatile("fxsave %0" : "=m"(*f));
}

void fpu_restore(const struct fpu_state *f)
{
    __asm__ volatile("fxrstor %0" :: "m"(*f));
}
