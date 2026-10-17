/*
 * sieos-platform.c - WPE WebKit's platform on SIEOS's desktop (see the
 * header). Public WPEPlatform API only, as an out-of-tree platform is
 * written:
 *
 *   SieDisplay   no GL (WebKit draws with Skia on the CPU into shared
 *                memory), a keymap that passes key values through
 *   SieToplevel  an atlas window: its pixels in shared memory, a thread
 *                waiting for its input, which the main loop turns into
 *                WPE events for the view under the pointer (or with the keys)
 *   SieView      a box in its window: each frame WebKit renders (a
 *                WPEBufferSHM) is copied there, and atlas told what changed
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "sieos-platform.h"
#include <sieos/sieos.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include "mk/atlas.h"
#include "mk/proto.h"

/* ---- The keymap: atlas gives characters; they go as key values. */
typedef struct { WPEKeymap parent; } SieKeymap;
typedef struct { WPEKeymapClass parent; } SieKeymapClass;
G_DEFINE_FINAL_TYPE(SieKeymap, sie_keymap, WPE_TYPE_KEYMAP)
static WPEModifiers modifiers;          /* Control while a Ctrl+letter goes */
static gboolean km_entries(WPEKeymap *k, guint keyval, WPEKeymapEntry **e, guint *n) { (void)k; (void)keyval; *e = 0; *n = 0; return FALSE; }
static gboolean km_translate(WPEKeymap *k, guint keycode, WPEModifiers m, int group, guint *keyval, int *eg, int *level, WPEModifiers *consumed)
{
    (void)k; (void)m; (void)group;
    if (keyval) *keyval = keycode;
    if (eg) *eg = 0;
    if (level) *level = 0;
    if (consumed) *consumed = 0;
    return TRUE;
}
static WPEModifiers km_modifiers(WPEKeymap *k) { (void)k; return modifiers; }
static void sie_keymap_class_init(SieKeymapClass *c)
{
    c->parent.get_entries_for_keyval = km_entries;
    c->parent.translate_keyboard_state = km_translate;
    c->parent.get_modifiers = km_modifiers;
}
static void sie_keymap_init(SieKeymap *k) { (void)k; }

/* ---- The view: a box in its window. */
typedef struct { WPEView parent; int x, y, w, h; WPEBuffer *done; } SieView;
typedef struct { WPEViewClass parent; } SieViewClass;
G_DEFINE_FINAL_TYPE(SieView, sie_view, WPE_TYPE_VIEW)

/* ---- The toplevel: an atlas window. */
typedef struct {
    WPEToplevel parent;
    long win;
    uint32_t *px; int w, h; size_t size;  /* the window's pixels */
    SieosLayout layout; gpointer ldata;
    SieosKeyHook hook; gpointer hdata;
    WPEView *focus, *pressed;
    int buttons;
} SieToplevel;
typedef struct { WPEToplevelClass parent; } SieToplevelClass;
G_DEFINE_FINAL_TYPE(SieToplevel, sie_toplevel, WPE_TYPE_TOPLEVEL)

static long atlas(sieos_msg_t *m) { return sieos_call("atlas", m); }
static guint32 now_ms(void) { return (guint32)(g_get_monotonic_time() / 1000); }

static void damage(SieToplevel *t, const atlas_rect_t *r, int n)
{
    sieos_msg_t m = { .w = { ATLAS_WIN_DAMAGE, (uint64_t)t->win }, .sbuf = r, .slen = n * sizeof *r };
    atlas(&m);
}

/* Each frame: copied into the window (the view's box, clipped), atlas told. */
static gboolean released(gpointer data)
{
    SieView *v = data;
    WPEBuffer *b = v->done;
    v->done = 0;
    if (b) { wpe_view_buffer_rendered(WPE_VIEW(v), b); wpe_view_buffer_released(WPE_VIEW(v), b); g_object_unref(b); }
    g_object_unref(v);
    return G_SOURCE_REMOVE;
}
static gboolean view_render(WPEView *view, WPEBuffer *buffer, const WPERectangle *rects, guint n, GError **error)
{
    SieView *v = (SieView *)view;
    SieToplevel *t = (SieToplevel *)wpe_view_get_toplevel(view);
    (void)error;
    if (t && t->px && WPE_IS_BUFFER_SHM(buffer)) {
        WPEBufferSHM *shm = WPE_BUFFER_SHM(buffer);
        gsize len;
        const guint8 *src = g_bytes_get_data(wpe_buffer_shm_get_data(shm), &len);
        guint stride = wpe_buffer_shm_get_stride(shm);
        int bw = wpe_buffer_get_width(buffer), bh = wpe_buffer_get_height(buffer);
        int w = bw < v->w ? bw : v->w, h = bh < v->h ? bh : v->h;
        if (v->x + w > t->w) w = t->w - v->x;
        if (v->y + h > t->h) h = t->h - v->y;
        for (int y = 0; y < h && w > 0; y++)
            memcpy(t->px + (size_t)(v->y + y) * t->w + v->x, src + (size_t)y * stride, (size_t)w * 4);
        atlas_rect_t r[16];
        int k = 0;
        for (guint i = 0; i < n && k < 16; i++)
            r[k++] = (atlas_rect_t){ v->x + rects[i].x, v->y + rects[i].y, v->x + rects[i].x + rects[i].width, v->y + rects[i].y + rects[i].height };
        if (!k || n > 16) { r[0] = (atlas_rect_t){ v->x, v->y, v->x + w, v->y + h }; k = 1; }
        damage(t, r, k);
    }
    if (v->done) { wpe_view_buffer_rendered(view, v->done); wpe_view_buffer_released(view, v->done); g_object_unref(v->done); }
    v->done = g_object_ref(buffer);
    g_idle_add(released, g_object_ref(v));
    return TRUE;
}
static void view_toplevel_changed(WPEView *view, GParamSpec *p, gpointer data)
{
    (void)p; (void)data;
    SieView *v = (SieView *)view;
    if (!wpe_view_get_toplevel(view)) { wpe_view_unmap(view); return; }
    if (v->w && v->h) wpe_view_resized(view, v->w, v->h);
    wpe_view_map(view);
}
static void view_constructed(GObject *o)
{
    G_OBJECT_CLASS(sie_view_parent_class)->constructed(o);
    g_signal_connect(o, "notify::toplevel", G_CALLBACK(view_toplevel_changed), 0);
}
static void sie_view_class_init(SieViewClass *c)
{
    G_OBJECT_CLASS(c)->constructed = view_constructed;
    c->parent.render_buffer = view_render;
}
static void sie_view_init(SieView *v) { (void)v; }

void sieos_view_place(WPEView *view, int x, int y, int w, int h)
{
    SieView *v = (SieView *)view;
    v->x = x; v->y = y; v->w = w; v->h = h;
    if (w > 0 && h > 0) { wpe_view_resized(view, w, h); if (wpe_view_get_toplevel(view)) wpe_view_map(view); }
}

void sieos_view_focus(WPEView *view)
{
    SieToplevel *t = (SieToplevel *)wpe_view_get_toplevel(view);
    if (!t || t->focus == view) return;
    if (t->focus) wpe_view_focus_out(t->focus);
    t->focus = view;
    wpe_view_focus_in(view);
}

/* ---- The window's size: new pixels (shared memory), the views placed again. */
static void resized(SieToplevel *t, int w, int h)
{
    uint64_t id;
    if (w <= 0 || h <= 0) return;
    if (getrandom(&id, sizeof id, 0) != sizeof id) return;
    id &= ~(1ULL << 63);
    size_t size = ((size_t)w * h * 4 + 4095) & ~(size_t)4095;
    uint32_t *px = sieos_shm_create(id, size) < 0 ? 0 : sieos_shm_map(id);
    if (!px) return;
    for (size_t i = 0; i < (size_t)w * h; i++) px[i] = 0xffffff;
    sieos_msg_t m = { .w = { ATLAS_WIN_BUFFER, (uint64_t)t->win, id, (uint64_t)w | (uint64_t)h << 16 } };
    atlas(&m);
    sieos_shm_close(id);
    if (t->px) munmap(t->px, t->size);
    t->px = px; t->w = w; t->h = h; t->size = size;
    wpe_toplevel_resized(WPE_TOPLEVEL(t), w, h);
    if (t->layout) t->layout(WPE_TOPLEVEL(t), w, h, t->ldata);
    atlas_rect_t all = { 0, 0, w, h };
    damage(t, &all, 1);
}

/* ---- Input: the reader thread waits for the window's events; the main
 * loop hands them out. */
typedef struct { SieToplevel *t; long n; atlas_event_t ev[64]; } batch_t;

typedef struct { int x, y; WPEView *view; } find_t;
static gboolean find_view(WPEToplevel *tl, WPEView *view, gpointer data)
{
    (void)tl;
    find_t *f = data;
    SieView *v = (SieView *)view;
    if (f->x >= v->x && f->x < v->x + v->w && f->y >= v->y && f->y < v->y + v->h) { f->view = view; return TRUE; }
    return FALSE;
}
static gboolean close_view(WPEToplevel *tl, WPEView *view, gpointer data) { (void)tl; (void)data; wpe_view_closed(view); return FALSE; }

static guint keyval_of(int c, WPEModifiers *m)
{
    *m = 0;
    switch (c) {
    case '\n': return WPE_KEY_Return;
    case '\t': return WPE_KEY_Tab;
    case '\b': return WPE_KEY_BackSpace;
    case 27: return WPE_KEY_Escape;
    case KEY_UP: return WPE_KEY_Up;
    case KEY_DOWN: return WPE_KEY_Down;
    case KEY_LEFT: return WPE_KEY_Left;
    case KEY_RIGHT: return WPE_KEY_Right;
    case KEY_PGUP: return WPE_KEY_Page_Up;
    case KEY_PGDN: return WPE_KEY_Page_Down;
    case KEY_HOME: return WPE_KEY_Home;
    case KEY_END: return WPE_KEY_End;
    case KEY_DEL: return WPE_KEY_Delete;
    case KEY_BTAB: *m = WPE_MODIFIER_KEYBOARD_SHIFT; return WPE_KEY_ISO_Left_Tab;
    }
    if (c >= KEY_F1 && c < KEY_F1 + 12) return WPE_KEY_F1 + (c - KEY_F1);
    if (c >= 1 && c <= 26) { *m = WPE_MODIFIER_KEYBOARD_CONTROL; return 'a' + c - 1; }
    if (c >= KEY_C1 && c < KEY_C1 + 9) { *m = WPE_MODIFIER_KEYBOARD_CONTROL; return '1' + c - KEY_C1; }
    if (c >= 32 && c < 0x100) return wpe_unicode_to_keyval((guint32)c);
    return 0;
}

static void key(SieToplevel *t, int c)
{
    WPEView *view = t->focus;
    WPEModifiers m;
    guint kv = keyval_of(c, &m);
    if (!view || !kv) return;
    if (t->hook && t->hook(WPE_TOPLEVEL(t), kv, m, t->hdata)) return;
    modifiers = m;
    for (int up = 0; up < 2; up++) {
        WPEEvent *e = wpe_event_keyboard_new(up ? WPE_EVENT_KEYBOARD_KEY_UP : WPE_EVENT_KEYBOARD_KEY_DOWN, view, WPE_INPUT_SOURCE_KEYBOARD, now_ms(), m, kv, kv);
        wpe_view_event(view, e);
        wpe_event_unref(e);
    }
    modifiers = 0;
}

static WPEModifiers button_mods(int b)
{
    return (b & MB_LEFT ? WPE_MODIFIER_POINTER_BUTTON1 : 0) | (b & MB_MIDDLE ? WPE_MODIFIER_POINTER_BUTTON2 : 0) | (b & MB_RIGHT ? WPE_MODIFIER_POINTER_BUTTON3 : 0);
}

static void pointer(SieToplevel *t, const atlas_event_t *e)
{
    find_t f = { e->x, e->y, 0 };
    if (t->pressed) f.view = t->pressed; else wpe_toplevel_foreach_view(WPE_TOPLEVEL(t), find_view, &f);
    WPEView *view = f.view;
    if (!view) return;
    SieView *v = (SieView *)view;
    double x = e->x - v->x, y = e->y - v->y;
    guint32 time = now_ms();
    if (e->wheel) {
        WPEEvent *s = wpe_event_scroll_new(view, WPE_INPUT_SOURCE_MOUSE, time, button_mods(e->buttons), 0, -e->wheel, FALSE, FALSE, x, y);
        wpe_view_event(view, s);
        wpe_event_unref(s);
    }
    int changed = e->buttons ^ t->buttons;
    if (!changed) {
        WPEEvent *mv = wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, view, WPE_INPUT_SOURCE_MOUSE, time, button_mods(e->buttons), x, y, 0, 0);
        wpe_view_event(view, mv);
        wpe_event_unref(mv);
        return;
    }
    static const struct { int mb; guint button; } bt[] = { { MB_LEFT, 1 }, { MB_MIDDLE, 2 }, { MB_RIGHT, 3 } };
    for (int i = 0; i < 3; i++) {
        if (!(changed & bt[i].mb)) continue;
        int down = e->buttons & bt[i].mb;
        if (down) sieos_view_focus(view);
        guint count = down ? wpe_view_compute_press_count(view, x, y, bt[i].button, time) : 0;
        WPEEvent *b = wpe_event_pointer_button_new(down ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP, view, WPE_INPUT_SOURCE_MOUSE, time,
                                                   button_mods(down ? e->buttons : t->buttons), bt[i].button, x, y, count);
        wpe_view_event(view, b);
        wpe_event_unref(b);
    }
    t->buttons = e->buttons;
    t->pressed = e->buttons ? view : 0;
}

static gboolean deliver(gpointer data)
{
    batch_t *b = data;
    SieToplevel *t = b->t;
    if (b->n < 0) {                                   /* the window is gone: its views close */
        wpe_toplevel_foreach_view(WPE_TOPLEVEL(t), close_view, 0);
        wpe_toplevel_closed(WPE_TOPLEVEL(t));
    }
    for (long i = 0; i < b->n; i++) {
        const atlas_event_t *e = &b->ev[i];
        if (e->type == ATW_SIZE) resized(t, e->x, e->y);
        else if (e->type == ATW_KEY) key(t, e->code);
        else if (e->type == ATW_POINTER) pointer(t, e);
        else if (e->type == ATW_FOCUS && t->focus) { if (e->code) wpe_view_focus_in(t->focus); else wpe_view_focus_out(t->focus); }
        else if (e->type == ATW_CLOSE) { wpe_toplevel_foreach_view(WPE_TOPLEVEL(t), close_view, 0); wpe_toplevel_closed(WPE_TOPLEVEL(t)); }
    }
    return G_SOURCE_REMOVE;
}

static gpointer reader(gpointer data)
{
    SieToplevel *t = data;
    for (;;) {
        batch_t *b = g_new0(batch_t, 1);
        b->t = t;
        sieos_msg_t m = { .w = { ATLAS_WIN_EVENT, (uint64_t)t->win }, .rbuf = b->ev, .rlen = sizeof b->ev };
        long n = b->n = atlas(&m);
        g_main_context_invoke_full(0, G_PRIORITY_DEFAULT, deliver, b, g_free);   /* (b is the main loop's now) */
        if (n < 0) return 0;
    }
}

static void toplevel_set_title(WPEToplevel *tl, const char *title)
{
    SieToplevel *t = (SieToplevel *)tl;
    if (!t->win || !title) return;
    sieos_msg_t m = { .w = { ATLAS_WIN_TITLE, (uint64_t)t->win }, .sbuf = title, .slen = strlen(title) };
    atlas(&m);
}
static gboolean toplevel_resize(WPEToplevel *tl, int w, int h) { (void)tl; (void)w; (void)h; return FALSE; }   /* (the user sizes windows) */
static void sie_toplevel_class_init(SieToplevelClass *c)
{
    c->parent.set_title = toplevel_set_title;
    c->parent.resize = toplevel_resize;
}
static void sie_toplevel_init(SieToplevel *t) { (void)t; }

WPEToplevel *sieos_toplevel_new(WPEDisplay *display, const char *title, guint max_views)
{
    SieToplevel *t = g_object_new(sie_toplevel_get_type(), "display", display, "max-views", max_views, NULL);
    sieos_msg_t m = { .w = { ATLAS_WIN_OPEN }, .sbuf = title, .slen = strlen(title) };
    t->win = atlas(&m);
    if (t->win < 0) { g_warning("no window on the desktop (%ld)", t->win); g_object_unref(t); return 0; }
    wpe_toplevel_state_changed(WPE_TOPLEVEL(t), WPE_TOPLEVEL_STATE_ACTIVE);
    g_thread_unref(g_thread_new("sieos-window", reader, t));
    return WPE_TOPLEVEL(t);
}

void sieos_toplevel_set_key_hook(WPEToplevel *tl, SieosKeyHook hook, gpointer data)
{
    ((SieToplevel *)tl)->hook = hook;
    ((SieToplevel *)tl)->hdata = data;
}

void sieos_toplevel_set_layout(WPEToplevel *tl, SieosLayout layout, gpointer data)
{
    SieToplevel *t = (SieToplevel *)tl;
    t->layout = layout; t->ldata = data;
    if (t->w && t->h) layout(tl, t->w, t->h, data);
}

/* ---- The display. */
typedef struct { WPEDisplay parent; } SieDisplay;
typedef struct { WPEDisplayClass parent; } SieDisplayClass;
G_DEFINE_FINAL_TYPE(SieDisplay, sie_display, WPE_TYPE_DISPLAY)
static gboolean display_connect(WPEDisplay *d, GError **error) { (void)d; (void)error; return TRUE; }
static WPEView *display_create_view(WPEDisplay *d) { return g_object_new(sie_view_get_type(), "display", d, NULL); }
static WPEToplevel *display_create_toplevel(WPEDisplay *d, guint max_views) { return sieos_toplevel_new(d, "Web", max_views); }
static gpointer display_egl(WPEDisplay *d, GError **error)
{
    (void)d;
    g_set_error_literal(error, WPE_EGL_ERROR, WPE_EGL_ERROR_NOT_AVAILABLE, "No GL on SIEOS");
    return 0;
}
static WPEKeymap *display_keymap(WPEDisplay *d) { (void)d; return g_object_new(sie_keymap_get_type(), NULL); }
static void sie_display_class_init(SieDisplayClass *c)
{
    c->parent.connect = display_connect;
    c->parent.create_view = display_create_view;
    c->parent.create_toplevel = display_create_toplevel;
    c->parent.get_egl_display = display_egl;
    c->parent.get_keymap = display_keymap;
}
static void sie_display_init(SieDisplay *d) { (void)d; }

WPEDisplay *sieos_display_new(void) { return g_object_new(sie_display_get_type(), NULL); }
