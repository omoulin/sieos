/*
 * pci.c - Finding a PCI device and making it usable (pci.h).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "pci.h"

#if defined(__x86_64__)
/* Configuration space through the kernel (SYS_PCI): other drivers scan the
 * bus too, and the two ports must not be shared halfway. */
uint32_t pci_cfg(const pci_t *p, int reg)            { return pci_read(p->at | reg); }
void pci_cfgw(const pci_t *p, int reg, uint32_t v)   { pci_write(p->at | reg, v); }

/* Every bus, from bus 0 through the bridges (their secondary bus). A
 * device found behind bridges gets memory decoding and bus mastering on
 * each of them up to bus 0, as Linux does: a firmware may leave a root
 * port's off (the Surface Pro 7's NVMe drive: its registers answered, its
 * DMA never arrived). */
int pci_scan(pci_t *p, int reg, uint32_t val, uint32_t mask, int nth)
{
    int bus[64] = { 0 }, up[64] = { -1 }, nb = 1;   /* a bus; the entry of the bus its bridge is on */
    uint32_t br[64] = { 0 };                          /* that bridge */
    memset(p, 0, sizeof *p);
    for (int bi = 0; bi < nb; bi++)
        for (int d = 0; d < 32; d++)
            for (int f = 0; f < 8; f++) {
                p->at = (uint32_t)(bus[bi] << 16 | d << 11 | f << 8);
                if (pci_cfg(p, 0) == 0xFFFFFFFF) { if (!f) break; continue; }
                uint32_t hdr = pci_cfg(p, 0xC) >> 16;
                if ((hdr & 0x7F) == 1 && nb < 64) { br[nb] = p->at; up[nb] = bi; bus[nb++] = pci_cfg(p, 0x18) >> 8 & 0xFF; }
                if ((pci_cfg(p, reg) & mask) == val && !nth--) {
                    for (int b = bi; b > 0; b = up[b]) {
                        uint32_t c = pci_read(br[b] | 4);
                        if ((c & 6) != 6) pci_write(br[b] | 4, c | 6);
                    }
                    int line = pci_cfg(p, 0x3C) & 0xFF;  /* routed by the chipset to an ISA IRQ: level */
                    p->irq = line && line < 0xFF ? line | IRQ_LEVEL : -1;
                    p->id = pci_cfg(p, 0);
                    return 0;
                }
                if (!f && !(hdr & 0x80)) break;           /* not multi-function */
            }
    return -1;
}

#elif defined(__aarch64__)
#include "mk/fdt.h"

static int cellsof(const void *fdt, int node, const char *name, int dflt)
{
    int len;
    const uint8_t *p = fdt_prop(fdt, node, name, &len);
    return p && len == 4 ? (int)fdt_be32(p) : dflt;
}

/* "ranges" entries: PCI address (3 cells: space code, then 2), CPU
 * address, size. The first 32-bit memory one (space code 2). */
int pci_window(const void *fdt, int node, const char *prop, uint64_t *pci, uint64_t *cpu, uint64_t *size)
{
    int len, pac = 2, sc = cellsof(fdt, node, "#size-cells", 2);
    const uint8_t *r = fdt_prop(fdt, node, prop, &len);
    int e = 4 * (3 + pac + sc);
    for (int i = 0; r && i + e <= len; i += e)
        if ((fdt_be32(r + i) >> 24 & 3) == 2) {
            *pci = fdt_cells(r + i + 4, 2);
            *cpu = fdt_cells(r + i + 12, pac);
            *size = fdt_cells(r + i + 12 + 4 * pac, sc);
            return 0;
        }
    return -1;
}

int pci_pin_irq(const void *fdt, int node, uint32_t devfn_hi, int pin)
{
    int len, mlen;
    const uint8_t *m = fdt_prop(fdt, node, "interrupt-map", &len), *k = fdt_prop(fdt, node, "interrupt-map-mask", &mlen);
    uint32_t mask[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
    for (int i = 0; k && i < 4 && 4 * i + 4 <= mlen; i++) mask[i] = fdt_be32(k + 4 * i);
    for (int i = 0; m && i + 20 <= len; ) {
        uint32_t hi = fdt_be32(m + i), ip = fdt_be32(m + i + 12);
        int parent = fdt_phandle(fdt, fdt_be32(m + i + 16));
        if (parent < 0) return -1;
        int pac = cellsof(fdt, parent, "#address-cells", 0), pic = cellsof(fdt, parent, "#interrupt-cells", 3);
        const uint8_t *spec = m + i + 20 + 4 * pac;
        if ((hi & mask[0]) == (devfn_hi & mask[0]) && (ip & mask[3]) == ((uint32_t)pin & mask[3]) && pic == 3 &&
            fdt_be32(spec) == 0)
            return (int)fdt_be32(spec + 4) | (fdt_be32(spec + 8) & 0xC ? IRQ_LEVEL : 0);
        i += 20 + 4 * (pac + pic);
    }
    return -1;
}

/* QEMU's virt: the bridge, read from the device tree once (then dropped:
 * it is about 1 MiB): its configuration space mapped, its window, and the
 * line of each of bus 0's 32 slots. */
static struct { int done; volatile uint8_t *e; uint64_t wpci, wcpu; int64_t bus_off; int irq[32]; } br;

static void bridge(void)
{
    long size = sys_fdt(0, 0);
    char *f = size > 0 ? malloc(size) : 0;
    uint64_t base, len, wsize;
    int n;
    br.done = 1;
    if (f && sys_fdt(f, size) == size && (n = fdt_find(f, -1, "pci-host-ecam-generic")) >= 0 &&
        !fdt_reg(f, n, 0, &base, &len) && !pci_window(f, n, "ranges", &br.wpci, &br.wcpu, &wsize) &&
        (long)(br.e = map_phys(base, 1 << 20)) > 0) {
        uint64_t bus;
        if (!fdt_bus_addr(f, n, 0x40000000, &bus)) br.bus_off = (int64_t)bus - 0x40000000;
        for (int d = 0; d < 32; d++) {
            int pin = ((volatile uint32_t *)(br.e + (d << 15)))[15] >> 8 & 0xFF;
            br.irq[d] = pin ? pci_pin_irq(f, n, (uint32_t)d << 11, pin) : -1;
        }
    } else br.e = 0;
    free(f);
}

uint32_t pci_cfg(const pci_t *p, int reg)            { return p->cfg[reg / 4]; }
void pci_cfgw(const pci_t *p, int reg, uint32_t v)   { p->cfg[reg / 4] = v; }

int pci_scan(pci_t *p, int reg, uint32_t val, uint32_t mask, int nth)
{
    if (!br.done) bridge();
    for (int d = 0; br.e && d < 32; d++) {
        volatile uint32_t *c = (volatile uint32_t *)(br.e + (d << 15));
        if (c[0] == 0xFFFFFFFF || (c[reg / 4] & mask) != val || nth--) continue;
        *p = (pci_t){ c[0], br.irq[d], br.bus_off, (uint32_t)d << 11, c, br.wpci, br.wcpu };
        return 0;
    }
    return -1;
}
#endif

/* Powered to D0 (PMCSR) if the firmware left it lower: -> the state it was
 * in (0: it was on). A device without "no soft reset" comes out of D3hot
 * reset, its BARs and command register cleared (an LPSS UART the Surface
 * Pro 7's firmware left in D3: BAR0 then read 0): they are written back,
 * as Linux's pci_restore_bars does. */
int pci_d0(pci_t *p)
{
    for (int cp = pci_cfg(p, 0x34) & 0xFC, n = 0; cp && n < 48; cp = pci_cfg(p, cp) >> 8 & 0xFC, n++) {
        if ((pci_cfg(p, cp) & 0xFF) != 1) continue;
        uint32_t pm = pci_cfg(p, cp + 4), saved[7];
        if (!(pm & 3)) return 0;
        for (int i = 0; i < 6; i++) saved[i] = pci_cfg(p, 0x10 + 4 * i);
        saved[6] = pci_cfg(p, 4);
        pci_cfgw(p, cp + 4, pm & ~3u);
        sys_sleep(10000000);                       /* (D3hot -> D0: 10 ms) */
        for (int i = 0; i < 6; i++) if (pci_cfg(p, 0x10 + 4 * i) != saved[i]) pci_cfgw(p, 0x10 + 4 * i, saved[i]);
        if ((pci_cfg(p, 4) & 0x407) != (saved[6] & 0x407)) pci_cfgw(p, 4, (pci_cfg(p, 4) & ~0x407u) | (saved[6] & 0x407));
        return (int)(pm & 3);
    }
    return 0;
}

/* The device's interrupt as a message: MSI if it has the capability, else
 * MSI-X's first table entry (the kernel's address and data, SYS_IRQ_MSI),
 * its line (INTx) then off. Firmware often leaves no line on laptops (the
 * Surface Pro 7's xHCI): Linux uses MSI there too. */
int pci_msi(pci_t *p, long port, const char **how)
{
    int msi = 0, msix = 0;
    if (pci_cfg(p, 4) >> 20 & 1)                   /* a capability list */
        for (int cp = pci_cfg(p, 0x34) & 0xFC, n = 0; cp && n < 48; cp = pci_cfg(p, cp) >> 8 & 0xFC, n++) {
            int id = pci_cfg(p, cp) & 0xFF;
            if (id == 5 && !msi) msi = cp;
            if (id == 0x11 && !msix) msix = cp;
        }
    uint32_t m[2];
    long irq;
    if ((!msi && !msix) || (irq = irq_msi(port, m)) < 0) return -1;
    if (msi) {                                     /* address (64-bit: high half 0), data, its vector unmasked, enabled */
        uint32_t ctl = pci_cfg(p, msi);
        int b64 = ctl >> 23 & 1, data = msi + (b64 ? 12 : 8);
        pci_cfgw(p, msi + 4, m[0]);
        if (b64) pci_cfgw(p, msi + 8, 0);
        pci_cfgw(p, data, m[1]);
        if (ctl >> 24 & 1) pci_cfgw(p, data + 4, 0); /* (per-vector mask bits) */
        pci_cfgw(p, msi, (ctl & ~(7u << 20)) | 1u << 16);   /* one message, on */
        *how = "MSI";
    } else {                                       /* the table: in a BAR, entry 0 */
        uint32_t t = pci_cfg(p, msix + 4), ctl = pci_cfg(p, msix);
        uint64_t len, pa = pci_bar(p, t & 7, &len);
        volatile uint32_t *e = pa ? map_phys(pa + (t & ~7u), 16) : 0;
        if (!pa || (long)e < 0) return -1;         /* (the MSI stays taken: harmless) */
        pci_cfgw(p, msix, ctl | 1u << 30);         /* (function masked while written) */
        e[0] = m[0]; e[1] = 0; e[2] = m[1]; e[3] = 0;
        pci_cfgw(p, msix, (ctl & ~(1u << 30)) | 1u << 31);
        *how = "MSI-X";
    }
    pci_cfgw(p, 4, pci_cfg(p, 4) | 1u << 10);      /* its line off */
    return (int)irq;
}

/* Sizing: write all ones, read back which bits stick, restore. A memory BAR
 * with no address in the window (none given yet) gets one: device d's BAR i
 * at the window's d-th 16 MiB slot + i MiB (aligned to its size), so drivers
 * that each set up their own device never give two the same; one already
 * given (by the firmware, or an earlier start of the driver) is kept. */
uint64_t pci_bar(pci_t *p, int i, uint64_t *len)
{
    int r = 0x10 + 4 * i;
    if (i > 5) return 0;
    uint32_t lo = pci_cfg(p, r);
    if (lo & 1) {                                  /* an I/O range */
        pci_cfgw(p, 4, pci_cfg(p, 4) | 5);         /* I/O decoding, bus mastering on */
        *len = 0;
        return lo & ~3u;
    }
    int b64 = (lo & 6) == 4;
    uint32_t hi = b64 ? pci_cfg(p, r + 4) : 0;
    uint64_t old = (lo & ~0xFu) | (uint64_t)hi << 32, at = old;
    uint32_t cmd = pci_cfg(p, 4);
    pci_cfgw(p, 4, cmd & ~3u);                     /* (no decoding while the BAR holds all ones) */
    pci_cfgw(p, r, ~0u); if (b64) pci_cfgw(p, r + 4, ~0u);
    uint64_t m = (b64 ? (uint64_t)pci_cfg(p, r + 4) << 32 : 0xFFFFFFFF00000000ull) | (pci_cfg(p, r) & ~0xFu);
    uint64_t sz = ~m + 1;
    if (!sz || sz > (1u << 24)) sz = 0x10000;
    if (!old || old < p->wpci)
        at = p->wcpu ? (p->wpci + ((uint64_t)(p->at >> 11 & 31) << 24) + ((uint64_t)i << 20) + sz - 1) & ~(sz - 1) : 0;
    if (!(uint32_t)m) at = 0;                      /* no such BAR: nothing sticks */
    pci_cfgw(p, r, at ? (uint32_t)at | (lo & 0xF) : lo); if (b64) pci_cfgw(p, r + 4, at ? (uint32_t)(at >> 32) : hi);
    pci_cfgw(p, 4, at ? cmd | 6 : cmd);           /* memory decoding, bus mastering on */
    if (!at) return 0;
    *len = sz;
    return at - p->wpci + p->wcpu;                 /* the CPU's address for it */
}
