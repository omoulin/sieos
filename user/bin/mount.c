/*
 * mount - list or add mounts
 *   mount                                  the mount table (/etc/mnttab)
 *   mount -F|-t type [-o ro,nosuid,remount] special dir
 */
#include "sieos.h"
#include <sys/mount.h>

static int list(void)
{
    FILE *f = fopen("/etc/mnttab", "r");
    if (!f) {
        perror("mount: /etc/mnttab");
        return 1;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char spec[64], dir[128], type[32], opts[96];
        long t = 0;
        if (sscanf(line, "%63s %127s %31s %95s %ld", spec, dir, type, opts, &t) < 4)
            continue;
        char when[32];
        strftime_simple(when, sizeof(when), t);
        printf("%s on %s type %s (%s) since %s\n", spec, dir, type, opts, when);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *type = NULL;
    unsigned long flags = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if ((!strcmp(argv[i], "-F") || !strcmp(argv[i], "-t")) && i + 1 < argc) {
            type = argv[++i];
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            char *opts = argv[++i], *o;
            while ((o = strsep(&opts, ",")) != NULL) {
                if (!strcmp(o, "ro"))
                    flags |= MS_RDONLY;
                else if (!strcmp(o, "nosuid"))
                    flags |= MS_NOSUID;
                else if (!strcmp(o, "remount"))
                    flags |= MS_REMOUNT;
                else if (strcmp(o, "rw") && *o) {
                    dprintf(STDERR_FILENO, "mount: unknown option %s\n", o);
                    return 2;
                }
            }
        } else {
            break;
        }
    }
    if (i == argc && !type && !flags)
        return list();
    if (argc - i != 2 || (!type && !(flags & MS_REMOUNT))) {
        dprintf(STDERR_FILENO, "usage: mount [-F type] [-o ro,nosuid,remount] special dir\n");
        return 2;
    }
    if (mount(argv[i], argv[i + 1], type ? type : "", flags, NULL) < 0) {
        dprintf(STDERR_FILENO, "mount: %s: %s\n", argv[i + 1], strerror(errno));
        return 1;
    }
    return 0;
}
