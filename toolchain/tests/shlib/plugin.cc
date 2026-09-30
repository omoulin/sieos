/*
 * plugin.cc
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
// Loaded by shtest with dlopen: its own TLS, a static object, and a C++ exception.
#include <stdexcept>
#include <string>

static thread_local int plugin_tls = 1234;
static std::string plugin_name = "plugin";
extern int plugin_unload_count;                           /* in shtest (-rdynamic) */

struct Unload { ~Unload() { plugin_unload_count++; } };
static Unload unload;

extern "C" int plugin_tls_value() { return plugin_tls++; }
extern "C" const char *plugin_get_name() { return plugin_name.c_str(); }
extern "C" void plugin_throw() { throw std::logic_error("from plugin"); }
