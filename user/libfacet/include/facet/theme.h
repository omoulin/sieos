/*
 * facet/theme.h - The Facet desktop's colours, for applications that want
 * to look like the rest of the desktop: the current skin's (facet/skin.h),
 * so they follow a change of skin.
 */
#ifndef FACET_THEME_H
#define FACET_THEME_H

#include "facet/gfx.h"
#include "facet/skin.h"

#define C_FACE        (fct_skin->face)          /* raised surfaces, buttons */
#define C_FACE_LIGHT  (fct_skin->face_light)    /* top highlights, separators */
#define C_FACE_SHADOW (fct_skin->face_shadow)   /* secondary text, subtle marks */
#define C_FACE_DARK   (fct_skin->face_dark)     /* outlines */
#define C_TEXT        (fct_skin->text)
#define C_DIM         (fct_skin->dim)
#define C_ACCENT      (fct_skin->accent)        /* focus, sia, workspace 1 */
#define C_BLUE        (fct_skin->blue)
#define C_TITLE_A1    (fct_skin->title_a1)      /* blue used by icons and graphs */
#define C_TITLE_A2    (fct_skin->title_a2)
#define C_DESK_TOP    (fct_skin->desk_top)
#define C_DESK_BOT    (fct_skin->desk_bot)
#define C_CONTENT     (fct_skin->content)       /* window content */
#define C_CONTENT_ALT (fct_skin->content_alt)   /* alternate rows */
#define C_LINE        (fct_skin->line)
#define C_SELECT      (fct_skin->select)        /* list selection */
#define C_MENU        (fct_skin->menu)
#define C_MENU_HOT    (fct_skin->menu_hot)
#define C_TITLEBAR    (fct_skin->titlebar)
#define C_TITLEBAR_I  (fct_skin->titlebar_i)
#define C_SPINE       (fct_skin->spine)
#define C_STRIP       (fct_skin->strip)
#define C_GOOD        (fct_skin->good)
#define C_BAD         (fct_skin->bad)

#endif
