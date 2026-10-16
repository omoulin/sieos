/*
 * input.c - Bluetooth keyboards and mice, into the console like USB ones
 * (user/lib/hidrep.c turns their reports into CON_INJECT).
 *
 * Classic HID (HIDP): two L2CAP channels, control (PSM 0x11) and interrupt
 * (0x13); we open them after pairing, a bonded device opens them itself
 * when it comes back. We ask for the boot protocol (SET_PROTOCOL 0), which
 * needs no report descriptor (that would take an SDP client): a keyboard's
 * reports are report 1 (the 8 boot bytes), a mouse's report 2 (buttons,
 * X, Y, and most send a wheel byte).
 *
 * HID over GATT (HOGP), a minimal ATT client, one request at a time:
 *   1. Find By Type Value: the HID service (0x1812), its handle range;
 *   2. Find Information over it: each attribute's handle and type: report
 *      values (0x2A4D), the report map (0x2A4B), their descriptors (client
 *      configuration 0x2902, report reference 0x2908) after each;
 *   3. read the report map (in pieces, Read Blob), then each report
 *      reference (the report's id and kind);
 *   4. notifications on for each input report (0x0001 into its 0x2902).
 * Then each notification is a report (its id put first when the map has ids).
 * A peer's own ATT requests get "not supported" (and its MTU question 23).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "btd.h"

#define MAXATT 48
typedef struct gatt {
    int step, start, end, i;
    int na;
    struct { uint16_t h, uuid; uint8_t id, kind, owner; } a[MAXATT];   /* owner: the value a descriptor follows */
    int mapn;
    uint8_t map[1024];
} gatt_t;
enum { G_SERVICE, G_INFO, G_MAP, G_REFS, G_NOTIFY, G_DONE };

static void connected(conn_t *c, hidrep_t *h)
{
    c->hid = h;
    c->dev->d.flags |= BTF_INPUT;
    hid_send(0, 0);                                       /* con: a keyboard or pointer is here */
    bt_log("bt: %s (%s): HID connected (%s%s%s)\n", c->dev->d.name, c->le ? "LE" : "classic",
           h->kn ? "keyboard" : "", h->kn && h->xsz ? ", " : "", h->xsz ? "pointer" : "");
}

void input_gone(conn_t *c)
{
    if (c->hid) { static const uint8_t none[8]; if (c->hid->kn) hid_keys(c->hid->kprev, none); free(c->hid); c->hid = 0; }
    if (c->gatt) { free(c->gatt); c->gatt = 0; }
    c->hidc = c->hidi = 0;
    c->dev->d.flags &= ~BTF_INPUT;
}

/* ---- Classic HID. */
static void hidp_open(l2ch_t *h);
static void hidp_recv(l2ch_t *h, const uint8_t *p, int n)
{
    conn_t *c = h->c;
    if (h == c->hidi && n > 1 && p[0] == 0xA1 && c->hid && c->enc) hid_input(c->hid, p + 1, n - 1);   /* DATA, input */
}
static void hidp_close(l2ch_t *h)
{
    conn_t *c = h->c;
    if (h == c->hidc || h == c->hidi) input_gone(c);
}
static const l2ops_t hidp = { hidp_open, hidp_recv, hidp_close };
static void hidp_open(l2ch_t *h)
{
    conn_t *c = h->c;
    if (h->psm == 0x11) { c->hidc = h; if (c->out && !c->hidi) l2_open(c, 0x13, &hidp); }
    else c->hidi = h;
    if (!c->hidc || !c->hidi || c->hid) return;
    hidrep_t *r = malloc(sizeof *r);
    if (!r) return;
    *r = (hidrep_t){ .krid = 1, .kkeys = 16, .kn = 6,                         /* boot keyboard: report 1 */
                     .rid = 2, .bn = 3, .xpos = 8, .xsz = 8, .ypos = 16, .ysz = 8, .wpos = 24, .wsz = 8 };   /* boot mouse: report 2 */
    static const uint8_t boot = 0x70;                     /* SET_PROTOCOL(boot) */
    l2_send(c->hidc, &boot, 1);
    connected(c, r);
}

void hidp_start(conn_t *c) { if (!c->hidc) l2_open(c, 0x11, &hidp); }
void input_init(void) { l2_listen(0x11, &hidp); l2_listen(0x13, &hidp); }

/* ---- HID over GATT. */
static void att(conn_t *c, int op, int h, int x, int n)  /* a request: op, a handle, then x (16 bits; n: 2 or 4 bytes) */
{
    uint8_t b[5] = { (uint8_t)op };
    p16(b + 1, h); p16(b + 3, x);
    acl_send(c, 4, b, 1 + n);
}

/* The next request, or done. */
static void gatt_next(conn_t *c)
{
    gatt_t *g = c->gatt;
    for (;;) switch (g->step) {
    case G_INFO:
        if (g->start <= g->end && g->na < MAXATT) { att(c, 0x04, g->start, g->end, 4); return; }
        g->step = G_MAP; g->i = -1;
        for (int k = 0; k < g->na; k++) if (g->a[k].uuid == 0x2A4B) g->i = k;
        if (g->i >= 0) { att(c, 0x0A, g->a[g->i].h, 0, 2); return; }
        break;
    case G_MAP: g->step = G_REFS; g->i = -1; /* fall through */
    case G_REFS:
        while (++g->i < g->na && g->a[g->i].uuid != 0x2908) ;
        if (g->i < g->na) { att(c, 0x0A, g->a[g->i].h, 0, 2); return; }
        g->step = G_NOTIFY; g->i = -1; /* fall through */
    case G_NOTIFY:
        while (++g->i < g->na && !(g->a[g->i].uuid == 0x2902 && g->a[g->a[g->i].owner].uuid == 0x2A4D && g->a[g->a[g->i].owner].kind == 1)) ;
        if (g->i < g->na) { uint8_t b[5] = { 0x12 }; p16(b + 1, g->a[g->i].h); b[3] = 1; acl_send(c, 4, b, 5); return; }
        g->step = G_DONE;
        hidrep_t *r = malloc(sizeof *r);
        if (!r) return;
        memset(r, 0, sizeof *r);
        hid_parse(r, g->map, g->mapn);
        bt_log("bt: HID over GATT: %d attributes, report map %d bytes\n", g->na, g->mapn);
        connected(c, r);
        return;
    default: return;
    }
}

void hogp_start(conn_t *c)
{
    if (c->gatt) return;
    if (!(c->gatt = malloc(sizeof *c->gatt))) return;
    memset(c->gatt, 0, sizeof *c->gatt);
    static const uint8_t find[9] = { 0x06, 1, 0, 0xFF, 0xFF, 0x00, 0x28, 0x12, 0x18 };   /* Find By Type Value: */
    acl_send(c, 4, find, 9);                              /* handles 1-FFFF, a primary service (2800), HID (1812) */
}

void att_rx(conn_t *c, const uint8_t *p, int n)
{
    gatt_t *g = c->gatt;
    if (n < 1) return;
    int op = p[0];
    if (op == 0x1B) {                                     /* Handle Value Notification: a report */
        if (!g || g->step != G_DONE || !c->hid || n < 3) return;
        for (int k = 0; k < g->na; k++)
            if (g->a[k].h == g16(p + 1) && g->a[k].uuid == 0x2A4D) {
                uint8_t r[64];
                int id = c->hid->rid || c->hid->krid || c->hid->trid, m = n - 3 < 63 ? n - 3 : 63;
                r[0] = g->a[k].id;
                memcpy(r + id, p + 3, m);
                hid_input(c->hid, r, m + id);
            }
        return;
    }
    if (op == 0x1D) { uint8_t b = 0x1E; acl_send(c, 4, &b, 1); return; }   /* an indication: confirmed */
    if (op == 0x1E) return;
    if (!(op & 1) && !(op & 0x40)) {                      /* a request to us */
        uint8_t b[5] = { 0x01, (uint8_t)op, 0, 0, 0x06 };
        if (op == 0x02) { b[0] = 0x03; b[1] = 23; acl_send(c, 4, b, 3); }
        else acl_send(c, 4, b, 5);
        return;
    }
    if (!g || g->step == G_DONE) return;
    if (op == 0x01) {                                     /* Error Response */
        if (g->step == G_SERVICE) { bt_log("bt: %s has no HID service\n", c->dev->d.name); g->step = G_DONE; return; }
        if (g->step == G_INFO) g->start = g->end + 1;      /* (attribute not found: the end) */
        else if (n >= 5) bt_log("bt: HID over GATT: request %02x on handle %d refused (%02x)\n", p[1], g16(p + 2), p[4]);
        gatt_next(c);
        return;
    }
    switch (g->step) {
    case G_SERVICE:
        if (op != 0x07 || n < 5) return;
        g->start = g16(p + 1); g->end = g16(p + 3);
        g->step = G_INFO;
        break;
    case G_INFO: {
        if (op != 0x05 || n < 2) return;
        int w = p[1] == 1 ? 4 : 18, last = g->end;
        for (int i = 2; i + w <= n && g->na < MAXATT; i += w) {
            int u = w == 4 ? g16(p + i + 2) : 0, k = g->na++;
            g->a[k].h = g16(p + i); g->a[k].uuid = (uint16_t)u;
            g->a[k].owner = (uint8_t)(u >= 0x2900 && u < 0x2A00 && k ? g->a[k - 1].owner : k);
            last = g->a[k].h;
        }
        g->start = last + 1;
        break;
    }
    case G_MAP: {
        int m = n - 1;
        if (op != 0x0B && op != 0x0D) return;
        if (g->mapn + m > (int)sizeof g->map) m = (int)sizeof g->map - g->mapn;
        memcpy(g->map + g->mapn, p + 1, m);
        g->mapn += m;
        if (n - 1 == 22 && m == 22) { att(c, 0x0C, g->a[g->i].h, g->mapn, 4); return; }   /* more: Read Blob */
        break;
    }
    case G_REFS:
        if (op == 0x0B && n >= 3) { int o = g->a[g->i].owner; g->a[o].id = p[1]; g->a[o].kind = p[2]; }
        break;
    }
    gatt_next(c);
}
