/*
 * wc - count lines, words and bytes: wc [-lwc] [file...]
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
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

static bool want_l, want_w, want_c;

static void show(unsigned long l, unsigned long w, unsigned long c, const char *name)
{
    if (want_l)
        printf("%7lu ", l);
    if (want_w)
        printf("%7lu ", w);
    if (want_c)
        printf("%7lu ", c);
    printf(name ? "%s\n" : "\n", name);
}

/* wc [-lwc] [file...] */
int main(int argc, char **argv)
{
    unsigned long l, w, c, tl = 0, tw = 0, tc = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'l')
                want_l = true;
            else if (*o == 'w')
                want_w = true;
            else if (*o == 'c' || *o == 'm')
                want_c = true;
            else {
                dprintf(STDERR_FILENO, "usage: wc [-lwc] [file...]\n");
                return 2;
            }
        }
    if (!want_l && !want_w && !want_c)
        want_l = want_w = want_c = true;
    if (i == argc) {
        count(STDIN_FILENO, &l, &w, &c);
        show(l, w, c, NULL);
        return 0;
    }
    int rc = 0, files = argc - i;
    for (; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "wc: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        count(fd, &l, &w, &c);
        close(fd);
        show(l, w, c, argv[i]);
        tl += l;
        tw += w;
        tc += c;
    }
    if (files > 1)
        show(tl, tw, tc, "total");
    return rc;
}
