/*
 * sleep - pause for N seconds
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        dprintf(STDERR_FILENO, "usage: sleep seconds\n");
        return 2;
    }
    return msleep(atoi(argv[1]) * 1000UL) < 0;
}
