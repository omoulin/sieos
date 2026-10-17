/*
 * kbl.c - The keyboard light of the Lenovo Legion 5 Gen 10 laptops: the
 * keyboard's ITE 8258 controller (USB 048d:c195), served on the port
 * "kblight" (mk/proto.h: KBL_SET, KBL_GET).
 *
 * The protocol is OpenRGB's ("Lenovo Legion 5 Gen 10", Controllers/
 * LenovoControllers/LenovoUSBController_Gen7_8): 960-byte feature reports
 * with id 7 on the keyboard's interface 0 (vendor page 0xFF89, usage 7):
 * the report id, a command, the payload's length (16 bits), the payload.
 *   0xCA  which profile is active (then read back: its number in byte 4)
 *   0xCB  a profile's lighting: groups of zones, each with its effect
 *         (0x0B: static), colour mode (2: given colours), colours, zones
 *         (this keyboard: 24 zones, ids 1..24); kept by the keyboard
 *   0xCE  brightness, 0 (off) .. 9
 * Nothing is sent unless the device is exactly 048d:c195 and its report
 * descriptor has that very feature report; each command sent is logged.
 * The setting ("level colour" in /etc/kblight) is given again at start.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef USB_HOST
#include "usb.h"
#endif

/* The report descriptor's feature report 7: page 0xFF89, usage 7, 959 bytes. */
static const uint8_t kbl_sig[] = { 0x06, 0x89, 0xFF, 0x09, 0x07, 0xA1, 0x01, 0x85, 0x07, 0x09, 0x01, 0x15, 0x00,
                                   0x26, 0xFF, 0x00, 0x75, 0x08, 0x96, 0xBF, 0x03, 0xB1 };
static int kbl_match(const uint8_t *rd, int n)
{
    for (int i = 0; i + (int)sizeof kbl_sig <= n; i++) if (!memcmp(rd + i, kbl_sig, sizeof kbl_sig)) return 1;
    return 0;
}

#ifndef USB_HOST
static usbdev_t *kd;
static int kif, level, colour = 0xFFFFFF;
static volatile long kbl_tid;
static uint8_t pk[960];

/* Command cmd with its payload (feature report 7); get: read the answer
 * back into pk. -> 0, or -EIO. */
static int report(int cmd, const uint8_t *pl, int n, int get)
{
    memset(pk, 0, sizeof pk);
    pk[0] = 7; pk[1] = cmd; pk[2] = n; pk[3] = n >> 8;
    memcpy(pk + 4, pl, n);
    char hex[3 * 24 + 1];
    for (int i = 0; i < 24 && i < n + 4; i++) sfmt(hex + 3 * i, 4, " %02x", pk[i]);
    int r = control(kd, 0x21, 9, 0x307, kif, pk, sizeof pk);                  /* SET_REPORT (feature 7) */
    if (r >= 0 && get) r = control(kd, 0xA1, 1, 0x307, kif, pk, sizeof pk);  /* GET_REPORT */
    log_line("usb: keyboard light:%s%s -> %s\n", hex, n + 4 > 24 ? " ..." : "", r < 0 ? "failed" : "ok");
    return r < 0 ? -EIO : 0;
}

static long kbl_apply(int lv, int c)
{
    if (!kd || kd->gone) return -ENOENT;
    uint8_t b[80] = { 0 };
    int prof = 1, n = 0;
    if (!report(0xCA, b, 1, 1) && pk[4] >= 1 && pk[4] <= 6) prof = pk[4];
    if (lv) {                                         /* the profile: one group, static, the colour, every zone */
        static const uint8_t g[14] = { 1, 6, 1, 0x0B, 2, 0, 3, 0, 4, 0, 5, 2, 6, 0 };
        b[n++] = prof; b[n++] = 1; b[n++] = 1;
        memcpy(b + n, g, sizeof g); n += sizeof g;
        b[n++] = 1; b[n++] = c >> 16; b[n++] = c >> 8; b[n++] = c;
        b[n++] = 24;
        for (int z = 1; z <= 24; z++) { b[n++] = z; b[n++] = 0; }
        if (report(0xCB, b, n, 0)) return -EIO;
    }
    b[0] = lv == KBL_HIGH ? 9 : lv == KBL_LOW ? 3 : 0;
    return report(0xCE, b, 1, 0);
}

static void kbl_thread(void *a)
{
    (void)a;
    while (!kbl_tid) sys_yield();
    self_tid = kbl_tid;
    port_lookup("fs");                                /* (the setting: once the files are there) */
    char *f = file_get("/etc/kblight", 0);
    if (f && f[0] >= '0' && f[0] <= '2') {
        level = f[0] - '0';
        colour = 0;
        for (const char *p = f + 2; (*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'f'); p++)
            colour = colour << 4 | (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
        log_line("usb: keyboard light: restoring level %d, colour %06x\n", level, colour & 0xFFFFFF);
        kbl_apply(level, colour);
    }
    free(f);
    long port = port_create("kblight");
    msg_t m = { 0 };
    long from = ipc_recv(port, &m);
    for (;;) {
        long r = -ENOSYS;
        if (from > 0 && m.w[0] == KBL_GET) r = kd && !kd->gone ? level : -ENOENT;
        if (from > 0 && m.w[0] == KBL_SET) {
            r = m.w[1] > KBL_HIGH ? -EINVAL : kbl_apply((int)m.w[1], (int)(m.w[2] & 0xFFFFFF));
            if (!r) {
                level = (int)m.w[1]; colour = (int)(m.w[2] & 0xFFFFFF);
                char line[16];
                sfmt(line, sizeof line, "%d %06x\n", level, colour);
                long h = fs_open("/etc/kblight", FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
                if (h >= 0) { fs_write(h, 0, line, strlen(line)); fs_close(h); }
            }
        }
        m = (msg_t){ .w = { r, (uint64_t)colour } };
        from = from > 0 ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m);
    }
}

/* A HID interface: the keyboard light only if it is exactly that one. */
int kbl_attach(usbdev_t *d, const uint8_t *f, int len)
{
    if (d->vendor != 0x048D || d->product != 0xC195) return -1;
    static uint8_t rd[1024];
    int rlen = 0;
    for (int i = f[0]; i + 8 < len && f[i]; i += f[i]) if (f[i + 1] == 0x21) rlen = f[i + 7] | f[i + 8] << 8;
    if (rlen > (int)sizeof rd) rlen = sizeof rd;
    if (!rlen || control(d, 0x81, 6, 0x2200, f[2], rd, rlen) <= 0 || !kbl_match(rd, rlen)) {
        log_line("usb: 048d:c195 interface %d: no feature report 7 (keyboard light left alone)\n", f[2]);
        return -1;
    }
    kd = d; kif = f[2];
    log_line("usb: keyboard light: ITE 8258 (048d:c195), interface %d, feature report 7\n", kif);
    if (!kbl_tid) kbl_tid = thread_start(kbl_thread, 0, 8192);
    return 0;
}
#endif
