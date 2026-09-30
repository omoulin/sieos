/*
 * id - print user and group identities
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

static void print_user(const char *label, uid_t uid)
{
    struct passwd *pw = getpwuid(uid);
    printf("%s=%d", label, uid);
    if (pw)
        printf("(%s)", pw->pw_name);
}

static void print_group(const char *label, gid_t gid)
{
    struct group *gr = getgrgid(gid);
    if (label)
        printf("%s=", label);
    printf("%d", gid);
    if (gr)
        printf("(%s)", gr->gr_name);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-u")) {
        printf("%d\n", geteuid());
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "-un")) {
        struct passwd *pw = getpwuid(geteuid());
        printf("%s\n", pw ? pw->pw_name : "?");
        return 0;
    }
    uid_t uid = getuid(), euid = geteuid();
    gid_t gid = getgid(), egid = getegid();
    print_user("uid", uid);
    printf(" ");
    print_group("gid", gid);
    if (euid != uid) {
        printf(" ");
        print_user("euid", euid);
    }
    if (egid != gid) {
        printf(" ");
        print_group("egid", egid);
    }
    gid_t groups[NGROUPS_MAX];
    int n = getgroups(NGROUPS_MAX, groups);
    if (n > 0) {
        printf(" groups=");
        for (int i = 0, shown = 0; i < n; i++) {
            bool dup = false;                  /* the list may repeat the primary group */
            for (int j = 0; j < i; j++)
                dup |= groups[j] == groups[i];
            if (dup)
                continue;
            if (shown++)
                printf(",");
            print_group(NULL, groups[i]);
        }
    }
    printf("\n");
    return 0;
}
