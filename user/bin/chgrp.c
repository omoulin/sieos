/*
 * chown / chgrp - change file owner and group.
 *   chown user[:group] file...     chgrp group file...
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

static int parse_user(const char *s)
{
    if (isdigit(*s))
        return atoi(s);
    struct passwd *pw = getpwnam(s);
    return pw ? (int)pw->pw_uid : -2;
}

static int parse_group(const char *s)
{
    if (isdigit(*s))
        return atoi(s);
    struct group *gr = getgrnam(s);
    return gr ? (int)gr->gr_gid : -2;
}

int main(int argc, char **argv)
{
    const char *prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    bool chgrp = !strcmp(prog, "chgrp");
    if (argc < 3) {
        dprintf(STDERR_FILENO, chgrp ? "usage: chgrp group file...\n" : "usage: chown user[:group] file...\n");
        return 2;
    }
    char spec[128];
    strlcpy(spec, argv[1], sizeof(spec));
    int uid = -1, gid = -1;
    if (chgrp) {
        gid = parse_group(spec);
    } else {
        char *colon = strchr(spec, ':');
        if (!colon)
            colon = strchr(spec, '.');
        if (colon) {
            *colon = 0;
            if (colon[1])
                gid = parse_group(colon + 1);
        }
        if (spec[0])
            uid = parse_user(spec);
    }
    if (uid == -2 || gid == -2) {
        dprintf(STDERR_FILENO, "%s: unknown user or group '%s'\n", prog, argv[1]);
        return 1;
    }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        if (chown(argv[i], uid, gid) < 0) {
            dprintf(STDERR_FILENO, "%s: %s: %s\n", prog, argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
