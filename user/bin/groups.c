/* groups - print group memberships */
#include "sieos.h"

int main(int argc, char **argv)
{
    gid_t groups[NGROUPS_MAX];
    int n;
    if (argc > 1) {
        struct passwd *pw = getpwnam(argv[1]);
        if (!pw) {
            dprintf(STDERR_FILENO, "groups: %s: no such user\n", argv[1]);
            return 1;
        }
        n = NGROUPS_MAX;
        char name[64];
        strlcpy(name, pw->pw_name, sizeof(name));
        getgrouplist(name, pw->pw_gid, groups, &n);
    } else {
        n = getgroups(NGROUPS_MAX, groups);
    }
    for (int i = 0, shown = 0; i < n; i++) {
        bool dup = false;                      /* the list may repeat the primary group */
        for (int j = 0; j < i; j++)
            dup |= groups[j] == groups[i];
        if (dup)
            continue;
        struct group *g = getgrgid(groups[i]);
        if (g)
            printf("%s%s", shown++ ? " " : "", g->gr_name);
        else
            printf("%s%d", shown++ ? " " : "", (int)groups[i]);
    }
    printf("\n");
    return 0;
}
