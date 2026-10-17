/*
 * iwl.c - wifid's driver (wifi.h) for Intel Wi-Fi on PCIe, x86-64:
 *   - the Wi-Fi 6E AX210 ("Typhoon Peak", PCI 8086:2725), firmware
 *     iwlwifi-ty-a0-gf-a0-89.ucode and .pnvm;
 *   - the 22000 family's CNVi Wi-Fi inside Intel's chipsets (Ice Lake
 *     8086:34f0, Comet Lake, Tiger Lake...) with an AX201 / Killer AX1650
 *     ("hr") radio module: firmware iwlwifi-Qu-b0-hr-b0-77, -Qu-c0-hr-b0-77
 *     or -QuZ-a0-hr-b0-77.ucode, by the chip's type and step.
 * Station mode, one network, 2.4 and 5 GHz, HT 20 MHz, open or WPA2-PSK
 * with CCMP in the chip; no power save, no aggregation (block-ack requests
 * are declined), no WPA3.
 *
 *   the firmware (linux-firmware): scans, picks rates, encrypts and decrypts
 *   with the keys we install; the host: brings the device up, authenticates
 *   and associates (802.11 frames), runs WPA2's handshake (wpa.c), carries
 *   data frames.
 *
 * Written from Linux's iwlwifi (pcie/ gen2 transport, fw/ for the file and
 * PNVM, mvm/ with the firmware's "MLD" commands, as Linux sends them: both
 * firmwares take them, in the same versions but for the scan's channels).
 * Transport, in DMA memory:
 *   - AX210: a context info (v2) and a "peripheral scratch" that tell the
 *     device where the firmware's sections, the PNVM, the RX queue and the
 *     command queue are, and an image loader (IML); the 22000: one context
 *     info (v1, Linux's ctxt-info.c) with all of that, no IML, no PNVM;
 *   - one RX queue: 128 buffers of 4 KiB, a free ring and a completion ring
 *     (entries of 16 and 32 bytes on the AX210, 8 and 4 on the 22000); the
 *     device writes how many it filled (closed_rb_num);
 *   - TX queues of 128 TFDs (256 bytes, up to 25 buffer pointers; we use
 *     two): the command queue, and two the firmware allocates for the
 *     access point's station: management (TID 15) and data (TID 0). The
 *     22000 counts a frame's bytes in dwords, wraps write pointers at 256
 *     and has the older TX command (gen2: the 802.11 header 8 bytes sooner).
 * Interrupts are not used (PCI lines need ACPI, MSI needs the kernel): one
 * thread polls the RX queue, every 50 us after traffic, up to 4 ms when quiet.
 * A second thread joins (scan, auth, assoc, handshake) and rejoins.
 *
 * UNTESTED ON HARDWARE (QEMU has neither): every stage is logged ("wifi:
 * ..."); docs/net.md says what to look for.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "wifi.h"
#include "wpa.h"
#include "pci.h"
#include "mk/crypto.h"

#define FWDIR "/lib/firmware/iwlwifi-"
#define PNVM  FWDIR "ty-a0-gf-a0.pnvm"
static int qu;                                   /* the 22000 family (Qu, QuZ), else the AX210 */

/* ---- Registers (CSR), and the peripheral ones behind HBUS (UMAC: +0x300000). */
enum { HW_IF = 0x000, INT = 0x008, CTXT_BA = 0x040, INT_MASK = 0x00c, RESET = 0x020, GP_CNTRL = 0x024, HW_REV = 0x028,
       GIO = 0x03c, GP1_CLR = 0x05c, MBOX_SET = 0x088, RF_ID = 0x09c, SHADOW = 0x0a8, LTR = 0x0d4,
       CHICKEN = 0x100, CTXT_ADDR = 0x118, IML_ADDR = 0x120, IML_SIZE = 0x128, HPET = 0x240,
       LINK_PWR = 0x250, MAC_OTP = 0x380, MAC_STRAP = 0x388, MEM_RADDR = 0x40c, MEM_RDAT = 0x41c,
       PRPH_WADDR = 0x444, PRPH_RADDR = 0x448, PRPH_WDAT = 0x44c, PRPH_RDAT = 0x450, WRPTR = 0x460,
       RX_WIDX = 0x1c80 };
enum { U_CHICK = 0xa05c00, U_DOORBELL = 0xa05c04, U_CPU_RUN = 0xa05c44, LTR_UMAC = 0xa03480, LTR_MAC = 0xa0348c };
static uint32_t umac = 0x300000;                 /* the AX210's UMAC registers are there (the 22000's: + 0) */
/* Firmware commands: (group << 8 | id); legacy ones go in group 1. */
enum { C_PHY = 0x108, C_SCAN_CFG = 0x10c, C_SCAN = 0x10d, C_TX = 0x11c, C_POWER = 0x177, C_TX_ANT = 0x198,
       C_BT = 0x19b, C_MCC = 0x1c8, C_SOC = 0x201, C_INIT_CFG = 0x203, C_SESSION = 0x305, C_MAC = 0x308,
       C_LINK = 0x309, C_STA = 0x30a, C_STA_RM = 0x30c, C_DQA = 0x500, C_RLC = 0x508, C_TLC = 0x50f,
       C_QUEUE = 0x517, C_KEY = 0x518, C_NVM_DONE = 0xc00 };
enum { ADD = 1, MODIFY, REMOVE };

#define NRX  128                                 /* RX buffers (4 KiB) */
#define NTX  128                                 /* TFDs per TX queue (the AX210's least) */
#define SLOT 2048                                /* a TX buffer: command or frame */

static volatile uint8_t *regs;
static uint32_t rd(int r) { return mmio_r32(regs + r); }
static void wr(int r, uint32_t v) { mmio_w32(regs + r, v); }
static void setb(int r, uint32_t b) { wr(r, rd(r) | b); }
static void w64(int r, uint64_t v) { wr(r, (uint32_t)v); wr(r + 4, (uint32_t)(v >> 32)); }
static void p16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v) { p16(p, v); p16(p + 2, v >> 16); }
static void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t)v); p32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t g16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t g32(const uint8_t *p) { return g16(p) | g16(p + 2) << 16; }
static long now_ms(void) { return sys_clock() / 1000000; }
static void msleep(int ms) { sys_sleep((uint64_t)ms * 1000000); }
static int poll(int r, uint32_t mask, uint32_t want, int us)
{
    for (long e = sys_clock() + us * 1000L; (rd(r) & mask) != want; ) if (sys_clock() > e) return -1;
    return 0;
}
/* Peripheral registers and device memory: keep the device's MAC awake meanwhile. */
static volatile int hbus;
static int grab(void)
{
    while (__atomic_exchange_n(&hbus, 1, __ATOMIC_ACQUIRE)) sys_yield();
    setb(GP_CNTRL, 8);                           /* MAC_ACCESS_REQ, until clock ready and not going to sleep */
    if (!poll(GP_CNTRL, 0x11, 1, 15000)) return 0;
    printf("wifi: the device did not wake (GP_CNTRL %x)\n", rd(GP_CNTRL));
    return 0;
}
static void release(void) { wr(GP_CNTRL, rd(GP_CNTRL) & ~8u); __atomic_store_n(&hbus, 0, __ATOMIC_RELEASE); }
static void prph_w(uint32_t a, uint32_t v) { grab(); wr(PRPH_WADDR, a | 3u << 24); wr(PRPH_WDAT, v); release(); }

static int64_t bus_off;
static void *dmem(size_t n, uint64_t *pa)
{
    void *v = dma_alloc(n, pa);
    if ((long)v < 0) { printf("wifi: out of DMA memory (%u bytes)\n", (unsigned)n); return 0; }
    memset(v, 0, n); *pa += bus_off;
    return v;
}

/* ---- TX queues. A slot's buffer: TB0 its first 20 bytes, TB1 the rest. */
typedef struct { uint8_t *tfd, *buf; uint16_t *bc; uint64_t pa; int id, nbuf; volatile uint16_t w, r; } txq_t;
static txq_t cmdq, mgq, dataq;
static int txq_alloc(txq_t *q, int nbuf)
{
    q->tfd = dmem(NTX * 256 + 4096 + (size_t)nbuf * SLOT, &q->pa);
    q->bc = (uint16_t *)(q->tfd + NTX * 256); q->buf = q->tfd + NTX * 256 + 4096; q->nbuf = nbuf;
    return q->tfd ? 0 : -1;
}
static uint8_t *slot(txq_t *q) { return q->buf + (q->w % q->nbuf) * SLOT; }
static void tfd_put(txq_t *q, int len, int bc)
{
    int i = q->w & (NTX - 1);
    uint8_t *t = q->tfd + 256 * i;
    uint64_t pa = q->pa + NTX * 256 + 4096 + (uint64_t)(q->w % q->nbuf) * SLOT;
    int tb0 = len < 20 ? len : 20;
    memset(t, 0, 22);
    p16(t, len > 20 ? 2 : 1);
    p16(t + 2, tb0); p64(t + 4, pa);
    if (len > 20) { p16(t + 12, len - 20); p64(t + 14, pa + 20); }
    if (bc) q->bc[i] = (uint16_t)(qu ? (bc + 3) / 4 : bc);   /* frame bytes (22000: dwords); one 64-byte fetch */
    q->w++;
    dma_wmb();
    wr(WRPTR, (q->w & (qu ? 0xffu : 0xffffu)) | (uint32_t)q->id << 16);
}

/* ---- Events from the receiving thread. */
enum { EV_ALIVE = 1, EV_INIT = 2, EV_PNVM = 4, EV_SCAN = 8, EV_SESSION = 16, EV_LOST = 32, EV_TXDONE = 64 };
static volatile uint32_t ev;
static void ev_set(uint32_t b) { __atomic_or_fetch(&ev, b, __ATOMIC_RELEASE); }
static void ev_clear(uint32_t b) { __atomic_and_fetch(&ev, ~b, __ATOMIC_RELEASE); }
static int wait_ev(uint32_t b, int ms)
{
    for (long e = now_ms() + ms; !(__atomic_load_n(&ev, __ATOMIC_ACQUIRE) & b); msleep(1)) if (now_ms() > e) return 0;
    ev_clear(b);
    return 1;
}

/* ---- Commands: one at a time (only one thread sends them), on one buffer. */
static uint8_t rsp[512];
static volatile int rsp_seq = -1, cmd_seq = -2;
static int cmd(int id, const void *data, int len)
{
    uint8_t *b = cmdq.buf;
    int seq = cmdq.w & 0xff;
    b[0] = (uint8_t)id; b[1] = (uint8_t)(id >> 8); p16(b + 2, seq); p16(b + 4, len); b[6] = b[7] = 0;
    memcpy(b + 8, data, len);
    __atomic_store_n(&rsp_seq, -1, __ATOMIC_RELEASE); cmd_seq = seq;
    tfd_put(&cmdq, 8 + len, 0);
    for (long e = now_ms() + 1000; __atomic_load_n(&rsp_seq, __ATOMIC_ACQUIRE) != seq; msleep(1))
        if (now_ms() > e) { printf("wifi: command %x: no answer\n", id); return -1; }
    return rsp[0] == 0xff && rsp[1] == 0xff ? -1 : 0;   /* (refused: see REPLY_ERROR) */
}
static int cmd32(int id, uint32_t a, uint32_t b) { uint8_t c[8]; p32(c, a); p32(c + 4, b); return cmd(id, c, 8); }
static int cmd4(int id, uint32_t a) { uint8_t c[4]; p32(c, a); return cmd(id, c, 4); }

/* ---- What we know of the network being joined (from its beacon). */
static struct {
    char ssid[33], pass[65];
    uint8_t bssid[6], ies[768];
    int ielen, chan, band24, cap, bi, dtim, rssi, found;
} bss;
static wifi_net_t found[WIFI_MAXNET];
static volatile int nfound, scanning;
static wpa_t wpa;
static int wpa_on, keys_on, ht, aid;
static uint16_t seq_m, seq_d;
static uint8_t ant = 3;                          /* valid antennas (the firmware file says) */

/* ---- Frames for the joining thread: management frames from the AP, EAPOL. */
static struct { int len; uint8_t d[1600]; } box[8];
static volatile unsigned box_w, box_r;
static void box_put(const uint8_t *f, int len, int eapol)
{
    unsigned w = box_w;
    if (w - box_r >= 8 || len > 1598) return;
    box[w % 8].len = len + 1; box[w % 8].d[0] = (uint8_t)eapol; memcpy(box[w % 8].d + 1, f, len);
    __atomic_store_n(&box_w, w + 1, __ATOMIC_RELEASE);
}
/* The next frame (d[0] = 1: EAPOL, else 802.11), waiting up to ms; 0 if none. */
static uint8_t *box_get(int ms, int *len)
{
    for (long e = now_ms() + ms; __atomic_load_n(&box_w, __ATOMIC_ACQUIRE) == box_r; msleep(1)) if (now_ms() >= e) return 0;
    uint8_t *d = box[box_r % 8].d; *len = box[box_r % 8].len - 1;
    box_r++;
    return d;
}

/* ---- Received frames. */
static uint8_t batch[NET_MAX];
static size_t nbatch;
static int rxd = 64, rxe = 40;                   /* the RX descriptor's size; its energy_a, _b, channel (22000: 48, 32) */
static int rssi_of(const uint8_t *d)
{
    int a = d[rxe] ? -d[rxe] : -128, b = d[rxe + 1] ? -d[rxe + 1] : -128;
    return a > b ? a : b;
}
static void scan_add(const uint8_t *h, int len, const uint8_t *d)
{
    const uint8_t *ie = h + 36, *e;
    int il = len - 36, rssi = rssi_of(d), cap = g16(h + 34);
    if (il < 0) return;
    char ssid[33] = { 0 };
    if ((e = ie_find(ie, il, 0)) && e[1] <= 32) memcpy(ssid, e + 2, e[1]);
    int chan = (e = ie_find(ie, il, 3)) && e[1] ? e[2] : d[rxe + 2];
    if (bss.ssid[0] && !strcmp(ssid, bss.ssid) && (!bss.found || rssi > bss.rssi) && il <= (int)sizeof bss.ies) {
        memcpy(bss.bssid, h + 16, 6); memcpy(bss.ies, ie, il); bss.ielen = il;
        bss.chan = chan; bss.band24 = chan <= 14; bss.cap = cap; bss.bi = g16(h + 32); bss.rssi = rssi;
        bss.dtim = (e = ie_find(ie, il, 5)) && e[1] >= 2 && e[3] ? e[3] : 1;
        bss.found = 1;
    }
    int i = 0;
    while (i < nfound && memcmp(found[i].bssid, h + 16, 6)) i++;
    if (i == WIFI_MAXNET) return;
    wifi_net_t *w = &found[i];
    if (i == nfound) { memset(w, 0, sizeof *w); nfound++; } else if (rssi <= w->rssi) return;
    memcpy(w->ssid, ssid, 33); memcpy(w->bssid, h + 16, 6);
    w->channel = (uint8_t)chan; w->rssi = (int16_t)rssi;
    w->security = (e = ie_find(ie, il, 0x30)) ? (wpa_rsn_ok(e) ? WIFI_WPA2 : WIFI_OTHER) : cap & 0x10 ? WIFI_OTHER : WIFI_OPEN;
}
static void rx_mpdu(const uint8_t *d, int dlen)
{
    int len = (int)g16(d), pad = d[3] & 0x20 ? 2 : 0;
    uint32_t st = g32(d + 12);
    const uint8_t *h = d + rxd;
    if ((st & 3) != 3 || len < 24 || rxd + len > dlen) return;      /* CRC and overrun checks */
    int fc = (int)g16(h), type = fc >> 2 & 3, sub = fc >> 4 & 15;
    if (type == 0) {                                                 /* management */
        if (scanning && (sub == 8 || sub == 5)) scan_add(h, len, d);
        else if (!memcmp(h + 4, wifi_mac, 6) && !memcmp(h + 16, bss.bssid, 6)) box_put(h, len, 0);
        return;
    }
    if (type != 2 || (fc & 0x300) != 0x200 || memcmp(h + 10, bss.bssid, 6)) return;   /* data, from our AP */
    int hl = 24 + (sub & 8 ? 2 : 0), crypt = 0;
    if (fc & 0x4000) {                                               /* decrypted by the firmware? */
        if ((st & 0x700) != 0x200 || !(st & (1 << 11)) || !(st & (1 << 6))) return;
        crypt = 8;
    }
    int off = hl + crypt + pad, n = len - pad - (d[2] >> 4) * 2 - off;
    const uint8_t *p = h + off;
    if (n < 8 || memcmp(p, "\xaa\xaa\x03\0\0\0", 6)) return;          /* LLC/SNAP */
    if (p[6] == 0x88 && p[7] == 0x8e) { box_put(p + 8, n - 8, 1); return; }   /* EAPOL */
    if (wifi_state != WIFI_UP || (keys_on && !crypt) || n - 6 + 14 > 1514) return;
    if (nbatch + 2 + 12 + n - 6 > sizeof batch) { wifi_frames(batch, nbatch); nbatch = 0; }
    uint8_t *o = batch + nbatch;                                     /* as Ethernet: DA, SA, type, data */
    p16(o, 12 + n - 6); memcpy(o + 2, h + 4, 6); memcpy(o + 8, h + 16, 6); memcpy(o + 14, p + 6, n - 6);
    nbatch += 2 + 12 + n - 6;
}

static uint32_t err_lmac, err_umac;
static void fw_error(void)
{
    uint32_t a[2] = { err_lmac, err_umac & 0x3fffffff };
    printf("wifi: FIRMWARE ERROR (CSR_INT %x)\n", rd(INT));
    for (int t = 0; t < 2; t++) {
        if (!a[t]) continue;
        grab(); wr(MEM_RADDR, a[t]);
        uint32_t v[8];
        for (int i = 0; i < 8; i++) v[i] = rd(MEM_RDAT);
        release();
        printf("wifi: %s error log: %x %x %x %x %x %x %x %x\n", t ? "UMAC" : "LMAC", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    }
    wifi_state = WIFI_FAILED; strlcpy(wifi_why, "firmware error (see the log)", sizeof wifi_why);
}

static void packet(const uint8_t *pk)
{
    int len = (int)(g32(pk) & 0x3fff) - 4, id = pk[4], grp = pk[5] <= 1 ? 1 : pk[5], seq = (int)g16(pk + 6);
    const uint8_t *d = pk + 8;
    if (len < 0 || len > 4096 - 8) return;
    switch (grp << 8 | id) {
    case 0x101:                                                      /* ALIVE */
        if (len < 128) break;
        err_lmac = g32(d + 4 + 16); err_umac = g32(d + 100 + 8);
        memcpy(rsp, d, 128);
        printf("wifi: firmware alive (status %x, LMAC %x.%x, UMAC %x.%x)\n", g16(d), g32(d + 4), g32(d + 8), g32(d + 100), g32(d + 104));
        ev_set(EV_ALIVE); return;
    case 0x104: ev_set(EV_INIT); return;                             /* INIT_COMPLETE */
    case 0xcfe: ev_set(EV_PNVM); return;                             /* PNVM_INIT_COMPLETE */
    case 0x10f: printf("wifi: scan done (%d networks)\n", nfound); ev_set(EV_SCAN); return;
    case 0x3fb: if (len >= 16 && g32(d + 8)) ev_set(EV_SESSION); return;   /* session protection started */
    case 0x1a2: case 0x3f6:                                          /* missed beacons */
        if (len >= 12 && g32(d + 8) >= 16 && wifi_state == WIFI_UP) { printf("wifi: %u beacons missed\n", g32(d + 8)); ev_set(EV_LOST); }
        return;
    case 0x1c1: rx_mpdu(d, len); return;
    case 0x11c: {                                                    /* TX done: the queue's read pointer */
        if (len < 44) return;
        int q = (int)g16(d + 36), ssn = (int)(g32(d + 40 + 4 * d[0]) & 0xffff);
        txq_t *t = q == dataq.id ? &dataq : q == mgq.id ? &mgq : 0;
        if (t) { uint16_t w = t->w; t->r = (uint16_t)(w - ((w - ssn) & (NTX - 1))); }   /* (the 22000's wraps at 256) */
        ev_set(EV_TXDONE); return; }
    case 0x102:                                                      /* REPLY_ERROR */
        printf("wifi: the firmware refused command %x (error %x)\n", len > 4 ? d[4] : 0, g32(d));
        if (len >= 8 && (int)g16(d + 6) == cmd_seq) { rsp[0] = rsp[1] = 0xff; __atomic_store_n(&rsp_seq, cmd_seq, __ATOMIC_RELEASE); }
        return;
    }
    if (!(seq & 0x8000) && (seq >> 8 & 0x1f) == 0 && (seq & 0xff) == cmd_seq) {   /* a command's answer */
        memcpy(rsp, d, len < (int)sizeof rsp ? len : (int)sizeof rsp);
        if (rsp[0] == 0xff && rsp[1] == 0xff) rsp[0] = 0;
        __atomic_store_n(&rsp_seq, cmd_seq, __ATOMIC_RELEASE);
    }
}

/* ---- The RX queue, polled by its own thread. */
static uint8_t *rxbuf, *fbd, *ubd;
static volatile uint16_t *stts;
static uint64_t rxbuf_pa;
static unsigned rx_r, rx_w = NRX - 1;
static void rb_give(unsigned i, unsigned vid)    /* free ring entry i: buffer vid (1..) */
{
    uint64_t pa = rxbuf_pa + (uint64_t)(vid - 1) * 4096;
    if (qu) p64(fbd + 8 * i, pa | vid);          /* 22000: the address, its id in the low 12 bits */
    else { p16(fbd + 16 * i, vid); p64(fbd + 16 * i + 8, pa); }
}
static void receiver(void *arg)
{
    (void)arg;
    for (int us = 50, dead = 0;;) {
        unsigned r = *stts & (NRX - 1), work = 0;
        while (rx_r != r) {
            unsigned vid = qu ? g32(ubd + 4 * rx_r) & 0xfff : g16(ubd + 32 * rx_r + 4);
            if (vid && vid < NRX) {
                packet(rxbuf + (vid - 1) * 4096);
                rb_give(rx_w, vid);                                  /* the buffer goes back to the device */
                rx_w = (rx_w + 1) & (NRX - 1);
            }
            rx_r = (rx_r + 1) & (NRX - 1);
            work++;
        }
        if (work) { dma_wmb(); wr(RX_WIDX, rx_w & ~7u); }
        if (nbatch) { wifi_frames(batch, nbatch); nbatch = 0; }
        uint32_t i = rd(INT);
        if (i & (1u << 25 | 1u << 29) && !dead) { dead = 1; fw_error(); }   /* firmware or DMA error */
        us = work ? 50 : us < 4000 ? us * 2 : 4000;
        sys_sleep((uint64_t)us * 1000);
    }
}

/* ---- The firmware file: TLVs; the sections (CPU1, a separator, CPU2, a
 * separator, then the paging ones) go to DMA memory, listed in the scratch
 * (the 22000: in the context info): UMAC, LMAC, paging, 64 entries each. */
static uint8_t *fw, *pnvm;
static size_t fwlen, pnvmlen;
static uint32_t phy;                             /* the firmware's PHY_SKU */
static int tlv_next(const uint8_t *f, size_t n, size_t *off, uint32_t *type, uint32_t *len)
{
    if (*off + 8 > n) return 0;
    *type = g32(f + *off); *len = g32(f + *off + 4);
    if (*off + 8 + *len > n) return 0;
    *off += 8;
    return 1;
}
#define TLV_ADV(off, len) (off += ((len) + 3) & ~3u)

static uint8_t *scratch, *misc, *dram;           /* dram: the sections' list */
static uint64_t scratch_pa, misc_pa;
static int load_sections(uint8_t **img, uint8_t **paging, uint64_t *img_pa, uint64_t *pg_pa, size_t *imgsz,
                         const uint8_t **iml, uint32_t *imllen)
{
    size_t off = 88, sz[2] = { 0, 0 };
    uint32_t t, l;
    int grp = 0, pass;
    if (fwlen < 88 || g32(fw + 4) != 0x0a4c5749) return -1;
    for (pass = 0; pass < 2; pass++) {
        int n[3] = { 0, 0, 0 };
        grp = 0; off = 88;
        uint8_t *dst[2] = { pass ? *img : 0, pass ? *paging : 0 };
        uint64_t dpa[2] = { pass ? *img_pa : 0, pass ? *pg_pa : 0 };
        while (tlv_next(fw, fwlen, &off, &t, &l)) {
            const uint8_t *v = fw + off;
            if (t == 19 && l > 4) {                                  /* SEC_RT: a load address, then the data */
                uint32_t a = g32(v);
                if (a == 0xffffcccc || a == 0xaaaabbbb) grp++;
                else if (grp < 3 && n[grp] < 64) {
                    int k = grp == 2, s = (int)((l - 4 + 4095) & ~4095u);
                    if (pass) {
                        static const int at[3] = { 512, 0, 1024 };   /* lmac_img, umac_img, virtual_img */
                        memcpy(dst[k], v + 4, l - 4);
                        p64(dram + at[grp] + 8 * n[grp], dpa[k]);
                        dst[k] += s; dpa[k] += (uint64_t)s;
                    } else sz[k] += (size_t)s;
                    n[grp]++;
                }
            } else if (t == 52) { *iml = v; *imllen = l; }
            else if (t == 23 && l >= 4) phy = g32(v), ant = (uint8_t)(phy >> 16 & 15);   /* PHY_SKU: radio, valid TX antennas */
            TLV_ADV(off, l);
        }
        if (!pass) {
            if (!(*img = dmem(sz[0], img_pa)) || !(*paging = dmem(sz[1] + 4096, pg_pa))) return -1;
            *imgsz = sz[0];
        } else printf("wifi: firmware: %d + %d sections, %d paging, IML %u bytes\n", n[0], n[1], n[2], *imllen);
    }
    return *iml || qu ? 0 : -1;                  /* (the 22000 has no IML) */
}

/* The PNVM file: the section for our SKU (from ALIVE) and hardware. */
static void load_pnvm(const uint8_t *sku, uint32_t hw_rev, uint32_t rf_id)
{
    size_t off = 0;
    uint32_t t, l;
    if (!g32(sku) && !g32(sku + 4) && !g32(sku + 8)) return;
    if (!pnvm) { printf("wifi: %s missing: going on without it\n", PNVM); return; }
    while (tlv_next(pnvm, pnvmlen, &off, &t, &l)) {
        if (t != 64 || l < 12 || memcmp(pnvm + off, sku, 12)) { TLV_ADV(off, l); continue; }
        TLV_ADV(off, l);
        const uint8_t *chunk[32];
        uint32_t clen[32], total = 0;
        int n = 0, hw = 0;
        while (tlv_next(pnvm, pnvmlen, &off, &t, &l) && t != 64) {
            const uint8_t *v = pnvm + off;
            if (t == 58 && l >= 4 && g16(v) == (hw_rev >> 4 & 0xfff) && g16(v + 2) == (rf_id >> 12 & 0xfff)) hw = 1;
            if (t == 19 && l > 4 && g32(v) != 0xddddeeee && n < 32) { chunk[n] = v + 4; clen[n] = l - 4; total += l - 4; n++; }
            TLV_ADV(off, l);
        }
        if (!hw || !n) continue;
        uint64_t pa;
        uint8_t *m = dmem(total + 4096u * n, &pa);
        if (!m) return;
        for (int i = 0; i < n; i++) {                                /* each payload, its address in the list */
            memcpy(m, chunk[i], clen[i]);
            p64(scratch + 0x800 + 8 * i, pa);
            uint32_t s = (clen[i] + 4095) & ~4095u;
            m += s; pa += s;
        }
        p64(scratch + 16, scratch_pa + 0x800); p32(scratch + 24, total);   /* pnvm_cfg */
        ev_clear(EV_PNVM);
        prph_w(umac + U_DOORBELL, 1u << 20);
        printf(wait_ev(EV_PNVM, 1000) ? "wifi: PNVM loaded (%d payloads, %u bytes)\n" : "wifi: PNVM: no answer (%d, %u)\n", n, total);
        return;
    }
    printf("wifi: PNVM: no section for this SKU (%x %x %x)\n", g32(sku), g32(sku + 4), g32(sku + 8));
}

static int prepare(void)                         /* take the device from the platform (and its ME) */
{
    for (int k = 0; k < 10; k++) {
        setb(HW_IF, 0x400000);                   /* PCI_OWN_SET */
        if (!poll(HW_IF, 0x400000, 0x400000, 50)) { setb(MBOX_SET, 1 << 5); return 0; }
        setb(LINK_PWR, 0x80000000u); setb(HW_IF, 0x8000000);   /* WAKE_ME */
        msleep(25);
    }
    return -1;
}
static int nic_on(void)                          /* the "apm init": out of low power, clocks on */
{
    setb(CHICKEN, 0x800000); setb(HPET, 0xffff0000); setb(HW_IF, 0x80000); setb(GIO, 2);
    setb(GP_CNTRL, 4);                           /* INIT_DONE, then wait for MAC_CLOCK_READY */
    return poll(GP_CNTRL, 1, 1, 25000);
}

/* ---- Firmware configuration after it is up. */
static int mac_cmd(int action, int assoc)
{
    uint8_t c[52] = { 0 };
    p32(c + 4, action); p32(c + 8, 5);           /* id 0, BSS station */
    memcpy(c + 12, wifi_mac, 6);
    p32(c + 20, assoc ? 4 : 4 | 8);              /* accept group frames, and beacons until associated */
    p32(c + 32, 1);                              /* no NIC ack (no HE) */
    c[36] = (uint8_t)assoc; p16(c + 40, aid);
    return cmd(C_MAC, c, sizeof c);
}
static uint8_t ac_qos[40];                       /* BK, BE, VI, VO: cw_min, cw_max, aifsn, fifo, txop */
static void qos_set(const uint8_t *wmm)          /* the AP's WMM parameters, or 802.11's defaults */
{
    static const uint8_t def[4][3] = { { 4, 10, 7 }, { 4, 10, 3 }, { 3, 4, 2 }, { 2, 3, 2 } };
    static const uint16_t txop[4] = { 0, 0, 94, 47 };
    static const int aci[4] = { 1, 0, 2, 3 };    /* ACI (BE, BK, VI, VO) -> our order */
    for (int i = 0; i < 4; i++) {
        uint8_t *a = ac_qos + 8 * i;
        p16(a, (1u << def[i][0]) - 1); p16(a + 2, (1u << def[i][1]) - 1); a[4] = def[i][2]; a[5] = (uint8_t)(1 << (i + 1));
        p16(a + 6, txop[i] * 32u);
    }
    for (int k = 0; wmm && k < 4; k++) {
        const uint8_t *r = wmm + 10 + 4 * k;
        uint8_t *a = ac_qos + 8 * aci[r[0] >> 5 & 3];
        p16(a, (1u << (r[1] & 15)) - 1); p16(a + 2, (1u << (r[1] >> 4)) - 1); a[4] = r[0] & 15;
        p16(a + 6, g16(r + 2) * 32);
    }
}
static int link_cmd(int action, int active, uint32_t mask)
{
    uint8_t c[208] = { 0 };
    const uint8_t *e;
    p32(c, action); p32(c + 12, action == ADD ? 0xffffffffu : 0);  /* link 0, MAC 0, PHY 0 */
    memcpy(c + 16, wifi_mac, 6);
    p32(c + 24, mask); p32(c + 28, (uint32_t)active);
    if (action != ADD) {
        p32(c + 36, bss.band24 ? 15 : 0); p32(c + 40, 0x15);        /* ACK rates: CCK; 6, 12, 24 */
        p32(c + 44, bss.cap >> 5 & 1); p32(c + 48, bss.cap >> 10 & 1);
        uint32_t prot = 0;
        if ((e = ie_find(bss.ies, bss.ielen, 61)) && e[1] >= 3 && e[4] & 3) prot |= 6;   /* HT protection */
        if (bss.band24 && (e = ie_find(bss.ies, bss.ielen, 42)) && e[1] && e[2] & 2) prot |= 1;
        p32(c + 52, prot); p32(c + 56, 1 | (ht ? 2 : 0));
        memcpy(c + 60, ac_qos, 32);
        p32(c + 136, (uint32_t)bss.bi); p32(c + 140, (uint32_t)(bss.bi * bss.dtim));
    }
    return cmd(C_LINK, c, sizeof c);
}
static int sta_cmd(int authorized)
{
    uint8_t c[96] = { 0 };
    const uint8_t *e = ht ? ie_find(bss.ies, bss.ielen, 45) : 0;
    memcpy(c + 8, bss.bssid, 6); memcpy(c + 16, bss.bssid, 6);   /* station 0 on link 0, a peer */
    p32(c + 28, (uint32_t)aid); p32(c + 36, !authorized);
    if (e && e[1] >= 5 && e[6] && (e[2] >> 2 & 3)) p32(c + 40, 1);   /* two streams, unless static SM power save */
    if (e && (e[2] >> 2 & 3) == 1) p32(c + 44, 1);
    if (ht) p32(c + 64, 128);
    return cmd(C_STA, c, sizeof c);
}
static int queue_add(txq_t *q, int tid)
{
    uint8_t c[36] = { 0 };
    p32(c + 4, 1); c[8] = (uint8_t)tid; p32(c + 16, 4);              /* station 0; 128 TFDs */
    p64(c + 20, q->pa + NTX * 256); p64(c + 28, q->pa);
    if (cmd(C_QUEUE, c, sizeof c)) return -1;
    q->id = (int)g16(rsp); q->w = q->r = (uint16_t)g16(rsp + 4);
    return 0;
}
static void queue_rm(int tid) { uint8_t c[36] = { 0 }; p32(c, 1); p32(c + 4, 1); p32(c + 8, (uint32_t)tid); cmd(C_QUEUE, c, sizeof c); }
static int key_add(const uint8_t *k, int id, int group)
{
    uint8_t c[80] = { 0 };
    p32(c, ADD); p32(c + 4, 1); p32(c + 8, (uint32_t)id); p32(c + 12, group ? 0x42 : 2);   /* CCMP (+ multicast) */
    memcpy(c + 16, k, 16);
    int r = cmd(C_KEY, c, sizeof c);
    wipe(c, sizeof c);
    return r;
}

/* ---- Frames we send. A slot: command header (4), TX command (AX210: gen3,
 * 28 bytes; 22000: gen2, 20, flags and offload_assist swapped), the 802.11
 * header (then 2 bytes of padding when it is not dword-aligned, which the
 * firmware wants and marks), an optional SNAP, and the body. The body is
 * not copied for data frames: it is netd's bytes straight into the slot. */
static volatile int txlock;
static int tx_frame(txq_t *q, const uint8_t *hdr, int hl, const uint8_t *snap, const uint8_t *body, int n, int host_rate)
{
    int pad = hl & 3 ? 4 - (hl & 3) : 0, sn = snap ? 8 : 0, flen = hl + sn + n, o = qu ? 24 : 32;
    for (long e = now_ms() + 200; (uint16_t)(q->w - q->r) >= NTX - 2; sys_yield()) if (now_ms() > e) return -EAGAIN;
    uint8_t *b = slot(q);
    uint32_t fl = host_rate ? 1 | 2 : 0;                             /* our rate, no encryption; else the firmware's */
    uint32_t off = (uint32_t)(hl / 2) << 8 | (pad ? 1u << 13 : 0);   /* the 802.11 header's length (words), padded */
    memset(b, 0, o);
    b[0] = 0x1c; p16(b + 2, (q->id & 0x1f) << 8 | (q->w & 0xff));
    p16(b + 4, flen);
    p16(b + 6, qu ? off : fl); p32(b + 8, qu ? fl : off);
    if (host_rate) p32(b + 20, (bss.band24 ? 0 : 0x100) | 0x8000);   /* 1 Mb/s CCK or 6 Mb/s OFDM, antenna B */
    memcpy(b + o, hdr, hl); memset(b + o + hl, 0, pad);
    if (snap) memcpy(b + o + hl + pad, snap, 8);
    memcpy(b + o + hl + pad + sn, body, n);
    tfd_put(q, o + hl + pad + sn + n, flen);
    return 0;
}
static int tx_mgmt(int sub, const uint8_t *body, int n)
{
    uint8_t h[24] = { (uint8_t)(sub << 4) };
    memcpy(h + 4, bss.bssid, 6); memcpy(h + 10, wifi_mac, 6); memcpy(h + 16, bss.bssid, 6);
    p16(h + 22, (uint32_t)(seq_m++ << 4));
    return tx_frame(&mgq, h, 24, 0, body, n, 1);
}
/* One data frame (QoS, TID 0) to dst: the 802.11 header, SNAP, the payload. */
static long tx_data(const uint8_t *dst, int type, const uint8_t *p, int n)
{
    uint8_t h[26] = { 0x88, (uint8_t)(keys_on ? 0x41 : 0x01) };      /* QoS data to the AP (+ protected) */
    uint8_t snap[8] = { 0xaa, 0xaa, 3, 0, 0, 0, (uint8_t)(type >> 8), (uint8_t)type };
    memcpy(h + 4, bss.bssid, 6); memcpy(h + 10, wifi_mac, 6); memcpy(h + 16, dst, 6);
    while (__atomic_exchange_n(&txlock, 1, __ATOMIC_ACQUIRE)) sys_yield();
    p16(h + 22, (uint32_t)(seq_d++ << 4));
    long r = tx_frame(&dataq, h, 26, snap, p, n, !keys_on);
    __atomic_store_n(&txlock, 0, __ATOMIC_RELEASE);
    return r;
}
long chip_send(uint8_t *f, size_t len)
{
    for (size_t off = 0; off + 2 <= len; ) {
        uint32_t fl = f[off] | f[off + 1] << 8;
        if (fl < 14 || fl > 1514 || off + 2 + fl > len) return -EINVAL;
        const uint8_t *e = f + off + 2;
        if (wifi_state == WIFI_UP && tx_data(e, e[12] << 8 | e[13], e + 14, (int)fl - 14) < 0) return -EAGAIN;
        off += 2 + fl;
    }
    return 0;
}

/* ---- Scanning: passive (the firmware listens on each channel), all of
 * 2.4 GHz and 5 GHz; beacons arrive as frames (scan_add). */
static int do_scan(void)
{
    static uint8_t c[1940];
    static const uint8_t ch5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
                                   132, 136, 140, 144, 149, 153, 157, 161, 165 };
    memset(c, 0, sizeof c);
    p32(c + 4, 6);                               /* priority */
    uint8_t *g = c + 8, *cp = g + 36;
    p16(g, 1 << 11 | 1 << 7 | 1 << 1);           /* passive, adaptive dwell, pass all frames */
    g[4] = g[5] = 10; g[6] = 2; g[7] = 8; g[8] = 10; p16(g + 10, 300); p32(g + 28, 6); g[32] = g[33] = 110;
    int n = 0;
    for (int i = 1; i <= 13; i++, n++) {         /* band 2.4 GHz: in the flags (scan v17), else its own byte (v15) */
        if (qu) cp[9 + 8 * n] = 1; else p32(cp + 4 + 8 * n, 1u << 30);
        cp[8 + 8 * n] = (uint8_t)i; cp[10 + 8 * n] = 1;
    }
    for (unsigned i = 0; i < sizeof ch5; i++, n++) { cp[8 + 8 * n] = ch5[i]; cp[10 + 8 * n] = 1; }
    cp[0] = 1 << 5; cp[1] = (uint8_t)n; cp[2] = 10; cp[3] = 2;     /* channel order; how many */
    c[8 + 576 + 2] = 1;                          /* one iteration */
    nfound = 0; bss.found = 0; scanning = 1;
    ev_clear(EV_SCAN);
    int ok = !cmd(C_SCAN, c, sizeof c) && wait_ev(EV_SCAN, 15000);
    scanning = 0;
    if (!ok) printf("wifi: the scan did not complete\n");
    return nfound;
}

/* ---- Joining. */
static void fail(const char *why)
{
    printf("wifi: join failed: %s\n", why);
    strlcpy(wifi_why, why, sizeof wifi_why);
    wifi_state = WIFI_FAILED;
}
static int joined;                               /* firmware contexts set up (to undo) */
static void teardown(void)
{
    if (!joined) return;
    if (wifi_state == WIFI_UP) { uint8_t r[2] = { 3, 0 }; tx_mgmt(12, r, 2); msleep(20); }   /* deauthenticate */
    if (wifi_state == WIFI_UP) wifi_link(0);
    queue_rm(15); queue_rm(0);
    cmd4(C_STA_RM, 0);
    aid = 0; keys_on = 0;
    mac_cmd(MODIFY, 0);
    link_cmd(MODIFY, 0, 1);                      /* inactive */
    uint8_t c[32] = { 0 }; p32(c + 4, REMOVE); cmd(C_PHY, c, sizeof c);
    joined = 0;
}
static int eapol(int ms, int *done)              /* one EAPOL-Key step: wait, answer -> 0 */
{
    static uint8_t out[256];
    int len;
    for (long e = now_ms() + ms; now_ms() < e; ) {
        uint8_t *f = box_get((int)(e - now_ms()), &len);
        if (!f) break;
        if (!f[0]) continue;
        int n = wpa_rx(&wpa, f + 1, len, out, done);
        if (n < 0) return -1;
        if (n) return (int)tx_data(bss.bssid, 0x888e, out, n) < 0 ? -1 : 0;
    }
    return -2;
}
static void join(void)
{
    const uint8_t *e, *rsn;
    uint8_t c[96];
    int len;
    keys_on = 0; ht = 0; aid = 0;
    printf("wifi: looking for \"%s\"\n", bss.ssid);
    do_scan();
    if (!bss.found) return fail("network not found");
    rsn = ie_find(bss.ies, bss.ielen, 0x30);
    wpa_on = bss.pass[0] != 0;
    if (wpa_on && (!rsn || !wpa_rsn_ok(rsn))) return fail("not a WPA2-PSK (CCMP) network");
    if (!wpa_on && (bss.cap & 0x10)) return fail("the network needs a passphrase");
    memcpy(wifi_bssid, bss.bssid, 6); wifi_chan = bss.chan; wifi_rssi = bss.rssi;
    printf("wifi: %02x:%02x:%02x:%02x:%02x:%02x channel %d, %d dBm, %s\n", bss.bssid[0], bss.bssid[1], bss.bssid[2],
           bss.bssid[3], bss.bssid[4], bss.bssid[5], bss.chan, bss.rssi, wpa_on ? "WPA2" : "open");
    const uint8_t *wmm = 0;
    for (e = bss.ies; e && (e = ie_find(e, bss.ielen - (int)(e - bss.ies), 0xdd)); e += 2 + e[1])
        if (e[1] >= 24 && !memcmp(e + 2, "\x00\x50\xf2\x02\x01", 5)) { wmm = e; break; }
    ht = wmm && ie_find(bss.ies, bss.ielen, 45);
    qos_set(wmm);
    if (wpa_on) {                                /* the key from the passphrase, our nonce */
        memset(&wpa, 0, sizeof wpa);
        wpa_pmk(wpa.pmk, bss.pass, bss.ssid);
        memcpy(wpa.aa, bss.bssid, 6); memcpy(wpa.spa, wifi_mac, 6);
        wpa.ap_rsn = rsn; sys_random(wpa.snonce, 32);
    }
    /* The channel (PHY context 0), the link on it, the AP's station and its queues. */
    memset(c, 0, 32); p32(c + 4, ADD); p32(c + 8, (uint32_t)bss.chan); c[12] = (uint8_t)bss.band24;
    joined = 1;
    if (cmd(C_PHY, c, 32)) return teardown(), fail("the firmware refused the channel");
    memset(c, 0, 32); p32(c + 4, 2u << 1 | ant << 1 | 2u << 10 | 2u << 12); cmd(C_RLC, c, 32);
    if (link_cmd(MODIFY, 1, 0x1f) || sta_cmd(0) || queue_add(&mgq, 15) || queue_add(&dataq, 0))
        return teardown(), fail("the firmware refused the station");
    seq_m = seq_d = 0;
    memset(c, 0, 24); p32(c + 4, ADD); p32(c + 12, 900);           /* stay on the channel 900 TU */
    ev_clear(EV_SESSION);
    cmd(C_SESSION, c, 24);
    if (!wait_ev(EV_SESSION, 1000)) printf("wifi: no session protection notice (going on)\n");
    while (box_get(0, &len)) ;
    /* Open System authentication, then association. */
    static const uint8_t auth[6] = { 0, 0, 1, 0, 0, 0 };
    uint8_t *f = 0;
    for (int t = 0; t < 3 && !f; t++) {
        tx_mgmt(11, auth, 6);
        for (long dl = now_ms() + 300; (f = box_get((int)(dl - now_ms()), &len)) && (f[0] || len < 30 || (g16(f + 1) >> 4 & 15) != 11); ) ;
    }
    if (!f) return teardown(), fail("no answer to authentication");
    if (g16(f + 1 + 28)) return teardown(), fail("authentication refused");
    printf("wifi: authenticated\n");
    static uint8_t a[400];
    int n = 0;
    p16(a, 1 | (bss.cap & 0x420) | (wpa_on ? 0x10 : 0)); p16(a + 2, 10); n = 4;   /* capabilities, listen interval */
    a[n++] = 0; a[n++] = (uint8_t)strlen(bss.ssid); memcpy(a + n, bss.ssid, strlen(bss.ssid)); n += (int)strlen(bss.ssid);
    if ((e = ie_find(bss.ies, bss.ielen, 1))) { memcpy(a + n, e, 2 + e[1]); n += 2 + e[1]; }    /* the AP's rates */
    if ((e = ie_find(bss.ies, bss.ielen, 50))) { memcpy(a + n, e, 2 + e[1]); n += 2 + e[1]; }
    if (wpa_on) { memcpy(a + n, wpa_ie, sizeof wpa_ie); n += (int)sizeof wpa_ie; }
    if (ht) {                                    /* HT 20 MHz: SGI, SM power save off, MCS 0-15 */
        static const uint8_t htc[28] = { 45, 26, 0x2c, 0, 3, 0xff, 0xff };
        memcpy(a + n, htc, 28); n += 28;
    }
    if (wmm) { memcpy(a + n, "\xdd\x07\x00\x50\xf2\x02\x00\x01\x00", 9); n += 9; }
    f = 0;
    for (int t = 0; t < 3 && !f; t++) {
        tx_mgmt(0, a, n);
        for (long dl = now_ms() + 500; (f = box_get((int)(dl - now_ms()), &len)) && (f[0] || len < 30 || (g16(f + 1) >> 4 & 15) != 1); ) ;
    }
    if (!f) return teardown(), fail("no answer to association");
    if (g16(f + 1 + 26)) { printf("wifi: association refused (status %u)\n", g16(f + 1 + 26)); return teardown(), fail("association refused"); }
    aid = (int)(g16(f + 1 + 28) & 0x3fff);
    for (e = f + 1 + 30; (e = ie_find(e, len - (int)(e - f - 1), 0xdd)); e += 2 + e[1])
        if (e[1] >= 24 && !memcmp(e + 2, "\x00\x50\xf2\x02\x01", 5)) { qos_set(e); break; }
    printf("wifi: associated (AID %d%s)\n", aid, ht ? ", HT" : "");
    /* Associated: the station, the link, the MAC; rates chosen by the firmware. */
    sta_cmd(0); link_cmd(MODIFY, 1, 0x1e); mac_cmd(MODIFY, 1);
    memset(c, 0, 28); c[6] = ant;
    uint32_t rates = 0;
    static const uint8_t legacy[12] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
    for (int k = 1; k <= 50; k += 49)
        if ((e = ie_find(bss.ies, bss.ielen, k))) for (int i = 0; i < e[1]; i++)
            for (int r = 0; r < 12; r++) if ((e[2 + i] & 0x7f) == legacy[r]) rates |= 1u << r;
    p16(c + 10, rates);
    if (ht && (e = ie_find(bss.ies, bss.ielen, 45)) && e[1] >= 5) {
        c[5] = 1; c[7] = e[2] >> 5 & 1;          /* HT; short guard interval at 20 MHz */
        p16(c + 12, e[5]); p16(c + 18, e[6]);    /* MCS 0-7, 8-15 */
    }
    cmd(C_TLC, c, 28);
    if (wpa_on) {                                /* the 4-way handshake */
        int done = 0, r;
        if ((r = eapol(3000, &done)) || (r = eapol(3000, &done)) || done != WPA_PTK) {
            teardown();
            return fail(r == -1 ? "wrong passphrase (handshake refused)" : "no handshake from the AP");
        }
        printf("wifi: 4-way handshake done, keys installed (group key %d)\n", wpa.gtkid);
        for (long dl = now_ms() + 200; dataq.r != dataq.w && now_ms() < dl; ) msleep(1);   /* message 4 sent first */
        key_add(wpa.ptk + 32, 0, 0); key_add(wpa.gtk, wpa.gtkid, 1);
        keys_on = 1;
    }
    sta_cmd(1);
    memset(c, 0, 24); p32(c + 4, REMOVE); cmd(C_SESSION, c, 24);
    ev_clear(EV_LOST);
    wifi_link(1);
}

/* ---- The joining thread: requests from wifid, then the link's life. */
enum { R_JOIN = 1, R_LEAVE = 2, R_SCAN = 4 };
static volatile int req, scan_gen;
static int mgr_tid;
static void manager(void *arg)
{
    (void)arg;
    long retry = 0;
    for (;;) {
        int r = __atomic_exchange_n(&req, 0, __ATOMIC_ACQUIRE), len;
        if (r & (R_LEAVE | R_JOIN)) { teardown(); if (wifi_state == WIFI_UP) wifi_state = WIFI_IDLE; retry = 0; }
        if (r & R_JOIN) join();
        if (r & R_SCAN) { if (!joined) do_scan(); scan_gen++; }
        uint8_t *f;
        while (wifi_state == WIFI_UP && (f = box_get(0, &len))) {
            int sub = f[0] ? -1 : (int)(g16(f + 1) >> 4 & 15), done;
            if (f[0]) { if (!eapol(0, &done) && done == WPA_GTK) key_add(wpa.gtk, wpa.gtkid, 1); }   /* group rekey */
            else if (sub == 10 || sub == 12) { printf("wifi: %s by the AP (reason %u)\n", sub == 12 ? "deauthenticated" : "disassociated", len >= 26 ? g16(f + 25) : 0); ev_set(EV_LOST); }
            else if (sub == 13 && len >= 33 && f[25] == 3 && f[26] == 0) {   /* a block-ack request: declined */
                uint8_t b[9] = { 3, 1, f[27], 37, 0, f[28], f[29], f[30], f[31] };
                tx_mgmt(13, b, 9);
            }
        }
        if (__atomic_load_n(&ev, __ATOMIC_ACQUIRE) & EV_LOST) {
            ev_clear(EV_LOST); printf("wifi: connection lost\n");
            teardown(); wifi_state = WIFI_IDLE; retry = now_ms() + 3000;
        }
        if (retry && now_ms() > retry && bss.ssid[0] && !joined) { retry = 0; wifi_state = WIFI_JOINING; join(); if (!joined) retry = now_ms() + 10000; }
        if (!req) msleep(50);
    }
}
void chip_join(const char *ssid, const char *pass)
{
    strlcpy(bss.ssid, ssid, sizeof bss.ssid); strlcpy(bss.pass, pass, sizeof bss.pass);
    __atomic_or_fetch(&req, R_JOIN, __ATOMIC_RELEASE);
}
void chip_leave(void) { bss.ssid[0] = 0; __atomic_or_fetch(&req, R_LEAVE, __ATOMIC_RELEASE); }
int chip_scan(wifi_net_t *list)
{
    int g = scan_gen;
    __atomic_or_fetch(&req, R_SCAN, __ATOMIC_RELEASE);
    for (long e = now_ms() + 20000; scan_gen == g && now_ms() < e; ) msleep(20);
    memcpy(list, found, sizeof found);
    return nfound;
}

/* ---- Bringing the device up. */
/* The devices: the AX210 (discrete), then the 22000 family's CNVi in Intel's
 * chipsets, with their SoC latency (Linux's cfg/22000.c and init.c): flags
 * (integrated, LTR delay 200 / 2500 / 1820 us, low-latency crystal), us. */
static const struct { uint16_t id, soc, lat; } devs[] = {
    { 0x2725, 1, 500 },                                  /* AX210 */
    { 0x34f0, 3 << 2, 1820 }, { 0x3df0, 3 << 2, 1820 }, { 0x4df0, 3 << 2, 1820 },   /* Ice Lake-LP; Linux's other medium-latency ones */
    { 0x02f0, 1 << 2, 500 }, { 0x06f0, 1 << 2, 500 },   /* Comet Lake */
    { 0x43f0, 2 << 2 | 2, 12000 }, { 0xa0f0, 2 << 2 | 2, 12000 },   /* Tiger Lake (long latency) */
};
int chip_up(void)
{
    pci_t p;
    uint64_t len, pa;
    unsigned d = 0;
    while (pci_scan(&p, 0, 0x8086u | (uint32_t)devs[d].id << 16, ~0u, 0)) if (++d == sizeof devs / sizeof *devs) return 1;
    qu = d > 0;
    if (!(pa = pci_bar(&p, 0, &len)) || (long)(regs = map_phys(pa, len)) < 0) return -1;
    bus_off = p.bus_off;
    pci_cfgw(&p, 0x40, pci_cfg(&p, 0x40) & ~0xff00u);   /* PCI retry timeout 0 (as Linux) */
    uint32_t hw_rev = rd(HW_REV), type = hw_rev >> 4 & 0xfff, rf;
    printf("wifi: Intel %s at PCI %x:%x.%x (8086:%04x), BAR %x (%u KiB), rev %x\n", qu ? "22000 CNVi" : "AX210",
           p.at >> 16, p.at >> 11 & 31, p.at >> 8 & 7, devs[d].id, (unsigned)pa, (unsigned)(len >> 10), hw_rev);
    if (hw_rev == 0xffffffff || prepare()) { printf("wifi: the device does not answer\n"); return -1; }
    setb(RESET, 0x80); msleep(6);                /* software reset */
    if (prepare() || nic_on()) { printf("wifi: the device did not wake up\n"); return -1; }
    if (qu) umac = 0, rxd = 48, rxe = 32;        /* the 22000: no UMAC offset, the older RX descriptor */
    prph_w(umac + U_CHICK, 1u << 24);            /* interrupts as MSI/INTx (unused: we poll) */
    uint32_t rf_id = rd(RF_ID);
    printf("wifi: RF id %x%s\n", rf_id, rd(GP_CNTRL) & 1u << 27 ? "" : ", radio switched off (rfkill)");

    /* The firmware: the 22000's by MAC type and step (Qu B0, Qu C0, QuZ) and radio (hr: AX201; jf: 9560). */
    char fwp[64], *name = fwp + sizeof FWDIR - 1;
    size_t fl = strlcpy(fwp, FWDIR, sizeof fwp);
    rf = rf_id >> 12 & 0xfff;
    fl += strlcpy(fwp + fl, !qu ? "ty-a0-gf-a0" : type == 0x35 ? "QuZ-a0" : (hw_rev & 15) >= 2 ? "Qu-c0" : "Qu-b0", sizeof fwp - fl);
    strlcpy(fwp + fl, !qu ? "-89.ucode" : rf == 0x105 || rf == 0x108 ? "-jf-b0-77.ucode" : "-hr-b0-77.ucode", sizeof fwp - fl);
    if (qu) printf("wifi: 22000 family, MAC type %x step %u, radio type %x: %s\n", type, hw_rev & 15, rf, name);
    fw = (uint8_t *)file_get(fwp, &fwlen);
    pnvm = qu ? 0 : (uint8_t *)file_get(PNVM, &pnvmlen);
    if (!fw) { printf("wifi: the firmware is missing (%s): build the USB key with 'make usb' (docs/net.md)\n", fwp); return -1; }
    printf("wifi: firmware %s (%u bytes)\n", fw + 8, (unsigned)fwlen);

    /* DMA memory: misc (context info, RX rings and status), scratch, queues. */
    uint64_t rx_pa, img_pa, pg_pa, iml_pa = 0;
    if (!(misc = dmem(3 * 4096, &misc_pa)) || !(scratch = dmem(8192, &scratch_pa)) ||
        !(rxbuf = dmem(NRX * 4096, &rx_pa)) || txq_alloc(&cmdq, 1) || txq_alloc(&mgq, 4) || txq_alloc(&dataq, NTX)) return -1;
    rxbuf_pa = rx_pa; fbd = misc + 0x800; ubd = misc + 0x1000;
    stts = (volatile uint16_t *)(misc + 0x2000);
    for (unsigned i = 0; i < NRX - 1; i++) rb_give(i, i + 1);
    dram = qu ? misc + 192 : scratch + 124;      /* where the sections are listed */
    uint8_t *img, *paging, *imlbuf = 0;
    size_t imgsz = 0;
    const uint8_t *iml = 0;
    uint32_t imllen = 0;
    if (load_sections(&img, &paging, &img_pa, &pg_pa, &imgsz, &iml, &imllen) || (iml && !(imlbuf = dmem(imllen, &iml_pa)))) {
        printf("wifi: the firmware file is not usable\n"); return -1;
    }
    uint8_t *ci = misc;
    if (qu) {                                    /* the context info v1: 1792 bytes, all in it */
        p16(ci, hw_rev); p16(ci + 4, 1792 / 4);
        p32(ci + 8, 0x100 | 7 << 4 | 4 << 9);    /* long TFDs, 2^7 RX buffers of 4 KiB */
        p64(ci + 24, misc_pa + 0x800); p64(ci + 32, misc_pa + 0x1000); p64(ci + 40, misc_pa + 0x2000);   /* RX: free, used, status */
        p64(ci + 48, cmdq.pa); ci[56] = 4;       /* the command queue: log2(128) - 3 */
    } else {
        memcpy(imlbuf, iml, imllen);
        /* The scratch: version, control (4 KiB RX buffers, 256-byte TFDs), the RX free ring. */
        p16(scratch, hw_rev); p16(scratch + 4, 1724 / 4);
        p32(scratch + 8, 1u << 16 | 1u << 17 | 0xc0000u);
        p64(scratch + 48, misc_pa + 0x800);
        /* The context info: where the rest is. */
        p64(ci + 8, misc_pa + 0x400); p64(ci + 16, misc_pa + 0x2000);  /* peripheral info; RX status */
        p64(ci + 24, misc_pa + 0x600); p64(ci + 32, misc_pa + 0x700);  /* (dummy tail pointers) */
        p64(ci + 52, cmdq.pa); p64(ci + 60, misc_pa + 0x1000);
        p16(ci + 68, 4); p16(ci + 70, 7);                              /* log2(128) - 3, log2(128) */
        p64(ci + 88, scratch_pa); p32(ci + 96, 1660);
    }

    thread_start(receiver, 0, 16384);
    wr(INT, ~0u); wr(INT_MASK, 0);               /* no interrupts */
    wr(GP1_CLR, 2); wr(GP1_CLR, 4);
    if (nic_on()) { printf("wifi: the device did not wake up\n"); return -1; }
    if (qu)                                      /* the chip's and radio's steps (mvm's nic_config) */
        wr(HW_IF, (rd(HW_IF) & ~0xff0fu) | (hw_rev & 15) | (phy & 3) << 10 | (phy >> 2 & 3) << 14 | (phy >> 4 & 3) << 12);
    setb(SHADOW, 0x800fffff);
    if (qu) {
        w64(CTXT_BA, misc_pa);                   /* the context info: the ROM loads from it */
        prph_w(LTR_MAC, 0xf); prph_w(LTR_UMAC, 0x88fa88fa);   /* 250 us latency during boot (integrated) */
    } else {
        w64(CTXT_ADDR, misc_pa); w64(IML_ADDR, iml_pa); wr(IML_SIZE, imllen);
        setb(HW_IF, 2);                          /* boot from the context info */
        wr(LTR, 0x88fa88fa);                     /* 250 us latency during boot (as Linux) */
    }
    printf("wifi: starting the firmware (context info v%d)\n", qu ? 1 : 2);
    ev_clear(EV_ALIVE);
    prph_w(umac + U_CPU_RUN, 1);
    int alive = 0;
    for (int k = 0; k < 100 && !(alive = wait_ev(EV_ALIVE, 10)); k++) wr(RX_WIDX, rx_w & ~7u);   /* buffers once it set up RX */
    if (!alive) { printf("wifi: the firmware did not start (no ALIVE; CSR_INT %x, GP_CNTRL %x)\n", rd(INT), rd(GP_CNTRL)); return -1; }
    if (g16(rsp) != 0xcafe) { printf("wifi: the firmware reports an error at start (%x)\n", g16(rsp)); return -1; }
    dma_free(img, imgsz); if (imlbuf) dma_free(imlbuf, imllen);
    uint8_t sku[12];
    memcpy(sku, rsp + 116, 12);
    if (!qu) load_pnvm(sku, hw_rev, rf_id);
    free(fw); free(pnvm); fw = pnvm = 0;

    /* The firmware's own initialization, then our configuration. */
    ev_clear(EV_INIT);
    if (cmd4(C_INIT_CFG, 2) || cmd4(C_NVM_DONE, 0) || !wait_ev(EV_INIT, 2000)) {
        printf("wifi: the firmware did not finish its initialization\n"); return -1;
    }
    uint32_t m0 = rd(MAC_STRAP), m1 = rd(MAC_STRAP + 4);
    if (!(m0 | m1) || (m0 >> 24 & 1)) m0 = rd(MAC_OTP), m1 = rd(MAC_OTP + 4);
    uint8_t mac[6] = { (uint8_t)(m0 >> 24), (uint8_t)(m0 >> 16), (uint8_t)(m0 >> 8), (uint8_t)m0, (uint8_t)(m1 >> 8), (uint8_t)m1 };
    memcpy(wifi_mac, mac, 6);
    printf("wifi: NVM MAC %02x:%02x:%02x:%02x:%02x:%02x, antennas %x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ant);
    uint8_t c[28] = { 0 }, sc[12] = { 0 };
    p16(c, 'Z' << 8 | 'Z');
    p32(sc + 4, ant); p32(sc + 8, ant);          /* scan: TX and RX chains */
    if (cmd4(C_TX_ANT, ant) || cmd32(C_BT, 3, 0) || cmd32(C_SOC, devs[d].soc, devs[d].lat) || cmd4(C_DQA, 0) ||
        cmd4(C_POWER, 0) || cmd(C_MCC, c, 28) || cmd(C_SCAN_CFG, sc, 12) || mac_cmd(ADD, 0) || link_cmd(ADD, 0, 0)) {
        printf("wifi: the firmware refused its configuration\n"); return -1;
    }
    fl = strlcpy(wifi_desc, qu ? "Intel 22000 CNVi, " : "Intel AX210, ", sizeof wifi_desc);
    strlcpy(wifi_desc + fl, name, sizeof wifi_desc - fl);
    mgr_tid = (int)thread_start(manager, 0, 16384);
    printf("wifi: ready\n");
    return 0;
}
