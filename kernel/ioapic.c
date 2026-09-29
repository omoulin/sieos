/*
 * ioapic.c - Device interrupts through the I/O APIC.
 *
 * With an I/O APIC in the ACPI MADT, the 8259 PICs are masked and every
 * registered interrupt line (ISA IRQs, and PCI lines, which PC firmware
 * routes to the same numbers) is programmed in the I/O APIC: the pin is the
 * line's global system interrupt (an interrupt source override may move it,
 * as IRQ 0 goes to pin 2), polarity and trigger mode come from the override
 * (ISA defaults: edge, active high), the vector stays IRQ_BASE + line, and
 * delivery is fixed to one CPU.  Level-triggered lines are acknowledged at
 * the local APIC after their handler has quietened the device (trap.c).
 * Without an I/O APIC the PICs stay in charge.
 */
#include "kernel.h"
#include "arch.h"
#include "smp.h"
#include "mm.h"

#define IOREGSEL 0x00
#define IOWIN    0x10
#define IOAPICVER 0x01
#define IOREDTBL(n) (0x10 + 2 * (n))

#define RTE_MASKED   (1U << 16)
#define RTE_LEVEL    (1U << 15)
#define RTE_LOWACT   (1U << 13)

bool ioapic_ok;

static struct {
    volatile uint32_t *regs;
    uint32_t gsi_base, npins;
} ioapics[MADT_MAX_IOAPIC];
static int nio;

static uint32_t io_read(int i, uint32_t reg)
{
    ioapics[i].regs[IOREGSEL / 4] = reg;
    return ioapics[i].regs[IOWIN / 4];
}

static void io_write(int i, uint32_t reg, uint32_t v)
{
    ioapics[i].regs[IOREGSEL / 4] = reg;
    ioapics[i].regs[IOWIN / 4] = v;
}

/* The I/O APIC and pin of a global system interrupt; false if none has it. */
static bool pin_of(uint32_t gsi, int *io, uint32_t *pin)
{
    for (int i = 0; i < nio; i++)
        if (gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + ioapics[i].npins) {
            *io = i;
            *pin = gsi - ioapics[i].gsi_base;
            return true;
        }
    return false;
}

/* Line irq: its GSI and redirection flags (from an override, else ISA's edge, active high). */
static uint32_t line_gsi(int irq, uint32_t *flags)
{
    *flags = 0;
    for (int i = 0; i < madt_niso; i++)
        if (madt_isos[i].irq == irq) {
            uint16_t f = madt_isos[i].flags;
            if ((f & 3) == 3)
                *flags |= RTE_LOWACT;
            if (((f >> 2) & 3) == 3)
                *flags |= RTE_LEVEL;
            return madt_isos[i].gsi;
        }
    return irq;
}

bool ioapic_is_level(int irq)
{
    uint32_t flags;
    line_gsi(irq, &flags);
    return ioapic_ok && (flags & RTE_LEVEL);
}

void ioapic_route(int irq, int dest_apic)
{
    uint32_t flags, pin;
    int io;
    uint32_t gsi = line_gsi(irq, &flags);
    if (!pin_of(gsi, &io, &pin))
        return;
    io_write(io, IOREDTBL(pin) + 1, (uint32_t)dest_apic << 24);
    io_write(io, IOREDTBL(pin), (IRQ_BASE + irq) | flags);      /* fixed delivery, physical destination */
}

void ioapic_mask(int irq)
{
    uint32_t flags, pin;
    int io;
    if (pin_of(line_gsi(irq, &flags), &io, &pin))
        io_write(io, IOREDTBL(pin), io_read(io, IOREDTBL(pin)) | RTE_MASKED);
}

bool ioapic_init(void)
{
    if (!lapic_ok || !madt_nioapic)
        return false;
    for (int i = 0; i < madt_nioapic; i++) {
        uint64_t pa = madt_ioapics[i].addr;
        if (!pa || pa >= DIRECT_MAP_SIZE)
            continue;
        vmm_set_uncached(pa);
        ioapics[nio].regs = P2V(pa);
        ioapics[nio].gsi_base = madt_ioapics[i].gsi_base;
        ioapics[nio].npins = ((io_read(nio, IOAPICVER) >> 16) & 0xFF) + 1;
        for (uint32_t pin = 0; pin < ioapics[nio].npins; pin++)
            io_write(nio, IOREDTBL(pin), RTE_MASKED);
        nio++;
    }
    if (!nio)
        return false;
    outb(0x21, 0xFF);                                /* the PICs: everything masked */
    outb(0xA1, 0xFF);
    ioapic_ok = true;
    return true;
}
