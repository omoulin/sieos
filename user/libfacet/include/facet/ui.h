/*
 * facet/ui.h - Widgets and icons in the Facet desktop's style (libfacet).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef FACET_UI_H
#define FACET_UI_H

#include "facet/gfx.h"

/* ---------------- widgets (drawn in content coordinates) ---------------- */
void ui_button(struct surface *s, struct rect r, const char *label, bool pressed);
void ui_panel(struct surface *s, struct rect r, bool sunken);
void ui_meter(struct surface *s, struct rect r, int percent, color_t fill);

/* ---------------- icons ---------------- */
enum { ICON_TERMINAL, ICON_FOLDER, ICON_FILE, ICON_PROGRAM, ICON_MONITOR, ICON_CLOCK,
       ICON_INFO, ICON_LOGOUT, ICON_HOME, ICON_NETWORK, ICON_DISK };
void icon_draw(struct surface *s, int kind, int x, int y, int size);

/* The SIEOS logo (Orbit Node), centred on (cx, cy), size pixels across. */
void logo_draw(struct surface *s, int cx, int cy, int size);
void logo_pixels(struct surface *s, int x, int y, int scale);   /* 16x16 bitmap version */

#endif
