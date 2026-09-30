/*
 * facet/settings.h - The desktop's settings (libfacet).
 *
 * Each user's are in ~/.facet/settings, one "key=value" per line; the
 * system's defaults (and the login screen's) in /etc/facet/settings.
 *   skin        strata, beos or irix (default beos)
 *   resolution  WIDTHxHEIGHT of the screen Facet runs on (default: as booted)
 * Facet applies them when a session starts and saves them when they change.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef FACET_SETTINGS_H
#define FACET_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#define FCT_SETTINGS_SYSTEM "/etc/facet/settings"

/* The user's value, else the system's; false if neither has the key. */
bool fct_setting_get(const char *key, char *out, size_t n);
bool fct_setting_get_system(const char *key, char *out, size_t n);
/* Set (value non-NULL) or remove (NULL) the user's value. */
bool fct_setting_set(const char *key, const char *value);
/* The user's settings file ("~/.facet/settings"), for display. */
const char *fct_settings_path(void);

#endif
