/*
 * sieos/power.h - power management (ABI v2): /dev/power (character 181,0).
 *
 * SIEOS_POWER_GET (anyone) returns the state: the policy, the processors'
 * temperatures and frequencies, the thermal policy's thresholds and how
 * much it holds the processors back, the fans it can see, and what the
 * machine supports.  SIEOS_POWER_SET (root) changes the policy and the
 * thresholds; poweradm(1M) keeps them in /etc/power.conf.
 */
#ifndef SIEOS_ABI_POWER_H
#define SIEOS_ABI_POWER_H

#define SIEOS_DEV_POWER_MAJOR 181

#define SIEOS_POWER_IOC_BASE (('P' << 16) | ('W' << 8))
#define SIEOS_POWER_GET      (SIEOS_POWER_IOC_BASE | 0x01)   /* struct sieos_power_info */
#define SIEOS_POWER_SET      (SIEOS_POWER_IOC_BASE | 0x02)   /* struct sieos_power_set (root) */

/* Policies: the processors' speed against their power and heat. */
#define SIEOS_POWER_PERFORMANCE 0          /* full speed, shallow idle states */
#define SIEOS_POWER_BALANCED    1          /* the default */
#define SIEOS_POWER_POWERSAVE   2          /* no turbo, prefer efficiency */

/* dpi_flags: what the machine has and uses */
#define SIEOS_PWR_HWP        0x0001        /* Intel hardware P-states (Speed Shift) */
#define SIEOS_PWR_EPP        0x0002        /* ... with an energy/performance preference */
#define SIEOS_PWR_PSTATE     0x0004        /* legacy P-states (IA32_PERF_CTL) */
#define SIEOS_PWR_MWAIT      0x0008        /* deep idle states with MWAIT */
#define SIEOS_PWR_DTS        0x0010        /* per-core digital thermal sensors */
#define SIEOS_PWR_PKGTEMP    0x0020        /* the package's sensor */
#define SIEOS_PWR_AMDTEMP    0x0040        /* AMD's (SMN) */
#define SIEOS_PWR_APERF      0x0080        /* the actual frequency is measured (APERF/MPERF) */
#define SIEOS_PWR_ACPI_OFF   0x0100        /* power-off through ACPI (S5) */
#define SIEOS_PWR_ACPI_RESET 0x0200        /* restart through the ACPI reset register */
#define SIEOS_PWR_PWRBTN     0x0400        /* the power button shuts down cleanly */
#define SIEOS_PWR_HOT        0x0800        /* the processor has throttled itself since boot (PROCHOT) */
#define SIEOS_PWR_MOBILE     0x1000        /* a laptop or tablet (ACPI): its firmware runs the fans */

#define SIEOS_POWER_MAXCPU 64
#define SIEOS_POWER_MAXFAN 8

struct sieos_power_cpu {
    short temp;                            /* degrees Celsius, -1 unknown */
    unsigned short mhz;                    /* the average while running, 0 unknown */
};

struct sieos_power_fan {
    unsigned int rpm;
    char name[12];
};

struct sieos_power_info {
    unsigned int dpi_flags;
    unsigned int dpi_policy;
    unsigned int dpi_ncpu;
    int dpi_temp;                          /* the package (or the hottest core), -1 unknown */
    int dpi_tjmax;                         /* where the processor throttles itself */
    int dpi_passive;                       /* the thermal policy starts slowing the processors */
    int dpi_critical;                      /* ... shuts the system down */
    unsigned int dpi_throttle;             /* percent of the speed range held back (0: none) */
    unsigned int dpi_perf_lowest, dpi_perf_highest, dpi_perf_max;   /* HWP levels or ratios (x 100 MHz) */
    unsigned int dpi_pm_profile;           /* ACPI Preferred_PM_Profile (1 desktop, 2 mobile, 8 tablet, ...) */
    char dpi_idle[32];                     /* "MWAIT C10 (hint 0x60)", "HLT" */
    char dpi_cpufreq[32];                  /* "Intel HWP", "P-states", "none" */
    char dpi_sensor[32];                   /* "Intel DTS", "AMD SMN", "none" */
    char dpi_fans[48];                     /* who runs the fans */
    unsigned int dpi_nfans;
    struct sieos_power_fan dpi_fan[SIEOS_POWER_MAXFAN];
    struct sieos_power_cpu dpi_cpu[SIEOS_POWER_MAXCPU];
};

struct sieos_power_set {
    int dps_policy;                        /* SIEOS_POWER_*, -1 unchanged */
    int dps_passive;                       /* degrees, -1 unchanged, 0 the default */
    int dps_critical;                      /* degrees, -1 unchanged, 0 the default */
};

#endif
