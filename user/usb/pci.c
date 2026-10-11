/*
 * pci.c - Finding the xHCI controller (PCI class 0C/03/30) and making it
 * usable: its registers mapped at an address, memory and DMA ("bus
 * master") on, its interrupt line known. pci_find does the same for any
 * class (the disk server uses it for an SD controller, 08/05, on QEMU's
 * virt: user/vblk/sd.c).
 *   x86-64:  PCI configuration through ports 0xCF8/0xCFC; the firmware has
 *            already given the controller its address and interrupt line.
 *   QEMU virt (arm64): a "pci-host-ecam-generic" bridge in the device tree,
 *            configuration space in memory (ECAM); nobody assigned addresses
 *            (we start without firmware), so we give BAR 0 one from the
 *            bridge's memory window, and find the interrupt in its
 *            "interrupt-map".
 *   Raspberry Pi 4 (untested): the BCM2711's PCIe root complex and the
 *            VL805 USB chip behind it, set up by the firmware when it brought
 *            the PCIe link up; we assign the VL805's address and ask the
 *            firmware to load its microcode (mailbox "notify xHCI reset").
 *   Raspberry Pi 5 (untested): the RP1 chip's USB controllers (DWC3 cores,
 *            in xHCI mode), at the address the firmware's PCIe window gives.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "usb.h"

static void name_xhci(hc_info_t *h) { char t[40]; strlcpy(t, h->what, sizeof t); sfmt(h->what, sizeof h->what, "xHCI %s", t); }

#if defined(__x86_64__)
/* Configuration space through the kernel (SYS_PCI): other drivers scan the
 * bus too, and the two ports must not be shared halfway. */
static uint32_t cfg(int b, int d, int f, int r)            { return pci_read(b << 16 | d << 11 | f << 8 | r); }
static void cfgw(int b, int d, int f, int r, uint32_t v)   { pci_write(b << 16 | d << 11 | f << 8 | r, v); }

/* Every bus, from bus 0 through the bridges (their secondary bus). */
int pci_find(uint32_t cls, uint32_t mask, hc_info_t *h, int nth)
{
    int bus[64] = { 0 }, nb = 1;
    for (int bi = 0; bi < nb; bi++)
        for (int d = 0; d < 32; d++)
            for (int f = 0; f < 8; f++) {
                int b = bus[bi];
                uint32_t id = cfg(b, d, f, 0);
                if (id == 0xFFFFFFFF) { if (!f) break; continue; }
                uint32_t hdr = cfg(b, d, f, 0xC) >> 16;
                if ((hdr & 0x7F) == 1 && nb < 64) bus[nb++] = cfg(b, d, f, 0x18) >> 8 & 0xFF;
                if ((cfg(b, d, f, 8) >> 8 & mask) == cls && !nth--) {
                    uint32_t bar = cfg(b, d, f, 0x10), hi = (bar & 6) == 4 ? cfg(b, d, f, 0x14) : 0;
                    cfgw(b, d, f, 4, cfg(b, d, f, 4) | 6);        /* memory, bus mastering on */
                    h->mmio = (bar & ~0xFu) | (uint64_t)hi << 32;
                    h->len = 0x10000;                             /* (xHCI: 64 KiB of registers at most) */
                    int line = cfg(b, d, f, 0x3C) & 0xFF;
                    h->irq = line && line < 0xFF ? line | IRQ_LEVEL : -1;
                    h->bus_off = 0;
                    sfmt(h->what, sizeof h->what, "%04x:%04x at %02x:%02x.%x", id & 0xFFFF, id >> 16, b, d, f);
                    return 0;
                }
                if (!f && !(hdr & 0x80)) break;                   /* not multi-function */
            }
    return -1;
}
int hc_find(hc_info_t *h, int nth)
{
    if (pci_find(0x0C0330, 0xFFFFFF, h, nth)) return -1;
    name_xhci(h);
    return 0;
}

#elif defined(__aarch64__)
#include "mk/fdt.h"
#include "mbox.h"

static char *fdt;
static int load(void)
{
    long n = sys_fdt(0, 0);
    if (n <= 0 || !(fdt = malloc(n)) || sys_fdt(fdt, n) != n) return -1;
    return 0;
}
static int cellsof(int node, const char *name, int dflt)
{
    int len;
    const uint8_t *p = fdt_prop(fdt, node, name, &len);
    return p && len == 4 ? (int)fdt_be32(p) : dflt;
}
static int by_phandle(uint32_t ph)
{
    int depth = 0, len;
    for (int n = fdt_next(fdt, -1, &depth); n >= 0; n = fdt_next(fdt, n, &depth)) {
        const uint8_t *p = fdt_prop(fdt, n, "phandle", &len);
        if (p && len == 4 && fdt_be32(p) == ph) return n;
    }
    return -1;
}

/* The bridge's 32-bit memory window ("ranges": PCI address, CPU address,
 * size; "dma-ranges": the same, the other way: where devices see our RAM). */
static int window_of(int node, const char *prop, uint64_t *pci, uint64_t *cpu, uint64_t *size)
{
    int len, pac = 2, sc = cellsof(node, "#size-cells", 2);
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
static int window(int node, uint64_t *pci, uint64_t *cpu, uint64_t *size) { return window_of(node, "ranges", pci, cpu, size); }

/* Which GIC interrupt a device's pin (1..4) uses: the bridge's interrupt-map. */
static int pin_irq(int node, uint32_t devfn_hi, int pin)
{
    int len, mlen;
    const uint8_t *m = fdt_prop(fdt, node, "interrupt-map", &len), *k = fdt_prop(fdt, node, "interrupt-map-mask", &mlen);
    uint32_t mask[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
    for (int i = 0; k && i < 4 && 4 * i + 4 <= mlen; i++) mask[i] = fdt_be32(k + 4 * i);
    for (int i = 0; m && i + 20 <= len; ) {
        uint32_t hi = fdt_be32(m + i), ip = fdt_be32(m + i + 12);
        int parent = by_phandle(fdt_be32(m + i + 16));
        if (parent < 0) return -1;
        int pac = cellsof(parent, "#address-cells", 0), pic = cellsof(parent, "#interrupt-cells", 3);
        const uint8_t *spec = m + i + 20 + 4 * pac;
        if ((hi & mask[0]) == (devfn_hi & mask[0]) && (ip & mask[3]) == ((uint32_t)pin & mask[3]) && pic == 3 &&
            fdt_be32(spec) == 0)
            return (int)fdt_be32(spec + 4) | (fdt_be32(spec + 8) & 0xC ? IRQ_LEVEL : 0);
        i += 20 + 4 * (pac + pic);
    }
    return -1;
}

/* A device's BAR 0 (memory, 32- or 64-bit) given an address in the window:
 * device d gets the window's d-th 16 MiB slot, so the servers that each
 * set up their own device (USB, the disk) never give two the same; one
 * already given (by the firmware, or by an earlier start of the server) is kept. */
static uint64_t give_bar(volatile uint32_t *c, int d, uint64_t wpci, uint64_t wcpu, uint64_t *len)
{
    int b64 = (c[4] & 6) == 4;
    uint64_t old = (c[4] & ~0xFu) | (b64 ? (uint64_t)c[5] << 32 : 0);
    uint32_t keep4 = c[4], keep5 = b64 ? c[5] : 0;
    c[4] = ~0u; if (b64) c[5] = ~0u;
    uint64_t sz = ~((b64 ? (uint64_t)c[5] << 32 : 0xFFFFFFFF00000000ull) | (c[4] & ~0xFu)) + 1;
    if (!sz || sz > (1u << 24)) sz = 0x10000;
    uint64_t at = old >= wpci ? old : (wpci + ((uint64_t)d << 24) + sz - 1) & ~(sz - 1);
    if (old >= wpci) { c[4] = keep4; if (b64) c[5] = keep5; }
    else { c[4] = (uint32_t)at | (c[4] & 0xF); if (b64) c[5] = (uint32_t)(at >> 32); }
    c[1] |= 6;                                     /* memory, bus mastering on */
    *len = sz;
    return at - wpci + wcpu;                       /* the CPU's address for it */
}

/* QEMU's virt: ECAM, bus 0. */
static int ecam(uint32_t cls, uint32_t mask, hc_info_t *h, int nth)
{
    int n = fdt_find(fdt, -1, "pci-host-ecam-generic");
    uint64_t base, size, wpci, wcpu, wsize;
    if (n < 0 || fdt_reg(fdt, n, 0, &base, &size) || window(n, &wpci, &wcpu, &wsize)) return -1;
    volatile uint8_t *e = map_phys(base, 1 << 20);
    if ((long)e < 0) return -1;
    for (int d = 0; d < 32; d++) {
        volatile uint32_t *c = (volatile uint32_t *)(e + (d << 15));
        if (c[0] == 0xFFFFFFFF) continue;
        if ((c[2] >> 8 & mask) == cls && !nth--) {
            h->mmio = give_bar(c, d, wpci, wcpu, &h->len);
            int pin = c[15] >> 8 & 0xFF;
            h->irq = pin ? pin_irq(n, (uint32_t)d << 11, pin) : -1;
            fdt_bus_addr(fdt, n, 0x40000000, (uint64_t *)&h->bus_off);
            h->bus_off -= 0x40000000;
            sfmt(h->what, sizeof h->what, "%04x:%04x on PCIe 00:%02x.0", c[0] & 0xFFFF, c[0] >> 16, d);
            return 0;
        }
    }
    return -1;
}

/* Raspberry Pi 4: BCM2711 PCIe (registers: root complex config at 0, the
 * other buses through EXT_CFG_INDEX 0x9000 / EXT_CFG_DATA 0x8000). The
 * firmware leaves the bridge held in reset, and reading its registers then
 * makes the bus fail (an SError, seen on a real Pi 4): we start it as
 * Linux's pcie-brcmstb does, then train the link to the VL805 USB chip. */
#define P_MISC_CTRL      0x4008          /* SCB access, UR mode, burst size, SCB0 size */
#define P_WIN0_LO        0x400C          /* outbound window 0: its PCI address, low and high */
#define P_WIN0_HI        0x4010
#define P_BAR1_LO        0x402C          /* inbound windows: BAR1 and BAR3 off, BAR2 our RAM */
#define P_BAR2_LO        0x4034
#define P_BAR2_HI        0x4038
#define P_BAR3_LO        0x403C
#define P_STATUS         0x4068          /* bit 4 PHY link up, 5 data link active, 7 root complex mode */
#define P_WIN0_BASELIM   0x4070          /* outbound window 0: CPU base and limit, MiB, low bits */
#define P_WIN0_BASE_HI   0x4080
#define P_WIN0_LIMIT_HI  0x4084
#define P_HARD_DEBUG     0x4204          /* bit 27 SerDes powered down, bit 1 CLKREQ# debug */
#define P_MSI_CLR        0x4508
#define P_MSI_MASK_SET   0x4510
#define P_ID_VAL3        0x043C          /* the root complex's class code */
#define P_VENDOR_REG1    0x0188          /* endianness of the inbound window */
#define P_SW_INIT        0x9210          /* bit 1 the bridge in reset, bit 0 PERST# asserted */

static int ilog2(uint64_t v) { int n = -1; while (v) { v >>= 1; n++; } return n; }

/* -> 0 the link is up (it already was, or we brought it up). */
static int pi4_start(volatile uint8_t *r, int n)
{
    #define R(o) (*(volatile uint32_t *)(r + (o)))
    if (!(R(P_SW_INIT) & 3) && (R(P_STATUS) & 0x30) == 0x30) return 0;   /* the firmware did it */
    uint64_t ipci, icpu, isize, opci, ocpu, osize;
    if (window_of(n, "dma-ranges", &ipci, &icpu, &isize) || window(n, &opci, &ocpu, &osize)) {
        log_line("usb: Pi 4 PCIe: no windows in the device tree\n");
        return -1;
    }
    R(P_SW_INIT) |= 3;                                     /* bridge and PERST# in reset */
    sys_sleep(200000);
    R(P_SW_INIT) &= ~2u;                                   /* the bridge out of reset */
    R(P_HARD_DEBUG) &= ~(1u << 27);                        /* SerDes on */
    sys_sleep(200000);
    uint32_t c = R(P_MISC_CTRL);
    c = (c & ~0x300000u) | 1u << 12 | 1u << 13;            /* SCB access, UR reads as ~0, 128-byte bursts */
    R(P_MISC_CTRL) = c;
    /* inbound (BAR2): our RAM as the devices see it, a power of two */
    uint64_t bsize = 1;
    while (bsize < isize) bsize <<= 1;
    int lg = ilog2(bsize);
    uint32_t enc = lg >= 12 && lg <= 15 ? (uint32_t)(lg - 12) + 0x1C : lg >= 16 && lg <= 35 ? (uint32_t)(lg - 15) : 0;
    R(P_BAR2_LO) = ((uint32_t)ipci & ~0x1Fu) | enc;        /* (at the window's PCI address) */
    R(P_BAR2_HI) = (uint32_t)(ipci >> 32);
    c = R(P_MISC_CTRL);
    R(P_MISC_CTRL) = (c & ~0xF8000000u) | (uint32_t)(lg - 15) << 27;   /* SCB0 size */
    R(P_BAR1_LO) &= ~0x1Fu;
    R(P_BAR3_LO) &= ~0x1Fu;
    R(P_MSI_MASK_SET) = ~0u;                               /* no MSI interrupts (INTx only) */
    R(P_MSI_CLR) = ~0u;
    R(P_SW_INIT) &= ~1u;                                   /* PERST# released: the link trains */
    for (int i = 0; i < 20 && (R(P_STATUS) & 0x30) != 0x30; i++) sys_sleep(5000000);
    uint32_t st = R(P_STATUS);
    if ((st & 0x30) != 0x30) { log_line("usb: Pi 4 PCIe link down (status %x): the USB chip is not reachable\n", st); return -1; }
    if (!(st & 0x80)) { log_line("usb: Pi 4 PCIe not in root complex mode\n"); return -1; }
    /* outbound window 0: CPU ocpu.. -> PCI opci.. */
    R(P_WIN0_LO) = (uint32_t)opci;
    R(P_WIN0_HI) = (uint32_t)(opci >> 32);
    uint64_t bmb = ocpu >> 20, lmb = (ocpu + osize - 1) >> 20;
    R(P_WIN0_BASELIM) = (R(P_WIN0_BASELIM) & ~0xFFF0FFF0u) | (uint32_t)(bmb & 0xFFF) << 4 | (uint32_t)(lmb & 0xFFF) << 20;
    R(P_WIN0_BASE_HI) = (R(P_WIN0_BASE_HI) & ~0xFFu) | (uint32_t)(bmb >> 12 & 0xFF);
    R(P_WIN0_LIMIT_HI) = (R(P_WIN0_LIMIT_HI) & ~0xFFu) | (uint32_t)(lmb >> 12 & 0xFF);
    R(P_ID_VAL3) = (R(P_ID_VAL3) & ~0xFFFFFFu) | 0x060400; /* a PCI-to-PCI bridge */
    R(P_VENDOR_REG1) &= ~0xCu;                             /* little-endian inbound */
    R(P_HARD_DEBUG) |= 2;                                  /* refclk gated by CLKREQ# */
    log_line("usb: Pi 4 PCIe link up (status %x)\n", st);
    return 0;
    #undef R
}

static int pi4(hc_info_t *h, int nth)
{
    int n = fdt_find(fdt, -1, "brcm,bcm2711-pcie");
    uint64_t base, size, wpci, wcpu, wsize;
    if (nth || n < 0 || fdt_reg(fdt, n, 0, &base, &size) || window(n, &wpci, &wcpu, &wsize)) return -1;
    volatile uint8_t *r = map_phys(base, 0x9310);
    if ((long)r < 0) return -1;
    if (pi4_start(r, n)) return -1;
    volatile uint32_t *rc = (volatile uint32_t *)r;               /* the root complex (bus 0) */
    rc[6] = 0x00010100;                                           /* buses: primary 0, secondary 1, subordinate 1 */
    rc[8] = (uint32_t)((wpci >> 16 & 0xFFF0) | ((wpci + wsize - 1) & 0xFFF00000));   /* memory base/limit */
    rc[1] |= 6;
    *(volatile uint32_t *)(r + 0x9000) = 1 << 20;                 /* bus 1, device 0 */
    volatile uint32_t *c = (volatile uint32_t *)(r + 0x8000);
    if (c[2] >> 8 != 0x0C0330) return -1;
    h->mmio = give_bar(c, 0, wpci, wcpu, &h->len);
    uint32_t mb[7] __attribute__((aligned(16))) = { 28, 0, 0x00030058, 4, 4, 1 << 20, 0 };   /* notify xHCI reset */
    mbox_call(mb, 7);
    sys_sleep(100000000);
    int pin = c[15] >> 8 & 0xFF;
    h->irq = pin ? pin_irq(n, 1u << 16, pin) : -1;
    uint64_t bus;
    h->bus_off = fdt_bus_addr(fdt, n, 0, &bus) ? 0 : (int64_t)bus;
    h->low = 1;
    sfmt(h->what, sizeof h->what, "VL805 xHCI (Raspberry Pi 4)");
    return 0;
}

/* Raspberry Pi 5: RP1's two DWC3 controllers ("snps,dwc3" nodes; RP1's
 * addresses 0xC0_4000_0000... appear at 0x1F_0000_0000 through the PCIe
 * window the firmware set up; RP1 sees our RAM at 0x10_0000_0000). Polled
 * (RP1's interrupts go through MSI-X, not supported yet). UNTESTED. */
static int pi5(hc_info_t *h, int nth)
{
    if (fdt_find(fdt, -1, "brcm,bcm2712-pcie") < 0) return -1;
    for (int n = -1; (n = fdt_find(fdt, n, "snps,dwc3")) >= 0; ) {
        uint64_t a, len;
        if (fdt_reg(fdt, n, 0, &a, &len) || nth--) continue;
        h->mmio = a >= 0xC040000000ULL ? a - 0xC040000000ULL + 0x1F00000000ULL : a;
        h->len = len ? len : 0x100000;
        h->irq = -1;
        h->dwc3 = 1;
        h->low = 1;
        uint64_t bus;
        h->bus_off = !fdt_bus_addr(fdt, n, 0, &bus) && bus ? (int64_t)bus : 0x1000000000LL;
        sfmt(h->what, sizeof h->what, "RP1 DWC3 xHCI %d (Raspberry Pi 5)", 0);
        return 0;
    }
    return -1;
}

int hc_find(hc_info_t *h, int nth)
{
    memset(h, 0, sizeof *h);
    if (!fdt && load()) return -1;
    if (!ecam(0x0C0330, 0xFFFFFF, h, nth)) { name_xhci(h); return 0; }
    return pi4(h, nth) && pi5(h, nth) ? -1 : 0;
}
int pci_find(uint32_t cls, uint32_t mask, hc_info_t *h, int nth)
{
    memset(h, 0, sizeof *h);
    if (!fdt && load()) return -1;
    return ecam(cls, mask, h, nth);
}
#endif
