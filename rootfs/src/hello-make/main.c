/*
 * main.c - hello-make's program: it greets, and adds up with util.c.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "util.h"

int main(int argc, char **argv)
{
    printf("hello from make: %s, sum 1..10 = %d\n", greeting(argc > 1 ? argv[1] : "SIEOS"), sum_to(10));
    return 0;
}
