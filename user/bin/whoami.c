/*
 * whoami - print the effective user name
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(void)
{
    struct passwd *pw = getpwuid(geteuid());
    if (pw)
        printf("%s\n", pw->pw_name);
    else
        printf("%d\n", geteuid());
    return 0;
}
