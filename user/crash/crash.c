/*
 * crash - Fails at once (exit status 3): used to test that init backs off
 * and gives up on a service that cannot start (svc add crasher /bin/crash).
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

int main(int argc, char **argv) { (void)argc; (void)argv; return 3; }
