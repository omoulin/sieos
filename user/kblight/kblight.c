/*
 * kblight - The keyboard's light (the USB server's port "kblight", the
 * Lenovo Legion's ITE 8258; Settings does the same):
 *
 *   kblight                     the level and colour now
 *   kblight off|low|high [C]    set them; C: white, red, orange, green,
 *                               cyan, blue, purple, or RRGGBB in hex
 *   kblight C                   the colour, at the level it has (off: high)
 *
 * The setting is kept (/etc/kblight) and given to the keyboard at each start.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static const char *const lv[3] = { "off", "low", "high" };
static const struct { const char *name; int rgb; } kc[KBL_NCOL] = KBL_COLOURS;

static long call(uint64_t op, uint64_t a, uint64_t b, int *rgb)
{
    long p = port_find("kblight");
    msg_t m = { .w = { op, a, b } };
    long e = p <= 0 ? -ENOENT : ipc_call(p, &m);
    if (e >= 0) { e = (long)m.w[0]; *rgb = (int)m.w[1]; }
    return e;
}

static int same(const char *a, const char *b) { while (*a && (*a | 32) == (*b | 32)) a++, b++; return !*a && !*b; }

int main(int argc, char **argv)
{
    int rgb = 0;
    long l = call(KBL_GET, 0, 0, &rgb);
    if (l < 0) { printf("kblight: no keyboard light on this machine\n"); return 1; }
    long nl = -1;
    for (int i = 1; i < argc; i++) {
        int k = 0, h = 0, n = 0;
        while (k < 3 && !same(argv[i], lv[k])) k++;
        if (k < 3) { nl = k; continue; }
        for (k = 0; k < KBL_NCOL && !same(argv[i], kc[k].name); k++) ;
        if (k < KBL_NCOL) { rgb = kc[k].rgb; continue; }
        for (const char *p = argv[i]; *p; p++, n++) {
            int c = *p | 32, d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) n = 99;
            h = h << 4 | (d & 15);
        }
        if (n != 6) { printf("usage: kblight [off|low|high] [white|red|orange|green|cyan|blue|purple|RRGGBB]\n"); return 1; }
        rgb = h;
    }
    if (argc > 1) {
        if (nl < 0) nl = l ? l : KBL_HIGH;
        if (call(KBL_SET, nl, rgb, &rgb) < 0) { printf("kblight: the keyboard did not take it\n"); return 1; }
        l = nl;
    }
    const char *name = 0;
    for (int k = 0; k < KBL_NCOL; k++) if (kc[k].rgb == rgb) name = kc[k].name;
    printf("keyboard light: %s, %s%s%06x%s\n", lv[l < 3 ? l : 0], name ? name : "", name ? " (" : "", rgb, name ? ")" : "");
    return 0;
}
