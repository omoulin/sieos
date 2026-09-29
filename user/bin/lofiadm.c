/*
 * lofiadm - attach files as block devices (/dev/lofi/N), as on Solaris
 *   lofiadm                     list the attached files
 *   lofiadm -a file [-r]        attach (read-only with -r); prints the device
 *   lofiadm -d file|/dev/lofi/N detach
 */
#include "sieos.h"
#include "sieos/lofi.h"

static int ctl(void)
{
    int fd = open("/dev/lofictl", O_RDWR);
    if (fd < 0) {
        perror("lofiadm: /dev/lofictl");
        exit(1);
    }
    return fd;
}

static int filename(int fd, unsigned minor, struct sieos_lofi_ioctl *li)
{
    memset(li, 0, sizeof(*li));
    li->li_minor = minor;
    return ioctl(fd, SIEOS_LOFI_GET_FILENAME, li);
}

int main(int argc, char **argv)
{
    struct sieos_lofi_ioctl li;
    if (argc == 1) {
        int fd = ctl();
        printf("%-16s %s\n", "Block Device", "File");
        for (unsigned m = 1; m <= SIEOS_LOFI_MAX_FILES; m++)
            if (filename(fd, m, &li) == 0)
                printf("/dev/lofi/%-6u %s\n", m, li.li_filename);
        return 0;
    }
    if (!strcmp(argv[1], "-a") && (argc == 3 || (argc == 4 && !strcmp(argv[3], "-r")))) {
        int fd = ctl();
        memset(&li, 0, sizeof(li));
        if (argv[2][0] != '/') {                     /* the kernel keeps the name: make it absolute */
            char cwd[512];
            if (!getcwd(cwd, sizeof(cwd)))
                strcpy(cwd, "/");
            snprintf(li.li_filename, sizeof(li.li_filename), "%s/%s", strcmp(cwd, "/") ? cwd : "", argv[2]);
        } else {
            snprintf(li.li_filename, sizeof(li.li_filename), "%s", argv[2]);
        }
        li.li_readonly = argc == 4;
        if (ioctl(fd, SIEOS_LOFI_MAP_FILE, &li) < 0) {
            dprintf(STDERR_FILENO, "lofiadm: %s: %s\n", argv[2], strerror(errno));
            return 1;
        }
        printf("/dev/lofi/%u\n", li.li_minor);
        return 0;
    }
    if (!strcmp(argv[1], "-d") && argc == 3) {
        int fd = ctl();
        unsigned minor = 0;
        if (!strncmp(argv[2], "/dev/lofi/", 10)) {
            minor = atoi(argv[2] + 10);
        } else {                                     /* a file: find its device */
            struct stat a, b;
            if (stat(argv[2], &a) < 0) {
                dprintf(STDERR_FILENO, "lofiadm: %s: %s\n", argv[2], strerror(errno));
                return 1;
            }
            for (unsigned m = 1; m <= SIEOS_LOFI_MAX_FILES && !minor; m++)
                if (filename(fd, m, &li) == 0 && stat(li.li_filename, &b) == 0 && a.st_ino == b.st_ino &&
                    a.st_dev == b.st_dev)
                    minor = m;
        }
        memset(&li, 0, sizeof(li));
        li.li_minor = minor;
        if (ioctl(fd, SIEOS_LOFI_UNMAP_FILE_MINOR, &li) < 0) {
            dprintf(STDERR_FILENO, "lofiadm: %s: %s\n", argv[2], strerror(errno));
            return 1;
        }
        return 0;
    }
    dprintf(STDERR_FILENO, "usage: lofiadm [-a file [-r] | -d file|/dev/lofi/N]\n");
    return 2;
}
