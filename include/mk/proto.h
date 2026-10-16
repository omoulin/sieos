/*
 * mk/proto.h - The message protocols of the system servers: what goes in w[]
 * of a msg_t. The kernel knows nothing of these; only clients and servers do.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

/* Console server, port "console". Reply: w[0] = result (>= 0) or -error. */
enum {
    CON_WRITE = 1,  /* sbuf/slen: text to print.             -> bytes written  */
    CON_READ,       /* w[1] = max size; data comes in rbuf.  -> bytes (0: EOF) */
    CON_ECHO,       /* w[1] = 1: show typed characters, 0: hide (passwords),
                       CON_RAW: each key as it comes, unedited, unshown (ssh) */
    CON_INPUT,      /* (root) become the owner of the keyboard and mouse: their
                       events go to the caller (the desktop) instead of the text
                       console (the serial line stays a text console). The reply
                       comes when there are events: con_event_t[] in rbuf -> count */
    CON_HASINPUT,   /* -> 1 if there is a keyboard or pointer (a desktop is usable),
                       0 if not (a Raspberry Pi without a USB keyboard) */
    CON_INJECT,     /* (root: the USB server) input from a USB keyboard or mouse:
                       sbuf = con_inject_t[]; none: "a keyboard or pointer is here" -> 0 */
    CON_MOUSE,      /* (root: the desktop) how far a mouse's move goes: w[1] = speed in %
                       (25..400, 100: about a pixel per count), w[2], w[3] = the screen's
                       width and height in pixels -> 0 */
    CON_FG,         /* w[1] = the process group of the job the shell runs now (0: none,
                       the shell reads again), for Ctrl-C: SIGINT to that group. Only
                       the group's user (or root) may set it -> 0 */
};
#define CON_RAW 2
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
    DISK_INFO = 1,  /*                    -> number of 4 KiB blocks; reply w[1] = the SD
                       controller's physical address when the disk is an SD card (0: not) */
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

/* A USB camera (the USB server's uvc.c), port "camera" while one is
 * plugged in: one client at a time, the camera streaming only while open
 * (and stopped by itself after 3 s without a request). Frames come
 * converted: 0x00RRGGBB pixels, rows one after the other. Reply: w[0]. */
enum {
    CAM_OPEN = 1,   /* -> 0; reply w[1] = width, w[2] = height (-EBUSY: another client's) */
    CAM_FRAME,      /* rbuf <- the next frame (waits for it, up to 1 s) -> bytes */
    CAM_CLOSE,      /* the camera stops                                 -> 0 */
};
/* A keyboard's light (the USB server's kbl.c: the Legion laptops' ITE 8258,
 * 048d:c195), port "kblight" while one is there. The setting is kept in
 * /etc/kblight and given to the keyboard again at each start. */
enum {
    KBL_SET = 1,    /* w[1] = KBL_OFF / KBL_LOW / KBL_HIGH, w[2] = colour 0xRRGGBB -> 0 */
    KBL_GET,        /* -> the level; reply w[1] = the colour */
};
enum { KBL_OFF, KBL_LOW, KBL_HIGH };
/* The colours offered (Settings, /bin/kblight; any 0xRRGGBB may be set). */
#define KBL_NCOL 7
#define KBL_COLOURS { { "White", 0xFFFFFF }, { "Red", 0xFF0000 }, { "Orange", 0xFF6000 }, { "Green", 0x00FF00 }, \
                      { "Cyan", 0x00FFFF }, { "Blue", 0x0000FF }, { "Purple", 0xA000FF } }

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
    FS_SNAP,        /* "path\0name", w[1] = SNAP_...: a snapshot of path's volume. Taking,
                       deleting, restoring: root, or the owner of the volume's root. LIST:
                       w[2] = cursor; rbuf <- records {i64 created (ns), u8 length, name}
                                                     -> bytes (LIST: w[1] = next cursor) */
    FS_MKVOL,       /* "path\0" [fs_key_t]; w[1] = mode, w[2] = uid, w[3] = gid (root only):
                       a new volume mounted at path (a user's home); with a key: encrypted,
                       its key wrapped by that key-encryption key           -> 0 */
    FS_RMVOL,       /* path (root only): an empty volume, without snapshots   -> 0 */
    FS_AUTOSNAP,    /* (the server's own snapshot thread: the hourly snapshots) -> ns to wait */
    FS_UNLOCK,      /* "path\0" kek[32] (root: auth, at a login): path's encrypted volume
                       opens (0 if it is open already)                     -> 0, -EACCES */
    FS_REKEY,       /* "path\0" fs_key_t [old kek[32]] (root: auth, a new password): the
                       volume key is wrapped anew; the old key opens it first if locked -> 0 */
    FS_KDF,         /* path (root): how its volume's key-encryption key is derived; a
                       siefs_kdf_t in rbuf (-EINVAL: the volume is not encrypted) -> 0 */
};
/* A key-encryption key and how it was derived from a password (Argon2id:
 * auth does that, as for its logins; the file server never sees passwords). */
typedef struct { uint8_t salt[16]; uint32_t t, m_kib; uint8_t kek[32]; } fs_key_t;
enum { SNAP_TAKE = 1, SNAP_DELETE, SNAP_RESTORE, SNAP_LIST };

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
    AUTH_LOGIN,     /* "name\0password[\0terminal[\0command]]"; w[1] = AUTH_NOPW (root only:
                       no password needed). Starts the user's shell as a child of the caller,
                       on that terminal (mk_ident_t.console; default: the caller's), running
                       the command if one is given (sh -c) -> its pid (the caller waits for
                       it with SYS_WAIT) */
    AUTH_PASSWD,    /* "name\0old\0new": one's own (with the old one), or root: anyone's -> 0 */
    AUTH_USERADD,   /* "name\0full name\0password" (root)       -> the new uid */
    AUTH_USERDEL,   /* "name" (root; the home folder is kept)   -> 0 */
    AUTH_CHECK,     /* "name\0password" (root: the desktop, before a system setting is
                       changed): is it that user's password? -> 0, or -EACCES (after 1 s) */
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
    INIT_ADD,       /* "name\0path\0arguments[\0port[\0watched port]]" (root), w[1] =
                       policy: supervise a program from the disk; port: started on
                       demand; watched port: pinged (SVC_PING), ended if stuck; w[3]
                       = 1: background work (SPAWN_BACKGROUND)               -> 0 */
    INIT_TICK,      /* (init's own timer thread)                             -> ns to wait */
    INIT_IDLE,      /* "name" (root), w[1] = seconds without use before an on-demand
                       service stops (0: never)                              -> 0 */
    INIT_ANSWER,    /* (init's own timer thread: a server's SVC_MAYSTOP answer) -> 0 */
    INIT_PING,      /* (init's own watcher thread) w[1] = the service, w[2] = 1: a ping
                       is going out to it, 0: it answered                    -> 0 */
};
enum { SVC_ALWAYS, SVC_ON_FAILURE, SVC_NEVER };                 /* restart policies */
/* Any server started on demand answers this question from init (pid 1)
 * only: "may you stop?" w[1] = its idle time, in ns. Reply w[0]: 0 = yes:
 * nobody asked it anything since the last question and it holds no client
 * state; it ends (status 0) right after answering. > 0: ask again after
 * that many ns. (A number no protocol above uses.) */
#define SVC_MAYSTOP 0x7e01
/* init's "are you there?" to a core server (any answer will do: a server
 * answers a request it does not know with -ENOSYS at once). One that does
 * not answer for 30 s is stuck: init ends it, and starts it again. */
#define SVC_PING 0x7e02
enum { SVC_RUNNING, SVC_WAITING, SVC_GIVEN_UP, SVC_STOPPED,     /* states; WAITING: restart */
       SVC_IDLE };                     /* pending (backoff, or a session still open); IDLE: an
                                          on-demand service not running: it starts when needed */
typedef struct {
    char name[16];
    int32_t state, pid, restarts, last_status, policy, driver;
    uint64_t started_ns;                       /* when it was last started (ns since boot) */
    char port[16];                             /* on demand: the port that starts it ("": at boot) */
    uint64_t idle_ns;                          /* on demand: stops after this long unused (0: never) */
    int32_t starts;                            /* how many times it was started */
    int32_t bg;                                /* background work (svc add -b): E-cores first */
} svc_info_t;
#define SVC_MAX 32                             /* services init supervises, at most */

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
    NET_PING,       /* w[1] = address, w[2] = time to live (0: 64), w[3] = timeout -> round
                       trip in µs; reply w[1] = who answered (with a small TTL: a router
                       on the way, "time exceeded")                               */
    NET_INFO,       /* rbuf <- net_info_t                                         -> 0 */
    NET_FRAMES,     /* (from a network driver) w[1] = its interface (NET_IF_...); sbuf =
                       frames received, each a 16-bit length then the frame                                     -> 0 */
    NET_TICK,       /* (netd's own timer thread)                       -> ns to wait */
    NET_LINK,       /* (from a network driver) w[2] = its interface; w[1] = 1: the link came up
                       (netd asks DHCP again), 0: it went down               -> 0 */
    NET_SOCKETS,    /* rbuf <- net_sock_t[], every socket                         -> how many */
};
typedef struct {
    uint32_t rip;                              /* the other side's address and port (TCP) */
    uint16_t lport, rport;                     /* lport: ours */
    uint8_t type, state, _pad[2];              /* NET_TCP/NET_UDP; TCP's state: closed, listen,
                                                  syn-sent, syn-received, established, fin-wait-1,
                                                  fin-wait-2, closing, time-wait, close-wait,
                                                  last-ack, waiting for the network */
    int32_t owner;                             /* the process's pid */
} net_sock_t;
typedef struct {
    uint32_t ip, mask, gw, dns;                /* from DHCP; 0 while not configured */
    uint8_t mac[6]; uint16_t mtu;
    int64_t time;                              /* now, in seconds since 1970 (UTC) */
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
    int32_t up, sockets;                       /* up: a network card is present */
    int32_t iface, link;                       /* the interface in use (NET_IF_..., -1: none), its
                                                  link (1 up, 0 down, -1 not known: virtio) */
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
    int32_t link, _pad;                        /* 1 up, 0 down, -1 no link to watch (virtio) */
} nic_info_t;
/* netd's interfaces: the cable (vnet, port "nic0") and Wi-Fi (wifid, port
 * "wifi", which also answers NIC_INFO and NIC_SEND). netd uses one at a
 * time: the cable when its link is up, else Wi-Fi when it is associated. */
enum { NET_IF_ETH, NET_IF_WIFI };

/* The Wi-Fi server, port "wifi" (/bin/wifid; the command: /bin/wifi;
 * docs/net.md). The Raspberry Pis' CYW43455. Reply: w[0] = result or -error. */
#define WIFI_MAXNET 32
enum {
    WIFI_STATUS = 0x40, /* rbuf <- wifi_status_t                                   -> 0 */
    WIFI_SCAN,      /* look for networks (a few seconds); rbuf <- wifi_net_t[]   -> how many */
    WIFI_JOIN,      /* (root) sbuf = "ssid\0passphrase" (empty: an open network): saved in
                       /etc/wifi.conf (root only) and joined; see WIFI_STATUS  -> 0 */
    WIFI_FORGET,    /* (root) leave the network, forget it                       -> 0 */
    WIFI_RXWAIT,    /* (wifid's own delivery thread)                              */
};
enum { WIFI_OFF, WIFI_IDLE, WIFI_SCANNING, WIFI_JOINING, WIFI_UP, WIFI_FAILED };
enum { WIFI_OPEN, WIFI_WPA2, WIFI_OTHER };     /* security, as WIFI_SCAN sees it */
typedef struct {
    int32_t state, rssi, channel, security;    /* rssi in dBm (when up) */
    char ssid[33], why[63];                    /* why: the last failure, in words */
    uint8_t mac[6], bssid[6];
    char fw[64];                               /* the chip and its firmware */
} wifi_status_t;
typedef struct { char ssid[33]; uint8_t bssid[6], channel; int16_t rssi; uint8_t security, _pad[3]; } wifi_net_t;

/* The GPIO server, port "gpio" (/bin/gpiod; the library: user/lib/gpio.h).
 * Pins are numbered as the chip does (a Raspberry Pi's BCM numbers, 0..27 on
 * the 40-pin header). On a machine without GPIO (x86-64, QEMU) the server
 * simulates the pins: outputs read back what they were set to, inputs read
 * their pull. A pin a device uses (the serial console, I2C, SPI: an
 * alternate function) is refused. Reply: w[0] = result (>= 0) or -error. */
#define GPIO_PINS 28
#define GPIO_MAXSTEP 4096                     /* steps in a pattern */
enum {
    GPIO_INFO = 1,  /* -> number of pins; reply w[1] = 1: real pins, 0: simulated;
                       rbuf <- the board's name                               */
    GPIO_MODE,      /* w[1] = pin, w[2] = GPIO_IN / GPIO_OUT, w[3] = GPIO_PULL_... -> 0 */
    GPIO_WRITE,     /* w[1] = pin, w[2] = 0 / 1 (an output)                   -> 0 */
    GPIO_READ,      /* w[1] = pin                                             -> 0 / 1 */
    GPIO_STATE,     /* rbuf <- gpio_pin_t[GPIO_PINS]; -> the levels, a bit per pin */
    GPIO_PLAY,      /* sbuf = gpio_pattern_t then its steps (uint32_t each: the levels
                       of the pins in `mask`); played by the server, from step 0,
                       one step each step_us, again and again if loop      -> 0 */
    GPIO_STOP,      /* the pattern stops (the pins stay as they are)           -> 0 */
    GPIO_PLAYING,   /* -> the step being played, or -1: none; reply w[1] = rounds done */
    GPIO_RECORD,    /* rbuf <- uint32_t[steps]: every pin's level, sampled at each step of
                       the pattern as it played (the inputs' recording)  -> steps sampled */
    GPIO_SELFTEST,  /* checks the free pins, nothing wired to them, then puts each back as it
                       was (not while a pattern plays: -EBUSY); rbuf <- uint8_t[GPIO_PINS],
                       a byte per pin (GPIO_CHK_...: the checks passed; 0: skipped)
                                                                 -> pins that failed */
};
enum { GPIO_IN, GPIO_OUT, GPIO_BUSY };        /* a pin's mode (BUSY: a device's) */
enum { GPIO_PULL_NONE, GPIO_PULL_DOWN, GPIO_PULL_UP };
enum {                                        /* a pin's self-check: the checks it passed */
    GPIO_CHK_DONE = 1,                        /* checked (not skipped) */
    GPIO_CHK_HIGH = 2, GPIO_CHK_LOW = 4,      /* an output set to 1, to 0, reads it back */
    GPIO_CHK_UP = 8, GPIO_CHK_DOWN = 16,      /* an input reads 1 with a pull-up, 0 with a pull-down */
    GPIO_CHK_KEPT = 32,                       /* put back as it was */
    GPIO_CHK_ALL = 63,
};
typedef struct { uint8_t mode, pull, level, pad; } gpio_pin_t;
typedef struct { uint32_t mask, steps, step_us, loop; } gpio_pattern_t;

/* The audio server, port "audio" (/bin/audiod, docs/audio.md): one output
 * and one input stream, 48 kHz, 16-bit, stereo (4 bytes a frame). Written
 * data goes by IPC straight into the ring the controller plays from; read
 * data straight out of the one it records into. Reply: w[0] = result or -error. */
enum {
    AUDIO_WRITE = 1,  /* sbuf = frames; playing starts -> bytes taken (0: there was no
                         room; the reply waited for some: send again)            */
    AUDIO_READ,       /* w[1] = rbuf's size; recording starts, and runs while someone
                         reads -> bytes in rbuf (waits for some)                 */
    AUDIO_VOL,        /* w[1] = volume 0-100, | AUDIO_MUTE; AUDIO_ASK: no change -> the same */
    AUDIO_OUT,        /* w[1] = output (0 speakers, 1... HDMI/DP monitors with sound), or
                         AUDIO_ASK -> w[0] the one playing, w[1] how many; rbuf: their
                         names, one a line                                        */
};
#define AUDIO_MUTE 0x100
#define AUDIO_ASK  0x200

/* Bluetooth (docs/bluetooth.md). A USB adapter's HCI pipe, port "bthci"
 * (the USB server, user/usb/btusb.c; btd is its only client). Reply: w[0]. */
enum {
    BTH_SEND = 0x50,  /* w[1] = 0: an HCI command (control endpoint), 1: bulk OUT (ACL data;
                         Intel's bootloader commands); sbuf = the packet          -> 0 */
    BTH_RECV,         /* rbuf <- what came, records of {endpoint: 0 interrupt (events),
                         1 bulk IN; length (16 bits, LE); bytes}; waits for some -> bytes */
    BTH_INFO,         /*                                     -> vendor << 16 | product */
};
/* The Bluetooth server, port "bt" (/bin/btd; the command: /bin/bt; Settings).
 * Reply: w[0] = result or -error. Addresses: 6 bytes, least significant first. */
#define BT_MAXDEV 24
enum {
    BT_STATUS = 0x58, /* rbuf <- bt_status_t                                         -> 0 */
    BT_LIST,          /* rbuf <- bt_dev_t[]: found by the last scan, and bonded      -> how many */
    BT_SCAN,          /* look for devices (classic and LE), about 10 s; see BT_LIST   -> 0 */
    BT_PAIR,          /* (root) sbuf = bt_dev_t (addr, type): connect, pair, bond; then its
                         keyboard or mouse is used. BT_STATUS follows it (a code to show) -> 0 */
    BT_CONFIRM,       /* (root) w[1] = 1: the code matches, 0: it does not          -> 0 */
    BT_FORGET,        /* (root) sbuf = bt_dev_t (addr): disconnect, delete its keys -> 0 */
    BT_POWER,         /* (root) w[1] = 1 on, 0 off (kept in /etc/bluetooth.keys)    -> 0 */
};
enum { BTS_OFF, BTS_STARTING, BTS_ON, BTS_FAILED };            /* bt_status_t.state */
enum { BTP_NONE, BTP_BUSY, BTP_CONFIRM, BTP_PASSKEY, BTP_DONE, BTP_FAILED };   /* .pair: CONFIRM:
                         does the code match the device's? PASSKEY: type the code on the device */
enum { BTK_OTHER, BTK_KEYBOARD, BTK_MOUSE, BTK_AUDIO, BTK_PHONE, BTK_COMPUTER }; /* bt_dev_t.kind */
enum { BTT_CLASSIC, BTT_LE_PUBLIC, BTT_LE_RANDOM };             /* bt_dev_t.type */
#define BTF_BONDED    1
#define BTF_CONNECTED 2
#define BTF_INPUT     4                        /* its keyboard / mouse is in use */
typedef struct { uint8_t addr[6], type, kind, flags; int8_t rssi; char name[32]; } bt_dev_t;
typedef struct {
    uint8_t state, scanning, pair, _pad;
    uint8_t addr[6], pair_addr[6];             /* ours; the device being paired */
    uint32_t code;                             /* BTP_CONFIRM / BTP_PASSKEY: the 6 digits */
    char chip[48], why[64];                    /* the controller and its firmware; the last failure */
} bt_status_t;
