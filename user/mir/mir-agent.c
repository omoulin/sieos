/*
 * mir-agent [PROJECT] - MiR's sia: libsia's agent loop (JSON lines on
 * stdin and stdout, as sia-agent's) with MiR's application tools and
 * instructions instead of the desktop's and the commands'.  facet-mir runs
 * it.  A model that has not been tested for sight is tested first (it
 * decides whether sia may look at the applications it makes).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "libsia.h"
#include "mir.h"

static void setup(struct sia_session *s, void *ctx)
{
    mir_add_tools(s, ctx);
    sia_set_instructions(s, mir_instructions, NULL);
    /* writing, building and testing a program takes many steps; a model that
     * reasons may think ten minutes before writing a whole program */
    sia_set_limits(s, 80, 160000, 600000);
}

int main(int argc, char **argv)
{
    struct sia_agent a = { SIA_ROLE_APP, setup, argc > 1 ? argv[1] : NULL, true };
    return sia_agent_main(&a);
}
