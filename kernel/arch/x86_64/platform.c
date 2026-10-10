/*
 * platform.c (x86-64) - The PC around the processor: the boot loader's
 * information (multiboot), the first serial port for the kernel log,
 * powering off and restarting, and the CPU's random number generator.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "x86.h"
#include "mk/boot.h"

/* ---- Multiboot boot information (only what we use). */
struct mb_info { uint32_t flags, mem_lower, mem_upper, boot_device, cmdline,
                 mods_count, mods_addr, syms[4], mmap_length, mmap_addr; };
struct mb_mod  { uint32_t start, end, cmdline, pad; };
struct mb_mmap { uint32_t size; uint64_t addr, len; uint32_t type; } __attribute__((packed));

extern char _kernel_end[];          /* linker.ld */
static boot_info_t bi;              /* (large: kept out of the boot stack) */

static void add(range_t *r, int *n, int max, uint64_t pa, uint64_t len)
{
    if (*n < max && len) { r[*n].start = pa; r[*n].end = pa + len; (*n)++; }
}

/* ---- The log: COM1, polled. This is the only device the kernel drives
 * itself, and only for output (the console server enables its interrupts). */
#define COM1 0x3F8
static void serial_init(void)
{
    outb(COM1 + 1, 0);       /* no interrupts */
    outb(COM1 + 3, 0x80);    /* set the speed: divisor 1 = 115200 bit/s */
    outb(COM1 + 0, 1);
    outb(COM1 + 1, 0);
    outb(COM1 + 3, 3);       /* 8 bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xC7);    /* FIFOs on */
}
void arch_putc(char c)
{
    while (!(inb(COM1 + 5) & 0x20)) ;   /* wait until the port can take a byte */
    outb(COM1, c);
}

/* ---- SIEOS's own UEFI loader (boot/uefi, mk/boot.h): its information
 * instead of multiboot's. The firmware's memory map says which RAM is free
 * now that its boot services are gone: its "loader", "boot services" and
 * "conventional" memory. The loader put the modules, this information and
 * room for the page bitmap in one block (mem.c places the bitmap after the
 * last of them). No BIOS areas are assumed: the ACPI pointer is given. */
uint64_t x86_rsdp;
struct bootfb x86_fb;

static void from_uefi(uint64_t pa)
{
    sieos_boot_t *b = P2V(pa);
    if (b->version != 1) panic("UEFI loader: unknown version %u\n", b->version);
    for (uint64_t o = 0; o + b->desc_size <= b->mmap_size; o += b->desc_size) {
        uint8_t *d = (uint8_t *)P2V(b->mmap) + o;
        uint32_t type = *(uint32_t *)d;
        uint64_t start = *(uint64_t *)(d + 8), len = *(uint64_t *)(d + 24) * PAGE;
        if (!(type >= 1 && type <= 4) && type != 7) continue;          /* not free RAM */
        if (bi.nram && bi.ram[bi.nram - 1].end == start) bi.ram[bi.nram - 1].end += len;   /* adjacent: merge */
        else add(bi.ram, &bi.nram, MAXRANGE, start, len);
    }
    add(bi.keep, &bi.nkeep, MAXRANGE, b->self, b->self_len);           /* this information and the map */
    for (uint32_t i = 0; i < b->nmod && i < MAXMOD; i++) {
        bi.mod[i].pa = b->mod[i].pa;
        bi.mod[i].len = b->mod[i].len;
        bi.mod[i].cmdline = b->mod[i].cmdline;
        bi.nmod++;
    }
    x86_rsdp = b->rsdp;
    x86_fb = (struct bootfb){ b->fb.pa, b->fb.size, b->fb.width, b->fb.height, b->fb.pitch, b->fb.format };
    add(bi.rsvd, &bi.nrsvd, 8, 0, PAGE);                               /* page 0: "no page" */
    add(bi.rsvd, &bi.nrsvd, 8, TRAMP, PAGE);                           /* where the other CPUs start */
    kprintf("mk: started by the UEFI loader, %d RAM ranges%s\n", bi.nram, x86_fb.pa ? ", firmware screen" : "");
}

/* SYS_SCREEN: the firmware's screen, as the loader found it. */
long arch_screen(uint64_t ubuf)
{
    if (!x86_fb.pa) return -ENOSYS;
    mk_screen_t s = { x86_fb.pa, x86_fb.size, x86_fb.w, x86_fb.h, x86_fb.pitch, x86_fb.fmt };
    return vm_copy(cur->proc->as, (void *)ubuf, 0, &s, sizeof s) ? -EFAULT : 0;
}

/* boot.S calls kmain in 64-bit mode with the boot loader's magic number
 * and information: turn it into a boot_info_t for the portable kernel. */
void kmain(uint32_t magic, uint32_t mbi_pa)
{
    serial_init();
    if (magic == SIEOS_BOOT_MAGIC) {
        from_uefi(mbi_pa);
        bi.kernel_start = 0x100000;
        bi.kernel_end = (uint64_t)_kernel_end - KVMA;
        bi.dmap_boot = 4UL << 30;
        kernel_main(&bi);
    }
    if (magic != 0x2BADB002) panic("not started by a multiboot boot loader\n");
    struct mb_info *mbi = P2V(mbi_pa);
    if (!(mbi->flags & 1 << 6)) panic("no memory map from the boot loader\n");
    uint8_t *mm = P2V(mbi->mmap_addr), *mm_end = mm + mbi->mmap_length;
    struct mb_mmap *e;
    for (uint8_t *p = mm; p < mm_end; p += e->size + 4)
        if ((e = (void *)p)->type == 1) add(bi.ram, &bi.nram, MAXRANGE, e->addr, e->len);

    /* What the boot loader left for us: its information and the modules'
     * names (freed once read), and the modules (kept: init restarts
     * servers from them). */
    struct mb_mod *mod = P2V(mbi->mods_addr);
    add(bi.keep, &bi.nkeep, MAXRANGE, mbi_pa, sizeof *mbi);
    add(bi.keep, &bi.nkeep, MAXRANGE, mbi->mmap_addr, mbi->mmap_length);
    add(bi.keep, &bi.nkeep, MAXRANGE, mbi->mods_addr, mbi->mods_count * sizeof *mod);
    for (uint32_t i = 0; i < mbi->mods_count; i++) {
        add(bi.keep, &bi.nkeep, MAXRANGE, mod[i].cmdline, strlen(P2V(mod[i].cmdline)) + 1);
        if (i < MAXMOD) {
            bi.mod[i].pa = mod[i].start;
            bi.mod[i].len = mod[i].end - mod[i].start;
            bi.mod[i].cmdline = P2V(mod[i].cmdline);
            bi.nmod++;
        }
    }

    /* In the first MiB only what is really needed: page 0 (BIOS data; and
     * physical address 0 means "no page"), the BIOS's extended data area
     * (it may hold the ACPI pointer, smp.c), and the page where the other
     * CPUs start (smp.c frees it once they run). */
    uint64_t ebda = (uint64_t)*(uint16_t *)P2V(0x40E) << 4;
    add(bi.rsvd, &bi.nrsvd, 8, 0, PAGE);
    if (ebda >= 0x80000 && ebda < 0xA0000) add(bi.rsvd, &bi.nrsvd, 8, ebda & ~(PAGE - 1), 0xA0000 - (ebda & ~(PAGE - 1)));
    add(bi.rsvd, &bi.nrsvd, 8, TRAMP, PAGE);
    bi.kernel_start = 0x100000;
    bi.kernel_end = (uint64_t)_kernel_end - KVMA;
    bi.dmap_boot = 4UL << 30;            /* boot.S maps the first 4 GiB */
    kernel_main(&bi);
}

/* ---- Power. (The core has already stopped the other CPUs.) */
void arch_power(int restart)
{
    if (restart) outb(0x64, 0xFE);      /* the keyboard controller resets the machine */
    else { outw(0x604, 0x2000); outw(0xB004, 0x2000); }  /* ACPI off in QEMU (new and old) */
    arch_halt_forever();
}

/* ---- The CPU's own random number generator: RDSEED (true random bits)
 * if present, else RDRAND (a generator the CPU reseeds itself). */
static int has_seed, has_rand;

const char *arch_hw_random_init(void)
{
    uint32_t r[4];
    cpuid(1, r); has_rand = r[2] >> 30 & 1;
    cpuid(0, r);
    if (r[0] >= 7) { cpuid(7, r); has_seed = r[1] >> 18 & 1; }
    return has_seed ? "RDSEED" : has_rand ? "RDRAND" : 0;
}

/* Features for programs (SYS_INFO). AVX counts only if the kernel enabled
 * it (CR4.OSXSAVE set and XCR0 holding the AVX state, see fpu.c). */
uint32_t arch_hwcap(void)
{
    uint32_t r[4], cap = 0, xcr0 = 0, max;
    cpuid(0, r); max = r[0];
    cpuid(1, r);
    if (r[2] >> 27 & 1) asm volatile(".byte 0x0F, 0x01, 0xD0" : "=a"(xcr0) : "c"(0) : "edx");   /* xgetbv (as bytes: sicc's assembler lacks it) */
    if ((xcr0 & 6) != 6) return 0;                       /* no AVX state saved: no AVX */
    if (r[2] >> 12 & 1) cap |= HWCAP_FMA;
    if (max >= 7) {
        cpuid(7, r); if (r[1] >> 5 & 1) cap |= HWCAP_AVX2;
        asm volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(7), "c"(1));
        if (r[0] >> 4 & 1) cap |= HWCAP_AVXVNNI;
    }
    return cap;
}

int arch_hw_random(uint64_t *v)
{
    uint8_t ok = 0;
    for (int i = 0; i < 16 && !ok; i++) {
        if (has_seed) asm volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok));
        else if (has_rand) asm volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
        else return 0;
    }
    return ok;
}

/* ---- No device tree on a PC (ACPI tables describe it instead). */
long arch_fdt(uint64_t ubuf, uint64_t size) { (void)ubuf; (void)size; return -ENOSYS; }
