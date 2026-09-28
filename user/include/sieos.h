/*
 * sieos.h - The SIEOS userland on the C library (musl, ABI v2): the
 * standard headers the programs use, and libsieos, the SIEOS extensions
 * (processor, memory and process tables, the network interface, the
 * framebuffer and input events) and a few shared helpers.
 */
#ifndef SIEOS_H
#define SIEOS_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>

/* ---- processors, memory, processes (SIEOS extensions) ---- */
struct cpuinfo {
    int id;
    int apic_id;
    int online;
    int pid;                        /* process running on it, 0 when idle */
    unsigned long busy_ticks;
    unsigned long idle_ticks;
};

struct meminfo {
    unsigned long total_kb;
    unsigned long free_kb;
    unsigned long kernel_kb;
    unsigned long heap_kb;
};

#define PSTATE_UNUSED   0
#define PSTATE_EMBRYO   1
#define PSTATE_RUNNABLE 2
#define PSTATE_RUNNING  3
#define PSTATE_SLEEPING 4
#define PSTATE_ZOMBIE   5
#define PSTATE_STOPPED  6
struct procinfo {
    int pid, ppid, pgid, sid, uid, euid;
    int state;                      /* PSTATE_* */
    int tty_fg;
    int cpu;                        /* -1 when not running */
    int pad;
    unsigned long mem_kb;
    unsigned long ticks;
    char name[32];
};

#define TICKS_PER_SEC 100           /* busy_ticks, idle_ticks, procinfo.ticks */

int cpuinfo(struct cpuinfo *buf, int max);      /* processors filled, -1 */
int meminfo(struct meminfo *mi);
int procinfo(struct procinfo *buf, int max);    /* processes filled, -1 */

/* The root file system (statvfs on "/"). */
struct fsinfo {
    char volname[32];
    unsigned long block_size;
    unsigned long total_blocks;
    unsigned long free_blocks;
    unsigned long total_inodes;
    unsigned long free_inodes;
};
int fsinfo(struct fsinfo *fi);

/* ---- the network interface and sockets (SIEOS extensions) ---- */
struct netinfo {
    char name[8];                   /* "eth0" */
    int up;
    int dhcp;
    unsigned char mac[6];
    unsigned char pad[2];
    unsigned int ip, netmask, gateway, dns;     /* host byte order */
    unsigned long rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
    char driver[24];
};

#define TCP_CLOSED       0
#define TCP_LISTEN       1
#define TCP_SYN_SENT     2
#define TCP_SYN_RCVD     3
#define TCP_ESTABLISHED  4
#define TCP_FIN_WAIT_1   5
#define TCP_FIN_WAIT_2   6
#define TCP_CLOSE_WAIT   7
#define TCP_CLOSING      8
#define TCP_LAST_ACK     9
#define TCP_TIME_WAIT    10
struct sockinfo {
    int proto;                      /* IPPROTO_TCP / UDP / ICMP */
    int state;                      /* TCP_*, 0 for others */
    unsigned int lip, rip;          /* host byte order */
    unsigned short lport, rport;
    unsigned int rxq, txq;
    int uid;
    int pad;
};

int netinfo(struct netinfo *ni);
int netstat(struct sockinfo *buf, int max);     /* sockets filled, -1 */

/* Name or dotted quad -> IPv4 address (network order): /etc/hosts, then
 * the DNS server the interface was configured with.  0, or -1. */
int resolve_host(const char *name, unsigned int *addr);
struct sockaddr_in make_addr(unsigned int addr_net, unsigned short port);
const char *ip_to_str(unsigned int host_order_ip, char *buf16);

/* ---- the framebuffer and input (SIEOS extensions) ---- */
#define FBIOGET_INFO 0x4600
struct fb_info {
    unsigned int width, height;
    unsigned int pitch;             /* bytes per line */
    unsigned int bpp;               /* 32 */
    unsigned int red_pos, green_pos, blue_pos;
    unsigned int pad;
};
void *fbmap(int fd);                /* map /dev/fb0; MAP_FAILED on failure */

#define EV_KEY       1
#define EV_MOUSE     2
#define EV_MOUSE_ABS 3
struct input_event {
    unsigned short type;
    unsigned short code;
    int value;
    int dx, dy;
    unsigned int buttons;
    unsigned int ascii;
    unsigned int mods;
};
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
#define KEY_UP     0x148
#define KEY_DOWN   0x150
#define KEY_LEFT   0x14B
#define KEY_RIGHT  0x14D
#define KEY_HOME   0x147
#define KEY_END    0x14F
#define KEY_PGUP   0x149
#define KEY_PGDN   0x151
#define KEY_DELETE 0x153
#define KEY_F1     0x03B

/* ---- time, system ---- */
long uptime_ms(void);               /* milliseconds since boot */
int  msleep(unsigned long ms);
#define REBOOT_HALT    0
#define REBOOT_RESTART 1
int  sieos_reboot(int mode);        /* uadmin; root only */
int  strftime_simple(char *buf, size_t n, time_t t);   /* "YYYY-MM-DD HH:MM:SS" UTC */

/* ---- signals ---- */
const char *signame(int sig);       /* "INT", "TERM", ...; "?" if unknown */
int  signum(const char *name);      /* inverse of signame (with or without "SIG"), -1 */

/* ---- passwords: "$5a$<salt>$<hex>" (see libsieos/crypt.c) ---- */
char *crypt_password(const char *password, const char *salt);
bool  check_password(const char *password, const char *hash);
void  sha256(const void *data, size_t len, uint8_t out[32]);

/* ---- misc ---- */
int  read_line_fd(int fd, char *buf, size_t size);      /* -1 on EOF with no data */

#endif
