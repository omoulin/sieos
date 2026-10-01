/*
 * mir.h - MiR (Make it Real), the package mir: shared by its window
 * (facet-mir), its agent (mir-agent) and its tools (apptools.c).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef MIR_H
#define MIR_H

#define MIR_SHARE "/usr/pkg/share/mir"            /* the guide, the templates, the examples */
#define MIR_AGENT "/usr/pkg/libexec/mir-agent"    /* sia, with MiR's tools */

struct sia_session;
struct sbuf;

/* The application tools (apptools.c); project: the one to continue, or NULL */
void mir_add_tools(struct sia_session *s, const char *project);
/* MiR's instructions to the model (sia_set_instructions) */
void mir_instructions(struct sia_session *s, struct sbuf *out, void *ctx);

#endif
