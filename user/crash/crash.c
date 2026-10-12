/*
 * crash - Fails at once (exit status 3): used to test that init backs off
 * and gives up on a service that cannot start (svc add crasher /bin/crash).
 * "crash hang PORT": a server that is stuck (it has PORT but never
 * answers): init's watcher must end it (svc add -w PORT ...).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "hang")) {
        if (port_create(argv[2]) < 0) return 4;
        for (;;) sys_sleep(1000000000000ULL);     /* a port, and never an answer */
    }
    return 3;
}
