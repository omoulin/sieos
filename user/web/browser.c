/*
 * browser.c - /bin/web, SIEOS's web browser: WPE WebKit in a window of the
 * desktop (user/web/platform). Two views in one window: the toolbar, a
 * page of its own (/lib/web/toolbar.html: back, forward, reload or stop,
 * the address, the progress), and the page shown under it. They talk by
 * messages (the toolbar's "browser" handler) and ui.update().
 *
 *   web [URL]       opens it (else /lib/web/start.html); words are searched
 *   Ctrl-L          the address; F5 or Ctrl-R reload; Ctrl-W closes
 *
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
#include "platform/sieos-platform.h"

#define BAR_H 44
#define START "file:///lib/web/start.html"
#define SEARCH "https://html.duckduckgo.com/html/?q="

static GMainLoop *loop;
static WebKitWebView *bar, *page;

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

static void update_bar(void)
{
    const char *uri = webkit_web_view_get_uri(page);
    GTlsCertificate *cert = 0;
    GTlsCertificateFlags errors = 0;
    gboolean secure = uri && !strncmp(uri, "https:", 6) && webkit_web_view_get_tls_info(page, &cert, &errors) && !errors;
    char *u = g_strescape(uri ? uri : "", NULL);
    char *js = g_strdup_printf("ui.update(\"%s\", %.2f, %d, %d, %d, %d)", u, webkit_web_view_get_estimated_load_progress(page),
                               webkit_web_view_can_go_back(page), webkit_web_view_can_go_forward(page), webkit_web_view_is_loading(page), secure);
    webkit_web_view_evaluate_javascript(bar, js, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(js); g_free(u);
}

static void page_changed(GObject *o, GParamSpec *p, gpointer data)
{
    (void)o; (void)data;
    if (!strcmp(p->name, "title")) {
        const char *t = webkit_web_view_get_title(page);
        char *title = g_strdup_printf("%s%sWeb", t && *t ? t : "", t && *t ? " - " : "");
        wpe_toplevel_set_title(wpe_view_get_toplevel(webkit_web_view_get_wpe_view(page)), title);
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

static void bar_message(WebKitUserContentManager *m, JSCValue *value, gpointer data)
{
    (void)m; (void)data;
    char *json = jsc_value_to_string(value);
    char *op = strstr(json, "\"op\":\""), *text = strstr(json, "\"text\":\"");
    if (op) {
        op += 6;
        if (!strncmp(op, "go\"", 3) && text) {
            text += 8;
            char *end = strrchr(text, '"');
            if (end) *end = 0;
            char *t = g_strcompress(text), *u = address(t);
            webkit_web_view_load_uri(page, u);
            g_free(u); g_free(t);
            sieos_view_focus(webkit_web_view_get_wpe_view(page));
        } else if (!strncmp(op, "back\"", 5)) webkit_web_view_go_back(page);
        else if (!strncmp(op, "forward\"", 8)) webkit_web_view_go_forward(page);
        else if (!strncmp(op, "reload\"", 7)) webkit_web_view_reload(page);
        else if (!strncmp(op, "stop\"", 5)) webkit_web_view_stop_loading(page);
        else if (!strncmp(op, "page\"", 5)) sieos_view_focus(webkit_web_view_get_wpe_view(page));
    }
    g_free(json);
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

static void layout(WPEToplevel *tl, int w, int h, gpointer data)
{
    (void)tl; (void)data;
    sieos_view_place(webkit_web_view_get_wpe_view(bar), 0, 0, w, BAR_H);
    sieos_view_place(webkit_web_view_get_wpe_view(page), 0, BAR_H, w, h - BAR_H);
}

/* The window's own keys, before the views get them */
static gboolean key(WPEToplevel *tl, guint k, WPEModifiers m, gpointer data)
{
    (void)tl; (void)data;
    int ctrl = m & WPE_MODIFIER_KEYBOARD_CONTROL;
    if (ctrl && k == 'l') {
        sieos_view_focus(webkit_web_view_get_wpe_view(bar));
        webkit_web_view_evaluate_javascript(bar, "ui.focusUrl()", -1, NULL, NULL, NULL, NULL, NULL);
        return TRUE;
    }
    if ((ctrl && k == 'r') || k == WPE_KEY_F5) { webkit_web_view_reload(page); return TRUE; }
    return FALSE;
}

static void closed(WPEView *v, gpointer data) { (void)v; (void)data; g_main_loop_quit(loop); }

int sieos_webkit_process(int argc, char **argv, int *status);   /* (multicall.cpp) */

int main(int argc, char **argv)
{
    int status;
    if (sieos_webkit_process(argc, argv, &status)) return status;   /* started as WebKit's WebProcess or NetworkProcess */
    setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 1);   /* no GL on SIEOS (user/web/patches/webkit-04-no-gl.patch) */
    setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", 1);
    loop = g_main_loop_new(NULL, FALSE);
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

    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(ucm, "browser", NULL);
    g_signal_connect(ucm, "script-message-received::browser", G_CALLBACK(bar_message), NULL);
    bar = g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", d, "network-session", session, "user-content-manager", ucm, NULL);
    page = g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", d, "network-session", session, NULL);
    g_signal_connect(page, "notify::uri", G_CALLBACK(page_changed), NULL);
    g_signal_connect(page, "notify::title", G_CALLBACK(page_changed), NULL);
    g_signal_connect(page, "notify::estimated-load-progress", G_CALLBACK(page_changed), NULL);
    g_signal_connect(page, "load-changed", G_CALLBACK(load_changed), NULL);
    g_signal_connect(page, "load-failed", G_CALLBACK(load_failed), NULL);
    g_signal_connect(page, "decide-policy", G_CALLBACK(decide), NULL);

    WPEToplevel *tl = sieos_toplevel_new(d, "Web", 2);
    if (!tl) return 1;
    WPEView *bv = webkit_web_view_get_wpe_view(bar), *pv = webkit_web_view_get_wpe_view(page);
    wpe_view_set_toplevel(bv, tl);
    wpe_view_set_toplevel(pv, tl);
    sieos_toplevel_set_layout(tl, layout, NULL);
    sieos_toplevel_set_key_hook(tl, key, NULL);
    g_signal_connect(pv, "closed", G_CALLBACK(closed), NULL);
    sieos_view_focus(pv);

    webkit_web_view_load_uri(bar, "file:///lib/web/toolbar.html");
    char *start = argc > 1 ? address(argv[1]) : g_strdup(START);
    say("web: opening %s", start);
    webkit_web_view_load_uri(page, start);
    g_free(start);
    g_main_loop_run(loop);
    fflush(stdout);
    _exit(0);                        /* (WebKit's threads and its processes: not waited for) */
}
