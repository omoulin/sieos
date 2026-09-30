/*
 * launch.c - Starting the desktop's applications.  They are separate
 * programs (/bin/facet-*) that draw through libfacet; Facet starts them
 * with FACET_DISPLAY set, and terminals also get the desktop control
 * channel (desktop.c) for the sia assistant inside them.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "facet.h"

#define MAXKIDS 64

static struct { pid_t pid; int chan; } kids[MAXKIDS];
static int term_count;

/* Start /bin/<prog> with up to three arguments; the child's pid, or -1. */
static pid_t spawn(const char *prog, const char *a1, const char *a2, const char *a3, bool channel)
{
    char path[64];
    snprintf(path, sizeof(path), "/bin/%s", prog);
    int chan = channel ? desktop_channel_new() : -1;
    pid_t pid = fork();
    if (pid < 0) {
        desktop_channel_close(chan);
        return -1;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        desktop_channel_child(chan);             /* fds 3/4, or none; closes Facet's others */
        const char *home = getenv("HOME");
        if (home)
            chdir(home);
        char *argv[5] = { (char *)prog, (char *)a1, (char *)a2, (char *)a3, NULL };
        execv(path, argv);
        _exit(127);
    }
    desktop_channel_parent(chan);
    for (int i = 0; i < MAXKIDS; i++)
        if (!kids[i].pid) {
            kids[i].pid = pid;
            kids[i].chan = chan;
            return pid;
        }
    return pid;
}

/* A child has exited: its control channel goes. */
void app_reaped(pid_t pid)
{
    for (int i = 0; i < MAXKIDS; i++)
        if (kids[i].pid == pid) {
            desktop_channel_close(kids[i].chan);
            kids[i].pid = 0;
        }
}

pid_t app_launch(const char *app, const char *arg)
{
    char n[16];
    if (!strcmp(app, "terminal") || !strcmp(app, "shell")) {
        snprintf(n, sizeof(n), "%d", ++term_count);
        return strcmp(app, "shell") ? spawn("facet-terminal", "-n", n, NULL, true)
                                    : spawn("facet-terminal", "-s", "-n", n, true);
    }
    if (!strcmp(app, "files"))
        return spawn("facet-files", arg && *arg ? arg : getenv("HOME") ? getenv("HOME") : "/", NULL, NULL, false);
    if (!strcmp(app, "viewer"))
        return spawn("facet-viewer", arg, NULL, NULL, false);
    if (!strcmp(app, "monitor"))
        return spawn("facet-monitor", NULL, NULL, NULL, false);
    if (!strcmp(app, "network"))
        return spawn("facet-network", NULL, NULL, NULL, false);
    if (!strcmp(app, "clock"))
        return spawn("facet-clock", NULL, NULL, NULL, false);
    if (!strcmp(app, "sipm") || !strcmp(app, "packages"))    /* SiPM, the package manager */
        return spawn("facet-sipm", NULL, NULL, NULL, false);
    if (!strcmp(app, "browser"))                         /* NetSurf, on the disk (not the ISO's root) */
        return spawn("netsurf", arg && *arg ? arg : NULL, NULL, NULL, false);
    if (!strcmp(app, "about"))
        return spawn("facet-about", NULL, NULL, NULL, false);
    if (!strcmp(app, "power"))
        return spawn("facet-power", NULL, NULL, NULL, false);
    if (!strcmp(app, "installer"))
        return spawn("facet-installer", NULL, NULL, NULL, true);   /* (Restart: the desktop channel) */
    if (!strcmp(app, "settings") || !strcmp(app, "display") || !strcmp(app, "appearance"))   /* (on that section) */
        return spawn("facet-settings", strcmp(app, "settings") ? app : arg, NULL, NULL, true);   /* (asks Facet through the desktop channel) */
    return -1;
}

void app_terminal(void) { app_launch("terminal", NULL); }
void app_shell_terminal(void) { app_launch("shell", NULL); }
void app_files(const char *path) { app_launch("files", path); }
void app_viewer(const char *path) { app_launch("viewer", path); }
void app_monitor(void) { app_launch("monitor", NULL); }
void app_clock(void) { app_launch("clock", NULL); }
void app_about(void) { app_launch("about", NULL); }
void app_network(void) { app_launch("network", NULL); }
void app_browser(void) { app_launch("browser", NULL); }
void app_sipm(void) { app_launch("sipm", NULL); }
void app_installer(void) { app_launch("installer", NULL); }
void app_power(void) { app_launch("power", NULL); }
void app_settings(void) { app_launch("settings", NULL); }

void app_message(const char *title, const char *line1, const char *line2)
{
    spawn("facet-message", title, line1, line2 ? line2 : "", false);
}

/* A terminal, once its window is up (the desktop's first-use setup, "open"). */
struct window *term_open(bool plain)
{
    pid_t pid = app_launch(plain ? "shell" : "terminal", NULL);
    return pid > 0 ? server_wait_window(pid, 5000) : NULL;
}

bool term_is_assistant(struct window *w) { return server_is_assistant(w); }
void term_type_line(struct window *w, const char *text) { server_type_line(w, text); }
