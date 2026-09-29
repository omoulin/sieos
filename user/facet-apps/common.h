/*
 * common.h - The Facet desktop's applications (separate programs on
 * libfacet): shared definitions.
 */
#ifndef FACET_APPS_COMMON_H
#define FACET_APPS_COMMON_H

#include "sieos.h"
#include <facet/facet.h>

#define SB_W 16
void draw_scrollbar(struct surface *s, struct rect r, int first, int visible, int total);
int scrollbar_click(struct rect r, int y, int first, int visible, int total);   /* the new first line */
bool desktop_request(const char *json, char *result, size_t n);                  /* the desktop channel */

#endif
