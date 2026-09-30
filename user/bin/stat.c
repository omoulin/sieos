/*
 * stat [-L] file... - display file status (of a symbolic link itself; -L: of what it points to)
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(int argc, char **argv)
{
    int rc = 0, i = 1;
    bool follow = false;
    if (argc > 1 && !strcmp(argv[1], "-L")) {
        follow = true;
        i++;
    }
    for (; i < argc; i++) {
        struct stat st;
        if ((follow ? stat(argv[i], &st) : lstat(argv[i], &st)) < 0) {
            dprintf(STDERR_FILENO, "stat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        char m[32], a[32], c[32];
        strftime_simple(m, sizeof(m), st.st_mtime);
        strftime_simple(a, sizeof(a), st.st_atime);
        strftime_simple(c, sizeof(c), st.st_ctime);
        const char *type = S_ISDIR(st.st_mode) ? "directory" : S_ISREG(st.st_mode) ? "regular file" :
                           S_ISLNK(st.st_mode) ? "symbolic link" : "special file";
        char target[512] = "";
        if (S_ISLNK(st.st_mode)) {
            ssize_t n = readlink(argv[i], target + 4, sizeof(target) - 5);
            if (n >= 0) {
                memcpy(target, " -> ", 4);
                target[4 + n] = 0;
            }
        }
        printf("  File: %s%s\n  Size: %lu\tBlocks: %lu\t%s\n", argv[i], target, st.st_size, st.st_blocks, type);
        printf(" Inode: %lu\tLinks: %u\tMode: %04o\tUid: %u\tGid: %u\n", st.st_ino, st.st_nlink,
               st.st_mode & 07777, st.st_uid, st.st_gid);
        printf("Access: %s\nModify: %s\nChange: %s\n", a, m, c);
    }
    return rc;
}
