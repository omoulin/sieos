/* cat - concatenate files to standard output */
#include "sieos.h"

static int cat_fd(int fd)
{
    char buf[4096];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        if (write(STDOUT_FILENO, buf, n) != n)
            return -1;
    return n < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2)
        return cat_fd(STDIN_FILENO) < 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "cat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (cat_fd(fd) < 0) {
            dprintf(STDERR_FILENO, "cat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
        close(fd);
    }
    return rc;
}
