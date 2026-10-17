/*
 * uvc.c - USB cameras ("USB video class"), served on the port "camera"
 * (mk/proto.h: CAM_OPEN, CAM_FRAME, CAM_CLOSE).
 *
 * A camera has a VideoControl interface (its units: we leave them as they
 * are) and a VideoStreaming one: formats, each with frame sizes and
 * frame rates, then alternate settings with an isochronous endpoint of
 * growing size. We take the uncompressed YUY2 format (no JPEG decoder):
 * 640 x 480, else the biggest size that does at least 15 frames a second.
 * Opening: "probe" (we propose format, size, rate; the camera answers with
 * how many bytes it sends per interval), "commit", then the smallest
 * alternate setting that carries that many. Closing: alternate setting 0
 * (the camera stops, its bandwidth is given back).
 *
 * Data: NTD isochronous transfers stay queued, one per interval (125 µs at
 * high speed), each into its own 4 KiB of DMA memory; each completion
 * (on the interrupt thread; one interrupt per 8 of them) hands the packet
 * to uvc_put and queues the transfer again. A packet is a payload header
 * (its length, a frame bit that flips each frame, end of frame, error)
 * then image bytes, appended to the frame being filled. A frame complete
 * and exactly the right size becomes the ready one (three buffers: being
 * filled, ready, being sent); a client's CAM_FRAME converts the ready one
 * to 0x00RRGGBB pixels (integer BT.601) and replies with it.
 *
 * The parsing, the assembly and the conversion are also built on the host
 * (USB_HOST) for tests/usbdev, with the laptop camera's descriptors.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef USB_HOST
#include "usb.h"
static void uvc_wake(void);                   /* a frame is ready: the camera's thread */
#endif

#define NTD 40                                /* transfers queued: 5 ms */
#define NALT 16
#define NIVAL 8

typedef struct { int alt, ep, esit; } uvc_alt_t;   /* an alternate setting: its endpoint (offset in cfg), bytes per interval */
typedef struct {
    int vc, vs, ver;                          /* interface numbers; the UVC version (0x0150: 1.5) */
    int fmt, frm, w, h;                       /* the format and frame chosen; the size */
    uint32_t ival[NIVAL]; int nival;          /* its frame intervals (100 ns units), fastest first */
    uvc_alt_t alt[NALT];
    int nalt;
    uint8_t *buf[3];                          /* frames: being filled, ready, being sent */
    volatile int fill, ready, sending, lk;
    uint32_t have, size;
    int fid, bad;
    volatile int on;
    volatile unsigned frames;                 /* frames completed */
} uvc_t;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* The configuration descriptor -> the camera's interfaces, the YUY2 frame
 * to use, its rates, the streaming alternate settings. -> 0, or -1: none. */
static int uvc_parse(uvc_t *u, const uint8_t *c, int len)
{
    int ifn = -1, alt = 0, cls = 0, sub = 0, cur = 0;
    uint64_t best = 0;
    u->nalt = u->fmt = 0;
    for (int i = 0; i + 2 <= len && c[i] >= 2 && i + c[i] <= len; i += c[i]) {
        const uint8_t *p = c + i;
        if (p[1] == 4 && p[0] >= 9) { ifn = p[2]; alt = p[3]; cls = p[5]; sub = p[6]; continue; }
        if (cls != 14) continue;
        if (p[1] == 0x24 && sub == 1 && p[2] == 1 && p[0] >= 5) { u->vc = ifn; u->ver = p[3] | p[4] << 8; }
        if (p[1] == 0x24 && sub == 2 && p[2] == 4 && p[0] >= 27) cur = !memcmp(p + 5, "YUY2", 4) ? p[3] : 0;
        if (p[1] == 0x24 && sub == 2 && p[2] == 5 && p[0] >= 30 && cur) {   /* a frame of the YUY2 format */
            int w = p[5] | p[6] << 8, h = p[7] | p[8] << 8, t = p[25], n = t ? t : 1;
            if (26 + 4 * n > p[0]) continue;
            uint32_t fast = le32(p + 26);
            for (int k = 1; k < n; k++) if (le32(p + 26 + 4 * k) < fast) fast = le32(p + 26 + 4 * k);
            uint64_t score = w == 640 && h == 480 ? ~0ULL : fast <= 666666 ? (uint64_t)w * h : 0;
            if (score <= best) continue;
            best = score;
            u->vs = ifn; u->fmt = cur; u->frm = p[3]; u->w = w; u->h = h;
            u->nival = n < NIVAL ? n : NIVAL;
            for (int k = 0; k < u->nival; k++) {           /* (sorted: the fastest first) */
                uint32_t v = le32(p + 26 + 4 * k);
                int j = k;
                for (; j && u->ival[j - 1] > v; j--) u->ival[j] = u->ival[j - 1];
                u->ival[j] = v;
            }
        }
        if (p[1] == 5 && p[0] >= 7 && sub == 2 && alt && ifn == u->vs && (p[2] & 0x80) && (p[3] & 3) == 1 && u->nalt < NALT) {
            int mps = p[4] | p[5] << 8, esit = (mps & 0x7FF) * (1 + (mps >> 11 & 3));
            if (esit <= 4096) u->alt[u->nalt++] = (uvc_alt_t){ alt, i, esit };
        }
    }
    return u->fmt && u->nalt ? 0 : -1;
}

/* A frame ended: complete and the right size, it becomes the ready one. */
static void uvc_end(uvc_t *u)
{
    if (!u->bad && u->have == u->size) {
        lock(&u->lk);
        int r = u->fill, f = 0;
        while (f == r || f == u->sending) f++;
        u->ready = r; u->fill = f;
        unlock(&u->lk);
        u->frames++;
        uvc_wake();
    }
    u->have = 0; u->bad = 0;
}

/* One packet: its payload header (length, bits: 0 frame id, 1 end of
 * frame, 6 error), then image bytes. */
static void uvc_put(uvc_t *u, const uint8_t *p, int n)
{
    if (n < 2 || p[0] < 2 || p[0] > n) return;
    int f = p[1];
    if ((f & 1) != u->fid && u->have) uvc_end(u);     /* the frame bit flipped: the last frame had no end mark */
    u->fid = f & 1;
    if (f & 0x40) u->bad = 1;
    n -= p[0];
    if (u->have + n > u->size) u->bad = 1;
    else { memcpy(u->buf[u->fill] + u->have, p + p[0], n); u->have += n; }
    if (f & 2) uvc_end(u);
}

/* YUY2 (Y0 U Y1 V for two pixels, BT.601's limited range) -> 0x00RRGGBB. */
static inline uint32_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v; }
static void uvc_rgb(uint32_t *o, const uint8_t *s, int npix)
{
    for (const uint8_t *e = s + 2 * npix; s < e; s += 4, o += 2) {
        int d = s[1] - 128, v = s[3] - 128;
        int r = 409 * v + 128, g = 128 - 100 * d - 208 * v, b = 516 * d + 128;
        int y0 = 298 * (s[0] - 16), y1 = 298 * (s[2] - 16);
        o[0] = clamp8((y0 + r) >> 8) << 16 | clamp8((y0 + g) >> 8) << 8 | clamp8((y0 + b) >> 8);
        o[1] = clamp8((y1 + r) >> 8) << 16 | clamp8((y1 + g) >> 8) << 8 | clamp8((y1 + b) >> 8);
    }
}

#ifndef USB_HOST
/* ---- The camera: one at a time. */
static uvc_t u;
static usbdev_t *cd;
static uint8_t *slots;                        /* NTD x 4 KiB: the transfers' memory */
static uint64_t sbus;
static uint32_t *rgb;                         /* the frame being sent, converted */
static int dci, head, esit, tlbpc, owner;
static volatile long cam_tid;
static volatile int64_t last;                 /* the last request (the watchdog) */

static void uvc_wake(void) { sys_wake(cam_tid); }

static trb_t iso_trb(int k)                   /* "isoch" TRB: as soon as possible; an interrupt every 8th */
{
    uint64_t b = sbus + 4096UL * k;
    return (trb_t){ (uint32_t)b, (uint32_t)(b >> 32), (uint32_t)esit,
                    1u << 31 | tlbpc << 16 | 5 << 10 | (k % 8 != 7 ? 1 << 9 : 0) | 1 << 5 | 1 << 2 };
}

/* Completions (interrupt thread). The one named is done, and those before
 * it the controller skipped (a missed interval): each queued again. */
static void uvc_async(usbdev_t *d, int dc, int code, uint32_t left)
{
    ring_t *r = &d->ep[dc];
    uint64_t at = async_trb - r->bus;
    int p = (int)(at / 16), was, n = 0;
    if (!u.on || at >= (uint64_t)(r->n - 1) * 16 || (p - head + r->n - 1) % (r->n - 1) >= NTD) return;   /* (none named, or not queued) */
    trb_t td[NTD];
    do {
        trb_t *t = &r->t[head];
        int k = (int)(((t->p0 | (uint64_t)t->p1 << 32) - sbus) >> 12);
        if ((was = head == p) && (code == CC_SUCCESS || code == CC_SHORT)) {
            dsync(slots + 4096 * k, esit);
            uvc_put(&u, slots + 4096 * k, esit - (int)left);
        }
        td[n++] = iso_trb(k);
        head = head + 1 == r->n - 1 ? 0 : head + 1;
    } while (!was && n < NTD);
    td_queue(d, dc, td, n);
}

static void cam_stop(void)
{
    if (!u.on) return;
    u.on = 0;
    if (!cd->gone) { control(cd, 0x01, 11, 0, u.vs, 0, 0); ep_drop(cd, dci); }   /* alternate setting 0 */
    else sys_sleep(10000000);
    for (int i = 0; i < 3; i++) { free(u.buf[i]); u.buf[i] = 0; }   /* (no completion runs after the drop) */
    free(rgb); rgb = 0;
    log_line("usb: camera off (%u frames)\n", u.frames);
}

/* Probe at each rate, fastest first, until an alternate setting carries
 * what the camera says it sends; commit; start the transfers. */
static long cam_start(void)
{
    if (u.on) return 0;
    if (!cd || cd->gone) return -ENOENT;
    if (!slots && !(slots = dmem(NTD * 4096, &sbus))) return -ENOMEM;
    uint8_t pc[48];
    int n = u.ver >= 0x150 ? 48 : u.ver >= 0x110 ? 34 : 26, a = -1, iv = 0;
    for (int i = 0; i < u.nival && a < 0; i++) {
        memset(pc, 0, sizeof pc);
        pc[0] = 1; pc[2] = u.fmt; pc[3] = u.frm;                   /* hint: keep the rate; format, frame */
        iv = u.ival[i];
        pc[4] = iv; pc[5] = iv >> 8; pc[6] = iv >> 16; pc[7] = iv >> 24;
        if (control(cd, 0x21, 1, 1 << 8, u.vs, pc, n) < 0 || control(cd, 0xA1, 0x81, 1 << 8, u.vs, pc, n) < 0) return -EIO;
        uint32_t want = le32(pc + 22);                             /* dwMaxPayloadTransferSize */
        for (int k = 0; k < u.nalt; k++)
            if (u.alt[k].esit >= (int)want && (a < 0 || u.alt[k].esit < u.alt[a].esit)) a = k;
    }
    if (a < 0 || pc[2] != u.fmt || pc[3] != u.frm) { log_line("usb: camera: no setting fits (format %d frame %d)\n", pc[2], pc[3]); return -EIO; }
    if (control(cd, 0x21, 1, 2 << 8, u.vs, pc, n) < 0) return -EIO;      /* commit */
    u.size = (uint32_t)u.w * u.h * 2;
    for (int i = 0; i < 3; i++) u.buf[i] = malloc(u.size);
    rgb = malloc(u.size * 2);
    const uint8_t *ep = cd->cfg + u.alt[a].ep;
    int mps = ep[4] | ep[5] << 8;
    esit = u.alt[a].esit; tlbpc = mps >> 11 & 3; dci = dci_of(ep[2]);
    u.fill = 0; u.ready = u.sending = -1; u.have = 0; u.bad = 0; u.frames = 0;
    if (!u.buf[0] || !u.buf[1] || !u.buf[2] || !rgb || configure(cd, &ep, 1)) { u.on = 1; cam_stop(); return -ENOMEM; }
    u.on = 1;                                                      /* (so cam_stop undoes what follows) */
    if (control(cd, 0x01, 11, u.alt[a].alt, u.vs, 0, 0) < 0) { cam_stop(); return -EIO; }
    head = cd->ep[dci].i;
    cd->async[dci] = uvc_async;
    trb_t td[NTD];
    for (int k = 0; k < NTD; k++) td[k] = iso_trb(k);
    td_queue(cd, dci, td, NTD);
    log_line("usb: camera on: %dx%d YUY2, %d.%d frames/s, alternate setting %d (%d bytes per interval)\n",
             u.w, u.h, 10000000 / iv, 100000000 / iv % 10, u.alt[a].alt, esit);
    return 0;
}

/* The next frame (waiting up to 1 s), converted into rgb. -> bytes. */
static long cam_frame(void)
{
    int64_t end = sys_clock() + 1000000000L;
    while (u.ready < 0 && !cd->gone) {
        int64_t left = end - sys_clock();
        if (left <= 0) return -EIO;
        sys_sleep(left);
    }
    if (cd->gone) return -EIO;
    lock(&u.lk);
    int s = u.sending = u.ready;
    u.ready = -1;
    unlock(&u.lk);
    uvc_rgb(rgb, u.buf[s], u.w * u.h);
    u.sending = -1;
    return u.size * 2;
}

/* Nobody asked for 3 s (a client that ended without closing): stop. */
static void watchdog(void *a)
{
    for (;;) {
        sys_sleep(1000000000L);
        if (!u.on || sys_clock() - last < 3000000000L) continue;
        msg_t m = { .w = { CAM_CLOSE } };
        ipc_call((long)a, &m);
    }
}

static void cam_thread(void *a)
{
    (void)a;
    while (!cam_tid) sys_yield();
    self_tid = cam_tid;
    long port = port_create("camera");
    thread_start(watchdog, (void *)port, 4096);
    msg_t m = { 0 };
    long from = ipc_recv(port, &m);
    for (;;) {
        long r = -ENOSYS, out = 0;
        int mine = !u.on || m.pid == owner || sys_clock() - last >= 3000000000L;
        last = sys_clock();
        if (from > 0) switch (m.w[0]) {
        case CAM_OPEN:  r = !mine ? -EBUSY : cam_start(); if (!r) owner = m.pid; break;
        case CAM_FRAME: r = !u.on || !mine ? -EBUSY : cam_frame(); out = r > 0; break;
        case CAM_CLOSE: if (mine) cam_stop(); r = mine ? 0 : -EBUSY; break;
        }
        m = (msg_t){ .w = { r, u.w, u.h }, .sbuf = out ? rgb : 0, .slen = out ? (uint64_t)r : 0 };
        from = from > 0 ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m);
    }
}

int uvc_attach(usbdev_t *d, const uint8_t *f, int len)
{
    (void)f; (void)len;
    if ((cd && !cd->gone) || u.on || uvc_parse(&u, d->cfg, d->cfglen)) return -1;   /* (the whole configuration: streaming too) */
    cd = d;
    log_line("usb: camera %04x:%04x: %dx%d YUY2 (format %d, frame %d), %d alternate settings, UVC %x.%02x\n",
             d->vendor, d->product, u.w, u.h, u.fmt, u.frm, u.nalt, u.ver >> 8, u.ver & 0xFF);
    if (!cam_tid) cam_tid = thread_start(cam_thread, 0, 16384);
    return 0;
}
#endif
