/*
 * sia-agent - libsia without a terminal: one JSON object per line on
 * stdin/stdout.  Facet runs it for the sia strip; any program can.
 * The loop is libsia's (sia_agent_main, agent.c); this is it with the
 * desktop's tools and the commands'.
 *
 * Requests (stdin):
 *   {"op":"ask","text":"open a terminal for me"}
 *   {"op":"confirm","answer":0|1|2}     reply to a "confirm" event (no, yes, always)
 *   {"op":"clear"}                      forget the conversation
 *   {"op":"reload"}                     re-read ~/.sia/config
 * Events (stdout):
 *   {"ev":"ready","model":"...","vision":true|false}   {"ev":"unavailable","text":"why"}
 *   {"ev":NAME,"text":"..."}       a tool's news (sia_notify), e.g. MiR's "project"
 *   {"ev":"thinking","on":true}    {"ev":"tool","text":"ls -l /etc"}
 *   {"ev":"output","text":"..."}   {"ev":"text","text":"..."}
 *   {"ev":"confirm","text":"rm x"} {"ev":"error","text":"..."}
 *   {"ev":"done","ok":true}
 * SIGINT interrupts the request in progress.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "libsia.h"

static void setup(struct sia_session *s, void *ctx)
{
    (void)ctx;
    sia_add_desktop_tools(s);
    sia_add_command_tools(s);
}

int main(void)
{
    static const struct sia_agent a = { SIA_ROLE_DESKTOP, setup, NULL, false };
    return sia_agent_main(&a);
}
