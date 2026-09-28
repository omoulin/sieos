/*
 * sieos/sysinfo.h - sysinfo(), sysconfig(), uadmin(), processors (ABI v2).
 * Command numbers follow Solaris.
 */
#ifndef SIEOS_ABI_SYSINFO_H
#define SIEOS_ABI_SYSINFO_H

#include "types.h"

/* sysinfo(): returns the size needed (including NUL) */
#define SIEOS_SI_SYSNAME           1   /* "SIEOS" */
#define SIEOS_SI_HOSTNAME          2
#define SIEOS_SI_RELEASE           3   /* e.g. "0.4" */
#define SIEOS_SI_VERSION           4
#define SIEOS_SI_MACHINE           5   /* "i86pc" */
#define SIEOS_SI_ARCHITECTURE      6   /* "i386" */
#define SIEOS_SI_HW_SERIAL         7
#define SIEOS_SI_HW_PROVIDER       8
#define SIEOS_SI_SRPC_DOMAIN       9
#define SIEOS_SI_SET_HOSTNAME    258   /* root only */
#define SIEOS_SI_SET_SRPC_DOMAIN 265
#define SIEOS_SI_PLATFORM        513
#define SIEOS_SI_ISALIST         514   /* "amd64 pentium_pro+mmx ..." */
#define SIEOS_SI_ARCHITECTURE_64 517   /* "amd64" */
#define SIEOS_SI_ARCHITECTURE_K  518
#define SIEOS_SI_ARCHITECTURE_NATIVE 519

/* sysconfig() names */
#define SIEOS_CONFIG_NGROUPS       2
#define SIEOS_CONFIG_CHILD_MAX     3
#define SIEOS_CONFIG_OPEN_FILES    4
#define SIEOS_CONFIG_POSIX_VER     5
#define SIEOS_CONFIG_PAGESIZE      6
#define SIEOS_CONFIG_CLK_TCK       7
#define SIEOS_CONFIG_XOPEN_VER     8
#define SIEOS_CONFIG_NPROC_CONF   11
#define SIEOS_CONFIG_NPROC_ONLN   15
#define SIEOS_CONFIG_ARG_MAX      20
#define SIEOS_CONFIG_PHYS_PAGES   26
#define SIEOS_CONFIG_AVPHYS_PAGES 27
#define SIEOS_CONFIG_STACK_PROT   33
#define SIEOS_CONFIG_NPROC_MAX    35

/* uadmin() */
#define SIEOS_A_REBOOT   1
#define SIEOS_A_SHUTDOWN 2
#define SIEOS_A_REMOUNT  4
#define SIEOS_AD_HALT     0
#define SIEOS_AD_BOOT     1
#define SIEOS_AD_IBOOT    2
#define SIEOS_AD_POWEROFF 6

/* processors */
#define SIEOS_P_OFFLINE  0x0001
#define SIEOS_P_ONLINE   0x0002
#define SIEOS_P_STATUS   0x0003
#define SIEOS_P_NOINTR   0x0006
#define SIEOS_PBIND_NONE  (-1)
#define SIEOS_PBIND_QUERY (-2)

#define SIEOS_PI_TYPELEN  16
#define SIEOS_PI_FPUTYPE  32
typedef struct {
    int pi_state;                   /* SIEOS_P_ONLINE ... */
    char pi_processor_type[SIEOS_PI_TYPELEN];  /* "i386" */
    char pi_fputypes[SIEOS_PI_FPUTYPE];        /* "i387 compatible" */
    int pi_clock;                   /* MHz */
} sieos_processor_info_t;

/* SIEOS extension: network interface summary (netinfo) */
struct sieos_netinfo {
    char name[8];
    int up, dhcp;
    unsigned char mac[6], pad[2];
    unsigned int ip, netmask, gateway, dns;     /* host byte order */
    unsigned long rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
    char driver[24];
};

/* SIEOS extension: sockets (netstat) */
struct sieos_sockinfo {
    int proto;                      /* SIEOS_IPPROTO_TCP / UDP / ICMP */
    int state;                      /* SIEOS_TCPS_*, 0 for others */
    unsigned int lip, rip;          /* host byte order */
    unsigned short lport, rport;
    unsigned int rxq, txq;          /* queued bytes */
    int uid;
    int pad;
};
#define SIEOS_TCPS_CLOSED      0
#define SIEOS_TCPS_LISTEN      1
#define SIEOS_TCPS_SYN_SENT    2
#define SIEOS_TCPS_SYN_RCVD    3
#define SIEOS_TCPS_ESTABLISHED 4
#define SIEOS_TCPS_FIN_WAIT_1  5
#define SIEOS_TCPS_FIN_WAIT_2  6
#define SIEOS_TCPS_CLOSE_WAIT  7
#define SIEOS_TCPS_CLOSING     8
#define SIEOS_TCPS_LAST_ACK    9
#define SIEOS_TCPS_TIME_WAIT   10

/* SIEOS extension: per-processor load (cpuinfo) */
struct sieos_cpuinfo {
    int id;
    int apic_id;
    int online;
    int pid;                        /* process running on it, 0 when idle */
    unsigned long busy_ticks;
    unsigned long idle_ticks;
};

/* SIEOS extension: memory (meminfo) */
struct sieos_meminfo {
    unsigned long total_kb;
    unsigned long free_kb;
    unsigned long kernel_kb;
    unsigned long heap_kb;          /* kernel heap in use */
};

/* SIEOS extension: process table summary (procinfo) */
#define SIEOS_PSTATE_UNUSED   0
#define SIEOS_PSTATE_EMBRYO   1
#define SIEOS_PSTATE_RUNNABLE 2
#define SIEOS_PSTATE_RUNNING  3
#define SIEOS_PSTATE_SLEEPING 4
#define SIEOS_PSTATE_ZOMBIE   5
#define SIEOS_PSTATE_STOPPED  6
struct sieos_procinfo {
    int pid, ppid, pgid, sid, uid, euid;
    int state;                      /* SIEOS_PSTATE_* */
    int tty_fg;                     /* in its terminal's foreground group */
    int cpu;                        /* processor it runs on, -1 */
    int pad;
    unsigned long mem_kb;           /* resident user memory */
    unsigned long ticks;            /* CPU ticks used */
    char name[32];
};

/* SIEOS extension: the framebuffer (/dev/fb0), mapped with fbmap() */
#define SIEOS_FBIOGET_INFO 0x4600   /* ioctl(fd, SIEOS_FBIOGET_INFO, struct sieos_fb_info *) */
struct sieos_fb_info {
    unsigned int width, height;
    unsigned int pitch;             /* bytes per line */
    unsigned int bpp;               /* 32 */
    unsigned int red_pos, green_pos, blue_pos;
    unsigned int pad;
};

/* SIEOS extension: keyboard and mouse events, read from /dev/events */
#define SIEOS_EV_KEY       1
#define SIEOS_EV_MOUSE     2        /* relative motion in dx, dy */
#define SIEOS_EV_MOUSE_ABS 3        /* absolute position: dx, dy in 0..65535 */
struct sieos_input_event {
    unsigned short type;
    unsigned short code;            /* EV_KEY: key code (SIEOS_KEY_* or scancode) */
    int value;                      /* EV_KEY: 1 press, 0 release */
    int dx, dy;
    unsigned int buttons;           /* bit 0 left, 1 right, 2 middle */
    unsigned int ascii;             /* EV_KEY: the character, 0 if none */
    unsigned int mods;              /* SIEOS_MOD_* */
};
#define SIEOS_MOD_SHIFT 1
#define SIEOS_MOD_CTRL  2
#define SIEOS_MOD_ALT   4
#define SIEOS_KEY_UP     0x148
#define SIEOS_KEY_DOWN   0x150
#define SIEOS_KEY_LEFT   0x14B
#define SIEOS_KEY_RIGHT  0x14D
#define SIEOS_KEY_HOME   0x147
#define SIEOS_KEY_END    0x14F
#define SIEOS_KEY_PGUP   0x149
#define SIEOS_KEY_PGDN   0x151
#define SIEOS_KEY_DELETE 0x153
#define SIEOS_KEY_F1     0x03B

SIEOS_STATIC_ASSERT(sizeof(sieos_processor_info_t) == 56, "processor_info size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_sockinfo) == 36, "sockinfo size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_procinfo) == 88, "procinfo size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_input_event) == 28, "input_event size");

#endif
