/*
 * mount - list or add mounts
 *   mount                                  the mount table (/etc/mnttab)
 *   mount -F|-t type [-o ro,nosuid,remount] special dir
 *   mount -L label [-o ...] dir           the ext4 file system with that label, on any disk
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"
#include <sys/mount.h>
#include <dirent.h>
#include <fcntl.h>

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

/* The partition (or disk) under /dev/dsk whose ext4 volume label is label, in path. */
static bool find_label(const char *label, char *path, size_t n)
{
    DIR *dir = opendir("/dev/dsk");
    if (!dir)
        return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, n, "/dev/dsk/%s", e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        unsigned char sb[1024];
        if (pread(fd, sb, sizeof(sb), 1024) == (ssize_t)sizeof(sb) && sb[56] == 0x53 && sb[57] == 0xEF) {
            char name[17];                       /* s_volume_name, at 120 */
            memcpy(name, sb + 120, 16);
            name[16] = 0;
            found = !strcmp(name, label);
        }
        close(fd);
    }
    closedir(dir);
    return found;
}

int main(int argc, char **argv)
{
    const char *type = NULL, *label = NULL;
    unsigned long flags = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if ((!strcmp(argv[i], "-F") || !strcmp(argv[i], "-t")) && i + 1 < argc) {
            type = argv[++i];
        } else if (!strcmp(argv[i], "-L") && i + 1 < argc) {
            label = argv[++i];
            type = "ext4";
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
    char special[300];
    if (label) {
        if (argc - i != 1) {
            dprintf(STDERR_FILENO, "usage: mount -L label [-o ro,nosuid] dir\n");
            return 2;
        }
        if (!find_label(label, special, sizeof(special))) {
            dprintf(STDERR_FILENO, "mount: no file system labelled %s\n", label);
            return 1;
        }
        if (mount(special, argv[i], type, flags, NULL) < 0) {
            dprintf(STDERR_FILENO, "mount: %s: %s\n", argv[i], strerror(errno));
            return 1;
        }
        return 0;
    }
    if (argc - i != 2 || (!type && !(flags & MS_REMOUNT))) {
        dprintf(STDERR_FILENO, "usage: mount [-F type] [-o ro,nosuid,remount] special dir\n       mount -L label [-o ro,nosuid] dir\n");
        return 2;
    }
    if (mount(argv[i], argv[i + 1], type ? type : "", flags, NULL) < 0) {
        dprintf(STDERR_FILENO, "mount: %s: %s\n", argv[i + 1], strerror(errno));
        return 1;
    }
    return 0;
}
