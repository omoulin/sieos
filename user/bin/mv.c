/* mv - move or rename files and directories:  mv src dst | mv src... dir */
#include "sieos.h"

static int move(const char *src, const char *dst)
{
    struct stat st;
    char target[512];
    if (stat(dst, &st) == 0 && S_ISDIR(st.st_mode)) {
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        snprintf(target, sizeof(target), "%s/%s", dst, base);
        dst = target;
    }
    if (rename(src, dst) < 0) {
        dprintf(STDERR_FILENO, "mv: %s -> %s: %s\n", src, dst, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        dprintf(STDERR_FILENO, "usage: mv src dst | mv src... dir\n");
        return 2;
    }
    struct stat st;
    if (argc > 3 && (stat(argv[argc - 1], &st) < 0 || !S_ISDIR(st.st_mode))) {
        dprintf(STDERR_FILENO, "mv: target '%s' is not a directory\n", argv[argc - 1]);
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc - 1; i++)
        rc |= move(argv[i], argv[argc - 1]);
    return rc;
}
