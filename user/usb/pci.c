/*
 * pci.c - Finding the xHCI controller (PCI class 0C/03/30) and making it
 * usable: its registers mapped at an address, memory and DMA ("bus
 * master") on, its interrupt line known. pci_find does the same for any
 * class (the disk server uses it for an SD controller, 08/05, on QEMU's
 * virt: user/vblk/sd.c).
 *   x86-64 and QEMU virt (arm64): any PCI device, user/lib/pci.c.
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
#include "pci.h"

static void name_xhci(hc_info_t *h) { char t[40]; strlcpy(t, h->what, sizeof t); sfmt(h->what, sizeof h->what, "xHCI %s", t); }

/* An ordinary PCI device (user/lib/pci.c): BAR 0 and its line. */
int pci_find(uint32_t cls, uint32_t mask, hc_info_t *h, int nth)
{
    pci_t p;
    memset(h, 0, sizeof *h);
    if (pci_scan(&p, 8, cls << 8, mask << 8, nth) || !(h->mmio = pci_bar(&p, 0, &h->len))) return -1;
    h->irq = p.irq;
    h->bus_off = p.bus_off;
    sfmt(h->what, sizeof h->what, "%04x:%04x at %02x:%02x.%x", p.id & 0xFFFF, p.id >> 16, p.at >> 16, p.at >> 11 & 31, p.at >> 8 & 7);
    return 0;
}

#if defined(__x86_64__)
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
static int window(int node, uint64_t *pci, uint64_t *cpu, uint64_t *size) { return pci_window(fdt, node, "ranges", pci, cpu, size); }

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
    if (pci_window(fdt, n, "dma-ranges", &ipci, &icpu, &isize) || window(n, &opci, &ocpu, &osize)) {
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
    pci_t p = { .cfg = c, .wpci = wpci, .wcpu = wcpu };
    h->mmio = pci_bar(&p, 0, &h->len);
    uint32_t mb[7] __attribute__((aligned(16))) = { 28, 0, 0x00030058, 4, 4, 1 << 20, 0 };   /* notify xHCI reset */
    mbox_call(mb, 7);
    sys_sleep(100000000);
    int pin = c[15] >> 8 & 0xFF;
    h->irq = pin ? pci_pin_irq(fdt, n, 1u << 16, pin) : -1;
    /* Where the VL805 sees our RAM: the buses above the bridge, then the
     * bridge's own "dma-ranges" (the firmware may put RAM at PCI
     * 0x4_0000_0000 rather than 0, as on an 8 GB Pi 4). */
    uint64_t bus, ipci, icpu, isize;
    h->bus_off = fdt_bus_addr(fdt, n, 0, &bus) ? 0 : (int64_t)bus;
    if (!pci_window(fdt, n, "dma-ranges", &ipci, &icpu, &isize)) h->bus_off += (int64_t)(ipci - icpu);
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
    if (!pci_find(0x0C0330, 0xFFFFFF, h, nth)) { name_xhci(h); return 0; }
    memset(h, 0, sizeof *h);
    if (!fdt && load()) return -1;
    return pi4(h, nth) && pi5(h, nth) ? -1 : 0;
}
#endif
