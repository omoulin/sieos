/*
 * platform.c (arm64) - The machine around the processor, as its device
 * tree describes it: the RAM, the reserved areas, the boot modules (one
 * archive, the "initrd"), the serial port for the kernel log (PL011),
 * power (PSCI), and the CPU's random number generator (RNDR, ARMv8.5).
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "kernel.h"
#include "arm64.h"
#include "mk/fdt.h"

extern char _kernel_end[];          /* linker.ld */
const void *fdt;                    /* the device tree, in the direct map */
static uint32_t fdt_size;
uint64_t phys_base;                 /* where the kernel image is, physically */
boot_info_t *boot_bi;
static boot_info_t bi;              /* (large: kept out of the boot stack) */
range_t forbid[4];                  /* the kernel's own devices: never mapped for drivers */
range_t screen[2];                  /* the firmware's frame buffers: never allocated, mapped for the desktop */
int nscreen;
int nforbid;
long (*psci)(uint64_t fn, uint64_t a, uint64_t b, uint64_t c);

static void add(range_t *r, int *n, int max, uint64_t pa, uint64_t len)
{
    if (*n < max && len) { r[*n].start = pa; r[*n].end = pa + len; (*n)++; }
}

/* ---- The log: a PL011 UART, polled (the console server also uses it,
 * with its interrupts: SYS_MAP_PHYS). Lines written before it is found
 * are lost, so it is found first thing. */
static volatile uint32_t *uart;
void arch_putc(char c)
{
    if (!uart) return;
    while (uart[0x18 / 4] & 1 << 5) ;   /* FR.TXFF: transmit FIFO full */
    uart[0] = (uint8_t)c;
}

/* The console's node: /chosen's "stdout-path" ("/pl011@9000000", or an
 * alias like "serial0:115200n8"), else the first PL011. */
static int console_node(void)
{
    int len, n = -1;
    const char *p = fdt_prop(fdt, fdt_path(fdt, "/chosen"), "stdout-path", &len);
    if (p && len > 1) {
        char path[64];
        int k = 0;
        while (k < 63 && k < len && p[k] && p[k] != ':') { path[k] = p[k]; k++; }
        path[k] = 0;
        if (path[0] != '/') {                                   /* an alias */
            int a = fdt_path(fdt, "/aliases");
            p = a >= 0 ? fdt_prop(fdt, a, path, &len) : 0;
            if (p) n = fdt_path(fdt, p);
        } else n = fdt_path(fdt, path);
    }
    /* (a Pi's firmware names its "mini UART", which is not a PL011: then the
     * first PL011, the Pi's UART0, which config.txt puts on the GPIO pins) */
    return n >= 0 && fdt_compatible(fdt, n, "arm,pl011") ? n : fdt_find(fdt, -1, "arm,pl011");
}

/* The initrd: our module archive (tools/mkmods.py): "SIEOSMOD", count, then
 * {name[48], offset, size} entries. The modules stay where they are (init
 * restarts servers from them); the header is freed once read. */
static void modules_from(uint64_t pa, uint64_t len)
{
    const uint8_t *a = P2V(pa);
    if (len < 16 || memcmp(a, "SIEOSMOD", 8)) {
        kprintf("mk: the initrd is not a SIEOS module archive\n");
        diag_fault(2, 0, pa, len);                   /* (on a Pi's screen: kind 2) */
        return;
    }
    uint32_t n = *(const uint32_t *)(a + 8);
    if (16 + 64UL * n > len) return;
    add(bi.keep, &bi.nkeep, MAXRANGE, pa, 16 + 64UL * n);
    for (uint32_t i = 0; i < n && bi.nmod < MAXMOD; i++) {
        const uint8_t *e = a + 16 + 64 * i;
        uint64_t off = *(const uint64_t *)(e + 48), size = *(const uint64_t *)(e + 56);
        if (off + size > len || e[47]) continue;
        bi.mod[bi.nmod].pa = pa + off;
        bi.mod[bi.nmod].len = size;
        bi.mod[bi.nmod].cmdline = (const char *)e;
        bi.nmod++;
    }
}

/* boot.S calls this at the kernel's high address: dtb = the device tree's
 * physical address, phys = the image's. */
void arm64_main(uint64_t dtb, uint64_t phys)
{
    phys_base = phys;
    diag_stage(7);                               /* step 7: C, at the high address */
    fdt = P2V(dtb);
    if (!(fdt_size = fdt_valid(fdt, 1 << 24))) arch_halt_forever();   /* no device tree: no way to talk */
    uint64_t a, s;
    int n = console_node();
    if (n >= 0 && !fdt_reg(fdt, n, 0, &a, &s)) {
        uart = kmap_dev(a, PAGE);
        if (!(uart[0x30 / 4] & 1)) {             /* not enabled by firmware (QEMU's -kernel): */
            uart[0x2C / 4] = 0x70;               /* 8 bits, FIFOs on, */
            uart[0x30 / 4] = 0x301;              /* receive, transmit, UART on */
        }
    }
    boot_bi = &bi;

    /* RAM: every node of type "memory" */
    for (int d = 0, m = -1; (m = fdt_next(fdt, m, &d)) >= 0; ) {
        const char *t = fdt_prop(fdt, m, "device_type", 0);
        if (t && !strcmp(t, "memory"))
            for (int i = 0; !fdt_reg(fdt, m, i, &a, &s); i++) add(bi.ram, &bi.nram, MAXRANGE, a, s);
    }
    if (!bi.nram) panic("no memory in the device tree\n");

    /* Never to be used: the firmware's reserved areas (the "memreserve"
     * block, and /reserved-memory's children), and the device tree itself
     * (SYS_FDT gives drivers a copy of it). */
    const uint8_t *rsv = (const uint8_t *)fdt + fdt_be32((const uint8_t *)fdt + 16);
    for (int i = 0; i < 8; i++, rsv += 16) {
        uint64_t ra = fdt_cells(rsv, 2), rs = fdt_cells(rsv + 8, 2);
        if (!ra && !rs) break;
        add(bi.rsvd, &bi.nrsvd, 8, ra, rs);
    }
    int r = fdt_path(fdt, "/reserved-memory");
    for (int d = 1, m = r; r >= 0 && (m = fdt_next(fdt, m, &d)) >= 0 && d > 1; )
        if (d == 2 && !fdt_reg(fdt, m, 0, &a, &s)) add(bi.rsvd, &bi.nrsvd, 8, a, s);
    /* the frame buffer the firmware set up and describes (the Pi 5's
     * "simple-framebuffer"): the desktop draws there, the kernel never uses it */
    for (int fb = -1; (fb = fdt_find(fdt, fb, "simple-framebuffer")) >= 0; )
        if (!fdt_reg(fdt, fb, 0, &a, &s)) { add(bi.rsvd, &bi.nrsvd, 8, a, s); add(screen, &nscreen, 2, a, s); }
    /* the other CPUs wait on their "spin table" release addresses (the Pi 4:
     * in the firmware's first page, usually also a /memreserve/): keep them */
    for (int d = 0, c = -1; (c = fdt_next(fdt, c, &d)) >= 0; ) {
        int l;
        const void *ra = fdt_prop(fdt, c, "cpu-release-addr", &l);
        if (!ra) continue;
        uint64_t p = fdt_cells(ra, l / 4) & ~(PAGE - 1);
        int seen = 0;                                /* (the 3 CPUs share one page: once) */
        for (int i = 0; i < bi.nrsvd; i++) seen |= bi.rsvd[i].start <= p && p < bi.rsvd[i].end;
        if (!seen) add(bi.rsvd, &bi.nrsvd, 8, p, PAGE);
    }
    add(bi.rsvd, &bi.nrsvd, 8, dtb, fdt_size);
    add(bi.keep, &bi.nkeep, MAXRANGE, dtb, fdt_size);   /* (so the page bitmap goes after it too) */

    /* the boot modules: /chosen's linux,initrd-start and -end (1 or 2 cells) */
    int ch = fdt_path(fdt, "/chosen"), l1, l2;
    const void *is = ch >= 0 ? fdt_prop(fdt, ch, "linux,initrd-start", &l1) : 0;
    const void *ie = ch >= 0 ? fdt_prop(fdt, ch, "linux,initrd-end", &l2) : 0;
    if (is && ie) {
        uint64_t s0 = fdt_cells(is, l1 / 4), e0 = fdt_cells(ie, l2 / 4);
        /* the kernel cleared its .bss over them? (the firmware's
         * "followkernel" may ignore the Image header's size in memory) */
        if (s0 < phys + ((uint64_t)_kernel_end - KVMA) && e0 > phys) {
            kprintf("mk: the boot modules overlap the kernel's memory: give them their own address in config.txt\n");
            diag_fault(2, 0, s0, phys);
        }
        if (e0 > s0) modules_from(s0, e0 - s0);
    } else diag_fault(2, 0, 0, 0);                    /* no boot modules at all */

    /* PSCI: how to start CPUs and power off */
    int ps = fdt_find(fdt, -1, "arm,psci-1.0");
    if (ps < 0) ps = fdt_find(fdt, -1, "arm,psci-0.2");
    if (ps < 0) ps = fdt_find(fdt, -1, "arm,psci");
    const char *method = ps >= 0 ? fdt_prop(fdt, ps, "method", 0) : 0;
    psci = !method ? 0 : !strcmp(method, "smc") ? psci_smc : psci_hvc;

    bi.kernel_start = phys;
    bi.kernel_end = phys + ((uint64_t)_kernel_end - KVMA);
    bi.dmap_boot = ((phys >> 30) + 1) << 30;    /* boot.S: the kernel's GiB is in the direct map */
    diag_stage(8);                               /* step 8: memory, modules, firmware read */
    kernel_main(&bi);
}

/* ---- Power, through PSCI (SYSTEM_OFF, SYSTEM_RESET). (The core has
 * already stopped the other CPUs.) */
void arch_power(int restart)
{
    if (psci) psci(restart ? 0x84000009 : 0x84000008, 0, 0, 0);
    /* No PSCI (the Raspberry Pi 4): its power manager's watchdog. A reset in
     * a few ticks; to power off, the reset first records the "halt"
     * partition (63, in RSTS's even bits), and the firmware then stops
     * instead of starting again. (QEMU's raspi4b resets, which -no-reboot
     * turns into stopping.) */
    int n = fdt_find(fdt, -1, "brcm,bcm2835-pm-wdt");
    uint64_t a, len;
    if (n >= 0 && !fdt_reg(fdt, n, 0, &a, &len)) {
        volatile uint32_t *pm = kmap_dev(a, 0x28);
        enum { RSTC = 0x1C / 4, RSTS = 0x20 / 4, WDOG = 0x24 / 4 };
        const uint32_t pw = 0x5A000000;
        if (!restart) pm[RSTS] = pw | (pm[RSTS] & ~0xFFFFFAAAu & 0xFFFFFu) | 0x555;
        pm[WDOG] = pw | 10;
        pm[RSTC] = pw | (pm[RSTC] & ~0x30u & 0xFFFFFu) | 0x20;   /* "full reset" when the watchdog ends */
    }
    arch_halt_forever();
}

/* ---- The CPU's random number generator: RNDR (ARMv8.5), if present
 * (ID_AA64ISAR0_EL1 bits 63-60). It sets the Z flag when it has nothing. */
static int has_rndr;
const char *arch_hw_random_init(void)
{
    uint64_t isar;
    asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar));
    has_rndr = (isar >> 60) != 0;
    return has_rndr ? "RNDR" : 0;
}
/* Features for programs (SYS_INFO): ID_AA64ISAR0_EL1 is readable only at
 * EL1, so the kernel passes on what the engine and libraries need. */
uint32_t arch_hwcap(void)
{
    uint64_t isar;
    asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar));
    return ((isar >> 44 & 0xF) ? HWCAP_DOTPROD : 0)      /* DP, bits 47-44 */
         | ((isar >> 20 & 0xF) >= 2 ? HWCAP_LSE : 0);    /* Atomic, bits 23-20 */
}
int arch_hw_random(uint64_t *v)
{
    if (!has_rndr) return 0;
    for (int i = 0; i < 16; i++) {
        uint64_t x, ok;
        asm volatile("mrs %0, s3_3_c2_c4_0; cset %1, ne" : "=r"(x), "=r"(ok));
        if (ok) { *v = x; return 1; }
    }
    return 0;
}

/* ---- SYS_FDT: a copy of the device tree, for drivers to find their
 * devices. Returns its size (a buffer too small gets nothing). */
long arch_fdt(uint64_t ubuf, uint64_t size)
{
    if (size < fdt_size) return fdt_size;
    return vm_copy(cur->proc->as, (void *)ubuf, 0, fdt, fdt_size) ? -EFAULT : fdt_size;
}

/* ---- SYS_SCREEN: no UEFI loader here (the screen comes from the device tree). */
long arch_screen(uint64_t ubuf) { (void)ubuf; return -ENOSYS; }
