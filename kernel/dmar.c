/*
 * dmar.c - Intel VT-d left on by the firmware: turned off.
 *
 * SIEOS has no IOMMU driver: devices DMA to physical addresses.  Some
 * firmware (the Surface Pro 7's, with pre-boot DMA protection for Windows'
 * Kernel DMA Protection) leaves a remapping unit translating, or its
 * protected memory regions on, after it hands over; the devices behind
 * that unit then cannot reach memory (their commands never arrive, their
 * answers are never written).  At boot every unit of the ACPI DMAR table is
 * checked, and translation (GCMD.TE) and the protected memory regions
 * (PMEN.EPM) are turned off where they are on (Intel's VT-d specification:
 * the Global Command and Protected Memory Enable registers), as an
 * operating system that does not use the IOMMU has to.  Interrupt
 * remapping is only reported.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "mm.h"
#include "smp.h"

#define VTD_GCMD  0x18
#define VTD_GSTS  0x1C
#define VTD_PMEN  0x64
#define GSTS_TES  (1U << 31)
#define GSTS_IRES (1U << 25)
#define PMEN_EPM  (1U << 31)
#define PMEN_PRS  (1U << 0)

static bool wait_clear(volatile uint32_t *reg, uint32_t bit)
{
    uint64_t end = hrtime() + 100000000UL;       /* 100 ms */
    while (*reg & bit)
        if (hrtime() > end)
            return false;
    return true;
}

void dmar_init(void)
{
    uint32_t len;
    const uint8_t *t = acpi_table("DMAR", 0, &len);
    if (!t || len < 48)
        return;
    for (uint32_t off = 48; off + 16 <= len;) {
        uint16_t type = *(const uint16_t *)(t + off), slen = *(const uint16_t *)(t + off + 2);
        if (slen < 4 || off + slen > len)
            break;
        if (type == 0 && slen >= 16) {           /* DRHD: a remapping unit */
            uint64_t base = *(const uint64_t *)(t + off + 8);
            volatile uint8_t *r = base ? mmio_map(base, 0x1000) : NULL;
            if (r) {
                volatile uint32_t *gsts = (volatile uint32_t *)(r + VTD_GSTS);
                volatile uint32_t *gcmd = (volatile uint32_t *)(r + VTD_GCMD);
                volatile uint32_t *pmen = (volatile uint32_t *)(r + VTD_PMEN);
                uint32_t st = *gsts, pm = *pmen;
                if (st != 0xFFFFFFFF && (st & GSTS_TES)) {
                    *gcmd = (st & 0x96FFFFFF) & ~GSTS_TES;    /* (the one-shot bits masked) */
                    kprintf("dmar: unit at %#lx: the firmware left DMA translation on: %s\n", base,
                            wait_clear(gsts, GSTS_TES) ? "turned off" : "it did not turn off");
                }
                if (pm != 0xFFFFFFFF && (pm & PMEN_EPM)) {
                    *pmen = 0;
                    kprintf("dmar: unit at %#lx: the firmware left its protected memory regions on: %s\n", base,
                            wait_clear(pmen, PMEN_PRS) ? "turned off" : "they did not turn off");
                }
                if (st != 0xFFFFFFFF && (st & GSTS_IRES))
                    kprintf("dmar: unit at %#lx: interrupt remapping is on (left as it is)\n", base);
            }
        }
        off += slen;
    }
}
