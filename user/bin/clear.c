/*
 * clear - clear the screen
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(void)
{
    write(STDOUT_FILENO, "\f", 1);
    return 0;
}
