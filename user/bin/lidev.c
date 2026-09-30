/*
 * lidev - list the devices: each PCI function found at boot, with its
 * vendor:device ID, its class, the driver using it and its name.
 *   lidev       one line per device
 *   lidev -v    also the subsystem, revision, programming interface and IRQ
 *   lidev -n    IDs only (no names looked up)
 * Names come from the PCI ID database (/usr/share/misc/pci.ids) when the
 * system has it, else from a small built-in list.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"
#include "sieos/sysinfo.h"

#define MAXDEV 64
#define PCI_IDS "/usr/share/misc/pci.ids"

struct dev {
    struct sieos_devinfo di;
    char vendor[80], name[128], subsys[128], class_name[48];
};
static struct dev devs[MAXDEV];
static int ndev;

/* The PCI classes (and the usual subclasses), for when there is no database. */
static const struct { int cls, sub; const char *name; } classes[] = {
    { 0x00, -1, "Unclassified device" }, { 0x01, 0x01, "IDE interface" }, { 0x01, 0x06, "SATA controller" },
    { 0x01, 0x08, "Non-Volatile memory controller" }, { 0x01, -1, "Mass storage controller" },
    { 0x02, 0x00, "Ethernet controller" }, { 0x02, 0x80, "Network controller" }, { 0x02, -1, "Network controller" },
    { 0x03, 0x00, "VGA compatible controller" }, { 0x03, -1, "Display controller" },
    { 0x04, 0x03, "Audio device" }, { 0x04, -1, "Multimedia controller" }, { 0x05, -1, "Memory controller" },
    { 0x06, 0x00, "Host bridge" }, { 0x06, 0x01, "ISA bridge" }, { 0x06, 0x04, "PCI bridge" }, { 0x06, -1, "Bridge" },
    { 0x07, -1, "Communication controller" }, { 0x08, -1, "System peripheral" }, { 0x09, -1, "Input device controller" },
    { 0x0C, 0x03, "USB controller" }, { 0x0C, 0x05, "SMBus" }, { 0x0C, -1, "Serial bus controller" },
    { 0x0D, -1, "Wireless controller" }, { 0x10, -1, "Encryption controller" }, { 0x11, -1, "Signal processing controller" },
    { 0x12, -1, "Processing accelerators" }, { 0x13, -1, "Non-Essential Instrumentation" },
};

/* The devices SIEOS has drivers for, and QEMU's usual ones. */
static const struct { unsigned v, d; const char *name; } known[] = {
    { 0x8086, 0x1237, "440FX - 82441FX PMC [Natoma]" }, { 0x8086, 0x7000, "82371SB PIIX3 ISA [Natoma/Triton II]" },
    { 0x8086, 0x7010, "82371SB PIIX3 IDE [Natoma/Triton II]" }, { 0x8086, 0x7113, "82371AB/EB/MB PIIX4 ACPI" },
    { 0x8086, 0x100E, "82540EM Gigabit Ethernet Controller" }, { 0x8086, 0x10D3, "82574L Gigabit Network Connection" },
    { 0x8086, 0x29C0, "82G33/G31/P35/P31 Express DRAM Controller" }, { 0x8086, 0x2918, "82801IB (ICH9) LPC Interface Controller" },
    { 0x8086, 0x2922, "82801IR/IO/IH (ICH9R/DO/DH) 6 port SATA Controller [AHCI mode]" },
    { 0x8086, 0x7D51, "Arrow Lake-P [Arc Pro 130T/140T]" }, { 0x8086, 0x7DD1, "Arrow Lake-P [Arc 130T/140T]" },
    { 0x8086, 0xA780, "Raptor Lake-S GT1 [UHD Graphics 770]" }, { 0x8086, 0xA782, "Raptor Lake-S GT1 [UHD Graphics 730]" },
    { 0x1234, 0x1111, "QEMU standard VGA" }, { 0x1AF4, 0x1000, "Virtio network device" },
    { 0x1AF4, 0x1041, "Virtio 1.0 network device" }, { 0x1B36, 0x000D, "QEMU XHCI Host Controller" },
};
static const struct { unsigned v; const char *name; } vendors[] = {
    { 0x8086, "Intel Corporation" }, { 0x1234, "QEMU" }, { 0x1AF4, "Red Hat, Inc." }, { 0x1B36, "Red Hat, Inc." },
    { 0x10DE, "NVIDIA Corporation" }, { 0x1002, "Advanced Micro Devices, Inc. [AMD/ATI]" }, { 0x1022, "Advanced Micro Devices, Inc. [AMD]" },
    { 0x10EC, "Realtek Semiconductor Co., Ltd." }, { 0x144D, "Samsung Electronics Co Ltd" }, { 0x15AD, "VMware" },
};

static unsigned hex(const char *s, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++) {
        int c = tolower((unsigned char)s[i]);
        if (!isxdigit(c))
            return ~0u;
        v = v * 16 + (c <= '9' ? c - '0' : c - 'a' + 10);
    }
    return v;
}

static void copy_name(char *out, size_t n, const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    snprintf(out, n, "%s", s);
    out[strcspn(out, "\r\n")] = 0;
}

/* One pass over pci.ids: the vendors, devices, subsystems and classes we need. */
static bool from_database(void)
{
    FILE *f = fopen(PCI_IDS, "r");
    if (!f)
        return false;
    char line[512];
    unsigned vendor = ~0u, device = ~0u, cls = ~0u;
    bool in_classes = false;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (line[0] == 'C' && line[1] == ' ') {            /* "C 03  Display controller" */
            in_classes = true;
            cls = hex(line + 2, 2);
            for (int i = 0; i < ndev; i++)
                if (devs[i].di.class_code == cls && !devs[i].class_name[0])
                    copy_name(devs[i].class_name, sizeof(devs[i].class_name), line + 4);
            continue;
        }
        if (in_classes) {
            if (line[0] == '\t' && line[1] != '\t') {       /* "\t00  VGA compatible controller" */
                unsigned sub = hex(line + 1, 2);
                for (int i = 0; i < ndev; i++)
                    if (devs[i].di.class_code == cls && devs[i].di.subclass == sub)
                        copy_name(devs[i].class_name, sizeof(devs[i].class_name), line + 3);
            }
            continue;
        }
        if (line[0] != '\t') {                               /* "8086  Intel Corporation" */
            vendor = hex(line, 4);
            device = ~0u;
            for (int i = 0; i < ndev; i++)
                if (devs[i].di.vendor == vendor)
                    copy_name(devs[i].vendor, sizeof(devs[i].vendor), line + 4);
        } else if (line[1] != '\t') {                        /* "\t7d51  Arrow Lake-P ..." */
            device = hex(line + 1, 4);
            for (int i = 0; i < ndev; i++)
                if (devs[i].di.vendor == vendor && devs[i].di.device == device)
                    copy_name(devs[i].name, sizeof(devs[i].name), line + 5);
        } else {                                             /* "\t\t1028 0b1a  Subsystem" */
            unsigned sv = hex(line + 2, 4), sd = hex(line + 7, 4);
            for (int i = 0; i < ndev; i++)
                if (devs[i].di.vendor == vendor && devs[i].di.device == device && devs[i].di.subvendor == sv &&
                    devs[i].di.subdevice == sd)
                    copy_name(devs[i].subsys, sizeof(devs[i].subsys), line + 12);
        }
    }
    fclose(f);
    return true;
}

static void from_builtin(struct dev *d)
{
    for (size_t k = 0; k < sizeof(vendors) / sizeof(vendors[0]) && !d->vendor[0]; k++)
        if (vendors[k].v == d->di.vendor)
            snprintf(d->vendor, sizeof(d->vendor), "%s", vendors[k].name);
    for (size_t k = 0; k < sizeof(known) / sizeof(known[0]) && !d->name[0]; k++)
        if (known[k].v == d->di.vendor && known[k].d == d->di.device)
            snprintf(d->name, sizeof(d->name), "%s", known[k].name);
    for (size_t k = 0; k < sizeof(classes) / sizeof(classes[0]) && !d->class_name[0]; k++)
        if (classes[k].cls == d->di.class_code && (classes[k].sub < 0 || classes[k].sub == d->di.subclass))
            snprintf(d->class_name, sizeof(d->class_name), "%s", classes[k].name);
}

static void usage(void)
{
    fprintf(stderr, "usage: lidev [-v] [-n]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    bool verbose = false, numeric = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || !argv[i][1])
            usage();
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'v')
                verbose = true;
            else if (*p == 'n')
                numeric = true;
            else
                usage();
        }
    }
    while (ndev < MAXDEV && devinfo(&devs[ndev].di, ndev) == 0)
        ndev++;
    if (ndev == 0) {
        printf("no devices\n");
        return 0;
    }
    if (!numeric) {
        from_database();
        for (int i = 0; i < ndev; i++)
            from_builtin(&devs[i]);
    }
    if (!verbose)
        printf("%-13s %-10s %-26s %-12s %s\n", "LOCATION", "ID", "CLASS", "DRIVER", "NAME");
    for (int i = 0; i < ndev; i++) {
        struct dev *d = &devs[i];
        char cls[48], name[220];
        if (d->class_name[0])
            snprintf(cls, sizeof(cls), "%s", d->class_name);
        else
            snprintf(cls, sizeof(cls), "class %02x%02x", d->di.class_code, d->di.subclass);
        if (numeric || (!d->vendor[0] && !d->name[0]))
            snprintf(name, sizeof(name), "-");
        else
            snprintf(name, sizeof(name), "%s%s%s", d->vendor[0] ? d->vendor : "", d->vendor[0] && d->name[0] ? " " : "",
                     d->name[0] ? d->name : "(unknown device)");
        const char *loc = d->di.location + (strncmp(d->di.location, "0000:", 5) ? 0 : 5);
        if (!verbose) {
            printf("%-13s %04x:%04x  %-26.26s %-12s %s\n", loc, d->di.vendor, d->di.device, cls,
                   d->di.driver[0] ? d->di.driver : "-", name);
            continue;
        }
        printf("%s %s [%02x%02x]: %s [%04x:%04x]\n", loc, cls, d->di.class_code, d->di.subclass, name, d->di.vendor,
               d->di.device);
        printf("    subsystem %04x:%04x%s%s\n", d->di.subvendor, d->di.subdevice, d->subsys[0] ? "  " : "", d->subsys);
        printf("    revision %02x  prog-if %02x", d->di.revision, d->di.prog_if);
        if (d->di.irq >= 0)
            printf("  irq %d", d->di.irq);
        printf("\n    driver %s\n", d->di.driver[0] ? d->di.driver : "(none)");
    }
    return 0;
}
