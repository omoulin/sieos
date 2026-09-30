/*
 * stdio.c - libsieos: the SIEOS programs write their output as they
 * produce it (prompts without a newline, output mixed with write(2) and
 * dprintf, fork after printf), so stdout is unbuffered.  Every program
 * links this object (-u __sieos_stdio).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int __sieos_stdio;

__attribute__((constructor)) static void unbuffered_stdout(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
}
