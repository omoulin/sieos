/*
 * facet-settings - Settings: the desktop's settings, one page per section
 * in a sidebar (Display: the screen's resolution; Appearance: the skin;
 * Pointer: the mouse's and touchpad's speed and acceleration; Screen saver:
 * which one, after how long, whether it locks; Assistant: the model sia
 * uses (endpoint, model, API key: ~/.sia/config); Network: the
 * interfaces' IPv4 settings, DHCP or static, through ifconfig; Wi-Fi: the
 * networks the Wi-Fi device hears, scanned again every 30 seconds).
 * Facet owns the display and the look, so changes go through the desktop
 * channel ({"op":"display"}, {"op":"skin"}); Facet applies them and keeps
 * them in ~/.facet/settings for the next session (facet/settings.h).
 * "facet-settings SECTION" opens on that section.  A Facet application
 * (libfacet).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "common.h"
#include <facet/settings.h>
#include <sys/wait.h>
#include "sieos/sysinfo.h"

#define SIDE_W 150
#define PAD 14
#define ROW 24
#define CARD_H 118
#define MAXMODES 32

struct page {
    const char *name, *title;
    int icon;
    void (*enter)(void);
    void (*draw)(struct surface *s, struct rect c);
    void (*click)(struct rect c, int x, int y);
    void (*move)(struct rect c, int x, int y);
    bool (*field_mouse)(struct rect c, int x, int y, int kind);   /* text fields: every mouse event first */
};

static int cur_page, side_hover = -1;
static char status[200];

/* ---------------- Display ---------------- */

static struct {
    char header[128];
    struct { int w, h; bool current, preferred; } mode[MAXMODES];
    int n, hover;
    bool fixed;
} dsp = { .hover = -1 };

static void display_enter(void)
{
    char r[4096];
    dsp.n = 0;
    dsp.fixed = false;
    if (!desktop_request("{\"op\":\"display\"}", r, sizeof(r))) {
        snprintf(dsp.header, sizeof(dsp.header), "%s", r[0] ? r : "no display information");
        return;
    }
    char *save, *line = strtok_r(r, "\n", &save);
    snprintf(dsp.header, sizeof(dsp.header), "%s", line ? line : "");
    char *fixed = strstr(dsp.header, ", its mode cannot be changed");
    if (fixed) {
        dsp.fixed = true;
        *fixed = 0;
    }
    while ((line = strtok_r(NULL, "\n", &save)) && dsp.n < MAXMODES) {
        int w, h;
        if (sscanf(line, "%dx%d", &w, &h) != 2)
            continue;
        dsp.mode[dsp.n].w = w;
        dsp.mode[dsp.n].h = h;
        dsp.mode[dsp.n].current = strstr(line, "(current)") != NULL;
        dsp.mode[dsp.n].preferred = strstr(line, "(preferred)") != NULL;
        dsp.n++;
    }
}

/* The modes in as many columns as the page needs. */
static struct rect mode_rect(struct rect c, int i)
{
    int rows = MAX(1, (c.h - 62) / ROW), cols = (dsp.n + rows - 1) / rows;
    if (cols < 2)
        return rect_make(c.x + PAD, c.y + 62 + i * ROW, c.w - 2 * PAD, ROW - 4);
    rows = (dsp.n + 1) / 2;                          /* (two even columns) */
    int cw = (c.w - 2 * PAD - 10) / 2;
    return rect_make(c.x + PAD + i / rows * (cw + 10), c.y + 62 + i % rows * ROW, cw, ROW - 4);
}

static void display_draw(struct surface *s, struct rect c)
{
    icon_draw(s, ICON_MONITOR, c.x + PAD, c.y + 10, 32);
    gfx_text_bold(s, c.x + PAD + 42, c.y + 10, "Screen resolution", C_TEXT);
    gfx_text(s, c.x + PAD + 42, c.y + 28, dsp.header, C_DIM);
    for (int i = 0; i < dsp.n; i++) {
        struct rect r = mode_rect(c, i);
        char label[64];
        snprintf(label, sizeof(label), "%d x %d%s", dsp.mode[i].w, dsp.mode[i].h,
                 dsp.mode[i].preferred ? "   (native)" : "");
        if (dsp.mode[i].current)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_SELECT);
        else if (i == dsp.hover && !dsp.fixed)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_CONTENT_ALT);
        gfx_text(s, r.x + 10, r.y + (r.h - FONT_H) / 2, label, C_TEXT);
        if (dsp.mode[i].current)
            gfx_text_bold(s, r.x + r.w - 10 - text_width_bold("current"), r.y + (r.h - FONT_H) / 2, "current", C_ACCENT);
    }
    if (!status[0])
        snprintf(status, sizeof(status), "%s", dsp.fixed ? "This display's mode is fixed (the firmware set it)."
                                                         : "Click a resolution to use it; it is kept for the next session.");
}

static void display_move(struct rect c, int x, int y)
{
    int hit = -1;
    for (int i = 0; i < dsp.n; i++)
        if (rect_contains(mode_rect(c, i), x, y))
            hit = i;
    dsp.hover = hit;
}

static void display_click(struct rect c, int x, int y)
{
    for (int i = 0; i < dsp.n; i++) {
        if (!rect_contains(mode_rect(c, i), x, y) || dsp.fixed || dsp.mode[i].current)
            continue;
        char req[96], r[256];
        snprintf(req, sizeof(req), "{\"op\":\"display\",\"width\":%d,\"height\":%d}", dsp.mode[i].w, dsp.mode[i].h);
        bool ok = desktop_request(req, r, sizeof(r));
        snprintf(status, sizeof(status), "%s%s", ok ? "" : "Could not change: ", r);
        display_enter();
        return;
    }
}

/* ---------------- Appearance ---------------- */

static struct rect card_rect(struct rect c, int i)
{
    return rect_make(c.x + PAD, c.y + 36 + i * (CARD_H + 10), c.w - 2 * PAD, CARD_H);
}

/* A miniature desktop in skin k: its background, a small window, some icons. */
static void preview(struct surface *s, struct rect r, const struct fct_skin *k)
{
    const struct fct_skin *cur = fct_skin;
    fct_skin_use(k);
    gfx_vgradient(s, r.x, r.y, r.w, r.h, C_DESK_TOP, C_DESK_BOT);
    struct rect w = rect_make(r.x + 12, r.y + 14, r.w - 24, r.h - 22);
    if (k->id == FCT_SKIN_BEOS) {                   /* a yellow tab over a grey frame */
        gfx_fill(s, w.x, w.y, 80, 14, RGB(0x40, 0x40, 0x40));
        gfx_vgradient(s, w.x + 1, w.y + 1, 78, 13, color_shade(C_TITLEBAR, 40), C_TITLEBAR);
        gfx_fill(s, w.x, w.y + 14, w.w, w.h - 14, C_FACE);
        gfx_frame(s, w.x, w.y + 14, w.w, w.h - 14, RGB(0x40, 0x40, 0x40));
        gfx_fill(s, w.x + 4, w.y + 18, w.w - 8, w.h - 22, C_CONTENT);
    } else if (k->id == FCT_SKIN_IRIX) {            /* a thick steel-blue bevelled frame */
        gfx_fill(s, w.x, w.y, w.w, w.h, C_TITLEBAR);
        gfx_bevel(s, w.x, w.y, w.w, w.h, 2, true, color_shade(C_TITLEBAR, 70), color_shade(C_TITLEBAR, -60));
        gfx_fill(s, w.x + 5, w.y + 18, w.w - 10, w.h - 23, C_CONTENT);
    } else {                                        /* Strata: a dark rounded frame */
        gfx_round_rect_top(s, w.x, w.y, w.w, 16, 4, C_TITLEBAR);
        gfx_fill(s, w.x, w.y + 16, w.w, w.h - 16, C_CONTENT);
        gfx_frame(s, w.x, w.y, w.w, w.h, C_FACE_DARK);
        gfx_fill(s, w.x + 1, w.y + 16, 3, w.h - 17, C_ACCENT);
    }
    static const int icons[] = { ICON_FOLDER, ICON_TERMINAL, ICON_MONITOR, ICON_NETWORK, ICON_CLOCK };
    for (int i = 0; i < 5; i++)
        icon_draw(s, icons[i], w.x + 12 + i * 40, w.y + w.h - 42, 32);
    fct_skin_use(cur);
}

static void appearance_draw(struct surface *s, struct rect c)
{
    gfx_text_bold(s, c.x + PAD, c.y + 12, "Desktop skin", C_TEXT);
    for (int i = 0; i < FCT_NSKINS; i++) {
        const struct fct_skin *k = fct_skin_at(i);
        struct rect r = card_rect(c, i);
        bool cur = k == fct_skin;
        gfx_fill(s, r.x, r.y, r.w, r.h, cur ? C_SELECT : C_CONTENT);
        gfx_frame(s, r.x, r.y, r.w, r.h, cur ? C_ACCENT : C_LINE);
        struct rect pv = rect_make(r.x + 8, r.y + 8, 240, r.h - 16);
        preview(s, pv, k);
        gfx_frame(s, pv.x, pv.y, pv.w, pv.h, C_FACE_DARK);
        int tx = pv.x + pv.w + 14, tw = r.x + r.w - 10 - tx;
        gfx_text_bold(s, tx, r.y + 12, k->title, C_TEXT);
        if (cur)
            gfx_text_bold(s, r.x + r.w - 10 - text_width_bold("current"), r.y + 12, "current", C_ACCENT);
        const char *p = k->blurb;                   /* the description, wrapped */
        int y = r.y + 36;
        while (*p && y < r.y + r.h - FONT_H) {
            size_t n = text_fit(p, tw);
            if (p[n]) {
                size_t sp = n;
                while (sp > 0 && p[sp] != ' ')
                    sp--;
                if (sp > 0)
                    n = sp;
            }
            char line[128];
            snprintf(line, sizeof(line), "%.*s", (int)MIN(n, sizeof(line) - 1), p);
            gfx_text(s, tx, y, line, C_DIM);
            p += n;
            while (*p == ' ')
                p++;
            y += FONT_H + 2;
        }
    }
    if (!status[0])
        snprintf(status, sizeof(status), "Click a skin to use it; it is kept for the next session.");
}

static void appearance_click(struct rect c, int x, int y)
{
    for (int i = 0; i < FCT_NSKINS; i++) {
        if (!rect_contains(card_rect(c, i), x, y) || fct_skin_at(i) == fct_skin)
            continue;
        char req[64], r[256];
        snprintf(req, sizeof(req), "{\"op\":\"skin\",\"name\":\"%s\"}", fct_skin_at(i)->name);
        bool ok = desktop_request(req, r, sizeof(r));
        snprintf(status, sizeof(status), "%s%s", ok ? "" : "Could not change: ", r);
        if (ok)
            fct_skin_use(fct_skin_at(i));           /* (the desktop tells us too) */
    }
}

/* ---------------- Pointer ---------------- */

static struct { int speed, accel; } ptr = { 5, 1 };

static void pointer_parse(const char *r)
{
    const char *s = strstr(r, "speed "), *a = strstr(r, "accel ");
    if (s)
        ptr.speed = atoi(s + 6);
    if (a)
        ptr.accel = atoi(a + 6);
}

static void pointer_enter(void)
{
    char r[128];
    if (desktop_request("{\"op\":\"pointer\"}", r, sizeof(r)))
        pointer_parse(r);
}

static struct rect speed_box(struct rect c, int i)      /* 1..10 */
{
    int w = (c.w - 2 * PAD - 9 * 6) / 10;
    return rect_make(c.x + PAD + (i - 1) * (w + 6), c.y + 76, w, 30);
}

static struct rect accel_box(struct rect c) { return rect_make(c.x + PAD, c.y + 150, 18, 18); }

static void pointer_draw(struct surface *s, struct rect c)
{
    icon_draw(s, ICON_PROGRAM, c.x + PAD, c.y + 10, 32);
    gfx_text_bold(s, c.x + PAD + 42, c.y + 10, "Mouse and touchpad", C_TEXT);
    gfx_text(s, c.x + PAD + 42, c.y + 28, "How far the pointer goes for a move of the mouse or the finger", C_DIM);
    gfx_text(s, c.x + PAD, c.y + 58, "Speed", C_TEXT);
    for (int i = 1; i <= 10; i++) {
        struct rect r = speed_box(c, i);
        bool cur = i == ptr.speed;
        gfx_fill(s, r.x, r.y, r.w, r.h, cur ? C_SELECT : i <= ptr.speed ? C_CONTENT_ALT : C_CONTENT);
        gfx_frame(s, r.x, r.y, r.w, r.h, cur ? C_ACCENT : C_LINE);
        char n[4];
        snprintf(n, sizeof(n), "%d", i);
        gfx_text(s, r.x + (r.w - text_width(n)) / 2, r.y + (r.h - FONT_H) / 2, n, cur ? C_ACCENT : C_TEXT);
    }
    gfx_text(s, c.x + PAD, c.y + 112, "slower", C_DIM);
    gfx_text(s, c.x + c.w - PAD - text_width("faster"), c.y + 112, "faster", C_DIM);
    struct rect b = accel_box(c);
    gfx_fill(s, b.x, b.y, b.w, b.h, C_CONTENT);
    gfx_frame(s, b.x, b.y, b.w, b.h, C_LINE);
    if (ptr.accel) {
        gfx_thick_line(s, b.x + 4, b.y + 9, b.x + 8, b.y + 13, 2, C_ACCENT);
        gfx_thick_line(s, b.x + 8, b.y + 13, b.x + 14, b.y + 4, 2, C_ACCENT);
    }
    gfx_text(s, b.x + 28, b.y + 1, "Acceleration: fast moves go further", C_TEXT);
    if (!status[0])
        snprintf(status, sizeof(status), "Changes apply at once and are kept for the next session.");
}

static void pointer_set(int speed, int accel)
{
    char req[96], r[128];
    snprintf(req, sizeof(req), "{\"op\":\"pointer\",\"speed\":%d,\"accel\":%d}", speed, accel);
    if (desktop_request(req, r, sizeof(r)))
        pointer_parse(r);
    else
        snprintf(status, sizeof(status), "Could not change: %s", r);
}

static void pointer_click(struct rect c, int x, int y)
{
    for (int i = 1; i <= 10; i++)
        if (rect_contains(speed_box(c, i), x, y))
            pointer_set(i, -1);
    struct rect b = accel_box(c);
    if (rect_contains(rect_make(b.x, b.y, 320, b.h), x, y))
        pointer_set(-1, !ptr.accel);
}

/* ---------------- Screen saver ---------------- */

static struct { char kind[16]; int timeout, lock; } ss = { "logo", 300, 1 };
static const struct { const char *kind, *label; } ss_kinds[] = {
    { "logo", "SIEOS logo (3D)" }, { "blank", "Blank screen" }, { "none", "None" } };
static const int ss_minutes[] = { 1, 2, 5, 10, 15, 30, 60 };
#define NSSK 3
#define NSSM 7

static void ss_parse(const char *r)
{
    const char *k = strstr(r, "saver "), *t = strstr(r, "timeout "), *l = strstr(r, "lock ");
    if (k)
        sscanf(k + 6, "%15s", ss.kind);
    if (t)
        ss.timeout = atoi(t + 8);
    if (l)
        ss.lock = atoi(l + 5);
}

static void ss_request(const char *req)
{
    char r[160];
    if (desktop_request(req, r, sizeof(r)))
        ss_parse(r);
    else
        snprintf(status, sizeof(status), "Could not change: %s", r);
}

static void saver_enter(void)
{
    ss_request("{\"op\":\"screensaver\"}");
}

static struct rect kind_box(struct rect c, int i) { return rect_make(c.x + PAD + i * 176, c.y + 78, 170, 28); }
static struct rect min_box(struct rect c, int i) { return rect_make(c.x + PAD + i * 62, c.y + 144, 56, 28); }
static struct rect lock_box(struct rect c) { return rect_make(c.x + PAD, c.y + 196, 18, 18); }
static struct rect preview_btn(struct rect c) { return rect_make(c.x + PAD, c.y + 240, 110, 24); }
static struct rect locknow_btn(struct rect c) { return rect_make(c.x + PAD + 120, c.y + 240, 110, 24); }

static void saver_draw(struct surface *s, struct rect c)
{
    icon_draw(s, ICON_MONITOR, c.x + PAD, c.y + 10, 32);
    gfx_text_bold(s, c.x + PAD + 42, c.y + 10, "Screen saver", C_TEXT);
    gfx_text(s, c.x + PAD + 42, c.y + 28, "What the screen shows while you are away, and the lock", C_DIM);
    gfx_text(s, c.x + PAD, c.y + 60, "Screen saver", C_TEXT);
    for (int i = 0; i < NSSK; i++) {
        struct rect r = kind_box(c, i);
        bool cur = !strcmp(ss.kind, ss_kinds[i].kind);
        gfx_fill(s, r.x, r.y, r.w, r.h, cur ? C_SELECT : C_CONTENT);
        gfx_frame(s, r.x, r.y, r.w, r.h, cur ? C_ACCENT : C_LINE);
        gfx_text(s, r.x + (r.w - text_width(ss_kinds[i].label)) / 2, r.y + (r.h - FONT_H) / 2, ss_kinds[i].label,
                 cur ? C_ACCENT : C_TEXT);
    }
    bool none = !strcmp(ss.kind, "none");
    gfx_text(s, c.x + PAD, c.y + 126, "Start after (minutes without a key or a move of the mouse)", none ? C_DIM : C_TEXT);
    for (int i = 0; i < NSSM; i++) {
        struct rect r = min_box(c, i);
        bool cur = !none && ss.timeout == ss_minutes[i] * 60;
        gfx_fill(s, r.x, r.y, r.w, r.h, cur ? C_SELECT : none ? C_FACE : C_CONTENT);
        gfx_frame(s, r.x, r.y, r.w, r.h, cur ? C_ACCENT : C_LINE);
        char n[8];
        snprintf(n, sizeof(n), "%d", ss_minutes[i]);
        gfx_text(s, r.x + (r.w - text_width(n)) / 2, r.y + (r.h - FONT_H) / 2, n, cur ? C_ACCENT : none ? C_DIM : C_TEXT);
    }
    struct rect b = lock_box(c);
    gfx_fill(s, b.x, b.y, b.w, b.h, C_CONTENT);
    gfx_frame(s, b.x, b.y, b.w, b.h, C_LINE);
    if (ss.lock) {
        gfx_thick_line(s, b.x + 4, b.y + 9, b.x + 8, b.y + 13, 2, C_ACCENT);
        gfx_thick_line(s, b.x + 8, b.y + 13, b.x + 14, b.y + 4, 2, C_ACCENT);
    }
    gfx_text(s, b.x + 28, b.y + 1, "Ask for my password to come back", C_TEXT);
    ui_button(s, preview_btn(c), "Preview", false);
    ui_button(s, locknow_btn(c), "Lock now", false);
    if (!status[0])
        snprintf(status, sizeof(status), "Changes apply at once and are kept for the next session.");
}

static void saver_click(struct rect c, int x, int y)
{
    char req[128];
    for (int i = 0; i < NSSK; i++)
        if (rect_contains(kind_box(c, i), x, y)) {
            snprintf(req, sizeof(req), "{\"op\":\"screensaver\",\"saver\":\"%s\"}", ss_kinds[i].kind);
            ss_request(req);
        }
    for (int i = 0; i < NSSM && strcmp(ss.kind, "none"); i++)
        if (rect_contains(min_box(c, i), x, y)) {
            snprintf(req, sizeof(req), "{\"op\":\"screensaver\",\"timeout\":%d}", ss_minutes[i] * 60);
            ss_request(req);
        }
    struct rect b = lock_box(c);
    if (rect_contains(rect_make(b.x, b.y, 300, b.h), x, y)) {
        snprintf(req, sizeof(req), "{\"op\":\"screensaver\",\"lock\":%d}", !ss.lock);
        ss_request(req);
    }
    if (rect_contains(preview_btn(c), x, y))
        ss_request("{\"op\":\"screensaver\",\"preview\":1}");
    if (rect_contains(locknow_btn(c), x, y))
        ss_request("{\"op\":\"screensaver\",\"lock_now\":1}");
}

/* ---------------- Assistant (sia's model: ~/.sia/config) ---------------- */

enum { A_ENDPOINT, A_MODEL, A_KEY, NAFIELDS };
static struct {
    struct fct_field fld[NAFIELDS];             /* the key's is masked */
    bool have_key;                               /* a key is kept (not shown: typing one replaces it) */
    char old_key[512];
    bool auto_approve;
    int focus;
} ai = { .focus = -1 };

static const char *sia_path(void)
{
    static char p[300];
    const char *home = getenv("HOME");
    snprintf(p, sizeof(p), "%s/.sia/config", home && *home ? home : "");
    return p;
}

static void assistant_enter(void)
{
    memset(&ai, 0, sizeof(ai));
    ai.focus = -1;
    ai.fld[A_KEY].masked = true;
    FILE *f = fopen(sia_path(), "r");
    char line[1100];
    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq)
            continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(line, "endpoint"))
            fct_field_set(&ai.fld[A_ENDPOINT], v);
        else if (!strcmp(line, "model"))
            fct_field_set(&ai.fld[A_MODEL], v);
        else if (!strcmp(line, "api_key"))
            snprintf(ai.old_key, sizeof(ai.old_key), "%s", v), ai.have_key = v[0] != 0;
        else if (!strcmp(line, "auto_approve"))
            ai.auto_approve = !strcmp(v, "yes");
    }
    if (f)
        fclose(f);
}

static struct rect ai_field(struct rect c, int i) { return rect_make(c.x + PAD + 100, c.y + 64 + i * 34, c.w - 2 * PAD - 100, 24); }
static struct rect ai_save_btn(struct rect c) { return rect_make(c.x + c.w - PAD - 110, c.y + 200, 110, 24); }

static void assistant_draw(struct surface *s, struct rect c)
{
    icon_draw(s, ICON_TERMINAL, c.x + PAD, c.y + 10, 32);
    gfx_text_bold(s, c.x + PAD + 42, c.y + 10, "Assistant", C_TEXT);
    gfx_text(s, c.x + PAD + 42, c.y + 28, "The model the sia assistant talks to (an OpenAI-compatible endpoint)", C_DIM);
    static const char *const labels[] = { "Endpoint", "Model", "API key" };
    for (int i = 0; i < NAFIELDS; i++) {
        struct rect r = ai_field(c, i);
        gfx_text(s, c.x + PAD, r.y + 4, labels[i], C_TEXT);
        fct_field_draw(s, r, &ai.fld[i], i == ai.focus,
                       i == A_KEY && ai.have_key ? "(kept: type a new one to replace it)" : NULL);
    }
    gfx_text(s, c.x + PAD, c.y + 172, "Example: https://NAME.openai.azure.com, a deployment name, its key.", C_DIM);
    ui_button(s, ai_save_btn(c), "Save", false);
    if (!status[0])
        snprintf(status, sizeof(status), "Kept in %s (readable by you only); new sia sessions use it.", sia_path());
}

static void assistant_save(void)
{
    for (int i = 0; i < NAFIELDS; i++) {          /* (none of the three has spaces) */
        char *t = ai.fld[i].text, *b = t;
        while (*b == ' ')
            b++;
        memmove(t, b, strlen(b) + 1);
        size_t l = strlen(t);
        while (l && t[l - 1] == ' ')
            t[--l] = 0;
    }
    const char *ep = ai.fld[A_ENDPOINT].text, *model = ai.fld[A_MODEL].text;
    const char *key = ai.fld[A_KEY].text[0] ? ai.fld[A_KEY].text : ai.old_key;
    if (!ep[0] || !model[0] || !key[0]) {
        snprintf(status, sizeof(status), "The endpoint, the model and the key are all needed.");
        return;
    }
    char endpoint[520];
    snprintf(endpoint, sizeof(endpoint), "%s%s", strncmp(ep, "http://", 7) && strncmp(ep, "https://", 8) ? "https://" : "", ep);
    char dir[300];
    snprintf(dir, sizeof(dir), "%s", sia_path());
    *strrchr(dir, '/') = 0;
    mkdir(dir, 0700);
    chmod(dir, 0700);
    int fd = open(sia_path(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        snprintf(status, sizeof(status), "Could not write %s: %s", sia_path(), strerror(errno));
        return;
    }
    fchmod(fd, 0600);
    dprintf(fd, "# sia model connection. Keep this file private.\n");
    dprintf(fd, "endpoint=%s\nmodel=%s\napi_key=%s\nauto_approve=%s\n", endpoint, model, key,
            ai.auto_approve ? "yes" : "no");
    close(fd);
    snprintf(status, sizeof(status), "Saved. Open a new sia terminal to use it.");
    assistant_enter();
}

static void assistant_click(struct rect c, int x, int y)
{
    ai.focus = -1;
    if (rect_contains(ai_save_btn(c), x, y))
        assistant_save();
}

static bool assistant_field_mouse(struct rect c, int x, int y, int kind)
{
    for (int i = 0; i < NAFIELDS; i++)
        if ((kind != FCT_MOUSE_MOVE || ai.fld[i].dragging) && fct_field_mouse(&ai.fld[i], ai_field(c, i), x, y, kind)) {
            ai.focus = i;
            return true;
        }
    return false;
}

static bool assistant_key(const struct fct_key *k)
{
    if (ai.focus < 0 || !k->value)
        return false;
    if (k->ascii == '\t') {
        ai.focus = (ai.focus + 1) % NAFIELDS;
    } else if (k->ascii == '\n' || k->ascii == '\r') {
        ai.focus = -1;
        assistant_save();
    } else {
        fct_field_key(&ai.fld[ai.focus], k);
    }
    return true;
}

/* ---------------- Network ---------------- */

enum { F_IP, F_MASK, F_GW, F_DNS, F_PASS, NFIELDS };
static struct {
    struct netinfo ni[4];
    int n, cur;
    bool dhcp;
    struct fct_field nf[NFIELDS];               /* the password's is masked */
    int focus;                                   /* the field typed into, -1 none */
    char wifi[96];
} net = { .focus = -1 };

static void net_load_fields(void)
{
    struct netinfo *ni = &net.ni[net.cur];
    net.dhcp = ni->dhcp || !ni->up;
    { char t_[40]; ip_to_str(ni->ip, t_); fct_field_set(&net.nf[F_IP], t_); }
    { char t_[40]; ip_to_str(ni->netmask, t_); fct_field_set(&net.nf[F_MASK], t_); }
    { char t_[40]; ip_to_str(ni->gateway, t_); fct_field_set(&net.nf[F_GW], t_); }
    { char t_[40]; ip_to_str(ni->dns, t_); fct_field_set(&net.nf[F_DNS], t_); }
    fct_field_set(&net.nf[F_PASS], "");
    net.nf[F_PASS].masked = true;
}

static void network_enter(void)
{
    net.n = 0;
    while (net.n < 4 && netinfo_if(&net.ni[net.n], net.n) == 0)
        net.n++;
    if (net.cur >= net.n)
        net.cur = 0;
    if (net.n)
        net_load_fields();
    net.focus = -1;
    /* Wi-Fi: the adapters there are (devinfo), and what can be done with them */
    net.wifi[0] = 0;
    struct sieos_devinfo di;
    for (int i = 0; devinfo(&di, i) == 0; i++)
        if (di.class_code == 0x02 && di.subclass == 0x80) {        /* a network controller: other (Wi-Fi) */
            bool ax201 = di.vendor == 0x8086 && (di.device == 0x34F0 || di.device == 0x02F0 || di.device == 0xA0F0 ||
                                                  di.device == 0x43F0);
            snprintf(net.wifi, sizeof(net.wifi), "%s (%04x:%04x)%s%s", ax201 ? "Intel Wi-Fi 6 AX201" : "wireless adapter",
                     di.vendor, di.device, di.driver[0] ? ", driver " : ": no driver yet", di.driver);
            break;
        }
}

static struct rect if_tab(struct rect c, int i) { return rect_make(c.x + PAD + i * 96, c.y + 52, 90, 24); }
static struct rect mode_btn(struct rect c, int i) { return rect_make(c.x + PAD + i * 150, c.y + 136, 144, 24); }
static struct rect field_rect(struct rect c, int i)
{
    return i == F_PASS ? rect_make(c.x + PAD + 130, c.y + 290, 200, 22) : rect_make(c.x + PAD + 130, c.y + 170 + i * 28, 200, 22);
}
static struct rect apply_btn(struct rect c) { return rect_make(c.x + c.w - PAD - 110, c.y + 290, 110, 24); }

static void network_draw(struct surface *s, struct rect c)
{
    icon_draw(s, ICON_NETWORK, c.x + PAD, c.y + 10, 32);
    gfx_text_bold(s, c.x + PAD + 42, c.y + 10, "Network", C_TEXT);
    gfx_text(s, c.x + PAD + 42, c.y + 28, "Wired and USB Ethernet interfaces", C_DIM);
    if (!net.n) {
        gfx_text(s, c.x + PAD, c.y + 60, "No network interface.", C_TEXT);
    } else {
        for (int i = 0; i < net.n; i++) {
            struct rect r = if_tab(c, i);
            gfx_fill(s, r.x, r.y, r.w, r.h, i == net.cur ? C_SELECT : C_CONTENT);
            gfx_frame(s, r.x, r.y, r.w, r.h, i == net.cur ? C_ACCENT : C_LINE);
            gfx_text(s, r.x + 8, r.y + 4, net.ni[i].name, C_TEXT);
        }
        struct netinfo *ni = &net.ni[net.cur];
        char line[160], a[16], m[16], g[16];
        snprintf(line, sizeof(line), "%s  %02x:%02x:%02x:%02x:%02x:%02x   now %s/%s gw %s (%s)", ni->driver, ni->mac[0],
                 ni->mac[1], ni->mac[2], ni->mac[3], ni->mac[4], ni->mac[5], ip_to_str(ni->ip, a),
                 ip_to_str(ni->netmask, m), ip_to_str(ni->gateway, g), !ni->up ? "no address" : ni->dhcp ? "DHCP" : "static");
        gfx_text(s, c.x + PAD, c.y + 86, line, C_DIM);
        gfx_text(s, c.x + PAD, c.y + 116, "IPv4 settings", C_TEXT);
        static const char *const modes[] = { "Automatic (DHCP)", "Static address" };
        for (int i = 0; i < 2; i++) {
            struct rect r = mode_btn(c, i);
            bool on = (i == 0) == net.dhcp;
            gfx_fill(s, r.x, r.y, r.w, r.h, on ? C_SELECT : C_CONTENT);
            gfx_frame(s, r.x, r.y, r.w, r.h, on ? C_ACCENT : C_LINE);
            gfx_text(s, r.x + 8, r.y + 4, modes[i], C_TEXT);
        }
        static const char *const labels[] = { "Address", "Netmask", "Gateway", "DNS server", "Root password" };
        for (int i = 0; i < NFIELDS; i++) {
            if (i == F_PASS && geteuid() == 0)
                continue;
            struct rect r = field_rect(c, i);
            bool off = i != F_PASS && net.dhcp;
            gfx_text(s, c.x + PAD, r.y + 3, labels[i], off ? C_DIM : C_TEXT);
            if (off) {
                gfx_fill(s, r.x, r.y, r.w, r.h, C_FACE);
                gfx_frame(s, r.x, r.y, r.w, r.h, C_LINE);
                gfx_text(s, r.x + 6, r.y + 3, net.nf[i].text, C_DIM);
            } else {
                fct_field_draw(s, r, &net.nf[i], i == net.focus, NULL);
            }
        }
        struct rect b = apply_btn(c);
        ui_button(s, b, "Apply", false);
    }
    int wy = c.y + 330;
    gfx_hline(s, c.x + PAD, wy, c.w - 2 * PAD, C_LINE);
    gfx_text_bold(s, c.x + PAD, wy + 10, "Wi-Fi", C_TEXT);
    gfx_text(s, c.x + PAD + 60, wy + 10, net.wifi[0] ? net.wifi : "no wireless adapter", C_DIM);
    if (!status[0])
        snprintf(status, sizeof(status), "Apply keeps the settings for the next boot (/etc/network.conf).");
}

static void network_apply(void)
{
    char *argv[16];
    int n = 0;
    argv[n++] = "ifconfig";
    argv[n++] = "-P";
    argv[n++] = net.ni[net.cur].name;
    if (net.dhcp) {
        argv[n++] = "dhcp";
    } else {
        argv[n++] = "inet", argv[n++] = net.nf[F_IP].text;
        argv[n++] = "netmask", argv[n++] = net.nf[F_MASK].text;
        if (net.nf[F_GW].text[0] && strcmp(net.nf[F_GW].text, "0.0.0.0"))
            argv[n++] = "gateway", argv[n++] = net.nf[F_GW].text;
        if (net.nf[F_DNS].text[0] && strcmp(net.nf[F_DNS].text, "0.0.0.0"))
            argv[n++] = "dns", argv[n++] = net.nf[F_DNS].text;
    }
    argv[n] = NULL;
    int to[2], from[2];
    if (pipe(to) < 0 || pipe(from) < 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(to[0], 0);
        dup2(from[1], 2);
        dup2(from[1], 1);
        close(to[1]);
        close(from[0]);
        execv("/bin/ifconfig", argv);
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    dprintf(to[1], "%s\n", net.nf[F_PASS].text);
    close(to[1]);
    memset(net.nf[F_PASS].text, 0, sizeof(net.nf[F_PASS].text));
    fct_field_set(&net.nf[F_PASS], "");
    char out[200] = "";
    ssize_t k = read(from[0], out, sizeof(out) - 1);
    out[k > 0 ? k : 0] = 0;
    out[strcspn(out, "\n")] = 0;
    close(from[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    bool ok = pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    snprintf(status, sizeof(status), "%s", ok ? "Applied." : out[0] ? out : "Could not apply the settings.");
    if (ok) {
        sleep(net.dhcp ? 2 : 0);
        int keep = net.cur;
        network_enter();
        net.cur = keep;
        net_load_fields();
    }
}

static void network_click(struct rect c, int x, int y)
{
    net.focus = -1;
    for (int i = 0; i < net.n; i++)
        if (rect_contains(if_tab(c, i), x, y)) {
            net.cur = i;
            net_load_fields();
        }
    for (int i = 0; i < 2; i++)
        if (rect_contains(mode_btn(c, i), x, y))
            net.dhcp = i == 0;
    if (net.n && rect_contains(apply_btn(c), x, y))
        network_apply();
}

static bool network_field_mouse(struct rect c, int x, int y, int kind)
{
    for (int i = 0; i < NFIELDS; i++)
        if ((i == F_PASS ? geteuid() != 0 : !net.dhcp) && (kind != FCT_MOUSE_MOVE || net.nf[i].dragging) &&
            fct_field_mouse(&net.nf[i], field_rect(c, i), x, y, kind)) {
            net.focus = i;
            return true;
        }
    return false;
}

/* Typing into the focused field; true if the key was taken. */
static bool network_key(const struct fct_key *k)
{
    if (net.focus < 0 || !k->value)
        return false;
    if (k->ascii == '\t') {
        do
            net.focus = (net.focus + 1) % NFIELDS;
        while ((net.focus == F_PASS && geteuid() == 0) || (net.focus != F_PASS && net.dhcp));
    } else if (k->ascii == '\n' || k->ascii == '\r') {
        net.focus = -1;
        network_apply();
    } else if (net.focus == F_PASS || !(k->ascii >= 32 && k->ascii < 127) || (k->mods & FCT_MOD_CTRL) ||
               (k->ascii >= '0' && k->ascii <= '9') || k->ascii == '.') {
        fct_field_key(&net.nf[net.focus], k);    /* (an address: digits and dots typed) */
    }
    return true;
}

/* ---------------- Wi-Fi ---------------- */

#define WROWS 10
static struct {
    struct sieos_wifi_status st;
    struct sieos_wifi_bss b[64];
    int n, sel, ticks;
    unsigned last_scans;
    struct fct_field pass;                      /* (masked) */
    bool pass_focus;
    pid_t job;                                   /* dladm connect-wifi running */
    struct sieos_wifi_bss chosen;
} wl = { .sel = -1 };

static int bss_cmp(const void *a, const void *b)
{
    return ((const struct sieos_wifi_bss *)b)->wb_rssi - ((const struct sieos_wifi_bss *)a)->wb_rssi;
}

static void wifi_refresh(void)
{
    if (wifi(SIEOS_WIFI_OP_STATUS, &wl.st, sizeof(wl.st)) < 0)
        wl.st.ws_state = SIEOS_WIFI_NONE;
    unsigned char sel[6];
    bool had = wl.sel >= 0 && wl.sel < wl.n;
    if (had)
        memcpy(sel, wl.b[wl.sel].wb_bssid, 6);
    long n = wifi(SIEOS_WIFI_OP_RESULTS, wl.b, 64);
    wl.n = n > 0 ? (int)n : 0;
    qsort(wl.b, wl.n, sizeof(wl.b[0]), bss_cmp);
    wl.sel = -1;
    for (int i = 0; had && i < wl.n && i < WROWS; i++)
        if (!memcmp(wl.b[i].wb_bssid, sel, 6))
            wl.sel = i;
}

static void wifi_scan(void)
{
    if (wifi(SIEOS_WIFI_OP_SCAN, NULL, 0) == 0)
        snprintf(status, sizeof(status), "Scanning...");
    else if (errno != EBUSY)
        snprintf(status, sizeof(status), "Could not scan: %s", strerror(errno));
    wifi_refresh();
}

static void wifi_enter(void)
{
    wl.pass.masked = true;
    wl.sel = -1;
    wl.ticks = 0;
    wifi_refresh();
    wl.last_scans = wl.st.ws_scans;
    if (wl.st.ws_state == SIEOS_WIFI_READY)
        wifi_scan();
}

static struct rect scan_btn(struct rect c) { return rect_make(c.x + c.w - PAD - 110, c.y + 14, 110, 24); }
static struct rect wrow(struct rect c, int i) { return rect_make(c.x + PAD, c.y + 96 + i * ROW, c.w - 2 * PAD, ROW); }

static const char *wsec(unsigned s)
{
    if (s & SIEOS_WIFI_SEC_RSN)
        return s & SIEOS_WIFI_SEC_SAE ? "WPA3" : s & SIEOS_WIFI_SEC_PSK ? "WPA2" : "WPA2 Enterprise";
    return s & SIEOS_WIFI_SEC_WPA ? "WPA" : s & SIEOS_WIFI_SEC_WEP ? "WEP" : "Open";
}

/* Signal bars: four, filled by strength. */
static void bars(struct surface *s, int x, int y, int rssi)
{
    int n = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 3;
        gfx_fill(s, x + i * 5, y + 14 - h, 3, h, i < n ? C_ACCENT : C_LINE);
    }
}

static void wifi_join_draw(struct surface *s, struct rect c);

static void wifi_draw(struct surface *s, struct rect c)
{
    icon_draw(s, ICON_NETWORK, c.x + PAD, c.y + 10, 32);
    gfx_text_bold(s, c.x + PAD + 42, c.y + 10, "Wi-Fi", C_TEXT);
    static const char *const states[] = { "no Wi-Fi device", "not working", "not connected", "scanning",
                                          "connecting", "connected" };
    int st = wl.st.ws_state >= 0 && wl.st.ws_state <= 5 ? wl.st.ws_state : 1;
    char line[160];
    if (st == SIEOS_WIFI_NONE)
        snprintf(line, sizeof(line), "%s", states[st]);
    else
        snprintf(line, sizeof(line), "%s%s%s  (%02x:%02x:%02x:%02x:%02x:%02x)", states[st],
                 st == SIEOS_WIFI_DOWN ? ": " : "", st == SIEOS_WIFI_DOWN ? wl.st.ws_info : "", wl.st.ws_mac[0],
                 wl.st.ws_mac[1], wl.st.ws_mac[2], wl.st.ws_mac[3], wl.st.ws_mac[4], wl.st.ws_mac[5]);
    gfx_text(s, c.x + PAD + 42, c.y + 28, line, C_DIM);
    if (st >= SIEOS_WIFI_READY)
        ui_button(s, scan_btn(c), st == SIEOS_WIFI_SCANNING ? "Scanning..." : "Scan", st == SIEOS_WIFI_SCANNING);
    if (st < SIEOS_WIFI_READY)
        return;
    gfx_text(s, c.x + PAD, c.y + 70, "Network", C_DIM);
    gfx_text(s, c.x + c.w - PAD - 250, c.y + 70, "Security", C_DIM);
    gfx_text(s, c.x + c.w - PAD - 120, c.y + 70, "Signal", C_DIM);
    gfx_text(s, c.x + c.w - PAD - 50, c.y + 70, "Chan.", C_DIM);
    gfx_hline(s, c.x + PAD, c.y + 90, c.w - 2 * PAD, C_LINE);
    if (!wl.n)
        gfx_text(s, c.x + PAD, c.y + 100, st == SIEOS_WIFI_SCANNING ? "Looking for networks..." : "No network found.",
                 C_TEXT);
    for (int i = 0; i < wl.n && i < WROWS; i++) {
        struct rect r = wrow(c, i);
        if (i == wl.sel)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_SELECT);
        const struct sieos_wifi_bss *b = &wl.b[i];
        gfx_text(s, r.x + 6, r.y + 4, b->wb_ssid[0] ? b->wb_ssid : "(hidden network)", b->wb_ssid[0] ? C_TEXT : C_DIM);
        gfx_text(s, c.x + c.w - PAD - 250, r.y + 4, wsec(b->wb_sec), C_TEXT);
        bars(s, c.x + c.w - PAD - 120, r.y + 4, b->wb_rssi);
        char dbm[16], ch[8];
        snprintf(dbm, sizeof(dbm), "%d dBm", b->wb_rssi);
        gfx_text(s, c.x + c.w - PAD - 96, r.y + 4, dbm, C_DIM);
        snprintf(ch, sizeof(ch), "%d", b->wb_channel);
        gfx_text(s, c.x + c.w - PAD - 50, r.y + 4, ch, C_TEXT);
    }
    wifi_join_draw(s, c);
    if (!status[0])
        snprintf(status, sizeof(status), "%d network%s heard.", wl.n, wl.n == 1 ? "" : "s");
}

static int join_y(struct rect c) { return c.y + 96 + WROWS * ROW + 10; }
static struct rect pass_rect(struct rect c) { return rect_make(c.x + PAD + 90, join_y(c) + 24, 220, 22); }
static struct rect join_btn(struct rect c) { return rect_make(c.x + c.w - PAD - 110, join_y(c) + 23, 110, 24); }

/* dladm connect-wifi for the network chosen (the passphrase on its standard input), not waited for. */
static void wifi_connect(void)
{
    if (wl.job > 0)
        return;
    int p[2];
    if (pipe(p) < 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[0], 0);
        close(p[1]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0)
            dup2(dn, 1), dup2(dn, 2);
        if (wl.pass.text[0])
            execl("/bin/dladm", "dladm", "connect-wifi", "-e", wl.chosen.wb_ssid, "-k", "-", (char *)NULL);
        else
            execl("/bin/dladm", "dladm", "connect-wifi", "-e", wl.chosen.wb_ssid, (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    if (pid > 0 && wl.pass.text[0])
        dprintf(p[1], "%s\n", wl.pass.text);
    close(p[1]);
    memset(wl.pass.text, 0, sizeof(wl.pass.text));
    fct_field_set(&wl.pass, "");
    wl.pass_focus = false;
    wl.job = pid;
    snprintf(status, sizeof(status), "Connecting to %s...", wl.chosen.wb_ssid);
}

static void wifi_disconnect(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/dladm", "dladm", "disconnect-wifi", (char *)NULL);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
    wifi_refresh();
}

static void wifi_join_draw(struct surface *s, struct rect c)
{
    int y = join_y(c);
    gfx_hline(s, c.x + PAD, y, c.w - 2 * PAD, C_LINE);
    if (wl.st.ws_state == SIEOS_WIFI_JOINED || wl.st.ws_state == SIEOS_WIFI_JOINING) {
        char line[96];
        snprintf(line, sizeof(line), "%s %s", wl.st.ws_state == SIEOS_WIFI_JOINED ? "Connected to" : "Connecting to",
                 wl.st.ws_ssid);
        gfx_text_bold(s, c.x + PAD, y + 20, line, C_TEXT);
        if (wl.st.ws_state == SIEOS_WIFI_JOINED && wl.st.ws_mode) {
            static const char *const modes[] = { "", "802.11a/g", "802.11n", "802.11ac" };
            static const char *const pm[] = { "off", "on", "maximum" };
            snprintf(line, sizeof(line), "%s, %d MHz, %d stream%s; power saving %s", modes[wl.st.ws_mode & 3],
                     wl.st.ws_width, wl.st.ws_streams, wl.st.ws_streams == 1 ? "" : "s", pm[wl.st.ws_power % 3]);
            gfx_text(s, c.x + PAD, y + 40, line, C_DIM);
        }
        ui_button(s, join_btn(c), "Disconnect", false);
        return;
    }
    if (wl.sel < 0 || wl.sel >= wl.n) {
        gfx_text(s, c.x + PAD, y + 28, "Choose a network to connect to.", C_DIM);
        return;
    }
    const struct sieos_wifi_bss *b = &wl.b[wl.sel];
    gfx_text_bold(s, c.x + PAD, y + 6, b->wb_ssid[0] ? b->wb_ssid : "(hidden network)", C_TEXT);
    if (b->wb_sec) {
        gfx_text(s, c.x + PAD, y + 27, "Password", C_TEXT);
        struct rect r = pass_rect(c);
        fct_field_draw(s, r, &wl.pass, wl.pass_focus, NULL);
    } else {
        gfx_text(s, c.x + PAD, y + 27, "An open network (not encrypted).", C_DIM);
    }
    if (b->wb_ssid[0])
        ui_button(s, join_btn(c), wl.job > 0 ? "Connecting..." : "Connect", wl.job > 0);
}

static void wifi_click(struct rect c, int x, int y)
{
    if (wl.st.ws_state >= SIEOS_WIFI_READY && rect_contains(scan_btn(c), x, y)) {
        wifi_scan();
        return;
    }
    if (rect_contains(join_btn(c), x, y)) {
        if (wl.st.ws_state == SIEOS_WIFI_JOINED || wl.st.ws_state == SIEOS_WIFI_JOINING)
            wifi_disconnect();
        else if (wl.sel >= 0 && wl.sel < wl.n && wl.b[wl.sel].wb_ssid[0]) {
            wl.chosen = wl.b[wl.sel];
            wifi_connect();
        }
        return;
    }
    wl.pass_focus = wl.sel >= 0 && wl.sel < wl.n && wl.b[wl.sel].wb_sec && rect_contains(pass_rect(c), x, y);
    for (int i = 0; i < wl.n && i < WROWS; i++)
        if (rect_contains(wrow(c, i), x, y)) {
            if (wl.sel != i)
                fct_field_set(&wl.pass, "");
            wl.sel = i;
            wl.pass_focus = wl.b[i].wb_sec != 0;
        }
}

/* Typing the password; Enter connects. */
static bool wifi_key(const struct fct_key *k)
{
    if (!wl.pass_focus || !k->value)
        return false;
    if (k->ascii == '\n' || k->ascii == '\r') {
        if (wl.sel >= 0 && wl.sel < wl.n && wl.b[wl.sel].wb_ssid[0]) {
            wl.chosen = wl.b[wl.sel];
            wifi_connect();
        }
    } else if (strlen(wl.pass.text) < 63 || !(k->ascii >= 32 && k->ascii < 127)) {
        fct_field_key(&wl.pass, k);              /* (a WPA2 passphrase: up to 63 characters) */
    }
    return true;
}

static bool wifi_field_mouse(struct rect c, int x, int y, int kind)
{
    if (!(wl.sel >= 0 && wl.sel < wl.n && wl.b[wl.sel].wb_sec))
        return false;
    if ((kind != FCT_MOUSE_MOVE || wl.pass.dragging) && fct_field_mouse(&wl.pass, pass_rect(c), x, y, kind)) {
        wl.pass_focus = true;
        return true;
    }
    return false;
}

/* About 4 times a second: the status; the list when a scan ends; a new scan every 30 seconds. */
static void wifi_tick(void)
{
    wifi_refresh();
    int st;
    if (wl.job > 0 && waitpid(wl.job, &st, WNOHANG) == wl.job) {
        wl.job = 0;
        bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        snprintf(status, sizeof(status), "%s", ok ? "Connected." : wl.st.ws_info[0] ? wl.st.ws_info : "Could not connect.");
        return;
    }
    if (wl.st.ws_scans != wl.last_scans) {
        wl.last_scans = wl.st.ws_scans;
        status[0] = 0;
    }
    if (++wl.ticks % 120 == 0 && wl.st.ws_state == SIEOS_WIFI_READY)
        wifi_scan();
}

/* ---------------- the window ---------------- */

static const struct page pages[] = {
    { "display", "Display", ICON_MONITOR, display_enter, display_draw, display_click, display_move, NULL },
    { "appearance", "Appearance", ICON_PROGRAM, NULL, appearance_draw, appearance_click, NULL, NULL },
    { "pointer", "Pointer", ICON_PROGRAM, pointer_enter, pointer_draw, pointer_click, NULL, NULL },
    { "saver", "Screen Saver", ICON_MONITOR, saver_enter, saver_draw, saver_click, NULL, NULL },
    { "assistant", "Assistant", ICON_TERMINAL, assistant_enter, assistant_draw, assistant_click, NULL, assistant_field_mouse },
    { "network", "Network", ICON_NETWORK, network_enter, network_draw, network_click, NULL, network_field_mouse },
    { "wifi", "Wi-Fi", ICON_NETWORK, wifi_enter, wifi_draw, wifi_click, NULL, wifi_field_mouse },
};
#define NPAGES ((int)(sizeof(pages) / sizeof(pages[0])))

static struct rect side_item(struct rect c, int i)
{
    return rect_make(c.x + 6, c.y + 8 + i * 40, SIDE_W - 12, 36);
}

static struct rect page_rect(struct rect c)
{
    return rect_make(c.x + SIDE_W, c.y, c.w - SIDE_W, c.h - 30);
}

static void open_page(int i)
{
    cur_page = i;
    status[0] = 0;
    if (pages[i].enter)
        pages[i].enter();
}

static void draw(struct fct_view *v, struct surface *s, struct rect c)
{
    (void)v;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_FACE);
    /* the sidebar: the sections */
    gfx_fill(s, c.x, c.y, SIDE_W, c.h, C_CONTENT);
    gfx_vline(s, c.x + SIDE_W - 1, c.y, c.h, C_LINE);
    for (int i = 0; i < NPAGES; i++) {
        struct rect r = side_item(c, i);
        if (i == cur_page)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_SELECT);
        else if (i == side_hover)
            gfx_fill(s, r.x, r.y, r.w, r.h, C_CONTENT_ALT);
        icon_draw(s, pages[i].icon, r.x + 6, r.y + 6, 24);
        if (i == cur_page)
            gfx_text_bold(s, r.x + 38, r.y + (r.h - FONT_H) / 2, pages[i].title, C_TEXT);
        else
            gfx_text(s, r.x + 38, r.y + (r.h - FONT_H) / 2, pages[i].title, C_TEXT);
    }
    pages[cur_page].draw(s, page_rect(c));
    gfx_text(s, c.x + SIDE_W + PAD, c.y + c.h - 24, status, C_DIM);
}

static void mouse(struct fct_view *v, int x, int y, int kind, int buttons)
{
    (void)buttons;
    struct rect c = fct_view_content(v);
    x += c.x;
    y += c.y;
    if (pages[cur_page].field_mouse && pages[cur_page].field_mouse(page_rect(c), x, y, kind)) {
        fct_view_invalidate(v);
        return;
    }
    if (kind == FCT_MOUSE_MOVE) {
        int hit = -1;
        for (int i = 0; i < NPAGES; i++)
            if (rect_contains(side_item(c, i), x, y))
                hit = i;
        side_hover = hit;
        if (pages[cur_page].move)
            pages[cur_page].move(page_rect(c), x, y);
        fct_view_invalidate(v);
        return;
    }
    if (kind != FCT_MOUSE_DOWN)
        return;
    for (int i = 0; i < NPAGES; i++)
        if (rect_contains(side_item(c, i), x, y)) {
            if (i != cur_page)
                open_page(i);
            fct_view_invalidate(v);
            return;
        }
    if (pages[cur_page].click)
        pages[cur_page].click(page_rect(c), x, y);
    fct_view_invalidate(v);
}

static void tick(struct fct_view *v)
{
    if (!strcmp(pages[cur_page].name, "wifi")) {
        wifi_tick();
        fct_view_invalidate(v);
    }
}

static void key(struct fct_view *v, const struct fct_key *k)
{
    if (!strcmp(pages[cur_page].name, "assistant") && assistant_key(k)) {
        fct_view_invalidate(v);
        return;
    }
    if (!strcmp(pages[cur_page].name, "wifi") && wifi_key(k)) {
        fct_view_invalidate(v);
        return;
    }
    if (!strcmp(pages[cur_page].name, "network") && network_key(k)) {
        fct_view_invalidate(v);
        return;
    }
    if (k->value && (k->code == FCT_KEY_UP || k->code == FCT_KEY_DOWN)) {   /* up and down: the sections */
        open_page((cur_page + (k->code == FCT_KEY_DOWN ? 1 : NPAGES - 1)) % NPAGES);
        fct_view_invalidate(v);
    }
}

int main(int argc, char **argv)
{
    if (fct_app_init() < 0)
        return 1;
    int first = 0;
    for (int i = 0; argc > 1 && i < NPAGES; i++)
        if (!strcmp(argv[1], pages[i].name))
            first = i;
    open_page(first);
    int h = 36 + FCT_NSKINS * (CARD_H + 10) + 34;
    if (h < 96 + WROWS * ROW + 130)
        h = 96 + WROWS * ROW + 130;                  /* (the Wi-Fi page: the list, the join box) */
    struct fct_window_attr a = { "Settings", FCT_POS_CENTER, FCT_POS_CENTER, SIDE_W + 560, h, SIDE_W + 400, 300, 0 };
    struct fct_view *v = fct_view_create(&a);
    if (!v)
        return 1;
    v->draw = draw;
    v->mouse = mouse;
    v->key = key;
    v->tick = tick;
    return fct_main();
}
