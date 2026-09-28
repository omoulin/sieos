/* sifetch - system summary with the SIEOS logo */
#include "sieos.h"

static const char *logo[] = {
    "      .----------.      ",
    "     /  .-.  .-.  \\     ",
    "    |  ( o )( o )  |    ",
    "    |   '-'  '-'   |    ",
    "    |    SIEOS     |    ",
    "     \\  '------'  /     ",
    "      '----------'      ",
    "     __|__|__|__|__     ",
    "    [ neural  core ]    ",
    "                        ",
};

int main(void)
{
    struct utsname u;
    struct meminfo mi;
    struct fsinfo fi;
    struct procinfo p[64];
    uname(&u);
    meminfo(&mi);
    fsinfo(&fi);
    int nall = procinfo(p, 64), nproc = 0;
    for (int i = 0; i < nall; i++)
        nproc += p[i].pid != 0;
    const char *user = getenv("USER");
    if (!user) {
        struct passwd *pw = getpwuid(geteuid());
        user = pw ? pw->pw_name : "?";
    }
    long up = uptime_ms() / 1000;
    char info[11][80];
    int n = 0;
    snprintf(info[n++], 80, "\033[1;36m%s\033[0m@\033[1;36msieos\033[0m", user);
    snprintf(info[n++], 80, "----------");
    snprintf(info[n++], 80, "\033[1;33mOS\033[0m:      %s - Synthetic Intelligence Enhanced Operating System", u.sysname);
    snprintf(info[n++], 80, "\033[1;33mKernel\033[0m:  %s %s", u.release, u.machine);
    snprintf(info[n++], 80, "\033[1;33mUptime\033[0m:  %ldh %ldm %lds", up / 3600, up / 60 % 60, up % 60);
    struct cpuinfo ci[16];
    int ncpu = cpuinfo(ci, 16);
    snprintf(info[n++], 80, "\033[1;33mCPUs\033[0m:    %d x86_64 core%s (SMP)", ncpu, ncpu == 1 ? "" : "s");
    snprintf(info[n++], 80, "\033[1;33mMemory\033[0m:  %lu MiB / %lu MiB",
             (mi.total_kb - mi.free_kb) / 1024, mi.total_kb / 1024);
    snprintf(info[n++], 80, "\033[1;33mDisk\033[0m:    %lu MiB / %lu MiB (ext4)",
             (fi.total_blocks - fi.free_blocks) * fi.block_size / 1048576,
             fi.total_blocks * fi.block_size / 1048576);
    snprintf(info[n++], 80, "\033[1;33mProcs\033[0m:   %d", nproc);
    printf("\n");
    for (int i = 0; i < 10; i++)
        printf("\033[1;34m%s\033[0m  %s\n", logo[i], i < n ? info[i] : "");
    for (int i = 10; i < n; i++)
        printf("%24s  %s\n", "", info[i]);
    return 0;
}
