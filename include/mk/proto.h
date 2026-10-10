/*
 * mk/proto.h - The message protocols of the system servers: what goes in w[]
 * of a msg_t. The kernel knows nothing of these; only clients and servers do.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

/* Console server, port "console". Reply: w[0] = result (>= 0) or -error. */
enum {
    CON_WRITE = 1,  /* sbuf/slen: text to print.             -> bytes written  */
    CON_READ,       /* w[1] = max size; data comes in rbuf.  -> bytes (0: EOF) */
    CON_ECHO,       /* w[1] = 1: show typed characters, 0: hide (passwords)   */
    CON_INPUT,      /* (root) become the owner of the keyboard and mouse: their
                       events go to the caller (the desktop) instead of the text
                       console (the serial line stays a text console). The reply
                       comes when there are events: con_event_t[] in rbuf -> count */
    CON_HASINPUT,   /* -> 1 if there is a keyboard or pointer (a desktop is usable),
                       0 if not (a Raspberry Pi without a USB keyboard) */
    CON_INJECT,     /* (root: the USB server) input from a USB keyboard or mouse:
                       sbuf = con_inject_t[]; none: "a keyboard or pointer is here" -> 0 */
};
/* What CON_INJECT carries. INJ_KEY: sc is one byte of a set-1 scancode
 * (0xE0 then the key for the extended ones, +0x80 when released), as a PS/2
 * keyboard sends; INJ_REL: a mouse moved by dx, dy (its own units, about a
 * pixel each); INJ_ABS: a tablet's position x, y (0..65535). Both carry the
 * buttons held (MB_...) and the wheel (+ = away from the user). */
typedef struct { uint8_t kind, buttons; int8_t wheel; uint8_t sc; int16_t dx, dy; uint16_t x, y; } con_inject_t;
enum { INJ_KEY = 1, INJ_REL, INJ_ABS };
/* An input event. Keys: code = the character (Ctrl applied), or a KEY_...
 * Mouse: x, y = the pointer's position, 0..65535 across the screen whatever
 * its resolution; buttons = MB_... held; code = the wheel's movement (signed,
 * + = towards the user = scroll down). */
typedef struct { uint8_t type, buttons; uint16_t code, x, y; } con_event_t;
enum { EV_KEY = 1, EV_MOUSE };
enum { MB_LEFT = 1, MB_RIGHT = 2, MB_MIDDLE = 4 };
enum { KEY_UP = 0x101, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_PGUP, KEY_PGDN, KEY_HOME, KEY_END,
       KEY_DEL, KEY_BTAB,            /* KEY_BTAB: Shift+Tab */
       KEY_F1 = 0x111,               /* F1 .. F12: KEY_F1 + 0 .. 11 */
       KEY_CTAB = 0x120,             /* Ctrl+Tab */
       KEY_C1 };                     /* Ctrl+1 .. Ctrl+9: KEY_C1 + 0 .. 8 */

/* Disk driver, port "disk" (vblk). Blocks are 4 KiB. One message moves at
 * most DISK_MAX bytes; the data travels in sbuf (write) or rbuf (read).
 * Reply: w[0] = result (>= 0) or -error. */
#define DISK_BS  4096
#define DISK_MAX (128 << 10)
enum {
    DISK_INFO = 1,  /*                                   -> number of 4 KiB blocks */
    DISK_READ,      /* w[1] = first block, w[2] = count; data comes in rbuf -> 0  */
    DISK_WRITE,     /* w[1] = first block, w[2] = count; data in sbuf       -> 0  */
    DISK_FLUSH,     /* make every write so far durable                      -> 0  */
};

/* The USB server, port "usb" (/bin/usb: the xHCI controller and its
 * devices). Each USB disk (a key) is served on a port of its own, "usbdisk0",
 * "usbdisk1"..., with the DISK_* protocol above, over the whole device (the
 * disk server vblk finds the SieFS partition in it). Reply: w[0] = result. */
enum {
    USB_DISKS = 1,  /* -> number of USB disks, once the devices present at start are
                       ready (or after a few seconds: a slow key is not waited for) */
    USB_DISK,       /* w[1] = index; rbuf <- usb_disk_t                      -> 0 */
};
typedef struct {
    char port[16];                     /* "usbdisk0" */
    uint32_t sector;                   /* its sector size in bytes (512, 4096) */
    uint32_t present;                  /* 0: unplugged (its port answers -EIO meanwhile) */
    uint64_t blocks;                   /* its size in 4 KiB blocks */
    uint16_t vendor, product;
    char name[38];                     /* the device's product name, if it has one */
} usb_disk_t;

/* File server, port "fs" (SieFS). Paths are absolute ("/a/b"), sent in
 * sbuf; two paths ("from\0to") or a path and a name are separated by a 0.
 * Open files are named by a handle, valid only for the process that opened
 * it. The caller's uid/gid (stamped by the kernel) decide what is allowed;
 * root (uid 0) may do anything. Reply: w[0] = result (>= 0) or -error. */
#define FS_MAX (128 << 10)                     /* largest read or write per message */
enum { FS_RDONLY = 0, FS_WRONLY = 1, FS_RDWR = 2, FS_CREAT = 0x40, FS_EXCL = 0x80,
       FS_TRUNC = 0x200, FS_APPEND = 0x400, FS_DIRECTORY = 0x10000 };
enum {
    FS_OPEN = 1,    /* path; w[1] = flags, w[2] = mode for a new file        -> handle */
    FS_CLOSE,       /* w[1] = handle                                          -> 0     */
    FS_READ,        /* w[1] = handle, w[2] = offset, w[3] = size; data in rbuf -> bytes */
    FS_WRITE,       /* w[1] = handle, w[2] = offset (ignored with FS_APPEND); data in sbuf -> bytes */
    FS_STAT,        /* path; a siefs_stat_t comes in rbuf                     -> 0     */
    FS_FSTAT,       /* w[1] = handle; a siefs_stat_t in rbuf                  -> 0     */
    FS_READDIR,     /* w[1] = handle, w[2] = cursor; rbuf receives records
                       {u64 ino, u8 type, u8 namelen, name, no 0}             -> bytes, w[1] = next cursor */
    FS_MKDIR,       /* path; w[1] = mode                                      -> 0 */
    FS_UNLINK,      /* path                                                   -> 0 */
    FS_RMDIR,       /* path                                                   -> 0 */
    FS_RENAME,      /* "from\0to"                                             -> 0 */
    FS_LINK,        /* "existing\0new"                                        -> 0 */
    FS_SYMLINK,     /* "target\0path"                                         -> 0 */
    FS_READLINK,    /* path; the target comes in rbuf                         -> length */
    FS_TRUNCATE,    /* w[1] = handle, w[2] = size                             -> 0 */
    FS_CHMOD,       /* path; w[1] = mode                                      -> 0 */
    FS_CHOWN,       /* path; w[1] = uid, w[2] = gid (root only)               -> 0 */
    FS_UTIMES,      /* path; w[1] = access time, w[2] = change time (ns)      -> 0 */
    FS_GETXATTR,    /* "path\0name"; the value comes in rbuf                  -> length */
    FS_SETXATTR,    /* "path\0name\0value", w[1] = value length               -> 0 */
    FS_LISTXATTR,   /* path; "name\0name\0" comes in rbuf                     -> length */
    FS_RMXATTR,     /* "path\0name"                                           -> 0 */
    FS_FIND,        /* "name\0value", w[1] = cursor; paths ("p\0p\0") in rbuf -> bytes, w[1] = next cursor (0: done) */
    FS_STATFS,      /* a siefs_statfs_t comes in rbuf                         -> 0 */
    FS_SYNC,        /* commit everything to the disk now                      -> 0 */
    FS_TICK,        /* (the server's own commit thread)                       -> ns to wait */
};

/* Accounts server, port "auth" (/bin/auth, run as root by init). It alone
 * reads the password hashes (/etc/secrets), changes the account files, and
 * starts users' sessions. Strings travel in sbuf, separated by 0s; what a
 * caller may do depends on its uid, stamped by the kernel. Reply: w[0] =
 * result (>= 0) or -error. */
enum {
    AUTH_STATE = 1, /* w[1] = AUTH_WAIT: no answer until the first start is decided.
                       -> 1: accounts exist; 0: first start (no AUTH_WAIT); 2: first start,
                       and no screen does it: the terminal must (login, AUTH_WAIT only) */
    AUTH_SETUP,     /* "rootpw\0name\0full name\0userpw[\0name\0full\0pw]..." (up to
                       AUTH_SETUP_MAX users): the first start only, by its owner only
                       (the screen's greeter if one said so, else the terminal) -> 0 */
    AUTH_LOGIN,     /* "name\0password[\0terminal]"; w[1] = AUTH_NOPW (root only: no password
                       needed). Starts the user's shell as a child of the caller, on that
                       terminal (mk_ident_t.console; default: the caller's) -> its pid
                       (the caller waits for it with SYS_WAIT) */
    AUTH_PASSWD,    /* "name\0old\0new": one's own (with the old one), or root: anyone's -> 0 */
    AUTH_USERADD,   /* "name\0full name\0password" (root)       -> the new uid */
    AUTH_USERDEL,   /* "name" (root; the home folder is kept)   -> 0 */
};
#define AUTH_NOPW 1
#define AUTH_WAIT 2                     /* AUTH_STATE: wait until the first start is decided */
#define AUTH_SETUP_MAX 4                /* users created by the first start (root aside) */
#define AUTH_PWMIN 8                    /* a new password: at least 8 characters, not the name */
/* AUTH_DISPLAY (root only, the desktop): w[1] = 1: "I am a screen and I do the
 * first start" (the caller becomes its owner); 0: "there is no screen". -> 0 */
#define AUTH_DISPLAY 0x40

/* The supervisor, port "init" (/bin/svc uses it). Reply: w[0] = result or -error. */
enum {
    INIT_LIST = 1,  /* an array of svc_info_t comes in rbuf                  -> how many */
    INIT_RESTART,   /* "name" (root): stop it if running, start it now       -> 0 */
    INIT_STOP,      /* "name" (root): stop it, do not restart it             -> 0 */
    INIT_ADD,       /* "name\0path\0arguments" (root), w[1] = policy: supervise a
                       program from the disk                                 -> 0 */
    INIT_TICK,      /* (init's own timer thread)                             -> ns to wait */
    INIT_IDLE,      /* "name" (root), w[1] = seconds without use before an on-demand
                       service stops (0: never)                              -> 0 */
    INIT_ANSWER,    /* (init's own timer thread: a server's SVC_MAYSTOP answer) -> 0 */
};
enum { SVC_ALWAYS, SVC_ON_FAILURE, SVC_NEVER };                 /* restart policies */
/* Any server started on demand answers this question from init (pid 1)
 * only: "may you stop?" w[1] = its idle time, in ns. Reply w[0]: 0 = yes:
 * nobody asked it anything since the last question and it holds no client
 * state; it ends (status 0) right after answering. > 0: ask again after
 * that many ns. (A number no protocol above uses.) */
#define SVC_MAYSTOP 0x7e01
enum { SVC_RUNNING, SVC_WAITING, SVC_GIVEN_UP, SVC_STOPPED,     /* states; WAITING: restart */
       SVC_IDLE };                     /* pending (backoff, or a session still open); IDLE: an
                                          on-demand service not running: it starts when needed */
typedef struct {
    char name[16];
    int32_t state, pid, restarts, last_status, policy, driver;
    uint64_t started_ns;                       /* when it was last started (ns since boot) */
    char port[16];                             /* on demand: the port that starts it ("": at boot) */
    uint64_t idle_ns;                          /* on demand: stops after this long unused (0: never) */
    int32_t starts, _pad;                      /* how many times it was started */
} svc_info_t;

/* The assistant, port "sia" (/bin/siad; the command: /bin/sia; docs/ai-plan.md). A client opens
 * a session, sends a prompt and receives the answer in pieces: the reply to
 * SIA_ASK comes at once; then each SIA_NEXT returns the next piece of text
 * (rbuf), w[0] = bytes, 0 when the answer is complete, or -error. Several
 * sessions may run at once, each with its own conversation (and, for the
 * local model, its own attention cache). Reply: w[0] = result or -error. */
#define SIA_MAX 4096                          /* largest prompt or piece per message */
enum {
    SIA_OPEN = 1,   /* w[1] = flags (none yet).                          -> session id   */
    SIA_ASK,        /* w[1] = session; sbuf = the user's message (UTF-8). -> 0           */
    SIA_NEXT,       /* w[1] = session; rbuf <- next piece of the answer.  -> bytes, 0 = end */
    SIA_STOP,       /* w[1] = session: stop generating now.               -> 0           */
    SIA_CLOSE,      /* w[1] = session: forget the conversation.           -> 0           */
    SIA_INFO,       /* rbuf <- sia_info_t.                                -> 0           */
    SIA_CONFIG,     /* root only; sbuf = "key=value" (backend=local|remote, model=path,
                       url=..., api_model=..., api_key=...). Saved in /etc/sia.conf. -> 0 */
    /* Memory (docs/sia.md): conversations are saved in the caller's home
     * (~/.sia), so they survive restarts; a long one is compacted. */
    SIA_LIST,       /* rbuf <- lines "id<TAB>turns<TAB>tokens<TAB>title\n", newest first -> 0 */
    SIA_FORGET,     /* w[1] = conversation id, 0 = all: delete it.        -> 0           */
    SIA_FACTS,      /* w[1] = SIA_F_...: the long-term memory (what sia remembers about
                       the user; one fact per line, in the system prompt)  -> see below  */
    SIA_CONTEXT,    /* w[1] = session; sbuf = extra instructions for this conversation
                       (replace the previous ones; e.g. Atlas's actions and files). -> 0 */
};
/* SIA_OPEN flags (w[1]); with SIA_RESUME, w[2] = conversation id (0: the last
 * one). The reply's w[1] is the conversation id, w[2] = 1 if it was resumed. */
#define SIA_RESUME 1
/* SIA_FACTS operations: GET rbuf <- the facts; ADD sbuf = a fact; DEL w[2] =
 * its line (1 = first); CLEAR; OFFERED rbuf <- facts the model proposed in
 * its last answers ("[remember: ...]"), to be confirmed with ADD. */
enum { SIA_F_GET, SIA_F_ADD, SIA_F_DEL, SIA_F_CLEAR, SIA_F_OFFERED };
enum { SIA_LOCAL = 1, SIA_REMOTE };            /* backends */
typedef struct {
    int32_t backend, ready;                    /* ready: model loaded / endpoint reachable */
    int32_t sessions, ctx;                     /* open sessions; context length in tokens */
    uint64_t mem;                              /* memory used by the model, in bytes */
    uint32_t tok_s_x100;                       /* last generation speed, tokens/s x 100 */
    char model[64];                            /* model name, or the remote model id */
} sia_info_t;

/* The network server, port "net" (/bin/netd, docs/net.md): TCP and UDP
 * over IPv4, DNS names, ping. A handle belongs to the process that opened
 * it. IPv4 addresses are numbers a.b.c.d = a << 24 | b << 16 | c << 8 | d.
 * Calls that wait (CONNECT, ACCEPT, RECV, RESOLVE, PING) take a timeout in
 * milliseconds (0: the default, 30 s; RECV: 0 = forever). Reply: w[0] =
 * result or -error. */
#define NET_MAX (64 << 10)                    /* largest send or receive per message */
enum { NET_TCP = 1, NET_UDP };
enum {
    NET_SOCKET = 1, /* w[1] = NET_TCP or NET_UDP                                  -> handle */
    NET_CONNECT,    /* TCP: w[1] = handle, w[2] = address, w[3] = port | timeout << 16 -> 0 */
    NET_LISTEN,     /* TCP: w[1] = handle, w[2] = port, w[3] = backlog (port < 1024: root) -> 0 */
    NET_ACCEPT,     /* TCP: w[1] = handle, w[3] = timeout. -> new handle; reply w[1] =
                       the peer's address, w[2] its port */
    NET_SEND,       /* w[1] = handle, sbuf = data (waits for room)                -> bytes */
    NET_RECV,       /* w[1] = handle, w[2] = max bytes, w[3] = timeout; rbuf <- data
                       -> bytes, 0 = the other side closed                       */
    NET_BIND,       /* UDP: w[1] = handle, w[2] = local port (0: any)             -> 0 */
    NET_SENDTO,     /* UDP: w[1] = handle, w[2] = address, w[3] = port, sbuf      -> bytes */
    NET_RECVFROM,   /* UDP: w[1] = handle, w[3] = timeout; rbuf <- one datagram   -> bytes;
                       reply w[1] = sender's address, w[2] its port              */
    NET_CLOSE,      /* w[1] = handle                                              -> 0 */
    NET_RESOLVE,    /* sbuf = host name, w[3] = timeout -> 0; reply w[1] = address */
    NET_PING,       /* w[1] = address, w[3] = timeout             -> round trip in µs */
    NET_INFO,       /* rbuf <- net_info_t                                         -> 0 */
    NET_FRAMES,     /* (from the network driver) sbuf = frames received, each a 16-bit
                       length then the frame                                     -> 0 */
    NET_TICK,       /* (netd's own timer thread)                       -> ns to wait */
    NET_LINK,       /* (from the network driver) w[1] = 1: the cable's link came up
                       (netd asks DHCP again), 0: it went down               -> 0 */
};
typedef struct {
    uint32_t ip, mask, gw, dns;                /* from DHCP; 0 while not configured */
    uint8_t mac[6]; uint16_t mtu;
    int64_t time;                              /* now, in seconds since 1970 (UTC) */
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
    int32_t up, sockets;                       /* up: a network card is present */
} net_info_t;
/* Network errors (POSIX numbers). */
#ifndef ENETUNREACH
#define ENETUNREACH   101
#define ECONNRESET    104
#define ENOTCONN      107
#define ETIMEDOUT     110
#define ECONNREFUSED  111
#define EHOSTUNREACH  113
#define EADDRINUSE    98
#define EPROTO        71
#endif

/* A network card driver (vnet), port "nic0": the network server sends it
 * frames to transmit; the driver sends frames received to "net" (NET_FRAMES). */
enum {
    NIC_INFO = 1,   /* rbuf <- nic_info_t                                         -> 0 */
    NIC_SEND,       /* sbuf = frames, each a 16-bit length then the frame         -> 0 */
};
typedef struct {
    uint8_t mac[6]; uint16_t mtu;
    int64_t rtc;                               /* the clock chip's time at boot: seconds since
                                                  1970, read when the driver started ... */
    int64_t rtc_ns;                            /* ... at this time (ns since boot) */
} nic_info_t;
