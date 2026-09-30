/*
 * poweradm - power management (after Solaris poweradm(1M)).
 *
 *   poweradm                    the state: policy, processors, temperatures, fans
 *   poweradm set NAME=VALUE...  change and keep in /etc/power.conf:
 *       policy=performance|balanced|powersave   (any user: the desktop's choice)
 *       passive=DEGREES|default    critical=DEGREES|default   (root)
 *   poweradm -r                 apply /etc/power.conf (at boot, from /etc/rc)
 *
 * Installed set-user-ID root: /dev/power's settings are root's, but anyone
 * may choose the policy.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"
#include <sys/ioctl.h>
#include "sieos/power.h"

#define CONF "/etc/power.conf"

static const char *const policies[] = { "performance", "balanced", "powersave" };

static int open_power(void)
{
    int fd = open("/dev/power", O_RDONLY);
    if (fd < 0)
        perror("poweradm: /dev/power");
    return fd;
}

static int policy_of(const char *s)
{
    for (int i = 0; i < 3; i++)
        if (!strcmp(s, policies[i]))
            return i;
    if (!strcmp(s, "power-saver") || !strcmp(s, "power"))
        return SIEOS_POWER_POWERSAVE;
    return -1;
}

static int show(void)
{
    int fd = open_power();
    if (fd < 0)
        return 1;
    struct sieos_power_info pi;
    if (ioctl(fd, SIEOS_POWER_GET, &pi) < 0) {
        perror("poweradm");
        return 1;
    }
    close(fd);
    printf("Policy:       %s\n", pi.dpi_policy < 3 ? policies[pi.dpi_policy] : "?");
    printf("Processors:   %u, frequency: %s", pi.dpi_ncpu, pi.dpi_cpufreq);
    if (pi.dpi_perf_highest)
        printf(" (levels %u-%u, allowed up to %u)", pi.dpi_perf_lowest, pi.dpi_perf_highest, pi.dpi_perf_max);
    printf(", idle: %s\n", pi.dpi_idle);
    if (pi.dpi_temp >= 0)
        printf("Temperature:  %d C, the processor throttles at %d C (%s)\n", pi.dpi_temp, pi.dpi_tjmax, pi.dpi_sensor);
    else
        printf("Temperature:  no sensor\n");
    printf("Thermal:      passive cooling from %d C, shutdown at %d C: %s%s\n", pi.dpi_passive, pi.dpi_critical,
           pi.dpi_throttle ? "slowed" : "full speed", pi.dpi_flags & SIEOS_PWR_HOT ? " (the processor throttled itself)" : "");
    if (pi.dpi_throttle)
        printf("              %u%% of the speed range held back\n", pi.dpi_throttle);
    bool any = false;
    for (unsigned i = 0; i < pi.dpi_ncpu; i++)
        any |= pi.dpi_cpu[i].temp >= 0 || pi.dpi_cpu[i].mhz;
    if (any) {
        printf("CPU:         ");
        for (unsigned i = 0; i < pi.dpi_ncpu; i++) {
            printf(" %u:", i);
            if (pi.dpi_cpu[i].temp >= 0)
                printf(" %d C", pi.dpi_cpu[i].temp);
            if (pi.dpi_cpu[i].mhz)
                printf(" %u MHz", pi.dpi_cpu[i].mhz);
            if (i % 4 == 3 && i + 1 < pi.dpi_ncpu)
                printf("\n             ");
        }
        printf("\n");
    }
    printf("Fans:         %s", pi.dpi_fans);
    for (unsigned i = 0; i < pi.dpi_nfans; i++)
        printf("%s%s %u RPM", i ? ", " : ": ", pi.dpi_fan[i].name, pi.dpi_fan[i].rpm);
    printf("\n");
    printf("Machine:      %s, %s%s%s\n", pi.dpi_flags & SIEOS_PWR_MOBILE ? "laptop or tablet" : "desktop or server",
           pi.dpi_flags & SIEOS_PWR_ACPI_OFF ? "ACPI power-off" : "no ACPI power-off",
           pi.dpi_flags & SIEOS_PWR_ACPI_RESET ? ", ACPI reset" : "",
           pi.dpi_flags & SIEOS_PWR_PWRBTN ? ", power button" : "");
    return 0;
}

/* NAME=VALUE into the settings; -1 on a bad one. */
static int parse(const char *arg, struct sieos_power_set *ps)
{
    const char *eq = strchr(arg, '=');
    if (!eq)
        return -1;
    size_t n = eq - arg;
    const char *v = eq + 1;
    if (n == 6 && !strncmp(arg, "policy", 6))
        return (ps->dps_policy = policy_of(v)) < 0 ? -1 : 0;
    int *field = n == 7 && !strncmp(arg, "passive", 7) ? &ps->dps_passive
               : n == 8 && !strncmp(arg, "critical", 8) ? &ps->dps_critical : NULL;
    if (!field)
        return -1;
    if (!strcmp(v, "default")) {
        *field = 0;
        return 0;
    }
    char *end;
    long d = strtol(v, &end, 10);
    if (*end || d < 40 || d > 130)
        return -1;
    *field = (int)d;
    return 0;
}

static int apply(struct sieos_power_set *ps)
{
    int fd = open_power();
    if (fd < 0)
        return -1;
    int r = ioctl(fd, SIEOS_POWER_SET, ps);
    int e = errno;
    close(fd);
    errno = e;
    return r;
}

/* /etc/power.conf: the settings kept, applied over what is there now. */
static void load(struct sieos_power_set *ps)
{
    ps->dps_policy = ps->dps_passive = ps->dps_critical = -1;
    FILE *f = fopen(CONF, "r");
    if (!f)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && line[0] != '#' && parse(line, ps) < 0)
            fprintf(stderr, "poweradm: %s: ignored: %s\n", CONF, line);
    }
    fclose(f);
}

static int save(const struct sieos_power_set *ps)
{
    FILE *f = fopen(CONF ".new", "w");
    if (!f)
        return -1;
    fprintf(f, "# power.conf - power management settings, kept by poweradm(1M)\n");
    if (ps->dps_policy >= 0)
        fprintf(f, "policy=%s\n", policies[ps->dps_policy]);
    if (ps->dps_passive > 0)
        fprintf(f, "passive=%d\n", ps->dps_passive);
    if (ps->dps_critical > 0)
        fprintf(f, "critical=%d\n", ps->dps_critical);
    if (fclose(f) < 0)
        return -1;
    chmod(CONF ".new", 0644);
    return rename(CONF ".new", CONF);
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return show();
    if (!strcmp(argv[1], "-r")) {
        struct sieos_power_set ps;
        load(&ps);
        if (apply(&ps) < 0) {
            perror("poweradm: applying " CONF);
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "set") || argc < 3) {
        fprintf(stderr, "usage: poweradm | poweradm set policy=performance|balanced|powersave "
                        "[passive=C|default] [critical=C|default] | poweradm -r\n");
        return 2;
    }
    struct sieos_power_set change = { -1, -1, -1 };
    for (int i = 2; i < argc; i++)
        if (parse(argv[i], &change) < 0) {
            fprintf(stderr, "poweradm: bad setting: %s\n", argv[i]);
            return 2;
        }
    if (getuid() != 0 && (change.dps_passive >= 0 || change.dps_critical >= 0)) {
        fprintf(stderr, "poweradm: only root sets the thermal thresholds\n");
        return 1;
    }
    if (apply(&change) < 0) {
        perror("poweradm");
        return 1;
    }
    struct sieos_power_set kept;                 /* the file, with this change */
    load(&kept);
    if (change.dps_policy >= 0)
        kept.dps_policy = change.dps_policy;
    if (change.dps_passive >= 0)
        kept.dps_passive = change.dps_passive;
    if (change.dps_critical >= 0)
        kept.dps_critical = change.dps_critical;
    if (save(&kept) < 0)
        perror("poweradm: " CONF);
    return 0;
}
