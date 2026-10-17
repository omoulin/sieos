/*
 * btd - The Bluetooth server (docs/bluetooth.md), port "bt": scan, pair,
 * forget, on/off for Settings and /bin/bt; bonded keyboards and mice come
 * back by themselves and type into the console like USB ones.
 *
 *   the PC     the Intel AX210's Bluetooth, a USB device: the usb server
 *              passes its packets (port "bthci", user/usb/btusb.c). It starts
 *              in its bootloader: Intel Read Version (TLV), the firmware
 *              ibt-<cnvi>-<cnvr>.sfi sent in Secure Send fragments, Intel
 *              Reset into it, then its DDC settings (as Linux's btintel.c)
 *   the Pis    the CYW43455's Bluetooth on a UART (uart.c): Broadcom's
 *              patch BCM4345C0.hcd (Download Minidriver, then its records;
 *              as Linux's btbcm.c), at the line's top speed
 * Then the common set-up (address, buffers, event mask, SSP, LE) and the
 * stack (hci.c, l2cap.c, smp.c, input.c). Bonds: /etc/bluetooth.keys,
 * root only. Every step is logged ("bt: ...") for reports from hardware.
 *
 * Threads: main answers the port; the receiver reads the controller and
 * runs the stack; the start thread brings the controller up, then ticks
 * the stack once a second; "sound" answers audiod on the port "btaudio"
 * (its writes land in a2dp.c's ring) and the pump sends them to the
 * headphones on time. One lock for the stack. Files offered are taken if
 * the user says so (Atlas's Accept, or bt accept: BT_ACCEPT) into the
 * desktop user's ~/Downloads (Atlas says who: BT_USER). What goes to Atlas
 * and audiod (notes, the headset's keys and volume) is sent by the tick
 * thread, outside the lock, woken for it.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "btd.h"
#include "mk/fdt.h"

int  uart_open(const void *f, char *what, int cap);
void uart_baud(int baud);
void uart_write(const uint8_t *p, int n);
int  uart_read(uint8_t *b, int cap);
extern int uart_max;

static volatile int big;                          /* the stack's lock */
static char note[128];                            /* for Atlas to show (sent by the tick, without the lock); */
static int asking, aud[3] = { -1, -1, -1 };       /* a question; for audiod: AUDIO_VOL, _OUT, _KEY's w[1] (-1: none) */
static long tick_tid;
static void lock(void)   { while (__atomic_exchange_n(&big, 1, __ATOMIC_ACQUIRE)) sys_yield(); }
static void unlock(void) { __atomic_store_n(&big, 0, __ATOMIC_RELEASE); }

struct sbuf { char *p; int n, cap; };
static void sput(void *c, char ch) { struct sbuf *b = c; if (b->n + 1 < b->cap) b->p[b->n++] = ch; }
static void fmt(char *out, int cap, const char *f, ...)
{
    struct sbuf b = { out, 0, cap };
    va_list ap; va_start(ap, f); vformat(sput, &b, f, ap); va_end(ap);
    out[b.n] = 0;
}
void bt_log(const char *f, ...)
{
    char line[200];
    struct sbuf b = { line, 0, sizeof line };
    va_list ap; va_start(ap, f); vformat(sput, &b, f, ap); va_end(ap);
    sys_debug(line, b.n);
}
void bt_random(void *p, int n) { sys_random(p, n); }

/* ---- The transport: USB (the usb server's pipe) or a UART (H4). */
static long hci;                                  /* port "bthci", 0: the UART */
static volatile int intel_boot;                   /* Intel's bootloader: its commands and events on the bulk pipe */
void tr_send(int type, const uint8_t *p, int n)
{
    if (!hci) { uint8_t t = (uint8_t)type; uart_write(&t, 1); uart_write(p, n); return; }
    msg_t m = { .w = { BTH_SEND, type == H4_ACL || (intel_boot && g16(p) == 0xFC09) }, .sbuf = p, .slen = (size_t)n };
    if (ipc_call(hci, &m) < 0 || (long)m.w[0] < 0) bt_log("bt: the adapter took no packet\n");
}

/* Packets put together from a byte stream: the UART's (a type byte first),
 * or one of USB's pipes (the type is the pipe's). */
typedef struct { int type, n; uint8_t b[L2_MTU + 16]; } rasm_t;
static volatile int ready;
static void packet(int type, const uint8_t *p, int n);
static void rasm(rasm_t *r, const uint8_t *p, int n, int h4)
{
    while (n-- > 0) {
        if (h4 && !r->type) { r->type = *p++; r->n = 0; if (r->type != H4_EVT && r->type != H4_ACL) r->type = 0; continue; }
        if (r->n >= (int)sizeof r->b) { r->n = 0; if (h4) r->type = 0; continue; }   /* (garbage: start again) */
        r->b[r->n++] = *p++;
        int h = r->type == H4_EVT ? 2 : 4;
        if (r->n >= h && r->n == h + (h == 2 ? r->b[1] : g16(r->b + 2))) {
            packet(r->type, r->b, r->n);
            r->n = 0;
            if (h4) r->type = 0;
        }
    }
}
static void receiver(void *a)
{
    (void)a;
    static uint8_t b[4096];
    static rasm_t ra[2];
    for (;;) {
        if (!hci) { rasm(&ra[0], b, uart_read(b, sizeof b), 1); continue; }
        msg_t m = { .w = { BTH_RECV }, .rbuf = b, .rlen = sizeof b };
        if (ipc_call(hci, &m) < 0 || (long)m.w[0] < 0) { bt_log("bt: the adapter is gone\n"); sys_exit(1); }
        for (int i = 0; i + 3 <= (int)m.w[0]; i += 3 + g16(b + i + 1)) {
            int e = b[i] & 1;
            if (!ra[e].n) ra[e].type = e && !intel_boot ? H4_ACL : H4_EVT;
            rasm(&ra[e], b + i + 3, g16(b + i + 1), 0);
        }
    }
}

/* ---- Commands during the start: one at a time, waited for. */
static volatile int sync_op, sync_done, sync_st, sync_n, vend_code;
static uint8_t sync_rsp[260], vend[64];
static void packet(int type, const uint8_t *p, int n)
{
    if (ready) { lock(); bt_rx(type, p, n); unlock(); return; }
    if (type != H4_EVT || n < 3) return;
    if (p[0] == 0xFF) { memcpy(vend, p, n < 64 ? n : 64); vend_code = p[2]; return; }   /* a vendor event */
    if ((p[0] == 0x0E && n >= 6 && g16(p + 3) == sync_op) || (p[0] == 0x0F && n >= 6 && g16(p + 4) == sync_op && p[2])) {
        sync_st = p[0] == 0x0E ? p[5] : p[2];
        sync_n = p[0] == 0x0E ? n - 5 : 0;
        memcpy(sync_rsp, p + 5, sync_n);
        sync_done = 1;
    }
}
static int wait_for(volatile int *flag, int want, int ms)
{
    for (int64_t end = sys_clock() + ms * 1000000L; *flag != want; sys_sleep(500000)) if (sys_clock() > end) return -1;
    return 0;
}
static int hci_sync(int op, const void *p, int n)
{
    uint8_t b[3 + 255] = { (uint8_t)op, (uint8_t)(op >> 8), (uint8_t)n };
    memcpy(b + 3, p, n);
    sync_done = 0; sync_op = op;
    tr_send(H4_CMD, b, 3 + n);
    if (wait_for(&sync_done, 1, 3000)) { bt_log("bt: command %04x: no answer\n", op); return -1; }
    if (sync_st) bt_log("bt: command %04x failed (%02x)\n", op, sync_st);
    return sync_st;
}

/* ---- Intel (btintel.c's "TLV" boot). */
static struct { uint32_t cnvi, cnvr, cnvibt; int img, sbe, nn, ww, yy; } iv;
static int intel_version(void)
{
    static const uint8_t ff = 0xFF;
    if (hci_sync(0xFC05, &ff, 1)) return -1;
    memset(&iv, 0, sizeof iv);
    for (int i = 1; i + 2 <= sync_n && i + 2 + sync_rsp[i + 1] <= sync_n; i += 2 + sync_rsp[i + 1]) {
        const uint8_t *v = sync_rsp + i + 2;
        switch (sync_rsp[i]) {
        case 0x10: iv.cnvi = g32(v); break;
        case 0x11: iv.cnvr = g32(v); break;
        case 0x12: iv.cnvibt = g32(v); break;
        case 0x1C: iv.img = v[0]; break;
        case 0x1D: iv.ww = v[0]; iv.yy = v[1]; break;
        case 0x1F: iv.nn = v[0]; break;
        case 0x2F: iv.sbe = v[0]; break;
        }
    }
    bt_log("bt: Intel: CNVi %08x, CNVr %08x, variant %02x, %s, build %d week %d 20%02d\n", iv.cnvi, iv.cnvr,
           iv.cnvibt >> 16 & 0x3F, iv.img == 1 ? "bootloader" : iv.img == 3 ? "operational firmware" : "image ?", iv.nn, iv.ww, iv.yy);
    return 0;
}
static int secure_send(int type, const uint8_t *p, int n)
{
    for (int k; n > 0; p += k, n -= k) {
        uint8_t b[253] = { (uint8_t)type };
        k = n > 252 ? 252 : n;
        memcpy(b + 1, p, k);
        if (hci_sync(0xFC09, b, k + 1)) return -1;
    }
    return 0;
}
static int intel_fw(char *name, int cap)
{
    int t = iv.cnvi & 0xFFF, r = iv.cnvr & 0xFFF, hv = iv.cnvibt >> 16 & 0x3F;
    int a = (t << 4 | (iv.cnvi >> 24 & 15)), b = (r << 4 | (iv.cnvr >> 24 & 15));
    char path[64];
    fmt(name, cap, "ibt-%04x-%04x", (a >> 8 | a << 8) & 0xFFFF, (b >> 8 | b << 8) & 0xFFFF);
    if (iv.img == 3) { bt_log("bt: Intel: the firmware runs already\n"); return 0; }
    fmt(path, sizeof path, "/lib/firmware/%s.sfi", name);
    size_t len = 0;
    uint8_t *fw = (uint8_t *)file_get(path, &len);
    if (!fw) { bt_log("bt: Intel: %s is missing (make usb puts it on the key)\n", path); return -1; }
    int off = hv >= 0x17 ? 964 : 644, ecdsa = hv >= 0x17 && iv.sbe;
    if (len < 1024 || g32(fw + 8) != 0x10000 || (hv >= 0x17 && (fw[644] != 6 || g32(fw + 652) != 0x20000))) {
        bt_log("bt: Intel: %s: not a firmware file for this chip\n", path); free(fw); return -1;
    }
    uint32_t boot = 0;
    for (size_t i = off; i + 3 <= len; i += 3 + fw[i + 2])
        if (g16(fw + i) == 0xFC0E && i + 10 <= len) {     /* Write Boot Params: where to start, the version */
            boot = g32(fw + i + 3);
            bt_log("bt: Intel: loading %s (%u bytes), firmware %d-%d.%d, boot address %x\n", path, (unsigned)len, fw[i + 7], fw[i + 8], fw[i + 9], boot);
            break;
        }
    int64_t t0 = sys_clock();
    intel_boot = 1;
    const uint8_t *h = fw + (ecdsa ? 644 : 0);
    int e = ecdsa ? secure_send(0, h, 128) || secure_send(3, h + 128, 96) || secure_send(2, h + 224, 96)
                  : secure_send(0, h, 128) || secure_send(3, h + 128, 256) || secure_send(2, h + 388, 256);
    for (size_t p = off, fl = 0; !e && p + fl < len; ) {   /* the commands, sent in 4-byte-aligned pieces */
        fl += 3 + fw[p + fl + 2];
        if (fl % 4 == 0) { e = secure_send(1, fw + p, (int)fl); p += fl; fl = 0; }
    }
    free(fw);
    vend_code = 0;
    if (e || wait_for(&vend_code, 6, 5000) || vend[3]) { bt_log("bt: Intel: the firmware was not accepted (%d)\n", e ? e : vend[3]); return -1; }
    bt_log("bt: Intel: firmware loaded in %d ms; booting it\n", (int)((sys_clock() - t0) / 1000000));
    uint8_t rst[11] = { 0x01, 0xFC, 8, 0x00, 0x01, 0x00, 0x01 };
    memcpy(rst + 7, &boot, 4);
    vend_code = 0;
    tr_send(H4_CMD, rst, 11);                             /* Intel Reset: no answer but the boot event */
    if (wait_for(&vend_code, 2, 5000)) { bt_log("bt: Intel: the firmware did not boot\n"); return -1; }
    intel_boot = 0;
    bt_log("bt: Intel: booted in %d ms\n", (int)((sys_clock() - t0) / 1000000));
    intel_version();
    fmt(path, sizeof path, "/lib/firmware/%s.ddc", name);   /* its settings: optional */
    uint8_t *ddc = (uint8_t *)file_get(path, &len);
    for (size_t i = 0; ddc && i < len; i += ddc[i] + 1) hci_sync(0xFC8B, ddc + i, ddc[i] + 1);
    bt_log("bt: Intel: %s %s\n", path, ddc ? "applied" : "missing (optional)");
    free(ddc);
    return 0;
}

/* ---- Broadcom (btbcm.c): the patch, at the line's top speed. */
static int bcm_speed(int baud)
{
    uint8_t p[6] = { 0, 0 };
    memcpy(p + 2, &baud, 4);
    if (hci_sync(0xFC18, p, 6)) return -1;
    uart_baud(baud);
    sys_sleep(20000000);
    return 0;
}
static int bcm_fw(void)
{
    if (hci_sync(0x0C03, 0, 0) || hci_sync(0x1001, 0, 0)) return -1;
    int sub = g16(sync_rsp + 7);
    bt_log("bt: Broadcom %s, LMP subversion %04x, build %d\n", sub == 0x6119 ? "BCM4345C0" : "chip", sub, g16(sync_rsp + 2) & 0xFFF);
    size_t len = 0;
    uint8_t *fw = (uint8_t *)file_get("/lib/firmware/brcm/BCM4345C0.hcd", &len);
    if (!fw) { bt_log("bt: /lib/firmware/brcm/BCM4345C0.hcd is missing (make sdcard puts it on the card)\n"); return 0; }
    int64_t t0 = sys_clock();
    if (uart_max > 115200) bcm_speed(uart_max);
    int e = hci_sync(0xFC2E, 0, 0);                       /* Download Minidriver */
    sys_sleep(50000000);
    for (size_t i = 0; !e && i + 3 <= len && i + 3 + fw[i + 2] <= len; i += 3 + fw[i + 2]) e = hci_sync(g16(fw + i), fw + i + 3, fw[i + 2]);
    free(fw);
    sys_sleep(250000000);                                 /* Launch RAM: the patched firmware starts, at 115200 */
    uart_baud(115200);
    bt_log("bt: Broadcom patch %s in %d ms\n", e ? "failed" : "loaded", (int)((sys_clock() - t0) / 1000000));
    if (hci_sync(0x0C03, 0, 0)) return -1;
    if (uart_max > 115200) bcm_speed(uart_max);
    if (!hci_sync(0x1001, 0, 0)) bt_log("bt: Broadcom: now build %d\n", g16(sync_rsp + 2) & 0xFFF);
    return 0;
}

/* ---- The keys file: "SBT1", on/off, then the bonded devices (btdev_t). */
#define KEYS "/etc/bluetooth.keys"
void bonds_save(void)
{
    static uint8_t b[8 + BT_MAXDEV * sizeof(btdev_t)];
    int n = 8;
    memcpy(b, "SBT1", 4); b[4] = (uint8_t)bt.on;
    for (int i = 0; i < bt.ndev; i++)
        if (bt.dev[i].d.flags & BTF_BONDED) {
            btdev_t d = bt.dev[i];
            d.d.flags = BTF_BONDED; d.d.rssi = 0;
            memcpy(b + n, &d, sizeof d); n += sizeof d;
        }
    long h = fs_open(KEYS, FS_WRONLY | FS_CREAT | FS_TRUNC, 0600);
    if (h >= 0) { fs_chmod(KEYS, 0600); fs_write(h, 0, b, n); fs_close(h); }
    else bt_log("bt: cannot write " KEYS " (%ld)\n", h);
    wipe(b, sizeof b);
}
static void bonds_load(void)
{
    size_t n = 0;
    uint8_t *b = (uint8_t *)file_get(KEYS, &n);
    bt.on = 1;
    if (b && n >= 8 && !memcmp(b, "SBT1", 4)) {
        bt.on = b[4];
        for (size_t i = 8; i + sizeof(btdev_t) <= n && bt.ndev < BT_MAXDEV; i += sizeof(btdev_t)) memcpy(&bt.dev[bt.ndev++], b + i, sizeof(btdev_t));
    }
    if (b) { wipe(b, n); free(b); }
    bt_log("bt: %d bonded device%s, %s\n", bt.ndev, bt.ndev == 1 ? "" : "s", bt.on ? "on" : "off");
}

/* ---- The start: transport, firmware, set-up; then the clock tick. */
static const char *fail(const char *why) { lock(); bt.state = BTS_FAILED; strlcpy(bt.why, why, sizeof bt.why); unlock(); bt_log("bt: %s\n", why); return why; }
static void start(void *a)
{
    (void)a;
    char what[64] = "", fwname[32] = "";
    long size = sys_fdt(0, 0);
    char *f = size > 0 ? malloc(size) : 0;
    int pi = f && sys_fdt(f, size) == size && (fdt_find(f, -1, "brcm,bcm2711") >= 0 || fdt_find(f, -1, "brcm,bcm2712") >= 0);
    if (!pi) {                                            /* USB: once the usb server has found its devices */
        long usb = port_find("usb");
        msg_t m = { .w = { USB_DISKS } };
        if (usb <= 0 || ipc_call(usb, &m) < 0 || (hci = port_find("bthci")) <= 0) { bt_log("bt: no Bluetooth adapter\n"); sys_exit(0); }
        m = (msg_t){ .w = { BTH_INFO } };
        if (ipc_call(hci, &m) < 0 || (long)m.w[0] < 0) { bt_log("bt: the Bluetooth adapter is gone\n"); sys_exit(0); }
        fmt(what, sizeof what, "USB %04x:%04x", (unsigned)(m.w[0] >> 16), (unsigned)(m.w[0] & 0xFFFF));
    } else if (uart_open(f, what, sizeof what)) { fail(what); for (;;) sys_sleep(3600000000000L); }
    free(f);
    bt_log("bt: adapter: %s\n", what);
    thread_start(receiver, 0, 16384);
    sys_sleep(10000000);
    int intel = !memcmp(what, "USB 8087", 8);
    const char *why = 0;
    if (intel) { if (intel_version() || intel_fw(fwname, sizeof fwname)) why = "the Intel firmware did not load"; }
    else if (pi && bcm_fw()) why = "the controller does not answer";
    if (!why && hci_sync(0x0C03, 0, 0)) why = "the controller does not answer";
    if (!why && hci_sync(0x1009, 0, 0)) why = "no controller address";
    if (why) { fail(why); for (;;) sys_sleep(3600000000000L); }
    lock();
    memcpy(bt.addr, sync_rsp + 1, 6);
    unlock();
    hci_sync(0x1001, 0, 0);
    int manu = g16(sync_rsp + 5), ver = sync_rsp[4];
    hci_sync(0x1005, 0, 0);
    int acl_mtu = g16(sync_rsp + 1), acl_n = g16(sync_rsp + 4);
    hci_sync(0x2002, 0, 0);
    int le_mtu = sync_st ? 0 : g16(sync_rsp + 1), le_n = sync_rsp[3];
    static const uint8_t mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F }, one = 1, le[2] = { 1, 0 }, two = 2, cod[3] = { 0x0C, 0x01, 0x38 };
    static uint8_t name[248] = "SIEOS";
    hci_sync(0x0C01, mask, 8);                            /* every event (SSP's and LE's included) */
    hci_sync(0x0C56, &one, 1);                            /* Secure Simple Pairing */
    hci_sync(0x0C6D, le, 2);                              /* LE */
    hci_sync(0x0C45, &two, 1);                            /* inquiry results with RSSI and names */
    hci_sync(0x0C24, cod, 3);                             /* a laptop computer; sound out, file transfer */
    hci_sync(0x0C13, name, 248);
    hci_sync(0x0C7A, &one, 1);                            /* Secure Connections (P-256) for classic pairing too */
    lock();
    bt.acl_mtu = acl_mtu; bt.acl_free = acl_n; bt.le_mtu = le_mtu; bt.le_free = le_n; bt.ncmd = 1;
    fmt(bt.chip, sizeof bt.chip, "%s%s, %s, Bluetooth %d.%d", intel ? "Intel " : "", intel ? fwname : what,
        manu == 2 ? "Intel" : manu == 15 ? "Broadcom" : "?", ver >= 9 ? 5 : 4, ver >= 9 ? ver - 9 : ver - 6);
    bonds_load();
    bt_log("bt: ready: %s, address %02X:%02X:%02X:%02X:%02X:%02X, ACL %d x %d, LE %d x %d\n", bt.chip,
           bt.addr[5], bt.addr[4], bt.addr[3], bt.addr[2], bt.addr[1], bt.addr[0], acl_n, acl_mtu, le_n, le_mtu);
    ready = 1;
    bt_ready();
    unlock();
    for (int64_t next = sys_clock();;) {                  /* each second, or when woken (to_audiod, a note) */
        static char n[128];
        int64_t d = next + 1000000000 - sys_clock();
        if (d > 0) sys_sleep((uint64_t)d);
        lock();
        if (sys_clock() >= next + 1000000000) { bt_tick(); next += 1000000000; }
        int a[3], q = asking;
        memcpy(a, aud, sizeof a); memset(aud, -1, sizeof aud); asking = 0;
        strlcpy(n, note, sizeof n); note[0] = 0;
        unlock();
        long p = 0;                                       /* (outside the lock: they may be calling us) */
        for (int i = 0; i < 3; i++) {
            msg_t m = { .w = { (uint64_t)(AUDIO_VOL + i), (uint64_t)a[i] } };
            if (a[i] >= 0 && (p > 0 || (p = port_find("audio")) > 0)) ipc_call(p, &m);
        }
        msg_t m = { .w = { ATLAS_NOTE, (uint64_t)q }, .sbuf = n, .slen = strlen(n) + 1 };
        if (n[0] && (p = port_find("atlas")) > 0) ipc_call(p, &m);
    }
}

/* ---- Headphones: audiod's writes land in pcm_ring; sent when due (a2dp_pump). */
static long pump_tid;
static void pump(void *a)
{
    (void)a;
    for (;;) { lock(); int64_t d = a2dp_pump(sys_clock()); unlock(); sys_sleep(d > 0 ? (uint64_t)d : 1000000000); }
}
static void sound(void *a)
{
    (void)a;
    long port = port_create("btaudio"), from = 0, r = 0;
    static char name[32];
    pump_tid = thread_start(pump, 0, 8192);
    for (size_t outn = 0;; ) {
        lock();
        uint32_t at = pcm_w, i = at % PCM_RING, room = PCM_RING - (at - pcm_r);
        unlock();
        msg_t m = { .w = { (uint64_t)r }, .sbuf = name, .slen = outn, .rbuf = pcm_ring + i, .rlen = room < PCM_RING - i ? room : PCM_RING - i };
        from = from > 0 ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m);
        if (from <= 0) continue;
        outn = 0;
        lock();
        const char *s = a2dp_sink();
        if (m.uid) r = -EPERM;
        else if (m.w[0] == AUDIO_OUT) { r = s ? 0 : -ENOENT; if (s) outn = strlcpy(name, s, sizeof name); }
        else if (m.w[0] == AUDIO_VOL) { avrcp_vol((int)m.w[1]); r = 0; }
        else if (m.w[0] != AUDIO_WRITE) r = -ENOSYS;
        else if ((r = a2dp_put(at, (int)(m.rlen & ~3u), (int)m.w[1])) > 0) {
            sys_wake((int)pump_tid);
            while (PCM_RING - (pcm_w - pcm_r) < 4096 && a2dp_sink()) {   /* full: the answer waits for room */
                int64_t d = a2dp_pump(sys_clock());
                unlock(); sys_sleep(d > 0 ? (uint64_t)d : 2000000); lock();
            }
        }
        unlock();
    }
}

void to_audiod(int op, int arg) { aud[op - AUDIO_VOL] = arg; sys_wake((int)tick_tid); }

/* ---- Files received: asked, then into the desktop user's ~/Downloads (nobody's: root's). */
static int desk_uid = -1;
static long rxf = -1;
static uint64_t rxoff;
static char rxpath[160], rxname[64];
void file_ask(const btdev_t *from, const char *name, uint32_t size)
{
    fmt(note, sizeof note, "%s wants to send %s (%u KB).", from->d.name, name, (unsigned)((size + 1023) >> 10));
    strlcpy(rxname, name, sizeof rxname);
    asking = 1;
    sys_wake((int)tick_tid);
    bt_log("bt: OBEX: %s (bt accept / bt decline: 30 s)\n", note);
}
int file_open(const btdev_t *from, const char *name, uint32_t size)
{
    acct_user_t u;
    char dir[96];
    if (rxf >= 0 || acct_user(0, desk_uid >= 0 ? desk_uid : 0, &u)) return -1;
    fmt(dir, sizeof dir, "%s/Downloads", u.home);
    if (!fs_mkdir(dir, 0755)) fs_chown(dir, u.uid, u.gid);
    for (int i = 0; i < 10 && rxf < 0; i++) {             /* (taken: "1-name", "2-name"...) */
        if (i) fmt(rxpath, sizeof rxpath, "%s/%d-%s", dir, i, name); else fmt(rxpath, sizeof rxpath, "%s/%s", dir, name);
        rxf = fs_open(rxpath, FS_WRONLY | FS_CREAT | FS_EXCL, 0644);
    }
    if (rxf < 0) { bt_log("bt: OBEX: cannot write into %s\n", dir); return -1; }
    fs_chown(rxpath, u.uid, u.gid);
    rxoff = 0;
    strlcpy(rxname, name, sizeof rxname);
    fmt(note, sizeof note, "Receiving %s from %s...", name, from->d.name);
    asking = 0;
    bt_log("bt: OBEX: into %s (%u bytes)\n", rxpath, (unsigned)size);
    return 0;
}
int file_write(const void *p, int n)
{
    long r = fs_write(rxf, rxoff, p, (size_t)n);
    if (r > 0) rxoff += (uint64_t)r;
    return (int)r;
}
void file_close(int ok)
{
    if (ok >= 0) { fs_close(rxf); rxf = -1; }
    if (!ok) fs_unlink(rxpath);
    fmt(note, sizeof note, ok > 0 ? "Received %s (Downloads)." : ok ? "%s: not taken." : "Receiving %s failed.", rxname);
    asking = 0;
    sys_wake((int)tick_tid);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    long port = port_create("bt");
    bt.state = BTS_STARTING;
    tick_tid = thread_start(start, 0, 16384);
    thread_start(sound, 0, 16384);
    static bt_status_t st;
    static bt_dev_t list[BT_MAXDEV];
    static uint64_t in[BT_CHUNK / 8 + 16];                /* a request's data: a device, a file's piece */
    const bt_dev_t *want = (const bt_dev_t *)in;
    bt_send_t *snd = (bt_send_t *)in;
    static int sender;                                    /* who is sending a file */
    enum { ROOT = 1 << (BT_PAIR - BT_STATUS) | 1 << (BT_CONFIRM - BT_STATUS) | 1 << (BT_FORGET - BT_STATUS) | 1 << (BT_POWER - BT_STATUS) | 1 << (BT_USER - BT_STATUS) | 1 << (BT_ACCEPT - BT_STATUS) };
    msg_t m = { .rbuf = in, .rlen = sizeof in };
    long from = ipc_recv(port, &m);
    for (;;) {
        long r = 0;
        const void *out = 0;
        size_t outn = 0;
        int root = m.uid == 0, has = m.rlen == sizeof *want;
        lock();
        if (from <= 0) r = -EINVAL;
        else if (m.w[0] == BT_STATUS) {
            memset(&st, 0, sizeof st);
            st.state = (uint8_t)(bt.state == BTS_ON && !bt.on ? BTS_OFF : bt.state);
            st.scanning = bt.scan_left > 0 || bt.inquiring;
            st.pair = (uint8_t)bt.pair; st.code = bt.code;
            memcpy(st.addr, bt.addr, 6);
            if (bt.pdev) memcpy(st.pair_addr, bt.pdev->d.addr, 6);
            strlcpy(st.chip, bt.chip, sizeof st.chip); strlcpy(st.why, bt.why, sizeof st.why);
            out = &st; outn = sizeof st;
        } else if (m.w[0] == BT_LIST) { r = bt_list(list); out = list; outn = r * sizeof list[0]; }
        else if (m.w[0] < BT_SCAN || m.w[0] > BT_ACCEPT) r = -ENOSYS;
        else if (ROOT >> (m.w[0] - BT_STATUS) & 1 && !root) r = -EPERM;
        else if (m.w[0] == BT_USER) desk_uid = (int)m.w[1];
        else if (bt.state != BTS_ON) r = -EAGAIN;
        else switch (m.w[0]) {
        case BT_SCAN: r = bt_scan(); break;
        case BT_PAIR: r = has ? bt_pair(want) : -EINVAL; break;
        case BT_CONFIRM: r = bt_confirm(m.w[1] != 0); break;
        case BT_FORGET: r = has ? bt_forget(want) : -EINVAL; break;
        case BT_POWER: r = bt_power(m.w[1] != 0); bonds_save(); break;
        case BT_CONNECT: r = has ? bt_connect(want, m.w[1] != 0) : -EINVAL; break;
        case BT_SEND:
            snd->name[63] = 0;
            if (!(r = m.rlen == sizeof *snd ? opp_send(snd->addr, snd->name, snd->size) : -EINVAL)) sender = m.pid;
            break;
        case BT_DATA: r = m.pid == sender ? opp_data((const uint8_t *)in, (int)m.rlen, m.w[1] != 0) : -EBUSY; break;
        case BT_ACCEPT: r = opp_answer(m.w[1] != 0); break;
        }
        unlock();
        m = (msg_t){ .w = { (uint64_t)r }, .sbuf = out, .slen = outn, .rbuf = in, .rlen = sizeof in };
        from = from > 0 ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m);
    }
}
