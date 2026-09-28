/* stat - display file status */
#include "sieos.h"

int main(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
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
        printf("  File: %s\n  Size: %lu\tBlocks: %lu\t%s\n", argv[i], st.st_size, st.st_blocks, type);
        printf(" Inode: %lu\tLinks: %u\tMode: %04o\tUid: %u\tGid: %u\n", st.st_ino, st.st_nlink,
               st.st_mode & 07777, st.st_uid, st.st_gid);
        printf("Access: %s\nModify: %s\nChange: %s\n", a, m, c);
    }
    return rc;
}
