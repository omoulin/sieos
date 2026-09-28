/* head - print the first lines of a file (head [-n N] [file]) */
#include "sieos.h"

int main(int argc, char **argv)
{
    int lines = 10, i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
        lines = atoi(argv[i + 1]);
        i += 2;
    }
    int fd = i < argc ? open(argv[i], O_RDONLY) : STDIN_FILENO;
    if (fd < 0) {
        dprintf(STDERR_FILENO, "head: %s: %s\n", argv[i], strerror(errno));
        return 1;
    }
    char buf[1024];
    long n;
    while (lines > 0 && (n = read(fd, buf, sizeof(buf))) > 0) {
        long k = 0;
        while (k < n && lines > 0)
            if (buf[k++] == '\n')
                lines--;
        write(STDOUT_FILENO, buf, k);
    }
    return 0;
}
