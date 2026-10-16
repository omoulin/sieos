/*
 * wifid - The Wi-Fi server: the port "wifi", a second network interface for
 * netd, and the chosen network. One chip driver per architecture does the
 * rest (wifi.h): bcm.c, the Raspberry Pis' CYW43455 on SDIO (FullMAC: its
 * firmware joins and runs the handshake), or iwl.c, an Intel AX210 on PCIe
 * (x86-64: the host joins and runs the handshake, wpa.c).
 *
 *   netd ◄──NET_FRAMES / NET_LINK── wifid ──► the chip
 *        ──NIC_SEND / NIC_INFO────► (port "wifi", like vnet's "nic0")
 *
 *   Settings (the desktop) and /bin/wifi send WIFI_SCAN, WIFI_JOIN (root:
 *   the passphrase is kept in /etc/wifi.conf, root only), WIFI_STATUS. The
 *   chosen network is joined again at every start.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "wifi.h"

volatile int wifi_state = WIFI_OFF;
char wifi_why[63], wifi_desc[64];
uint8_t wifi_mac[6], wifi_bssid[6];
int wifi_chan, wifi_rssi;
static char cfg_ssid[33], cfg_pass[65];

static long net_port(void)
{
    static long p;
    if (p <= 0) p = port_find("net");
    return p;
}
void wifi_link(int up)
{
    if (up) wifi_state = WIFI_UP; else if (wifi_state == WIFI_UP) wifi_state = WIFI_IDLE;
    printf("wifi: link %s\n", up ? "up" : "down");
    msg_t m = { .w = { NET_LINK, (uint64_t)up, NET_IF_WIFI } };
    long p = net_port();
    if (p > 0 && ipc_call(p, &m) < 0) { p = port_find("net"); if (p > 0) ipc_call(p, &m); }
}
void wifi_frames(const void *batch, size_t n)
{
    msg_t m = { .w = { NET_FRAMES, NET_IF_WIFI }, .sbuf = batch, .slen = n };
    long p = net_port();
    if (n && p > 0) ipc_call(p, &m);
}

/* ---- The configuration file (root only): the chosen network. */
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
static int save_conf(void)
{
    static char out[160];
    int len = 0;
    len += (int)strlcpy(out + len, "ssid=", sizeof out - len);
    len += (int)strlcpy(out + len, cfg_ssid, sizeof out - len);
    out[len++] = '\n';
    len += (int)strlcpy(out + len, "passphrase=", sizeof out - len);
    len += (int)strlcpy(out + len, cfg_pass, sizeof out - len);
    out[len++] = '\n';
    long h = fs_open("/etc/wifi.conf", FS_WRONLY | FS_CREAT | FS_TRUNC, 0600);
    if (h < 0) return (int)h;
    fs_chmod("/etc/wifi.conf", 0600);
    long r = fs_write(h, 0, out, len);
    fs_close(h);
    return r < 0 ? (int)r : 0;
}

static void join(void)
{
    printf("wifi: joining \"%s\"\n", cfg_ssid);
    wifi_state = WIFI_JOINING; wifi_why[0] = 0;
    chip_join(cfg_ssid, cfg_pass);
}

static void fill_status(wifi_status_t *s)
{
    memset(s, 0, sizeof *s);
    s->state = wifi_state;
    memcpy(s->mac, wifi_mac, 6);
    strlcpy(s->fw, wifi_desc, sizeof s->fw);
    strlcpy(s->ssid, cfg_ssid, sizeof s->ssid);
    strlcpy(s->why, wifi_why, sizeof s->why);
    s->channel = wifi_chan; s->rssi = wifi_rssi;
    memcpy(s->bssid, wifi_bssid, 6);
    s->security = cfg_pass[0] ? WIFI_WPA2 : WIFI_OPEN;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int r0 = chip_up();
    if (r0 > 0) { printf("wifi: no Wi-Fi chip here; nothing to do\n"); return 0; }   /* init does not start it again */
    if (r0 < 0) { printf("wifi: the chip did not come up\n"); return 2; }
    wifi_state = WIFI_IDLE;
    load_conf();
    if (cfg_ssid[0]) join();

    static uint8_t txmem[NET_MAX];               /* NIC_SEND's frames land here */
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
            memcpy(info.mac, wifi_mac, 6); info.mtu = 1500; info.link = wifi_state == WIFI_UP;
            out = &info; outn = sizeof info; break;
        case NIC_SEND:
            r = m.uid != 0 ? -EPERM : chip_send(txmem, m.rlen); break;
        case WIFI_STATUS:
            fill_status(&st); out = &st; outn = sizeof st; break;
        case WIFI_SCAN:
            r = chip_scan(list); out = list; outn = (size_t)r * sizeof list[0]; break;
        case WIFI_JOIN:
            if (m.uid != 0) { r = -EPERM; break; }
            { const char *ssid = (const char *)m.sbuf, *pass = ssid + strlen(ssid) + 1;
              if (m.slen <= strlen(ssid) + 1) pass = "";
              strlcpy(cfg_ssid, ssid, sizeof cfg_ssid); strlcpy(cfg_pass, pass, sizeof cfg_pass);
              if (!(r = save_conf())) join(); }
            break;
        case WIFI_FORGET:
            if (m.uid != 0) { r = -EPERM; break; }
            chip_leave();
            fs_unlink("/etc/wifi.conf");
            cfg_ssid[0] = cfg_pass[0] = 0;
            if (wifi_state == WIFI_UP) wifi_link(0);
            wifi_state = WIFI_IDLE;
            break;
        case SVC_MAYSTOP:                        /* kept while a network is joined (never idle then) */
            if ((r = maystop_answer(&m, 0, &seen, wifi_state == WIFI_UP || wifi_state == WIFI_JOINING)) == 0) { reply_val(from, 0); return 0; }
            break;
        default: r = -ENOSYS;
        }
        m = (msg_t){ .w = { (uint64_t)r }, .sbuf = out, .slen = outn, .rbuf = txmem, .rlen = NET_MAX };
        from = from > 0 ? ipc_reply_recv(from, port, &m) : ipc_recv(port, &m);
    }
}
