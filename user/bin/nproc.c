/*
 * nproc - print the number of online CPUs
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(void)
{
    struct cpuinfo ci[16];
    int n = cpuinfo(ci, 16), online = 0;
    for (int i = 0; i < n; i++)
        online += ci[i].online != 0;
    printf("%d\n", online ? online : 1);
    return 0;
}
