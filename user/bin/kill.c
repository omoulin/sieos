/*
 * kill - send a signal to processes:  kill [-SIG | -s SIG | -l] pid...
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(int argc, char **argv)
{
    int sig = SIGTERM, i = 1, rc = 0;
    if (argc > 1 && !strcmp(argv[1], "-l")) {
        for (int s = 1; s < NSIG; s++)
            if (strcmp(signame(s), "?"))
                printf("%d) SIG%s\n", s, signame(s));
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "-s")) {
        sig = signum(argv[2]);
        i = 3;
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
        sig = signum(argv[1] + 1);
        i = 2;
    }
    if (sig < 0 || sig >= NSIG) {
        dprintf(STDERR_FILENO, "kill: invalid signal\n");
        return 2;
    }
    if (i >= argc) {
        dprintf(STDERR_FILENO, "usage: kill [-SIG] pid...\n");
        return 2;
    }
    for (; i < argc; i++) {
        if (kill(atoi(argv[i]), sig) < 0) {
            dprintf(STDERR_FILENO, "kill: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
