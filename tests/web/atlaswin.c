/*
 * atlaswin (make browser-test): a window on the desktop from a libposix
 * program (mk/atlas.h): opened, drawn (red on the left, blue on the right,
 * a white square where the pointer last pressed), damaged; its events
 * logged ("atlaswin: ..."). 'q' or the window's close ends it.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <sieos/sieos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include "mk/atlas.h"

static long win;
static uint32_t *px;
static int w, h;

static void draw(int mx, int my)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) px[y * w + x] = x < w / 2 ? 0xff0000 : 0x0000ff;
    for (int y = my - 10; y < my + 10; y++)
        for (int x = mx - 10; x < mx + 10; x++) if (x >= 0 && y >= 0 && x < w && y < h) px[y * w + x] = 0xffffff;
    atlas_rect_t r = { 0, 0, w, h };
    sieos_msg_t m = { .w = { ATLAS_WIN_DAMAGE, (uint64_t)win }, .sbuf = &r, .slen = sizeof r };
    sieos_call("atlas", &m);
}

static void resize(int nw, int nh)
{
    uint64_t id;
    getrandom(&id, sizeof id, 0);
    id &= ~(1ULL << 63);
    if (px) munmap(px, ((size_t)w * h * 4 + 4095) & ~(size_t)4095);
    w = nw; h = nh;
    if (sieos_shm_create(id, (size_t)w * h * 4) < 0 || !(px = sieos_shm_map(id))) { printf("atlaswin: no memory\n"); exit(1); }
    sieos_msg_t m = { .w = { ATLAS_WIN_BUFFER, (uint64_t)win, id, (uint64_t)w | (uint64_t)h << 16 } };
    long r = sieos_call("atlas", &m);
    sieos_shm_close(id);
    printf("atlaswin: buffer %d x %d: %ld\n", w, h, r);
    draw(-100, -100);
}

int main(void)
{
    const char *t = "atlas test";
    sieos_msg_t m = { .w = { ATLAS_WIN_OPEN }, .sbuf = t, .slen = strlen(t) };
    win = sieos_call("atlas", &m);
    printf("atlaswin: window %ld\n", win);
    if (win < 0) return 1;
    fflush(stdout);
    for (;;) {
        atlas_event_t ev[64];
        sieos_msg_t e = { .w = { ATLAS_WIN_EVENT, (uint64_t)win }, .rbuf = ev, .rlen = sizeof ev };
        long n = sieos_call("atlas", &e);
        if (n < 0) { printf("atlaswin: closed (%ld)\n", n); return 0; }
        for (long i = 0; i < n; i++) {
            atlas_event_t *v = &ev[i];
            if (v->type == ATW_SIZE) { printf("atlaswin: size %d x %d\n", v->x, v->y); resize(v->x, v->y); }
            else if (v->type == ATW_KEY) { printf("atlaswin: key %d\n", v->code); if (v->code == 'q') return 0; }
            else if (v->type == ATW_POINTER) {
                if (v->buttons || v->wheel) printf("atlaswin: pointer %d %d buttons %d wheel %d\n", v->x, v->y, v->buttons, v->wheel);
                if (v->buttons) draw(v->x, v->y);
            }
            else if (v->type == ATW_FOCUS) printf("atlaswin: focus %d\n", v->code);
            else if (v->type == ATW_CLOSE) { printf("atlaswin: closed by the user\n"); return 0; }
        }
        fflush(stdout);
    }
}
