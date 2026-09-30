/*
 * tty - print the terminal name
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(void)
{
    if (!isatty(STDIN_FILENO)) {
        printf("not a tty\n");
        return 1;
    }
    printf("/dev/console\n");
    return 0;
}
