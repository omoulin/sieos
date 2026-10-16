/*
 * tests/aml/test.c - The AML reader (user/i2chid/aml.c) on the host:
 *   aml-test [-m address=dword]... table.aml...
 * loads the tables (the DSDT first) as SYS_ACPI gives them, and prints the
 * I2C HID devices found, one line each. -m: what memory holds there (the
 * firmware's variables); anything else reads 0.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdarg.h>
#include "aml.h"

static struct { uint64_t a; uint32_t v; } mem[64];
static int nmem;
uint32_t aml_mem32(uint64_t pa)
{
    for (int i = 0; i < nmem; i++) if (mem[i].a == pa) return mem[i].v;
    return 0;
}
void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

int main(int argc, char **argv)
{
    static uint8_t t[8 << 20];
    long n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc && nmem < 64) {
            char *e;
            mem[nmem].a = strtoull(argv[++i], &e, 0);
            mem[nmem++].v = (uint32_t)strtoul(e + 1, 0, 0);
            continue;
        }
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 2; }
        n += (long)fread(t + n, 1, sizeof t - n, f);
        fclose(f);
    }
    printf("%ld named objects\n", aml_load(t, n));
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
