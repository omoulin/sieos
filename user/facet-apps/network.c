/*
 * facet-network - Network Status: an interface (Tab or a click on it: the
 * next one), its traffic graph, and the sockets.
 * A Facet application (libfacet).
 */
#include "common.h"

#define NET_HIST 60

struct netapp {
    struct netinfo ni;
    bool ok;
    int cur, nif;                                /* the interface shown, and how many there are */
    unsigned long prev_rx, prev_tx;
    int rx_rate[NET_HIST], tx_rate[NET_HIST];   /* bytes per second */
    int pos, ticks;
    struct sockinfo socks[32];
    int nsocks;
};

static void netapp_sample(struct netapp *a)
{
    a->nif = 0;
    for (struct netinfo tmp; netinfo_if(&tmp, a->nif) == 0; )
        a->nif++;
    if (a->cur >= a->nif)
        a->cur = 0;
    a->ok = netinfo_if(&a->ni, a->cur) == 0;
    if (a->ok) {
        if (a->prev_rx || a->prev_tx) {
            a->rx_rate[a->pos] = (int)(a->ni.rx_bytes - a->prev_rx);
            a->tx_rate[a->pos] = (int)(a->ni.tx_bytes - a->prev_tx);
            a->pos = (a->pos + 1) % NET_HIST;
        }
        a->prev_rx = a->ni.rx_bytes;
        a->prev_tx = a->ni.tx_bytes;
    }
    a->nsocks = netstat(a->socks, 32);
    if (a->nsocks < 0)
        a->nsocks = 0;
}

static void netapp_draw(struct fct_view *w, struct surface *s, struct rect c)
{
    struct netapp *a = w->app;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_FACE);
    icon_draw(s, ICON_NETWORK, c.x + 12, c.y + 10, 40);
    char line[96], b1[16], b2[16], b3[16];
    if (!a->ok) {
        gfx_text(s, c.x + 64, c.y + 20, "No network interface.", C_TEXT);
        return;
    }
    if (a->nif > 1)
        snprintf(line, sizeof(line), "%s  %s  (%s, %s)   %d of %d, Tab: next", a->ni.name, ip_to_str(a->ni.ip, b1),
                 a->ni.driver, a->ni.up ? (a->ni.dhcp ? "DHCP" : "static") : "no address", a->cur + 1, a->nif);
    else
        snprintf(line, sizeof(line), "%s  %s  (%s, %s)", a->ni.name, ip_to_str(a->ni.ip, b1), a->ni.driver,
                 a->ni.up ? (a->ni.dhcp ? "DHCP" : "static") : "no address");
    gfx_text(s, c.x + 64, c.y + 10, line, C_TEXT);
    snprintf(line, sizeof(line), "netmask %s   gateway %s", ip_to_str(a->ni.netmask, b1), ip_to_str(a->ni.gateway, b2));
    gfx_text(s, c.x + 64, c.y + 28, line, C_DIM);
    snprintf(line, sizeof(line), "dns %s   mac %02x:%02x:%02x:%02x:%02x:%02x", ip_to_str(a->ni.dns, b3),
             a->ni.mac[0], a->ni.mac[1], a->ni.mac[2], a->ni.mac[3], a->ni.mac[4], a->ni.mac[5]);
    gfx_text(s, c.x + 64, c.y + 46, line, C_DIM);

    /* traffic graph */
    int gy = c.y + 72, gh = 90;
    struct rect g = rect_make(c.x + 10, gy, c.w - 20, gh);
    gfx_fill(s, g.x, g.y, g.w, g.h, RGB(0x10, 0x18, 0x20));
    gfx_bevel(s, g.x, g.y, g.w, g.h, 1, false, C_LINE, C_FACE_DARK);
    int peak = 1024;
    for (int k = 0; k < NET_HIST; k++)
        peak = MAX(peak, MAX(a->rx_rate[k], a->tx_rate[k]));
    for (int pass = 0; pass < 2; pass++) {
        int *rate = pass ? a->tx_rate : a->rx_rate;
        color_t col = pass ? RGB(0xFF, 0xB0, 0x30) : RGB(0x60, 0xE0, 0x70);
        int px = -1, py = 0;
        for (int k = 0; k < NET_HIST; k++) {
            int v = rate[(a->pos + k) % NET_HIST];
            int xx = g.x + 2 + k * (g.w - 4) / (NET_HIST - 1), yy = g.y + g.h - 3 - (int)((long)v * (g.h - 6) / peak);
            if (px >= 0)
                gfx_line(s, px, py, xx, yy, col);
            px = xx;
            py = yy;
        }
    }
    int last = (a->pos + NET_HIST - 1) % NET_HIST;
    snprintf(line, sizeof(line), "in %d B/s", a->rx_rate[last]);
    gfx_text(s, g.x + 6, g.y + 4, line, RGB(0x60, 0xE0, 0x70));
    snprintf(line, sizeof(line), "out %d B/s", a->tx_rate[last]);
    gfx_text(s, g.x + 130, g.y + 4, line, RGB(0xFF, 0xB0, 0x30));

    snprintf(line, sizeof(line), "RX %lu packets, %lu KB     TX %lu packets, %lu KB", a->ni.rx_packets,
             a->ni.rx_bytes / 1024, a->ni.tx_packets, a->ni.tx_bytes / 1024);
    gfx_text(s, c.x + 10, gy + gh + 8, line, C_TEXT);

    /* sockets */
    static const char *states[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
                                    "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };
    struct rect t = rect_make(c.x + 10, gy + gh + 30, c.w - 20, c.y + c.h - (gy + gh + 30) - 10);
    gfx_fill(s, t.x, t.y, t.w, t.h, C_CONTENT);
    gfx_bevel(s, t.x, t.y, t.w, t.h, 1, false, C_LINE, C_FACE_DARK);
    gfx_fill(s, t.x + 1, t.y + 1, t.w - 2, 18, RGB(0x26, 0x29, 0x2E));
    gfx_text_mono(s, t.x + 6, t.y + 2, "Proto Local                 Remote                State", C_TEXT);   /* columns: monospace */
    struct rect clip = s->clip;
    gfx_set_clip(s, rect_intersect(clip, t));
    for (int i = 0; i < a->nsocks; i++) {
        struct sockinfo *si = &a->socks[i];
        char l[28], r[28];
        snprintf(l, sizeof(l), "%s:%u", si->lip ? ip_to_str(si->lip, b1) : "*", si->lport);
        if (si->rip || si->rport)
            snprintf(r, sizeof(r), "%s:%u", ip_to_str(si->rip, b2), si->rport);
        else
            strcpy(r, "*:*");
        snprintf(line, sizeof(line), "%-5s %-21s %-21s %s",
                 si->proto == IPPROTO_TCP ? "tcp" : si->proto == IPPROTO_UDP ? "udp" : "raw", l, r,
                 si->proto == IPPROTO_TCP && si->state <= 10 ? states[si->state] : "");
        gfx_text_mono(s, t.x + 6, t.y + 21 + i * 17, line, C_TEXT);
    }
    gfx_set_clip(s, clip);
}

static void netapp_tick(struct fct_view *w)
{
    struct netapp *a = w->app;
    if (++a->ticks % 4)
        return;
    netapp_sample(a);
    fct_view_invalidate(w);
}

static void netapp_next(struct fct_view *w)
{
    struct netapp *a = w->app;
    if (a->nif < 2)
        return;
    a->cur = (a->cur + 1) % a->nif;
    memset(a->rx_rate, 0, sizeof(a->rx_rate));   /* a new graph */
    memset(a->tx_rate, 0, sizeof(a->tx_rate));
    a->prev_rx = a->prev_tx = 0;
    netapp_sample(a);
    fct_view_invalidate(w);
}

static void netapp_key(struct fct_view *w, const struct fct_key *k)
{
    if (k->value && k->ascii == '\t')
        netapp_next(w);
}

static void netapp_mouse(struct fct_view *w, int x, int y, int kind, int buttons)
{
    (void)x;
    (void)buttons;
    if (kind == FCT_MOUSE_DOWN && y >= 0 && y < 64)     /* (content coordinates) */
        netapp_next(w);
}

static void netapp_destroy(struct fct_view *w)
{
    free(w->app);
}

int main(void)
{
    struct netapp *a = calloc(1, sizeof(*a));
    if (!a || fct_app_init() < 0)
        return 1;
    netapp_sample(a);
    struct fct_window_attr at = { "Network Status", FCT_POS_AUTO, FCT_POS_AUTO, 520, 400, 454, 271, 0 };
    struct fct_view *w = fct_view_create(&at);
    if (!w)
        return 1;
    w->app = a;
    w->draw = netapp_draw;
    w->tick = netapp_tick;
    w->key = netapp_key;
    w->mouse = netapp_mouse;
    w->destroy = netapp_destroy;
    return fct_main();
}
