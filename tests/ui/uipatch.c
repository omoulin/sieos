/*
 * uipatch (make ui-test): a window of another program (mk/atlas.h) that
 * draws exact colour patches, to check that atlas shows a program's pixels
 * as it drew them in every look (no tint, no scanlines, no dimming).
 * Ten patches of 80 x 80 in two rows, from (20, 20), 100 apart, on a grey
 * 0x404040 ground; their colours are below. It logs "uipatch: drawn W x H"
 * after each frame. 'q' or the window's close ends it.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "mk/atlas.h"

static const uint32_t patch[10] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0x000000,
                                    0x808080, 0x123456, 0xfedcba, 0x4be3c1, 0x0a0f1c };
static long port, win;
static uint32_t *px;
static int w, h;
static size_t size;

static long call(msg_t *m) { long e = ipc_call(port, m); return e < 0 ? e : (long)m->w[0]; }   /* -> the reply's w[0] */
static void say(const char *what, long a, long b)     /* on the serial console too (the test reads it) */
{
    char line[96], *p = line;
    p += strlcpy(p, "uipatch: ", 16); p += strlcpy(p, what, 48);
    for (long v = a, k = 0; k < 2; v = b, k++) {
        char d[24]; int n = 0;
        if (v < 0) { *p++ = '-'; v = -v; }
        *p++ = ' ';
        do d[n++] = '0' + v % 10; while (v /= 10);
        while (n) *p++ = d[--n];
    }
    *p++ = '\n';
    sys_debug(line, p - line);
}

static void draw(void)
{
    for (int i = 0; i < w * h; i++) px[i] = 0x404040;
    for (int k = 0; k < 10; k++) {
        int x0 = 20 + k % 5 * 100, y0 = 20 + k / 5 * 100;
        for (int y = y0; y < y0 + 80 && y < h; y++)
            for (int x = x0; x < x0 + 80 && x < w; x++) px[y * w + x] = patch[k];
    }
    atlas_rect_t r = { 0, 0, w, h };
    msg_t m = { .w = { ATLAS_WIN_DAMAGE, (uint64_t)win }, .sbuf = &r, .slen = sizeof r };
    call(&m);
    say("drawn", w, h);
}

static void resize(int nw, int nh)
{
    uint64_t id;
    sys_random(&id, sizeof id);
    id &= ~(1ULL << 63);
    if (px) sys_munmap(px, size, 0);
    w = nw; h = nh; size = ((size_t)w * h * 4 + 4095) & ~(size_t)4095;
    if (sys_shm_create(id, size) < 0 || (long)(px = sys_shm_map(id, 0, PROT_READ | PROT_WRITE)) <= 0) { say("no memory", (long)px, 0); sys_exit(1); }
    msg_t m = { .w = { ATLAS_WIN_BUFFER, (uint64_t)win, id, (uint64_t)w | (uint64_t)h << 16 } };
    say("buffer", call(&m), 0);
    sys_shm_close(id);
    draw();
}

int main(int argc, char **argv)
{
    const char *t = "uipatch";
    (void)argc; (void)argv;
    port = port_lookup("atlas");
    msg_t m = { .w = { ATLAS_WIN_OPEN }, .sbuf = t, .slen = strlen(t) };
    win = call(&m);
    say("window", win, 0);
    if (win < 0) return 1;
    for (;;) {
        atlas_event_t ev[64];
        msg_t e = { .w = { ATLAS_WIN_EVENT, (uint64_t)win }, .rbuf = ev, .rlen = sizeof ev };
        long n = call(&e);
        if (n < 0) return 0;
        for (long i = 0; i < n; i++) {
            if (ev[i].type == ATW_SIZE) resize(ev[i].x, ev[i].y);
            else if (ev[i].type == ATW_KEY && ev[i].code == 'q') return 0;
            else if (ev[i].type == ATW_CLOSE) return 0;
        }
    }
}
