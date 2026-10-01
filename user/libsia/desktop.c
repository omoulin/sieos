/*
 * desktop.c - Client side of the Facet desktop channel, and the desktop
 * tools built on it (open applications, manage windows and workspaces).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "internal.h"

static int rfd = -2, wfd = -2;

static void channel(void)
{
    if (rfd != -2)
        return;
    rfd = wfd = -1;
    const char *e = getenv("SIEOS_DESKTOP");
    if (!e)
        return;
    int r = atoi(e);
    const char *comma = strchr(e, ',');
    if (!comma)
        return;
    int w = atoi(comma + 1);
    struct stat st;
    if (r < 3 || w < 3 || fstat(r, &st) < 0 || fstat(w, &st) < 0)
        return;
    rfd = r;
    wfd = w;
}

bool sia_desktop_connected(void)
{
    channel();
    return rfd >= 0;
}

bool sia_desktop_call(const char *request, struct sbuf *result, char *err, size_t errlen)
{
    channel();
    if (rfd < 0) {
        snprintf(err, errlen, "not running inside the Facet desktop");
        return false;
    }
    size_t n = strlen(request);
    if (write(wfd, request, n) != (long)n || write(wfd, "\n", 1) != 1) {
        snprintf(err, errlen, "the desktop does not answer");
        return false;
    }
    struct sbuf line;
    sb_init(&line);
    char c;
    long r;
    while ((r = read(rfd, &c, 1)) == 1 && c != '\n')
        sb_putc(&line, c);
    if (r != 1) {
        sb_free(&line);
        snprintf(err, errlen, "the desktop closed the channel");
        return false;
    }
    struct json *j = json_parse(line.s, line.len);
    sb_free(&line);
    const struct json *ok = json_get(j, "ok");
    bool good = ok && ok->type == JSON_TRUE;
    const char *text = json_get_str(j, good ? "result" : "error");
    if (good) {
        if (result)
            sb_puts(result, text ? text : "done");
    } else {
        snprintf(err, errlen, "%s", text ? text : "the desktop refused the request");
    }
    json_free(j);
    return good;
}

/* ---------------- tools ---------------- */

/* The tool's arguments become the request, with its "op" added. */
static void desktop_run(struct sia_session *s, const struct sia_tool *t, const struct json *args, struct sbuf *result)
{
    struct sbuf req;
    sb_init(&req);
    sb_puts(&req, "{\"op\":");
    sb_json_str(&req, (const char *)t->arg);
    for (int i = 0; args && args->type == JSON_OBJECT && i < args->n; i++) {
        sb_putc(&req, ',');
        sb_json_str(&req, args->keys[i]);
        sb_putc(&req, ':');
        json_write(&req, args->items[i]);
    }
    sb_putc(&req, '}');
    char err[200];
    struct sbuf out;
    sb_init(&out);
    if (sia_desktop_call(req.s, &out, err, sizeof(err))) {
        sb_printf(result, "exit status: 0\n%s\n", out.s);
        if (s->role == SIA_ROLE_TERMINAL || strcmp(t->name, "list_windows"))
            sia_emit(s, "%s\n", out.s);
    } else {
        sb_printf(result, "exit status: 1\nerror: %s\n", err);
        sia_emit(s, "desktop: %s\n", err);
    }
    sb_free(&out);
    sb_free(&req);
}

static void desktop_describe(const struct sia_tool *t, const struct json *args, struct sbuf *out)
{
    const char *app = json_get_str(args, "app"), *cmd = json_get_str(args, "command");
    const struct json *id = json_get(args, "id"), *ws = json_get(args, "workspace");
    if (!strcmp(t->name, "open_app")) {
        sb_printf(out, "open %s", app ? app : "?");
        if (json_get_str(args, "path"))
            sb_printf(out, " %s", json_get_str(args, "path"));
        if (cmd)
            sb_printf(out, " running '%s'", cmd);
    } else if (!strcmp(t->name, "switch_workspace")) {
        sb_printf(out, "switch to workspace %ld", ws ? ws->num : 0);
    } else if (!strcmp(t->name, "move_window")) {
        sb_printf(out, "move window %ld to workspace %ld", id ? id->num : 0, ws ? ws->num : 0);
    } else if (id) {
        sb_printf(out, "%s window %ld", !strcmp(t->name, "close_window") ? "close" : "focus", id->num);
    } else {
        sb_puts(out, t->name);
    }
}

static void add(struct sia_session *s, const char *name, const char *op, const char *desc, const char *params,
                bool readonly)
{
    struct sia_tool t;
    memset(&t, 0, sizeof(t));
    snprintf(t.name, sizeof(t.name), "%s", name);
    t.description = desc;
    t.parameters = params;
    t.readonly = readonly;
    t.run = desktop_run;
    t.describe = desktop_describe;
    t.arg = (void *)op;
    sia_add_tool(s, &t);
}

bool sia_add_desktop_tools(struct sia_session *s)
{
    if (!sia_desktop_connected())
        return false;
    static char desc[900], params[700];
    bool mir = access(SIA_MIR_PROGRAM, X_OK) == 0;         /* (MiR: the package mir, when installed) */
    snprintf(desc, sizeof(desc),
             "Open an application window on the Facet desktop. app: terminal (the sia assistant terminal), shell "
             "(a plain shell terminal), files (file browser, optional path), monitor (system monitor), network "
             "(network status), browser (the NetSurf web browser, optional path: a URL to open), sipm (SiPM, the "
             "package manager: installs software), %sclock, settings (the desktop's settings), display (settings, on "
             "the screen resolution), appearance (settings, on the skin), about. For terminal or shell, 'command' "
             "is typed into it once it opens.",
             mir ? "mir (MiR, Make it Real: makes a new application the user describes; path: their request), " : "");
    snprintf(params, sizeof(params),
             "{\"type\":\"object\",\"properties\":{\"app\":{\"type\":\"string\",\"enum\":[\"terminal\",\"shell\","
             "\"files\",\"monitor\",\"network\",\"browser\",\"sipm\",%s\"clock\",\"settings\",\"display\","
             "\"appearance\",\"about\"]},\"path\":{\"type\":\"string\",\"description\":\"folder for files, URL for "
             "browser%s\"},\"command\":{\"type\":\"string\",\"description\":\"command line to run in the new "
             "terminal\"}},\"required\":[\"app\"]}",
             mir ? "\"mir\"," : "", mir ? ", the request for mir" : "");
    add(s, "open_app", "open", desc, params, true);
    add(s, "list_windows", "windows",
        "List the open windows on the desktop: id, workspace, title, and whether hidden or focused.",
        "{\"type\":\"object\",\"properties\":{}}", true);
    add(s, "focus_window", "focus", "Bring a window to the front (switching to its workspace).",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}},\"required\":[\"id\"]}", true);
    add(s, "close_window", "close", "Close a window (its program is ended).",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}},\"required\":[\"id\"]}", false);
    add(s, "switch_workspace", "workspace", "Show another of the four workspaces.",
        "{\"type\":\"object\",\"properties\":{\"workspace\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4}},"
        "\"required\":[\"workspace\"]}",
        true);
    add(s, "screen_resolution", "display",
        "The screen resolution: without width and height, list the display and the modes it offers (current and "
        "preferred marked); with them, change the resolution to that mode.",
        "{\"type\":\"object\",\"properties\":{\"width\":{\"type\":\"integer\"},\"height\":{\"type\":\"integer\"}}}",
        true);
    add(s, "desktop_skin", "skin",
        "The desktop's look (skin): without a name, list the skins and the current one; with one, switch to it: "
        "strata (SIEOS's own, dark), beos (in the style of BeOS: yellow tabs, Deskbar), irix (in the style of IRIX: "
        "4Dwm frames, Toolchest).",
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"enum\":[\"strata\",\"beos\",\"irix\"]}}}",
        true);
    add(s, "move_window", "move", "Move a window to another workspace.",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"workspace\":{\"type\":\"integer\","
        "\"minimum\":1,\"maximum\":4}},\"required\":[\"id\",\"workspace\"]}",
        true);
    return true;
}
