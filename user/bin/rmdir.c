/* rmdir - remove empty directories */
#include "sieos.h"

int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: rmdir dir...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) < 0) {
            dprintf(STDERR_FILENO, "rmdir: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
