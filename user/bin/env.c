/*
 * env - run a program in a modified environment, or print the environment.
 *   env [-i] [-u NAME] [NAME=value...] [command [args...]]
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "-")) {
            clearenv();
        } else if (!strcmp(argv[i], "-u") && i + 1 < argc) {
            unsetenv(argv[++i]);
        } else if (strchr(argv[i], '=') && argv[i][0] != '=') {
            putenv(argv[i]);
        } else {
            break;
        }
    }
    if (i == argc) {
        for (char **e = environ; e && *e; e++)
            printf("%s\n", *e);
        return 0;
    }
    execvp(argv[i], &argv[i]);
    dprintf(STDERR_FILENO, "env: %s: %s\n", argv[i], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}
