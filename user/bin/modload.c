/*
 * modload - load a driver (an ELF relocatable object built against
 * /usr/include/ddk, see /usr/share/ddk/README): its devices are matched
 * against its aliases and its _init called.  A driver copied to /drv is
 * loaded at every boot when one of its devices is found.
 *   modload FILE.drv
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: modload FILE.drv\n");
        return 2;
    }
    char path[PATH_MAX];
    if (argv[1][0] != '/' && getcwd(path, sizeof(path)))
        snprintf(path + strlen(path), sizeof(path) - strlen(path), "/%s", argv[1]);
    else
        snprintf(path, sizeof(path), "%s", argv[1]);
    if (modload(path) < 0) {
        fprintf(stderr, "modload: %s: %s%s\n", argv[1], strerror(errno),
                errno == ENOEXEC ? " (not a driver, or it failed: see dmesg)" : "");
        return 1;
    }
    printf("modload: %s loaded\n", argv[1]);
    return 0;
}
