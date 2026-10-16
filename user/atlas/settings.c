/*
 * settings.c - The Settings window (docs/desktop.md): a window of the
 * desktop, like the editor, on every machine:
 *
 *   Screen     its size, where the screen can take any (QEMU's VGA and
 *              ramfb, the Pi 4's firmware, the Intel display and its panel's
 *              own size); a firmware's fixed screen says its size. A new
 *              size shows at once and goes back by itself after 15 s unless
 *              kept (a monitor that cannot show it is never a dead end); the
 *              panel's brightness (the Intel display). Kept in /etc/display.conf.
 *   Look       Phosphor (a green monitor), Amber 1984, Glass or BeOS (draw.c),
 *              and the size of everything, 50% to 400% (draw.c's zoom):
 *              the user's own (~/.look), applied at login.
 *   Mouse      the pointer's speed, left- or right-handed buttons, the
 *              wheel's direction: the user's own (~/.mouse), applied at login.
 *   Sound      the volume, or mute (the audio server's, docs/audio.md).
 *   Bluetooth  on or off, a scan, the devices found and bonded; Pair shows
 *              the 6 digits to compare (Yes / No) or to type on a keyboard;
 *              Forget (docs/bluetooth.md). Root's password unless root.
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
       S_APPLY = 80, S_KEEP = 90, S_BACK, S_LOOK = 100, S_ZOOM = 110, S_VOL = 120, S_BRIGHT = 130, S_OUT = 140,
       S_WSCAN = 200, S_WNET = 210, S_WJOIN = 250, S_WFORGET,
       S_BPOW = 260, S_BSCAN = 262, S_BPAIR, S_BFORGET, S_BYES, S_BNO, S_BDEV = 270 };
enum { F_URL, F_MODEL, F_KEY, F_ROOTPW, F_WPASS, NF };

static const int sizes[][2] = { { 1280, 720 }, { 1366, 768 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 } };
#define NSIZE (int)(sizeof sizes / sizeof sizes[0])
static int nat[2];                       /* the screen's own size, if it has one (screen_native) */
static const int brights[] = { 10, 25, 50, 75, 100 };
static const int zooms[] = { 50, 75, 100, 125, 150, 200, 300, 400 };
#define NZOOM 8
static const int speeds[] = { 50, 100, 150, 200 };
static const char *speed_names[] = { "Slow", "Normal", "Fast", "Faster" };

static long port;
static int scroll, field = -1, remote, key_set;
static char fields[NF][200], msg[100];
static int msg_err, vol = -1;           /* vol: AUDIO_VOL's answer; -1: not asked yet, -2: no sound card */
static int nouts, osel;                  /* the sound outputs (AUDIO_OUT), the one playing */
static char onames[96];
static const char *outs[5];
static struct { int on, w, h; } back;    /* a new size not kept yet: the old one */
static volatile unsigned gen;            /* (which countdown is the current one) */

/* The Network section's own state: the chip's status, the last scan. */
static int wifi_have, wifi_sel = -1, wifi_n;
static wifi_status_t wstat;
static wifi_net_t wnets[WIFI_MAXNET];

/* The Bluetooth section's: the server's status, its devices, the one picked. */
static int bt_have, bt_sel = -1, bt_n;
static volatile int bt_busy, bt_ticking;
static bt_status_t bst;
static bt_dev_t bdev[BT_MAXDEV];

void settings_init(long atlas_port) { port = atlas_port; }

/* The Bluetooth server, only if it runs (none: no adapter). */
static long bt_call(msg_t *m)
{
    long p = port_find("bt");
    if (p <= 0) return -ENOENT;
    long e = ipc_call(p, m);
    return e < 0 ? e : (long)m->w[0];
}
static void bt_refresh(void)
{
    msg_t m = { .w = { BT_STATUS }, .rbuf = &bst, .rlen = sizeof bst };
    bt_have = bt_call(&m) == 0 && m.rlen == sizeof bst;
    msg_t l = { .w = { BT_LIST }, .rbuf = bdev, .rlen = sizeof bdev };
    long n = bt_have ? bt_call(&l) : 0;
    bt_n = n > 0 ? (int)n : 0;
    if (bt_sel >= bt_n) bt_sel = -1;
    bt_busy = bt_have && (bst.scanning || bst.pair == BTP_BUSY || bst.pair == BTP_CONFIRM || bst.pair == BTP_PASSKEY);
}
static void bt_ticker(void *a)                           /* twice a second while something goes on */
{
    (void)a;
    for (int i = 0; i < 300 && (bt_busy || !i); i++) { sys_sleep(500000000); msg_t m = { .w = { ATLAS_BT } }; ipc_call(port, &m); }
    bt_ticking = 0;
}
static void bt_follow(void) { bt_refresh(); if (!bt_ticking) { bt_ticking = 1; thread_start(bt_ticker, 0, 4096); } }
int settings_bt_tick(void) { bt_refresh(); return 1; }

/* The audio server (it starts when first used): its volume; its outputs
 * (names, one a line). */
static long audio_call(uint64_t op, uint64_t v)
{
    static long ap;
    msg_t m = { .w = { op, v }, .rbuf = onames, .rlen = sizeof onames - 1 };
    long e = call_named(&ap, "audio", &m, 1);
    if (e >= 0 && op == AUDIO_OUT && (long)m.w[0] >= 0) {
        osel = (int)m.w[0]; nouts = 0;
        onames[m.rlen] = 0;
        for (char *p = onames, *n; nouts < 5 && (n = strchr(p, '\n')); p = n + 1) { *n = 0; outs[nouts++] = p; }
    }
    return e < 0 ? -2 : (long)m.w[0];
}
static long audio_vol(uint64_t v) { return audio_call(AUDIO_VOL, v); }

/* Ask the Wi-Fi server for its status (if one is running). */
static void wifi_refresh(void)
{
    msg_t m = { .w = { WIFI_STATUS }, .rbuf = &wstat, .rlen = sizeof wstat };
    wifi_have = atlas_wifi(&m) == 0 && m.rlen == sizeof wstat;
}

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
    wipe(fields[F_WPASS], sizeof fields[0]);
    wipe(fields[F_ROOTPW], sizeof fields[0]);
    wifi_sel = -1; wifi_n = 0;
    wifi_refresh();
    bt_sel = -1;
    bt_refresh();
    if (vol != -2) vol = port_find("audio") > 0 ? audio_vol(AUDIO_ASK) : -1;   /* (not started just for this) */
    if (vol >= 0) audio_call(AUDIO_OUT, AUDIO_ASK);
    field = -1; scroll = 0; msg[0] = 0;
    say("atlas: settings open (wifi %s)\n", wifi_have ? "present" : "none");
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
/* A configuration file with these "key=value" lines replaced (/etc/display.conf, /etc/sia.conf). */
static long conf_set(const char *path, const char *const *kv, int n)
{
    static char out[2048];
    char tmp[64];
    size_t len = 0, fl = 0;
    char *f = file_get(path, &fl), *p = f;
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
    strlcpy(tmp + strlcpy(tmp, path, sizeof tmp - 4), ".new", 5);
    long h = fs_open(tmp, FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
    if (h < 0) return h;
    long r = fs_write(h, 0, out, len);
    fs_close(h);
    return r < 0 ? r : fs_rename(tmp, path);
}
static void keep(void)
{
    int w, h;
    atlas_screen(&w, &h);
    back.on = 0; gen++;
    char line[32], *p = line + strlcpy(line, "size=", sizeof line);
    p += num(p, w); *p++ = 'x'; p[num(p, h)] = 0;
    const char *kv[1] = { line };
    conf_set("/etc/display.conf", kv, 1);
    say("atlas: screen size %d x %d kept\n", w, h);
}
static void set_bright(int pct)                          /* the panel's backlight (the Intel display) */
{
    char line[24];
    strlcpy(line, "brightness=", sizeof line);
    line[11 + num(line + 11, screen_backlight(pct))] = 0;
    const char *kv[1] = { line };
    conf_set("/etc/display.conf", kv, 1);
}

/* ---- The assistant's model: /etc/sia.conf (these keys replaced), /etc/sia.key. */

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
    long r = conf_set("/etc/sia.conf", kv, remote ? 3 : 1);
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

/* Root's password, for a system setting (Join/Forget), the way apply_sia
 * does: logged in as root, or the password in F_ROOTPW checks out. */
static int check_root(void)
{
    if (atlas_uid() == 0) return 1;
    if (!fields[F_ROOTPW][0]) { strlcpy(msg, "Root's password is needed to change this.", sizeof msg); msg_err = 1; field = F_ROOTPW; return 0; }
    char req[160], *p = req;
    p += strlcpy(p, "root", 8) + 1;
    p += strlcpy(p, fields[F_ROOTPW], 128) + 1;
    msg_t m = { .w = { AUTH_CHECK }, .sbuf = req, .slen = p - req };
    long r = atlas_auth(&m);
    wipe(req, sizeof req);
    wipe(fields[F_ROOTPW], sizeof fields[0]);
    if (r < 0) { strlcpy(msg, "Wrong password for root.", sizeof msg); msg_err = 1; return 0; }
    return 1;
}

/* ---- The Network section's actions. */
static void wifi_scan(void)
{
    msg_t m = { .w = { WIFI_SCAN }, .rbuf = wnets, .rlen = sizeof wnets };
    long r = atlas_wifi(&m);                              /* (blocks a few seconds: the chip scans) */
    wifi_n = r > 0 ? (int)r : 0; wifi_sel = -1;
    wifi_refresh();
    msg_err = r < 0;
    strlcpy(msg, r < 0 ? "No Wi-Fi on this machine." : "Scan done.", sizeof msg);
    say("atlas: settings: wifi scan, %d networks\n", wifi_n);
}
static void wifi_join(void)
{
    if (wifi_sel < 0 || wifi_sel >= wifi_n) { strlcpy(msg, "Pick a network first.", sizeof msg); msg_err = 1; return; }
    if (!check_root()) return;
    char req[128];
    size_t a = strlcpy(req, wnets[wifi_sel].ssid, sizeof req) + 1;
    if (wnets[wifi_sel].security == WIFI_WPA2) a += strlcpy(req + a, fields[F_WPASS], sizeof req - a) + 1;
    else req[a++] = 0;
    msg_t m = { .w = { WIFI_JOIN }, .sbuf = req, .slen = a };
    long r = atlas_wifi(&m);
    wipe(req, sizeof req); wipe(fields[F_WPASS], sizeof fields[0]);
    msg_err = r < 0;
    strlcpy(msg, r < 0 ? "Could not join that network." : "Joining...", sizeof msg);
    wifi_refresh();
    say("atlas: settings: wifi join %s -> %ld\n", wnets[wifi_sel].ssid, r);
}
static void wifi_forget(void)
{
    if (!check_root()) return;
    msg_t m = { .w = { WIFI_FORGET } };
    long r = atlas_wifi(&m);
    msg_err = r < 0;
    strlcpy(msg, r < 0 ? "Could not do that." : "Forgotten.", sizeof msg);
    wifi_refresh();
    say("atlas: settings: wifi forget -> %ld\n", r);
}

/* ---- The Bluetooth section's actions. */
static void bt_do(int req, uint64_t arg, int sel, const char *done)
{
    if (req != BT_SCAN && req != BT_CONFIRM && !check_root()) return;
    bt_dev_t d = sel >= 0 ? bdev[sel] : (bt_dev_t){ 0 };
    msg_t m = { .w = { (uint64_t)req, arg }, .sbuf = &d, .slen = sel >= 0 ? sizeof d : 0 };
    long r = bt_call(&m);
    msg_err = r < 0;
    strlcpy(msg, r == -EAGAIN ? "Bluetooth is off or not ready." : r < 0 ? "Could not do that." : done, sizeof msg);
    say("atlas: settings: bluetooth request %d -> %ld\n", req, r);
    bt_follow();
}

/* ---- Drawing: rows from the top (scrolled), only those wholly in view. */
static int X0, Y0, X1, Y1, cy, lx, cx;
static int fits(int h) { return cy >= Y0 + TOP + 6 && cy + h <= Y1 - 6; }
static void heading(const char *s)
{
    cy += 14;
    if (fits(34)) {                                       /* (a title: bigger, bold) */
        dl_text(lx, cy, s, strlen(s), ST, C_ACCT, 0, 0, SW, SH);
        dl_text(lx + 1, cy, s, strlen(s), ST, C_ACCT, 0, 0, SW, SH);
    }
    cy += 44;
}
static void row_label(const char *s)
{
    if (cx == lx) { if (fits(30)) label(lx, cy, s, 2, C_DIM); cy += 34; }
    else if (fits(42)) label(lx, cy + 10, s, 2, C_DIM);
}
/* One choice: its options joined in a segmented control (round at its
 * two ends, a line between options), wrapped onto more rows if narrow. */
#define MAXCH 12
static void segment(const char *const *s, int a, int b, int on, int base, const int *xs, const int *ws)
{
    int x0 = xs[a], x1 = xs[b] + ws[b], y = cy;
    dl_rect(x0, y, x1, y + 42, 21, C_GLASS);
    for (int i = a; i <= b; i++) {
        int x = xs[i], w = ws[i];
        if (i == on) {                                   /* the chosen one: filled, round where the control is */
            if (a == b) dl_rect(x, y, x + w, y + 42, 21, C_ACC);
            else if (i == a) { dl_rect(x, y, x + w, y + 42, 21, C_ACC); dl_rect(x + 21, y, x + w, y + 42, 0, C_ACC); }
            else if (i == b) { dl_rect(x, y, x + w, y + 42, 21, C_ACC); dl_rect(x, y, x + w - 21, y + 42, 0, C_ACC); }
            else dl_rect(x, y, x + w, y + 42, 0, C_ACC);
        } else if (i > a && i - 1 != on) dl_rect(x, y + 9, x + 1, y + 33, 0, C_LINE);   /* (between two options) */
        dl_text(x + 14, y + 10, s[i], strlen(s[i]), S(2), i == on ? C_INK : C_TXT, 0, 0, SW, SH);
        hitbox(x, y, x + w, y + 42, H_SET, base + i);
        if (atlas_debug) say("atlas: dbg set %d at %d %d\n", base + i, x + w / 2, y + 21);
    }
    dl_frame(x0, y, x1, y + 42, 21, 1, C_LINE);
}
static void choices(const char *name, const char *const *s, int n, int on, int base)
{
    row_label(name);
    int xs[MAXCH], ws[MAXCH], rows[MAXCH], x = cx, r = 0;
    if (n > MAXCH) n = MAXCH;
    for (int i = 0; i < n; i++) {                        /* where each option goes */
        ws[i] = tw(strlen(s[i]), 2) + 28;
        if (x > cx && x + ws[i] > X1 - 24) { x = cx; r++; }
        xs[i] = x; rows[i] = r;
        x += ws[i];
    }
    for (int a = 0, b; a < n; a = b + 1) {               /* each row of them: one control */
        for (b = a; b + 1 < n && rows[b + 1] == rows[a]; b++) ;
        if (fits(42)) segment(s, a, b, on, base, xs, ws);
        if (b + 1 < n) cy += ROW;
    }
    cy += ROW;
}
static void text_field(const char *name, int f, int secret, const char *empty)
{
    row_label(name);
    if (fits(44)) {
        int on = field == f, n = strlen(fields[f]);
        dl_rect(cx, cy, X1 - 24, cy + 44, 10, C_FIELD);
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
    dl_rect(x0 + 2, y0 + TOP, x1 - 2, y1 - 2, 0, C_PANEL);
    hitbox(x0, y0 + TOP, x1, y1, H_SET, S_BODY);
    lx = x0 + 24; cx = x1 - x0 >= 760 ? x0 + 250 : lx;
    cy = y0 + TOP + 4 - scroll;

    heading("SCREEN");
    int w, h, rs = atlas_screen(&w, &h);
    char t[64], *p;
    if (rs) {
        const char *names[NSIZE + 1];
        static char nb[NSIZE + 1][16];
        int on = -1, n = NSIZE + screen_native(&nat[0], &nat[1]);   /* (and the panel's own size) */
        for (int i = 0; i < n; i++) {
            const int *z = i < NSIZE ? sizes[i] : nat;
            p = nb[i]; p += num(p, z[0]); p += strlcpy(p, " x ", 4); p[num(p, z[1])] = 0;
            names[i] = nb[i];
            if (z[0] == w && z[1] == h) on = i;
        }
        choices("Size", names, n, on, S_SIZE);
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
    int bl = screen_backlight(-1);
    if (bl >= 0) {
        static const char *bn[5] = { "10%", "25%", "50%", "75%", "100%" };
        int bk = -1;
        for (int i = 0; i < 5; i++) if (brights[i] == bl) bk = i;
        choices("Brightness", bn, 5, bk, S_BRIGHT);
    }

    heading("LOOK");
    static const char *looks[NLOOK] = { "Phosphor", "Amber 1984", "Glass", "BeOS" };
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

    heading("SOUND");
    if (vol == -2) {
        row_label("Volume");
        if (fits(42)) label(cx, cy + 10, "No sound card.", 2, C_TXT);
        cy += ROW;
    } else {
        static const char *vn[5] = { "Mute", "25%", "50%", "75%", "100%" };
        choices("Volume", vn, 5, vol < 0 ? -1 : vol & AUDIO_MUTE ? 0 : ((vol & 0xFF) + 12) / 25, S_VOL);
        if (nouts > 1) choices("Output", outs, nouts, osel, S_OUT);   /* (a monitor with sound) */
    }

    heading("NETWORK");
    if (!wifi_have) {
        row_label("Wi-Fi");
        if (fits(42)) label(cx, cy + 10, "No Wi-Fi on this machine.", 2, C_TXT);
        cy += ROW;
    } else {
        static const char *st_name[] = { "off", "idle", "scanning", "joining", "connected", "failed" };
        char s[96], *p = s;
        if (wstat.state == WIFI_UP) { p += strlcpy(p, "Connected to ", 14); p += strlcpy(p, wstat.ssid, 33);
            p += strlcpy(p, " (", 3); p += num(p, wstat.rssi); strlcpy(p, " dBm)", 6); }
        else { p += strlcpy(p, "Wi-Fi: ", 8); strlcpy(p, wstat.state <= WIFI_FAILED ? st_name[wstat.state] : "?", 12); }
        row_label("Status");
        if (fits(42)) label(cx, cy + 10, s, 2, C_TXT);
        cy += ROW;
        if (fits(42)) { int x = pill(cx, cy, "Scan", 0, H_SET, S_WSCAN);
            if (atlas_debug) say("atlas: dbg set %d at %d %d\n", S_WSCAN, x - 40, cy + 21); }
        cy += ROW;
        if (wifi_n) {
            static const char *names[WIFI_MAXNET];
            for (int i = 0; i < wifi_n; i++) names[i] = wnets[i].ssid[0] ? wnets[i].ssid : "(hidden)";
            choices("Networks", names, wifi_n, wifi_sel, S_WNET);
        }
        if (wifi_sel >= 0 && wifi_sel < wifi_n && wnets[wifi_sel].security == WIFI_WPA2)
            text_field("Passphrase", F_WPASS, 1, "the network's password");
        if (fits(42)) {
            int x = pill(cx, cy, "Join", 1, H_SET, S_WJOIN);
            x = pill(x + 10, cy, "Forget", 0, H_SET, S_WFORGET);
            if (atlas_debug) { say("atlas: dbg set %d at %d %d\n", S_WJOIN, cx + 30, cy + 21);
                say("atlas: dbg set %d at %d %d\n", S_WFORGET, x - 50, cy + 21); }
        }
        cy += ROW;
    }

    heading("BLUETOOTH");
    if (!bt_have) {
        row_label("Bluetooth");
        if (fits(42)) label(cx, cy + 10, "No Bluetooth on this machine.", 2, C_TXT);
        cy += ROW;
    } else {
        static const char *onoff[2] = { "Off", "On" };
        char s[120], *p = s;
        choices("Bluetooth", onoff, 2, bst.state == BTS_OFF ? 0 : bst.state == BTS_ON ? 1 : -1, S_BPOW);
        p += strlcpy(p, bst.state == BTS_STARTING ? "Starting... " : bst.state == BTS_FAILED ? "Failed: " : "", 16);
        p += strlcpy(p, bst.state == BTS_FAILED ? bst.why : bst.chip, 64);
        if (bst.scanning) strlcpy(p, ", scanning...", 16);
        row_label("Controller");
        if (fits(42)) label(cx, cy + 10, s, 2, C_TXT);
        cy += ROW;
        const char *who = "the device";
        for (int i = 0; i < bt_n; i++) if (!memcmp(bdev[i].addr, bst.pair_addr, 6) && bdev[i].name[0]) who = bdev[i].name;
        p = s; s[0] = 0;
        if (bst.pair == BTP_CONFIRM || bst.pair == BTP_PASSKEY) {
            p += strlcpy(p, bst.pair == BTP_CONFIRM ? "Does " : "Type ", 8);
            if (bst.pair == BTP_CONFIRM) { p += strlcpy(p, who, 33); p += strlcpy(p, " show ", 8); }
            *p++ = (char)('0' + bst.code / 100000 % 10); *p++ = (char)('0' + bst.code / 10000 % 10); *p++ = (char)('0' + bst.code / 1000 % 10);
            *p++ = (char)('0' + bst.code / 100 % 10); *p++ = (char)('0' + bst.code / 10 % 10); *p++ = (char)('0' + bst.code % 10);
            if (bst.pair == BTP_CONFIRM) strlcpy(p, "?", 2);
            else { p += strlcpy(p, " on ", 5); p += strlcpy(p, who, 33); strlcpy(p, ", then Enter.", 16); }
        } else if (bst.pair == BTP_BUSY) strlcpy(s, "Pairing...", 16);
        else if (bst.pair == BTP_DONE) { p += strlcpy(p, "Paired with ", 14); strlcpy(p, who, 33); }
        else if (bst.pair == BTP_FAILED) { p += strlcpy(p, "Pairing failed: ", 18); strlcpy(p, bst.why, 64); }
        if (s[0]) {
            row_label("Pairing");
            if (fits(42)) label(cx, cy + 10, s, 2, bst.pair == BTP_FAILED ? C_ERR : C_TXT);
            cy += ROW;
            if (bst.pair == BTP_CONFIRM && fits(42)) { int x = pill(cx, cy, "Yes", 1, H_SET, S_BYES); pill(x + 10, cy, "No", 0, H_SET, S_BNO); cy += ROW; }
        }
        if (fits(42)) { int x = pill(cx, cy, "Scan", 0, H_SET, S_BSCAN);
            if (atlas_debug) say("atlas: dbg set %d at %d %d\n", S_BSCAN, x - 40, cy + 21); }
        cy += ROW;
        if (bt_n) {
            static char lbl[BT_MAXDEV][48];
            static const char *names[BT_MAXDEV];
            for (int i = 0; i < bt_n; i++) {
                p = lbl[i] + strlcpy(lbl[i], bdev[i].name[0] ? bdev[i].name : "(no name)", 33);
                strlcpy(p, bdev[i].flags & BTF_INPUT ? " (in use)" : bdev[i].flags & BTF_BONDED ? " (paired)" : "", 12);
                names[i] = lbl[i];
            }
            choices("Devices", names, bt_n, bt_sel, S_BDEV);
        }
        if (bt_sel >= 0 && fits(42)) { int x = pill(cx, cy, "Pair", 1, H_SET, S_BPAIR); pill(x + 10, cy, "Forget", 0, H_SET, S_BFORGET); }
        if (bt_sel >= 0) cy += ROW;
    }

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
        if (msg[0]) dl_text(ax + 20, cy + 10, msg, strlen(msg), S(2), msg_err ? C_ERR : C_ACCT, ax, cy, X1 - 20, cy + 42);
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
    else if (arg == S_SIZE + NSIZE) set_size(nat[0], nat[1]);
    else if (arg >= S_BRIGHT && arg < S_BRIGHT + 5) set_bright(brights[arg - S_BRIGHT]);
    else if (arg == S_KEEP) keep();
    else if (arg == S_BACK) { gen++; settings_revert(); back.on = 0; }
    else if (arg >= S_SPEED && arg < S_SPEED + 4) { int s, w, r; atlas_mouse_get(&s, &w, &r); atlas_mouse(speeds[arg - S_SPEED], w, r); }
    else if (arg >= S_HAND && arg < S_HAND + 2) { int s, w, r; atlas_mouse_get(&s, &w, &r); atlas_mouse(s, arg - S_HAND, r); }
    else if (arg >= S_WHEEL && arg < S_WHEEL + 2) { int s, w, r; atlas_mouse_get(&s, &w, &r); atlas_mouse(s, w, arg - S_WHEEL); }
    else if (arg >= S_LOOK && arg < S_LOOK + NLOOK) atlas_look(arg - S_LOOK);
    else if (arg >= S_ZOOM && arg < S_ZOOM + NZOOM) atlas_zoom(zooms[arg - S_ZOOM]);
    else if (arg >= S_VOL && arg < S_VOL + 5) vol = audio_vol(arg > S_VOL ? 25 * (arg - S_VOL) : AUDIO_MUTE | (vol < 0 ? 80 : vol & 0xFF));
    else if (arg >= S_OUT && arg < S_OUT + nouts) audio_call(AUDIO_OUT, arg - S_OUT);
    else if (arg >= S_SIA && arg < S_SIA + 2) { remote = arg - S_SIA; msg[0] = 0; }
    else if (arg >= S_FIELD && arg < S_FIELD + NF) field = arg - S_FIELD;
    else if (arg == S_WSCAN) wifi_scan();
    else if (arg >= S_WNET && arg < S_WNET + wifi_n) { wifi_sel = arg - S_WNET; wipe(fields[F_WPASS], sizeof fields[0]); msg[0] = 0; }
    else if (arg == S_WJOIN) wifi_join();
    else if (arg == S_WFORGET) wifi_forget();
    else if (arg >= S_BPOW && arg < S_BPOW + 2) bt_do(BT_POWER, arg - S_BPOW, -1, arg > S_BPOW ? "Bluetooth on." : "Bluetooth off.");
    else if (arg == S_BSCAN) bt_do(BT_SCAN, 0, -1, "Scanning for 10 s...");
    else if (arg >= S_BDEV && arg < S_BDEV + bt_n) { bt_sel = arg - S_BDEV; msg[0] = 0; }
    else if (arg == S_BPAIR && bt_sel >= 0) bt_do(BT_PAIR, 0, bt_sel, "Pairing...");
    else if (arg == S_BFORGET && bt_sel >= 0) bt_do(BT_FORGET, 0, bt_sel, "Forgotten.");
    else if (arg == S_BYES || arg == S_BNO) bt_do(BT_CONFIRM, arg == S_BYES, -1, arg == S_BYES ? "Confirmed." : "Refused.");
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
    else if (c == '\n') { if (field == F_WPASS) wifi_join(); else apply_sia(); }
    else if (c == '\b') { if (n) f[n - 1] = 0; }
    else if (c >= 32 && c < 127 && n + 1 < sizeof fields[0]) { f[n] = c; f[n + 1] = 0; }
    else return 0;
    return 1;
}
