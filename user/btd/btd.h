/*
 * btd.h - The Bluetooth server's parts (user/btd, docs/bluetooth.md):
 *   btd.c    the server: port "bt", the transports (USB through the usb
 *            server, the Pis' UART: uart.c), firmware, the controller's set-up,
 *            the keys file; the platform calls below
 *   hci.c    HCI: commands, events, ACL data and its flow control; devices,
 *            scanning, connections, classic pairing (the controller's SSP)
 *   l2cap.c  L2CAP: signalling, channels (the API A2DP and RFCOMM will use)
 *   smp.c    LE pairing (SMP): Secure Connections, legacy for old devices
 *   input.c  keyboards and mice: classic HID (boot protocol), HID over GATT
 *   sdp.c    SDP: our records, and a client to find a device's services
 *   a2dp.c   headphones and speakers: AVDTP, the media channel (RTP, SBC)
 *   sbc.c    the SBC encoder
 *   rfcomm.c RFCOMM: one multiplexer and data channel, for OBEX
 *   obex.c   file transfer: OBEX Object Push, both ways
 * All but btd.c and uart.c are the stack: they touch no hardware
 * and are also built on the host (tests/bt), driven by scripted traces.
 * One lock (btd.c) covers the stack: the receiving thread, the clients'
 * requests and the clock tick each take it.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "mk.h"
#include "mk/crypto.h"
#include "hidrep.h"

enum { H4_CMD = 1, H4_ACL = 2, H4_EVT = 4 };    /* HCI packet types (the UART's H4 framing) */
#define L2_MTU   1017                           /* what our channels take (an ACL buffer, less headers) */
#define MAXCONN  4

/* ---- What the platform gives the stack (btd.c; tests/bt/hcitest.c). */
void tr_send(int type, const uint8_t *p, int n);        /* a packet to the controller */
void bt_log(const char *fmt, ...);                      /* a line in the log */
void bt_random(void *p, int n);
void bonds_save(void);                                  /* the bonded devices changed */
struct btdev;
int  file_open(const struct btdev *from, const char *name, uint32_t size);   /* a file comes (obex.c) -> 0 to take it */
int  file_write(const void *p, int n);                  /* -> n */
void file_close(int ok);                                /* complete, or broken off (deleted) */

/* ---- Devices: found by a scan, or bonded (kept in /etc/bluetooth.keys). */
typedef struct btdev {
    bt_dev_t d;                                 /* address, type, kind, flags, name (mk/proto.h) */
    uint8_t key[16];                            /* bonded: the classic link key, or the LE LTK */
    uint8_t rand[8];                            /* LE legacy: the LTK's Rand and EDIV */
    uint16_t ediv;
    uint8_t keysize, named, psrm, _pad;         /* named: a name was asked for; psrm, clkoff: */
    uint16_t clkoff;                            /* where to page it (the inquiry said) */
} btdev_t;

typedef struct l2ch l2ch_t;
typedef struct conn {
    uint16_t handle;
    uint8_t le, out, enc, master, peer_io;      /* out: we connected; peer_io: its IO capability */
    btdev_t *dev;
    int inflight;                               /* its ACL packets the controller still holds */
    int rxn;
    uint8_t rx[L2_MTU + 8];                     /* an L2CAP frame being put together */
    struct smp *smp;                            /* LE pairing in progress */
    struct gatt *gatt;                          /* HID over GATT */
    hidrep_t *hid;                              /* its keyboard / mouse, once in use */
    l2ch_t *hidc, *hidi;                        /* classic HID: control and interrupt channels */
} conn_t;

extern struct bt {
    int state, on;                              /* BTS_*; on: wanted on (kept in the keys file) */
    uint8_t addr[6];
    int acl_mtu, acl_free, le_mtu, le_free;     /* the controller's buffers (LE: 0 = shared) */
    int ncmd;                                   /* commands it takes now */
    int scan_left, inquiring, le_busy;          /* le_busy: 1 auto-connecting, 2 connecting to pdev */
    int pair, pair_left;                        /* BTP_*; seconds before it gives up */
    uint32_t code;
    btdev_t *pdev;                              /* the device being paired */
    btdev_t *cdev;                              /* the device being connected to (bt connect, bt send) */
    char chip[48], why[64];
    btdev_t dev[BT_MAXDEV];
    int ndev;
    conn_t conn[MAXCONN];
} bt;

/* hci.c: the stack's entry points (called with the lock held) */
void bt_ready(void);                            /* the controller is set up (bt.addr, buffers) */
void bt_rx(int type, const uint8_t *p, int n);  /* a whole packet from the controller */
void bt_tick(void);                             /* once a second */
int  bt_scan(void);
int  bt_pair(const bt_dev_t *d);
int  bt_confirm(int ok);
int  bt_forget(const bt_dev_t *d);
int  bt_power(int on);
int  bt_list(bt_dev_t *out);
int  bt_connect(const bt_dev_t *d, int on);     /* bt connect / disconnect */
/* hci.c, for the other parts */
void cmd(int op, const void *p, int n);         /* an HCI command (queued while the controller is busy) */
void acl_send(conn_t *c, int cid, const void *p, int n);   /* an L2CAP frame on channel cid */
void le_encrypt(conn_t *c, const uint8_t *ltk, const uint8_t *rand, int ediv);
void pair_end(int ok, const char *why);         /* the pairing in progress succeeded or failed */
btdev_t *dev_add(const uint8_t *addr, int type);
btdev_t *dev_find(const uint8_t *addr);
conn_t *bt_link(btdev_t *d);                    /* its encrypted link, or 0 (it is paged: services come then) */
void bt_disconnect(conn_t *c, int reason);

/* l2cap.c. A channel: open() when both sides have configured it, recv()
 * each frame (at most L2_MTU bytes), close() when it ends (either side, or
 * the link). l2_listen: incoming channels on psm (bonded devices only);
 * l2_open: an outgoing one; l2_send: one frame (-1: not open, or too long
 * for the peer). A2DP will add AVDTP's PSM 0x19, RFCOMM (OBEX) PSM 3. */
typedef struct {
    void (*open)(l2ch_t *h);
    void (*recv)(l2ch_t *h, const uint8_t *p, int n);
    void (*close)(l2ch_t *h);
} l2ops_t;
struct l2ch {
    conn_t *c;
    const l2ops_t *ops;
    void *priv;                                 /* the user's */
    uint16_t psm, lcid, rcid, rmtu;             /* rmtu: the largest frame the peer takes */
    uint8_t state, done, ident;
};
void l2_listen(uint16_t psm, const l2ops_t *ops);
l2ch_t *l2_open(conn_t *c, uint16_t psm, const l2ops_t *ops);
int  l2_send(l2ch_t *h, const void *p, int n);
void l2_close(l2ch_t *h);
void l2_rx(conn_t *c, const uint8_t *p, int n); /* a whole frame from the link */
void l2_gone(conn_t *c);                        /* the link ended */

/* smp.c */
void smp_start(conn_t *c);
void smp_rx(conn_t *c, const uint8_t *p, int n);
void smp_encrypted(conn_t *c, int ok);
int  smp_confirm(conn_t *c, int ok);
void smp_end(conn_t *c);

/* input.c */
void input_init(void);                          /* listen on the HID PSMs */
void hidp_start(conn_t *c);                     /* classic: open the HID channels */
void hogp_start(conn_t *c);                     /* LE: find the HID service, use it */
void att_rx(conn_t *c, const uint8_t *p, int n);
void input_gone(conn_t *c);

/* a2dp.c. audiod's sound (48 kHz, 16-bit stereo) lands in pcm_ring (btd.c's
 * port "btaudio"); pcm_w, pcm_r: bytes written and sent, free-running. */
#define PCM_RING (32 << 10)
extern uint8_t pcm_ring[PCM_RING];
extern uint32_t pcm_w, pcm_r;
extern volatile int a2dp_new;                   /* a sink is ready: audiod should play to it */
void a2dp_init(void);
void a2dp_link(conn_t *c);                      /* an audio device's link is encrypted */
void a2dp_tick(void);
int  a2dp_put(uint32_t at, int n, int vol);     /* n bytes landed at pcm_w (at) -> n, or -ENOENT: no sink */
int64_t a2dp_pump(int64_t now);                 /* send what is due -> ns until the next packet, 0: none waiting */
const char *a2dp_sink(void);                    /* the ready sink's name, or 0 */

/* rfcomm.c (calls ob_open, ob_recv, ob_close) and obex.c */
void rf_init(void);
int  rf_open(conn_t *c, int ch);
void rf_send(const uint8_t *p, int n);          /* (p stays valid until sent: it may wait for credits) */
void rf_close(void);
void ob_open(conn_t *c, int out);
void ob_recv(const uint8_t *p, int n);
void ob_close(void);
int  opp_send(const uint8_t *addr, const char *name, uint32_t size);
int  opp_data(const uint8_t *p, int n, int last);
int  opp_link(conn_t *c);                       /* a link is encrypted: 1 if a file waits for it */
void opp_tick(void);

/* sdp.c: our records (A2DP source, Object Push on RFCOMM channel OPP_CH);
 * sdp_query: one at a time, done(c, 0) if the device has no such service. */
#define OPP_CH 9
typedef struct { int psm, avdtp, ch, mapn; const uint8_t *map; } sdp_res_t;   /* L2CAP PSM, AVDTP version, RFCOMM channel, HID report map */
typedef void (*sdp_done_t)(conn_t *c, const sdp_res_t *r);
void sdp_init(void);
int  sdp_query(conn_t *c, int uuid, sdp_done_t done);

/* sbc.c: joint stereo, 16 blocks, 8 subbands, loudness; freq 2: 44.1 kHz, 3: 48 kHz */
typedef struct { int16_t h[2][208]; int32_t gain; uint8_t freq, bitpool; } sbc_t;   /* gain: Q16 */
int sbc_len(int bitpool);                       /* a frame's bytes */
int sbc_frame(sbc_t *s, const int16_t *pcm, unsigned at, unsigned mask, uint8_t *out);

static inline uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void p16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void pb16(uint8_t *p, int v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }   /* (big-endian: SDP, RTP, OBEX) */
