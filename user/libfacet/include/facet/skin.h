/*
 * facet/skin.h - The Facet desktop's skins (libfacet).
 *
 * A skin is the desktop's look: colours (the C_* names of facet/theme.h
 * are the current skin's), how widgets and icons are drawn, and, in Facet
 * itself, the window frames, the dock and the pointer.
 *
 *   strata  SIEOS's own: dark graphite, amber accent, the spine dock
 *   beos    in the style of BeOS: yellow title tabs, light bevelled
 *           surfaces, a Deskbar at the top right, a blue desktop
 *   irix    in the style of IRIX (4Dwm, Indigo Magic): steel-blue bevelled
 *           frames, a Toolchest at the top left, an indigo desktop
 *   cde     in the style of CDE (Solaris): lavender-grey Motif frames, plum
 *           for the active window, the Front Panel along the bottom
 *   amiga   in the style of the Amiga's Workbench: grey, black, white and
 *           blue; close, zoom and depth gadgets; icons on the desktop
 *
 * The skins evoke those desktops with original artwork (no logos).  The
 * default is BeOS style; the user's choice is the "skin" setting
 * (~/.facet/settings); FACET_SKIN overrides it.  When Facet
 * changes skin it tells every application (FCT_EV_SKIN), and libfacet
 * switches and redraws its views.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef FACET_SKIN_H
#define FACET_SKIN_H

#include "facet/gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { FCT_SKIN_STRATA, FCT_SKIN_BEOS, FCT_SKIN_IRIX, FCT_SKIN_CDE, FCT_SKIN_AMIGA, FCT_NSKINS };

struct fct_skin {
    int id;
    const char *name;                  /* "strata", "beos", "irix", "cde", "amiga" */
    const char *title;                 /* "Strata", "BeOS style", "IRIX style", ... */
    const char *blurb;
    bool light;                        /* light surfaces, bevelled widgets */
    color_t face, face_light, face_shadow, face_dark;
    color_t text, dim, accent, blue, title_a1, title_a2;
    color_t desk_top, desk_bot, content, content_alt, line, select, menu, menu_hot;
    color_t titlebar, titlebar_i, title_text, title_text_i;
    color_t spine, strip, good, bad;
};

extern const struct fct_skin *fct_skin;             /* the current skin */
const struct fct_skin *fct_skin_at(int i);          /* 0 .. FCT_NSKINS - 1 */
const struct fct_skin *fct_skin_named(const char *name);   /* NULL if unknown */
void fct_skin_use(const struct fct_skin *s);
/* The user's skin: FACET_SKIN, the "skin" setting (facet/settings.h), else
 * BeOS style; the system's (the login screen's): FACET_SKIN, the setting in
 * /etc/facet/settings, else BeOS style. */
const struct fct_skin *fct_skin_load(void);
const struct fct_skin *fct_skin_load_system(void);
bool fct_skin_save(const struct fct_skin *s);       /* the user's "skin" setting */

#ifdef __cplusplus
}
#endif

#endif
