/*
 * util.c - hello-make's helpers.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "util.h"

const char *greeting(const char *who)
{
    static char b[64];
    strlcpy(b, "dear ", sizeof b);
    strlcpy(b + 5, who, sizeof b - 5);
    return b;
}

int sum_to(int n)
{
    int s = 0;
    for (int i = 1; i <= n; i++) s += i;
    return s;
}
