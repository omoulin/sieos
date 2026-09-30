/*
 * saver.c - The screen saver and the screen lock.
 *
 * After the idle time set in Settings (the "screensaver_timeout" setting,
 * in seconds: 300 by default, 0 never) the screen goes black and the
 * saver runs: the SIEOS logo in 3D (the Orbit Node: the ring, the stratum
 * bar and the node, with the SIEOS wordmark under it) turning in the middle
 * of the screen ("screensaver=logo", the default), or nothing ("blank").
 * A key or a move of the mouse then asks for the user's password
 * ("screensaver_lock=1", the default) before the desktop comes back; the
 * password is checked by ckpw (set-user-ID root: the shadow file is
 * root's), which checks only the password of whoever runs it.
 *
 * The logo is drawn by a small software renderer: triangle meshes (a
 * torus, a capsule, spheres, the letters as extruded blocks), rotated and
 * projected in perspective, filled with a depth buffer, lit per vertex
 * (ambient, diffuse and a specular highlight) and shaded smoothly.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "facet.h"
#include <math.h>
#include <sys/wait.h>

enum { SAVER_OFF, SAVER_RUN, SAVER_PROMPT };

static struct {
    int state;
    bool preview, lock_now;                      /* (a preview does not lock; "Lock Screen" locks at once) */
    char kind[16];                               /* "logo", "blank", "none" */
    int timeout;                                 /* seconds, 0: never */
    bool lock;
    long last_input, started, prompt_since;
    int last_abs_x, last_abs_y;
    char pass[64];
    char msg[64];
    /* the renderer's box */
    struct rect box;
    int k;                                       /* the box's pixels per rendered pixel (high-resolution screens) */
    float *zbuf;
    struct surface img;
    bool cleared;
} sv = { .kind = "logo", .timeout = 300, .lock = true, .last_abs_x = -1 };

void wm_present(const struct surface *s, struct rect r);   /* wm.c: a surface's rectangle to the screen */
void wm_redraw_all(void);                                  /* wm.c: the desktop again */

/* ---------------------------------------------------------------- the model */

struct vtx { float x, y, z, nx, ny, nz; color_t c; };
struct tri { int a, b, c; };

#define MAXV 12000
#define MAXT 20000
static struct vtx *mv;
static struct tri *mt;
static int nv, nt;

static int vadd(float x, float y, float z, float nx, float ny, float nz, color_t c)
{
    if (nv == MAXV)
        return nv - 1;
    mv[nv] = (struct vtx){ x, y, z, nx, ny, nz, c };
    return nv++;
}

static void tadd(int a, int b, int c)
{
    if (nt < MAXT)
        mt[nt++] = (struct tri){ a, b, c };
}

/* A grid of vertices (u around, v across) joined into quads. */
static void grid(int nu, int nvv, int base, bool wrap_u, bool wrap_v)
{
    int cu = wrap_u ? nu : nu - 1, cv = wrap_v ? nvv : nvv - 1;
    for (int i = 0; i < cu; i++)
        for (int j = 0; j < cv; j++) {
            int i1 = (i + 1) % nu, j1 = (j + 1) % nvv;
            int a = base + i * nvv + j, b = base + i1 * nvv + j, c = base + i1 * nvv + j1, d = base + i * nvv + j1;
            tadd(a, b, c);
            tadd(a, c, d);
        }
}

/* The ring (a torus in the XY plane), light blue, its lower right quarter darker. */
static void torus(float cx, float cy, float R, float r, int nu, int nvv, color_t light, color_t dark)
{
    int base = nv;
    for (int i = 0; i < nu; i++) {
        float u = 2 * (float)M_PI * i / nu, cu = cosf(u), su = sinf(u);
        for (int j = 0; j < nvv; j++) {
            float w = 2 * (float)M_PI * j / nvv, cw = cosf(w), sw = sinf(w);
            float nx = cu * cw, ny = su * cw, nz = sw;
            color_t c = (cu > 0 && su < 0) ? dark : light;
            vadd(cx + (R + r * cw) * cu, cy + (R + r * cw) * su, r * sw, nx, ny, nz, c);
        }
    }
    grid(nu, nvv, base, true, true);
}

static void sphere(float cx, float cy, float cz, float r, int nu, int nvv, color_t c)
{
    int base = nv;
    for (int i = 0; i < nu; i++) {
        float u = 2 * (float)M_PI * i / nu;
        for (int j = 0; j < nvv; j++) {
            float v = (float)M_PI * j / (nvv - 1), sv_ = sinf(v);
            float nx = cosf(u) * sv_, ny = cosf(v), nz = sinf(u) * sv_;
            vadd(cx + r * nx, cy + r * ny, cz + r * nz, nx, ny, nz, c);
        }
    }
    grid(nu, nvv, base, true, false);
}

/* The stratum bar: a cylinder along X with round ends (a capsule): rings from the left tip to the right one. */
static void capsule(float x0, float x1, float r, int nu, color_t c)
{
    const int H = 6;
    int base = nv, rings = 2 * (H + 1);
    for (int k = 0; k < rings; k++) {
        bool left = k <= H;
        float a = left ? (float)M_PI / 2 * (1 - (float)k / H) : (float)M_PI / 2 * (float)(k - H - 1) / H;
        float ax = left ? -sinf(a) : sinf(a), rad = cosf(a), cx = left ? x0 : x1;
        for (int i = 0; i < nu; i++) {
            float u = 2 * (float)M_PI * i / nu, cu = cosf(u), su = sinf(u);
            vadd(cx + r * ax, r * rad * cu, r * rad * su, ax, rad * cu, rad * su, c);
        }
    }
    for (int k = 0; k + 1 < rings; k++)
        for (int i = 0; i < nu; i++) {
            int i1 = (i + 1) % nu;
            int q0 = base + k * nu + i, q1 = base + k * nu + i1, q2 = base + (k + 1) * nu + i, q3 = base + (k + 1) * nu + i1;
            tadd(q0, q2, q3);
            tadd(q0, q3, q1);
        }
}

/* The wordmark: 5x7 letters, each lit cell a block (only the faces nothing covers). */
static const uint8_t glyphs[5][7] = {
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },   /* S */
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },   /* I */
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },   /* E */
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },   /* O */
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },   /* S */
};

static bool cell(int g, int col, int row)
{
    return g >= 0 && g < 5 && col >= 0 && col < 5 && row >= 0 && row < 7 && (glyphs[g][row] >> (4 - col) & 1);
}

static void quad(float p[4][3], float nx, float ny, float nz, color_t c)
{
    int a = vadd(p[0][0], p[0][1], p[0][2], nx, ny, nz, c), b = vadd(p[1][0], p[1][1], p[1][2], nx, ny, nz, c);
    int d = vadd(p[2][0], p[2][1], p[2][2], nx, ny, nz, c), e = vadd(p[3][0], p[3][1], p[3][2], nx, ny, nz, c);
    tadd(a, b, d);
    tadd(a, d, e);
}

static void wordmark(float y_top, float cellsz, float depth, color_t c)
{
    float width = (5 * 6 - 1) * cellsz, x0 = -width / 2, h = depth / 2;
    for (int g = 0; g < 5; g++)
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++) {
                if (!cell(g, col, row))
                    continue;
                float xa = x0 + (g * 6 + col) * cellsz, xb = xa + cellsz;
                float ya = y_top - row * cellsz, yb = ya - cellsz;
                float front[4][3] = { { xa, ya, h }, { xa, yb, h }, { xb, yb, h }, { xb, ya, h } };
                float back[4][3] = { { xb, ya, -h }, { xb, yb, -h }, { xa, yb, -h }, { xa, ya, -h } };
                quad(front, 0, 0, 1, c);
                quad(back, 0, 0, -1, c);
                if (!cell(g, col, row - 1)) {                                  /* top */
                    float q[4][3] = { { xa, ya, -h }, { xa, ya, h }, { xb, ya, h }, { xb, ya, -h } };
                    quad(q, 0, 1, 0, c);
                }
                if (!cell(g, col, row + 1)) {                                  /* bottom */
                    float q[4][3] = { { xa, yb, h }, { xa, yb, -h }, { xb, yb, -h }, { xb, yb, h } };
                    quad(q, 0, -1, 0, c);
                }
                if (!cell(g, col - 1, row)) {                                  /* left */
                    float q[4][3] = { { xa, ya, -h }, { xa, yb, -h }, { xa, yb, h }, { xa, ya, h } };
                    quad(q, -1, 0, 0, c);
                }
                if (!cell(g, col + 1, row)) {                                  /* right */
                    float q[4][3] = { { xb, ya, h }, { xb, yb, h }, { xb, yb, -h }, { xb, ya, -h } };
                    quad(q, 1, 0, 0, c);
                }
            }
}

/* The logo, in the console's proportions (a 200-unit box: the ring at 100,100 radius 62, the bar, the node at
 * 146,58), one unit = 1/100; the wordmark under it. */
static void build_model(void)
{
    if (mv)
        return;
    mv = malloc(sizeof(*mv) * MAXV);
    mt = malloc(sizeof(*mt) * MAXT);
    if (!mv || !mt)
        return;
    const float up = 0.35f;                      /* (the emblem above the wordmark) */
    torus(0, up, 0.62f, 0.09f, 96, 16, RGB(0x8F, 0xB4, 0xDC), RGB(0x4F, 0x7F, 0xB8));
    int b0 = nv;
    capsule(-0.64f, 0.64f, 0.07f, 20, RGB(0xE4, 0xE0, 0xD8));
    for (int i = b0; i < nv; i++)
        mv[i].y += up;
    sphere(0.46f, up + 0.42f, 0, 0.17f, 32, 16, RGB(0xD9, 0xA1, 0x5F));
    torus(0.46f, up + 0.42f, 0.27f, 0.015f, 48, 6, RGB(0x8A, 0x66, 0x3C), RGB(0x8A, 0x66, 0x3C));   /* the halo */
    wordmark(-0.55f, 0.075f, 0.14f, RGB(0xEE, 0xEE, 0xF2));
}

/* ---------------------------------------------------------------- rendering */

struct pv { float x, y, z, l; color_t c; };      /* projected: screen x, y, 1/z, light */

static void raster(const struct pv *a, const struct pv *b, const struct pv *c)
{
    struct surface *s = &sv.img;
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area > 0) {                              /* (either winding: the depth buffer sorts them out) */
        const struct pv *t = b;
        b = c, c = t;
        area = -area;
    }
    if (area > -0.01f)
        return;                                  /* (degenerate) */
    int x0 = (int)floorf(fminf(a->x, fminf(b->x, c->x))), x1 = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int y0 = (int)floorf(fminf(a->y, fminf(b->y, c->y))), y1 = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > s->w - 1 ? s->w - 1 : x1, y1 = y1 > s->h - 1 ? s->h - 1 : y1;
    float inv = 1 / area;
    for (int y = y0; y <= y1; y++) {
        float py = y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) * inv;
            float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) * inv;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            float z = w0 * a->z + w1 * b->z + w2 * c->z;   /* (1/z: larger is nearer) */
            float *zp = &sv.zbuf[y * s->w + x];
            if (z <= *zp)
                continue;
            *zp = z;
            float l = w0 * a->l + w1 * b->l + w2 * c->l;
            float rr = w0 * (a->c >> 16 & 255) + w1 * (b->c >> 16 & 255) + w2 * (c->c >> 16 & 255);
            float gg = w0 * (a->c >> 8 & 255) + w1 * (b->c >> 8 & 255) + w2 * (c->c >> 8 & 255);
            float bb = w0 * (a->c & 255) + w1 * (b->c & 255) + w2 * (c->c & 255);
            float spec = l > 1 ? (l - 1) * 255 : 0, k = l > 1 ? 1 : l;
            int R = (int)(rr * k + spec), G = (int)(gg * k + spec), B = (int)(bb * k + spec);
            s->px[y * s->stride + x] = RGB(R > 255 ? 255 : R, G > 255 ? 255 : G, B > 255 ? 255 : B);
        }
    }
}

static void render_logo(long t)
{
    struct surface *s = &sv.img;
    memset(s->px, 0, (size_t)s->stride * s->h * 4);
    for (int i = 0; i < s->w * s->h; i++)
        sv.zbuf[i] = 0;
    float yaw = t / 1000.0f * 0.7f, pitch = 0.22f * sinf(t / 1000.0f * 0.37f);   /* (a turn in 9 s, a slow nod) */
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    float scale = fminf(s->h / 3.1f, s->w / 3.4f), dist = 4.2f, f = scale * dist;
    /* the light: from the upper left, in front; the viewer along +z */
    float lx = -0.45f, ly = 0.6f, lz = 0.66f, hx = lx, hy = ly, hz = lz + 1;
    float hl = sqrtf(hx * hx + hy * hy + hz * hz);
    hx /= hl, hy /= hl, hz /= hl;
    static struct pv *pv;
    if (!pv && !(pv = malloc(sizeof(*pv) * MAXV)))
        return;
    for (int i = 0; i < nv; i++) {
        const struct vtx *v = &mv[i];
        /* yaw around Y, then pitch around X */
        float x = v->x * cy + v->z * sy, z = -v->x * sy + v->z * cy, y = v->y;
        float y2 = y * cp - z * sp, z2 = y * sp + z * cp;
        float nx = v->nx * cy + v->nz * sy, nz = -v->nx * sy + v->nz * cy, ny = v->ny;
        float ny2 = ny * cp - nz * sp, nz2 = ny * sp + nz * cp;
        float zz = dist - z2;
        pv[i].x = s->w / 2.0f + f * x / zz;
        pv[i].y = s->h / 2.0f - f * y2 / zz;
        pv[i].z = 1 / zz;
        float d = nx * lx + ny2 * ly + nz2 * lz, h = nx * hx + ny2 * hy + nz2 * hz;
        float l = 0.22f + 0.78f * (d > 0 ? d : 0);
        if (h > 0)
            l += powf(h, 40) * 0.55f;            /* (above 1: the highlight, towards white) */
        pv[i].l = l;
        pv[i].c = v->c;
    }
    for (int i = 0; i < nt; i++)
        raster(&pv[mt[i].a], &pv[mt[i].b], &pv[mt[i].c]);
}

/* The renderer's box: in the middle, 70% of the screen's height, a little wider; rendered at up to 720 lines
 * (each rendered pixel k x k screen pixels on larger screens: the frame rate stays). */
static bool box_setup(void)
{
    int h = screen_h * 7 / 10, w = h * 5 / 4;
    if (w > screen_w * 9 / 10)
        w = screen_w * 9 / 10;
    int k = (h + 719) / 720;
    w -= w % k, h -= h % k;
    if (sv.img.px && sv.k == k && sv.img.w == w / k && sv.img.h == h / k)
        return true;
    free(sv.img.px);
    free(sv.zbuf);
    sv.k = k;
    sv.img.w = sv.img.stride = w / k;
    sv.img.h = h / k;
    sv.img.px = malloc((size_t)sv.img.w * sv.img.h * 4);
    sv.zbuf = malloc((size_t)sv.img.w * sv.img.h * sizeof(float));
    sv.img.clip = rect_make(0, 0, sv.img.w, sv.img.h);
    sv.box = rect_make((screen_w - w) / 2, (screen_h - h) / 2 - screen_h / 20, w, h);
    return sv.img.px && sv.zbuf;
}

/* ---------------------------------------------------------------- the lock */

/* The password, checked by ckpw (its standard input). */
static bool password_ok(const char *pass)
{
    int p[2];
    if (pipe(p) < 0)
        return false;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[0], 0);
        close(p[1]);
        execl("/bin/ckpw", "ckpw", (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    if (pid < 0) {
        close(p[1]);
        return false;
    }
    dprintf(p[1], "%s\n", pass);
    close(p[1]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void draw_prompt(struct surface *s)
{
    int w = 420, h = 128, x = (screen_w - w) / 2, y = sv.box.y + sv.box.h + screen_h / 40;
    if (y + h > screen_h - 10)
        y = screen_h - 10 - h;
    gfx_set_clip(s, rect_make(0, 0, s->w, s->h));
    gfx_fill(s, x, y, w, h, RGB(0x1C, 0x1E, 0x24));
    gfx_frame(s, x, y, w, h, RGB(0x4F, 0x7F, 0xB8));
    char line[96];
    snprintf(line, sizeof(line), "This screen is locked by %s.", desktop_user);
    gfx_text_bold(s, x + 16, y + 14, line, RGB(0xEE, 0xEE, 0xF2));
    gfx_text(s, x + 16, y + 42, "Password", RGB(0xB0, 0xB4, 0xBC));
    int fx = x + 100, fy = y + 38, fw = w - 116, fh = 24;
    gfx_fill(s, fx, fy, fw, fh, RGB(0x0E, 0x0F, 0x12));
    gfx_frame(s, fx, fy, fw, fh, RGB(0x8F, 0xB4, 0xDC));
    char dots[64];
    size_t n = strlen(sv.pass);
    if (n > sizeof(dots) - 1)
        n = sizeof(dots) - 1;
    memset(dots, '*', n);
    dots[n] = 0;
    int tw = gfx_text(s, fx + 8, fy + 4, dots, RGB(0xEE, 0xEE, 0xF2));
    if ((uptime_ms() / 500) % 2)
        gfx_vline(s, fx + 9 + tw, fy + 4, FONT_H, RGB(0x8F, 0xB4, 0xDC));
    gfx_text(s, x + 16, y + 80, sv.msg[0] ? sv.msg : "Enter unlocks; Esc goes back to the screen saver.",
             sv.msg[0] ? RGB(0xE0, 0x80, 0x70) : RGB(0x80, 0x84, 0x8C));
}

/* ---------------------------------------------------------------- the desktop's side */

void saver_load_settings(void)
{
    char v[32];
    if (fct_setting_get("screensaver", v, sizeof(v)) && (!strcmp(v, "logo") || !strcmp(v, "blank") || !strcmp(v, "none")))
        snprintf(sv.kind, sizeof(sv.kind), "%s", v);
    if (fct_setting_get("screensaver_timeout", v, sizeof(v)) && atoi(v) >= 0)
        sv.timeout = atoi(v);
    if (fct_setting_get("screensaver_lock", v, sizeof(v)))
        sv.lock = atoi(v) != 0;
    sv.last_input = uptime_ms();
}

/* The desktop op "screensaver": change (any of saver, timeout, lock; -1 / NULL unchanged), then the settings. */
bool saver_configure(const char *kind, int timeout, int lock, char *msg, size_t n)
{
    if (kind && (!strcmp(kind, "logo") || !strcmp(kind, "blank") || !strcmp(kind, "none")))
        snprintf(sv.kind, sizeof(sv.kind), "%s", kind);
    if (timeout >= 0)
        sv.timeout = timeout;
    if (lock >= 0)
        sv.lock = lock != 0;
    char t[16];
    snprintf(t, sizeof(t), "%d", sv.timeout);
    bool ok = fct_setting_set("screensaver", sv.kind) && fct_setting_set("screensaver_timeout", t) &&
              fct_setting_set("screensaver_lock", sv.lock ? "1" : "0");
    snprintf(msg, n, "saver %s\ntimeout %d\nlock %d%s", sv.kind, sv.timeout, sv.lock,
             ok ? "" : "\n(not saved in ~/.facet/settings)");
    sv.last_input = uptime_ms();
    return true;
}

static void start(bool preview, bool lock_now)
{
    if (sv.state != SAVER_OFF)
        return;
    build_model();
    sv.state = SAVER_RUN;
    sv.preview = preview;
    sv.lock_now = lock_now;
    sv.started = uptime_ms();
    sv.cleared = false;
    sv.pass[0] = sv.msg[0] = 0;
}

void saver_preview(void) { start(true, false); }
void saver_lock_now(void) { start(false, true); }

bool saver_active(void)
{
    return sv.state != SAVER_OFF;
}

static void stop(void)
{
    sv.state = SAVER_OFF;
    memset(sv.pass, 0, sizeof(sv.pass));
    sv.last_input = uptime_ms();
    wm_redraw_all();
}

/* An input event: true if the saver took it (the desktop does not see it). */
bool saver_input(const struct input_event *ev)
{
    bool moved = ev->type == EV_WHEEL;
    if (ev->type == EV_MOUSE)
        moved = ev->dx || ev->dy || ev->buttons;
    else if (ev->type == EV_MOUSE_ABS) {
        moved = (sv.last_abs_x >= 0 && (ev->dx != sv.last_abs_x || ev->dy != sv.last_abs_y)) || ev->buttons;
        sv.last_abs_x = ev->dx, sv.last_abs_y = ev->dy;
    }
    bool key = ev->type == EV_KEY && ev->value;
    if (sv.state == SAVER_OFF) {
        if (moved || ev->type == EV_KEY)
            sv.last_input = uptime_ms();
        return false;
    }
    if (!moved && !key)
        return true;
    if (uptime_ms() - sv.started < 700)
        return true;                             /* (the click or key that started it) */
    bool locks = !sv.preview && (sv.lock || sv.lock_now);
    if (sv.state == SAVER_RUN) {
        if (!locks) {
            stop();
            return true;
        }
        sv.state = SAVER_PROMPT;
        sv.prompt_since = uptime_ms();
        sv.cleared = false;
        if (!key || !ev->ascii || ev->ascii < 32)
            return true;                         /* (a printable key starts the password) */
    }
    sv.prompt_since = uptime_ms();
    if (!key)
        return true;
    size_t n = strlen(sv.pass);
    if (ev->ascii == 27) {
        memset(sv.pass, 0, sizeof(sv.pass));
        sv.msg[0] = 0;
        sv.state = SAVER_RUN;
        sv.cleared = false;
    } else if (ev->ascii == '\b' || ev->code == 0x0E) {
        if (n)
            sv.pass[n - 1] = 0;
    } else if (ev->ascii == '\n' || ev->ascii == '\r') {
        if (password_ok(sv.pass)) {
            stop();
        } else {                                 /* (ckpw took a second already) */
            memset(sv.pass, 0, sizeof(sv.pass));
            snprintf(sv.msg, sizeof(sv.msg), "Wrong password. Try again.");
        }
    } else if (ev->ascii >= 32 && ev->ascii < 127 && n + 1 < sizeof(sv.pass)) {
        sv.pass[n] = ev->ascii;
        sv.pass[n + 1] = 0;
        sv.msg[0] = 0;
    }
    return true;
}

/* From the main loop: time to start? The next frame. The poll's timeout it wants (ms). */
int saver_step(struct surface *back)
{
    long now = uptime_ms();
    if (sv.state == SAVER_OFF) {
        if (sv.timeout > 0 && strcmp(sv.kind, "none") && now - sv.last_input >= sv.timeout * 1000L)
            start(false, false);
        if (sv.state == SAVER_OFF)
            return 100;
    }
    if (sv.state == SAVER_PROMPT && now - sv.prompt_since > 30000) {
        memset(sv.pass, 0, sizeof(sv.pass));     /* (nobody typed: the saver again) */
        sv.msg[0] = 0;
        sv.state = SAVER_RUN;
        sv.cleared = false;
    }
    if (!sv.cleared) {                           /* black, once */
        gfx_set_clip(back, rect_make(0, 0, screen_w, screen_h));
        gfx_fill(back, 0, 0, screen_w, screen_h, RGB(0, 0, 0));
        wm_present(back, rect_make(0, 0, screen_w, screen_h));
        sv.cleared = true;
    }
    bool logo = !strcmp(sv.kind, "logo") || sv.preview || sv.lock_now;
    if (logo && mv && box_setup()) {
        render_logo(now - sv.started);
        for (int y = 0; y < sv.box.h; y++) {
            uint32_t *dst = back->px + (size_t)(sv.box.y + y) * back->stride + sv.box.x;
            const uint32_t *src = sv.img.px + (size_t)(y / sv.k) * sv.img.stride;
            if (sv.k == 1)
                memcpy(dst, src, (size_t)sv.box.w * 4);
            else
                for (int x = 0; x < sv.box.w; x++)
                    dst[x] = src[x / sv.k];
        }
        wm_present(back, sv.box);
    }
    if (sv.state == SAVER_PROMPT) {
        draw_prompt(back);
        int y = sv.box.y + sv.box.h;
        wm_present(back, rect_make(0, y, screen_w, screen_h - y));
    }
    return logo ? 33 : 250;                      /* (about 30 frames a second) */
}
