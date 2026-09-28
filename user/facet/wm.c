/*
 * wm.c - Facet desktop, Strata look: window manager, spine dock, sia
 * strip, workspaces, menus, cursor and the main event loop.
 *
 *   spine    a rounded dock down the left edge: the SIEOS menu (gem),
 *            application tiles with running marks, four workspaces, clock
 *   windows  small-radius frames with a window-menu button on the left, a
 *            close box on the right and a band in the workspace colour
 *   strip    the sia command line along the top: what is typed there is
 *            sent to the assistant terminal; it also shows the model's
 *            state and last action (read from ~/.sia)
 *
 * Keys: Ctrl+Space strip, Ctrl+Alt+1..4 or Ctrl+Alt+Left/Right workspaces,
 * Alt+Tab next window, Alt+F4 close.
 */
#include "facet.h"
#include "json.h"

#define MAXWIN 24
#define DOUBLE_CLICK_MS 450

/* spine */
#define SPINE_X   12
#define SPINE_W   72
#define SPINE_R   22
#define SP_PAD    14
#define GEM_H     32
#define SEP_H     17
#define TILE      48
#define TILE_GAP  10
#define WS_CELL   26
#define WS_GAP    6
#define CLOCK_H   40
/* strip */
#define STRIP_H   36
#define MARGIN    12
/* windows */
#define WIN_R     6
#undef SHADOW                      /* <shadow.h> names the file */
#define SHADOW    14
#define SHADOW_DY 6
#define BTN_W     22
#define BTN_H     20

int screen_w, screen_h;
char desktop_user[32];
const color_t ws_color[NWORKSPACES] = {
    RGB(0xD9, 0xA1, 0x5F), RGB(0x8F, 0xB4, 0xDC), RGB(0x9A, 0xAB, 0x8C), RGB(0xB3, 0x9C, 0xC0),
};

static uint32_t *fbmem;
static struct fb_info fbi;
static struct surface back, bg;
static struct rect dirty;
static int ev_fd = -1;

static struct window *zorder[MAXWIN];      /* bottom .. top */
static int nwin, next_id = 1;
static struct window *focus, *prev_focus;
static int cur_ws;

static int mouse_x, mouse_y;
static unsigned buttons;
static unsigned key_mods;
static bool running = true;

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_CONTENT, DRAG_BUTTON };
static int drag_mode;
static struct window *drag_win;
static int drag_dx, drag_dy;
static struct rect drag_start;
static int pressed_button = -1;

static long last_click_ms;
static int last_click_x, last_click_y;

static struct netinfo net;
static bool net_ok;

/* sia strip */
static bool strip_focus;
static char strip_buf[256];
static int strip_len;
static bool caret_on = true;
static bool sia_registered;
static char sia_model[64];
static char sia_state[16];
static char sia_last[96];

static int hover_tile = -1, hover_ws = -1;
static bool agent_busy, agent_thinking, confirm_pending, panel_open;   /* the strip's assistant */

static void do_logout(void) { running = false; }
static void open_home(void) { app_files(getenv("HOME") ? getenv("HOME") : "/"); }

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

struct tile {
    const char *label;
    int icon;
    void (*open)(void);
    const char *match;                     /* window titles starting with this */
    color_t top, bottom;
};

static struct tile tiles[] = {
    { "Terminal (sia)", ICON_TERMINAL, app_terminal, "Terminal", RGB(0x44, 0x4A, 0x52), RGB(0x25, 0x28, 0x2D) },
    { "Shell", ICON_PROGRAM, app_shell_terminal, "Shell", RGB(0x5C, 0x61, 0x6A), RGB(0x36, 0x3A, 0x41) },
    { "Files", ICON_FOLDER, open_home, "Files", RGB(0x7F, 0xA3, 0xC8), RGB(0x4F, 0x72, 0x99) },
    { "System Monitor", ICON_MONITOR, app_monitor, "System Monitor", RGB(0x9A, 0xAB, 0x8C), RGB(0x6C, 0x7C, 0x60) },
    { "Network Status", ICON_NETWORK, app_network, "Network Status", RGB(0xD6, 0xBF, 0x96), RGB(0xA8, 0x8F, 0x66) },
    { "Clock", ICON_CLOCK, app_clock, "Clock", RGB(0xB3, 0x9C, 0xC0), RGB(0x7F, 0x6A, 0x8F) },
};
#define NTILES (int)(sizeof(tiles) / sizeof(tiles[0]))

/* The sia strip runs along the top, across the whole screen. */
static struct rect strip_rect(void)
{
    return rect_make(MARGIN, MARGIN, screen_w - 2 * MARGIN, STRIP_H);
}

/* Below the strip: the spine and the windows. */
static int below_strip(void)
{
    return MARGIN + STRIP_H + MARGIN;
}

static struct rect spine_rect(void)
{
    int h = SP_PAD + GEM_H + SEP_H + NTILES * (TILE + TILE_GAP) - TILE_GAP + SEP_H + 2 * WS_CELL + WS_GAP + 8 +
            CLOCK_H + SP_PAD;
    int top = below_strip(), avail = screen_h - top - MARGIN;
    int y = top + MAX(0, (avail - h) / 2);
    return rect_make(SPINE_X, y, SPINE_W, MIN(h, avail));
}

static struct rect gem_rect(void)
{
    struct rect sp = spine_rect();
    return rect_make(sp.x + (SPINE_W - 40) / 2, sp.y + SP_PAD, 40, GEM_H);
}

static struct rect tile_rect(int i)
{
    struct rect sp = spine_rect();
    return rect_make(sp.x + (SPINE_W - TILE) / 2, sp.y + SP_PAD + GEM_H + SEP_H + i * (TILE + TILE_GAP), TILE, TILE);
}

static int ws_top(void)
{
    struct rect last = tile_rect(NTILES - 1);
    return last.y + TILE + SEP_H;
}

static struct rect ws_rect(int i)
{
    struct rect sp = spine_rect();
    int x0 = sp.x + (SPINE_W - (2 * WS_CELL + WS_GAP)) / 2;
    return rect_make(x0 + (i % 2) * (WS_CELL + WS_GAP), ws_top() + (i / 2) * (WS_CELL + WS_GAP), WS_CELL, WS_CELL);
}

static struct rect clock_rect(void)
{
    struct rect sp = spine_rect();
    return rect_make(sp.x, ws_top() + 2 * WS_CELL + WS_GAP + 8, SPINE_W, CLOCK_H);
}

static struct rect tooltip_rect(int i)
{
    struct rect t = tile_rect(i);
    return rect_make(SPINE_X + SPINE_W + 10, t.y + (TILE - 26) / 2, text_width(tiles[i].label) + 22, 26);
}

/* Where windows may be placed or maximised. */
static struct rect work_area(void)
{
    int x = SPINE_X + SPINE_W + MARGIN, y = below_strip();
    return rect_make(x, y, screen_w - x - MARGIN, screen_h - y - MARGIN);
}

void wm_invalidate_rect(struct rect r)
{
    dirty = rect_union(dirty, rect_intersect(r, rect_make(0, 0, screen_w, screen_h)));
}

static void invalidate_all(void)
{
    wm_invalidate_rect(rect_make(0, 0, screen_w, screen_h));
}

struct window *wm_focused(void)
{
    return focus;
}

struct rect wm_content(struct window *w)
{
    return rect_make(w->r.x + 1 + BAND_W, w->r.y + TITLE_H, w->r.w - 2 - BAND_W, w->r.h - TITLE_H - 1);
}

/* Everything the window paints, shadow included. */
static struct rect window_bounds(struct window *w)
{
    return rect_make(w->r.x - SHADOW, w->r.y - SHADOW + SHADOW_DY, w->r.w + 2 * SHADOW, w->r.h + 2 * SHADOW);
}

void wm_invalidate(struct window *w)
{
    wm_invalidate_rect(window_bounds(w));
}

static bool visible(const struct window *w)
{
    return !w->minimized && !w->dead && w->ws == cur_ws;
}

static void invalidate_spine(void)
{
    struct rect sp = spine_rect();
    wm_invalidate_rect(rect_make(sp.x - SHADOW, sp.y - SHADOW, sp.w + 2 * SHADOW + 200, sp.h + 2 * SHADOW + SHADOW_DY));
}

static void invalidate_strip(void)
{
    struct rect s = strip_rect();
    wm_invalidate_rect(rect_make(s.x - SHADOW, s.y - SHADOW, s.w + 2 * SHADOW, s.h + 2 * SHADOW));
}

/* ------------------------------------------------------------------ */
/* Desktop background                                                  */
/* ------------------------------------------------------------------ */

static void render_background(void)
{
    gfx_set_clip(&bg, rect_make(0, 0, bg.w, bg.h));
    gfx_vgradient(&bg, 0, 0, bg.w, bg.h, C_DESK_TOP, C_DESK_BOT);
    /* strata: alternate bands a shade lighter */
    for (int i = 0; i < 4; i += 2)
        gfx_blend_fill(&bg, 0, i * bg.h / 4, bg.w, bg.h / 4, RGB(0xFF, 0xFF, 0xFF), 5);
    for (int y = 0; y < bg.h; y += 4)                /* faint grain */
        gfx_blend_fill(&bg, 0, y, bg.w, 1, RGB(0, 0, 0), 10);
    const char *mark = "SIEOS";
    const char *tag = "Synthetic Intelligence Enhanced Operating System";
    int scale = 4;
    int mx = bg.w - MAX(text_width(mark) * scale, text_width(tag)) - 28;
    int my = bg.h - FONT_H * scale - FONT_H - 8 - 28;
    gfx_text_scaled(&bg, mx, my, mark, scale, color_shade(C_DESK_BOT, 16));
    gfx_text(&bg, mx + 2, my + FONT_H * scale + 8, tag, color_shade(C_DESK_BOT, 44));
}

/* ------------------------------------------------------------------ */
/* Popup menus                                                         */
/* ------------------------------------------------------------------ */

struct menu_item {
    const char *label;                     /* NULL = separator */
    void (*action)(void *arg);
    void *arg;
    int icon;                              /* -1 = none */
};

#define MENU_ITEM_H 24
#define MENU_SEP_H  9
#define MENU_MAX    28

static struct {
    bool open;
    struct rect r;
    struct menu_item items[MENU_MAX];
    char labels[MENU_MAX][40];
    int n, hover;
} menu;

static struct rect menu_bounds(void)
{
    return rect_make(menu.r.x - SHADOW, menu.r.y - SHADOW, menu.r.w + 2 * SHADOW, menu.r.h + 2 * SHADOW + SHADOW_DY);
}

static void menu_open(int x, int y, struct menu_item *items, int n)
{
    n = MIN(n, MENU_MAX);
    int w = 0, h = 8;
    for (int i = 0; i < n; i++) {
        menu.items[i] = items[i];
        if (items[i].label) {
            strlcpy(menu.labels[i], items[i].label, sizeof(menu.labels[i]));
            menu.items[i].label = menu.labels[i];
            w = MAX(w, text_width(menu.labels[i]));
            h += MENU_ITEM_H;
        } else {
            h += MENU_SEP_H;
        }
    }
    w += 52;
    if (x + w > screen_w - 4)
        x = screen_w - 4 - w;
    if (y + h > screen_h - 4)
        y = screen_h - 4 - h;
    menu.r = rect_make(MAX(0, x), MAX(0, y), w, h);
    menu.n = n;
    menu.hover = -1;
    menu.open = true;
    wm_invalidate_rect(menu_bounds());
}

static void menu_close(void)
{
    if (menu.open)
        wm_invalidate_rect(menu_bounds());
    menu.open = false;
}

static struct rect menu_item_rect(int i)
{
    int y = menu.r.y + 4;
    for (int k = 0; k < i; k++)
        y += menu.items[k].label ? MENU_ITEM_H : MENU_SEP_H;
    return rect_make(menu.r.x + 4, y, menu.r.w - 8, menu.items[i].label ? MENU_ITEM_H : MENU_SEP_H);
}

static int menu_hit(int x, int y)
{
    if (!rect_contains(menu.r, x, y))
        return -1;
    for (int i = 0; i < menu.n; i++)
        if (menu.items[i].label && rect_contains(menu_item_rect(i), x, y))
            return i;
    return -1;
}

static void draw_menu(struct surface *s)
{
    if (!menu.open)
        return;
    struct rect r = menu.r;
    gfx_shadow(s, r, 8, SHADOW, SHADOW_DY, 170);
    gfx_round_rect(s, r.x, r.y, r.w, r.h, 8, C_MENU);
    gfx_round_frame(s, r.x, r.y, r.w, r.h, 8, C_FACE_LIGHT);
    for (int i = 0; i < menu.n; i++) {
        struct rect ir = menu_item_rect(i);
        if (!menu.items[i].label) {
            gfx_hline(s, ir.x + 8, ir.y + ir.h / 2, ir.w - 16, C_LINE);
            continue;
        }
        if (i == menu.hover)
            gfx_round_rect(s, ir.x, ir.y, ir.w, ir.h, 5, C_MENU_HOT);
        if (menu.items[i].icon >= 0)
            icon_draw(s, menu.items[i].icon, ir.x + 6, ir.y + 3, 18);
        gfx_text(s, ir.x + 32, ir.y + 4, menu.items[i].label, C_TEXT);
    }
}

/* ------------------------------------------------------------------ */
/* System information and sia status                                   */
/* ------------------------------------------------------------------ */

static unsigned long cpu_prev_busy[16], cpu_prev_total[16];
static int cpu_pct[16], ncpus = 1;

/* Value of "key=" in a small key=value file. */
static bool read_key(const char *path, const char *key, char *out, size_t outlen)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    char line[256];
    size_t kl = strlen(key);
    bool found = false;
    while (read_line_fd(fd, line, sizeof(line)) >= 0) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            strlcpy(out, line + kl + 1, outlen);
            found = true;
        }
    }
    close(fd);
    return found;
}

static void sample_sia(void)
{
    const char *home = getenv("HOME");
    if (!home)
        return;
    char cfg[160], st[160], endpoint[16] = "";
    snprintf(cfg, sizeof(cfg), "%s/.sia/config", home);
    snprintf(st, sizeof(st), "%s/.sia/status", home);
    sia_model[0] = 0;
    read_key(cfg, "model", sia_model, sizeof(sia_model));
    read_key(cfg, "endpoint", endpoint, sizeof(endpoint));
    sia_registered = sia_model[0] && endpoint[0];
    sia_state[0] = sia_last[0] = 0;
    read_key(st, "state", sia_state, sizeof(sia_state));
    read_key(st, "last", sia_last, sizeof(sia_last));
}

static void sample_system(void)
{
    struct cpuinfo ci[16];
    int n = cpuinfo(ci, 16);
    ncpus = n > 0 ? n : 1;
    for (int i = 0; i < n; i++) {
        unsigned long busy = ci[i].busy_ticks, total = ci[i].busy_ticks + ci[i].idle_ticks;
        unsigned long db = busy - cpu_prev_busy[i], dt = total - cpu_prev_total[i];
        cpu_pct[i] = dt ? (int)(db * 100 / dt) : 0;
        cpu_prev_busy[i] = busy;
        cpu_prev_total[i] = total;
    }
    net_ok = netinfo(&net) == 0 && net.up;
    sample_sia();
}

int wm_cpu_percent(int cpu) { return cpu < ncpus ? cpu_pct[cpu] : 0; }
int wm_ncpus(void) { return ncpus; }

/* ------------------------------------------------------------------ */
/* Spine                                                               */
/* ------------------------------------------------------------------ */

static bool tile_matches(int i, const struct window *w)
{
    return !w->dead && !strncmp(w->title, tiles[i].match, strlen(tiles[i].match));
}

static bool tile_running(int i)
{
    for (int k = 0; k < nwin; k++)
        if (tile_matches(i, zorder[k]))
            return true;
    return false;
}

static bool gem_menu_open;

static void draw_spine(struct surface *s)
{
    struct rect sp = spine_rect();
    struct rect area = rect_make(sp.x - SHADOW, sp.y - SHADOW, sp.w + 2 * SHADOW + 220, sp.h + 2 * SHADOW + SHADOW_DY);
    if (rect_empty(rect_intersect(area, s->clip)))
        return;
    gfx_shadow(s, sp, SPINE_R, SHADOW, SHADOW_DY, 190);
    gfx_round_rect_vgradient(s, sp.x, sp.y, sp.w, sp.h, SPINE_R, RGB(0x2E, 0x31, 0x37), RGB(0x24, 0x27, 0x2B));
    gfx_round_frame(s, sp.x, sp.y, sp.w, sp.h, SPINE_R, RGB(0x3B, 0x3F, 0x46));
    gfx_hline(s, sp.x + SPINE_R, sp.y + 1, sp.w - 2 * SPINE_R, RGB(0x46, 0x4A, 0x52));

    struct rect g = gem_rect();
    if (gem_menu_open)
        gfx_round_rect(s, g.x - 2, g.y - 2, g.w + 4, g.h + 4, 9, C_MENU_HOT);
    logo_draw(s, g.x + g.w / 2, g.y + g.h / 2, 30);
    gfx_hline(s, sp.x + 16, sp.y + SP_PAD + GEM_H + SEP_H / 2, sp.w - 32, C_LINE);

    for (int i = 0; i < NTILES; i++) {
        struct rect t = tile_rect(i);
        gfx_round_rect_vgradient(s, t.x, t.y, t.w, t.h, 13, tiles[i].top, tiles[i].bottom);
        gfx_blend_fill(s, t.x + 10, t.y + 1, t.w - 20, 1, RGB(0xFF, 0xFF, 0xFF), 60);
        icon_draw(s, tiles[i].icon, t.x + 8, t.y + 8, 32);
        if (i == hover_tile)
            gfx_round_rect_alpha(s, t.x, t.y, t.w, t.h, 13, RGB(0xFF, 0xFF, 0xFF), 34);
        if (tile_running(i))
            gfx_round_rect(s, sp.x + 3, t.y + 17, 4, 14, 2, C_ACCENT);
    }
    int sy = ws_top() - SEP_H / 2 - 1;
    gfx_hline(s, sp.x + 16, sy, sp.w - 32, C_LINE);

    for (int i = 0; i < NWORKSPACES; i++) {
        struct rect r = ws_rect(i);
        bool used = false;
        for (int k = 0; k < nwin; k++)
            if (!zorder[k]->dead && zorder[k]->ws == i)
                used = true;
        color_t bgc = i == cur_ws ? ws_color[i] : i == hover_ws ? RGB(0x3A, 0x3E, 0x45) : RGB(0x31, 0x34, 0x3A);
        gfx_round_rect(s, r.x, r.y, r.w, r.h, 7, bgc);
        char d[2] = { (char)('1' + i), 0 };
        color_t fg = i == cur_ws ? RGB(0x16, 0x17, 0x1A) : used ? C_TEXT : C_DIM;
        gfx_text_bold(s, r.x + (r.w - FONT_W) / 2, r.y + (r.h - FONT_H) / 2, d, fg);
        if (used && i != cur_ws)
            gfx_round_rect(s, r.x + r.w - 7, r.y + 3, 4, 4, 2, ws_color[i]);
    }

    struct rect c = clock_rect();
    struct tm tm;
    time_t now_t = time(NULL);
    gmtime_r(&now_t, &tm);
    static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    char hm[8], day[12];
    snprintf(hm, sizeof(hm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    snprintf(day, sizeof(day), "%s %d", days[tm.tm_wday], tm.tm_mday);
    gfx_text_bold(s, c.x + (c.w - text_width(hm)) / 2, c.y + 2, hm, C_TEXT);
    gfx_text(s, c.x + (c.w - text_width(day)) / 2, c.y + 20, day, C_DIM);

    if (hover_tile >= 0 && !menu.open) {
        struct rect tt = tooltip_rect(hover_tile);
        gfx_shadow(s, tt, 6, 10, 4, 150);
        gfx_round_rect(s, tt.x, tt.y, tt.w, tt.h, 6, C_MENU);
        gfx_round_frame(s, tt.x, tt.y, tt.w, tt.h, 6, C_FACE_LIGHT);
        gfx_text(s, tt.x + 11, tt.y + 5, tiles[hover_tile].label, C_TEXT);
    }
}

/* ------------------------------------------------------------------ */
/* sia strip                                                           */
/* ------------------------------------------------------------------ */

static void draw_strip(struct surface *s)
{
    struct rect st = strip_rect();
    struct rect area = rect_make(st.x - SHADOW, st.y - SHADOW, st.w + 2 * SHADOW, st.h + 2 * SHADOW);
    if (rect_empty(rect_intersect(area, s->clip)))
        return;
    gfx_shadow(s, st, 9, SHADOW, SHADOW_DY, 190);
    gfx_round_rect(s, st.x, st.y, st.w, st.h, 9, C_STRIP);
    gfx_round_frame(s, st.x, st.y, st.w, st.h, 9, strip_focus ? C_ACCENT : C_LINE);
    int ty = st.y + (st.h - FONT_H) / 2;
    int x = st.x + 16;
    x += gfx_text_bold(s, x, ty, "sia", C_ACCENT) + 14;

    /* right side: activity light, model, last action, system */
    char right[200], sys[64], ip[16];
    bool busy = !strcmp(sia_state, "busy") || agent_busy;
    if (!sia_registered)
        snprintf(right, sizeof(right), "no model registered");
    else if (sia_last[0])
        snprintf(right, sizeof(right), "%s - last: %s", sia_model, sia_last);
    else
        snprintf(right, sizeof(right), "%s", sia_model);
    int avg = 0;
    for (int i = 0; i < ncpus; i++)
        avg += cpu_pct[i];
    avg /= ncpus;
    snprintf(sys, sizeof(sys), "cpu %d%%  %s", avg, net_ok ? ip_to_str(net.ip, ip) : "offline");
    int rw = text_width(right), sw = text_width(sys);
    int rx = st.x + st.w - 16 - rw;
    int maxr = st.w / 2;
    if (rw > maxr) {                                   /* keep the input area usable */
        int cut = maxr / FONT_W - 3;
        memcpy(right + cut, "...", 4);
        rw = text_width(right);
        rx = st.x + st.w - 16 - rw;
    }
    gfx_text(s, rx, ty, right, sia_registered ? RGB(0xBD, 0xB7, 0xAD) : C_DIM);
    color_t led = !sia_registered ? RGB(0x55, 0x59, 0x60) : busy ? C_ACCENT : C_GOOD;
    gfx_round_rect(s, rx - 16, ty + 4, 8, 8, 4, led);
    int sx = rx - 16 - 18 - sw;
    gfx_text(s, sx, ty, sys, C_DIM);
    gfx_vline(s, sx - 12, st.y + 9, st.h - 18, C_LINE);

    /* input */
    int avail = (sx - 24 - x) / FONT_W;
    struct rect clip = s->clip;
    gfx_set_clip(s, rect_intersect(clip, rect_make(x, st.y, MAX(0, sx - 24 - x), st.h)));
    if (strip_len || strip_focus) {
        const char *shown = strip_buf;
        if (strip_len > avail - 1)
            shown += strip_len - (avail - 1);
        int w = gfx_text(s, x, ty, shown, C_TEXT);
        if (strip_focus && caret_on)
            gfx_fill(s, x + w + 1, ty, FONT_W, FONT_H, C_TEXT);
    }
    if (!strip_len && !strip_focus) {
        const char *hint = sia_registered ? "Ask sia: open a terminal, launch the clock, how much disk is free?   (Ctrl+Space)"
                                          : "Press Ctrl+Space and Enter to connect a model";
        if (sia_registered && text_width(hint) > sx - 24 - x)
            hint = "Ask sia: open a terminal, launch the clock, how much disk is free?";   /* no room for the shortcut */
        gfx_text(s, x, ty, hint, C_DIM);
    }
    gfx_set_clip(s, clip);
}

static void focus_strip(void);

/* ---------------- the strip's assistant: sia-agent over pipes ---------------- */

#define PANEL_LINES 12
#define PANEL_KEEP  64

static pid_t agent_pid = -1;
static int agent_in = -1, agent_out = -1, agent_chan = -1;
static struct sbuf agent_buf;

static struct pline {
    char text[128];
    color_t color;
} plines[PANEL_KEEP];
static int npl;

static struct rect panel_rect(void)
{
    struct rect st = strip_rect();
    int shown = MIN(npl, PANEL_LINES) + (agent_thinking ? 1 : 0);
    int h = MAX(1, shown) * (FONT_H + 2) + 20;
    return rect_make(st.x, st.y + st.h + 10, st.w, h);         /* drops down below the strip */
}

static void invalidate_panel(void)
{
    struct rect st = strip_rect();
    int bottom = st.y + st.h + 10 + (PANEL_LINES + 1) * (FONT_H + 2) + 20 + SHADOW + SHADOW_DY;
    wm_invalidate_rect(rect_make(st.x - SHADOW, st.y + st.h, st.w + 2 * SHADOW, bottom - st.y - st.h));
}

static void panel_add(const char *text, size_t len, color_t color)
{
    int width = (strip_rect().w - 32) / FONT_W;
    width = MIN(width, (int)sizeof(plines[0].text) - 1);
    size_t i = 0;
    do {                                               /* wrap at the panel width */
        size_t n = 0;
        char line[128];
        while (i < len && text[i] != '\n' && (int)n < width) {
            unsigned char ch = (unsigned char)text[i++];
            if (ch == '\033') {                        /* drop colour codes */
                while (i < len && !(text[i] >= '@' && text[i] <= '~'))
                    i++;
                if (i < len)
                    i++;
                continue;
            }
            line[n++] = ch == '\t' ? ' ' : (ch < 32 || ch > 126) ? '?' : (char)ch;
        }
        if (i < len && text[i] == '\n')
            i++;
        line[n] = 0;
        if (npl == PANEL_KEEP) {
            memmove(plines, plines + 1, sizeof(plines[0]) * (PANEL_KEEP - 1));
            npl--;
        }
        snprintf(plines[npl].text, sizeof(plines[npl].text), "%s", line);
        plines[npl++].color = color;
    } while (i < len);
    invalidate_panel();
}

static void panel_say(const char *text, color_t color)
{
    panel_add(text, strlen(text), color);
}

static void draw_panel(struct surface *s)
{
    if (!panel_open)
        return;
    struct rect p = panel_rect();
    if (rect_empty(rect_intersect(rect_make(p.x - SHADOW, p.y - SHADOW, p.w + 2 * SHADOW, p.h + 2 * SHADOW), s->clip)))
        return;
    gfx_shadow(s, p, 9, SHADOW, SHADOW_DY, 190);
    gfx_round_rect(s, p.x, p.y, p.w, p.h, 9, C_MENU);
    gfx_round_frame(s, p.x, p.y, p.w, p.h, 9, confirm_pending ? C_ACCENT : C_FACE_LIGHT);
    int first = MAX(0, npl - PANEL_LINES), y = p.y + 10;
    for (int i = first; i < npl; i++, y += FONT_H + 2)
        gfx_text(s, p.x + 16, y, plines[i].text, plines[i].color);
    if (agent_thinking) {
        static const char *dots[] = { ".", "..", "..." };
        gfx_text(s, p.x + 16, y, dots[(uptime_ms() / 400) % 3], C_DIM);
    }
    const char *hint = confirm_pending ? "Enter/y: run   n/Esc: skip   a: always" :
                       agent_busy ? "Esc: interrupt" : "Esc: close";
    gfx_text(s, p.x + p.w - 16 - text_width(hint), p.y + p.h - FONT_H - 6, hint, C_DIM);
}

static void agent_send(const char *op, const char *key, const char *text, long num)
{
    if (agent_in < 0)
        return;
    struct sbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"op\":\"%s\"", op);
    if (key && text) {
        sb_printf(&b, ",\"%s\":", key);
        sb_json_str(&b, text);
    } else if (key) {
        sb_printf(&b, ",\"%s\":%ld", key, num);
    }
    sb_puts(&b, "}\n");
    write(agent_in, b.s, b.len);
    sb_free(&b);
}

static void agent_stop(void)
{
    if (agent_pid > 0)
        kill(agent_pid, SIGTERM);
    if (agent_in >= 0)
        close(agent_in);
    if (agent_out >= 0)
        close(agent_out);
    desktop_channel_close(agent_chan);
    agent_pid = -1;
    agent_in = agent_out = agent_chan = -1;
    agent_busy = agent_thinking = confirm_pending = false;
}

static bool agent_start(void)
{
    if (agent_pid > 0)
        return true;
    int in[2], out[2];
    if (pipe(in) < 0)
        return false;
    if (pipe(out) < 0) {
        close(in[0]);
        close(in[1]);
        return false;
    }
    agent_chan = desktop_channel_new();
    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        desktop_channel_close(agent_chan);
        agent_chan = -1;
        return false;
    }
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        int nul = open("/dev/null", O_WRONLY);
        dup2(nul, 2);
        desktop_channel_child(agent_chan);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        char *argv[] = { "sia-agent", NULL };
        execv("/bin/sia-agent", argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    desktop_channel_parent(agent_chan);
    agent_pid = pid;
    agent_in = in[1];
    agent_out = out[0];
    sb_free(&agent_buf);
    sb_init(&agent_buf);
    return true;
}

static void agent_event(const char *line, size_t len)
{
    struct json *j = json_parse(line, len);
    const char *ev = json_get_str(j, "ev"), *text = json_get_str(j, "text");
    if (!ev) {
        json_free(j);
        return;
    }
    if (!strcmp(ev, "thinking")) {
        const struct json *on = json_get(j, "on");
        agent_thinking = on && on->type == JSON_TRUE;
        invalidate_panel();
    } else if (!strcmp(ev, "tool")) {
        char l[140];
        snprintf(l, sizeof(l), "$ %s", text ? text : "");
        panel_say(l, C_BLUE);
    } else if (!strcmp(ev, "output")) {
        if (text)
            panel_add(text, strlen(text), C_DIM);
    } else if (!strcmp(ev, "text")) {
        if (text && *text)
            panel_say(text, C_TEXT);
    } else if (!strcmp(ev, "confirm")) {
        char l[160];
        snprintf(l, sizeof(l), "Run '%s'?", text ? text : "");
        panel_say(l, C_ACCENT);
        confirm_pending = true;
        if (!strip_focus)
            focus_strip();
    } else if (!strcmp(ev, "error")) {
        panel_say(text ? text : "error", RGB(0xF2, 0x8C, 0x7C));
    } else if (!strcmp(ev, "unavailable")) {
        char l[300];
        snprintf(l, sizeof(l), "The model is not available: %s", text ? text : "");
        panel_say(l, RGB(0xF2, 0x8C, 0x7C));
    } else if (!strcmp(ev, "done")) {
        agent_busy = agent_thinking = confirm_pending = false;
        invalidate_panel();
    }
    invalidate_strip();
    json_free(j);
}

static void agent_readable(void)
{
    char buf[2048];
    long n = read(agent_out, buf, sizeof(buf));
    if (n <= 0) {
        bool was_busy = agent_busy;
        agent_stop();
        if (was_busy)
            panel_say("The assistant stopped unexpectedly.", RGB(0xF2, 0x8C, 0x7C));
        invalidate_strip();
        return;
    }
    sb_putn(&agent_buf, buf, n);
    for (;;) {
        size_t len = 0;
        while (len < agent_buf.len && agent_buf.s[len] != '\n')
            len++;
        if (len == agent_buf.len)
            break;
        agent_event(agent_buf.s, len);
        if (agent_out < 0)
            return;
        memmove(agent_buf.s, agent_buf.s + len + 1, agent_buf.len - len - 1);
        agent_buf.len -= len + 1;
        agent_buf.s[agent_buf.len] = 0;
    }
}

static void switch_ws(int n);

static void strip_submit(void)
{
    strip_buf[strip_len] = 0;
    if (!strip_len)
        return;
    panel_open = true;
    if (!sia_registered) {
        panel_say("No model is registered yet: set one up in the new terminal.", C_ACCENT);
        struct window *w = term_open(false);                 /* sia's first-use setup */
        strip_len = 0;
        strip_focus = false;
        if (w)
            wm_focus(w);
        invalidate_strip();
        return;
    }
    if (agent_busy) {
        panel_say("sia is still working on the last request (Esc interrupts it).", C_DIM);
        return;
    }
    if (!agent_start()) {
        panel_say("Cannot start /bin/sia-agent.", RGB(0xF2, 0x8C, 0x7C));
        return;
    }
    char l[300];
    snprintf(l, sizeof(l), "> %s", strip_buf);
    panel_say(l, C_ACCENT);
    agent_send("ask", "text", strip_buf, 0);
    agent_busy = true;
    strip_len = 0;
    strip_buf[0] = 0;
    invalidate_strip();
}

static void strip_key(const struct input_event *ev)
{
    if (ev->type != EV_KEY || !ev->value)
        return;
    unsigned c = ev->ascii;
    if (confirm_pending) {                             /* y / n / a for the pending command */
        int answer = -1;
        if (c == '\n' || c == '\r' || c == 'y' || c == 'Y')
            answer = 1;
        else if (c == 'n' || c == 'N' || c == 27)
            answer = 0;
        else if (c == 'a' || c == 'A')
            answer = 2;
        if (answer >= 0) {
            confirm_pending = false;
            panel_say(answer ? "(run)" : "(skipped)", C_DIM);
            agent_send("confirm", "answer", NULL, answer);
        }
        return;
    }
    if (c == '\n' || c == '\r') {
        strip_submit();
        return;
    }
    if (c == 27) {
        if (agent_busy && agent_pid > 0) {
            kill(agent_pid, SIGINT);                   /* interrupt the request */
            return;
        }
        if (panel_open) {
            panel_open = false;
            invalidate_panel();
            return;
        }
        strip_len = 0;
        strip_focus = false;
        if (prev_focus && !prev_focus->dead && visible(prev_focus))
            wm_focus(prev_focus);
    } else if (c == '\b' || c == 127) {
        if (strip_len)
            strip_len--;
    } else if (c == 21) {                              /* ^U */
        strip_len = 0;
    } else if (c >= 32 && c < 127 && strip_len < (int)sizeof(strip_buf) - 1) {
        strip_buf[strip_len++] = (char)c;
    } else {
        return;
    }
    strip_buf[strip_len] = 0;
    caret_on = true;
    invalidate_strip();
}

static void focus_strip(void)
{
    if (focus)
        prev_focus = focus;
    strip_focus = true;
    caret_on = true;
    if (focus) {
        struct window *f = focus;
        focus = NULL;
        wm_invalidate(f);
    }
    invalidate_strip();
}

/* ------------------------------------------------------------------ */
/* Windows                                                             */
/* ------------------------------------------------------------------ */

enum { HIT_NONE, HIT_CLOSE, HIT_MENU, HIT_TITLE, HIT_RESIZE, HIT_CONTENT, HIT_BORDER };

static struct rect menu_btn(struct window *w)
{
    return rect_make(w->r.x + 6, w->r.y + (TITLE_H - BTN_H) / 2, BTN_W, BTN_H);
}

static struct rect close_btn(struct window *w)
{
    return rect_make(w->r.x + w->r.w - 6 - BTN_W, w->r.y + (TITLE_H - BTN_H) / 2, BTN_W, BTN_H);
}

static int hit_test(struct window *w, int x, int y)
{
    if (!rect_contains(w->r, x, y))
        return HIT_NONE;
    if (y < w->r.y + TITLE_H) {
        if (rect_contains(close_btn(w), x, y))
            return HIT_CLOSE;
        if (rect_contains(menu_btn(w), x, y))
            return HIT_MENU;
        return HIT_TITLE;
    }
    if (x >= w->r.x + w->r.w - 14 && y >= w->r.y + w->r.h - 14 && !w->maximized)
        return HIT_RESIZE;
    if (rect_contains(wm_content(w), x, y))
        return HIT_CONTENT;
    return HIT_BORDER;
}

static void draw_button(struct surface *s, struct rect b, bool active, bool pressed, bool danger)
{
    color_t c = pressed ? (danger ? C_BAD : RGB(0x23, 0x26, 0x2A)) : active ? RGB(0x36, 0x3A, 0x41) : RGB(0x2C, 0x2F, 0x34);
    gfx_round_rect(s, b.x, b.y, b.w, b.h, 5, c);
    if (!pressed)
        gfx_blend_fill(s, b.x + 4, b.y, b.w - 8, 1, RGB(0xFF, 0xFF, 0xFF), 26);
}

static void draw_window(struct surface *s, struct window *w)
{
    struct rect r = w->r;
    if (rect_empty(rect_intersect(window_bounds(w), s->clip)))
        return;
    bool active = w == focus;
    struct rect clip = s->clip;

    gfx_shadow(s, r, WIN_R, SHADOW, SHADOW_DY, active ? 200 : 140);
    /* title bar with rounded top corners, content area below */
    gfx_round_rect_top(s, r.x, r.y, r.w, TITLE_H, WIN_R, active ? C_TITLEBAR : C_TITLEBAR_I);
    gfx_fill(s, r.x, r.y + TITLE_H, r.w, r.h - TITLE_H, C_CONTENT);
    /* outline: rounded frame clipped so the bottom corners stay square */
    gfx_set_clip(s, rect_intersect(clip, r));
    gfx_round_frame(s, r.x, r.y, r.w, r.h + 2 * WIN_R, WIN_R, C_FACE_DARK);
    gfx_set_clip(s, clip);
    gfx_hline(s, r.x, r.y + r.h - 1, r.w, C_FACE_DARK);
    gfx_blend_fill(s, r.x + WIN_R, r.y + 1, r.w - 2 * WIN_R, 1, RGB(0xFF, 0xFF, 0xFF), active ? 30 : 16);
    gfx_hline(s, r.x + 1, r.y + TITLE_H - 1, r.w - 2, C_FACE_DARK);
    /* workspace band */
    gfx_fill(s, r.x + 1, r.y + TITLE_H, BAND_W, r.h - TITLE_H - 1, active ? ws_color[w->ws] : RGB(0x4A, 0x4E, 0x55));

    /* window menu button (three bars) and close box */
    bool pm = drag_mode == DRAG_BUTTON && drag_win == w && pressed_button == HIT_MENU;
    bool pc = drag_mode == DRAG_BUTTON && drag_win == w && pressed_button == HIT_CLOSE &&
              rect_contains(close_btn(w), mouse_x, mouse_y);
    struct rect mb = menu_btn(w), cb = close_btn(w);
    draw_button(s, mb, active, pm, false);
    color_t glyph = active ? RGB(0xCF, 0xC9, 0xBE) : C_DIM;
    for (int i = 0; i < 3; i++)
        gfx_hline(s, mb.x + 6, mb.y + 6 + i * 4, mb.w - 12, glyph);
    draw_button(s, cb, active, pc, true);
    int cx = cb.x + cb.w / 2, cy = cb.y + cb.h / 2;
    color_t xg = pc ? RGB(0xFF, 0xFF, 0xFF) : glyph;
    gfx_line(s, cx - 4, cy - 4, cx + 4, cy + 4, xg);
    gfx_line(s, cx - 3, cy - 4, cx + 4, cy + 3, xg);
    gfx_line(s, cx + 4, cy - 4, cx - 4, cy + 4, xg);
    gfx_line(s, cx + 3, cy - 4, cx - 4, cy + 3, xg);

    /* title */
    int tx = mb.x + mb.w + 10, tmax = cb.x - 10 - tx;
    gfx_set_clip(s, rect_intersect(clip, rect_make(tx, r.y, MAX(0, tmax), TITLE_H)));
    int ty = r.y + (TITLE_H - FONT_H) / 2;
    if (active)
        gfx_text_bold(s, tx, ty, w->title, C_TEXT);
    else
        gfx_text(s, tx, ty, w->title, C_DIM);
    gfx_set_clip(s, clip);

    /* content, then the resize grip on top of it */
    struct rect c = wm_content(w);
    gfx_set_clip(s, rect_intersect(clip, c));
    if (w->draw)
        w->draw(w, s, c);
    else
        gfx_fill(s, c.x, c.y, c.w, c.h, C_CONTENT);
    if (!w->maximized) {
        int gx = r.x + r.w - 3, gy = r.y + r.h - 3;
        for (int i = 0; i < 3; i++)
            gfx_line(s, gx - 3 - i * 4, gy, gx, gy - 3 - i * 4, C_DIM);
    }
    gfx_set_clip(s, clip);
}

struct window *wm_create(const char *title, int x, int y, int cw, int ch)
{
    if (nwin == MAXWIN)
        return NULL;
    struct window *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->id = next_id++;
    w->ws = cur_ws;
    strlcpy(w->title, title, sizeof(w->title));
    struct rect wa = work_area();
    int ww = cw + 2 + BAND_W, wh = ch + TITLE_H + 1;
    if (x < 0) {                                     /* cascade */
        static int cascade;
        x = wa.x + 40 + (cascade % 8) * 28;
        y = wa.y + 10 + (cascade % 8) * 28;
        cascade++;
    }
    if (x + ww > wa.x + wa.w)
        x = MAX(wa.x, wa.x + wa.w - ww);
    if (x < wa.x)
        x = wa.x;
    if (y + wh > wa.y + wa.h)
        y = MAX(wa.y, wa.y + wa.h - wh);
    if (y < wa.y)
        y = wa.y;
    w->r = rect_make(x, y, ww, wh);
    w->min_w = 180;
    w->min_h = 90;
    zorder[nwin++] = w;
    wm_focus(w);
    wm_invalidate(w);
    invalidate_spine();
    return w;
}

static void raise_window(struct window *w)
{
    int i = 0;
    while (i < nwin && zorder[i] != w)
        i++;
    if (i == nwin)
        return;
    for (; i < nwin - 1; i++)
        zorder[i] = zorder[i + 1];
    zorder[nwin - 1] = w;
}

void wm_focus(struct window *w)
{
    if (focus)
        wm_invalidate(focus);
    focus = w;
    if (w) {
        w->minimized = false;
        raise_window(w);
        wm_invalidate(w);
        if (strip_focus) {
            strip_focus = false;
            invalidate_strip();
        }
    }
}

static struct window *topmost_visible(void)
{
    for (int i = nwin - 1; i >= 0; i--)
        if (visible(zorder[i]))
            return zorder[i];
    return NULL;
}

void wm_close(struct window *w)
{
    w->dead = true;
    wm_invalidate(w);
}

static void reap_windows(void)
{
    for (int i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        if (!w->dead)
            continue;
        wm_invalidate(w);
        invalidate_spine();
        if (w->destroy)
            w->destroy(w);
        for (int k = i; k < nwin - 1; k++)
            zorder[k] = zorder[k + 1];
        nwin--;
        i--;
        if (prev_focus == w)
            prev_focus = NULL;
        if (drag_win == w) {
            drag_win = NULL;
            drag_mode = DRAG_NONE;
        }
        bool was_focus = focus == w;
        if (was_focus)
            focus = NULL;
        free(w);
        if (was_focus)
            wm_focus(topmost_visible());
    }
}

static void minimize(struct window *w)
{
    w->minimized = true;
    wm_invalidate(w);
    if (focus == w) {
        focus = NULL;
        wm_focus(topmost_visible());
    }
    invalidate_spine();
}

static void set_rect(struct window *w, struct rect r)
{
    wm_invalidate(w);
    w->r = r;
    wm_invalidate(w);
    if (w->resized)
        w->resized(w);
}

static void toggle_zoom(struct window *w)
{
    if (w->maximized) {
        w->maximized = false;
        set_rect(w, w->restore);
    } else {
        w->restore = w->r;
        w->maximized = true;
        set_rect(w, work_area());
    }
}

static void switch_ws(int n)
{
    if (n < 0 || n >= NWORKSPACES || n == cur_ws)
        return;
    cur_ws = n;
    if (focus && focus->ws != n)
        focus = NULL;
    wm_focus(topmost_visible());
    invalidate_all();
}

static void send_to_ws(struct window *w, int n)
{
    if (w->ws == n)
        return;
    wm_invalidate(w);
    w->ws = n;
    if (focus == w) {
        focus = NULL;
        wm_focus(topmost_visible());
    }
    invalidate_spine();
}

static void menu_min(void *a)     { minimize(a); }
static void menu_zoom(void *a)    { toggle_zoom(a); }
static void menu_close_w(void *a) { wm_close(a); }
static void menu_ws0(void *a)     { send_to_ws(a, 0); }
static void menu_ws1(void *a)     { send_to_ws(a, 1); }
static void menu_ws2(void *a)     { send_to_ws(a, 2); }
static void menu_ws3(void *a)     { send_to_ws(a, 3); }
static void (*const menu_ws_fn[NWORKSPACES])(void *) = { menu_ws0, menu_ws1, menu_ws2, menu_ws3 };

static void window_menu(struct window *w, int x, int y)
{
    static const char *names[NWORKSPACES] = { "Send to Workspace 1", "Send to Workspace 2", "Send to Workspace 3",
                                              "Send to Workspace 4" };
    struct menu_item items[12];
    int n = 0;
    items[n++] = (struct menu_item){ "Minimize", menu_min, w, -1 };
    items[n++] = (struct menu_item){ w->maximized ? "Restore Size" : "Zoom", menu_zoom, w, -1 };
    items[n++] = (struct menu_item){ NULL, NULL, NULL, -1 };
    for (int i = 0; i < NWORKSPACES; i++)
        if (i != w->ws)
            items[n++] = (struct menu_item){ names[i], menu_ws_fn[i], w, -1 };
    items[n++] = (struct menu_item){ NULL, NULL, NULL, -1 };
    items[n++] = (struct menu_item){ "Close", menu_close_w, w, -1 };
    menu_open(x, y, items, n);
}

static void call_action(void *a) { ((void (*)(void))a)(); }

static void menu_show_window(void *a)
{
    struct window *w = a;
    if (w->ws != cur_ws)
        switch_ws(w->ws);
    wm_focus(w);
}

static void open_gem_menu(void)
{
    struct menu_item items[MENU_MAX];
    int n = 0;
    items[n++] = (struct menu_item){ "Terminal (sia)", call_action, (void *)app_terminal, ICON_TERMINAL };
    items[n++] = (struct menu_item){ "Shell Terminal", call_action, (void *)app_shell_terminal, ICON_TERMINAL };
    items[n++] = (struct menu_item){ "Files", call_action, (void *)open_home, ICON_FOLDER };
    items[n++] = (struct menu_item){ "System Monitor", call_action, (void *)app_monitor, ICON_MONITOR };
    items[n++] = (struct menu_item){ "Network Status", call_action, (void *)app_network, ICON_NETWORK };
    items[n++] = (struct menu_item){ "Clock", call_action, (void *)app_clock, ICON_CLOCK };
    items[n++] = (struct menu_item){ "About SIEOS", call_action, (void *)app_about, ICON_INFO };
    int listed = 0;
    for (int i = nwin - 1; i >= 0 && listed < 10; i--) {
        struct window *w = zorder[i];
        if (w->dead)
            continue;
        if (!listed)
            items[n++] = (struct menu_item){ NULL, NULL, NULL, -1 };
        static char labels[10][40];
        snprintf(labels[listed], sizeof(labels[listed]), "%d  %s%s", w->ws + 1, w->title, w->minimized ? " (hidden)" : "");
        items[n++] = (struct menu_item){ labels[listed], menu_show_window, w, -1 };
        listed++;
    }
    items[n++] = (struct menu_item){ NULL, NULL, NULL, -1 };
    items[n++] = (struct menu_item){ "Log Out", call_action, (void *)do_logout, ICON_LOGOUT };
    struct rect g = gem_rect();
    menu_open(SPINE_X + SPINE_W + 10, g.y - 4, items, n);
    gem_menu_open = true;
    invalidate_spine();
}

static void desktop_menu(int x, int y)
{
    struct menu_item items[] = {
        { "New Terminal (sia)", call_action, (void *)app_terminal, ICON_TERMINAL },
        { "New Shell Terminal", call_action, (void *)app_shell_terminal, ICON_TERMINAL },
        { "Home Folder", call_action, (void *)open_home, ICON_HOME },
        { NULL, NULL, NULL, -1 },
        { "About SIEOS", call_action, (void *)app_about, ICON_INFO },
        { "Log Out", call_action, (void *)do_logout, ICON_LOGOUT },
    };
    menu_open(x, y, items, (int)(sizeof(items) / sizeof(items[0])));
}

static void tile_new(void *a)
{
    tiles[(long)a].open();
}

static void tile_menu(int i, int x, int y)
{
    struct menu_item items[MENU_MAX];
    int n = 0;
    static char label[48];
    snprintf(label, sizeof(label), "New %s", tiles[i].label);
    items[n++] = (struct menu_item){ label, tile_new, (void *)(long)i, tiles[i].icon };
    bool sep = false;
    for (int k = nwin - 1; k >= 0 && n < MENU_MAX - 1; k--) {
        struct window *w = zorder[k];
        if (!tile_matches(i, w))
            continue;
        if (!sep) {
            items[n++] = (struct menu_item){ NULL, NULL, NULL, -1 };
            sep = true;
        }
        items[n++] = (struct menu_item){ w->title, menu_show_window, w, -1 };
    }
    menu_open(x, y, items, n);
}

/* Tile click: open the application, or bring its windows forward in turn. */
static void tile_click(int i)
{
    struct window *top = NULL, *other_ws = NULL;
    for (int k = nwin - 1; k >= 0; k--) {
        struct window *w = zorder[k];
        if (!tile_matches(i, w))
            continue;
        if (w->ws == cur_ws && !top)
            top = w;
        if (w->ws != cur_ws && !other_ws)
            other_ws = w;
    }
    if (!top && other_ws) {
        switch_ws(other_ws->ws);
        wm_focus(other_ws);
        return;
    }
    if (!top) {
        tiles[i].open();
        return;
    }
    if (top == focus && !top->minimized) {
        /* already in front: cycle to the lowest matching window */
        for (int k = 0; k < nwin; k++)
            if (tile_matches(i, zorder[k]) && zorder[k]->ws == cur_ws && zorder[k] != top) {
                wm_focus(zorder[k]);
                return;
            }
        return;
    }
    wm_focus(top);
}

/* ------------------------------------------------------------------ */
/* Services for the desktop control channel (desktop.c)                */
/* ------------------------------------------------------------------ */

int wm_window_list(struct window **out, int max)
{
    int n = 0;
    for (int i = nwin - 1; i >= 0 && n < max; i--)
        if (!zorder[i]->dead)
            out[n++] = zorder[i];
    return n;
}

struct window *wm_find_window(int id)
{
    for (int i = 0; i < nwin; i++)
        if (zorder[i]->id == id && !zorder[i]->dead)
            return zorder[i];
    return NULL;
}

int wm_workspace(void) { return cur_ws; }
void wm_switch_workspace(int n) { switch_ws(n); }
void wm_move_to_workspace(struct window *w, int n) { send_to_ws(w, n); }
void wm_show_window(struct window *w) { menu_show_window(w); }

/* ------------------------------------------------------------------ */
/* Cursor                                                              */
/* ------------------------------------------------------------------ */

static const char *cursor_shape[] = {
    "X...........",
    "XX..........",
    "XOX.........",
    "XOOX........",
    "XOOOX.......",
    "XOOOOX......",
    "XOOOOOX.....",
    "XOOOOOOX....",
    "XOOOOOOOX...",
    "XOOOOOOOOX..",
    "XOOOOOOOOOX.",
    "XOOOOOXXXXXX",
    "XOOXOOX.....",
    "XOX.XOOX....",
    "XX..XOOX....",
    "X....XOOX...",
    ".....XOOX...",
    "......XX....",
};
#define CURSOR_W 12
#define CURSOR_H 18

static struct rect cursor_rect(void)
{
    return rect_make(mouse_x, mouse_y, CURSOR_W, CURSOR_H);
}

static void draw_cursor(struct surface *s)
{
    for (int y = 0; y < CURSOR_H; y++)
        for (int x = 0; x < CURSOR_W; x++) {
            char c = cursor_shape[y][x];
            if (c == 'X')
                gfx_pixel(s, mouse_x + x, mouse_y + y, RGB(0x10, 0x11, 0x13));
            else if (c == 'O')
                gfx_pixel(s, mouse_x + x, mouse_y + y, RGB(0xF2, 0xEE, 0xE6));
        }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void render(void)
{
    if (rect_empty(dirty))
        return;
    struct rect d = dirty;
    dirty = rect_make(0, 0, 0, 0);
    gfx_set_clip(&back, d);
    gfx_blit(&back, d.x, d.y, &bg, d);
    for (int i = 0; i < nwin; i++)
        if (visible(zorder[i]))
            draw_window(&back, zorder[i]);
    draw_spine(&back);
    draw_strip(&back);
    draw_panel(&back);                               /* drops down over the spine and windows */
    draw_menu(&back);
    draw_cursor(&back);

    bool native = fbi.red_pos == 16 && fbi.green_pos == 8 && fbi.blue_pos == 0;
    for (int y = d.y; y < d.y + d.h; y++) {
        uint32_t *src = back.px + y * back.stride + d.x;
        uint32_t *dst = (uint32_t *)((uint8_t *)fbmem + (size_t)y * fbi.pitch) + d.x;
        if (native) {
            memcpy(dst, src, d.w * 4);
        } else {
            for (int x = 0; x < d.w; x++) {
                uint32_t c = src[x];
                dst[x] = (((c >> 16) & 255) << fbi.red_pos) | (((c >> 8) & 255) << fbi.green_pos) |
                         ((c & 255) << fbi.blue_pos);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Input handling                                                      */
/* ------------------------------------------------------------------ */

static struct window *window_at(int x, int y)
{
    for (int i = nwin - 1; i >= 0; i--) {
        struct window *w = zorder[i];
        if (visible(w) && rect_contains(w->r, x, y))
            return w;
    }
    return NULL;
}

static void content_mouse(struct window *w, int kind)
{
    if (!w->mouse)
        return;
    struct rect c = wm_content(w);
    w->mouse(w, mouse_x - c.x, mouse_y - c.y, kind, buttons);
}

static int tile_at(int x, int y)
{
    for (int i = 0; i < NTILES; i++)
        if (rect_contains(tile_rect(i), x, y))
            return i;
    return -1;
}

static int ws_at(int x, int y)
{
    for (int i = 0; i < NWORKSPACES; i++)
        if (rect_contains(ws_rect(i), x, y))
            return i;
    return -1;
}

static void close_menus(void)
{
    menu_close();
    if (gem_menu_open) {
        gem_menu_open = false;
        invalidate_spine();
    }
}

static void press_left(bool dbl)
{
    if (menu.open) {
        int i = menu_hit(mouse_x, mouse_y);
        struct menu_item it = i >= 0 ? menu.items[i] : (struct menu_item){ 0 };
        bool was_gem = gem_menu_open;
        close_menus();
        if (i >= 0 && it.action) {
            it.action(it.arg);
            return;
        }
        if (was_gem && rect_contains(gem_rect(), mouse_x, mouse_y))
            return;                                  /* clicking the gem again closes its menu */
        if (i < 0 && rect_contains(menu.r, mouse_x, mouse_y))
            return;
    }
    /* spine */
    if (rect_contains(spine_rect(), mouse_x, mouse_y)) {
        if (rect_contains(gem_rect(), mouse_x, mouse_y)) {
            open_gem_menu();
            return;
        }
        int t = tile_at(mouse_x, mouse_y);
        if (t >= 0) {
            tile_click(t);
            return;
        }
        int ws = ws_at(mouse_x, mouse_y);
        if (ws >= 0)
            switch_ws(ws);
        return;
    }
    /* strip and its panel */
    if (rect_contains(strip_rect(), mouse_x, mouse_y)) {
        focus_strip();
        return;
    }
    if (panel_open && rect_contains(panel_rect(), mouse_x, mouse_y))
        return;
    if (panel_open && !agent_busy && !confirm_pending) {
        panel_open = false;
        invalidate_panel();
    }
    /* windows */
    struct window *w = window_at(mouse_x, mouse_y);
    if (w) {
        if (w != focus || zorder[nwin - 1] != w)
            wm_focus(w);
        int hit = hit_test(w, mouse_x, mouse_y);
        switch (hit) {
        case HIT_CLOSE:
            drag_mode = DRAG_BUTTON;
            drag_win = w;
            pressed_button = hit;
            wm_invalidate(w);
            break;
        case HIT_MENU: {
            struct rect mb = menu_btn(w);
            drag_mode = DRAG_BUTTON;
            drag_win = w;
            pressed_button = hit;
            wm_invalidate(w);
            window_menu(w, mb.x, mb.y + mb.h + 4);
            break;
        }
        case HIT_TITLE:
            if (dbl) {                               /* double-click the title: zoom */
                toggle_zoom(w);
                break;
            }
            /* fall through */
        case HIT_BORDER:
            if (!w->maximized) {
                drag_mode = DRAG_MOVE;
                drag_win = w;
                drag_dx = mouse_x - w->r.x;
                drag_dy = mouse_y - w->r.y;
            }
            break;
        case HIT_RESIZE:
            drag_mode = DRAG_RESIZE;
            drag_win = w;
            drag_start = w->r;
            drag_dx = mouse_x;
            drag_dy = mouse_y;
            break;
        case HIT_CONTENT:
            drag_mode = DRAG_CONTENT;
            drag_win = w;
            content_mouse(w, dbl ? MOUSE_DOUBLE : MOUSE_DOWN);
            break;
        }
        return;
    }
    /* empty desktop: typing now goes to the strip */
    focus_strip();
}

static void release_left(void)
{
    if (drag_mode == DRAG_BUTTON && drag_win) {
        struct window *w = drag_win;
        int b = pressed_button;
        drag_mode = DRAG_NONE;
        wm_invalidate(w);
        if (b == HIT_CLOSE && rect_contains(close_btn(w), mouse_x, mouse_y))
            wm_close(w);
    }
    if (drag_mode == DRAG_CONTENT && drag_win)
        content_mouse(drag_win, MOUSE_UP);
    drag_mode = DRAG_NONE;
    drag_win = NULL;
    pressed_button = -1;
}

static void mouse_moved(void)
{
    if (menu.open) {
        int h = menu_hit(mouse_x, mouse_y);
        if (h != menu.hover) {
            if (menu.hover >= 0)
                wm_invalidate_rect(menu_item_rect(menu.hover));
            if (h >= 0)
                wm_invalidate_rect(menu_item_rect(h));
            menu.hover = h;
        }
    }
    int t = drag_mode == DRAG_NONE ? tile_at(mouse_x, mouse_y) : -1;
    int ws = drag_mode == DRAG_NONE ? ws_at(mouse_x, mouse_y) : -1;
    if (t != hover_tile || ws != hover_ws) {
        hover_tile = t;
        hover_ws = ws;
        invalidate_spine();
    }
    switch (drag_mode) {
    case DRAG_MOVE: {
        struct rect r = drag_win->r;
        r.x = mouse_x - drag_dx;
        r.y = MAX(0, MIN(mouse_y - drag_dy, screen_h - TITLE_H));
        set_rect(drag_win, r);
        break;
    }
    case DRAG_RESIZE: {
        struct rect r = drag_start;
        r.w = MAX(drag_win->min_w, drag_start.w + mouse_x - drag_dx);
        r.h = MAX(drag_win->min_h, drag_start.h + mouse_y - drag_dy);
        set_rect(drag_win, r);
        break;
    }
    case DRAG_CONTENT:
        content_mouse(drag_win, MOUSE_MOVE);
        break;
    case DRAG_BUTTON:
        wm_invalidate(drag_win);
        break;
    }
}

static void cycle_focus(void)
{
    for (int i = 0; i < nwin; i++)                    /* the lowest visible window comes to the front */
        if (visible(zorder[i])) {
            wm_focus(zorder[i]);
            return;
        }
}

int abs_int(int v)
{
    return v < 0 ? -v : v;
}

static void handle_event(const struct input_event *ev)
{
    if (ev->type == EV_MOUSE || ev->type == EV_MOUSE_ABS) {
        struct rect old = cursor_rect();
        int ox = mouse_x, oy = mouse_y;
        if (ev->type == EV_MOUSE_ABS) {
            mouse_x = (int)((long)ev->dx * screen_w / 65536);
            mouse_y = (int)((long)ev->dy * screen_h / 65536);
        } else {
            mouse_x += ev->dx;
            mouse_y += ev->dy;
        }
        mouse_x = MAX(0, MIN(screen_w - 1, mouse_x));
        mouse_y = MAX(0, MIN(screen_h - 1, mouse_y));
        unsigned prev = buttons;
        buttons = ev->buttons;
        if (mouse_x != ox || mouse_y != oy) {
            wm_invalidate_rect(old);
            wm_invalidate_rect(cursor_rect());
            mouse_moved();
        }
        if ((buttons & 1) && !(prev & 1)) {
            long now = uptime_ms();
            bool dbl = now - last_click_ms < DOUBLE_CLICK_MS &&
                       abs_int(mouse_x - last_click_x) < 5 && abs_int(mouse_y - last_click_y) < 5;
            last_click_ms = dbl ? 0 : now;
            last_click_x = mouse_x;
            last_click_y = mouse_y;
            press_left(dbl);
        }
        if (!(buttons & 1) && (prev & 1))
            release_left();
        if ((buttons & 2) && !(prev & 2)) {
            close_menus();
            struct window *w = window_at(mouse_x, mouse_y);
            int t = tile_at(mouse_x, mouse_y);
            if (w)
                window_menu(w, mouse_x, mouse_y);
            else if (t >= 0)
                tile_menu(t, SPINE_X + SPINE_W + 10, tile_rect(t).y);
            else if (!rect_contains(spine_rect(), mouse_x, mouse_y) && !rect_contains(strip_rect(), mouse_x, mouse_y))
                desktop_menu(mouse_x, mouse_y);
        }
        return;
    }
    if (ev->type == EV_KEY) {
        key_mods = ev->mods;
        if (ev->code == 0x2A || ev->code == 0x36)          /* shift keys themselves */
            key_mods = ev->value ? (key_mods | MOD_SHIFT) : (key_mods & ~MOD_SHIFT);
    }
    if (ev->type == EV_KEY && ev->value) {
        unsigned mods = ev->mods;
        if ((mods & MOD_CTRL) && !(mods & MOD_ALT) && ev->code == 0x39) {   /* Ctrl+Space */
            if (strip_focus) {
                strip_focus = false;
                invalidate_strip();
                if (prev_focus && !prev_focus->dead && visible(prev_focus))
                    wm_focus(prev_focus);
            } else {
                focus_strip();
            }
            return;
        }
        if ((mods & MOD_CTRL) && (mods & MOD_ALT)) {
            if (ev->code >= 0x02 && ev->code < 0x02 + NWORKSPACES) {      /* Ctrl+Alt+1..4 */
                switch_ws(ev->code - 0x02);
                return;
            }
            if (ev->code == KEY_LEFT || ev->code == KEY_RIGHT) {
                switch_ws((cur_ws + (ev->code == KEY_LEFT ? NWORKSPACES - 1 : 1)) % NWORKSPACES);
                return;
            }
        }
        if ((mods & MOD_ALT) && ev->ascii == '\t') {
            cycle_focus();
            return;
        }
        if ((mods & MOD_ALT) && ev->code == 0x3E) {        /* Alt+F4 */
            if (focus)
                wm_close(focus);
            return;
        }
        if (menu.open && ev->ascii == 27) {
            close_menus();
            return;
        }
    }
    if (strip_focus || !focus) {
        if (ev->type == EV_KEY && ev->value && ev->ascii && !strip_focus)
            focus_strip();                              /* typing on the empty desktop */
        if (strip_focus)
            strip_key(ev);
        return;
    }
    if (focus->key && !focus->minimized)
        focus->key(focus, ev);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
    (void)sig;
    running = false;
}

int main(void)
{
    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        perror("facet: /dev/fb0");
        return 1;
    }
    if (ioctl(fb_fd, FBIOGET_INFO, &fbi) < 0) {
        perror("facet: framebuffer");
        return 1;
    }
    ev_fd = open("/dev/events", O_RDONLY);
    if (ev_fd < 0) {
        perror("facet: /dev/events");
        return 1;
    }
    fbmem = fbmap(fb_fd);
    if (!fbmem) {
        perror("facet: fbmap");
        return 1;
    }
    screen_w = fbi.width;
    screen_h = fbi.height;
    back.w = bg.w = screen_w;
    back.h = bg.h = screen_h;
    back.stride = bg.stride = screen_w;
    back.px = malloc((size_t)screen_w * screen_h * 4);
    bg.px = malloc((size_t)screen_w * screen_h * 4);
    if (!back.px || !bg.px) {
        dprintf(STDERR_FILENO, "facet: out of memory\n");
        return 1;
    }
    struct passwd *pw = getpwuid(geteuid());
    strlcpy(desktop_user, pw ? pw->pw_name : "user", sizeof(desktop_user));
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    render_background();
    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;
    sample_system();
    app_about();
    app_terminal();                      /* created last: has the keyboard focus */
    invalidate_all();

    long last_tick = uptime_ms(), last_sec = 0, last_blink = 0;
    while (running) {
        render();
        struct pollfd pfd[MAXWIN + 1 + 1 + 32];
        struct window *owner[MAXWIN + 1];
        int n = 0;
        pfd[n].fd = ev_fd;
        pfd[n].events = POLLIN;
        owner[n++] = NULL;
        for (int i = 0; i < nwin; i++) {
            int fd = zorder[i]->pollfd ? zorder[i]->pollfd(zorder[i]) : -1;
            if (fd >= 0 && !zorder[i]->dead) {
                pfd[n].fd = fd;
                pfd[n].events = POLLIN;
                owner[n++] = zorder[i];
            }
        }
        int nwinfd = n, agent_slot = -1, chan_ids[32];
        if (agent_out >= 0) {
            agent_slot = n;
            pfd[n].fd = agent_out;
            pfd[n++].events = POLLIN;
        }
        int nchan = desktop_poll_fds(pfd + n, (int)(sizeof(pfd) / sizeof(pfd[0])) - n, chan_ids);
        int chan_base = n;
        n += nchan;
        int r = poll(pfd, n, agent_thinking ? 100 : 100);
        if (r > 0) {
            if (pfd[0].revents & POLLIN) {
                struct input_event evs[32];
                long got = read(ev_fd, evs, sizeof(evs));
                for (long i = 0; i < got / (long)sizeof(struct input_event); i++)
                    handle_event(&evs[i]);
            }
            for (int i = 1; i < nwinfd; i++)
                if (pfd[i].revents && owner[i]->readable && !owner[i]->dead)
                    owner[i]->readable(owner[i]);
            if (agent_slot >= 0 && pfd[agent_slot].revents && agent_out >= 0)
                agent_readable();
            for (int i = 0; i < nchan; i++)
                if (pfd[chan_base + i].revents)
                    desktop_readable(chan_ids[i]);
        }
        if (agent_thinking)
            invalidate_panel();                          /* animate the dots */
        long now = uptime_ms();
        if (now - last_tick >= 250) {
            last_tick = now;
            for (int i = 0; i < nwin; i++)
                if (zorder[i]->tick && !zorder[i]->dead)
                    zorder[i]->tick(zorder[i]);
        }
        if (strip_focus && now - last_blink >= 500) {
            last_blink = now;
            caret_on = !caret_on;
            invalidate_strip();
        }
        if (now / 1000 != last_sec) {
            last_sec = now / 1000;
            sample_system();
            invalidate_strip();
            wm_invalidate_rect(clock_rect());
        }
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        reap_windows();
    }

    agent_stop();
    for (int i = 0; i < nwin; i++)
        zorder[i]->dead = true;
    reap_windows();
    close(ev_fd);
    return 0;
}
