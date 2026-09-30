/*
 * sdm - SIEOS display manager: the graphical login screen.
 *
 * Started by init (as root) when the system boots in graphical mode.  It
 * owns the framebuffer and the input devices, authenticates a user and
 * then becomes that user's session: device ownership is handed over
 * (/etc/logindevperm), credentials are dropped and the Facet desktop is
 * exec'd in the same process.  When the desktop exits, init starts sdm
 * again.
 *
 * The screen is drawn in the system's skin (/etc/facet/settings, BeOS
 * style unless changed there): the desktop's look before anyone logs in.
 *
 * Exit status 3 asks init for a text console login instead.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "facet.h"

#define CONSOLE_REQUEST SESSION_EXIT_CONSOLE
#define MAXUSERS 6
#define FIELD_MAX 63

int screen_w, screen_h;
char desktop_user[32];

static struct fb_info fbi;
static uint32_t *fbmem;
static struct surface scene;            /* everything except the cursor */
static struct surface frame;            /* scene + cursor, copied to the screen */
static int ev_fd = -1;
static bool scene_dirty = true;
static struct rect screen_dirty;

static char hostname[64] = "sieos";
static char name_buf[FIELD_MAX + 1], pass_buf[FIELD_MAX + 1];
static int focus_field;                  /* 0 = name, 1 = password */
static char status_msg[96];
static bool status_error;
static bool caret_on = true;

static struct {
    char name[32];
    char gecos[48];
} users[MAXUSERS];
static int nusers;

static int mouse_x, mouse_y;
static unsigned buttons;
static int pressed = -1;                 /* button index being pressed */

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

#define PANEL_W 500
#define PANEL_H 360

static struct rect panel(void)
{
    return rect_make((screen_w - PANEL_W) / 2, (screen_h - PANEL_H) / 2 + 10, PANEL_W, PANEL_H);
}

static struct rect field_rect(int i)
{
    struct rect p = panel();
    return rect_make(p.x + 130, p.y + 206 + i * 38, 300, 26);
}

static struct rect user_rect(int i)
{
    struct rect p = panel();
    int total = nusers * 80;
    return rect_make(p.x + (p.w - total) / 2 + i * 80, p.y + 110, 76, 76);
}

enum { BTN_LOGIN, BTN_CONSOLE, BTN_RESTART, BTN_SHUTDOWN, NBTN };
static const char *btn_label[NBTN] = { "Log In", "Console", "Restart", "Shut Down" };

static struct rect button_rect(int b)
{
    struct rect p = panel();
    switch (b) {
    case BTN_LOGIN:    return rect_make(p.x + p.w - 110, p.y + p.h - 44, 92, 28);
    case BTN_CONSOLE:  return rect_make(p.x + 18, p.y + p.h - 44, 82, 28);
    case BTN_RESTART:  return rect_make(p.x + 106, p.y + p.h - 44, 82, 28);
    default:           return rect_make(p.x + 194, p.y + p.h - 44, 96, 28);
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void draw_person(struct surface *s, int x, int y, bool selected)
{
    color_t body = selected ? RGB(0x8F, 0xB4, 0xDC) : RGB(0x5F, 0x68, 0x74);
    gfx_disc(s, x + 24, y + 14, 10, selected ? RGB(0xF0, 0xD2, 0xB0) : RGB(0xB8, 0xA6, 0x92));
    gfx_disc(s, x + 24, y + 40, 16, body);
    gfx_fill(s, x + 8, y + 40, 33, 10, body);
}

static void draw_field(struct surface *s, int i)
{
    struct rect r = field_rect(i);
    bool focused = focus_field == i;
    if (fct_skin->light) {                             /* a sunken white field */
        gfx_fill(s, r.x, r.y, r.w, r.h, C_CONTENT);
        gfx_bevel(s, r.x, r.y, r.w, r.h, 1, false, C_FACE_LIGHT, C_FACE_SHADOW);
        if (focused)
            gfx_frame(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, C_ACCENT);
    } else {
        gfx_round_rect(s, r.x, r.y, r.w, r.h, 6, C_STRIP);
        gfx_round_frame(s, r.x, r.y, r.w, r.h, 6, focused ? C_ACCENT : C_LINE);
    }
    char shown[FIELD_MAX + 1];
    if (i == 0) {
        strlcpy(shown, name_buf, sizeof(shown));
    } else {
        size_t n = strlen(pass_buf);
        memset(shown, '*', n);
        shown[n] = 0;
    }
    int tx = r.x + 8, ty = r.y + (r.h - FONT_H) / 2;
    gfx_text(s, tx, ty, shown, C_TEXT);
    if (focused && caret_on)
        gfx_fill(s, tx + text_width(shown) + 1, ty, 2, FONT_H, C_TEXT);
}

static void render_scene(void)
{
    struct surface *s = &scene;
    gfx_set_clip(s, rect_make(0, 0, s->w, s->h));
    bool light = fct_skin->light;
    gfx_vgradient(s, 0, 0, s->w, s->h, C_DESK_TOP, C_DESK_BOT);
    if (fct_skin->id == FCT_SKIN_STRATA) {
        for (int i = 0; i < 4; i += 2)                 /* strata bands, as on the desktop */
            gfx_blend_fill(s, 0, i * s->h / 4, s->w, s->h / 4, RGB(0xFF, 0xFF, 0xFF), 5);
        for (int y = 0; y < s->h; y += 4)
            gfx_blend_fill(s, 0, y, s->w, 1, RGB(0, 0, 0), 10);
    }

    /* top bar: brand, host name, clock */
    if (light) {                                       /* a bevelled bar, like the Deskbar */
        gfx_fill(s, 0, 0, s->w, 28, C_SPINE);
        gfx_bevel(s, 0, 0, s->w, 28, 1, true, C_FACE_LIGHT, C_FACE_SHADOW);
        gfx_hline(s, 0, 28, s->w, C_FACE_DARK);
    } else {
        gfx_fill(s, 0, 0, s->w, 28, RGB(0x17, 0x19, 0x1C));
        gfx_hline(s, 0, 28, s->w, C_LINE);
    }
    logo_draw(s, 18, 14, 16);
    gfx_text_bold(s, 34, 6, "SIEOS", C_TEXT);
    gfx_text(s, 34 + text_width("SIEOS") + 12, 6, hostname, C_DIM);
    struct tm tm;
    time_t now_t = time(NULL);
    gmtime_r(&now_t, &tm);
    static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    char clk[40];
    snprintf(clk, sizeof(clk), "%s %04d-%02d-%02d  %02d:%02d", days[tm.tm_wday], tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min);
    gfx_text(s, s->w - text_width(clk) - 14, 6, clk, C_TEXT);

    /* the login card */
    struct rect p = panel();
    if (fct_skin->id == FCT_SKIN_BEOS) {               /* a window: a yellow title tab, a bevelled grey body */
        const char *title = "Welcome to SIEOS";
        int tw = text_width_bold(title) + 36, th = 22;
        color_t edge = RGB(0x40, 0x40, 0x40);
        gfx_shadow(s, p, 0, 10, 6, 120);
        gfx_fill(s, p.x, p.y - th, tw, th + 1, edge);
        gfx_vgradient(s, p.x + 1, p.y - th + 1, tw - 2, th - 1, color_shade(C_TITLEBAR, 60), C_TITLEBAR);
        gfx_hline(s, p.x + 1, p.y - th + 1, tw - 2, color_shade(C_TITLEBAR, 90));
        gfx_text_bold(s, p.x + 18, p.y - th + (th - FONT_H) / 2 + 1, title, fct_skin->title_text);
        gfx_fill(s, p.x, p.y, p.w, p.h, C_FACE);
        gfx_frame(s, p.x, p.y, p.w, p.h, edge);
        gfx_bevel(s, p.x + 1, p.y + 1, p.w - 2, p.h - 2, 1, true, C_FACE_LIGHT, C_FACE_SHADOW);
    } else if (light) {                                /* a thick bevelled frame, a steel-blue title */
        gfx_shadow(s, p, 0, 10, 6, 140);
        gfx_fill(s, p.x, p.y, p.w, p.h, C_FACE);
        gfx_bevel(s, p.x, p.y, p.w, p.h, 3, true, C_FACE_LIGHT, C_FACE_DARK);
        gfx_fill(s, p.x + 4, p.y + 4, p.w - 8, 6, C_TITLEBAR);
    } else {
        gfx_shadow(s, p, 14, 18, 8, 210);
        gfx_round_rect_vgradient(s, p.x, p.y, p.w, p.h, 14, RGB(0x2C, 0x2F, 0x35), RGB(0x22, 0x24, 0x28));
        gfx_round_frame(s, p.x, p.y, p.w, p.h, 14, RGB(0x3B, 0x3F, 0x46));
        gfx_blend_fill(s, p.x + 14, p.y + 1, p.w - 28, 1, RGB(0xFF, 0xFF, 0xFF), 36);
        gfx_fill(s, p.x + 24, p.y + 1, 60, 2, C_ACCENT);
    }

    /* logo */
    logo_draw(s, p.x + 58, p.y + 54, 76);
    gfx_text_scaled(s, p.x + 108, p.y + 18, "SIEOS", 4, C_TEXT);
    const char *tagline = "Synthetic Intelligence Enhanced Operating System";
    gfx_text(s, p.x + (p.w - text_width(tagline)) / 2, p.y + 86, tagline, C_DIM);

    /* users */
    for (int i = 0; i < nusers; i++) {
        struct rect r = user_rect(i);
        bool sel = !strcmp(name_buf, users[i].name);
        if (sel && light) {
            gfx_fill(s, r.x, r.y, r.w, r.h, C_SELECT);
            gfx_frame(s, r.x, r.y, r.w, r.h, C_BLUE);
        } else if (sel) {
            gfx_round_rect(s, r.x, r.y, r.w, r.h, 10, C_SELECT);
            gfx_round_frame(s, r.x, r.y, r.w, r.h, 10, C_BLUE);
        }
        draw_person(s, r.x + 14, r.y + 4, sel);
        int tw = text_width(users[i].name);
        gfx_text(s, r.x + (r.w - tw) / 2, r.y + 56, users[i].name, C_TEXT);
    }

    /* fields */
    gfx_text(s, p.x + 40, field_rect(0).y + 5, "Name:", C_DIM);
    gfx_text(s, p.x + 40, field_rect(1).y + 5, "Password:", C_DIM);
    draw_field(s, 0);
    draw_field(s, 1);

    /* status */
    if (status_msg[0]) {
        int tw = text_width(status_msg);
        gfx_text(s, p.x + (p.w - tw) / 2, p.y + p.h - 72, status_msg,
                 status_error ? (light ? C_BAD : RGB(0xF2, 0x8C, 0x7C)) : C_BLUE);
    }

    /* buttons */
    for (int b = 0; b < NBTN; b++) {
        struct rect r = button_rect(b);
        ui_button(s, r, btn_label[b], pressed == b && rect_contains(r, mouse_x, mouse_y));
        if (b == BTN_LOGIN && light)                   /* the default button */
            gfx_frame(s, r.x - 2, r.y - 2, r.w + 4, r.h + 4, C_FACE_DARK);
        else if (b == BTN_LOGIN)
            gfx_round_frame(s, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 8, C_ACCENT);
    }

    char foot[96];
    snprintf(foot, sizeof(foot), "SIEOS 0.4.0 on %s - Tab switches fields, Enter logs in", hostname);
    gfx_text(s, 14, s->h - 26, foot, light ? RGB(0xE8, 0xEC, 0xF2) : C_DIM);   /* (on the desktop's colour) */
    scene_dirty = false;
    screen_dirty = rect_make(0, 0, screen_w, screen_h);
}

static const char *cursor_shape[] = {
    "X...........", "XX..........", "XOX.........", "XOOX........", "XOOOX.......", "XOOOOX......",
    "XOOOOOX.....", "XOOOOOOX....", "XOOOOOOOX...", "XOOOOOOOOX..", "XOOOOOOOOOX.", "XOOOOOXXXXXX",
    "XOOXOOX.....", "XOX.XOOX....", "XX..XOOX....", "X....XOOX...", ".....XOOX...", "......XX....",
};

static struct rect cursor_rect(void)
{
    return rect_make(mouse_x, mouse_y, 12, 18);
}

static void present(void)
{
    if (scene_dirty)
        render_scene();
    if (rect_empty(screen_dirty))
        return;
    struct rect d = rect_intersect(screen_dirty, rect_make(0, 0, screen_w, screen_h));
    screen_dirty = rect_make(0, 0, 0, 0);
    gfx_set_clip(&frame, d);
    gfx_blit(&frame, d.x, d.y, &scene, d);
    for (int y = 0; y < 18; y++)
        for (int x = 0; x < 12; x++) {
            char c = cursor_shape[y][x];
            if (c == 'X')
                gfx_pixel(&frame, mouse_x + x, mouse_y + y, RGB(0x10, 0x11, 0x13));
            else if (c == 'O')
                gfx_pixel(&frame, mouse_x + x, mouse_y + y, RGB(0xF2, 0xEE, 0xE6));
        }
    bool native = fbi.red_pos == 16 && fbi.green_pos == 8 && fbi.blue_pos == 0;
    for (int y = d.y; y < d.y + d.h; y++) {
        uint32_t *src = frame.px + y * frame.stride + d.x;
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

static void set_status(const char *msg, bool error)
{
    strlcpy(status_msg, msg, sizeof(status_msg));
    status_error = error;
    scene_dirty = true;
}

/* ------------------------------------------------------------------ */
/* Users and sessions                                                  */
/* ------------------------------------------------------------------ */

static void load_users(void)
{
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd < 0)
        return;
    char line[256];
    while (nusers < MAXUSERS && read_line_fd(fd, line, sizeof(line)) >= 0) {
        char *f[7] = { 0 };
        char *rest = line;
        for (int i = 0; i < 7 && rest; i++)
            f[i] = strsep(&rest, ":");
        if (!f[2] || !f[6])
            continue;
        int uid = atoi(f[2]);
        const char *shell = f[6];
        if ((uid != 0 && (uid < 100 || uid >= 60000)) || strstr(shell, "false") || strstr(shell, "nologin"))
            continue;
        strlcpy(users[nusers].name, f[0], sizeof(users[nusers].name));
        strlcpy(users[nusers].gecos, f[4] ? f[4] : "", sizeof(users[nusers].gecos));
        nusers++;
    }
    close(fd);
}

static void set_device_owner(uid_t uid, gid_t gid)
{
    int fd = open("/etc/logindevperm", O_RDONLY);
    if (fd < 0)
        return;
    char line[256];
    while (read_line_fd(fd, line, sizeof(line)) >= 0) {
        if (line[0] == '#' || !line[0])
            continue;
        char *rest = line, *tok;
        int field = 0, mode = 0600;
        while ((tok = strsep(&rest, " \t")) != NULL) {
            if (!*tok)
                continue;
            if (field == 1)
                mode = strtol(tok, NULL, 8);
            else if (field >= 2) {
                chown(tok, uid, gid);
                chmod(tok, mode);
            }
            field++;
        }
    }
    close(fd);
}

static void start_session(struct passwd *pw)
{
    set_status("Starting your session...", false);
    present();
    close(ev_fd);                                /* facet opens it again */
    chown("/dev/console", pw->pw_uid, pw->pw_gid);
    set_device_owner(pw->pw_uid, pw->pw_gid);
    if (initgroups(pw->pw_name, pw->pw_gid) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0)
        exit(1);
    if (chdir(pw->pw_dir) < 0)
        chdir("/");
    clearenv();
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", pw->pw_shell, 1);
    setenv("PATH", pw->pw_uid == 0 ? "/sbin:/bin:/usr/bin:/usr/gnu/bin:/usr/pkg/bin" : "/bin:/usr/bin:/usr/gnu/bin:/usr/pkg/bin:/sbin", 1);
    setenv("TERM", "sieos", 1);
    setenv("ENV", "/etc/shrc", 1);           /* interactive /bin/sh: the prompt */
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    char *argv[] = { "facet", NULL };
    execv("/bin/facet", argv);
    exit(1);
}

static void try_login(void)
{
    if (!name_buf[0]) {
        set_status("Please enter your name.", true);
        focus_field = 0;
        return;
    }
    set_status("Checking...", false);
    present();
    struct passwd *pwp = getpwnam(name_buf);
    bool ok = false;
    struct passwd pw;
    char name[32], dir[128], shell[64];
    if (pwp) {
        pw = *pwp;
        strlcpy(name, pw.pw_name, sizeof(name));
        strlcpy(dir, pw.pw_dir, sizeof(dir));
        strlcpy(shell, pw.pw_shell, sizeof(shell));
        pw.pw_name = name;
        pw.pw_dir = dir;
        pw.pw_shell = shell;
        char hash[160];
        strlcpy(hash, pw.pw_passwd, sizeof(hash));
        if (!strcmp(hash, "x")) {
            struct spwd *sp = getspnam(name);
            strlcpy(hash, sp ? sp->sp_pwdp : "!", sizeof(hash));
        }
        ok = hash[0] != '!' && hash[0] != '*' && check_password(pass_buf, hash);
    }
    memset(pass_buf, 0, sizeof(pass_buf));
    if (ok) {
        start_session(&pw);
        return;
    }
    sleep(1);
    set_status("Login incorrect. Please try again.", true);
    focus_field = pwp ? 1 : 0;
}

static void press_button(int b)
{
    switch (b) {
    case BTN_LOGIN:
        try_login();
        break;
    case BTN_CONSOLE:
        exit(CONSOLE_REQUEST);
    case BTN_RESTART:
        set_status("Restarting...", false);
        present();
        sync();
        sieos_reboot(REBOOT_RESTART);
        break;
    case BTN_SHUTDOWN:
        set_status("Shutting down...", false);
        present();
        sync();
        sieos_reboot(REBOOT_HALT);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void key_event(const struct input_event *ev)
{
    if (ev->type != EV_KEY || !ev->value)
        return;
    char *buf = focus_field ? pass_buf : name_buf;
    unsigned c = ev->ascii;
    if (status_error && (c || ev->code == KEY_DOWN || ev->code == KEY_UP))
        status_msg[0] = 0;                        /* typing dismisses the last error */
    if (c == '\t' || ev->code == KEY_DOWN || ev->code == KEY_UP) {
        focus_field = !focus_field;
    } else if (c == '\n') {
        if (focus_field == 0 && name_buf[0])
            focus_field = 1;
        else
            try_login();
    } else if (c == '\b' || c == 127) {
        size_t n = strlen(buf);
        if (n)
            buf[n - 1] = 0;
    } else if (c == 27) {
        buf[0] = 0;
    } else if (c == 21) {                         /* ^U */
        buf[0] = 0;
    } else if (c >= 32 && c < 127) {
        size_t n = strlen(buf);
        if (n < FIELD_MAX) {
            buf[n] = c;
            buf[n + 1] = 0;
        }
    } else {
        return;
    }
    caret_on = true;
    scene_dirty = true;
}

static void mouse_event(const struct input_event *ev)
{
    struct rect old = cursor_rect();
    if (ev->type == EV_MOUSE_ABS) {
        mouse_x = (int)((long)ev->dx * screen_w / 65536);
        mouse_y = (int)((long)ev->dy * screen_h / 65536);
    } else {
        mouse_x += ev->dx;
        mouse_y += ev->dy;
    }
    mouse_x = MAX(0, MIN(screen_w - 1, mouse_x));
    mouse_y = MAX(0, MIN(screen_h - 1, mouse_y));
    screen_dirty = rect_union(screen_dirty, rect_union(old, cursor_rect()));

    unsigned prev = buttons;
    buttons = ev->buttons;
    if ((buttons & 1) && !(prev & 1)) {
        for (int i = 0; i < 2; i++)
            if (rect_contains(field_rect(i), mouse_x, mouse_y)) {
                focus_field = i;
                scene_dirty = true;
            }
        for (int i = 0; i < nusers; i++)
            if (rect_contains(user_rect(i), mouse_x, mouse_y)) {
                strlcpy(name_buf, users[i].name, sizeof(name_buf));
                pass_buf[0] = 0;
                focus_field = 1;
                status_msg[0] = 0;
                scene_dirty = true;
            }
        for (int b = 0; b < NBTN; b++)
            if (rect_contains(button_rect(b), mouse_x, mouse_y)) {
                pressed = b;
                scene_dirty = true;
            }
    }
    if (!(buttons & 1) && (prev & 1) && pressed >= 0) {
        int b = pressed;
        pressed = -1;
        scene_dirty = true;
        if (rect_contains(button_rect(b), mouse_x, mouse_y))
            press_button(b);
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    set_device_owner(0, 0);                      /* take devices back from the last user */
    chown("/dev/console", 0, 0);
    fct_skin_load_system();                      /* the login screen's look (/etc/facet/settings) */

    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_INFO, &fbi) < 0) {
        dprintf(STDERR_FILENO, "sdm: no framebuffer\n");
        return 1;
    }
    ev_fd = open("/dev/events", O_RDONLY);
    fbmem = fbmap(fb_fd);
    if (ev_fd < 0 || !fbmem) {
        dprintf(STDERR_FILENO, "sdm: cannot open the display\n");
        return 1;
    }
    screen_w = fbi.width;
    screen_h = fbi.height;
    scene.w = frame.w = screen_w;
    scene.h = frame.h = screen_h;
    scene.stride = frame.stride = screen_w;
    scene.px = malloc((size_t)screen_w * screen_h * 4);
    frame.px = malloc((size_t)screen_w * screen_h * 4);
    if (!scene.px || !frame.px)
        return 1;

    struct utsname u;
    if (uname(&u) == 0 && u.nodename[0])
        strlcpy(hostname, u.nodename, sizeof(hostname));
    load_users();
    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2 + 200;

    long last_blink = uptime_ms(), last_min = time(NULL) / 60;
    for (;;) {
        present();
        struct pollfd p = { ev_fd, POLLIN, 0 };
        if (poll(&p, 1, 250) > 0) {
            struct input_event evs[32];
            long got = read(ev_fd, evs, sizeof(evs));
            for (long i = 0; i < got / (long)sizeof(struct input_event); i++) {
                if (evs[i].type == EV_KEY)
                    key_event(&evs[i]);
                else
                    mouse_event(&evs[i]);
            }
        }
        long now = uptime_ms();
        if (now - last_blink >= 500) {
            last_blink = now;
            caret_on = !caret_on;
            scene_dirty = true;
        }
        if (time(NULL) / 60 != last_min) {
            last_min = time(NULL) / 60;
            scene_dirty = true;
        }
    }
}
