/*
 * web-render (make web-test): WPE WebKit headless on SIEOS. Loads a page in
 * a web view on WPEPlatform's headless display (its WebProcess and
 * NetworkProcess started as WebKit does), takes a snapshot of what it drew,
 * writes it to a PNG and checks pixels:
 *
 *   web-render URL OUT.png [X,Y=RRGGBB]...   (exact colours, or ~RRGGBB: within 24 each)
 *
 * Prints "web render: ..." lines, the last "web render: ok" or "... FAIL".
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wpe/webkit.h>
#include <wpe/headless/wpe-headless.h>

static GMainLoop *loop;
static int fails, nprobes;
static char **probes;
static const char *out;
static gint64 t0;

static void check(const char *what, int ok) { printf("web render: %s: %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }

static int write_png(const char *path, const guint8 *bgra, int w, int h, guint stride)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    png_structp p = png_create_write_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    png_infop in = png_create_info_struct(p);
    if (setjmp(png_jmpbuf(p))) { fclose(f); return -1; }
    png_init_io(p, f);
    png_set_IHDR(p, in, (png_uint_32)w, (png_uint_32)h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(p, in);
    guint8 *row = g_malloc((gsize)w * 3);
    for (int y = 0; y < h; y++) {
        const guint8 *s = bgra + (gsize)y * stride;
        for (int x = 0; x < w; x++) { row[3 * x] = s[4 * x + 2]; row[3 * x + 1] = s[4 * x + 1]; row[3 * x + 2] = s[4 * x]; }
        png_write_row(p, row);
    }
    png_write_end(p, in);
    png_destroy_write_struct(&p, &in);
    g_free(row);
    return fclose(f);
}

static void snapshot_done(GObject *o, GAsyncResult *r, gpointer data)
{
    GError *err = 0;
    WebKitImage *img = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(o), r, &err);
    (void)data;
    check("a snapshot", img != 0);
    if (!img) { printf("    %s\n", err ? err->message : "?"); g_main_loop_quit(loop); return; }
    int w = webkit_image_get_width(img), h = webkit_image_get_height(img);
    guint stride = webkit_image_get_stride(img);
    GBytes *b = webkit_image_as_bytes(img);
    const guint8 *px = g_bytes_get_data(b, 0);
    printf("web render: %dx%d, %lld ms after the load began; title \"%s\"\n", w, h, (long long)(g_get_monotonic_time() - t0) / 1000,
           webkit_web_view_get_title(WEBKIT_WEB_VIEW(o)) ? webkit_web_view_get_title(WEBKIT_WEB_VIEW(o)) : "");
    check("written to PNG", write_png(out, px, w, h, stride) == 0);
    for (int i = 0; i < nprobes; i++) {
        int x, y, near = 0;
        unsigned rgb;
        const char *eq = strchr(probes[i], '=');
        if (!eq || sscanf(probes[i], "%d,%d", &x, &y) != 2) { check(probes[i], 0); continue; }
        if (eq[1] == '~') { near = 1; eq++; }
        rgb = (unsigned)strtoul(eq + 1, 0, 16);
        if (x < 0 || y < 0 || x >= w || y >= h) { check(probes[i], 0); continue; }
        const guint8 *p = px + (gsize)y * stride + 4 * (gsize)x;
        int dr = abs(p[2] - (int)(rgb >> 16)), dg = abs(p[1] - (int)(rgb >> 8 & 255)), db = abs(p[0] - (int)(rgb & 255));
        char what[96];
        snprintf(what, sizeof what, "pixel %d,%d is %02x%02x%02x, want %s%06x", x, y, p[2], p[1], p[0], near ? "~" : "", rgb);
        check(what, near ? dr <= 24 && dg <= 24 && db <= 24 : !dr && !dg && !db);
    }
    g_bytes_unref(b);
    g_object_unref(img);
    g_main_loop_quit(loop);
}

static gboolean snapshot_later(gpointer v)
{
    webkit_web_view_get_snapshot(WEBKIT_WEB_VIEW(v), WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE, 0, snapshot_done, 0);
    return FALSE;
}

static void load_changed(WebKitWebView *v, WebKitLoadEvent e, gpointer data)
{
    (void)data;
    if (e != WEBKIT_LOAD_FINISHED) return;
    printf("web render: loaded in %lld ms\n", (long long)(g_get_monotonic_time() - t0) / 1000);
    g_timeout_add(300, snapshot_later, v);                 /* (the page's own timers: a moment) */
}

static gboolean load_failed(WebKitWebView *v, WebKitLoadEvent e, char *uri, GError *err, gpointer data)
{
    (void)v; (void)e; (void)data;
    printf("web render: %s: %s\n", uri, err->message);
    check("the load", 0);
    g_main_loop_quit(loop);
    return TRUE;
}

static gboolean timeout(gpointer data)
{
    (void)data;
    check("done within 300 s", 0);
    g_main_loop_quit(loop);
    return FALSE;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: web-render URL OUT.png [X,Y=RRGGBB]...\nweb render: FAIL\n"); return 2; }
    out = argv[2]; probes = argv + 3; nprobes = argc - 3;
    setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 1);   /* no GL on SIEOS: Skia on the CPU, no compositor */
    setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", 1);   /* (user/web/patches/webkit-04-no-gl.patch) */
    t0 = g_get_monotonic_time();
    loop = g_main_loop_new(0, FALSE);
    GError *err = 0;
    WPEDisplay *display = wpe_display_headless_new();
    check("the headless display", display && wpe_display_connect(display, &err));
    WebKitWebView *view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, NULL);
    g_signal_connect(view, "load-changed", G_CALLBACK(load_changed), 0);
    g_signal_connect(view, "load-failed", G_CALLBACK(load_failed), 0);
    g_timeout_add_seconds(300, timeout, 0);
    webkit_web_view_load_uri(view, argv[1]);
    g_main_loop_run(loop);
    printf("web render: %s\n", fails ? "FAIL" : "ok");
    fflush(stdout);
    _exit(fails != 0);                 /* (WebKit's own threads and children: not waited for) */
}
