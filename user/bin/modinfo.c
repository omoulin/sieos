/*
 * modinfo - the drivers: those of the boot archive and of /drv, loaded
 * (their devices found) or not.
 *   modinfo        Id, load address, size, phase, state, name, description
 *   modinfo -a     with the alias each driver's first device matched
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"
#include "sieos/sysinfo.h"

int main(int argc, char **argv)
{
    bool aliases = argc > 1 && !strcmp(argv[1], "-a");
    static const char *const phase[] = { "display", "boot", "root" };
    static const char *const state[] = { "-", "loaded", "FAILED" };
    printf("%3s %-18s %7s %-7s %-6s %-12s %s\n", "Id", "Loadaddr", "Size", "Phase", "State", "Module", "Description");
    struct sieos_modinfo mi;
    for (int i = 0; modinfo(&mi, i) == 0; i++) {
        printf("%3d %18lx %7u %-7s %-6s %-12s %s\n", i, mi.mi_base, mi.mi_size,
               mi.mi_phase >= 0 && mi.mi_phase <= 2 ? phase[mi.mi_phase] : "?",
               mi.mi_state >= 0 && mi.mi_state <= 2 ? state[mi.mi_state] : "?", mi.mi_name, mi.mi_desc);
        if (aliases && mi.mi_alias[0])
            printf("%38s%s (%d device%s, from the %s)\n", "", mi.mi_alias, mi.mi_ndev, mi.mi_ndev == 1 ? "" : "s",
                   mi.mi_source);
    }
    return 0;
}
