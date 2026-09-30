/*
 * useradd - create a user account (root only).
 *   useradd [-u uid] [-g group] [-d home] [-s shell] [-c comment] name
 * The account is locked until a password is set with passwd(1).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

static int append(const char *path, const char *line)
{
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0)
        return -1;
    long n = write(fd, line, strlen(line));
    close(fd);
    return n == (long)strlen(line) ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (geteuid() != 0) {
        dprintf(STDERR_FILENO, "useradd: permission denied (must be root)\n");
        return 1;
    }
    int uid = -1, gid = 10;
    const char *home = NULL, *shell = "/bin/sh", *comment = "", *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-u") && i + 1 < argc) uid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) {
            const char *g = argv[++i];
            struct group *gr = isdigit(*g) ? NULL : getgrnam(g);
            gid = gr ? (int)gr->gr_gid : atoi(g);
        }
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) home = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) shell = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) comment = argv[++i];
        else name = argv[i];
    }
    if (!name || strchr(name, ':') || !name[0]) {
        dprintf(STDERR_FILENO, "usage: useradd [-u uid] [-g group] [-d home] [-s shell] [-c comment] name\n");
        return 2;
    }
    if (getpwnam(name)) {
        dprintf(STDERR_FILENO, "useradd: user %s already exists\n", name);
        return 1;
    }
    if (uid < 0) {
        uid = 100;
        while (getpwuid(uid))
            uid++;
    }
    char homebuf[128], line[256];
    if (!home) {
        snprintf(homebuf, sizeof(homebuf), "/home/%s", name);
        home = homebuf;
    }
    snprintf(line, sizeof(line), "%s:x:%d:%d:%s:%s:%s\n", name, uid, gid, comment, home, shell);
    if (append("/etc/passwd", line) < 0) {
        perror("useradd: /etc/passwd");
        return 1;
    }
    snprintf(line, sizeof(line), "%s:!:::::::\n", name);
    if (append("/etc/shadow", line) < 0) {
        perror("useradd: /etc/shadow");
        return 1;
    }
    if (mkdir(home, 0755) == 0 || errno == EEXIST)
        chown(home, uid, gid);
    printf("useradd: created user %s (uid %d, gid %d, home %s)\n", name, uid, gid, home);
    printf("useradd: run 'passwd %s' to set a password\n", name);
    return 0;
}
