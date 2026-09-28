/* lscpu - show processors, what they are running and their load */
#include "sieos.h"

static void cpuid(unsigned leaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

int main(void)
{
    unsigned a, b, c, d;
    char vendor[13], brand[49];
    cpuid(0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;
    cpuid(0x80000000, &a, &b, &c, &d);
    brand[0] = 0;
    if (a >= 0x80000004) {
        unsigned *p = (unsigned *)brand;
        for (unsigned leaf = 0x80000002; leaf <= 0x80000004; leaf++, p += 4)
            cpuid(leaf, &p[0], &p[1], &p[2], &p[3]);
        brand[48] = 0;
    }
    const char *model = brand;
    while (*model == ' ')
        model++;

    struct cpuinfo ci[16];
    int n = cpuinfo(ci, 16);
    printf("Architecture:  x86_64\n");
    printf("CPU(s):        %d\n", n);
    printf("Vendor:        %s\n", vendor);
    if (*model)
        printf("Model name:    %s\n", model);
    printf("\nCPU  APIC  STATE    RUNNING   BUSY%%  BUSY-TICKS  IDLE-TICKS\n");
    for (int i = 0; i < n; i++) {
        unsigned long total = ci[i].busy_ticks + ci[i].idle_ticks;
        unsigned long pct = total ? ci[i].busy_ticks * 100 / total : 0;
        char running[16];
        if (ci[i].pid)
            snprintf(running, sizeof(running), "pid %d", ci[i].pid);
        else
            strcpy(running, "idle");
        printf("%3d  %4d  %-7s  %-8s  %4lu  %10lu  %10lu\n", ci[i].id, ci[i].apic_id,
               ci[i].online ? "online" : "offline", running, pct, ci[i].busy_ticks, ci[i].idle_ticks);
    }
    return 0;
}
