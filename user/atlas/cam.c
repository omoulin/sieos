/*
 * cam.c - The Camera window (docs/desktop.md): the USB camera's live
 * picture (the USB server's port "camera", mk/proto.h), fitted to the
 * window; Snapshot saves it as ~/Pictures/camera-N.ppm (P6: a header, then
 * RGB bytes). The window also shows a .ppm opened from Files; Live goes
 * back to the camera.
 *
 * A thread of its own streams while the window is open: CAM_FRAME into
 * the back buffer, then ATLAS_CAM to atlas, which swaps the buffers and
 * draws the picture before answering (so the thread never writes into the
 * one shown). The window closed, the camera is closed (it powers down).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"

#define TOP 48                                /* the insides start this far below the window's top */
static long port;                             /* atlas's */
static volatile int live, on;                 /* the camera wanted (window open, not a file shown); on: streaming */
static volatile long tid;
static uint32_t *img[2];                      /* shown, being filled */
static int W, H, have, cap;                   /* the camera's size; have: img[0] holds a frame; cap: pixels allocated */
static uint32_t *pic;                         /* a .ppm shown instead, FW x FH */
static int FW, FH;
static char msg[96], file[64];                /* file: that .ppm's name, "" for the camera */
static int bx0, by0, bx1, by1;                /* where the picture was drawn */

static long notify(int w1) { msg_t m = { .w = { ATLAS_CAM, (uint64_t)w1 } }; return ipc_call(port, &m); }
static int room(int w, int h)                 /* both buffers hold w x h (have dropped first: not drawn meanwhile) */
{
    if ((long)w * h <= cap) return 0;
    if (have) { have = 0; notify(0); }
    free(img[0]); free(img[1]);
    img[0] = malloc((size_t)w * h * 4); img[1] = malloc((size_t)w * h * 4);
    cap = img[0] && img[1] ? w * h : 0;
    return cap ? 0 : -ENOMEM;
}

static void cam_thread(void *a)
{
    (void)a;
    long cp = -1;
    for (;;) {
        while (!live) sys_sleep(3600000000000L);
        msg_t m = { .w = { CAM_OPEN } };
        long e = (cp = port_find("camera")) < 0 ? -ENOENT : ipc_call(cp, &m) < 0 ? -EIO : (long)m.w[0];
        if (!e) e = room((int)m.w[1], (int)m.w[2]);
        if (!e) { W = (int)m.w[1]; H = (int)m.w[2]; on = 1; say("atlas: camera %d x %d\n", W, H); }
        else { strlcpy(msg, e == -ENOENT ? "No camera." : e == -EBUSY ? "The camera is in use." : "The camera does not start.", sizeof msg); say("atlas: camera: %s\n", msg); notify(0); }
        while (live && on) {
            m = (msg_t){ .w = { CAM_FRAME }, .rbuf = img[1], .rlen = (uint64_t)W * H * 4 };
            if (ipc_call(cp, &m) < 0 || (long)m.w[0] != (long)W * H * 4) { strlcpy(msg, "The camera stopped.", sizeof msg); on = 0; notify(0); break; }
            notify(1);
        }
        if (cp > 0) { m = (msg_t){ .w = { CAM_CLOSE } }; ipc_call(cp, &m); }
        on = 0;
        while (live) sys_sleep(3600000000000L);       /* (failed: until the window is opened again) */
    }
}

void cam_init(long atlas_port) { port = atlas_port; }
static void start(void)
{
    file[0] = 0; msg[0] = 0; have = 0; live = 1;
    if (!tid) tid = thread_start(cam_thread, 0, 8192);
    sys_wake(tid);
}
void cam_open(void) { if (!live) start(); }
void cam_close(void) { live = 0; file[0] = 0; free(pic); pic = 0; }

/* ATLAS_CAM: w1 = 1: a new frame (swapped in). -> 1: draw the picture again. */
int cam_tick(int w1)
{
    if (w1 && live) { uint32_t *t = img[0]; img[0] = img[1]; img[1] = t; have = 1; }
    return 1;
}
void cam_box(int *x0, int *y0, int *x1, int *y1) { *x0 = bx0; *y0 = by0; *x1 = bx1; *y1 = by1; }

/* A .ppm (P6, 255) from Files, in the window instead of the camera
 * (whose thread stops by itself). */
int cam_show(const char *path)
{
    live = 0; msg[0] = 0; file[0] = 0;
    free(pic); pic = 0;
    long h = fs_open(path, FS_RDONLY, 0);
    char hd[32];
    int v[3] = { 0 }, i = 2, n = h < 0 ? -1 : (int)fs_read(h, 0, hd, sizeof hd);
    for (int k = 0; n > 2 && hd[0] == 'P' && hd[1] == '6' && k < 3; k++) {   /* width, height, 255 */
        while (i < n && (hd[i] == ' ' || hd[i] == '\n')) i++;
        while (i < n && hd[i] >= '0' && hd[i] <= '9') v[k] = v[k] * 10 + hd[i++] - '0';
    }
    long np = (long)v[0] * v[1];
    if (v[2] != 255 || v[0] <= 0 || v[1] <= 0 || v[0] > 8192 || v[1] > 8192 || !(pic = malloc(np * 4))) {
        if (h >= 0) fs_close(h);
        strlcpy(msg, "Not a picture this window shows.", sizeof msg);
        return -EINVAL;
    }
    uint8_t *rgb = (uint8_t *)pic + np;                /* (read into the end, spread forward: in place) */
    long got = fs_read(h, i + 1, rgb, np * 3);
    fs_close(h);
    for (long p = 0; p < np; p++) pic[p] = 3 * p + 2 < got ? (uint32_t)rgb[3 * p] << 16 | rgb[3 * p + 1] << 8 | rgb[3 * p + 2] : 0;
    FW = v[0]; FH = v[1];
    strlcpy(file, base_name(path), sizeof file);
    say("atlas: camera window shows %s (%d x %d)\n", file, FW, FH);
    return 0;
}

/* Snapshot: the picture shown -> ~/Pictures/camera-N.ppm, the user's. */
static void snapshot(void)
{
    if (!have) return;
    char path[128], *p;
    int uid, gid;
    mk_groups_t g;
    atlas_user(&uid, &gid, &g);
    p = path + strlcpy(path, atlas_home(), sizeof path);
    strlcpy(p, "/Pictures", 10);
    if (!fs_mkdir(path, 0755)) fs_chown(path, uid, gid);
    long h = -1;
    for (int k = 1; k < 1000 && h < 0; k++) {           /* the first free number */
        p = path + strlen(atlas_home()) + 9;
        p += strlcpy(p, "/camera-", 9); p += num(p, k); strlcpy(p, ".ppm", 5);
        h = fs_open(path, FS_WRONLY | FS_CREAT | FS_EXCL, 0644);
        if (h < 0 && h != -EEXIST) break;
    }
    if (h < 0) { strlcpy(msg, "Could not save the picture.", sizeof msg); return; }
    static uint8_t b[3 * 4096 + 32];
    char *q = (char *)b + strlcpy((char *)b, "P6\n", 4);
    q += num(q, W); *q++ = ' '; q += num(q, H); q += strlcpy(q, "\n255\n", 6);
    long off = 0, r = fs_write(h, off, b, q - (char *)b);
    off = q - (char *)b;
    for (long i = 0, n; r >= 0 && i < (long)W * H; i += n) {
        n = (long)W * H - i < 4096 ? (long)W * H - i : 4096;
        for (long k = 0; k < n; k++) { uint32_t c = img[0][i + k]; b[3 * k] = c >> 16; b[3 * k + 1] = c >> 8; b[3 * k + 2] = c; }
        r = fs_write(h, off, b, 3 * n);
        off += 3 * n;
    }
    fs_close(h);
    fs_chown(path, uid, gid);
    strlcpy(msg, r < 0 ? "Could not save the picture." : base_name(path), sizeof msg);
    if (r >= 0) strlcpy(msg + strlen(msg), " saved in Pictures.", sizeof msg - strlen(msg));
    say("atlas: camera snapshot %s (%d x %d) -> %ld\n", path, W, H, r);
}

void cam_info(char *t, size_t cap_) { strlcpy(t, file[0] ? file : on ? "live" : "", cap_); }

void cam_draw(int x0, int y0, int x1, int y1, int focused)
{
    (void)focused;
    int Y0 = y0 + TOP;
    dl_rect(x0 + 2, Y0, x1 - 2, y1 - 2, 0, C_TERM);
    hitbox(x0, Y0, x1, y1, H_CAM, 0);
    int x = x0 + 14, ty = Y0 + 8;
    x = pill(x, ty, file[0] ? "Live" : "Snapshot", !file[0] && have, H_CAM, file[0] ? 2 : 1) + 20;
    if (atlas_debug) say("atlas: dbg cam %d at %d %d\n", file[0] ? 2 : 1, x0 + 60, ty + 21);
    const char *s = msg[0] ? msg : !have && live ? "Starting the camera..." : "";
    dl_text(x, ty + 10, s, strlen(s), SF(2), C_DIM, x, ty, x1 - 14, ty + 42);
    int ax0 = x0 + 14, ay0 = ty + 54, ax1 = x1 - 14, ay1 = y1 - 14;
    const uint32_t *px = file[0] ? pic : have ? img[0] : 0;
    int pw = file[0] ? FW : W, ph = file[0] ? FH : H;
    bx0 = bx1 = by0 = by1 = 0;
    if (!px || ax1 <= ax0 || ay1 <= ay0) return;
    int w = ax1 - ax0, h = (int)((long)w * ph / pw);     /* fitted, its shape kept, centred */
    if (h > ay1 - ay0) { h = ay1 - ay0; w = (int)((long)h * pw / ph); }
    bx0 = ax0 + (ax1 - ax0 - w) / 2; by0 = ay0 + (ay1 - ay0 - h) / 2; bx1 = bx0 + w; by1 = by0 + h;
    dl_image(bx0, by0, bx1, by1, px, pw, ph);
}

int cam_click(int kind, int arg)
{
    if (kind != H_CAM) return 0;
    if (arg == 1) snapshot();
    if (arg == 2) { free(pic); pic = 0; start(); }
    return 1;
}
