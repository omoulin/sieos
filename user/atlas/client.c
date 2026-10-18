/*
 * client.c - Windows of other programs (mk/atlas.h): the program draws the
 * insides into shared memory, atlas frames them like its own windows and
 * passes on the input they get. Window ids W_CLIENT0 + 0..7. Its sizes and
 * places are the screen's pixels (the desktop's picture may be zoomed).
 *
 *   program                         atlas
 *   ATLAS_WIN_OPEN "title"  ───►    a window, placed, in front
 *   ATLAS_WIN_EVENT (waits) ◄───    ATW_SIZE w x h (its insides)
 *   ATLAS_WIN_BUFFER shm, w x h ─►  mapped; shown 1:1 (stretched until the
 *                                   program catches up with a new size)
 *   ATLAS_WIN_DAMAGE rects  ───►    redrawn, then answered
 *   ATLAS_WIN_EVENT (waits) ◄───    keys, the pointer, focus, size, close
 *
 * A program that ended is noticed when its window is drawn or gets input
 * (its process is asked for, SYS_KILL with no signal): the window goes.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"
#include "mk/atlas.h"

#define QMAX 64
typedef struct {
    int used, pid, uid;
    char title[64];
    uint64_t shm;                       /* the pixels: mapped at px, bw x bh */
    uint32_t *px; int bw, bh; size_t bsize;
    int cx0, cy0, cx1, cy1;             /* where the insides were drawn last (screen) */
    int sw, sh;                         /* the size last told (ATW_SIZE) */
    long waiter;                        /* its ATLAS_WIN_EVENT, waiting (0: none) */
    atlas_event_t q[QMAX]; int nq;      /* events not yet taken */
    int buttons, focus;
} client_t;
static client_t cl[ATLAS_WIN_MAX];

static client_t *of(int w) { return w >= W_CLIENT0 && w < W_CLIENT0 + ATLAS_WIN_MAX && cl[w - W_CLIENT0].used ? &cl[w - W_CLIENT0] : 0; }

static void unmap(client_t *c)
{
    if (c->px) sys_munmap(c->px, c->bsize, 0);
    c->px = 0; c->bw = c->bh = 0; c->bsize = 0;
}

static void flush(client_t *c)          /* the waiting call gets what is queued */
{
    if (!c->waiter || !c->nq) return;
    msg_t r = { .w = { (uint64_t)c->nq }, .sbuf = c->q, .slen = c->nq * sizeof *c->q };
    long t = c->waiter;
    c->waiter = 0; c->nq = 0;
    ipc_reply(t, &r);
}

static void post(client_t *c, int type, int code, int x, int y, int wheel)
{
    if (c->nq == QMAX) {                /* full: the oldest pointer motion goes */
        int i = 0;
        while (i < QMAX && !(c->q[i].type == ATW_POINTER && !c->q[i].wheel)) i++;
        if (i == QMAX) return;
        memmove(c->q + i, c->q + i + 1, (QMAX - i - 1) * sizeof *c->q);
        c->nq--;
    }
    c->q[c->nq++] = (atlas_event_t){ (uint16_t)type, (uint16_t)code, (int16_t)x, (int16_t)y, (uint8_t)c->buttons, (int8_t)wheel, 0 };
    flush(c);
}

static void gone(client_t *c)           /* the window is no more (its program ended, or it closed it) */
{
    int w = W_CLIENT0 + (int)(c - cl);
    if (c->waiter) reply_val(c->waiter, -EBADF);
    unmap(c);
    memset(c, 0, sizeof *c);
    atlas_client_gone(w);
}

static int alive(client_t *c)
{
    if (SYS(SYS_KILL, c->pid, 0) == -ESRCH) { say("atlas: window %d: its program ended\n", W_CLIENT0 + (int)(c - cl)); gone(c); return 0; }
    return 1;
}

int client_used(int w) { return of(w) != 0; }
void client_title(int w, char *t, size_t cap) { client_t *c = of(w); strlcpy(t, c ? c->title : "", cap); }
void client_info(int w, char *t, size_t cap)
{
    client_t *c = of(w);
    t[0] = 0;
    if (c && c->bw) { char *p = t; p += num(p, c->bw); p += strlcpy(p, " x ", 4); p[num(p, c->bh)] = 0; (void)cap; }
}

/* Its insides: the program's pixels, 1:1 at the top left (stretched while
 * a new size is on its way); a new size told when the window changed. */
void client_draw(int w, int x0, int y0, int x1, int y1, int focused)
{
    client_t *c = of(w);
    if (!c) return;                                   /* (an ended program is noticed at its next input: not while drawing) */
    (void)focused;
    c->cx0 = x0 + 2; c->cy0 = y0 + 48; c->cx1 = x1 - 2; c->cy1 = y1 - 2;
    int z = draw_zoom_get();                          /* (its pixels are the screen's: the picture is zoomed) */
    int iw = (int)(((int64_t)c->cx1 * z >> 16) - ((int64_t)c->cx0 * z >> 16)), ih = (int)(((int64_t)c->cy1 * z >> 16) - ((int64_t)c->cy0 * z >> 16));
    if (iw < 1 || ih < 1) return;
    if (iw != c->sw || ih != c->sh) { c->sw = iw; c->sh = ih; post(c, ATW_SIZE, atlas_zoom_get(), iw, ih, 0); }
    hitbox(c->cx0, c->cy0, c->cx1, c->cy1, H_CLIENT, w);
    if (!c->px) { dl_rect(c->cx0, c->cy0, c->cx1, c->cy1, 0, 0xffffff); return; }   /* (white until its first frame) */
    dl_client(c->cx0, c->cy0, c->cx1, c->cy1, c->px, c->bw, c->bh, 1);   /* (its own colours, whatever the look) */
}

void client_key(int w, int k) { client_t *c = of(w); if (c && alive(c)) post(c, ATW_KEY, k, 0, 0, 0); }
void client_focus(int w, int in)
{
    client_t *c = of(w);
    if (c && c->focus != in) { c->focus = in; post(c, ATW_FOCUS, in, 0, 0, 0); }
}

/* The pointer over its insides (x, y on the screen): moves, buttons, the
 * wheel. -> 1 if it was over them (or a button is still held from there). */
int client_pointer(int w, int x, int y, int buttons, int wheel)
{
    client_t *c = of(w);
    if (!c) return 0;
    int in = x >= c->cx0 && x < c->cx1 && y >= c->cy0 && y < c->cy1;
    if (!in && !c->buttons) return 0;
    if (!alive(c)) return 1;
    c->buttons = buttons;
    if (c->nq && c->q[c->nq - 1].type == ATW_POINTER && !c->q[c->nq - 1].wheel && !wheel &&
        c->q[c->nq - 1].buttons == buttons) {          /* (a motion not taken yet: moved on) */
        int z = draw_zoom_get();
        c->q[c->nq - 1].x = (int16_t)((int64_t)(x - c->cx0) * z >> 16); c->q[c->nq - 1].y = (int16_t)((int64_t)(y - c->cy0) * z >> 16);
        return 1;
    }
    int z = draw_zoom_get();
    post(c, ATW_POINTER, 0, (int)((int64_t)(x - c->cx0) * z >> 16), (int)((int64_t)(y - c->cy0) * z >> 16), wheel);
    return 1;
}

void client_closed(int w)               /* the user closed it */
{
    client_t *c = of(w);
    if (!c) return;
    post(c, ATW_CLOSE, 0, 0, 0, 0);     /* (told now if it waits; else its next call learns -EBADF) */
    unmap(c);
    memset(c, 0, sizeof *c);
}

void client_logout(void) { for (int i = 0; i < ATLAS_WIN_MAX; i++) if (cl[i].used) gone(&cl[i]); }

/* A message on port "atlas" (ATLAS_WIN_...). uid: the user logged in (-1: none). */
void client_msg(long from, msg_t *m, const char *data, int uid)
{
    long r = 0;
    if (uid < 0 || (m->uid != uid && m->uid != 0)) { reply_val(from, -EPERM); return; }
    if (m->w[0] == ATLAS_WIN_OPEN) {
        int i = 0;
        while (i < ATLAS_WIN_MAX && cl[i].used) i++;
        if (i == ATLAS_WIN_MAX) { reply_val(from, -EMFILE); return; }
        client_t *c = &cl[i];
        memset(c, 0, sizeof *c);
        c->used = 1; c->pid = m->pid; c->uid = m->uid;
        size_t n = m->rlen < sizeof c->title ? m->rlen : sizeof c->title - 1;
        memcpy(c->title, data, n); c->title[n] = 0;
        say("atlas: window %d opened by pid %d: %s\n", W_CLIENT0 + i, m->pid, c->title);
        reply_val(from, W_CLIENT0 + i);
        atlas_client_show(W_CLIENT0 + i);
        return;
    }
    if (m->w[0] == ATLAS_WIN_CLIP_GET) {                /* the clipboard (atlas.c's: a terminal's selection, or a program's) */
        msg_t r = { .w = { (uint64_t)clip_n }, .sbuf = clip, .slen = (uint64_t)clip_n };
        ipc_reply(from, &r);
        return;
    }
    if (m->w[0] == ATLAS_WIN_CLIP_SET) {
        clip_n = m->rlen < sizeof clip ? (int)m->rlen : (int)sizeof clip;
        memcpy(clip, data, (size_t)clip_n);
        say("atlas: copied %d bytes\n", clip_n);
        reply_val(from, 0);
        return;
    }
    client_t *c = of((int)m->w[1]);
    if (!c || c->pid != m->pid) { reply_val(from, -EBADF); return; }
    switch (m->w[0]) {
    case ATLAS_WIN_BUFFER: {
        int w = (int)(m->w[3] & 0xffff), h = (int)(m->w[3] >> 16 & 0xffff);
        size_t size = ((size_t)w * h * 4 + 4095) & ~(size_t)4095;
        void *p = w > 0 && h > 0 ? sys_shm_map(m->w[2], 0, PROT_READ) : 0;
        if (!p || (long)p < 0) { r = -EINVAL; break; }
        unmap(c);
        c->px = p; c->bw = w; c->bh = h; c->bsize = size; c->shm = m->w[2];
        break;
    }
    case ATLAS_WIN_DAMAGE: {
        const atlas_rect_t *a = (const atlas_rect_t *)data;
        int n = (int)(m->rlen / sizeof *a);
        int z = draw_zoom_get();                       /* (its rectangles: screen pixels; redraw_box: the picture's) */
        for (int i = 0; i < n && c->cx1 > c->cx0; i++) {
            int x0 = c->cx0 + (int)(((int64_t)a[i].x0 << 16) / z), y0 = c->cy0 + (int)(((int64_t)a[i].y0 << 16) / z);
            int x1 = c->cx0 + (int)((((int64_t)a[i].x1 << 16) + z - 1) / z) + 1, y1 = c->cy0 + (int)((((int64_t)a[i].y1 << 16) + z - 1) / z) + 1;
            if (x1 > c->cx1) x1 = c->cx1;
            if (y1 > c->cy1) y1 = c->cy1;
            if (x1 > x0 && y1 > y0 && atlas_client_visible(W_CLIENT0 + (int)(c - cl))) redraw_box(x0, y0, x1, y1);
        }
        break;
    }
    case ATLAS_WIN_EVENT:
        if (c->waiter) reply_val(c->waiter, 0);         /* (one waits: the older goes) */
        c->waiter = from;
        flush(c);
        return;
    case ATLAS_WIN_TITLE: {
        size_t n = m->rlen < sizeof c->title ? m->rlen : sizeof c->title - 1;
        memcpy(c->title, data, n); c->title[n] = 0;
        atlas_client_show(-1);                           /* (its tab redrawn) */
        break;
    }
    case ATLAS_WIN_CLOSE:
        reply_val(from, 0);
        gone(c);
        return;
    default: r = -ENOSYS;
    }
    reply_val(from, r);
}
