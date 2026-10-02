/*
 * power.c - Power management: power-off and restart (ACPI), the power
 * button, processor idle states and frequencies, temperatures, the thermal
 * policy and the fans that can be seen.
 *
 * ACPI without AML: the FADT's fixed hardware is enough for most of it.
 *   - Power-off (S5): SLP_TYPa/b from the DSDT's \_S5 package (read, not
 *     run), written with SLP_EN into PM1a/PM1b_CNT (or the hardware-reduced
 *     SLEEP_CONTROL_REG); ACPI mode is entered first if the firmware left
 *     the machine in legacy mode.
 *   - Restart: the FADT's reset register, else port 0xCF9, else the
 *     i8042, else a triple fault.
 *   - ACPI mode is entered at boot (SMI_CMD), as every ACPI system does;
 *     the fixed power button (PWRBTN_STS) is then polled: a press sends
 *     SIGPWR to init, which shuts down.  (The SCI itself is not used.)
 *   Thermal zones, fan devices and batteries are AML methods: not here.
 *
 * Processors (Intel; AMD: temperatures only):
 *   - Idle: MWAIT into the deepest C-state CPUID 5 lists, when the local
 *     APIC timer keeps running there (ARAT: the scheduler's tick); HLT
 *     otherwise, and with the performance policy.
 *   - Frequency: hardware P-states (HWP) with an energy/performance
 *     preference (EPP) per policy; else legacy P-states (IA32_PERF_CTL).
 *   - Temperatures: the digital thermal sensors (IA32_THERM_STATUS per
 *     core, IA32_PACKAGE_THERM_STATUS), below TjMax; AMD family 17h+ from
 *     the SMN (Tctl).  The actual frequency from APERF/MPERF.
 *   Per-processor registers are read and written by each processor on its
 *   own tick (power_cpu_tick, once a second); the boot processor runs the
 *   policy.
 *
 * The thermal policy (passive cooling): at or above the passive threshold
 * (default TjMax - 10) the highest performance allowed is lowered a tenth
 * of the range each second; 5 degrees below it, raised again.  At the
 * critical threshold (default TjMax + 5, which the processor's own
 * throttling at TjMax should never let it reach) for 3 seconds, init is
 * asked to shut down.
 *
 * Fans: a laptop's or tablet's (ACPI profile mobile, tablet...) are run by
 * its embedded controller, which ACPI reaches only through AML: they are
 * reported as the firmware's.  On desktops the Super I/O hardware monitor
 * is read, read-only: Nuvoton NCT6779D and later (the RPM registers), ITE
 * IT87xx (the 16-bit counters).  "nosuperio" on the command line skips it.
 * "thermaltest" makes the temperature up (70 C, rising 2 C a second) to
 * see the thermal policy work: passive cooling, then the shutdown.
 * Written from the vendors' register descriptions; not tested on hardware.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "proc.h"
#include "smp.h"
#include "mm.h"
#include "pci.h"
#include "ksig.h"
#include "poll.h"
#include "sieos/power.h"
#include "power.h"
#include "hid.h"                                 /* (boot_option) */

#define MSR_MPERF              0xE7
#define MSR_APERF              0xE8
#define MSR_PLATFORM_INFO      0xCE
#define MSR_PERF_CTL           0x199
#define MSR_THERM_STATUS       0x19C
#define MSR_TEMPERATURE_TARGET 0x1A2
#define MSR_TURBO_RATIO_LIMIT  0x1AD
#define MSR_PKG_THERM_STATUS   0x1B1
#define MSR_PM_ENABLE          0x770
#define MSR_HWP_CAPABILITIES   0x771
#define MSR_HWP_REQUEST        0x774

#define POLL_TICKS TIMER_HZ                      /* per-processor work: once a second */

/* ---------------------------------------------------------------- safe MSR access */

static volatile bool probing[NCPU], faulted[NCPU];

/* From the #GP handler (kernel mode): an MSR access being probed is skipped. */
bool msr_fixup(struct trapframe *tf)
{
    int id = mycpu()->id;
    if (id < 0 || id >= NCPU || !probing[id])
        return false;
    const uint8_t *ip = (const uint8_t *)tf->rip;
    if (ip[0] != 0x0F || (ip[1] != 0x32 && ip[1] != 0x30))
        return false;
    tf->rip += 2;                                /* rdmsr, wrmsr: 2 bytes */
    faulted[id] = true;
    return true;
}

static bool rdmsr_safe(uint32_t msr, uint64_t *v)
{
    int id = mycpu()->id;
    uint32_t lo = 0, hi = 0;
    faulted[id] = false;
    probing[id] = true;
    __asm__ volatile("rdmsr" : "+a"(lo), "+d"(hi) : "c"(msr) : "memory");
    probing[id] = false;
    if (faulted[id])
        return false;
    *v = (uint64_t)hi << 32 | lo;
    return true;
}

static bool wrmsr_safe(uint32_t msr, uint64_t v)
{
    int id = mycpu()->id;
    faulted[id] = false;
    probing[id] = true;
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
    probing[id] = false;
    return !faulted[id];
}

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

/* ---------------------------------------------------------------- state */

static struct {
    unsigned flags;
    int policy;
    bool intel, amd;
    int amd_family;
    /* idle */
    bool mwait;
    uint32_t mwait_hint;
    char idle_name[32];
    /* frequency */
    int perf_lowest, perf_guaranteed, perf_highest, perf_efficient;
    int perf_max;                                /* allowed now (the thermal policy) */
    volatile uint32_t gen;                       /* the settings' generation: processors apply a newer one */
    /* thermal */
    int tjmax, passive, critical, passive_set, critical_set;
    int temp, level, hot_seconds;                /* level: tenths of the range held back */
    /* ACPI */
    uint16_t pm1a_evt, pm1b_evt, pm1a_cnt, pm1b_cnt, smi_cmd;
    uint8_t acpi_enable, pm_profile;
    bool s5, hw_reduced, pwrbtn_fixed;
    uint8_t slp_typa, slp_typb;
    uint8_t reset_space, reset_value;
    uint64_t reset_addr, sleep_ctl;
    uint8_t sleep_ctl_space;
    uint64_t button_quiet;
    /* fans */
    char fans_name[48];
    int sio_kind;                                /* 0 none, 1 Nuvoton, 2 ITE */
    uint16_t sio_base;
    int nfans;
    struct sieos_power_fan fan[SIEOS_POWER_MAXFAN];
    volatile bool ready;                         /* power_init is done: the ticks may work */
    int fake;                                    /* "thermaltest": a made-up temperature (seconds so far) */
} pw = { .policy = SIEOS_POWER_BALANCED };

static struct pcpu {
    uint64_t next;                               /* the tick of the next work */
    uint32_t gen;
    bool hwp_on;
    int temp;
    unsigned mhz;
    uint64_t aperf, mperf;
} pcpu[NCPU];

/* ---------------------------------------------------------------- ACPI fixed hardware */

struct gas { uint8_t space, width, offset, access; uint64_t addr; } __attribute__((packed));

/* \_S5's two values from an AML table (a Name with a Package: read, not run). */
static bool find_s5(const uint8_t *aml, uint32_t len)
{
    for (uint32_t i = 1; i + 12 < len; i++) {
        if (memcmp(aml + i, "_S5_", 4) || aml[i + 4] != 0x12)
            continue;
        if (aml[i - 1] != 0x08 && !(i >= 2 && aml[i - 2] == 0x08 && aml[i - 1] == '\\'))
            continue;                            /* Name (_S5_, Package ...) */
        const uint8_t *p = aml + i + 5;
        p += ((p[0] >> 6) & 3) + 1;              /* PkgLength */
        p++;                                     /* NumElements */
        uint8_t v[2];
        for (int k = 0; k < 2; k++) {
            if (*p == 0x0A)                      /* BytePrefix */
                p++;
            v[k] = *p == 0xFF ? 0 : *p;          /* (Zero 0x00, One 0x01, a byte) */
            p++;
        }
        pw.slp_typa = v[0] & 7;
        pw.slp_typb = v[1] & 7;
        return true;
    }
    return false;
}

static void acpi_mode_on(void)
{
    if (!pw.pm1a_cnt || (inw(pw.pm1a_cnt) & 1) || !pw.smi_cmd || !pw.acpi_enable)
        return;
    outb(pw.smi_cmd, pw.acpi_enable);
    for (int i = 0; i < 300 && !(inw(pw.pm1a_cnt) & 1); i++)
        for (int k = 0; k < 10000; k++)
            io_wait();                           /* (about 10 ms) */
}

static void acpi_power_init(void)
{
    uint32_t len;
    const uint8_t *f = acpi_table("FACP", 0, &len);
    if (!f || len < 116)
        return;
    uint32_t flags = *(uint32_t *)(f + 112);
    pw.pm_profile = f[45];
    pw.smi_cmd = *(uint32_t *)(f + 48);
    pw.acpi_enable = f[52];
    pw.pm1a_evt = *(uint32_t *)(f + 56);
    pw.pm1b_evt = *(uint32_t *)(f + 60);
    pw.pm1a_cnt = *(uint32_t *)(f + 64);
    pw.pm1b_cnt = *(uint32_t *)(f + 68);
    uint8_t evt_len = f[88];
    pw.hw_reduced = flags & (1U << 20);
    if (len >= 196) {                            /* the 64-bit blocks, when in I/O space */
        const struct gas *x = (const struct gas *)(f + 148);
        if (x[0].space == 1 && x[0].addr) pw.pm1a_evt = x[0].addr;
        if (x[1].space == 1 && x[1].addr) pw.pm1b_evt = x[1].addr;
        if (x[2].space == 1 && x[2].addr) pw.pm1a_cnt = x[2].addr;
        if (x[3].space == 1 && x[3].addr) pw.pm1b_cnt = x[3].addr;
    }
    if ((flags & (1U << 10)) && len >= 129) {    /* RESET_REG_SUP */
        const struct gas *r = (const struct gas *)(f + 116);
        pw.reset_space = r->space;
        pw.reset_addr = r->addr;
        pw.reset_value = f[128];
        if (pw.reset_addr && pw.reset_space <= 2)
            pw.flags |= SIEOS_PWR_ACPI_RESET;
    }
    if (pw.hw_reduced && len >= 256) {
        const struct gas *sc = (const struct gas *)(f + 244);
        pw.sleep_ctl = sc->addr;
        pw.sleep_ctl_space = sc->space;
    }
    const uint8_t *t = acpi_table("DSDT", 0, &len);
    pw.s5 = t && find_s5(t + 36, len - 36);
    for (int k = 0; !pw.s5 && (t = acpi_table("SSDT", k, &len)); k++)
        pw.s5 = find_s5(t + 36, len - 36);
    if (pw.s5 && (pw.pm1a_cnt || pw.sleep_ctl))
        pw.flags |= SIEOS_PWR_ACPI_OFF;
    /* ACPI mode (as every ACPI system does at boot), then the fixed power button is polled */
    if (!pw.hw_reduced)
        acpi_mode_on();
    pw.pwrbtn_fixed = !(flags & (1U << 4)) && pw.pm1a_evt && evt_len >= 4 && !pw.hw_reduced &&
                      pw.pm1a_cnt && (inw(pw.pm1a_cnt) & 1);
    if (pw.pwrbtn_fixed) {
        outw(pw.pm1a_evt, 1U << 8);              /* (clear a press from before) */
        uint16_t en = pw.pm1a_evt + evt_len / 2;  /* PWRBTN_EN: some chipsets (QEMU's) latch the status only then; */
        outw(en, inw(en) | 1U << 8);             /* the SCI it raises stays masked (no handler) */
        pw.flags |= SIEOS_PWR_PWRBTN;
    }
    if (pw.pm_profile == 2 || pw.pm_profile == 8 || pw.pm_profile == 6)   /* mobile, tablet, appliance */
        pw.flags |= SIEOS_PWR_MOBILE;
}

static void gas_write8(uint8_t space, uint64_t addr, uint8_t v)
{
    if (space == 1) {
        outb(addr, v);
    } else if (space == 0) {
        volatile uint8_t *m = mmio_map(addr, 1);
        if (m)
            *m = v;
    } else if (space == 2) {                     /* PCI configuration: dev 31-16, func 15-0? (ACPI: dev, func, offset) */
        uint8_t dev = (addr >> 32) & 0x1F, func = (addr >> 16) & 7, off = addr & 0xFF;
        uint32_t old = pci_read32(0, dev, func, off & 0xFC);
        int sh = (off & 3) * 8;
        pci_write32(0, dev, func, off & 0xFC, (old & ~(0xFFU << sh)) | (uint32_t)v << sh);
    }
}

void power_off(void)
{
    if (pw.flags & SIEOS_PWR_ACPI_OFF) {
        cli();
        if (pw.hw_reduced && pw.sleep_ctl) {
            gas_write8(pw.sleep_ctl_space, pw.sleep_ctl, (pw.slp_typa & 7) << 2 | 1U << 5);
        } else {
            acpi_mode_on();
            uint16_t a = inw(pw.pm1a_cnt) & ~(7U << 10);
            outw(pw.pm1a_cnt, a | (uint16_t)pw.slp_typa << 10 | 1U << 13);
            if (pw.pm1b_cnt) {
                uint16_t b = inw(pw.pm1b_cnt) & ~(7U << 10);
                outw(pw.pm1b_cnt, b | (uint16_t)pw.slp_typb << 10 | 1U << 13);
            }
        }
        for (int k = 0; k < 1000000; k++)
            io_wait();                           /* (about a second) */
        kprintf("power: ACPI power-off did not take\n");
    }
    outw(0x604, 0x2000);                         /* QEMU / Bochs */
    outw(0xB004, 0x2000);
}

void power_reset(void)
{
    cli();
    if (pw.flags & SIEOS_PWR_ACPI_RESET) {
        gas_write8(pw.reset_space, pw.reset_addr, pw.reset_value);
        for (int k = 0; k < 100000; k++)
            io_wait();
    }
    outb(0xCF9, 0x02);                           /* the chipset's reset control: system reset */
    io_wait();
    outb(0xCF9, 0x06);
    for (int k = 0; k < 100000; k++)
        io_wait();
    outb(0xCF9, 0x0E);                           /* full reset (power cycle) */
    for (int k = 0; k < 100000; k++)
        io_wait();
    if (inb(0x64) != 0xFF) {                     /* an i8042: its reset line */
        for (int i = 0; i < 100000 && (inb(0x64) & 2); i++)
            ;
        outb(0x64, 0xFE);
        for (int k = 0; k < 100000; k++)
            io_wait();
    }
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) none = { 0, 0 };
    __asm__ volatile("lidt %0; int3" :: "m"(none));   /* a triple fault */
}

/* ---------------------------------------------------------------- Super I/O fans (desktops) */

static uint8_t sio_read(uint16_t port, uint8_t reg)
{
    outb(port, reg);
    return inb(port + 1);
}

static uint8_t nct_read(uint16_t reg)
{
    outb(pw.sio_base + 5, 0x4E);                 /* bank */
    outb(pw.sio_base + 6, reg >> 8);
    outb(pw.sio_base + 5, reg & 0xFF);
    return inb(pw.sio_base + 6);
}

static uint8_t ite_read(uint8_t reg)
{
    outb(pw.sio_base + 5, reg);
    return inb(pw.sio_base + 6);
}

static void fans_read(void)
{
    pw.nfans = 0;
    if (pw.sio_kind == 1) {
        static const uint16_t regs[] = { 0x4C0, 0x4C2, 0x4C4, 0x4C6, 0x4C8, 0x4CA, 0x4CE };
        for (size_t i = 0; i < ARRAY_SIZE(regs) && pw.nfans < SIEOS_POWER_MAXFAN; i++) {
            unsigned rpm = (unsigned)nct_read(regs[i]) << 8 | nct_read(regs[i] + 1);
            if (rpm && rpm < 20000) {
                pw.fan[pw.nfans].rpm = rpm;
                snprintf(pw.fan[pw.nfans++].name, sizeof(pw.fan[0].name), "fan%zu", i + 1);
            }
        }
    } else if (pw.sio_kind == 2) {
        static const uint8_t lo[] = { 0x0D, 0x0E, 0x0F, 0x80, 0x82 }, hi[] = { 0x18, 0x19, 0x1A, 0x81, 0x83 };
        for (size_t i = 0; i < ARRAY_SIZE(lo) && pw.nfans < SIEOS_POWER_MAXFAN; i++) {
            unsigned count = ite_read(lo[i]) | (unsigned)ite_read(hi[i]) << 8;
            if (count && count != 0xFFFF) {
                pw.fan[pw.nfans].rpm = 1350000 / (count * 2);
                snprintf(pw.fan[pw.nfans++].name, sizeof(pw.fan[0].name), "fan%zu", i + 1);
            }
        }
    }
}

static void superio_probe(void)
{
    static const uint16_t ports[] = { 0x2E, 0x4E };
    for (size_t i = 0; i < ARRAY_SIZE(ports) && !pw.sio_kind; i++) {
        uint16_t p = ports[i];
        outb(p, 0x87);                           /* Nuvoton: enter */
        outb(p, 0x87);
        uint16_t id = (uint16_t)sio_read(p, 0x20) << 8 | sio_read(p, 0x21);
        static const struct { uint16_t id; const char *name; } nct[] = {
            { 0xC560, "NCT6779D" }, { 0xC800, "NCT6791D" }, { 0xC910, "NCT6792D" }, { 0xD120, "NCT6793D" },
            { 0xD350, "NCT6795D" }, { 0xD420, "NCT6796D" }, { 0xD428, "NCT6798D" }, { 0xD450, "NCT6797D" },
            { 0xD800, "NCT6799D" },
        };
        for (size_t k = 0; k < ARRAY_SIZE(nct); k++)
            if ((id & 0xFFF8) == nct[k].id) {
                outb(p, 0x07);                   /* the hardware monitor's logical device */
                outb(p + 1, 0x0B);
                uint16_t base = ((uint16_t)sio_read(p, 0x60) << 8 | sio_read(p, 0x61)) & ~7;
                if ((sio_read(p, 0x30) & 1) && base >= 0x100) {
                    pw.sio_kind = 1;
                    pw.sio_base = base;
                    snprintf(pw.fans_name, sizeof(pw.fans_name), "Nuvoton %s (read-only)", nct[k].name);
                }
            }
        outb(p, 0xAA);                           /* exit */
        if (pw.sio_kind)
            break;
        static const uint8_t ite_key[2][4] = { { 0x87, 0x01, 0x55, 0x55 }, { 0x87, 0x01, 0x55, 0xAA } };
        for (int k = 0; k < 4; k++)
            outb(p, ite_key[i][k]);
        id = (uint16_t)sio_read(p, 0x20) << 8 | sio_read(p, 0x21);
        if ((id & 0xF000) == 0x8000 && id != 0xFFFF) {
            outb(p, 0x07);
            outb(p + 1, 0x04);                   /* the environment controller */
            uint16_t base = ((uint16_t)sio_read(p, 0x60) << 8 | sio_read(p, 0x61)) & ~7;
            if ((sio_read(p, 0x30) & 1) && base >= 0x100) {
                pw.sio_kind = 2;
                pw.sio_base = base;
                snprintf(pw.fans_name, sizeof(pw.fans_name), "ITE IT%04X (read-only)", id);
            }
        }
        outb(p, 0x02);                           /* exit */
        outb(p + 1, 0x02);
    }
    if (pw.sio_kind)
        fans_read();
}

/* ---------------------------------------------------------------- processors */

static void cpu_detect(void)
{
    uint32_t a, b, c, d, max;
    char vendor[13];
    cpuid(0, 0, &max, &b, &c, &d);
    memcpy(vendor, &b, 4), memcpy(vendor + 4, &d, 4), memcpy(vendor + 8, &c, 4), vendor[12] = 0;
    pw.intel = !strcmp(vendor, "GenuineIntel");
    pw.amd = !strcmp(vendor, "AuthenticAMD");
    cpuid(1, 0, &a, &b, &c, &d);
    int family = (a >> 8) & 0xF;
    if (family == 0xF)
        family += (a >> 20) & 0xFF;
    pw.amd_family = pw.amd ? family : 0;
    bool eist = c & (1U << 7), has_mwait = c & (1U << 3);
    uint32_t pm_a = 0, pm_c = 0;
    if (max >= 6)
        cpuid(6, 0, &pm_a, &b, &pm_c, &d);
    bool arat = pm_a & (1U << 2);
    strlcpy(pw.idle_name, "HLT", sizeof(pw.idle_name));
    if (pw.intel && has_mwait && arat && max >= 5) {
        uint32_t ea, eb, ec, ed;
        cpuid(5, 0, &ea, &eb, &ec, &ed);
        for (int i = 7; i >= 2; i--)             /* the deepest C-state with a sub-state */
            if ((ed >> (4 * i)) & 0xF) {
                pw.mwait = true;
                pw.mwait_hint = (uint32_t)(i - 1) << 4;
                snprintf(pw.idle_name, sizeof(pw.idle_name), "MWAIT C%d (hint %#x)", i, pw.mwait_hint);
                break;
            }
    }
    if (pw.mwait)
        pw.flags |= SIEOS_PWR_MWAIT;
    if (pm_c & 1)
        pw.flags |= SIEOS_PWR_APERF;
    pw.tjmax = 100;
    if (pw.intel) {
        uint64_t v;
        if (pm_a & 1)
            pw.flags |= SIEOS_PWR_DTS;
        if (pm_a & (1U << 6))
            pw.flags |= SIEOS_PWR_PKGTEMP;
        if ((pw.flags & SIEOS_PWR_DTS) && rdmsr_safe(MSR_TEMPERATURE_TARGET, &v) && ((v >> 16) & 0xFF) >= 50)
            pw.tjmax = (v >> 16) & 0xFF;
        if ((pm_a & (1U << 7)) && rdmsr_safe(MSR_HWP_CAPABILITIES, &v)) {    /* HWP */
            pw.flags |= SIEOS_PWR_HWP;
            if (pm_a & (1U << 10))
                pw.flags |= SIEOS_PWR_EPP;
            pw.perf_highest = v & 0xFF;
            pw.perf_guaranteed = (v >> 8) & 0xFF;
            pw.perf_efficient = (v >> 16) & 0xFF;
            pw.perf_lowest = (v >> 24) & 0xFF;
        } else if (eist && rdmsr_safe(MSR_PLATFORM_INFO, &v)) {           /* legacy P-states */
            pw.perf_guaranteed = (v >> 8) & 0xFF;
            pw.perf_lowest = pw.perf_efficient = (v >> 40) & 0xFF;
            uint64_t t;
            pw.perf_highest = rdmsr_safe(MSR_TURBO_RATIO_LIMIT, &t) && (int)(t & 0xFF) > pw.perf_guaranteed
                              ? (int)(t & 0xFF) : pw.perf_guaranteed;
            if (pw.perf_guaranteed && pw.perf_lowest)
                pw.flags |= SIEOS_PWR_PSTATE;
        }
        if (pw.perf_lowest > pw.perf_highest || !pw.perf_highest)
            pw.flags &= ~(SIEOS_PWR_HWP | SIEOS_PWR_EPP | SIEOS_PWR_PSTATE);
    } else if (pw.amd && family >= 0x17 && family <= 0x1A) {
        pw.flags |= SIEOS_PWR_AMDTEMP;
    }
    pw.perf_max = pw.perf_highest;
}

/* AMD Tctl through the SMN (the root complex's index/data registers). */
static int amd_temp(void)
{
    pci_write32(0, 0, 0, 0x60, 0x00059800);
    uint32_t v = pci_read32(0, 0, 0, 0x64);
    int t = (int)(v >> 21) / 8;
    if (v & (1U << 19))
        t -= 49;                                 /* the range with the offset */
    return t > 0 && t < 130 ? t : -1;
}

/* The settings for this processor, from the policy and the thermal cap. */
static void cpu_apply(void)
{
    int top = pw.perf_max;
    if (pw.flags & SIEOS_PWR_HWP) {
        struct pcpu *pc = &pcpu[mycpu()->id];
        if (!pc->hwp_on) {
            pc->hwp_on = wrmsr_safe(MSR_PM_ENABLE, 1);
            if (!pc->hwp_on)
                return;
        }
        int lo = pw.perf_lowest, epp = 128;
        if (pw.policy == SIEOS_POWER_PERFORMANCE) {
            lo = top;
            epp = 0;
        } else if (pw.policy == SIEOS_POWER_POWERSAVE) {
            top = MIN(top, pw.perf_guaranteed ? pw.perf_guaranteed : top);
            epp = 255;
        }
        lo = MIN(lo, top);
        uint64_t req = (uint64_t)lo | (uint64_t)top << 8 | (pw.flags & SIEOS_PWR_EPP ? (uint64_t)epp << 24 : 0);
        wrmsr_safe(MSR_HWP_REQUEST, req);
    } else if (pw.flags & SIEOS_PWR_PSTATE) {
        int ratio = pw.policy == SIEOS_POWER_PERFORMANCE ? top
                  : pw.policy == SIEOS_POWER_POWERSAVE ? pw.perf_efficient : MIN(top, pw.perf_guaranteed);
        ratio = MIN(MAX(ratio, pw.perf_lowest), top);
        wrmsr_safe(MSR_PERF_CTL, (uint64_t)ratio << 8);
    }
}

static void cpu_sample(struct pcpu *pc)
{
    uint64_t v;
    pc->temp = -1;
    if ((pw.flags & SIEOS_PWR_DTS) && rdmsr_safe(MSR_THERM_STATUS, &v) && (v & (1U << 31))) {
        pc->temp = pw.tjmax - (int)((v >> 16) & 0x7F);
        if (v & 3)
            pw.flags |= SIEOS_PWR_HOT;           /* (PROCHOT now or since) */
    }
    if (pw.flags & SIEOS_PWR_APERF) {
        uint64_t a, m;
        if (rdmsr_safe(MSR_APERF, &a) && rdmsr_safe(MSR_MPERF, &m)) {
            if (pc->mperf && m > pc->mperf && a >= pc->aperf)
                pc->mhz = (unsigned)((a - pc->aperf) * (tsc_hz / 1000000) / (m - pc->mperf));
            pc->aperf = a;
            pc->mperf = m;
        }
    }
}

/* ---------------------------------------------------------------- the policy */

static int default_passive(void) { return pw.tjmax - 10; }
static int default_critical(void) { return MIN(pw.tjmax + 5, 125); }

static void thermal_policy(void)
{
    int t = -1;
    uint64_t v;
    if ((pw.flags & SIEOS_PWR_PKGTEMP) && rdmsr_safe(MSR_PKG_THERM_STATUS, &v) && (v & (1U << 31)))
        t = pw.tjmax - (int)((v >> 16) & 0x7F);
    else if (pw.flags & SIEOS_PWR_AMDTEMP)
        t = amd_temp();
    for (int i = 0; i < ncpu; i++)
        t = MAX(t, pcpu[i].temp);
    if (pw.fake)
        t = 70 + 2 * pw.fake++;                  /* 70 C rising 2 C a second: passive at 90, critical at 105 */
    pw.temp = t;
    if (t < 0)
        return;
    int old = pw.level;
    if (t >= pw.passive && pw.level < 10)
        pw.level++;
    else if (t < pw.passive - 5 && pw.level > 0)
        pw.level--;
    if (pw.level != old) {
        if (pw.perf_highest) {
            pw.perf_max = pw.perf_highest - (pw.perf_highest - pw.perf_lowest) * pw.level / 10;
            pw.gen++;
        }
        if (!old || !pw.level)
            kprintf("power: %d C: %s\n", t, pw.level ? "slowing the processors (passive cooling)" : "back to full speed");
    }
    if (t >= pw.critical) {
        if (++pw.hot_seconds == 3) {
            kprintf("power: %d C, at the critical %d C: shutting down\n", t, pw.critical);
            mutex_enter(&pidlock);
            struct proc *init = proc_find(1);
            if (init)
                signal_send(init, SIGPWR);
            mutex_exit(&pidlock);
        }
    } else {
        pw.hot_seconds = 0;
    }
}

static void power_button(void)
{
    uint16_t st = inw(pw.pm1a_evt);
    if (st == 0xFFFF || !(st & (1U << 8)))
        return;
    outw(pw.pm1a_evt, 1U << 8);                  /* (write 1 to clear) */
    if (ticks < pw.button_quiet)
        return;
    pw.button_quiet = ticks + 2 * TIMER_HZ;
    kprintf("power: the power button: shutting down\n");
    mutex_enter(&pidlock);
    struct proc *init = proc_find(1);
    if (init)
        signal_send(init, SIGPWR);
    mutex_exit(&pidlock);
}

/* Every local timer tick, on every processor (lapic_timer_irq). */
/* Each processor's timer interrupt: its own frequency and temperature. */
void power_cpu_tick(void)
{
    struct cpu *c = mycpu();
    if (!pw.ready || c->id < 0 || c->id >= NCPU)
        return;                                  /* (the processors tick before power_init) */
    struct pcpu *pc = &pcpu[c->id];
    if (ticks < pc->next)
        return;
    pc->next = ticks + POLL_TICKS;
    if (pc->gen != pw.gen) {
        pc->gen = pw.gen;
        cpu_apply();
    }
    cpu_sample(pc);
}

/* The clock thread, every tick: the power button, the thermal policy, the fans. */
static kmutex_t pw_lock;                         /* the policy, the thresholds (power_tick, power_ioctl) */

void power_tick(void)
{
    static uint64_t next;
    if (!pw.ready)
        return;
    if (pw.pwrbtn_fixed)
        power_button();
    if (ticks < next)
        return;
    next = ticks + POLL_TICKS;
    mutex_enter(&pw_lock);
    thermal_policy();
    if (pw.sio_kind && ticks / POLL_TICKS % 2 == 0)
        fans_read();
    mutex_exit(&pw_lock);
}

/* The idle loop's wait (interrupts are off; on for the wait). */
void power_idle(void)
{
    if (pw.mwait && pw.policy != SIEOS_POWER_PERFORMANCE) {
        volatile void *mon = &mycpu()->need_resched;
        __asm__ volatile("monitor" :: "a"(mon), "c"(0), "d"(0));
        if (!mycpu()->need_resched)
            __asm__ volatile("sti; mwait" :: "a"(pw.mwait_hint), "c"(1) : "memory");   /* (interrupts wake it) */
        else
            sti();
        return;
    }
    sti();
    hlt();
}

void power_init(const char *cmdline)
{
    acpi_power_init();
    cpu_detect();
    pw.passive = default_passive();
    pw.critical = default_critical();
    pw.temp = -1;
    for (int i = 0; i < NCPU; i++)
        pcpu[i].temp = -1;
    if (pw.flags & SIEOS_PWR_MOBILE)
        strlcpy(pw.fans_name, "the firmware (embedded controller)", sizeof(pw.fans_name));
    else if (!boot_option(cmdline, "nosuperio"))
        superio_probe();
    if (!pw.fans_name[0])
        strlcpy(pw.fans_name, "the firmware (none seen)", sizeof(pw.fans_name));
    pw.gen = 1;                                  /* every processor applies the policy on its next tick */
    pw.fake = boot_option(cmdline, "thermaltest");
    __atomic_store_n(&pw.ready, true, __ATOMIC_RELEASE);
}

const char *power_summary(char *buf, size_t n)
{
    snprintf(buf, n, "Power: %s, %s, %s idle, %s%s%s", pw.flags & SIEOS_PWR_HWP ? "Intel HWP"
             : pw.flags & SIEOS_PWR_PSTATE ? "P-states" : "no frequency control",
             pw.flags & (SIEOS_PWR_DTS | SIEOS_PWR_AMDTEMP) ? "thermal sensors" : "no thermal sensor",
             pw.idle_name, pw.flags & SIEOS_PWR_ACPI_OFF ? "ACPI power-off" : "no ACPI power-off",
             pw.flags & SIEOS_PWR_PWRBTN ? ", power button" : "", pw.sio_kind ? ", fans read" : "");
    return buf;
}

/* ---------------------------------------------------------------- /dev/power */

long power_ioctl(unsigned long cmd, void *arg)
{
    if (cmd == SIEOS_POWER_GET) {
        struct sieos_power_info *pi = kzalloc(sizeof(*pi));
        if (!pi)
            return -ENOMEM;
        pi->dpi_flags = pw.flags;
        pi->dpi_policy = pw.policy;
        pi->dpi_ncpu = MIN(ncpu, SIEOS_POWER_MAXCPU);
        pi->dpi_temp = pw.temp;
        pi->dpi_tjmax = (pw.flags & (SIEOS_PWR_DTS | SIEOS_PWR_AMDTEMP)) ? pw.tjmax : -1;
        pi->dpi_passive = pw.passive;
        pi->dpi_critical = pw.critical;
        pi->dpi_throttle = pw.level * 10;
        pi->dpi_perf_lowest = pw.perf_lowest;
        pi->dpi_perf_highest = pw.perf_highest;
        pi->dpi_perf_max = pw.perf_max;
        pi->dpi_pm_profile = pw.pm_profile;
        strlcpy(pi->dpi_idle, pw.idle_name, sizeof(pi->dpi_idle));
        strlcpy(pi->dpi_cpufreq, pw.flags & SIEOS_PWR_HWP ? "Intel HWP" : pw.flags & SIEOS_PWR_PSTATE ? "P-states"
                                                                                                      : "none",
                sizeof(pi->dpi_cpufreq));
        strlcpy(pi->dpi_sensor, pw.flags & SIEOS_PWR_DTS ? "Intel DTS" : pw.flags & SIEOS_PWR_AMDTEMP ? "AMD SMN"
                                                                                                    : "none",
                sizeof(pi->dpi_sensor));
        strlcpy(pi->dpi_fans, pw.fans_name, sizeof(pi->dpi_fans));
        pi->dpi_nfans = pw.nfans;
        memcpy(pi->dpi_fan, pw.fan, sizeof(pw.fan));
        for (unsigned i = 0; i < pi->dpi_ncpu; i++) {
            pi->dpi_cpu[i].temp = pcpu[i].temp;
            pi->dpi_cpu[i].mhz = pcpu[i].mhz;
        }
        memcpy(arg, pi, sizeof(*pi));
        kfree(pi);
        return 0;
    }
    if (cmd == SIEOS_POWER_SET) {
        if (current->euid != 0)
            return -EPERM;
        struct sieos_power_set ps;
        memcpy(&ps, arg, sizeof(ps));
        if (ps.dps_policy > SIEOS_POWER_POWERSAVE || ps.dps_passive > 125 || ps.dps_critical > 130 ||
            (ps.dps_passive > 0 && ps.dps_passive < 40) || (ps.dps_critical > 0 && ps.dps_critical < 50))
            return -EINVAL;
        mutex_enter(&pw_lock);
        int passive = ps.dps_passive < 0 ? pw.passive : ps.dps_passive ? ps.dps_passive : default_passive();
        int critical = ps.dps_critical < 0 ? pw.critical : ps.dps_critical ? ps.dps_critical : default_critical();
        if (passive >= critical) {
            mutex_exit(&pw_lock);
            return -EINVAL;
        }
        if (ps.dps_policy >= 0)
            pw.policy = ps.dps_policy;
        pw.passive = passive;
        pw.critical = critical;
        pw.gen++;
        mutex_exit(&pw_lock);
        return 0;
    }
    return -ENOTTY;
}
