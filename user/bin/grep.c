/*
 * grep - print lines matching a pattern.
 *   grep [-i] [-v] [-n] [-c] [-q] pattern [file...]
 * Patterns support ^ $ . * and character literals (Kernighan & Pike style).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

static bool opt_i, opt_v, opt_n, opt_c, opt_q;

static bool ceq(char a, char b)
{
    return opt_i ? tolower(a) == tolower(b) : a == b;
}

static bool match_here(const char *re, const char *text);

static bool match_star(char c, const char *re, const char *text)
{
    do {
        if (match_here(re, text))
            return true;
    } while (*text && (ceq(*text++, c) || c == '.'));   /* advance first: '.' matches any character */
    return false;
}

static bool match_here(const char *re, const char *text)
{
    if (!re[0])
        return true;
    if (re[1] == '*')
        return match_star(re[0], re + 2, text);
    if (re[0] == '$' && !re[1])
        return !*text;
    if (*text && (re[0] == '.' || ceq(re[0], *text)))
        return match_here(re + 1, text + 1);
    return false;
}

static bool match(const char *re, const char *text)
{
    if (re[0] == '^')
        return match_here(re + 1, text);
    do {
        if (match_here(re, text))
            return true;
    } while (*text++);
    return false;
}

static int grep_fd(const char *re, int fd, const char *name, bool show_name)
{
    static char buf[4096], line[1024];
    size_t ll = 0;
    long n, lineno = 0, count = 0;
    bool eof = false;
    while (!eof) {
        n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            eof = true;
            if (ll == 0)
                break;
            n = 0;
        }
        for (long i = 0; i <= n; i++) {
            bool end = (i == n) ? eof : buf[i] == '\n';
            if (i == n && !eof)
                break;
            if (!end) {
                if (ll + 1 < sizeof(line))
                    line[ll++] = buf[i];
                continue;
            }
            if (ll == 0 && eof && i == n)
                break;
            line[ll] = 0;
            lineno++;
            if (match(re, line) != opt_v) {
                count++;
                if (opt_q)
                    return 1;
                if (!opt_c) {
                    if (show_name)
                        printf("%s:", name);
                    if (opt_n)
                        printf("%ld:", lineno);
                    printf("%s\n", line);
                }
            }
            ll = 0;
        }
    }
    if (opt_c) {
        if (show_name)
            printf("%s:", name);
        printf("%ld\n", count);
    }
    return count > 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (char *o = argv[i] + 1; *o; o++)
            switch (*o) {
            case 'i': opt_i = true; break;
            case 'v': opt_v = true; break;
            case 'n': opt_n = true; break;
            case 'c': opt_c = true; break;
            case 'q': opt_q = true; break;
            default:
                dprintf(STDERR_FILENO, "usage: grep [-ivncq] pattern [file...]\n");
                return 2;
            }
    if (i >= argc) {
        dprintf(STDERR_FILENO, "usage: grep [-ivncq] pattern [file...]\n");
        return 2;
    }
    const char *re = argv[i++];
    int found = 0, err = 0;
    if (i >= argc)
        found = grep_fd(re, STDIN_FILENO, "(stdin)", false);
    for (int k = i; k < argc; k++) {
        int fd = open(argv[k], O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "grep: %s: %s\n", argv[k], strerror(errno));
            err = 1;
            continue;
        }
        found |= grep_fd(re, fd, argv[k], argc - i > 1);
        close(fd);
        if (found && opt_q)
            break;
    }
    return err ? 2 : found ? 0 : 1;
}
