/*
 * wifi - The Wi-Fi command (the server is wifid; the desktop's Settings do
 * the same through the "wifi" port, mk/proto.h):
 *
 *   wifi                 the state: the chip, the network, the signal
 *   wifi scan            the networks in range (name, signal, security)
 *   wifi join SSID [PASS] (root) join a network; its passphrase is kept in
 *                        /etc/wifi.conf (root only), and rejoined at boot
 *   wifi forget          (root) leave the network and forget it
 *
 * Starting the command starts wifid on demand (the port "wifi"); on a
 * machine that is not a Raspberry Pi, wifid ends at once and this says so.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static long wifi;
static const char *const states[] = { "off", "idle", "scanning", "joining", "connected", "failed" };
static const char *const secs[] = { "open", "WPA2", "other" };

static int call(msg_t *m) { return (int)call_named(&wifi, "wifi", m, 1); }

static void show_status(void)
{
    wifi_status_t s;
    msg_t m = { .w = { WIFI_STATUS }, .rbuf = &s, .rlen = sizeof s };
    if (call(&m) < 0 || m.rlen != sizeof s) { printf("wifi: no Wi-Fi on this machine\n"); return; }
    printf("chip:    %s\n", s.fw[0] ? s.fw : "(none)");
    printf("state:   %s", s.state <= WIFI_FAILED ? states[s.state] : "?");
    if (s.state == WIFI_FAILED && s.why[0]) printf(" (%s)", s.why);
    printf("\n");
    if (s.ssid[0]) printf("network: %s (%s)\n", s.ssid, s.security < 3 ? secs[s.security] : "?");
    if (s.state == WIFI_UP) {
        printf("signal:  %d dBm, channel %d\n", s.rssi, s.channel);
        printf("address: %02x:%02x:%02x:%02x:%02x:%02x\n", s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5]);
    }
}

static void scan(void)
{
    static wifi_net_t n[WIFI_MAXNET];
    msg_t m = { .w = { WIFI_SCAN }, .rbuf = n, .rlen = sizeof n };
    long r = call(&m);
    if (r < 0) { printf("wifi: no Wi-Fi on this machine\n"); return; }
    printf("%-32s %7s  %s\n", "NETWORK", "SIGNAL", "SECURITY");
    for (long i = 0; i < r; i++)
        printf("%-32s %4d dBm  %s\n", n[i].ssid[0] ? n[i].ssid : "(hidden)", n[i].rssi,
               n[i].security < 3 ? secs[n[i].security] : "?");
    if (!r) printf("(no networks found)\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { show_status(); return 0; }
    if (!strcmp(argv[1], "scan")) { scan(); return 0; }
    if (!strcmp(argv[1], "join") && argc >= 3) {
        char buf[128];
        size_t a = strlcpy(buf, argv[2], sizeof buf) + 1;
        if (argc >= 4) a += strlcpy(buf + a, argv[3], sizeof buf - a) + 1; else buf[a] = 0, a++;
        msg_t m = { .w = { WIFI_JOIN }, .sbuf = buf, .slen = a };
        long r = call(&m);
        if (r == -EPERM) printf("wifi: only root may join a network (try 'su')\n");
        else if (r < 0) printf("wifi: could not join (%ld)\n", -r);
        else printf("wifi: joining %s...\n", argv[2]);
        return r < 0 ? 1 : 0;
    }
    if (!strcmp(argv[1], "forget")) {
        msg_t m = { .w = { WIFI_FORGET } };
        long r = call(&m);
        if (r == -EPERM) printf("wifi: only root may do that\n");
        return r < 0 ? 1 : 0;
    }
    printf("usage: wifi | wifi scan | wifi join SSID [PASSPHRASE] | wifi forget\n");
    return 1;
}
