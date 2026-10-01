/*
 * facet-installer - Install SIEOS: puts the running system on a disk of
 * this computer, in four pages: what it does, the disk (a list of this
 * computer's disks: the ones SIEOS runs from, mounted ones and RAM disks
 * cannot be chosen), the confirmation (all its data is lost; root's
 * password when the session is not root's), then the installation, whose
 * progress comes from sieinstall (set-user-ID root) through a pipe.  At the
 * end, Restart asks Facet to reboot (the desktop channel).
 * A Facet application (libfacet).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "common.h"
#include <signal.h>
#include <sys/wait.h>

#define PAD 16
#define ROW 30
#define MAXDISKS 16
#define BTN_W 104
#define BTN_H 26
#define MIN_MIB 1024

enum { P_WELCOME, P_DISK, P_CONFIRM, P_RUN, P_DONE, P_FAILED };

struct disk {
    char name[24], desc[64];
    unsigned long mib;
    const char *why;                             /* why it cannot be chosen, NULL if it can */
};

static struct {
    int page;
    struct disk disks[MAXDISKS];
    int ndisks, sel, hover;
    bool agree;
    struct fct_field pass;                       /* root's password (masked) */
    bool is_root;
    pid_t pid;
    int out;                                     /* sieinstall's output, -1 */
    char line[256];
    int linelen;
    int step, nsteps, pct;
    char step_text[96], error[200], root_part[32];
    char log[4][120];
    int nlog;
} in = { .sel = -1, .hover = -1, .out = -1 };

static void load_disks(void)
{
    in.ndisks = 0;
    in.sel = -1;
    FILE *p = popen("/bin/sieinstall -l", "r");
    if (!p)
        return;
    char l[256];
    while (fgets(l, sizeof(l), p) && in.ndisks < MAXDISKS) {
        l[strcspn(l, "\n")] = 0;
        char *save, *name = strtok_r(l, "\t", &save), *mib = strtok_r(NULL, "\t", &save);
        char *flags = strtok_r(NULL, "\t", &save), *desc = strtok_r(NULL, "\t", &save);
        if (!name || !mib)
            continue;
        struct disk *d = &in.disks[in.ndisks++];
        snprintf(d->name, sizeof(d->name), "%s", name);
        snprintf(d->desc, sizeof(d->desc), "%s", desc ? desc : "");
        d->mib = strtoul(mib, NULL, 10);
        flags = flags ? flags : "";
        d->why = strstr(flags, "root") ? "SIEOS is running from it"
               : strstr(flags, "virtual") ? "not a real disk"
               : strstr(flags, "inuse") ? "in use (mounted)"
               : strstr(flags, "ro") ? "read-only"
               : d->mib < MIN_MIB ? "too small (at least 1 GiB)" : NULL;
    }
    pclose(p);
}

/* ---------------------------------------------------------------- layout */

static struct rect button(struct rect c, int i)      /* bottom right: 0 is the rightmost */
{
    return rect_make(c.x + c.w - PAD - (i + 1) * BTN_W - i * 10, c.y + c.h - PAD - BTN_H, BTN_W, BTN_H);
}

static struct rect disk_row(struct rect c, int i)
{
    return rect_make(c.x + PAD, c.y + 84 + i * ROW, c.w - 2 * PAD, ROW - 4);
}

static struct rect agree_box(struct rect c) { return rect_make(c.x + PAD, c.y + 150, 18, 18); }
static struct rect pass_field(struct rect c) { return rect_make(c.x + PAD + 130, c.y + 190, 220, 24); }

static const char *size_str(unsigned long mib, char *buf, size_t n)
{
    if (mib >= 10240)
        snprintf(buf, n, "%lu GiB", mib / 1024);
    else if (mib >= 1024)
        snprintf(buf, n, "%lu.%lu GiB", mib / 1024, mib % 1024 * 10 / 1024);
    else
        snprintf(buf, n, "%lu MiB", mib);
    return buf;
}

static int overall(void);

static bool can_install(void)
{
    return in.agree && (in.is_root || in.pass.text[0]);
}

/* The buttons of the page: labels from the right, NULL for none; enabled. */
static int buttons(const char **labels, bool *enabled)
{
    switch (in.page) {
    case P_WELCOME:
        labels[0] = "Next", labels[1] = "Cancel";
        enabled[0] = enabled[1] = true;
        return 2;
    case P_DISK:
        labels[0] = "Next", labels[1] = "Back", labels[2] = "Refresh";
        enabled[0] = in.sel >= 0;
        enabled[1] = enabled[2] = true;
        return 3;
    case P_CONFIRM:
        labels[0] = "Install", labels[1] = "Back";
        enabled[0] = can_install();
        enabled[1] = true;
        return 2;
    case P_RUN:
        return 0;
    case P_DONE:
        labels[0] = "Restart", labels[1] = "Close";
        enabled[0] = enabled[1] = true;
        return 2;
    default:
        labels[0] = "Close", labels[1] = "Back";
        enabled[0] = enabled[1] = true;
        return 2;
    }
}

/* ---------------------------------------------------------------- drawing */

static void text_wrapped(struct surface *s, int x, int y, int w, const char *t, color_t col)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", t);
    char *p = buf;
    while (*p) {
        size_t n = text_fit(p, w);
        if (n < strlen(p)) {                         /* break at a space */
            size_t k = n;
            while (k > 0 && p[k] != ' ')
                k--;
            if (k > 0)
                n = k;
        }
        char save = p[n];
        p[n] = 0;
        gfx_text(s, x, y, p, col);
        p[n] = save;
        p += n;
        while (*p == ' ')
            p++;
        y += FONT_H + 4;
    }
}

static void header(struct surface *s, struct rect c, const char *title, const char *sub)
{
    icon_draw(s, ICON_DISK, c.x + PAD, c.y + 12, 36);
    gfx_text_bold(s, c.x + PAD + 48, c.y + 14, title, C_TEXT);
    gfx_text(s, c.x + PAD + 48, c.y + 34, sub, C_DIM);
    gfx_hline(s, c.x + PAD, c.y + 60, c.w - 2 * PAD, C_LINE);
}

static void draw(struct fct_view *w, struct surface *s, struct rect c)
{
    (void)w;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_FACE);
    char b1[24], line[200];
    struct disk *d = in.sel >= 0 ? &in.disks[in.sel] : NULL;
    switch (in.page) {
    case P_WELCOME:
        header(s, c, "Install SIEOS", "Put SIEOS on a disk of this computer");
        text_wrapped(s, c.x + PAD, c.y + 76, c.w - 2 * PAD,
                     "This puts the system you are running on a disk of this computer, so that it starts "
                     "without the USB drive and keeps your files and settings.", C_TEXT);
        text_wrapped(s, c.x + PAD, c.y + 132, c.w - 2 * PAD,
                     "The whole disk you choose is used: everything on it is erased, other systems (Windows) "
                     "included. It gets a boot partition (UEFI) and SIEOS's ext4 file system.", C_TEXT);
        text_wrapped(s, c.x + PAD, c.y + 188, c.w - 2 * PAD,
                     "The computer must start in UEFI mode (Secure Boot off), as it does from the USB drive.",
                     C_DIM);
        break;
    case P_DISK:
        header(s, c, "Choose the disk", "SIEOS is installed on the whole disk");
        if (!in.ndisks)
            gfx_text(s, c.x + PAD, c.y + 84, "No disk found (NVMe, ATA). Refresh after connecting one.", C_DIM);
        for (int i = 0; i < in.ndisks; i++) {
            struct disk *k = &in.disks[i];
            struct rect r = disk_row(c, i);
            if (i == in.sel)
                gfx_fill(s, r.x, r.y, r.w, r.h, C_SELECT);
            else if (i == in.hover && !k->why)
                gfx_fill(s, r.x, r.y, r.w, r.h, C_CONTENT_ALT);
            color_t col = k->why ? C_DIM : C_TEXT;
            icon_draw(s, ICON_DISK, r.x + 6, r.y + 3, 20);
            gfx_text_bold(s, r.x + 34, r.y + 5, size_str(k->mib, b1, sizeof(b1)), col);
            snprintf(line, sizeof(line), "%s   /dev/dsk/%s", k->desc[0] ? k->desc : "disk", k->name);
            gfx_text(s, r.x + 120, r.y + 5, line, col);
            if (k->why)
                gfx_text(s, r.x + r.w - 8 - text_width(k->why), r.y + 5, k->why, C_DIM);
        }
        break;
    case P_CONFIRM:
        header(s, c, "Erase the disk and install", d ? d->desc : "");
        snprintf(line, sizeof(line), "Everything on %s (/dev/dsk/%s, %s) will be erased: its partitions and "
                 "all their files. This cannot be undone.", d ? d->desc : "", d ? d->name : "",
                 d ? size_str(d->mib, b1, sizeof(b1)) : "");
        text_wrapped(s, c.x + PAD, c.y + 80, c.w - 2 * PAD, line, C_BAD);
        {
            struct rect b = agree_box(c);
            gfx_fill(s, b.x, b.y, b.w, b.h, C_CONTENT);
            gfx_bevel(s, b.x, b.y, b.w, b.h, 1, false, C_LINE, C_FACE_DARK);
            if (in.agree) {
                gfx_thick_line(s, b.x + 4, b.y + 9, b.x + 8, b.y + 13, 2, C_ACCENT);
                gfx_thick_line(s, b.x + 8, b.y + 13, b.x + 14, b.y + 4, 2, C_ACCENT);
            }
            gfx_text(s, b.x + 28, b.y + 1, "I understand that all the data on this disk will be lost", C_TEXT);
        }
        if (!in.is_root) {
            struct rect f = pass_field(c);
            gfx_text(s, c.x + PAD, f.y + 4, "Root password:", C_TEXT);
            fct_field_draw(s, f, &in.pass, true, NULL);
            gfx_text(s, c.x + PAD, f.y + 34, "Installing changes the disks: it needs the administrator (root).", C_DIM);
        }
        break;
    case P_RUN:
    case P_DONE:
    case P_FAILED: {
        header(s, c, in.page == P_DONE ? "SIEOS is installed" : in.page == P_FAILED ? "The installation failed"
                                                                                     : "Installing SIEOS",
               d ? d->desc : "");
        if (in.page == P_RUN)
            snprintf(line, sizeof(line), "Step %d of %d: %s", in.step, in.nsteps ? in.nsteps : 6, in.step_text);
        else
            snprintf(line, sizeof(line), "%s", in.page == P_DONE ? "Done." : in.error);
        text_wrapped(s, c.x + PAD, c.y + 80, c.w - 2 * PAD, line, in.page == P_FAILED ? C_BAD : C_TEXT);
        struct rect m = rect_make(c.x + PAD, c.y + 124, c.w - 2 * PAD, 18);
        ui_meter(s, m, in.page == P_DONE ? 100 : overall(), in.page == P_FAILED ? C_BAD : C_ACCENT);
        if (in.page == P_DONE) {
            snprintf(line, sizeof(line), "Remove the USB drive, then restart: the computer starts SIEOS from the "
                     "disk (its root file system is /dev/dsk/%s). If it starts another system, choose the disk "
                     "in the firmware's boot menu.", in.root_part);
            text_wrapped(s, c.x + PAD, c.y + 160, c.w - 2 * PAD, line, C_TEXT);
        }
        for (int i = 0; i < in.nlog; i++)
            gfx_text(s, c.x + PAD, c.y + 236 + i * (FONT_H + 2), in.log[i], C_DIM);
        break;
    }
    }
    const char *labels[3];
    bool enabled[3];
    int nb = buttons(labels, enabled);
    for (int i = 0; i < nb; i++) {
        struct rect r = button(c, i);
        ui_button(s, r, labels[i], false);
        if (!enabled[i])
            gfx_blend_fill(s, r.x, r.y, r.w, r.h, C_FACE, 150);
    }
}

/* ---------------------------------------------------------------- running sieinstall */

static void add_log(const char *t)
{
    if (in.nlog == 4) {
        memmove(in.log[0], in.log[1], sizeof(in.log[0]) * 3);
        in.nlog = 3;
    }
    snprintf(in.log[in.nlog++], sizeof(in.log[0]), "%s", t);
}

static void start(struct fct_view *w)
{
    struct disk *d = &in.disks[in.sel];
    int to[2], from[2];
    if (pipe(to) < 0 || pipe(from) < 0) {
        snprintf(in.error, sizeof(in.error), "could not start the installer: %s", strerror(errno));
        in.page = P_FAILED;
        return;
    }
    in.pid = fork();
    if (in.pid == 0) {
        dup2(to[0], 0);
        dup2(from[1], 1);
        dup2(from[1], 2);
        close(to[1]);
        close(from[0]);
        execl("/bin/sieinstall", "sieinstall", "-y", "-P", d->name, (char *)NULL);
        printf("error cannot run /bin/sieinstall: %s\n", strerror(errno));
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    if (in.pid < 0) {
        snprintf(in.error, sizeof(in.error), "could not start the installer: %s", strerror(errno));
        in.page = P_FAILED;
        close(to[1]);
        close(from[0]);
        return;
    }
    if (!in.is_root) {
        dprintf(to[1], "%s\n", in.pass.text);
        memset(in.pass.text, 0, sizeof(in.pass.text));
        fct_field_set(&in.pass, "");
    }
    close(to[1]);
    in.out = from[0];
    in.page = P_RUN;
    in.step = 0;
    in.pct = 0;
    in.nlog = 0;
    in.error[0] = 0;
    snprintf(in.step_text, sizeof(in.step_text), "starting");
    fct_view_invalidate(w);
}

/* Overall progress: the copy (step 4) is most of the time. */
static int overall(void)
{
    static const int base[] = { 0, 0, 4, 8, 12, 96, 100 };
    if (in.step == 4)
        return 12 + in.pct * 84 / 100;
    return in.step >= 0 && in.step <= 6 ? base[in.step] : 0;
}

static void finished(void)
{
    close(in.out);
    in.out = -1;
    int status = 0;
    waitpid(in.pid, &status, 0);
    in.pid = 0;
    if (in.page == P_RUN) {                      /* no "done" line */
        in.page = P_FAILED;
        if (!in.error[0])
            snprintf(in.error, sizeof(in.error), "the installer stopped (status %d)", WEXITSTATUS(status));
    }
}

static void got_line(const char *l)
{
    int a, b;
    if (sscanf(l, "step %d/%d", &a, &b) == 2) {
        in.step = a;
        in.nsteps = b;
        const char *t = strchr(l + 5, ' ');
        snprintf(in.step_text, sizeof(in.step_text), "%s", t ? t + 1 : "");
        in.pct = 0;
    } else if (sscanf(l, "progress %d", &a) == 1) {
        in.pct = a;
    } else if (!strncmp(l, "done", 4)) {
        in.page = P_DONE;
        snprintf(in.root_part, sizeof(in.root_part), "%s", l[4] ? l + 5 : "");
    } else if (!strncmp(l, "error ", 6)) {
        snprintf(in.error, sizeof(in.error), "%s", l + 6);
        in.page = P_FAILED;
    } else if (l[0]) {
        add_log(l);                              /* (warnings) */
    }
}

static int pollfd(struct fct_view *w)
{
    (void)w;
    return in.out;
}

static void readable(struct fct_view *w)
{
    char buf[512];
    ssize_t n = read(in.out, buf, sizeof(buf));
    if (n <= 0) {
        finished();
        fct_view_invalidate(w);
        return;
    }
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == '\n' || in.linelen == (int)sizeof(in.line) - 1) {
            in.line[in.linelen] = 0;
            in.linelen = 0;
            got_line(in.line);
        } else {
            in.line[in.linelen++] = buf[i];
        }
    }
    fct_view_invalidate(w);
}

/* ---------------------------------------------------------------- input */

static void press(struct fct_view *w, int i)
{
    const char *labels[3];
    bool enabled[3];
    int nb = buttons(labels, enabled);
    if (i >= nb || !enabled[i])
        return;
    const char *l = labels[i];
    if (!strcmp(l, "Cancel") || !strcmp(l, "Close")) {
        fct_view_close(w);
        return;
    }
    if (!strcmp(l, "Next") && in.page == P_WELCOME) {
        load_disks();
        in.page = P_DISK;
    } else if (!strcmp(l, "Next")) {
        in.agree = false;
        in.page = P_CONFIRM;
    } else if (!strcmp(l, "Refresh")) {
        load_disks();
    } else if (!strcmp(l, "Back")) {
        in.page = in.page == P_CONFIRM || in.page == P_FAILED ? P_DISK : P_WELCOME;
        if (in.page == P_DISK)
            load_disks();
    } else if (!strcmp(l, "Install")) {
        start(w);
    } else if (!strcmp(l, "Restart")) {
        char r[128];
        if (!desktop_request("{\"op\":\"reboot\"}", r, sizeof(r)))
            add_log(r[0] ? r : "could not restart: use Reboot in the SIEOS menu");
    }
    fct_view_invalidate(w);
}

static void mouse(struct fct_view *w, int x, int y, int kind, int buttons_)
{
    (void)buttons_;
    struct rect c = fct_view_content(w);
    if (in.page == P_CONFIRM && !in.is_root && fct_field_mouse(&in.pass, pass_field(c), x, y, kind)) {
        fct_view_invalidate(w);
        return;
    }
    if (kind == FCT_MOUSE_MOVE) {
        int h = -1;
        if (in.page == P_DISK)
            for (int i = 0; i < in.ndisks; i++)
                if (rect_contains(disk_row(c, i), x, y))
                    h = i;
        if (h != in.hover) {
            in.hover = h;
            fct_view_invalidate(w);
        }
        return;
    }
    if (kind != FCT_MOUSE_DOWN)
        return;
    for (int i = 0; i < 3; i++)
        if (rect_contains(button(c, i), x, y)) {
            press(w, i);
            return;
        }
    if (in.page == P_DISK) {
        for (int i = 0; i < in.ndisks; i++)
            if (rect_contains(disk_row(c, i), x, y) && !in.disks[i].why)
                in.sel = i;
    } else if (in.page == P_CONFIRM) {
        struct rect b = agree_box(c);
        if (rect_contains(rect_make(b.x, b.y, 420, b.h), x, y))
            in.agree = !in.agree;
    }
    fct_view_invalidate(w);
}

static void key(struct fct_view *w, const struct fct_key *k)
{
    if (!k->value)
        return;
    if (in.page == P_CONFIRM && !in.is_root) {
        if (k->ascii == '\n' || k->ascii == '\r') {
            if (can_install())
                start(w);
        } else {
            fct_field_key(&in.pass, k);
        }
        fct_view_invalidate(w);
        return;
    }
    if (in.page == P_DISK && in.ndisks && (k->code == 0x150 || k->code == 0x148)) {   /* Down, Up */
        int dir = k->code == 0x150 ? 1 : -1;
        for (int i = in.sel + dir, n = 0; n < in.ndisks; i += dir, n++) {
            int j = (i + in.ndisks) % in.ndisks;
            if (!in.disks[j].why) {
                in.sel = j;
                break;
            }
        }
        fct_view_invalidate(w);
    } else if (k->ascii == '\n' || k->ascii == '\r') {
        press(w, 0);
    }
}

static void tick(struct fct_view *w)
{
    if (in.page == P_RUN)
        fct_view_invalidate(w);
}

static void destroy(struct fct_view *w)
{
    (void)w;
    memset(in.pass.text, 0, sizeof(in.pass.text));   /* (a running install goes on: sieinstall ignores SIGPIPE) */
}

int main(void)
{
    if (fct_app_init() < 0)
        return 1;
    in.is_root = geteuid() == 0;
    in.pass.masked = true;
    struct fct_window_attr at = { "Install SIEOS", FCT_POS_AUTO, FCT_POS_AUTO, 620, 400, 560, 360, 0 };
    struct fct_view *w = fct_view_create(&at);
    if (!w)
        return 1;
    w->draw = draw;
    w->mouse = mouse;
    w->key = key;
    w->tick = tick;
    w->pollfd = pollfd;
    w->readable = readable;
    w->destroy = destroy;
    return fct_main();
}
