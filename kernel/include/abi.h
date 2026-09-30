/*
 * abi.h - Definitions shared between the kernel and user space.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_ABI_H
#define SIEOS_ABI_H

/* System call numbers (int 0x80, number in rax, args rdi rsi rdx r10 r8) */
#define SYS_exit        0
#define SYS_fork        1
#define SYS_exec        2
#define SYS_waitpid     3
#define SYS_read        4
#define SYS_write       5
#define SYS_open        6
#define SYS_close       7
#define SYS_getdents    8
#define SYS_stat        9
#define SYS_fstat       10
#define SYS_chdir       11
#define SYS_getcwd      12
#define SYS_mkdir       13
#define SYS_unlink      14
#define SYS_rmdir       15
#define SYS_dup2        16
#define SYS_getpid      17
#define SYS_sbrk        18
#define SYS_lseek       19
#define SYS_time        20
#define SYS_procinfo    21
#define SYS_meminfo     22
#define SYS_kill        23
#define SYS_sleep       24
#define SYS_uname       25
#define SYS_reboot      26
#define SYS_getppid     27
#define SYS_uptime      28
#define SYS_sync        29
#define SYS_fsinfo      30
/* 31 unused (was setfg, replaced by ioctl(TIOCSPGRP)) */
#define SYS_getuid      32
#define SYS_geteuid     33
#define SYS_getgid      34
#define SYS_getegid     35
#define SYS_setuid      36
#define SYS_setgid      37
#define SYS_seteuid     38
#define SYS_setegid     39
#define SYS_getgroups   40
#define SYS_setgroups   41
#define SYS_umask       42
#define SYS_chmod       43
#define SYS_fchmod      44
#define SYS_chown       45
#define SYS_access      46
#define SYS_setpgid     47
#define SYS_getpgid     48
#define SYS_setsid      49
#define SYS_getsid      50
#define SYS_sigaction   51
#define SYS_sigprocmask 52
#define SYS_sigreturn   53
#define SYS_pipe        54
#define SYS_ioctl       55
#define SYS_pause       56
#define SYS_dup         57
#define SYS_sigpending  58
#define SYS_rename      59
#define SYS_cpuinfo     60
#define SYS_poll        61
#define SYS_openpty     62
#define SYS_fbmap       63
#define SYS_socket      64
#define SYS_bind        65
#define SYS_listen      66
#define SYS_accept      67
#define SYS_connect     68
#define SYS_sendto      69
#define SYS_recvfrom    70
#define SYS_shutdown    71
#define SYS_getsockname 72
#define SYS_getpeername 73
#define SYS_setsockopt  74
#define SYS_netinfo     75
#define SYS_netstat     76
#define NSYSCALLS       77

/* errno values (returned negated) */
#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define ENXIO    6
#define E2BIG    7
#define ENOEXEC  8
#define EBADF    9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EBUSY   16
#define EEXIST  17
#define EXDEV   18
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOTTY  25
#define EFBIG   27
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define EPIPE   32
#define ERANGE  34
#define ENAMETOOLONG 36
#define ENOSYS  38
#define ENOTEMPTY 39
#define ELOOP   40
#define ENOTSOCK     88
#define EDESTADDRREQ 89
#define EMSGSIZE     90
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP   95
#define EAFNOSUPPORT 97
#define EADDRINUSE   98
#define EADDRNOTAVAIL 99
#define ENETDOWN    100
#define ENETUNREACH 101
#define ECONNABORTED 103
#define ECONNRESET  104
#define ENOBUFS     105
#define EISCONN     106
#define ENOTCONN    107
#define ETIMEDOUT   110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EALREADY    114
#define EINPROGRESS 115
#define ERESTART 512           /* kernel internal: restart the system call */

/* open() flags */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_ACCMODE  0x0003
#define O_CREAT    0x0040
#define O_EXCL     0x0080
#define O_NOCTTY   0x0100
#define O_TRUNC    0x0200
#define O_APPEND   0x0400
#define O_DIRECTORY 0x10000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* access() modes */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

/* File types and permission bits in st_mode (same as ext4 / POSIX) */
#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

#define MAJOR(d) (((d) >> 8) & 0xFFF)
#define MINOR(d) ((d) & 0xFF)
#define MKDEV(ma, mi) (((ma) << 8) | (mi))

struct stat {
    unsigned long st_ino;
    unsigned int  st_mode;
    unsigned int  st_nlink;
    unsigned int  st_uid;
    unsigned int  st_gid;
    unsigned long st_size;
    unsigned long st_blocks;     /* 512-byte blocks */
    long          st_atime;
    long          st_mtime;
    long          st_ctime;
    unsigned int  st_rdev;       /* device number for special files */
    unsigned int  st_pad;
};

/* Directory entry returned by getdents() */
#define DT_UNKNOWN 0
#define DT_REG     1
#define DT_DIR     2
#define DT_CHR     3
#define DT_BLK     4
#define DT_FIFO    5
#define DT_SOCK    6
#define DT_LNK     7

struct dirent {
    unsigned int  d_ino;
    unsigned short d_reclen;     /* size of this record */
    unsigned char d_type;
    unsigned char d_namlen;
    char          d_name[256];
};

/* Process information returned by procinfo() */
#define PSTATE_UNUSED   0
#define PSTATE_EMBRYO   1
#define PSTATE_RUNNABLE 2
#define PSTATE_RUNNING  3
#define PSTATE_SLEEPING 4
#define PSTATE_ZOMBIE   5
#define PSTATE_STOPPED  6

struct procinfo {
    int  pid;
    int  ppid;
    int  pgid;
    int  sid;
    int  uid;
    int  euid;
    int  state;
    int  tty_fg;                 /* 1 if in the terminal's foreground group */
    int  cpu;                    /* CPU it is running on, -1 if not running */
    int  pad;
    unsigned long mem_kb;        /* resident user memory */
    unsigned long ticks;         /* cpu ticks consumed */
    char name[32];
};

struct cpuinfo {
    int id;
    int apic_id;
    int online;
    int pid;                     /* process currently running (0 = idle) */
    unsigned long busy_ticks;
    unsigned long idle_ticks;
};

struct meminfo {
    unsigned long total_kb;
    unsigned long free_kb;
    unsigned long kernel_kb;
    unsigned long heap_kb;       /* kernel heap in use */
};

struct utsname {
    char sysname[32];
    char nodename[32];
    char release[32];
    char version[64];
    char machine[32];
};

struct fsinfo {
    char          volname[16];
    unsigned long block_size;
    unsigned long total_blocks;
    unsigned long free_blocks;
    unsigned long total_inodes;
    unsigned long free_inodes;
};

#define REBOOT_HALT    0
#define REBOOT_RESTART 1

/* ------------------------------------------------------------------ */
/* Signals                                                             */
/* ------------------------------------------------------------------ */

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGWINCH 28
#define SIGSYS   31
#define NSIG     32

typedef unsigned long sigset_t;
#define SIGBIT(s) (1UL << (s))

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

#define SA_NOCLDSTOP 0x00000001
#define SA_RESTORER  0x04000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

struct sigaction {
    void (*sa_handler)(int);
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
};

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* waitpid() options and status decoding */
#define WNOHANG    1
#define WUNTRACED  2
#define WCONTINUED 8

#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WSTOPSIG(s)     WEXITSTATUS(s)
#define WIFEXITED(s)    (WTERMSIG(s) == 0)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WIFSIGNALED(s)  (WTERMSIG(s) != 0 && WTERMSIG(s) != 0x7f)
#define WIFCONTINUED(s) ((s) == 0xffff)
#define WCOREDUMP(s)    ((s) & 0x80)

/* ------------------------------------------------------------------ */
/* Terminal interface                                                  */
/* ------------------------------------------------------------------ */

#define NCCS 20
struct termios {
    unsigned int c_iflag;
    unsigned int c_oflag;
    unsigned int c_cflag;
    unsigned int c_lflag;
    unsigned char c_cc[NCCS];
};

/* c_iflag */
#define ICRNL  0x0100
/* c_oflag */
#define OPOST  0x0001
#define ONLCR  0x0004
/* c_lflag */
#define ISIG   0x0001
#define ICANON 0x0002
#define ECHO   0x0008
#define ECHOE  0x0010
#define ECHOK  0x0020
#define ECHONL 0x0040
#define NOFLSH 0x0080
#define TOSTOP 0x0100
#define ECHOCTL 0x0200
#define IEXTEN 0x8000
/* c_cc indices */
#define VINTR  0
#define VQUIT  1
#define VERASE 2
#define VKILL  3
#define VEOF   4
#define VTIME  5
#define VMIN   6
#define VSUSP  10

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* ioctl requests */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCSCTTY  0x540E
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCGWINSZ 0x5413
#define TIOCNOTTY  0x5422
#define TIOCSWINSZ 0x5414

/* poll() */
struct pollfd {
    int fd;
    short events;
    short revents;
};
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

/* Framebuffer device (/dev/fb0) */
#define FBIOGET_INFO 0x4600
struct fb_info {
    unsigned int width, height;
    unsigned int pitch;          /* bytes per line */
    unsigned int bpp;            /* 32 */
    unsigned int red_pos, green_pos, blue_pos;
    unsigned int pad;
};

/* Input events (/dev/events): reading grabs keyboard and mouse */
#define EV_KEY   1
#define EV_MOUSE 2              /* relative motion in dx, dy */
#define EV_MOUSE_ABS 3          /* absolute position: dx, dy in 0..65535 */
struct input_event {
    unsigned short type;
    unsigned short code;         /* EV_KEY: key code (KEY_* or scancode) */
    int value;                   /* EV_KEY: 1 press, 0 release */
    int dx, dy;                  /* EV_MOUSE: relative motion (dy > 0 = down) */
    unsigned int buttons;        /* EV_MOUSE: bit0 left, bit1 right, bit2 middle */
    unsigned int ascii;          /* EV_KEY: translated character, 0 if none */
    unsigned int mods;           /* MOD_* */
};
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
/* key codes for non-character keys */
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

/* Device numbers */
#define DEV_MEM_MAJOR  1       /* 1,3 = null  1,5 = zero  1,8 = random  1,9 = urandom */
#define DEV_TTY_MAJOR  5       /* 5,0 = tty (controlling)  5,1 = console */
#define DEV_INPUT_MAJOR 13     /* 13,0 = events */
#define DEV_FB_MAJOR   29      /* 29,0 = fb0 */
#define DEV_PTS_MAJOR  136     /* 136,n = /dev/pts/n (5,2 = /dev/ptmx) */
#define DEV_BLK_MAJOR  8       /* block devices: 8,n = blkdev n (see blkdev.h) */
#define DEV_LOFI_MAJOR 147     /* 147,0 = /dev/lofictl */
#define DEV_POWER_MAJOR 181    /* 181,0 = /dev/power */

#define NGROUPS_MAX 16

/* ------------------------------------------------------------------ */
/* Networking                                                          */
/* ------------------------------------------------------------------ */

#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define SOCK_RAW     3
#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

#define INADDR_ANY       0x00000000U
#define INADDR_BROADCAST 0xFFFFFFFFU
#define INADDR_LOOPBACK  0x7F000001U   /* host byte order */

struct sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;     /* network byte order */
    unsigned int   sin_addr;     /* network byte order */
    unsigned char  sin_zero[8];
};

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_RCVTIMEO  20              /* value: int milliseconds */
#define SO_SNDTIMEO  21

#define MSG_DONTWAIT 0x40

struct netinfo {
    char name[8];                /* "eth0" */
    int up;                      /* link configured */
    int dhcp;                    /* address obtained by DHCP */
    unsigned char mac[6];
    unsigned char pad[2];
    unsigned int ip, netmask, gateway, dns;   /* host byte order */
    unsigned long rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
    char driver[24];
};

/* TCP states (also used by netstat) */
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
    int proto;                   /* IPPROTO_TCP / UDP / ICMP */
    int state;                   /* TCP state, 0 for others */
    unsigned int lip, rip;       /* host byte order */
    unsigned short lport, rport;
    unsigned int rxq, txq;       /* queued bytes */
    int uid;
    int pad;
};

#endif
