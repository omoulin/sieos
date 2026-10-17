/*
 * bt - The Bluetooth command (the server is btd; Settings does the same
 * through the port "bt", mk/proto.h):
 *
 *   bt                  the state: the controller, its address, on or off
 *   bt scan             look for devices (10 s), then list them
 *   bt list             the devices found and bonded
 *   bt pair ADDR        (root) pair with a device, show the code to compare
 *                       or to type on it, and wait until it is done
 *   bt forget ADDR      (root) forget a bonded device
 *   bt connect ADDR     connect a bonded device (headphones: the sound goes there)
 *   bt disconnect ADDR
 *   bt send ADDR FILE   send a file to a bonded phone or computer (OBEX Object Push)
 *   bt accept | bt decline  (root) the file a device offers (Atlas asks too)
 *   bt on | bt off      (root)
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static const char *const kinds[] = { "", "keyboard", "mouse", "audio", "phone", "computer" };

static long call(msg_t *m)
{
    long p = port_find("bt");
    if (p <= 0) return -ENOENT;
    long e = ipc_call(p, m);
    return e < 0 ? e : (long)m->w[0];
}
static void addr(const uint8_t *a)
{
    printf("%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}
static int parse(const char *s, uint8_t *a)               /* "AA:BB:CC:DD:EE:FF" */
{
    for (int i = 5; i >= 0; i--) {
        int v = 0;
        for (int k = 0; k < 2; k++, s++) {
            int c = *s, d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
        }
        a[i] = (uint8_t)v;
        if (i && *s++ != ':') return -1;
    }
    return *s ? -1 : 0;
}
static int status(bt_status_t *s)
{
    msg_t m = { .w = { BT_STATUS }, .rbuf = s, .rlen = sizeof *s };
    return call(&m) < 0 || m.rlen != sizeof *s ? -1 : 0;
}
static void list(void)
{
    static bt_dev_t d[BT_MAXDEV];
    msg_t m = { .w = { BT_LIST }, .rbuf = d, .rlen = sizeof d };
    long n = call(&m);
    for (long i = 0; i < n; i++) {
        addr(d[i].addr);
        printf("  %-7s %4d dBm  %-24s %-8s%s%s\n", d[i].type ? "LE" : "classic", d[i].rssi, d[i].name[0] ? d[i].name : "(no name)",
               d[i].kind < 6 ? kinds[d[i].kind] : "", d[i].flags & BTF_BONDED ? " bonded" : "",
               d[i].flags & (BTF_INPUT | BTF_AUDIO) ? " in use" : d[i].flags & BTF_CONNECTED ? " connected" : "");
    }
    if (n <= 0) printf("(no devices)\n");
}
static int perm(long r)
{
    if (r == -EPERM) printf("bt: only root may do that (try 'su')\n");
    else if (r == -ENOENT) printf("bt: no such device (bt scan first)\n");
    else if (r == -EAGAIN) printf("bt: Bluetooth is off or not ready\n");
    else if (r == -EBUSY) printf("bt: busy (one file at a time)\n");
    else if (r < 0) printf("bt: failed (%ld)\n", -r);
    return r < 0;
}

/* Follow a pairing: show its code, ask for a comparison, until it ends. */
static int pair(const uint8_t *a)
{
    bt_dev_t d = { 0 };
    memcpy(d.addr, a, 6);
    msg_t m = { .w = { BT_PAIR }, .sbuf = &d, .slen = sizeof d };
    if (perm(call(&m))) return 1;
    printf("pairing...\n");
    bt_status_t s;
    for (int shown = 0, t = 0; t < 1200 && !status(&s); t++, sys_sleep(100000000)) {
        if (s.pair == BTP_CONFIRM) {
            char c[8] = "";
            printf("Does the device show %06u? [y/n] ", s.code);
            con_read(c, sizeof c);
            msg_t k = { .w = { BT_CONFIRM, c[0] == 'y' || c[0] == 'Y' } };
            call(&k);
        } else if (s.pair == BTP_PASSKEY && !shown++) printf("Type %06u on the device, then Enter.\n", s.code);
        else if (s.pair == BTP_DONE) { printf("paired.\n"); return 0; }
        else if (s.pair == BTP_FAILED) { printf("bt: pairing failed: %s\n", s.why); return 1; }
    }
    return 1;
}

/* Send a file: BT_SEND, then its pieces (BT_DATA) as the device takes them. */
static int send(const uint8_t *a, const char *path)
{
    static uint8_t buf[BT_CHUNK];
    siefs_stat_t st;
    long f = fs_open(path, FS_RDONLY, 0), n = 0, at = 0, r;
    if (f < 0 || fs_fstat(f, &st) < 0) { printf("bt: cannot read %s\n", path); return 1; }
    bt_send_t s = { .size = (uint32_t)st.size };
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    memcpy(s.addr, a, 6);
    strlcpy(s.name, b, sizeof s.name);
    msg_t m = { .w = { BT_SEND }, .sbuf = &s, .slen = sizeof s };
    if (perm(call(&m))) return 1;
    printf("sending %s (%u bytes)...\n", s.name, (unsigned)s.size);
    int64_t t0 = sys_clock();
    for (uint64_t off = 0;; ) {
        if (at == n && off < st.size) { if ((n = fs_read(f, off, buf, sizeof buf)) <= 0) { r = -EIO; break; } off += n; at = 0; }
        int end = at == n;
        msg_t d = { .w = { BT_DATA, end }, .sbuf = buf + at, .slen = end ? 0 : (uint64_t)(n - at) };
        if ((r = call(&d)) == -EAGAIN) { sys_sleep(10000000); continue; }
        if (r < 0 || end) break;
        at += r;
    }
    bt_status_t t;
    if (r < 0) { printf("bt: not sent: %s\n", r == -EIO && !status(&t) && t.why[0] ? t.why : "failed"); return 1; }
    printf("sent in %ld ms.\n", (long)((sys_clock() - t0) / 1000000));
    return 0;
}

int main(int argc, char **argv)
{
    bt_status_t s;
    uint8_t a[6];
    if (status(&s)) { printf("bt: no Bluetooth on this machine\n"); return 1; }
    if (argc < 2) {
        static const char *const st[] = { "off", "starting", "on", "failed" };
        printf("controller: %s\nstate:      %s%s%s\naddress:    ", s.chip[0] ? s.chip : "(none)", s.state < 4 ? st[s.state] : "?",
               s.why[0] ? ", last failure: " : "", s.why);
        addr(s.addr);
        printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "scan")) {
        msg_t m = { .w = { BT_SCAN } };
        if (perm(call(&m))) return 1;
        printf("scanning for 10 s...\n");
        sys_sleep(11000000000L);
        list();
        return 0;
    }
    if (!strcmp(argv[1], "list")) { list(); return 0; }
    if (!strcmp(argv[1], "accept") || !strcmp(argv[1], "decline")) {
        msg_t m = { .w = { BT_ACCEPT, argv[1][0] == 'a' } };
        long r = call(&m);
        if (r == -ENOENT) printf("bt: no file offered\n");
        return r == -ENOENT || perm(r);
    }
    if (!strcmp(argv[1], "on") || !strcmp(argv[1], "off")) {
        msg_t m = { .w = { BT_POWER, argv[1][1] == 'n' } };
        return perm(call(&m));
    }
    if (argc == 4 && !parse(argv[2], a) && !strcmp(argv[1], "send")) return send(a, argv[3]);
    if (argc == 3 && !parse(argv[2], a)) {
        if (!strcmp(argv[1], "pair")) return pair(a);
        int op = !strcmp(argv[1], "forget") ? BT_FORGET : !strcmp(argv[1], "connect") || !strcmp(argv[1], "disconnect") ? BT_CONNECT : 0;
        bt_dev_t d = { 0 };
        memcpy(d.addr, a, 6);
        msg_t m = { .w = { (uint64_t)op, argv[1][0] == 'c' }, .sbuf = &d, .slen = sizeof d };
        if (op) return perm(call(&m));
    }
    printf("usage: bt | bt scan | bt list | bt pair ADDR | bt forget ADDR | bt connect ADDR | bt disconnect ADDR\n"
           "       bt send ADDR FILE | bt accept | bt decline | bt on | bt off\n");
    return 1;
}
