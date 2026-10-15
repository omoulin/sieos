/*
 * wifid - The Wi-Fi server: a FullMAC driver for the Raspberry Pi 4's and
 * Pi 5's CYW43455 (on SDIO), serving the port "wifi". It brings the chip up,
 * downloads its firmware, scans, joins open and WPA2-PSK networks (the
 * firmware's own supplicant does the handshake), and carries data frames to
 * and from the network server (netd) as a second interface.
 *
 *   netd ◄──NET_FRAMES / NET_LINK── wifi ──SDIO──► the CYW43455
 *        ──NIC_SEND / NIC_INFO────► (port "wifi", like vnet's "nic0")
 *
 *   Settings (the desktop) and /bin/wifi send WIFI_SCAN, WIFI_JOIN (root:
 *   the passphrase is kept in /etc/wifi.conf, root only), WIFI_STATUS. The
 *   chosen network is rejoined at every start.
 *
 * The chip over SDIO (sdio.c): function 1 is a window onto the chip's
 * backplane (its cores and RAM), function 2 carries frames. Bring-up
 * (sbinit, fwload, sbenable) follows Plan 9's ether4330; the control
 * protocol (BCDC over SDPCM) and the WPA2 iovars follow Linux's brcmfmac.
 *
 * UNTESTED ON HARDWARE (QEMU has neither the controller's card side nor the
 * chip): what to check on a real board is in docs/raspberrypi.md.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "wifi.h"
#include "mk/fdt.h"

/* ---- Small helpers. */
static void nic_udelay(uint64_t us) { long end = sys_clock() + (long)us * 1000; while (sys_clock() < end) ; }
static char *fdt;
const void *wifi_fdt(void)
{
    if (!fdt) {
        long size = sys_fdt(0, 0);
        char *f = size > 0 ? malloc(size) : 0;
        if (f && sys_fdt(f, size) == size) fdt = f; else { free(f); return 0; }
    }
    return fdt;
}
struct sbuf { char *p; size_t n, cap; };
static void sput(void *c, char ch) { struct sbuf *b = c; if (b->n + 1 < b->cap) b->p[b->n++] = ch; }
void wifi_fmt(char *out, size_t cap, const char *fmt, ...)
{
    struct sbuf b = { out, 0, cap };
    va_list ap; va_start(ap, fmt); vformat(sput, &b, fmt, ap); va_end(ap);
    if (cap) out[b.n] = 0;
}
/* One lock for the SDIO bus: the receiver thread and the main thread both
 * talk to the chip, one transaction at a time. */
static volatile int sdlock;
static int state = WIFI_OFF;                     /* WIFI_IDLE, WIFI_JOINING, WIFI_UP ... */
static void lock(void)   { while (__atomic_exchange_n(&sdlock, 1, __ATOMIC_ACQUIRE)) sys_yield(); }
static void unlock(void) { __atomic_store_n(&sdlock, 0, __ATOMIC_RELEASE); }

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void p16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* ---- Backplane access over SDIO function 1. */
enum { SBADDR = 0x1000A, FRAMECTRL = 0x1000D, CLKCSR = 0x1000E, PULLUPS = 0x1000F,
       WFRAMECNT = 0x10019, RFRAMECNT = 0x1001B };
#define SB32BIT  0x8000
#define SBWSIZE  0x8000
#define ENUMBASE 0x18000000
/* CLKCSR bits. */
enum { FORCE_ALP = 0x01, FORCE_HT = 0x02, REQ_ALP = 0x08, REQ_HT = 0x10, NOHWREQ = 0x20, ALP_AVAIL = 0x40, HT_AVAIL = 0x80 };
/* A core's control registers. */
enum { IOCTRL = 0x408, RESETCTRL = 0x800 };
/* The SDIO core's registers (from its base). */
enum { INTSTATUS = 0x20, INTMASK = 0x24, SBMBOX = 0x40, SBMBOXDATA = 0x48, HOSTMBOXDATA = 0x4C };
#define I_HMB_SW_MASK 0x000000F0
#define FRAME_INT     (1 << 6)
#define MAILBOX_INT   (1 << 7)
#define FC_CHANGE     (1 << 5)
/* ARM Cortex-R4 core (the 43455's). */
enum { CR4_CAP = 0x04, CR4_BANKIDX = 0x40, CR4_BANKINFO = 0x44, CR4_CPUHALT = 0x20 };

static uint32_t chipcommon, armctl, armregs, d11ctl, sdregs, rambase, socramsize;
static int sdiorev;

static void cfgw(uint32_t off, int v) { sdio_wb(1, off, v); }
static int  cfgr(uint32_t off) { return sdio_rb(1, off); }
static uint32_t cfgreadl(uint32_t off)
{
    uint8_t b[4] = { 0 };
    sdio_rw(1, 0, off | SB32BIT, b, 4, 1);
    return g32(b);
}
static void cfgwritel(uint32_t off, uint32_t v)
{
    uint8_t b[4];
    p32(b, v);
    sdio_rw(1, 1, off | SB32BIT, b, 4, 1);
}
static void sbwindow(uint32_t addr)
{
    addr &= ~(SBWSIZE - 1);
    cfgw(SBADDR, addr >> 8); cfgw(SBADDR + 1, addr >> 16); cfgw(SBADDR + 2, addr >> 24);
}
/* Read or write chip memory across window boundaries (byte granularity). */
static void sbmem(int write, uint8_t *buf, int len, uint32_t off)
{
    uint32_t n = ((off + SBWSIZE) & ~(SBWSIZE - 1)) - off;
    if (!n) n = SBWSIZE;
    while (len > 0) {
        if ((int)n > len) n = (uint32_t)len;
        sbwindow(off);
        sdio_rw(1, write, (off & (SBWSIZE - 1)) | SB32BIT, buf, (int)(n & ~3u ? n : 4), 1);
        off += n; buf += n; len -= (int)n; n = SBWSIZE;
    }
}

/* ---- Core enumeration and reset (the backplane). */
static void corescan(uint32_t r)
{
    static uint8_t buf[512];
    int coreid = 0;
    sbmem(0, buf, sizeof buf, r);
    for (unsigned i = 0; i < sizeof buf; i += 4) {
        int tag = buf[i] & 0xF;
        if (tag == 0xF) return;                  /* end */
        if (tag == 0x1) { if ((buf[i + 4] & 0xF) != 0x1) continue; coreid = (buf[i + 1] | buf[i + 2] << 8) & 0xFFF; i += 4; }
        else if (tag == 0x5) {
            uint32_t addr = ((uint32_t)buf[i + 1] << 8 | (uint32_t)buf[i + 2] << 16 | (uint32_t)buf[i + 3] << 24) & ~0xFFFu;
            int ctl = buf[i] & 0xC0;
            if (coreid == 0x800 && !ctl) chipcommon = addr;
            else if (coreid == 0x83E) { if (ctl) { if (!armctl) armctl = addr; } else if (!armregs) armregs = addr; }
            else if (coreid == 0x829 && !ctl) { sdregs = addr; }
            else if (coreid == 0x812 && ctl) d11ctl = addr;
        }
    }
}
static void sbdisable(uint32_t regs, int pre, int ioctl)
{
    sbwindow(regs);
    if (cfgreadl(regs + RESETCTRL) & 1) { cfgwritel(regs + IOCTRL, 3 | ioctl); cfgreadl(regs + IOCTRL); return; }
    cfgwritel(regs + IOCTRL, 3 | pre); cfgreadl(regs + IOCTRL);
    cfgwritel(regs + RESETCTRL, 1); nic_udelay(10);
    while (!(cfgreadl(regs + RESETCTRL) & 1)) ;
    cfgwritel(regs + IOCTRL, 3 | ioctl); cfgreadl(regs + IOCTRL);
}
static void sbreset(uint32_t regs, int pre, int ioctl)
{
    sbdisable(regs, pre, ioctl);
    sbwindow(regs);
    while (cfgreadl(regs + RESETCTRL) & 1) { cfgwritel(regs + RESETCTRL, 0); nic_udelay(40); }
    cfgwritel(regs + IOCTRL, 1 | ioctl); cfgreadl(regs + IOCTRL);
}
static void ramscan(void)                        /* the 43455 is a Cortex-R4: banks give the RAM size */
{
    uint32_t r = armregs;
    sbwindow(r);
    uint32_t n = cfgreadl(r + CR4_CAP);
    int banks = ((n >> 4) & 0xF) + (n & 0xF);
    socramsize = 0;
    for (int i = 0; i < banks; i++) { cfgwritel(r + CR4_BANKIDX, i); socramsize += 8192 * ((cfgreadl(r + CR4_BANKINFO) & 0x3F) + 1); }
    rambase = 0x198000;
}
static int sbinit(char *chip, size_t cap)
{
    sbwindow(ENUMBASE);
    uint32_t r = cfgreadl(ENUMBASE);
    int chipid = r & 0xFFFF;
    if (chipid != 0x4345) { printf("wifi: chip %x is not a CYW43455 (not supported)\n", chipid); return -1; }
    corescan(cfgreadl(ENUMBASE + 63 * 4));
    if (!armctl || !d11ctl || !sdregs) { printf("wifi: the backplane scan did not find the cores\n"); return -1; }
    sdiorev = 0;
    sbreset(armctl, CR4_CPUHALT, CR4_CPUHALT);
    sbreset(d11ctl, 8 | 4, 4);
    ramscan();
    cfgw(CLKCSR, 0); nic_udelay(10);
    cfgw(CLKCSR, NOHWREQ | REQ_ALP);
    for (int k = 0; k < 500 && !(cfgr(CLKCSR) & (HT_AVAIL | ALP_AVAIL)); k++) nic_udelay(10);
    cfgw(CLKCSR, NOHWREQ | FORCE_ALP); nic_udelay(65);
    cfgw(PULLUPS, 0);
    sbwindow(chipcommon);
    cfgwritel(chipcommon + 0x58, 0); cfgwritel(chipcommon + 0x5C, 0);   /* GPIO pull-ups/downs off */
    wifi_fmt(chip, cap, "CYW43455 (chip %x rev %ld), %u KiB RAM at %x", chipid, (r >> 16) & 0xF, socramsize >> 10, rambase);
    return 0;
}
static int sbenable(void)
{
    cfgw(CLKCSR, 0); nic_udelay(1000);
    cfgw(CLKCSR, REQ_HT);
    for (int k = 0; k < 50; k++) { if (cfgr(CLKCSR) & HT_AVAIL) break; nic_udelay(100000); }
    if (!(cfgr(CLKCSR) & HT_AVAIL)) { printf("wifi: the chip's HT clock did not come up\n"); return -1; }
    cfgw(CLKCSR, cfgr(CLKCSR) | FORCE_HT); nic_udelay(10000);
    sbwindow(sdregs);
    cfgwritel(sdregs + SBMBOXDATA, 4 << 16);     /* our protocol version */
    cfgwritel(sdregs + INTMASK, FRAME_INT | MAILBOX_INT | FC_CHANGE);
    return sdio_enable_f2();
}

/* ---- Firmware. The files are copied onto the card (make sdcard), from the
 * RPi-Distro firmware-nonfree repository (docs/net.md pins the commit). */
#define FW_DIR  "/lib/firmware/brcm/"
#define FW_BIN  FW_DIR "brcmfmac43455-sdio.bin"
#define FW_CLM  FW_DIR "brcmfmac43455-sdio.clm_blob"
#define FW_NVR  FW_DIR "brcmfmac43455-sdio.txt"

/* Condense an nvram file to "var=value\0...\0\0", padded to 4 bytes. */
static int condense(uint8_t *buf, int n)
{
    uint8_t *op = buf, *lp = buf;
    int skip = 0;
    for (int i = 0; i < n; i++) {
        int c = buf[i];
        if (c == '#') skip = 1;
        else if (c == '\n' || c == 0) { skip = 0; if (op != lp) { *op++ = 0; lp = op; } }
        else if (c != '\r' && !skip) *op++ = (uint8_t)c;
    }
    if (!skip && op != lp) *op++ = 0;
    *op++ = 0;
    int len = (int)(op - buf);
    while (len & 3) buf[len++] = 0;
    return len;
}
static int upload_fw(void)
{
    size_t codelen = 0, nvlen = 0;
    uint8_t *code = (uint8_t *)file_get(FW_BIN, &codelen);
    uint8_t *nvr = (uint8_t *)file_get(FW_NVR, &nvlen);
    if (!code || codelen < 4 || !nvr) {
        printf("wifi: the firmware is missing (%s and %s): copy it with 'make sdcard' (docs/net.md)\n", FW_BIN, FW_NVR);
        free(code); free(nvr); return -1;
    }
    uint32_t rstvec = g32(code);
    cfgw(CLKCSR, REQ_ALP);
    for (int k = 0; k < 500 && !(cfgr(CLKCSR) & ALP_AVAIL); k++) nic_udelay(10);
    for (size_t off = 0; off < codelen; off += 0x8000) {       /* the firmware into chip RAM */
        int n = codelen - off < 0x8000 ? (int)(codelen - off) : 0x8000;
        sbmem(1, code + off, (n + 3) & ~3, rambase + off);
    }
    int nl = condense(nvr, (int)nvlen);                        /* the nvram at the top of RAM */
    uint32_t nvoff = socramsize - nl - 4;
    sbmem(1, nvr, nl, rambase + nvoff);
    uint8_t tok[4];                                            /* a token the firmware checks: count and its complement */
    uint32_t t = ((uint32_t)nl / 4) & 0xFFFF; t = t | (~t << 16);
    p32(tok, t);
    sbmem(1, tok, 4, rambase + socramsize - 4);
    sbwindow(sdregs);
    cfgwritel(sdregs + INTSTATUS, ~0u);
    if (rstvec) { uint8_t rv[4]; p32(rv, rstvec); sbmem(1, rv, 4, 0); }
    sbreset(armctl, CR4_CPUHALT, 0);             /* take the ARM core out of reset: the firmware runs */
    free(code); free(nvr);
    printf("wifi: firmware loaded (%u bytes, nvram %d bytes)\n", (unsigned)codelen, nl);
    return 0;
}
/* The regulatory blob, sent to the running firmware as the "clmload" iovar. */
static void wlsetvar(const char *name, const void *val, int len);
static void upload_clm(void)
{
    size_t len = 0;
    uint8_t *clm = (uint8_t *)file_get(FW_CLM, &len);
    if (!clm) return;                            /* (the .clm_blob is optional) */
    enum { HDR = 12, CHUNK = 1400, FLAG_CLM = 1 << 12, FIRST = 1 << 1, LAST = 1 << 2 };
    static uint8_t buf[HDR + CHUNK];
    size_t off = 0;
    int flag = FLAG_CLM | FIRST;
    while (!(flag & LAST)) {
        int n = len - off < CHUNK ? (int)(len - off) : CHUNK;
        if (off + n >= len) flag |= LAST;
        p16(buf + 2, 2); p16(buf + 8, 0);        /* type DL, crc 0 */
        p16(buf + 0, flag); p32(buf + 4, n);
        memcpy(buf + HDR, clm + off, n);
        while (n & 7) buf[HDR + n++] = 0;
        wlsetvar("clmload", buf, HDR + n);
        off += n; flag &= ~FIRST;
    }
    free(clm);
}

/* ---- The BCDC/SDPCM control and data protocol. */
#define SDPCM 12                                 /* the SDPCM header */
#define BCDC  16                                 /* the BCDC command header */
#define GET_VAR 262
#define SET_VAR 263
static uint8_t txseq, rxseq, txwindow = 1, lastfc;
static int reqid, link, joinstatus, have_join;
static uint8_t our_mac[6];

/* Receive a frame (SDPCM): its length into *len (0: none), its type into
 * *chan. Up to 2048 bytes into buf. */
static int readpkt(uint8_t *buf, int *chan, int *doff)
{
    if (sdio_rw(2, 0, ENUMBASE, buf, SDPCM, 0)) return -1;
    int len = g16(buf), lenck = g16(buf + 2);
    if (!len) return 0;
    if ((lenck ^ 0xFFFF) != len || len < SDPCM || len > 2048) {
        cfgw(FRAMECTRL, 1);                      /* abort the read frame */
        while (cfgr(RFRAMECNT + 1) || cfgr(RFRAMECNT)) ;
        return 0;
    }
    if (len > SDPCM) sdio_rw(2, 0, ENUMBASE, buf + SDPCM, len - SDPCM, 0);
    uint8_t seq = buf[4], window = buf[9], fcmask = buf[8];
    rxseq = seq + 1;
    if (window != txwindow) txwindow = window;
    if (fcmask != lastfc) lastfc = fcmask;
    *chan = buf[5] & 0xF;
    *doff = buf[6];
    return len;
}
/* Send a frame already laid out with its SDPCM and (data) BDC headers. */
static int writepkt(uint8_t *buf, int len)
{
    p16(buf, len); p16(buf + 2, ~len & 0xFFFF);
    buf[4] = txseq++; buf[5] = 0; buf[6] = SDPCM;   /* chanflg 0 is overwritten by callers that need it */
    int r = sdio_rw(2, 1, ENUMBASE, buf, (len + 3) & ~3, 0);
    return r;
}

/* An event (channel 1) or a data frame (channel 2) read while waiting for
 * something else: act on it. Data frames go to netd as a one-frame batch. */
static void event(const uint8_t *p, int len);
static void deliver(uint8_t *buf, int len, int doff)
{
    int chan = buf[5] & 0xF;
    if (chan == 1) { event(buf, len); return; }
    if (chan != 2 || len <= doff + 4) return;
    int bdc = 4 + (buf[doff + 3] << 2);
    if (len <= doff + bdc + 14 || state != WIFI_UP) return;
    uint32_t fl = (uint32_t)(len - doff - bdc);
    if (fl > 1514) return;
    long net = port_find("net");
    if (net <= 0) return;
    static uint8_t one[2 + 1514];
    one[0] = (uint8_t)fl; one[1] = (uint8_t)(fl >> 8);
    memcpy(one + 2, buf + doff + bdc, fl);
    msg_t f = { .w = { NET_FRAMES, NET_IF_WIFI }, .sbuf = one, .slen = 2 + fl };
    ipc_call(net, &f);
}

/* A control command (BCDC). write: set; else get. Returns 0, or -1. Holds
 * the SDIO lock; dispatches any event or data frame read while waiting. */
static uint8_t rspbuf[2048];
static int wlcmd(int write, int op, const void *data, int dlen, void *res, int rlen)
{
    static uint8_t b[2048];
    int tlen = write ? dlen + rlen : (dlen > rlen ? dlen : rlen);
    int len = SDPCM + BCDC + tlen, ret = -1;
    if (len > (int)sizeof b) return -1;
    lock();
    memset(b, 0, len);
    uint8_t *q = b + SDPCM;
    int id = ++reqid;
    p32(q, (uint32_t)op); p32(q + 4, (uint32_t)tlen); p16(q + 8, write ? 2 : 0); p16(q + 10, (uint16_t)id);
    b[5] = 0; b[6] = SDPCM;
    if (dlen) memcpy(q + BCDC, data, dlen);
    if (write && rlen) memcpy(q + BCDC + dlen, res, rlen);
    if (!writepkt(b, len))
        for (int tries = 0; tries < 400; tries++) {
            int chan = -1, doff = 0, n = readpkt(rspbuf, &chan, &doff);
            if (n < 0) break;
            if (n == 0) { sdio_irq_wait(); continue; }
            if (chan != 0) { deliver(rspbuf, n, doff); continue; }
            uint8_t *c = rspbuf + doff;
            if (g16(c + 10) != (uint16_t)id) continue;
            if (g32(c + 12)) break;              /* the firmware's status: an error */
            if (!write && res) memcpy(res, c + BCDC, rlen);
            ret = 0; break;
        }
    unlock();
    return ret;
}
static void wlsetvar(const char *name, const void *val, int len)
{
    static uint8_t b[600];
    int nl = (int)strlen(name) + 1;
    if (nl + len > (int)sizeof b) return;
    memcpy(b, name, nl); if (len) memcpy(b + nl, val, len);
    wlcmd(1, SET_VAR, b, nl + len, 0, 0);
}
static int wlgetvar(const char *name, void *val, int len)
{
    return wlcmd(0, GET_VAR, name, (int)strlen(name) + 1, val, len);
}
static void wlsetint(const char *name, int v) { uint8_t b[4]; p32(b, (uint32_t)v); wlsetvar(name, b, 4); }
static void wlcmdint(int op, int v) { uint8_t b[4]; p32(b, (uint32_t)v); wlcmd(1, op, b, 4, 0, 0); }

/* ---- Scanning (escan) and its results. */
static wifi_net_t found[WIFI_MAXNET];
static int nfound, scanning;

static void add_result(const uint8_t *p, int len)
{
    if (len < 88 || nfound >= WIFI_MAXNET) return;
    const uint8_t *bssid = p + 8;
    int ssidlen = p[18]; if (ssidlen > 32) ssidlen = 32;
    for (int i = 0; i < nfound; i++) if (!memcmp(found[i].bssid, bssid, 6)) return;   /* already have it */
    wifi_net_t *w = &found[nfound++];
    memset(w, 0, sizeof *w);
    memcpy(w->ssid, p + 19, ssidlen);
    memcpy(w->bssid, bssid, 6);
    w->channel = (uint8_t)(g16(p + 72) & 0xFF);
    w->rssi = (int16_t)g16(p + 78);
    w->security = WIFI_OPEN;
    if (g16(p + 16) & 0x10) w->security = WIFI_OTHER;     /* privacy without RSN: WEP, unsupported */
    uint32_t ielen = g32(p + 0x78), ieoff = g32(p + 0x74);
    if (ielen && ieoff + ielen <= (uint32_t)len) {
        const uint8_t *t = p + ieoff, *e = t + ielen;
        for (; t + 2 < e; t += 2 + t[1]) if (t[0] == 0x30) w->security = WIFI_WPA2;   /* an RSN element */
    }
}
static void escan_start(void)
{
    /* The v1 escan parameters, as the 43455 takes them (brcmfmac / ether4330). */
    static uint8_t p[73];
    memset(p, 0, sizeof p);
    p32(p + 0, 1); p16(p + 4, 1); p16(p + 6, 0x1234);     /* version, action START, sync id */
    memset(p + 8 + 4 + 32, 0xFF, 6);                      /* bssid: broadcast */
    p[8 + 4 + 32 + 6] = 2;                                /* bss_type: any */
    p32(p + 8 + 4 + 32 + 8, (uint32_t)-1);                /* nprobes */
    p32(p + 8 + 4 + 32 + 12, (uint32_t)-1);               /* active_time */
    p32(p + 8 + 4 + 32 + 16, (uint32_t)-1);               /* passive_time */
    p32(p + 8 + 4 + 32 + 20, (uint32_t)-1);               /* home_time */
    p32(p + 8 + 4 + 32 + 24, 0);                          /* channel_num 0: all */
    wlcmdint(49, 0);                                      /* PASSIVE_SCAN off */
    nfound = 0; scanning = 1;
    wlsetvar("escan", p, sizeof p);
}

/* ---- Events (channel 1): the chip's status changes. */
static void event(const uint8_t *p, int len)
{
    if (len < 14 + 10 + 20) return;
    p += 14 + 10; len -= 14 + 10;                /* skip the Ethernet and bcm_ether headers */
    int ev = g16(p + 6), flags = g16(p + 2);
    uint32_t status = g32(p + 8);
    switch (ev) {
    case 69:                                     /* E_ESCAN_RESULT */
        if (len > 48 + 12) {
            const uint8_t *r = p + 48;
            if (g16(r + 10)) add_result(r + 12, len - 48 - 12);   /* bss_count > 0 */
            else scanning = 0;                   /* count 0: the scan is done */
        }
        break;
    case 0:                                      /* E_SET_SSID: association result */
    case 19: joinstatus = (int)status + 1; break;
    case 16: if (flags & 1) { link = 1; } else { link = 0; } break;   /* E_LINK */
    case 5: case 6: case 12: link = 0; break;    /* deauth / disassoc */
    }
}

/* ---- Joining. open: an empty passphrase. Returns 0 while it tries (the
 * link event confirms it); the firmware's supplicant does WPA2's handshake. */
static int do_join(const char *ssid, const char *pass)
{
    int wpa2 = pass && pass[0];
    wlcmdint(2, 1);                              /* UP */
    wlsetint("roam_off", 1);
    wlcmdint(0x56, wpa2 ? 0 : 0);                /* SET_PM 0: powersave off (simpler, more reliable) */
    wlcmdint(20, 1);                             /* SET_INFRA: infrastructure */
    wlsetint("sup_wpa", wpa2 ? 1 : 0);           /* the firmware's own supplicant */
    wlsetint("wsec", wpa2 ? 4 : 0);              /* AES-CCMP, or none */
    wlsetint("wpa_auth", wpa2 ? 0x80 : 0);       /* WPA2_AUTH_PSK, or open */
    if (wpa2) {
        uint8_t pmk[36];
        int n = (int)strlen(pass); if (n > 32) n = 32;
        memset(pmk, 0, sizeof pmk);
        p16(pmk, (uint16_t)n); p16(pmk + 2, 1);  /* key_len, flags: a passphrase */
        memcpy(pmk + 4, pass, n);
        if (wlcmd(1, 268, pmk, 4 + 32, 0, 0)) return -1;   /* WLC_SET_WSEC_PMK */
    }
    uint8_t jp[72];                              /* join: ssid, then assoc params (bssid bcast, any channel) */
    memset(jp, 0, sizeof jp);
    int sl = (int)strlen(ssid); if (sl > 32) sl = 32;
    p32(jp, (uint32_t)sl); memcpy(jp + 4, ssid, sl);
    memset(jp + 36, 0xFF, 6);                     /* bssid: broadcast */
    p32(jp + 44, 0);                              /* chanspec count 0: let the firmware find it */
    joinstatus = 0; link = 0;
    return wlsetvar("join", jp, sizeof jp), 0;
}

/* ---- Bring the chip all the way up: the SDIO card, the backplane, the
 * firmware, function 2, then the basic configuration. */
static char chipdesc[80];
static int chip_up(void)
{
    char what[80];
    if (sdio_up(what, sizeof what)) return -1;
    printf("wifi: %s\n", what);
    if (sbinit(chipdesc, sizeof chipdesc)) return -1;
    printf("wifi: %s\n", chipdesc);
    if (upload_fw() || sbenable()) return -1;
    /* Function 2 is up: settle, then the chip is ready for commands. */
    for (int k = 0; k < 50; k++) {
        int chan = -1, doff = 0, n = readpkt(rspbuf, &chan, &doff);
        if (n > 0 && chan == 1) event(rspbuf, n);
        nic_udelay(20000);
    }
    upload_clm();
    uint8_t eventmask[16];
    memset(eventmask, 0, sizeof eventmask);
    int want[] = { 0, 16, 5, 6, 12, 46, 69 };    /* set-ssid, link, deauth(s), psk-sup, escan-result */
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) eventmask[want[i] / 8] |= 1 << (want[i] % 8);
    wlsetvar("event_msgs", eventmask, sizeof eventmask);
    wlsetint("bus:txglom", 0);
    wlgetvar("cur_etheraddr", our_mac, 6);
    wlcmdint(2, 1);                              /* UP */
    printf("wifi: ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           our_mac[0], our_mac[1], our_mac[2], our_mac[3], our_mac[4], our_mac[5]);
    return 0;
}

/* ---- The configuration file (root only): the chosen network. */
static char cfg_ssid[33], cfg_pass[65];
static void load_conf(void)
{
    size_t n = 0;
    char *c = file_get("/etc/wifi.conf", &n), *p = c;
    while (p && *p) {
        char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > 5 && !memcmp(p, "ssid=", 5)) { size_t k = l - 5 < 32 ? l - 5 : 32; memcpy(cfg_ssid, p + 5, k); cfg_ssid[k] = 0; }
        else if (l > 11 && !memcmp(p, "passphrase=", 11)) { size_t k = l - 11 < 64 ? l - 11 : 64; memcpy(cfg_pass, p + 11, k); cfg_pass[k] = 0; }
        p = e ? e + 1 : p + l;
    }
    free(c);
}
static int save_conf(const char *ssid, const char *pass)
{
    static char out[160];
    int len = 0;
    len += (int)strlcpy(out + len, "ssid=", sizeof out - len);
    len += (int)strlcpy(out + len, ssid, sizeof out - len);
    out[len++] = '\n';
    len += (int)strlcpy(out + len, "passphrase=", sizeof out - len);
    len += (int)strlcpy(out + len, pass, sizeof out - len);
    out[len++] = '\n';
    long h = fs_open("/etc/wifi.conf", FS_WRONLY | FS_CREAT | FS_TRUNC, 0600);
    if (h < 0) return (int)h;
    fs_chmod("/etc/wifi.conf", 0600);
    long r = fs_write(h, 0, out, len);
    fs_close(h);
    return r < 0 ? (int)r : 0;
}

/* ---- The server. Frames to send land here (NIC_SEND's layout). */
static uint8_t txmem[NET_MAX];
static int cur_chan, cur_rssi;
static char cur_bssid[6], failwhy[64];

/* Send the frames in txmem (len bytes) as data frames (channel 2, a BDC header). */
static long tx_frames(size_t len)
{
    static uint8_t b[2048 + 64];
    lock();
    for (size_t off = 0; off + 2 <= len; ) {
        uint32_t fl = txmem[off] | txmem[off + 1] << 8;
        if (fl < 14 || fl > 1514 || off + 2 + fl > len) { unlock(); return -EINVAL; }
        int hdr = SDPCM + 4;                     /* SDPCM + BDC */
        memset(b, 0, hdr);
        b[5] = 2; b[6] = SDPCM;                  /* channel 2: data */
        b[SDPCM] = 0x20;                         /* BDC: flags (protocol version 2 << 4) */
        memcpy(b + hdr, txmem + off + 2, fl);
        writepkt(b, hdr + (int)fl);
        off += 2 + fl;
    }
    unlock();
    return 0;
}

/* Tell netd the link changed (like vnet's watcher). */
static void tell_net(int up)
{
    long net = port_find("net");
    if (net <= 0) return;
    msg_t m = { .w = { NET_LINK, (uint64_t)up, NET_IF_WIFI } };
    ipc_call(net, &m);
}

/* The receiving thread: wait for the card's interrupt, deliver data frames
 * to netd, and act on events (the link, scan results). */
static void receiver(void *arg)
{
    (void)arg;
    int was = 0;
    for (;;) {
        sdio_irq_wait();
        lock();
        sbwindow(sdregs);
        cfgwritel(sdregs + INTSTATUS, cfgreadl(sdregs + INTSTATUS));   /* clear the chip's interrupt */
        for (;;) {                                        /* drain every frame the chip has */
            int chan = -1, doff = 0, n = readpkt(rspbuf, &chan, &doff);
            if (n <= 0) break;
            deliver(rspbuf, n, doff);                     /* events and data (channels 1 and 2) */
        }
        unlock();
        if (link != was) {
            was = link;
            if (link) { state = WIFI_UP; wlgetvar("cur_etheraddr", our_mac, 6); }
            else if (state == WIFI_UP) state = WIFI_IDLE;
            printf("wifi: link %s\n", link ? "up" : "down");
            tell_net(link);
        }
        sdio_irq_ack();
    }
}

static void fill_status(wifi_status_t *s)
{
    memset(s, 0, sizeof *s);
    s->state = state;
    memcpy(s->mac, our_mac, 6);
    strlcpy(s->fw, chipdesc, sizeof s->fw);
    strlcpy(s->ssid, cfg_ssid, sizeof s->ssid);
    strlcpy(s->why, failwhy, sizeof s->why);
    s->channel = cur_chan; s->rssi = cur_rssi;
    memcpy(s->bssid, cur_bssid, 6);
    s->security = cfg_pass[0] ? WIFI_WPA2 : WIFI_OPEN;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!wifi_fdt() || (fdt_find(fdt, -1, "raspberrypi,bcm2835") < 0 && fdt_find(fdt, -1, "brcm,bcm2711") < 0 &&
                        fdt_find(fdt, -1, "brcm,bcm2712") < 0)) {
        printf("wifi: not a Raspberry Pi; nothing to do\n");
        return 0;                                /* init does not start it again */
    }
    if (chip_up()) { printf("wifi: the chip did not come up\n"); return 2; }
    state = WIFI_IDLE;
    thread_start(receiver, 0, 16384);

    load_conf();
    if (cfg_ssid[0]) { printf("wifi: joining \"%s\"\n", cfg_ssid); state = WIFI_JOINING; do_join(cfg_ssid, cfg_pass); }

    long port = port_create("wifi");
    msg_t m = { .rbuf = txmem, .rlen = NET_MAX };
    uint64_t seen = 0;
    long from = ipc_recv(port, &m);
    for (;;) {
        long r = 0; const void *out = 0; size_t outn = 0;
        static wifi_status_t st;
        static nic_info_t info;
        static wifi_net_t list[WIFI_MAXNET];
        if (from <= 0) r = -EINVAL;
        else switch (m.w[0]) {
        case NIC_INFO:
            memset(&info, 0, sizeof info);
            memcpy(info.mac, our_mac, 6); info.mtu = 1500; info.link = state == WIFI_UP;
            out = &info; outn = sizeof info; break;
        case NIC_SEND:
            if (m.uid != 0) r = -EPERM; else r = tx_frames(m.rlen); break;
        case WIFI_STATUS:
            fill_status(&st); out = &st; outn = sizeof st; break;
        case WIFI_SCAN:
            escan_start();
            for (int k = 0; k < 400 && scanning; k++) sys_sleep(10000000);   /* up to ~4 s */
            memcpy(list, found, sizeof list); r = nfound;
            out = list; outn = (size_t)nfound * sizeof list[0]; break;
        case WIFI_JOIN:
            if (m.uid != 0) { r = -EPERM; break; }
            { const char *ssid = (const char *)m.sbuf, *pass = ssid + strlen(ssid) + 1;
              if (m.slen <= strlen(ssid) + 1) pass = "";
              strlcpy(cfg_ssid, ssid, sizeof cfg_ssid); strlcpy(cfg_pass, pass, sizeof cfg_pass);
              r = save_conf(cfg_ssid, cfg_pass);
              if (!r) { state = WIFI_JOINING; failwhy[0] = 0; do_join(cfg_ssid, cfg_pass); } }
            break;
        case WIFI_FORGET:
            if (m.uid != 0) { r = -EPERM; break; }
            wlcmdint(52, 0);                     /* DISASSOC */
            fs_unlink("/etc/wifi.conf");
            cfg_ssid[0] = cfg_pass[0] = 0; link = 0; state = WIFI_IDLE; tell_net(0);
            break;
        case SVC_MAYSTOP:                        /* kept while a network is joined (never idle then) */
            if ((r = maystop_answer(&m, 0, &seen, state == WIFI_UP || state == WIFI_JOINING)) == 0) { reply_val(from, 0); return 0; }
            break;
        default: r = -ENOSYS;
        }
        m = (msg_t){ .w = { (uint64_t)r }, .sbuf = out, .slen = outn, .rbuf = txmem, .rlen = NET_MAX };
        from = from > 0 ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m);
    }
}
