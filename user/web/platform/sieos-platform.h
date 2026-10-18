/*
 * sieos-platform.h - WPE WebKit's platform on SIEOS's desktop
 * (sieos-platform.c): a WPEDisplay whose toplevels are windows of atlas
 * (mk/atlas.h), their views placed in them by the program (a browser's
 * toolbar above its page), their frames copied into the window, the
 * window's input turned into WPE events.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <wpe/wpe-platform.h>

WPEDisplay  *sieos_display_new(void);
/* A window on the desktop for up to max_views views (wpe_view_set_toplevel). */
WPEToplevel *sieos_toplevel_new(WPEDisplay *display, const char *title, guint max_views);
/* Where each view goes: called when the window gets a size (and at once,
 * if it has one): the program places its views (sieos_view_place). */
typedef void (*SieosLayout)(WPEToplevel *toplevel, int width, int height, gpointer data);
void sieos_toplevel_set_layout(WPEToplevel *toplevel, SieosLayout layout, gpointer data);
void sieos_view_place(WPEView *view, int x, int y, int width, int height);
/* The window's own keys (a browser's Ctrl-L), before its views get them:
 * keyval and WPE_MODIFIER_... -> TRUE if taken. */
typedef gboolean (*SieosKeyHook)(WPEToplevel *toplevel, guint keyval, WPEModifiers modifiers, gpointer data);
void sieos_toplevel_set_key_hook(WPEToplevel *toplevel, SieosKeyHook hook, gpointer data);
/* The view that gets the keys (also given by a click in it). */
void sieos_view_focus(WPEView *view);

/* Hosted: views drawn into pixels of the program's (a canvas of the UI
 * toolkit, sieos/ui.h), not a window of their own. damage: a view drew this
 * box of them; title: the page's. The program gives the pixels (again at
 * each new size: the layout runs), and passes on the input it gets for them
 * (x, y in the pixels; keys, buttons, wheel and focus as mk/atlas.h's). */
typedef struct {
    void (*damage)(gpointer data, int x, int y, int width, int height);
    void (*title)(gpointer data, const char *title);
    gpointer data;
} SieosHost;
WPEToplevel *sieos_toplevel_new_hosted(WPEDisplay *display, guint max_views, const SieosHost *host);
void sieos_toplevel_set_pixels(WPEToplevel *toplevel, guint32 *pixels, int width, int height);
void sieos_toplevel_key(WPEToplevel *toplevel, int code);
void sieos_toplevel_pointer(WPEToplevel *toplevel, int x, int y, int buttons, int wheel);
void sieos_toplevel_focus(WPEToplevel *toplevel, int in);
void sieos_toplevel_close(WPEToplevel *toplevel);
