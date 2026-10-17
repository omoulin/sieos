/*
 * tests/aml/test.c - The AML interpreter (user/lib/aml.c) on the host:
 *   aml-test [-m address=dword] [-e offset=byte] [-w] [-d HID] [-v PATH[:arg,...]]... table.aml...
 * loads the tables (the DSDT first) as SYS_ACPI gives them. Without -d or
 * -v it prints the I2C HID devices found, one line each. -m: what memory
 * holds there (the firmware's variables); -e: a byte of the embedded
 * controller; anything else reads 0 (I/O ports too). -d: the devices with
 * this _HID/_CID; -v: an object's value (a method: called with the
 * arguments), its integers; -w: print what the AML writes, its Notify()s,
 * its PCI reads (bridges answer: bus 2 behind).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdarg.h>
#include "aml.h"

static struct { uint64_t a; uint32_t v; } mem[64];
static int nmem, writes;
static uint8_t ec[256];
static uint32_t *mem32(uint64_t pa)
{
    static uint32_t zero;
    for (int i = 0; i < nmem; i++) if (mem[i].a == pa) return &mem[i].v;
    if (nmem == 64) return zero = 0, &zero;
    mem[nmem] = (typeof(mem[0])){ pa, 0 };
    return &mem[nmem++].v;
}
int aml_io(int space, uint64_t a, int w, uint64_t *v, int write)
{
    static const char *sp[] = { "memory", "io", "pci", "ec" };
    if (write) {
        if (writes) printf("write %s 0x%lx/%d = 0x%lx\n", space < 4 ? sp[space] : "?", (long)a, w, (long)*v);
        if (space == 3 && a < 256) ec[a] = (uint8_t)*v;
        for (int i = 0; !space && i < w; i++) {         /* memory keeps it */
            uint32_t *d = mem32((a + i) & ~3ULL), sh = (a + i) % 4 * 8;
            *d = (*d & ~(0xFFu << sh)) | (uint32_t)(*v >> 8 * i & 0xFF) << sh;
        }
        return 0;
    }
    *v = 0;
    if (space == 2) {                                   /* PCI: bridges say bus 2 is behind them */
        if (writes) printf("read pci %02lx:%02lx.%lx %02lx/%d\n", (long)(a >> 16), (long)(a >> 11 & 31), (long)(a >> 8 & 7), (long)(a & 255), w);
        *v = (a & 0xFC) == 0x18 ? 0x030200 : 0;
    }
    if (space == 3) *v = a < 256 ? ec[a] : 0;
    for (int i = 0; !space && i < w; i++) *v |= (uint64_t)(*mem32((a + i) & ~3ULL) >> (a + i) % 4 * 8 & 0xFF) << 8 * i;
    return 0;
}
void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
static void notify(int dev, int64_t v)
{
    char p[64];
    aml_path(dev, p, sizeof p);
    if (writes) printf("notify %s 0x%lx\n", p, (long)v);
}

int main(int argc, char **argv)
{
    static uint8_t t[8 << 20];
    long n = 0;
    int ask = 0;
    for (int i = 1; i < argc; i++) {
        char *e;
        if (!strcmp(argv[i], "-m") && i + 1 < argc && nmem < 64) {
            uint64_t a = strtoull(argv[++i], &e, 0);
            *mem32(a) = (uint32_t)strtoul(e + 1, 0, 0);
        } else if (!strcmp(argv[i], "-e") && i + 1 < argc) {
            unsigned long o = strtoul(argv[++i], &e, 0);
            ec[o & 255] = (uint8_t)strtoul(e + 1, 0, 0);
        } else if (!strcmp(argv[i], "-w")) writes = 1;
        else if ((!strcmp(argv[i], "-v") || !strcmp(argv[i], "-d")) && i + 1 < argc) ask = 1, i++;
        else {
            FILE *f = fopen(argv[i], "rb");
            if (!f) { perror(argv[i]); return 2; }
            n += (long)fread(t + n, 1, sizeof t - n, f);
            fclose(f);
        }
    }
    printf("%ld named objects\n", aml_load(t, n));
    aml_notify = notify;
    for (int i = 1; i < argc; i++) {                            /* -d, -v: in order */
        char p[64], *a;
        int64_t v[24], args[7];
        int na = 0, k, dev[16];
        if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "-e")) { i++; continue; }
        if (!strcmp(argv[i], "-d")) {
            k = aml_devs(argv[++i], dev, 16);
            for (int j = 0; j < k; j++) { aml_path(dev[j], p, sizeof p); printf("%s: %s\n", argv[i], p); }
            continue;
        }
        if (strcmp(argv[i], "-v")) continue;
        snprintf(p, sizeof p, "%s", argv[++i]);
        if ((a = strchr(p, ':'))) for (*a++ = 0; *a && na < 7; a += *a == ',') args[na++] = strtoll(a, &a, 0);
        k = aml_eval(0, p, na, args, v, 24);
        printf("%s =", argv[i]);
        if (k < 0) printf(" (none)");
        for (int j = 0; j < k; j++) printf(" 0x%lx", (long)v[j]);
        printf("\n");
    }
    if (ask) return 0;
    aml_hid_t d[8];
    int k = aml_hid(d, 8);
    for (int i = 0; i < k; i++) {
        int64_t adr = -1;
        char p[64];
        snprintf(p, sizeof p, "%s._ADR", d[i].bus);
        if (d[i].bus[0]) aml_int(p, 0, 0, &adr);
        printf("%s %s addr=0x%x speed=%u bus=%s adr=0x%lx pin=%d%s gpio=%s irq=%d desc=%d\n", d[i].path, d[i].hid, d[i].addr,
               d[i].speed, d[i].bus, (long)adr, d[i].pin, d[i].pin_low ? "(low)" : "", d[i].gpio, d[i].irq, d[i].desc);
        for (int64_t g = 0, a[2], n, first, at; g < 32; g++) {     /* the pin's pad, as i2chid finds it */
            a[0] = g; a[1] = 1;
            if (aml_int("\\_SB.GINF", 2, a, &n) || (a[1] = 8, aml_int("\\_SB.GINF", 2, a, &first))) break;
            if (d[i].pin < first || d[i].pin >= first + n || (a[1] = 2, aml_int("\\_SB.GADR", 2, a, &at))) continue;
            printf("  GPIO group %ld pad %ld at 0x%lx\n", (long)g, (long)(d[i].pin - first), (long)(at + (d[i].pin - first) * 16));
            break;
        }
    }
    return 0;
}
