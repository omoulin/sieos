/* wc - count lines, words and bytes */
#include "sieos.h"

static void count(int fd, unsigned long *l, unsigned long *w, unsigned long *c)
{
    char buf[4096];
    long n;
    bool inword = false;
    *l = *w = *c = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        *c += n;
        for (long i = 0; i < n; i++) {
            if (buf[i] == '\n')
                (*l)++;
            if (isspace(buf[i])) {
                inword = false;
            } else if (!inword) {
                inword = true;
                (*w)++;
            }
        }
    }
}

int main(int argc, char **argv)
{
    unsigned long l, w, c, tl = 0, tw = 0, tc = 0;
    if (argc < 2) {
        count(STDIN_FILENO, &l, &w, &c);
        printf("%7lu %7lu %7lu\n", l, w, c);
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "wc: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        count(fd, &l, &w, &c);
        close(fd);
        printf("%7lu %7lu %7lu %s\n", l, w, c, argv[i]);
        tl += l;
        tw += w;
        tc += c;
    }
    if (argc > 2)
        printf("%7lu %7lu %7lu total\n", tl, tw, tc);
    return rc;
}
