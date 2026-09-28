/*
 * sys.c - libsieos: the SIEOS system-call extensions (cpuinfo, meminfo,
 * procinfo, netinfo, netstat, fbmap) and small system helpers.
 */
#include "sieos.h"
#include <sys/syscall.h>
#include <sys/uadmin.h>
#include "sieos/syscall.h"
#include "sieos/stat.h"
#include "sieos/sysinfo.h"

_Static_assert(sizeof(struct cpuinfo) == sizeof(struct sieos_cpuinfo), "cpuinfo");
_Static_assert(sizeof(struct meminfo) == sizeof(struct sieos_meminfo), "meminfo");
_Static_assert(sizeof(struct procinfo) == sizeof(struct sieos_procinfo), "procinfo");
_Static_assert(sizeof(struct netinfo) == sizeof(struct sieos_netinfo), "netinfo");
_Static_assert(sizeof(struct sockinfo) == sizeof(struct sieos_sockinfo), "sockinfo");
_Static_assert(sizeof(struct fb_info) == sizeof(struct sieos_fb_info), "fb_info");
_Static_assert(sizeof(struct input_event) == sizeof(struct sieos_input_event), "input_event");
_Static_assert(FBIOGET_INFO == SIEOS_FBIOGET_INFO && EV_KEY == SIEOS_EV_KEY && KEY_F1 == SIEOS_KEY_F1, "codes");
_Static_assert(PSTATE_STOPPED == SIEOS_PSTATE_STOPPED && TCP_TIME_WAIT == SIEOS_TCPS_TIME_WAIT, "states");

int cpuinfo(struct cpuinfo *buf, int max)
{
    return syscall(SIEOS_SYS_cpuinfo, buf, max);
}

int meminfo(struct meminfo *mi)
{
    return syscall(SIEOS_SYS_meminfo, mi);
}

int procinfo(struct procinfo *buf, int max)
{
    return syscall(SIEOS_SYS_procinfo, buf, max);
}

int netinfo(struct netinfo *ni)
{
    return syscall(SIEOS_SYS_netinfo, ni);
}

int netstat(struct sockinfo *buf, int max)
{
    return syscall(SIEOS_SYS_netstat, buf, max);
}

void *fbmap(int fd)
{
    long r = syscall(SIEOS_SYS_fbmap, fd);
    return r == -1 ? MAP_FAILED : (void *)r;
}

int fsinfo(struct fsinfo *fi)
{
    struct sieos_statvfs sv;
    if (syscall(SIEOS_SYS_statvfs, "/", &sv) < 0)
        return -1;
    memset(fi, 0, sizeof(*fi));
    snprintf(fi->volname, sizeof(fi->volname), "%s", sv.f_fstr);
    fi->block_size = sv.f_frsize;
    fi->total_blocks = sv.f_blocks;
    fi->free_blocks = sv.f_bfree;
    fi->total_inodes = sv.f_files;
    fi->free_inodes = sv.f_ffree;
    return 0;
}

long uptime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int msleep(unsigned long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    while (nanosleep(&ts, &ts) < 0)
        if (errno != EINTR)
            return -1;
    return 0;
}

int sieos_reboot(int mode)
{
    sync();
    return uadmin(A_SHUTDOWN, mode == REBOOT_RESTART ? AD_BOOT : AD_HALT, 0);
}

int strftime_simple(char *buf, size_t n, time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    return snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                    tm.tm_hour, tm.tm_min, tm.tm_sec);
}

const char *signame(int sig)
{
    static char buf[SIG2STR_MAX];
    return sig2str(sig, buf) == 0 ? buf : "?";
}

int signum(const char *name)
{
    int sig;
    if (strncmp(name, "SIG", 3) == 0)
        name += 3;
    if (isdigit((unsigned char)*name))
        return atoi(name);
    char up[SIG2STR_MAX];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(up); i++)
        up[i] = toupper((unsigned char)name[i]);
    up[i] = 0;
    return str2sig(up, &sig) == 0 ? sig : -1;
}

int read_line_fd(int fd, char *buf, size_t size)
{
    size_t n = 0;
    char c;
    long r;
    while ((r = read(fd, &c, 1)) == 1) {
        if (c == '\n')
            break;
        if (n + 1 < size)
            buf[n++] = c;
    }
    buf[n] = 0;
    return (r <= 0 && n == 0) ? -1 : (int)n;
}
