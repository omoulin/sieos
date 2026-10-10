/*
 * host.c - The output function of fmt.c for the reference programs, which
 * the host compiler builds and the development machine runs directly.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
long write(int fd, const void *p, unsigned long n);

int fmt_write(int fd, const char *p, long n)
{
    while (n > 0) {
        long w = write(fd, p, (unsigned long)n);
        if (w <= 0) return -1;
        p += w; n -= w;
    }
    return 0;
}
