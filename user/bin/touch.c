/*
 * touch - create empty files if they do not exist
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: touch file...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "touch: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        close(fd);
    }
    return rc;
}
