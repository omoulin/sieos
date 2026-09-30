/*
 * desktop.c - The desktop control channel.
 *
 * Every program Facet starts (terminals, the sia strip agent) gets a pipe
 * pair as fds 3 (replies) and 4 (requests) and SIEOS_DESKTOP="3,4".
 * libsia's desktop tools send one JSON request per line; Facet answers
 * each with one JSON line (see libsia.h for the protocol).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "facet.h"
#include "json.h"

#define MAXCHAN 32

static struct chan {
    bool used, eof;
    int rfd, wfd;                 /* ours: read requests, write replies */
    int child_r, child_w;         /* the child's ends until it has started */
    struct sbuf buf;
} chans[MAXCHAN];

int desktop_channel_new(void)
{
    for (int i = 0; i < MAXCHAN; i++) {
        struct chan *c = &chans[i];
        if (c->used)
            continue;
        int req[2], rsp[2];
        if (pipe(req) < 0)
            return -1;
        if (pipe(rsp) < 0) {
            close(req[0]);
            close(req[1]);
            return -1;
        }
        c->used = true;
        c->rfd = req[0];
        c->child_w = req[1];
        c->child_r = rsp[0];
        c->wfd = rsp[1];
        sb_init(&c->buf);
        return i;
    }
    return -1;
}

/* In the child, before exec: the channel becomes fds 3 and 4, nothing else leaks. */
void desktop_channel_child(int id)
{
    if (id >= 0 && id < MAXCHAN && chans[id].used) {
        int r = dup2(chans[id].child_r, 60), w = dup2(chans[id].child_w, 61);
        dup2(r, 3);
        dup2(w, 4);
        setenv("SIEOS_DESKTOP", "3,4", 1);
        for (int fd = 5; fd < 64; fd++)
            close(fd);
    } else {
        unsetenv("SIEOS_DESKTOP");
        for (int fd = 3; fd < 64; fd++)
            close(fd);
    }
}

void desktop_channel_parent(int id)
{
    if (id < 0 || id >= MAXCHAN || !chans[id].used)
        return;
    close(chans[id].child_r);
    close(chans[id].child_w);
    chans[id].child_r = chans[id].child_w = -1;
}

void desktop_channel_close(int id)
{
    if (id < 0 || id >= MAXCHAN || !chans[id].used)
        return;
    struct chan *c = &chans[id];
    close(c->rfd);
    close(c->wfd);
    if (c->child_r >= 0)
        close(c->child_r);
    if (c->child_w >= 0)
        close(c->child_w);
    sb_free(&c->buf);
    memset(c, 0, sizeof(*c));
}

int desktop_poll_fds(struct pollfd *p, int max, int *ids)
{
    int n = 0;
    for (int i = 0; i < MAXCHAN && n < max; i++)
        if (chans[i].used && !chans[i].eof) {
            p[n].fd = chans[i].rfd;
            p[n].events = POLLIN;
            p[n].revents = 0;
            ids[n++] = i;
        }
    return n;
}

/* ---------------- requests ---------------- */

static long num_arg(const struct json *req, const char *key)
{
    const struct json *v = json_get(req, key);
    if (!v)
        return -1;
    if (v->type == JSON_NUMBER)
        return v->num;
    if (v->type == JSON_STRING)
        return atoi(v->str);
    return -1;
}

static void reply(struct chan *c, bool ok, const char *text)
{
    struct sbuf b;
    sb_init(&b);
    sb_puts(&b, ok ? "{\"ok\":true,\"result\":" : "{\"ok\":false,\"error\":");
    sb_json_str(&b, text);
    sb_puts(&b, "}\n");
    write(c->wfd, b.s, b.len);
    sb_free(&b);
}

static void describe(struct window *w, char *buf, size_t n)
{
    snprintf(buf, n, "%s (window %d) on workspace %d", w->title, w->id, w->ws + 1);
}

static void handle(struct chan *c, const char *line, size_t len)
{
    struct json *req = json_parse(line, len);
    const char *op = json_get_str(req, "op");
    char msg[600];
    if (!op) {
        reply(c, false, "malformed request");
    } else if (!strcmp(op, "open")) {
        const char *app = json_get_str(req, "app"), *path = json_get_str(req, "path");
        const char *cmd = json_get_str(req, "command");
        struct window *w = NULL;
        if (!app) {
            reply(c, false, "which app? terminal, shell, files, monitor, network, browser, clock, settings, display, appearance or about");
            goto out;
        }
        static const char *const known[] = { "terminal", "shell", "files", "monitor", "network", "browser", "clock",
                                             "settings", "display", "appearance", "about" };
        bool ok = false;
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
            ok |= !strcmp(app, known[i]);
        if (!ok) {
            snprintf(msg, sizeof(msg), "unknown app '%s' (terminal, shell, files, monitor, network, browser, clock, settings, display, appearance, about)",
                     app);
            reply(c, false, msg);
            goto out;
        }
        pid_t pid = app_launch(app, path);           /* a program: wait for its window */
        if (pid > 0)
            w = server_wait_window(pid, 5000);
        if (!w) {
            reply(c, false, "the window could not be opened");
            goto out;
        }
        if (cmd && *cmd && (!strcmp(app, "terminal") || !strcmp(app, "shell")))
            term_type_line(w, cmd);
        char d[200];
        describe(w, d, sizeof(d));
        snprintf(msg, sizeof(msg), "opened %s%s%s", d, cmd && *cmd ? ", running " : "", cmd && *cmd ? cmd : "");
        reply(c, true, msg);
    } else if (!strcmp(op, "windows")) {
        struct window *ws[32];
        int n = wm_window_list(ws, 32);
        struct sbuf b;
        sb_init(&b);
        for (int i = 0; i < n; i++)
            sb_printf(&b, "%swindow %d, workspace %d: %s%s%s", i ? "\n" : "", ws[i]->id, ws[i]->ws + 1,
                      ws[i]->title, ws[i]->minimized ? " (hidden)" : "", ws[i] == wm_focused() ? " (focused)" : "");
        sb_printf(&b, "%scurrent workspace: %d", n ? "\n" : "", wm_workspace() + 1);
        reply(c, true, b.s);
        sb_free(&b);
    } else if (!strcmp(op, "focus") || !strcmp(op, "close") || !strcmp(op, "move")) {
        struct window *w = wm_find_window((int)num_arg(req, "id"));
        if (!w) {
            reply(c, false, "no window with that id (list_windows shows them)");
            goto out;
        }
        char d[200];
        describe(w, d, sizeof(d));
        if (!strcmp(op, "focus")) {
            wm_show_window(w);
            snprintf(msg, sizeof(msg), "brought %s to the front", d);
        } else if (!strcmp(op, "close")) {
            wm_close(w);
            snprintf(msg, sizeof(msg), "closed %s", d);
        } else {
            long n = num_arg(req, "workspace");
            if (n < 1 || n > NWORKSPACES) {
                reply(c, false, "workspace must be 1 to 4");
                goto out;
            }
            wm_move_to_workspace(w, (int)n - 1);
            snprintf(msg, sizeof(msg), "moved %s to workspace %ld", w->title, n);
        }
        reply(c, true, msg);
    } else if (!strcmp(op, "skin")) {           /* the desktop's look: list the skins, or change */
        const char *name = json_get_str(req, "name");
        if (!name || !*name) {
            size_t k = snprintf(msg, sizeof(msg), "current: %s", fct_skin->name);
            for (int i = 0; i < FCT_NSKINS && k < sizeof(msg); i++)
                k += snprintf(msg + k, sizeof(msg) - k, "\n%s: %s - %s", fct_skin_at(i)->name, fct_skin_at(i)->title,
                              fct_skin_at(i)->blurb);
            reply(c, true, msg);
        } else {
            char err[128] = "";
            if (wm_set_skin(name, err, sizeof(err))) {
                snprintf(msg, sizeof(msg), "the desktop now uses the %s skin %s", fct_skin->title, err);
                reply(c, true, msg);
            } else {
                reply(c, false, err);
            }
        }
    } else if (!strcmp(op, "display")) {        /* the resolution: list the modes, or change it */
        long w = num_arg(req, "width"), h = num_arg(req, "height");
        if (w <= 0 || h <= 0) {
            if (wm_display_modes(msg, sizeof(msg)) < 0)
                reply(c, false, "no display information");
            else
                reply(c, true, msg);
        } else if (wm_set_resolution((int)w, (int)h, msg, sizeof(msg))) {
            snprintf(msg, sizeof(msg), "the screen is now %ldx%ld", w, h);
            reply(c, true, msg);
        } else {
            reply(c, false, msg);
        }
    } else if (!strcmp(op, "pointer")) {        /* the pointer's speed (1..10) and acceleration (0, 1) */
        long sp = num_arg(req, "speed"), ac = num_arg(req, "accel");
        wm_set_pointer((int)sp, (int)ac, msg, sizeof(msg));
        reply(c, true, msg);
    } else if (!strcmp(op, "screensaver")) {    /* the screen saver: saver (logo, blank, none), timeout (s), lock (0, 1) */
        if (num_arg(req, "preview") == 1) {
            reply(c, true, "preview");
            saver_preview();
        } else if (num_arg(req, "lock_now") == 1) {
            reply(c, true, "locked");
            saver_lock_now();
        } else {
            saver_configure(json_get_str(req, "saver"), (int)num_arg(req, "timeout"), (int)num_arg(req, "lock"), msg,
                            sizeof(msg));
            reply(c, true, msg);
        }
    } else if (!strcmp(op, "reboot")) {         /* (the installer's Restart) */
        reply(c, true, "restarting");
        wm_reboot();
    } else if (!strcmp(op, "workspace")) {
        long n = num_arg(req, "workspace");
        if (n < 0)
            n = num_arg(req, "n");
        if (n < 1 || n > NWORKSPACES) {
            reply(c, false, "workspace must be 1 to 4");
            goto out;
        }
        wm_switch_workspace((int)n - 1);
        snprintf(msg, sizeof(msg), "now showing workspace %ld", n);
        reply(c, true, msg);
    } else {
        snprintf(msg, sizeof(msg), "unknown request '%s'", op);
        reply(c, false, msg);
    }
out:
    json_free(req);
}

void desktop_readable(int id)
{
    if (id < 0 || id >= MAXCHAN || !chans[id].used)
        return;
    struct chan *c = &chans[id];
    char buf[1024];
    long n = read(c->rfd, buf, sizeof(buf));
    if (n <= 0) {                          /* the program is gone; its owner closes the channel */
        c->eof = true;
        return;
    }
    sb_putn(&c->buf, buf, n);
    for (;;) {
        size_t len = 0;
        while (len < c->buf.len && c->buf.s[len] != '\n')
            len++;
        if (len == c->buf.len)
            break;
        char *nl = c->buf.s + len;
        handle(c, c->buf.s, len);
        memmove(c->buf.s, nl + 1, c->buf.len - len - 1);
        c->buf.len -= len + 1;
        c->buf.s[c->buf.len] = 0;
    }
    if (c->buf.len > 65536) {              /* nonsense: drop it */
        c->buf.len = 0;
        c->buf.s[0] = 0;
    }
}
