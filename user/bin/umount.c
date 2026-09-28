/* umount - remove mounts: umount dir... */
#include "sieos.h"
#include <sys/mount.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: umount dir...\n");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++)
        if (umount(argv[i]) < 0) {
            dprintf(STDERR_FILENO, "umount: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    return rc;
}
