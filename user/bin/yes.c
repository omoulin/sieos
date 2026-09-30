/*
 * yes - repeatedly print a string (default "y")
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(int argc, char **argv)
{
    char buf[1024];
    const char *s = argc > 1 ? argv[1] : "y";
    size_t len = strlen(s), n = 0;
    while (n + len + 1 <= sizeof(buf)) {
        memcpy(buf + n, s, len);
        buf[n + len] = '\n';
        n += len + 1;
    }
    for (;;)
        if (write(STDOUT_FILENO, buf, n) < 0)
            return 1;
}
