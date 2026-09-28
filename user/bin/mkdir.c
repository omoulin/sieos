/* mkdir - create directories (-p: create parents, no error if existing) */
#include "sieos.h"

static int mkdir_p(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(path, 0755) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 0, i = 1;
    bool parents = argc > 1 && strcmp(argv[1], "-p") == 0;
    if (parents)
        i++;
    if (i >= argc) {
        dprintf(STDERR_FILENO, "usage: mkdir [-p] dir...\n");
        return 2;
    }
    for (; i < argc; i++) {
        int r = parents ? mkdir_p(argv[i]) : mkdir(argv[i], 0755);
        if (r < 0) {
            dprintf(STDERR_FILENO, "mkdir: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
