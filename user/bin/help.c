/*
 * help - how to use the shell, and the programs there are
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

static void list(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    printf("Programs in %s:\n  ", dir);
    int col = 2;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        int len = strlen(e->d_name);
        if (col + len + 1 > 76) {
            printf("\n  ");
            col = 2;
        }
        printf("%s ", e->d_name);
        col += len + 1;
    }
    closedir(d);
    printf("\n");
}

int main(void)
{
    printf("/bin/sh is a POSIX shell (dash):\n"
           "  a | b   a && b   a || b   a ; b   a &   < > >> 2> 2>&1 <<EOF   $(cmd) $((1+2))\n"
           "  if/then/elif/else/fi  for x in ...; do ...; done  while/until  case ... esac  f() { ...; }\n"
           "  built-ins: cd pwd echo printf read test [ eval exec exit export unset set shift\n"
           "             trap wait jobs fg bg kill umask alias type command getopts . true false\n"
           "Keys: ^C interrupt  ^Z suspend  ^\\ quit  ^D end of input  ^U kill line\n"
           "/bin/sish is the earlier SIEOS shell.\n");
    list("/bin");
    struct stat st;
    if (stat("/usr/bin/gcc", &st) == 0)
        printf("Development tools in /usr/bin: gcc g++ cc cpp as ld ar nm objdump readelf strip ...\n");
    return 0;
}
