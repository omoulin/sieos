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
               d[i].flags & BTF_INPUT ? " in use" : d[i].flags & BTF_CONNECTED ? " connected" : "");
    }
    if (n <= 0) printf("(no devices)\n");
}
static int perm(long r)
{
    if (r == -EPERM) printf("bt: only root may do that (try 'su')\n");
    else if (r == -ENOENT) printf("bt: no such device (bt scan first)\n");
    else if (r == -EAGAIN) printf("bt: Bluetooth is off or not ready\n");
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
    if (!strcmp(argv[1], "on") || !strcmp(argv[1], "off")) {
        msg_t m = { .w = { BT_POWER, argv[1][1] == 'n' } };
        return perm(call(&m));
    }
    if (argc == 3 && !parse(argv[2], a)) {
        if (!strcmp(argv[1], "pair")) return pair(a);
        if (!strcmp(argv[1], "forget")) {
            bt_dev_t d = { 0 };
            memcpy(d.addr, a, 6);
            msg_t m = { .w = { BT_FORGET }, .sbuf = &d, .slen = sizeof d };
            return perm(call(&m));
        }
    }
    printf("usage: bt | bt scan | bt list | bt pair ADDR | bt forget ADDR | bt on | bt off\n");
    return 1;
}
