/*
 * settings.c - The Settings window (docs/desktop.md): a window of the
 * desktop, like the editor, on every machine:
 *
 *   Screen     its size, where the screen can take any (QEMU's VGA and
 *              ramfb, the Pi 4's firmware); a firmware's fixed screen says
 *              its size. A new size shows at once and goes back by itself
 *              after 15 s unless kept (a monitor that cannot show it is
 *              never a dead end). Kept in /etc/display.conf.
 *   Look       Phosphor (a green monitor), Amber 1984 or Glass (draw.c),
 *              and the size of everything, 50% to 400% (draw.c's zoom):
 *              the user's own (~/.look), applied at login.
 *   Mouse      the pointer's speed, left- or right-handed buttons, the
 *              wheel's direction: the user's own (~/.mouse), applied at login.
 *   Assistant  sia's model: the local one, or an online one with an
 *              OpenAI-compatible API (server URL, model name, API key). A
 *              system setting: root's password is asked unless root is
 *              logged in. Kept in /etc/sia.conf, the key in /etc/sia.key
 *              (root only); a running sia restarts with them.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "atlas.h"
#include "mk/crypto.h"                   /* wipe */

#define TOP 48                           /* the window's title strip */
#define ROW 54                           /* a row of buttons or a field */
enum { S_BODY, S_SIZE = 10, S_SPEED = 30, S_HAND = 40, S_WHEEL = 50, S_SIA = 60, S_FIELD = 70,
       S_APPLY = 80, S_KEEP = 90, S_BACK, S_LOOK = 100, S_ZOOM = 110 };
enum { F_URL, F_MODEL, F_KEY, F_ROOTPW, NF };

static const int sizes[][2] = { { 1280, 720 }, { 1366, 768 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 } };
#define NSIZE (int)(sizeof sizes / sizeof sizes[0])
static const int zooms[] = { 50, 75, 100, 125, 150, 200, 300, 400 };
#define NZOOM 8
static const int speeds[] = { 50, 100, 150, 200 };
static const char *speed_names[] = { "Slow", "Normal", "Fast", "Faster" };

static long port;
static int scroll, field = -1, remote, key_set;
static char fields[NF][200], msg[100];
static int msg_err;
static struct { int on, w, h; } back;    /* a new size not kept yet: the old one */
static volatile unsigned gen;            /* (which countdown is the current one) */

void settings_init(long atlas_port) { port = atlas_port; }

static int starts(const char *s, const char *with) { while (*with) if (*s++ != *with++) return 0; return 1; }

/* A "key=value" line of a small settings file -> val (empty if none). */
static void conf_val(const char *text, const char *key, char *val, size_t cap)
{
    size_t kl = strlen(key);
    val[0] = 0;
    for (const char *p = text; p && *p; ) {
        if (starts(p, key) && p[kl] == '=') {
            size_t i = 0;
            for (p += kl + 1; *p && *p != '\n' && i + 1 < cap; ) val[i++] = *p++;
            val[i] = 0;
            return;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
}

/* Opened: what sia uses now. */
void settings_open(void)
{
    size_t n = 0;
    char *c = file_get("/etc/sia.conf", &n), v[16];
    conf_val(c, "backend", v, sizeof v);
    remote = !strcmp(v, "remote");
    conf_val(c, "url", fields[F_URL], sizeof fields[0]);
    conf_val(c, "api_model", fields[F_MODEL], sizeof fields[0]);
    free(c);
    siefs_stat_t st;
    key_set = !fs_stat("/etc/sia.key", &st, 1) && st.size > 0;
    wipe(fields[F_KEY], sizeof fields[0]);
    wipe(fields[F_ROOTPW], sizeof fields[0]);
    field = -1; scroll = 0; msg[0] = 0;
    say("atlas: settings open\n");
}
void settings_close(void)
{
    wipe(fields[F_KEY], sizeof fields[0]);
    wipe(fields[F_ROOTPW], sizeof fields[0]);
    field = -1;
}

/* ---- The screen's size: a new one, then 15 s to keep it. */
static void countdown(void *arg)
{
    unsigned g = (unsigned)(uintptr_t)arg;
    sys_sleep(15000000000ULL);
    if (g != gen) return;
    msg_t m = { .w = { ATLAS_REVERT } };
    ipc_call(port, &m);
}
static void set_size(int w, int h)
{
    int ow, oh;
    atlas_screen(&ow, &oh);
    if (w == ow && h == oh) return;
    int was = back.on;
    if (!back.on) { back.w = ow; back.h = oh; }
    back.on = 1;                                         /* (before the new picture: it shows Keep) */
    if (atlas_set_size(w, h)) { back.on = was; strlcpy(msg, "This screen cannot take that size.", sizeof msg); msg_err = 1; return; }
    gen++;
    thread_start(countdown, (void *)(uintptr_t)gen, 4096);
    say("atlas: screen size %d x %d (back to %d x %d in 15 s unless kept)\n", w, h, back.w, back.h);
}
void settings_revert(void)                /* ATLAS_REVERT: the countdown ended */
{
    if (!back.on) return;
    back.on = 0;
    atlas_set_size(back.w, back.h);
    say("atlas: screen size back to %d x %d\n", back.w, back.h);
    redraw();
}
static void keep(void)
{
    int w, h;
    atlas_screen(&w, &h);
    back.on = 0; gen++;
    char line[32], *p = line + strlcpy(line, "size=", sizeof line);
    p += num(p, w); *p++ = 'x'; p += num(p, h); *p++ = '\n'; *p = 0;
    long f = fs_open("/etc/display.conf", FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
    if (f >= 0) { fs_write(f, 0, line, strlen(line)); fs_close(f); }
    say("atlas: screen size %d x %d kept\n", w, h);
}

/* ---- The assistant's model: /etc/sia.conf (these keys replaced), /etc/sia.key. */
static long conf_set(const char *const *kv, int n)
{
    static char out[2048];
    size_t len = 0, fl = 0;
    char *f = file_get("/etc/sia.conf", &fl), *p = f;
    while (p && *p) {                                    /* the other lines, as they were */
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        int mine = 0;
        for (int i = 0; i < n; i++) { size_t kl = strchr(kv[i], '=') - kv[i] + 1; if (l >= kl && !memcmp(p, kv[i], kl)) mine = 1; }
        if (!mine && l && len + l + 2 < sizeof out) { memcpy(out + len, p, l); len += l; out[len++] = '\n'; }
        p = e ? e + 1 : p + l;
    }
    free(f);
    for (int i = 0; i < n; i++) {
        size_t l = strlen(kv[i]);
        if (len + l + 2 >= sizeof out) return -ENOSPC;
        memcpy(out + len, kv[i], l); len += l; out[len++] = '\n';
    }
    long h = fs_open("/etc/sia.conf.new", FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
    if (h < 0) return h;
    long r = fs_write(h, 0, out, len);
    fs_close(h);
    return r < 0 ? r : fs_rename("/etc/sia.conf.new", "/etc/sia.conf");
}

static void apply_sia(void)
{
    msg_err = 1;
    const char *u = fields[F_URL];
    if (remote && !starts(u, "http://") && !starts(u, "https://")) { strlcpy(msg, "The server URL starts with http:// or https://", sizeof msg); return; }
    if (remote && !fields[F_MODEL][0]) { strlcpy(msg, "Give the model's name at that service.", sizeof msg); return; }
    if (atlas_uid() != 0) {                              /* a system setting: root says yes */
        if (!fields[F_ROOTPW][0]) { strlcpy(msg, "Root's password is needed to change this.", sizeof msg); field = F_ROOTPW; return; }
        char req[160], *p = req;
        p += strlcpy(p, "root", 8) + 1;
        p += strlcpy(p, fields[F_ROOTPW], 128) + 1;
        msg_t m = { .w = { AUTH_CHECK }, .sbuf = req, .slen = p - req };
        strlcpy(msg, "checking...", sizeof msg);
        redraw();
        long r = atlas_auth(&m);
        wipe(req, sizeof req);
        wipe(fields[F_ROOTPW], sizeof fields[0]);
        if (r < 0) { strlcpy(msg, "Wrong password for root.", sizeof msg); say("atlas: settings: root's password refused\n"); return; }
    }
    char b[24], url[220], model[80];
    strlcpy(b, remote ? "backend=remote" : "backend=local", sizeof b);
    strlcpy(url + strlcpy(url, "url=", sizeof url), u, sizeof url - 4);
    strlcpy(model + strlcpy(model, "api_model=", sizeof model), fields[F_MODEL], sizeof model - 10);
    const char *kv[3] = { b, url, model };
    long r = conf_set(kv, remote ? 3 : 1);
    if (!r && remote && fields[F_KEY][0]) {              /* a new key (empty: the old one stays) */
        long h = fs_open("/etc/sia.key", FS_WRONLY | FS_CREAT | FS_TRUNC, 0600);
        if (h < 0) r = h;
        else { fs_chmod("/etc/sia.key", 0600); r = fs_write(h, 0, fields[F_KEY], strlen(fields[F_KEY])); fs_close(h); if (r > 0) r = 0; key_set = 1; }
    }
    wipe(fields[F_KEY], sizeof fields[0]);
    if (r) { strlcpy(msg, "Could not save the settings.", sizeof msg); return; }
    if (port_find("sia") > 0) {                          /* a running sia: restart it with them */
        long sia = 0;
        msg_t m = { .w = { SIA_CONFIG }, .sbuf = b, .slen = strlen(b) + 1 };
        call_named(&sia, "sia", &m, 0);
    }
    msg_err = 0;
    strlcpy(msg, remote ? "Saved: sia uses the online model." : "Saved: sia uses the model on this computer.", sizeof msg);
    say("atlas: settings: sia %s\n", remote ? "online" : "local");
    field = -1;
}

/* ---- Drawing: rows from the top (scrolled), only those wholly in view. */
static int X0, Y0, X1, Y1, cy, lx, cx;
static int fits(int h) { return cy >= Y0 + TOP + 6 && cy + h <= Y1 - 6; }
static void heading(const char *s)
{
    cy += 14;
    if (fits(34)) {                                       /* (a title: bigger, bold) */
        dl_text(lx, cy, s, strlen(s), ST, C_ACC, 0, 0, SW, SH);
        dl_text(lx + 1, cy, s, strlen(s), ST, C_ACC, 0, 0, SW, SH);
    }
    cy += 44;
}
static void row_label(const char *s)
{
    if (cx == lx) { if (fits(30)) label(lx, cy, s, 2, C_DIM); cy += 34; }
    else if (fits(42)) label(lx, cy + 10, s, 2, C_DIM);
}
static void choices(const char *name, const char *const *s, int n, int on, int base)
{
    row_label(name);
    int x = cx;
    for (int i = 0; i < n; i++) {
        int w = tw(strlen(s[i]), 2) + 28;
        if (x > cx && x + w > X1 - 24) { x = cx; cy += ROW; }
        if (fits(42)) pill(x, cy, s[i], i == on, H_SET, base + i);
        if (fits(42) && atlas_debug) say("atlas: dbg set %d at %d %d\n", base + i, x + w / 2, cy + 21);
        x += w + 10;
    }
    cy += ROW;
}
static void text_field(const char *name, int f, int secret, const char *empty)
{
    row_label(name);
    if (fits(44)) {
        int on = field == f, n = strlen(fields[f]);
        dl_rect(cx, cy, X1 - 24, cy + 44, 10, C_BG);
        dl_frame(cx, cy, X1 - 24, cy + 44, 10, on ? 2 : 1, on ? C_ACC : C_LINE);
        char st[64];
        int k = n < (int)sizeof st - 1 ? n : (int)sizeof st - 1, chars = (X1 - 24 - cx - 32) / KW;
        memset(st, '*', k);
        const char *t = secret ? st : fields[f];
        int len = secret ? k : n, from = len > chars ? len - chars : 0;   /* (the end, when long) */
        if (len) dl_text(cx + 14, cy + 11, t + from, len - from, S(2), C_TXT, cx, cy, X1 - 34, cy + 44);
        else if (empty) dl_text(cx + 14, cy + 11, empty, strlen(empty), S(2), C_DIM, cx, cy, X1 - 34, cy + 44);
        if (on) { int w = tw(len - from, 2); dl_rect(cx + 16 + w, cy + 10, cx + 26 + w, cy + 34, 0, C_ACC); }
        hitbox(cx, cy, X1 - 24, cy + 44, H_SET, S_FIELD + f);
        if (atlas_debug) say("atlas: dbg set %d at %d %d\n", S_FIELD + f, (cx + X1) / 2, cy + 22);
    }
    cy += ROW;
}

void settings_draw(int x0, int y0, int x1, int y1, int focused)
{
    (void)focused;
    X0 = x0; Y0 = y0; X1 = x1; Y1 = y1;
    dl_rect(x0 + 2, y0 + TOP, x1 - 2, y1 - 2, 0, C_BG);
    hitbox(x0, y0 + TOP, x1, y1, H_SET, S_BODY);
    lx = x0 + 24; cx = x1 - x0 >= 760 ? x0 + 250 : lx;
    cy = y0 + TOP + 4 - scroll;

    heading("SCREEN");
    int w, h, rs = atlas_screen(&w, &h);
    char t[64], *p;
    if (rs) {
        const char *names[NSIZE];
        static char nb[NSIZE][16];
        int on = -1;
        for (int i = 0; i < NSIZE; i++) {
            p = nb[i]; p += num(p, sizes[i][0]); p += strlcpy(p, " x ", 4); p[num(p, sizes[i][1])] = 0;
            names[i] = nb[i];
            if (sizes[i][0] == w && sizes[i][1] == h) on = i;
        }
        choices("Size", names, NSIZE, on, S_SIZE);
        if (back.on) {
            p = t + strlcpy(t, "Keep this size? Back to ", sizeof t);
            p += num(p, back.w); p += strlcpy(p, " x ", 4); p += num(p, back.h); strlcpy(p, " in 15 s.", 12);
            if (fits(30)) label(lx, cy, t, 2, C_TXT);
            cy += 38;
            static const char *kb[2] = { "Keep", "Go back" };
            int x = cx;
            for (int i = 0; i < 2; i++) {
                if (fits(42)) x = pill(x, cy, kb[i], i == 0, H_SET, S_KEEP + i) + 10;
                if (fits(42) && atlas_debug) say("atlas: dbg set %d at %d %d\n", S_KEEP + i, x - 60, cy + 21);
            }
            cy += ROW;
        }
    } else {
        row_label("Size");
        p = t; p += num(p, w); p += strlcpy(p, " x ", 4); p += num(p, h); strlcpy(p, ", set by the firmware", 24);
        if (fits(42)) label(cx, cy + 10, t, 2, C_TXT);
        cy += ROW;
    }

    heading("LOOK");
    static const char *looks[NLOOK] = { "Phosphor", "Amber 1984", "Glass" };
    choices("Desktop", looks, NLOOK, atlas_look_get(), S_LOOK);
    static const char *zn[NZOOM] = { "50%", "75%", "100%", "125%", "150%", "200%", "300%", "400%" };
    int zk = -1;
    for (int i = 0; i < NZOOM; i++) if (zooms[i] == atlas_zoom_get()) zk = i;
    choices("Size", zn, NZOOM, zk, S_ZOOM);

    heading("MOUSE");
    int sp, sw, rv, k = 1;
    atlas_mouse_get(&sp, &sw, &rv);
    for (int i = 0; i < 4; i++) if (speeds[i] == sp) k = i;
    choices("Pointer speed", speed_names, 4, k, S_SPEED);
    static const char *hands[2] = { "Right-handed", "Left-handed" }, *wheels[2] = { "Normal", "Reversed" };
    choices("Buttons", hands, 2, sw, S_HAND);
    choices("Wheel", wheels, 2, rv, S_WHEEL);

    heading("ASSISTANT");
    static const char *where[2] = { "On this computer", "Online (OpenAI API)" };
    choices("Model", where, 2, remote, S_SIA);
    if (remote) {
        text_field("Server URL", F_URL, 0, "https://api.openai.com/v1");
        text_field("Model name", F_MODEL, 0, "gpt-4o-mini");
        text_field("API key", F_KEY, 1, key_set ? "(kept: type to replace)" : "(none)");
    }
    if (atlas_uid() != 0) text_field("Root's password", F_ROOTPW, 1, "needed to change the model");
    if (fits(42)) {
        int ax = pill(cx, cy, "Apply", 1, H_SET, S_APPLY);
        if (atlas_debug) say("atlas: dbg set %d at %d %d\n", S_APPLY, cx + 40, cy + 21);
        if (msg[0]) dl_text(ax + 20, cy + 10, msg, strlen(msg), S(2), msg_err ? C_ERR : C_ACC, ax, cy, X1 - 20, cy + 42);
    }
    cy += ROW;
    panel_status(t, sizeof t);
    if (fits(30)) label(lx, cy, t, 2, C_DIM);
    cy += 40;
    int bottom = cy + scroll - (y1 - 6);                 /* how far it may scroll */
    if (scroll > 0 && bottom < scroll) scroll = bottom > 0 ? bottom : 0;
}

void settings_wheel(int w)
{
    scroll += w * 54;
    if (scroll < 0) scroll = 0;
}

int settings_click(int kind, int arg)
{
    if (kind != H_SET) return 0;
    if (arg >= S_SIZE && arg < S_SIZE + NSIZE) set_size(sizes[arg - S_SIZE][0], sizes[arg - S_SIZE][1]);
    else if (arg == S_KEEP) keep();
    else if (arg == S_BACK) { gen++; settings_revert(); back.on = 0; }
    else if (arg >= S_SPEED && arg < S_SPEED + 4) { int s, w, r; atlas_mouse_get(&s, &w, &r); atlas_mouse(speeds[arg - S_SPEED], w, r); }
    else if (arg >= S_HAND && arg < S_HAND + 2) { int s, w, r; atlas_mouse_get(&s, &w, &r); atlas_mouse(s, arg - S_HAND, r); }
    else if (arg >= S_WHEEL && arg < S_WHEEL + 2) { int s, w, r; atlas_mouse_get(&s, &w, &r); atlas_mouse(s, w, arg - S_WHEEL); }
    else if (arg >= S_LOOK && arg < S_LOOK + NLOOK) atlas_look(arg - S_LOOK);
    else if (arg >= S_ZOOM && arg < S_ZOOM + NZOOM) atlas_zoom(zooms[arg - S_ZOOM]);
    else if (arg >= S_SIA && arg < S_SIA + 2) { remote = arg - S_SIA; msg[0] = 0; }
    else if (arg >= S_FIELD && arg < S_FIELD + NF) field = arg - S_FIELD;
    else if (arg == S_APPLY) apply_sia();
    else if (arg == S_BODY) field = -1;
    return 1;
}

/* Keys: typing goes to the field in focus; Tab: the next field; Enter: Apply. */
int settings_key(int c)
{
    if (field < 0) return 0;
    char *f = fields[field];
    size_t n = strlen(f);
    if (c == '\t') {                                    /* the next field shown (if any other) */
        for (int k = 0; k < NF; k++) {
            field = (field + 1) % NF;
            if (!((!remote && field < F_ROOTPW) || (field == F_ROOTPW && atlas_uid() == 0))) break;
        }
    }
    else if (c == 27) field = -1;
    else if (c == '\n') apply_sia();
    else if (c == '\b') { if (n) f[n - 1] = 0; }
    else if (c >= 32 && c < 127 && n + 1 < sizeof fields[0]) { f[n] = c; f[n + 1] = 0; }
    else return 0;
    return 1;
}
