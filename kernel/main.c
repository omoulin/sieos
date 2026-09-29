/*
 * main.c - Kernel entry point.
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
    /* the Orbit Node logo beside the banner box (framebuffer consoles) */
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
    display_init();                   /* displays: the console's framebuffer write-combining, native drivers */
    char msg[128];
    snprintf(msg, sizeof(msg), "Memory: %lu MiB usable, %lu KiB kernel, paging enabled",
             pmm_total_pages() * 4 / 1024, pmm_kernel_pages() * 4);
    ok(msg);

    proc_init_cpu(&cpus[0]);
    bkl_lock();                       /* held by the BSP until its idle loop */
    timer_init();
    snprintf(msg, sizeof(msg), "Timer: PIT at %d Hz, scheduler ready", TIMER_HZ);
    ok(msg);

    random_init();
    ok(random_hw_available() ? "Random: /dev/urandom (ChaCha20, RDRAND/RDSEED + interrupt timing)"
                             : "Random: /dev/urandom (ChaCha20, interrupt timing)");

    acpi_init(mb_info);
    lapic_init();
    irq_use_ioapic();                 /* device interrupts through the I/O APIC, if there is one */
    lapic_timer_start();
    smp_boot();
    if (lapic_ok)
        snprintf(msg, sizeof(msg), "SMP: %d CPU%s online (local APIC timers, %s, big kernel lock)",
                 ncpu, ncpu > 1 ? "s" : "", ioapic_ok ? "I/O APIC interrupts" : "8259 PIC interrupts");
    else
        snprintf(msg, sizeof(msg), "SMP: no ACPI MADT found, running on 1 CPU");
    ok(msg);

    tty_init();
    keyboard_init();
    input_init();
    snprintf(msg, sizeof(msg), "Console: %s + COM1 serial, PS/2 keyboard, %s mouse", console_mode(),
             input_absolute() ? "absolute (vmmouse)" : "PS/2");
    ok(msg);
    const char *cmdline = boot_cmdline(mb_info);
    usb_init(cmdline);
    if (usb_summary(msg, sizeof(msg)))
        ok(msg);
    i2c_hid_init(cmdline);
    if (i2c_hid_summary(msg, sizeof(msg)))
        ok(msg);
    for (int i = 0; i < ndisplays; i++) {
        struct display *d = &displays[i];
        snprintf(msg, sizeof(msg), "Display: fb%d %s %ux%u (%s)%s", i, d->ops->name, d->mode.width, d->mode.height,
                 d->desc, d->console ? ", console" : "");
        ok(msg);
    }

    ata_init();
    const char *dev = blk_init();
    if (!dev)
        panic("no root device: attach an ext4 disk image or boot the ISO with its module");
    if ((root_fs = ext4_mount(blk_root(), false))) {
        vfs_init();
        vfs_mount_all();
        snprintf(msg, sizeof(msg), "Root file system: ext4 on %s; tmpfs /tmp and /dev/shm, proc, devpts", dev);
        ok(msg);
    } else {
        fail("Root file system: could not mount ext4");
        panic("cannot mount root file system");
    }

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

    kernel_running = true;            /* from now on kernel code runs in LWPs: disk I/O may sleep */
    cpu_idle();
}
