/*
 * main.c - Kernel entry point.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "kernel.h"
#include "random.h"
#include "arch.h"
#include "mm.h"
#include "proc.h"
#include "fs.h"
#include "ata.h"
#include "blkdev.h"
#include "display.h"
#include "smp.h"
#include "tty.h"
#include "poll.h"
#include "net.h"
#include "hid.h"
#include "power.h"
#include "ddi.h"

#define MB2_BOOTLOADER_MAGIC 0x36D76289

enum { BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LGRAY,
       DGRAY, LBLUE, LGREEN, LCYAN, LRED, LMAGENTA, YELLOW, WHITE };

static const char *logo[] = {
    "          ____      ___     _____      ___      ____          ",
    "         / ___|    |_ _|   | ____|    / _ \\    / ___|         ",
    "         \\___ \\     | |    |  _|     | | | |   \\___ \\         ",
    "          ___) |    | |    | |___    | |_| |    ___) |        ",
    "         |____/    |___|   |_____|    \\___/    |____/         ",
    "                                                              ",
};
static const char *tagline =
    "       Synthetic Intelligence Enhanced Operating System       ";

static void banner(void)
{
    char indent[64];
    int pad = (console_cols() - 64) / 2;
    pad = pad < 0 ? 0 : pad > 63 ? 63 : pad;
    memset(indent, ' ', pad);
    indent[pad] = 0;
    const char *rule = "+--------------------------------------------------------------+";
    console_set_color(LBLUE, BLACK);
    kprintf("%s%s\n", indent, rule);
    for (size_t i = 0; i < ARRAY_SIZE(logo); i++) {
        console_set_color(LBLUE, BLACK);
        kprintf("%s|", indent);
        console_set_color(LCYAN, BLACK);
        kprintf("%s", logo[i]);
        console_set_color(LBLUE, BLACK);
        kprintf("|\n");
    }
    kprintf("%s|", indent);
    console_set_color(YELLOW, BLACK);
    kprintf("%s", tagline);
    console_set_color(LBLUE, BLACK);
    kprintf("|\n%s%s\n", indent, rule);
    console_set_color(DGRAY, BLACK);
    kprintf("%s  %s %s - %s - x86_64\n", indent, OS_NAME, OS_RELEASE, OS_LONGNAME);
    kprintf("%s  Developed by %s - released under the %s\n\n", indent, OS_AUTHOR, OS_LICENSE);
    console_set_color(LGRAY, BLACK);
    /* the logo (the Facet cube) beside the banner box (framebuffer consoles) */
    int size = 132;
    int x = pad * 8 - size - 24, y = (9 * 16 - size) / 2;             /* 8x16 character cells */
    if (x >= 8)
        console_logo(x, y, size);
}

static void ok(const char *what)
{
    console_set_color(LGRAY, BLACK);
    kprintf("[");
    console_set_color(LGREEN, BLACK);
    kprintf(" OK ");
    console_set_color(LGRAY, BLACK);
    kprintf("] %s\n", what);
}

static void fail(const char *what)
{
    console_set_color(LGRAY, BLACK);
    kprintf("[");
    console_set_color(LRED, BLACK);
    kprintf("FAIL");
    console_set_color(LGRAY, BLACK);
    kprintf("] %s\n", what);
}

/* A boot summary line, for the drivers. */
void ddi_report(const char *line)
{
    ok(line);
}

static const char *kernel_cmdline = "";

const char *ddi_cmdline(void)
{
    return kernel_cmdline;
}

/* A word of the boot command line. */
bool boot_option(const char *cmdline, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = cmdline; *p;) {
        while (*p == ' ')
            p++;
        const char *e = strchr(p, ' ');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, word, n))
            return true;
        p += len;
    }
    return false;
}

/* The multiboot2 command line (after the kernel path), e.g. "text". */
static const char *boot_cmdline(uint64_t mb_info)
{
    static char line[256];
    uint32_t total = *(uint32_t *)P2V(mb_info);
    uint8_t *p = (uint8_t *)P2V(mb_info) + 8, *end = (uint8_t *)P2V(mb_info) + total;
    while (p < end) {
        uint32_t type = ((uint32_t *)p)[0], size = ((uint32_t *)p)[1];
        if (type == 0)
            break;
        if (type == 1) {
            strlcpy(line, (const char *)p + 8, sizeof(line));
            return line;
        }
        p += (size + 7) & ~7;
    }
    return "";
}

volatile bool kernel_running;

void kmain(uint32_t magic, uint32_t mb_info)
{
    console_init(magic == MB2_BOOTLOADER_MAGIC ? mb_info : 0);
    banner();
    if (magic != MB2_BOOTLOADER_MAGIC)
        panic("not booted by a multiboot2 loader (magic %x)", magic);

    cpu_early_init();                 /* BSP per-CPU data, GDT/TSS, %gs */
    idt_init();
    ok("CPU: long mode, GDT/TSS, IDT, PIC");

    pmm_init(mb_info);
    vmm_init();
    const char *cmdline = boot_cmdline(mb_info);
    kernel_cmdline = cmdline;
    smp_want_x2apic = boot_option(cmdline, "x2apic");
    acpi_init(mb_info);               /* the tables (the PM timer and HPET: references when there is no PIT) */
    dmar_init();                      /* VT-d left on by the firmware: off, before any driver's DMA */
    modules_init(boot_archive_pa, boot_archive_size);   /* the drivers the boot loader brought */
    display_init();                   /* displays: the console's framebuffer write-combining */
    modules_attach(DDI_PHASE_DISPLAY);                  /* the displays' drivers */
    display_order();
    char msg[128];
    snprintf(msg, sizeof(msg), "Memory: %lu MiB usable, %lu KiB kernel, paging enabled",
             pmm_total_pages() * 4 / 1024, pmm_kernel_pages() * 4);
    ok(msg);

    proc_init_cpu(&cpus[0]);

    timer_init();
    if (pit_ok)
        snprintf(msg, sizeof(msg), "Timer: PIT at %d Hz, TSC %lu MHz, scheduler ready", TIMER_HZ, tsc_hz / 1000000);
    else
        snprintf(msg, sizeof(msg), "Timer: no PIT (gated off), local APIC at %d Hz, TSC %lu MHz (%s), scheduler ready",
                 TIMER_HZ, tsc_hz / 1000000, timer_tsc_source());
    ok(msg);

    random_init();
    ok(random_hw_available() ? "Random: /dev/urandom (ChaCha20, RDRAND/RDSEED + interrupt timing)"
                             : "Random: /dev/urandom (ChaCha20, interrupt timing)");

    lapic_init();
    irq_use_ioapic();                 /* device interrupts through the I/O APIC, if there is one */
    lapic_timer_start();
    smp_boot();
    if (lapic_ok)
        snprintf(msg, sizeof(msg), "SMP: %d CPU%s online (local %sAPIC timers, %s)",
                 ncpu, ncpu > 1 ? "s" : "", x2apic ? "x2" : "", ioapic_ok ? "I/O APIC interrupts" : "8259 PIC interrupts");
    else
        snprintf(msg, sizeof(msg), "SMP: no ACPI MADT found, running on 1 CPU");
    ok(msg);
    uint64_t t0 = ticks, until = hrtime() + 100000000UL;  /* the tick advances (100 ms)? */
    while (ticks == t0 && hrtime() < until) {
        sti();
        __asm__ volatile("pause");
        cli();
    }
    if (ticks == t0)
        fail("Timer: no timer interrupt (PIT IRQ 0 nor the local APIC): the system cannot schedule");

    power_init(cmdline);
    ok(power_summary(msg, sizeof(msg)));

    tty_init();
    keyboard_init();
    modules_attach(DDI_PHASE_BOOT);   /* the drivers of the devices found: input, USB, disks */
    if (ps2_present)
        snprintf(msg, sizeof(msg), "Console: %s + COM1 serial, PS/2 keyboard, %s mouse", console_mode(),
                 input_absolute() ? "absolute (vmmouse)" : "PS/2");
    else
        snprintf(msg, sizeof(msg), "Console: %s + COM1 serial, no PS/2 controller", console_mode());
    ok(msg);
    for (int i = 0; i < ndisplays; i++) {
        struct display *d = &displays[i];
        snprintf(msg, sizeof(msg), "Display: fb%d %s %ux%u (%s)%s", i, d->ops->name, d->mode.width, d->mode.height,
                 d->desc, d->console ? ", console" : "");
        ok(msg);
    }

    const char *dev = blk_init(cmdline);
    if (!dev)
        panic("no root device: attach an ext4 disk image or boot the ISO with its module (no disk driver? see the drv: lines)");
    if ((root_fs = ext4_mount(blk_root(), false))) {
        vfs_init();
        vfs_mount_all();
        snprintf(msg, sizeof(msg), "Root file system: ext4 on %s; tmpfs /tmp and /dev/shm, proc, devpts", dev);
        ok(msg);
    } else {
        fail("Root file system: could not mount ext4");
        panic("cannot mount root file system");
    }

    modules_root();                   /* /drv's drivers; the network cards' (DDI_PHASE_ROOT) */
    net_init();
    if (nnetif) {
        net_wait_config(3 * TIMER_HZ);
        for (int i = 0; i < nnetif; i++) {
            struct netif *ifp = &netifs[i];
            char ip[16], gw[16];
            if (ifp->up)
                snprintf(msg, sizeof(msg), "Network: %s %s %02x:%02x:%02x:%02x:%02x:%02x, %s gw %s (%s)", ifp->name,
                         ifp->nic->name, ifp->mac[0], ifp->mac[1], ifp->mac[2], ifp->mac[3], ifp->mac[4],
                         ifp->mac[5], ip_str(ifp->ip, ip), ip_str(ifp->gateway, gw), ifp->dhcp ? "DHCP" : "static");
            else
                snprintf(msg, sizeof(msg), "Network: %s %s %02x:%02x:%02x:%02x:%02x:%02x, no address (DHCP continues)",
                         ifp->name, ifp->nic->name, ifp->mac[0], ifp->mac[1], ifp->mac[2], ifp->mac[3], ifp->mac[4],
                         ifp->mac[5]);
            ok(msg);
        }
    } else {
        fail("Network: no supported network card (e1000, virtio-net) found");
    }

    int pid = proc_spawn_init("/sbin/init", cmdline);
    if (pid < 0)
        pid = proc_spawn_init("/bin/sh", NULL);
    if (pid < 0)
        panic("cannot start /sbin/init or /bin/sh (error %d)", pid);
    ok("Starting init");

    kthreads_start();                 /* the clock, interrupt, network and fsflush threads */
    kernel_running = true;            /* from now on kernel code runs in LWPs: disk I/O may sleep */
    cpu_idle();
}
