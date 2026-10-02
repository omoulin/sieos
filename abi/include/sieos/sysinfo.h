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
#define SIEOS_A_NETTEST  0x5e06   /* testing: loopback packets dropped and reordered, fcn =
                                     drop per mille | reorder per mille << 16 (0: off) */
#define SIEOS_A_JTEST    0x5e05   /* testing: power off right after the next journal commit record
                                     (before the checkpoint), to exercise recovery */
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

/* lwp_affinity ops */
#define SIEOS_AFF_GET     0
#define SIEOS_AFF_SET     1

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

/* SIEOS extension: IPv6 on the interface (netinfo6) */
#define SIEOS_NET6_ADDRS 4
/* netconfig(): DHCP, or a static address (host byte order, as struct netinfo). */
struct sieos_netconfig {
    int nc_index;                        /* 0 eth0, 1 eth1, ... */
    int nc_dhcp;                         /* 1: ask DHCP (the others are ignored) */
    unsigned int nc_ip, nc_netmask, nc_gateway, nc_dns;
};

struct sieos_netinfo6 {
    int up;                         /* link-local address configured */
    int naddr;                      /* entries used in addr[] */
    struct {
        sieos_uint8_t addr[16];
        int prefixlen;
        int flags;                  /* SIEOS_NET6_* */
    } addr[SIEOS_NET6_ADDRS];
    sieos_uint8_t router[16], dns[16];    /* from router advertisements (zero if none) */
    unsigned int mtu;
    int hoplimit;
    unsigned long rx_packets, tx_packets;
};
#define SIEOS_NET6_LINKLOCAL 1
#define SIEOS_NET6_AUTOCONF  2      /* stateless autoconfiguration (RFC 4862) */

/* SIEOS extension: the devices (devinfo): the PCI functions found at boot */
struct sieos_devinfo {
    char bus[8];                    /* "pci" */
    char location[16];              /* "0000:00:02.0" (domain:bus:device.function) */
    sieos_uint16_t vendor, device, subvendor, subdevice;
    sieos_uint8_t class_code, subclass, prog_if, revision;
    int irq;                        /* -1 if none */
    char driver[24];                /* the driver using it, "" if none */
};

/* SIEOS extension: IPv4 and IPv6 sockets (netstat6) */
struct sieos_sockinfo6 {
    int family;                     /* SIEOS_AF_INET or SIEOS_AF_INET6 */
    int proto;                      /* SIEOS_IPPROTO_TCP / UDP / ICMP / ICMPV6 */
    int state;                      /* SIEOS_TCPS_*, 0 for others */
    unsigned short lport, rport;
    sieos_uint8_t laddr[16], raddr[16];   /* IPv4 ones mapped (::ffff:a.b.c.d) */
    unsigned int rxq, txq;
    int uid;
    int pad;
};

/* SIEOS extension: sockets (netstat; IPv4 endpoints only) */
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

/* SIEOS extension: displays (/dev/fb0, /dev/fb1, ...), mapped with fbmap().
 * The mapping covers the whole memory the display can scan out (vram), so
 * it stays valid when the mode changes; FBIOGET_INFO gives the new layout. */
#define SIEOS_FBIOGET_INFO 0x4600   /* ioctl(fd, SIEOS_FBIOGET_INFO, struct sieos_fb_info *) */
struct sieos_fb_info {
    unsigned int width, height;
    unsigned int pitch;             /* bytes per line */
    unsigned int bpp;               /* 32 */
    unsigned int red_pos, green_pos, blue_pos;
    unsigned int pad;
};
#define SIEOS_FBIOGET_DISPLAY 0x4601    /* struct sieos_fb_display * */
struct sieos_fb_display {
    char driver[24];                /* "firmware", "bochs-vbe", "intel-gen12" */
    char desc[48];                  /* the device, e.g. "QEMU standard VGA, 16 MiB" */
    unsigned int index;             /* N of /dev/fbN */
    unsigned int flags;             /* SIEOS_FB_* */
    unsigned int nmodes;
    unsigned int owner;             /* the process that has it mapped, 0 none */
    unsigned long vram;             /* bytes fbmap maps */
};
#define SIEOS_FB_SETMODE 1          /* the mode can be changed (FBIOSET_MODE) */
#define SIEOS_FB_CONSOLE 2          /* the text console draws here */
#define SIEOS_FB_MODES_MAX 32
struct sieos_fb_mode {
    unsigned short width, height;
    unsigned short refresh;         /* Hz, 0 if unknown */
    unsigned short flags;           /* SIEOS_FB_MODE_* */
};
#define SIEOS_FB_MODE_CURRENT   1
#define SIEOS_FB_MODE_PREFERRED 2   /* the monitor's native mode */
#define SIEOS_FBIOGET_MODES 0x4602  /* struct sieos_fb_modes * */
struct sieos_fb_modes {
    unsigned int n, pad;
    struct sieos_fb_mode mode[SIEOS_FB_MODES_MAX];
};
/* width and height of struct sieos_fb_mode: EINVAL (not a mode of the
 * display), ENOTSUP (fixed mode), EBUSY (another process has it mapped) */
#define SIEOS_FBIOSET_MODE 0x4603

/* SIEOS extension: keyboard and mouse events, read from /dev/events */
#define SIEOS_EV_KEY       1
#define SIEOS_EV_MOUSE     2        /* relative motion in dx, dy */
#define SIEOS_EV_MOUSE_ABS 3        /* absolute position: dx, dy in 0..65535 */
#define SIEOS_EV_WHEEL     4        /* the wheel turned: value notches, > 0 down (towards the user) */
struct sieos_input_event {
    unsigned short type;
    unsigned short code;            /* EV_KEY: key code (SIEOS_KEY_* or scancode) */
    int value;                      /* EV_KEY: 1 press, 0 release; EV_WHEEL: notches */
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

/* SIEOS extension: the Wi-Fi device (wifi()) */
#define SIEOS_WIFI_OP_STATUS  0          /* buf: struct sieos_wifi_status */
#define SIEOS_WIFI_OP_SCAN    1          /* start a scan (EBUSY while one runs); the results come in a few seconds */
#define SIEOS_WIFI_OP_RESULTS 2          /* buf: struct sieos_wifi_bss[n]; returns the number filled */
#define SIEOS_WIFI_OP_CONNECT 3          /* buf: struct sieos_wifi_connect (root): join a network of the last scan */
#define SIEOS_WIFI_OP_DISCONNECT 4       /* leave the network (root) */
#define SIEOS_WIFI_OP_POWER   5          /* n: the power mode, 0 off, 1 fast (the default), 2 max (root) */

#define SIEOS_WIFI_NONE      0           /* no Wi-Fi device */
#define SIEOS_WIFI_DOWN      1           /* the device or its firmware failed (ws_info says why) */
#define SIEOS_WIFI_READY     2           /* the firmware runs, not joined to a network */
#define SIEOS_WIFI_SCANNING  3
#define SIEOS_WIFI_JOINING   4
#define SIEOS_WIFI_JOINED    5

struct sieos_wifi_status {
    int ws_state;                        /* SIEOS_WIFI_* */
    char ws_info[64];                    /* the driver's state, readable */
    sieos_uint8_t ws_mac[6];
    char ws_ssid[34];                    /* the network joined ("" none) */
    int ws_nbss;                         /* networks the last scan found */
    unsigned int ws_scans;               /* scans completed since boot */
    int ws_link;                         /* the interface index when joined, else -1 */
    int ws_power;                        /* the power mode: 0 off, 1 fast, 2 max */
    int ws_mode;                         /* joined with: 1 802.11a/g, 2 802.11n, 3 802.11ac (0: not joined) */
    int ws_width;                        /* the channel width, MHz */
    int ws_streams;                      /* spatial streams */
};

/* Joining: the network by its SSID (the strongest of the last scan), or by its BSSID if not zero; the key: a
 * passphrase (8-63 characters) or 64 hexadecimal digits (the PSK). ENOENT: not heard; EOPNOTSUPP: a security
 * other than WPA2-Personal (CCMP) or open; EINVAL: the key. The result comes later, in the status. */
struct sieos_wifi_connect {
    char wc_ssid[34];
    sieos_uint8_t wc_bssid[6];
    char wc_key[66];
    char wc_reserved[2];
};

/* A network (BSS) seen by a scan. */
#define SIEOS_WIFI_SEC_WEP   0x01        /* privacy without WPA/RSN information */
#define SIEOS_WIFI_SEC_WPA   0x02        /* WPA (the vendor element) */
#define SIEOS_WIFI_SEC_RSN   0x04        /* WPA2/WPA3 (the RSN element) */
#define SIEOS_WIFI_SEC_PSK   0x10        /* key management: pre-shared key */
#define SIEOS_WIFI_SEC_8021X 0x20        /* 802.1X (enterprise) */
#define SIEOS_WIFI_SEC_SAE   0x40        /* SAE (WPA3 personal) */
struct sieos_wifi_bss {
    sieos_uint8_t wb_bssid[6];
    char wb_ssid[34];                    /* "" for a hidden network */
    int wb_channel;
    int wb_rssi;                         /* dBm */
    unsigned int wb_sec;                 /* SIEOS_WIFI_SEC_* (0: open) */
    unsigned int wb_age;                 /* seconds since it was last heard */
};

/* SIEOS extension: a driver (modinfo): the boot archive's and /drv's, loaded or not */
#define SIEOS_MOD_KNOWN   0              /* not loaded: no device of its */
#define SIEOS_MOD_LOADED  1
#define SIEOS_MOD_FAILED  2              /* loading or its _init failed */
struct sieos_modinfo {
    char mi_name[32];
    char mi_desc[96];
    int mi_state;                        /* SIEOS_MOD_* */
    int mi_phase;                        /* 0 display, 1 boot, 2 after the root */
    unsigned long mi_base;               /* where it is loaded (kernel address) */
    unsigned int mi_size;                /* bytes */
    char mi_source[16];                  /* "boot archive", "/drv", "modload" */
    char mi_alias[64];                   /* the alias its first device matched ("pci8086,34f0") */
    int mi_ndev;                         /* devices matched */
};

SIEOS_STATIC_ASSERT(sizeof(sieos_processor_info_t) == 56, "processor_info size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_sockinfo) == 36, "sockinfo size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_sockinfo6) == 64, "sockinfo6 size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_netinfo6) == 160, "netinfo6 size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_devinfo) == 64, "devinfo size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_procinfo) == 88, "procinfo size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_input_event) == 28, "input_event size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_wifi_status) == 136, "wifi_status size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_wifi_bss) == 56, "wifi_bss size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_wifi_connect) == 108, "wifi_connect size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_modinfo) == 232, "modinfo size");

#endif
