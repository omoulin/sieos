/*
 * browser.c - /bin/web, SIEOS's web browser: WPE WebKit in a window of the
 * desktop. The window is the UI toolkit's (sieos/ui.h): a toolbar (back,
 * forward, reload or stop, the address, the progress, a menu), drawn in the
 * desktop's look, and the page under it in a canvas: WebKit's view drawn
 * there (user/web/platform, hosted), in the page's real colours.
 *
 *   web [URL]       opens it (else /lib/web/start.html); words are searched
 *   Ctrl-L          the address; F5 or Ctrl-R reload; Ctrl-W closes
 *   the menu        zoom in (Ctrl-P), out, actual size; reload, stop; quit
 *
 * Two main loops in one: GLib's (WebKit) runs; a thread waits for the
 * window's input (ui_wait) and hands it to GLib's loop (ui_dispatch).
 * Cookies, the cache and local storage in ~/.web; downloads to ~/Downloads;
 * certificates checked against /etc/ssl/roots (GIO's TLS on tls.c).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wpe/webkit.h>
#include <sieos/ui.h>
#include "platform/sieos-platform.h"

#define START "file:///lib/web/start.html"
#define SEARCH "https://html.duckduckgo.com/html/?q="

static GMainLoop *loop;
static WebKitWebView *page;
static WPEToplevel *top;
static ui_app *app;
static ui_widget *canvas, *url, *back, *fwd, *reload, *progress, *secure, *menu;

static void say(const char *fmt, const char *s) { printf(fmt, s); putchar('\n'); fflush(stdout); }

/* What was typed: an address, a path, or words to search */
static char *address(const char *text)
{
    while (*text == ' ') text++;
    if (strstr(text, "://") || !strncmp(text, "about:", 6) || !strncmp(text, "data:", 5)) return g_strdup(text);
    if (text[0] == '/') return g_strconcat("file://", text, NULL);
    if (!strchr(text, ' ') && strchr(text, '.')) return g_strconcat("https://", text, NULL);
    char *q = g_uri_escape_string(text, NULL, FALSE), *u = g_strconcat(SEARCH, q, NULL);
    g_free(q);
    return u;
}

/* ---- The toolbar follows the page. */
static void update_bar(void)
{
    const char *uri = webkit_web_view_get_uri(page);
    GTlsCertificate *cert = 0;
    GTlsCertificateFlags errors = 0;
    gboolean tls = uri && !strncmp(uri, "https:", 6) && webkit_web_view_get_tls_info(page, &cert, &errors) && !errors;
    int busy = webkit_web_view_is_loading(page);
    if (!ui_has_focus(url)) ui_set_text(url, uri ? uri : "");   /* (not while the user types there) */
    ui_set_enabled(back, webkit_web_view_can_go_back(page));
    ui_set_enabled(fwd, webkit_web_view_can_go_forward(page));
    ui_set_icon(reload, busy ? UI_ICON_STOP : UI_ICON_RELOAD);
    ui_set_value(progress, busy ? (int)(webkit_web_view_get_estimated_load_progress(page) * 1000) : 0);
    ui_set_text(secure, tls ? "TLS" : "");
    ui_flush(app);
}

static void page_changed(GObject *o, GParamSpec *p, gpointer data)
{
    (void)o; (void)data;
    if (!strcmp(p->name, "title")) {
        const char *t = webkit_web_view_get_title(page);
        char *title = g_strdup_printf("%s%sWeb", t && *t ? t : "", t && *t ? " - " : "");
        ui_set_title(app, title);
        g_free(title);
    }
    update_bar();
}

static void load_changed(WebKitWebView *v, WebKitLoadEvent e, gpointer data)
{
    (void)data;
    if (e == WEBKIT_LOAD_FINISHED) say("web: loaded %s", webkit_web_view_get_uri(v));
    update_bar();
}

static gboolean load_failed(WebKitWebView *v, WebKitLoadEvent e, char *uri, GError *err, gpointer data)
{
    (void)e; (void)data;
    if (g_error_matches(err, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) return FALSE;
    char *html = g_markup_printf_escaped("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Not loaded</title></head>"
                                         "<body style=\"font:16px sans-serif;margin:40px\"><h2>This page did not load</h2>"
                                         "<p>%s</p><p style=\"color:#666\">%s</p></body></html>", uri, err->message);
    webkit_web_view_load_alternate_html(v, html, uri, NULL);
    g_free(html);
    say("web: failed %s", uri);
    return TRUE;
}

/* Links that ask for a new window open here. */
static gboolean decide(WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType type, gpointer data)
{
    (void)data;
    if (type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) return FALSE;
    WebKitURIRequest *r = webkit_navigation_action_get_request(webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d)));
    webkit_web_view_load_request(v, r);
    webkit_policy_decision_ignore(d);
    return TRUE;
}

static gboolean decide_destination(WebKitDownload *dl, char *name, gpointer data)
{
    (void)data;
    char *dir = g_build_filename(g_get_home_dir(), "Downloads", NULL), *path = g_build_filename(dir, name, NULL);
    g_mkdir_with_parents(dir, 0700);
    webkit_download_set_destination(dl, path);
    say("web: downloading to %s", path);
    g_free(path); g_free(dir);
    return TRUE;
}
static void download_started(WebKitNetworkSession *s, WebKitDownload *dl, gpointer data)
{
    (void)s; (void)data;
    g_signal_connect(dl, "decide-destination", G_CALLBACK(decide_destination), NULL);
}

/* ---- The toolbar's and the menu's actions. */
static void go(ui_widget *w, void *d)
{
    (void)d;
    char *u = address(ui_text(w));
    webkit_web_view_load_uri(page, u);
    g_free(u);
    ui_focus(canvas);
}
static void go_back(ui_widget *w, void *d) { (void)w; (void)d; webkit_web_view_go_back(page); }
static void go_forward(ui_widget *w, void *d) { (void)w; (void)d; webkit_web_view_go_forward(page); }
static void do_reload(ui_widget *w, void *d) { (void)w; (void)d; webkit_web_view_reload(page); }
static void reload_or_stop(ui_widget *w, void *d) { (void)w; (void)d; if (webkit_web_view_is_loading(page)) webkit_web_view_stop_loading(page); else webkit_web_view_reload(page); }
static void stop(ui_widget *w, void *d) { (void)w; (void)d; webkit_web_view_stop_loading(page); }
static void edit_url(ui_widget *w, void *d) { (void)w; (void)d; ui_focus(url); ui_field_select(url, 0, -1); }
static void zoom(ui_widget *w, void *d)
{
    (void)w;
    long k = (long)d;
    double z = webkit_web_view_get_zoom_level(page);
    z = k == 0 ? 1.0 : k > 0 ? z * 1.25 : z / 1.25;
    if (z < 0.25) z = 0.25;
    if (z > 5.0) z = 5.0;
    webkit_web_view_set_zoom_level(page, z);
    printf("web: zoom %d%%\n", (int)(z * 100 + 0.5)); fflush(stdout);
}
static void quit(ui_widget *w, void *d) { (void)w; (void)d; ui_quit(app, 0); g_main_loop_quit(loop); }
static void show_menu(ui_widget *w, void *d)
{
    int x, y, ww, hh;
    (void)d;
    ui_bounds(w, &x, &y, &ww, &hh);
    ui_popup(menu, x + ww - ui_scale(app, 300), y + hh);
}

/* ---- The page in the canvas: its pixels, its input. */
static void canvas_event(ui_widget *c, const ui_event *e, void *d)
{
    (void)d;
    if (e->type == UI_EV_SIZE) { int w, h; guint32 *px = ui_canvas_pixels(c, &w, &h); sieos_toplevel_set_pixels(top, px, w, h); }
    else if (e->type == UI_EV_POINTER) sieos_toplevel_pointer(top, e->x, e->y, e->buttons, e->wheel);
    else if (e->type == UI_EV_KEY) sieos_toplevel_key(top, e->key);
    else if (e->type == UI_EV_FOCUS) sieos_toplevel_focus(top, e->key);
}
static void host_damage(gpointer d, int x, int y, int w, int h) { (void)d; ui_canvas_damage(canvas, x, y, w, h); ui_flush(app); }
static void host_title(gpointer d, const char *t) { (void)d; ui_set_title(app, t); }
static const SieosHost host = { host_damage, host_title, 0 };
static void layout(WPEToplevel *tl, int w, int h, gpointer data) { (void)tl; (void)data; sieos_view_place(webkit_web_view_get_wpe_view(page), 0, 0, w, h); }

/* The window's input: a thread waits for it, GLib's loop dispatches it. */
typedef struct { int n; ui_event ev[64]; } batch_t;
static gboolean deliver(gpointer data)
{
    batch_t *b = data;
    if (b->n < 0) { g_main_loop_quit(loop); return G_SOURCE_REMOVE; }
    ui_dispatch(app, b->ev, b->n);
    ui_flush(app);
    if (ui_done(app)) { sieos_toplevel_close(top); g_main_loop_quit(loop); }
    return G_SOURCE_REMOVE;
}
static gpointer reader(gpointer data)
{
    (void)data;
    for (;;) {
        batch_t *b = g_new0(batch_t, 1);
        int n = b->n = ui_wait(app, b->ev, 64);
        g_main_context_invoke_full(0, G_PRIORITY_DEFAULT, deliver, b, g_free);
        if (n < 0) return 0;
    }
}

int sieos_webkit_process(int argc, char **argv, int *status);   /* (multicall.cpp) */

int main(int argc, char **argv)
{
    int status;
    if (sieos_webkit_process(argc, argv, &status)) return status;   /* started as WebKit's WebProcess or NetworkProcess */
    setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 1);   /* no GL on SIEOS (user/web/patches/webkit-04-no-gl.patch) */
    setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", 1);
    loop = g_main_loop_new(NULL, FALSE);
    app = ui_app_new("Web");
    if (!app) { fprintf(stderr, "web: no window on the desktop\n"); return 1; }
    WPEDisplay *d = sieos_display_new();
    GError *err = NULL;
    if (!wpe_display_connect(d, &err)) { fprintf(stderr, "web: %s\n", err->message); return 1; }
    wpe_settings_set_boolean(wpe_display_get_settings(d), WPE_SETTING_CREATE_VIEWS_WITH_A_TOPLEVEL, FALSE, WPE_SETTINGS_SOURCE_APPLICATION, NULL);

    char *data_dir = g_build_filename(g_get_home_dir(), ".web", NULL), *cache_dir = g_build_filename(data_dir, "cache", NULL);
    WebKitNetworkSession *session = webkit_network_session_new(data_dir, cache_dir);
    char *cookies = g_build_filename(data_dir, "cookies.sqlite", NULL);
    g_mkdir_with_parents(data_dir, 0700);
    webkit_cookie_manager_set_persistent_storage(webkit_network_session_get_cookie_manager(session), cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    g_signal_connect(session, "download-started", G_CALLBACK(download_started), NULL);

    page = g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", d, "network-session", session, NULL);
    g_signal_connect(page, "notify::uri", G_CALLBACK(page_changed), NULL);
    g_signal_connect(page, "notify::title", G_CALLBACK(page_changed), NULL);
    g_signal_connect(page, "notify::estimated-load-progress", G_CALLBACK(page_changed), NULL);
    g_signal_connect(page, "load-changed", G_CALLBACK(load_changed), NULL);
    g_signal_connect(page, "load-failed", G_CALLBACK(load_failed), NULL);
    g_signal_connect(page, "decide-policy", G_CALLBACK(decide), NULL);

    /* the window: the toolbar, the page */
    menu = ui_menu(app, 0, "Web");
    ui_item(menu, "Open an address", UI_CTRL('l'), edit_url, 0);
    ui_item(menu, "Reload", UI_CTRL('r'), do_reload, 0);
    ui_set_visible(ui_item(menu, "Reload", UI_KEY_F1 + 4, do_reload, 0), 0);   /* (F5 too) */
    ui_item(menu, "Stop", 0, stop, 0);
    ui_separator(menu);
    ui_item(menu, "Zoom in", UI_CTRL('p'), zoom, (void *)1);
    ui_item(menu, "Zoom out", 0, zoom, (void *)-1);
    ui_item(menu, "Actual size", 0, zoom, (void *)0);
    ui_separator(menu);
    ui_set_enabled(ui_item(menu, "Find in the page (later)", 0, 0, 0), 0);
    ui_item(menu, "Quit", UI_CTRL('q'), quit, 0);
    ui_widget *bar = ui_toolbar(ui_root(app));
    back = ui_tool(bar, UI_ICON_BACK, "Back", go_back, 0);
    fwd = ui_tool(bar, UI_ICON_FORWARD, "Forward", go_forward, 0);
    reload = ui_tool(bar, UI_ICON_RELOAD, "Reload", reload_or_stop, 0);
    url = ui_field(bar, "", go, 0);
    ui_set_stretch(url, 1);
    secure = ui_label(bar, "");
    ui_set_size(secure, 40, 0);
    progress = ui_progress(bar);
    ui_set_size(progress, 120, 0);
    ui_tool(bar, UI_ICON_MENU, "Menu", show_menu, 0);
    canvas = ui_canvas(ui_root(app), canvas_event, 0);

    top = sieos_toplevel_new_hosted(d, 1, &host);
    WPEView *pv = webkit_web_view_get_wpe_view(page);
    wpe_view_set_toplevel(pv, top);
    sieos_toplevel_set_layout(top, layout, NULL);
    sieos_view_focus(pv);
    ui_focus(canvas);
    g_thread_unref(g_thread_new("web-window", reader, NULL));

    char *start = argc > 1 ? address(argv[1]) : g_strdup(START);
    say("web: opening %s", start);
    webkit_web_view_load_uri(page, start);
    g_free(start);
    g_main_loop_run(loop);
    ui_app_close(app);
    fflush(stdout);
    _exit(0);                        /* (WebKit's threads and its processes: not waited for) */
}
