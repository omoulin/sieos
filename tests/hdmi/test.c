/*
 * tests/hdmi/test.c - audiod's codec side (user/audiod/codec.c) on the host,
 * against the target laptop's real codecs: their widget graphs as Linux
 * prints them (/proc/asound/cardN/codec#M: alc257.txt, the Realtek on the
 * PCH's HDA link; rpl-hdmi.txt, the Raptor Lake display codec there;
 * nvidia-hdmi.txt, the RTX 5060's HDA function), answered verb by verb by a
 * fake codec. Checked: the analog walk, the HDMI/DP pins and devices (the
 * Intel codec hides its converters and devices until its vendor verb), the
 * ELD built from two real monitors' EDIDs (eld.h, as igfx writes it) and
 * read back through the pin, and the verbs that send the stream to a monitor.
 *
 * Usage: test DIR (where the .txt dumps are).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int id; } hda_t;                /* a controller: 0 the PCH's, 1 NVIDIA's */
#define AUDIO_MUTE 0x100
#include "../../user/audiod/codec.c"

/* ---- The fake codecs. */
typedef struct {
    uint32_t wcaps, pincap, pindef, ampin, ampout;
    int nconn, ndev, sel;                        /* connections, devices (0: no list), device selected */
    uint8_t conn[16];
    const uint8_t *eld[4];                       /* per device: an ELD (the display wrote it) */
    int eldn[4];
} node_t;
typedef struct { uint32_t id, vendor; int first, n, hidden; node_t w[64]; } fcodec_t;
static fcodec_t fc[3];                           /* ALC257 (PCH, 0), RPL HDMI (PCH, 2), NVIDIA (NVIDIA, 0) */
static hda_t pch = { 0 }, nv = { 1 };

static uint32_t amp_caps(const char *s)          /* "ofs=0x57, nsteps=0x57, stepsize=0x02, mute=0" */
{
    unsigned o, n, st, m;
    return sscanf(s, " ofs=0x%x, nsteps=0x%x, stepsize=0x%x, mute=%u", &o, &n, &st, &m) == 4 ? o | n << 8 | st << 16 | m << 31 : 0;
}
static void load(fcodec_t *c, const char *dir, const char *file)
{
    char path[512], l[512];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    node_t *w = 0;
    unsigned v, nid;
    while (fgets(l, sizeof l, f)) {
        if (sscanf(l, "Vendor Id: 0x%x", &v) == 1) c->id = v;
        else if (sscanf(l, "Node 0x%x [%*[^]]] wcaps 0x%x", &nid, &v) == 2) {
            if (!c->first) c->first = nid;
            c->n = nid - c->first + 1;
            w = &c->w[nid]; w->wcaps = v;
        } else if (!w) continue;
        else if (sscanf(l, "  Pincap 0x%x", &v) == 1) w->pincap = v;
        else if (sscanf(l, "  Pin Default 0x%x", &v) == 1) w->pindef = v;
        else if (!strncmp(l, "  Amp-In caps:", 14)) w->ampin = amp_caps(l + 14);
        else if (!strncmp(l, "  Amp-Out caps:", 15)) w->ampout = amp_caps(l + 15);
        else if (sscanf(l, "  Devices: %u", &v) == 1) w->ndev = v;
        else if (sscanf(l, "  Connection: %u", &v) == 1 && v && fgets(l, sizeof l, f))
            for (char *p = l; w->nconn < (int)v && (p = strstr(p, "0x")); p += 2) w->conn[w->nconn++] = (uint8_t)strtoul(p, 0, 16);
    }
    fclose(f);
}

static struct { int ctl, cad, nid; uint32_t v; } trace[4096];
static int nt;
static fcodec_t *model(void) { return h->id ? &fc[2] : cad == 2 ? &fc[1] : cad == 0 ? &fc[0] : 0; }
static uint32_t verb(int nid, uint32_t v)
{
    fcodec_t *c = model();
    if (nt < 4096) trace[nt++] = (typeof(trace[0])){ h->id, cad, nid, v };
    if (!c) return 0;
    node_t *w = &c->w[nid & 63];
    unsigned cmd = v >> 8 & 0xFFF, p = v & 0xFF, type = w->wcaps >> 20 & 15;
    int conn = c->hidden && type == 4 ? 0 : w->nconn;
    if (cmd == 0xF00) switch (p) {
        case 0: return nid ? 0 : c->id;
        case 4: return nid == 0 ? 1 << 16 | 1 : nid == 1 ? (uint32_t)(c->first << 16 | c->n) : 0;
        case 5: return nid == 1 ? 1 : 0;
        case 9: return nid >= c->first ? w->wcaps : 0;
        case 0xC: return w->pincap;
        case 0xD: return w->ampin;
        case 0xE: return conn;
        case 0x12: return w->ampout;
        case 0x15: return w->ndev && !c->hidden ? w->ndev - 1 : 0;
        default: return 0;
    }
    if (cmd == 0xF02) { uint32_t r = 0; for (int i = 0; i < 4 && (int)p + i < conn; i++) r |= (uint32_t)w->conn[p + i] << 8 * i; return r; }
    if (cmd == 0xF1C) return w->pindef;
    if (cmd == 0xF81) return c->vendor;
    if (cmd == 0x781 && (p & 3) == 3 && c->hidden) {   /* Intel: every pin to every converter, 4 devices (pipes) */
        node_t *x = c->w + c->first;
        for (int i = 0, n = 0; i < c->n; i++) if (!(x[i].wcaps >> 20 & 15)) w->conn[n++] = (uint8_t)(c->first + i), w->nconn = n;
        for (int i = 0; i < c->n; i++) if ((x[i].wcaps >> 20 & 15) == 4) { memcpy(x[i].conn, w->conn, 16); x[i].nconn = w->nconn; x[i].ndev = 4; }
        c->hidden = 0;
    }
    if (cmd == 0x781) { c->vendor = p; return 0; }
    if (cmd == 0x735) { if ((int)p < (c->hidden ? 1 : w->ndev ? w->ndev : 1)) w->sel = p; return 0; }
    if (cmd == 0xF35) return w->sel;
    if (cmd == 0xF09) return w->eld[w->sel] ? 0xC0000000 : 0;   /* presence, ELD valid */
    if (cmd == 0xF2F) return w->eld[w->sel] && (int)p < w->eldn[w->sel] ? 0x80000000 | w->eld[w->sel][p] : 0;
    return 0;
}

/* ---- Checks. */
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static int sent(int ctl, int cad, int nid, uint32_t v, int from)   /* was this verb sent (since trace entry from)? */
{
    for (int i = from; i < nt; i++) if (trace[i].ctl == ctl && trace[i].cad == cad && trace[i].nid == nid && trace[i].v == v) return 1;
    return 0;
}
static void show(int from, const char *what)     /* the set verbs (no gets) since from */
{
    printf("%s:", what);
    for (int i = from; i < nt; i++)
        if ((trace[i].v >> 16) < 0xF && !((trace[i].v >> 19) == 1 && (trace[i].v >> 16 & 15) >= 0xA))
            printf(" %d.%d.%02x:%05x", trace[i].ctl, trace[i].cad, trace[i].nid, trace[i].v);
    printf("\n");
}

/* Two real monitors (linuxhw/EDID): Samsung C24F390 (HDMI, SAM0D2C) and Dell U2718Q (DP, DELA0EC). */
static const uint8_t samsung[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x4c, 0x2d, 0x2c, 0x0d, 0x57, 0x4a, 0x50, 0x30,
    0x2e, 0x19, 0x01, 0x03, 0x80, 0x34, 0x1d, 0x78, 0x2a, 0x52, 0x95, 0xa5, 0x56, 0x54, 0x9d, 0x25,
    0x0e, 0x50, 0x54, 0xbb, 0x8c, 0x00, 0xb3, 0x00, 0x81, 0xc0, 0x81, 0x00, 0x81, 0x80, 0x95, 0x00,
    0xa9, 0xc0, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
    0x45, 0x00, 0x09, 0x25, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x30, 0x48, 0x1e,
    0x52, 0x16, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x43,
    0x32, 0x34, 0x46, 0x33, 0x39, 0x30, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x48, 0x54, 0x48, 0x4a, 0x42, 0x30, 0x30, 0x33, 0x32, 0x31, 0x0a, 0x20, 0x20, 0x01, 0x46,
    0x02, 0x03, 0x24, 0xf1, 0x46, 0x90, 0x04, 0x1f, 0x13, 0x12, 0x03, 0x23, 0x09, 0x07, 0x07, 0x83,
    0x01, 0x00, 0x00, 0x67, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x80, 0x2b, 0x68, 0x1a, 0x00, 0x00, 0x01,
    0x01, 0x30, 0x48, 0x00, 0x01, 0x1d, 0x00, 0xbc, 0x52, 0xd0, 0x1e, 0x20, 0xb8, 0x28, 0x55, 0x40,
    0x09, 0x25, 0x21, 0x00, 0x00, 0x1e, 0x8c, 0x0a, 0xd0, 0x90, 0x20, 0x40, 0x31, 0x20, 0x0c, 0x40,
    0x55, 0x00, 0x09, 0x25, 0x21, 0x00, 0x00, 0x18, 0x8c, 0x0a, 0xd0, 0x8a, 0x20, 0xe0, 0x2d, 0x10,
    0x10, 0x3e, 0x96, 0x00, 0x09, 0x25, 0x21, 0x00, 0x00, 0x18, 0x21, 0x52, 0x80, 0xa0, 0x72, 0x38,
    0x30, 0x40, 0x88, 0xc8, 0x35, 0x00, 0x09, 0x25, 0x21, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfa };
static const uint8_t dell[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x10, 0xac, 0xec, 0xa0, 0x4c, 0x39, 0x4a, 0x30,
    0x29, 0x1c, 0x01, 0x04, 0xb5, 0x3d, 0x23, 0x78, 0x3e, 0xee, 0x95, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0xa5, 0x4b, 0x00, 0x71, 0x4f, 0x81, 0x80, 0xa9, 0xc0, 0xa9, 0x40, 0xd1, 0xc0,
    0xe1, 0x00, 0xd1, 0x00, 0x01, 0x01, 0x4d, 0xd0, 0x00, 0xa0, 0xf0, 0x70, 0x3e, 0x80, 0x30, 0x20,
    0x35, 0x00, 0x61, 0x5d, 0x21, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0xff, 0x00, 0x34, 0x4b, 0x38,
    0x58, 0x37, 0x38, 0x41, 0x39, 0x30, 0x4a, 0x39, 0x4c, 0x0a, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x44,
    0x45, 0x4c, 0x4c, 0x20, 0x55, 0x32, 0x37, 0x31, 0x38, 0x51, 0x0a, 0x20, 0x00, 0x00, 0x00, 0xfd,
    0x00, 0x31, 0x56, 0x0a, 0x89, 0x36, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0xda,
    0x02, 0x03, 0x1d, 0xf1, 0x50, 0x10, 0x1f, 0x20, 0x05, 0x14, 0x04, 0x13, 0x12, 0x11, 0x03, 0x02,
    0x16, 0x15, 0x07, 0x06, 0x01, 0x23, 0x09, 0x1f, 0x07, 0x83, 0x01, 0x00, 0x00, 0x56, 0x5e, 0x00,
    0xa0, 0xa0, 0xa0, 0x29, 0x50, 0x30, 0x20, 0x35, 0x00, 0x61, 0x5d, 0x21, 0x00, 0x00, 0x1a, 0xa3,
    0x66, 0x00, 0xa0, 0xf0, 0x70, 0x1f, 0x80, 0x30, 0x20, 0x35, 0x00, 0x61, 0x5d, 0x21, 0x00, 0x00,
    0x1a, 0x4d, 0xd0, 0x00, 0xa0, 0xf0, 0x70, 0x3e, 0x80, 0x30, 0x20, 0x35, 0x00, 0x61, 0x5d, 0x21,
    0x00, 0x00, 0x1a, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c, 0x25, 0x00, 0x61,
    0x5d, 0x21, 0x00, 0x00, 0x1e, 0xbf, 0x16, 0x00, 0xa0, 0x80, 0x38, 0x13, 0x40, 0x30, 0x20, 0x3a,
    0x00, 0x61, 0x5d, 0x21, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a };

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tests/hdmi";
    load(&fc[0], dir, "alc257.txt");
    load(&fc[1], dir, "rpl-hdmi.txt");
    load(&fc[2], dir, "nvidia-hdmi.txt");
    fc[1].hidden = 1;                            /* (as dumped: no connections, no devices yet) */
    CHECK(fc[0].id == 0x10ec0257 && fc[1].id == 0x80862818 && fc[2].id == 0x10de00ad);

    /* ELDs from the EDIDs, as igfx builds them; and read back. */
    uint8_t es[ELD_MAX], ed[ELD_MAX], no[256];
    char name[17];
    int ns = eld_build(samsung, 0, es), nd = eld_build(dell, 1, ed);
    memcpy(no, samsung, 256); no[131] &= ~0x40;  /* (no basic audio: no ELD) */
    static const uint8_t es_want[32] = { 0x10, 0, 7, 0, 0x67, 0x10, 0, 0x01, 0, 0, 0, 0, 0, 0, 0, 0,
        0x4c, 0x2d, 0x2c, 0x0d, 'C', '2', '4', 'F', '3', '9', '0', 0x09, 0x07, 0x07, 0, 0 };
    printf("ELD C24F390 (%d bytes):", ns); for (int i = 0; i < ns; i++) printf(" %02x", es[i]); printf("\n");
    printf("ELD U2718Q (%d bytes):", nd); for (int i = 0; i < nd; i++) printf(" %02x", ed[i]); printf("\n");
    CHECK(ns == 32 && !memcmp(es, es_want, 32));
    CHECK(nd == 36 && ed[4] == (3 << 5 | 11) && ed[5] == (1 << 4 | 4) && ed[7] == 1 && !memcmp(ed + 31, "\x09\x1f\x07", 3));
    CHECK(!eld_build(no, 0, es));                /* (returns before touching es) */
    CHECK(!eld_name(es, ns, name) && !strcmp(name, "C24F390"));
    CHECK(!eld_name(ed, nd, name) && !strcmp(name, "DELL U2718Q"));
    CHECK(eld_name(ed, 25, name) < 0);           /* (cut short) */

    /* The walk: the PCH's codecs 0 and 2, NVIDIA's codec 0. */
    int t0 = nt;
    h = &pch; CHECK(codec(0) == 0);
    CHECK(ach == &pch && spk && hp && dac[0]);
    show(t0, "ALC257 set-up");
    t0 = nt;
    h = &pch; CHECK(codec(2) < 0);
    CHECK(sent(0, 2, 2, 0x78103, t0));           /* the vendor verb: all pins, converters; DP 1.2 */
    CHECK(npin == 9 && hpin[0].nid == 4 && hpin[3].nid == 0xA && hpin[8].nid == 0xF);
    h = &nv; CHECK(codec(0) < 0);
    CHECK(npin == 11 && hpin[9].c == &nv && hpin[9].nid == 5 && hpin[10].nid == 6);   /* (4 and 7: not wired, says the pin default) */
    CHECK(ach == &pch && acad == 0);             /* (still the Realtek's) */
    CHECK(hdmi_scan(1) == 0 && nsink == 0);

    /* The display writes ELDs: the Samsung on pipe B of the Intel pin 6 (device 1), the Dell on NVIDIA's pin 5. */
    fc[1].w[6].eld[1] = es; fc[1].w[6].eldn[1] = ELD_MAX;
    fc[2].w[5].eld[0] = ed; fc[2].w[5].eldn[0] = ELD_MAX;
    CHECK(hdmi_scan(1) == 0 && nsink == 2);
    CHECK(sink[0].c == &pch && sink[0].cad == 2 && sink[0].nid == 6 && sink[0].dev == 1 && sink[0].cvt == 3 && !sink[0].dp && !strcmp(sink[0].name, "C24F390"));
    CHECK(sink[1].c == &nv && sink[1].nid == 5 && sink[1].dev == 0 && sink[1].cvt == 8 && sink[1].dp && !strcmp(sink[1].name, "DELL U2718Q"));

    /* Playing to the Samsung (HDMI): the pin's device, converter, infoframe; the DACs silent. */
    t0 = nt;
    route_out(1);
    show(t0, "to the Samsung");
    CHECK(sel == 1);
    CHECK(sent(0, 2, 6, 0x73501, t0) && sent(0, 2, 6, 0x70100, t0) && sent(0, 2, 6, 0x70740, t0) && sent(0, 2, 6, 0x732C0, t0));
    CHECK(sent(0, 2, 3, 0x70D01, t0) && sent(0, 2, 3, 0x72D01, t0) && sent(0, 2, 3, 0x20011, t0) && sent(0, 2, 3, 0x70620, t0));
    CHECK(sent(0, 0, dac[0], 0x70600, t0));
    static const uint8_t ai_hdmi[9] = { 0x84, 1, 10, 0x70, 1 };
    int k = 0, sum = 0;
    for (int i = t0; i < nt; i++) if (trace[i].nid == 6 && trace[i].v >> 8 == 0x731) { CHECK(k < 9 && (trace[i].v & 0xFF) == ai_hdmi[k]); sum += trace[i].v & 0xFF; k++; }
    CHECK(k == 9 && (sum & 0xFF) == 0);          /* (the infoframe's checksum) */

    /* To the Dell (NVIDIA, DP): the Samsung's converter stopped; DP's infoframe. Muted: its converter stopped. */
    t0 = nt;
    route_out(2);
    show(t0, "to the Dell");
    CHECK(sent(0, 2, 3, 0x70600, t0) && sent(1, 0, 5, 0x73500, t0) && sent(1, 0, 8, 0x70620, t0) && sent(1, 0, 5, 0x73100 | 0x1B, t0));
    t0 = nt;
    vol |= AUDIO_MUTE; set_vol();
    CHECK(sent(1, 0, 8, 0x70600, t0));
    vol &= ~AUDIO_MUTE; set_vol();

    /* The Dell unplugged: the scan says so; back to the speakers. */
    fc[2].w[5].eld[0] = 0;
    CHECK(hdmi_scan(0) == 1 && sel == 0 && nsink == 1);
    t0 = nt;
    route_out(0);
    jack();
    CHECK(sent(0, 0, dac[0], 0x70620, t0));
    CHECK(sent(0, 0, dac[0], 0x3B000 | ((ampcap(dac[0], 1) & 0x7F) * 80 / 100), t0));   /* (volume 80) */

    printf(fails ? "hdmi-test: %d FAILED\n" : "hdmi-test: all passed (%d verbs)\n", fails ? fails : nt);
    return fails != 0;
}
