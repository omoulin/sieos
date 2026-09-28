/*
 * uname - print system information
 *   -s system  -n node  -r release  -v version  -m machine  -p processor
 *   -i platform  -a all (-snrvmpi); no option: -s
 */
#include "sieos.h"
#include <sys/systeminfo.h>

int main(int argc, char **argv)
{
    struct utsname u;
    char arch[64] = "i386", plat[64] = "i86pc";
    uname(&u);
    sysinfo(SI_ARCHITECTURE, arch, sizeof(arch));
    sysinfo(SI_PLATFORM, plat, sizeof(plat));
    const char *fields[7] = { u.sysname, u.nodename, u.release, u.version, u.machine, arch, plat };
    const char *letters = "snrvmpi";
    bool want[7] = { false };
    int any = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || !argv[i][1])
            goto usage;
        for (const char *c = argv[i] + 1; *c; c++) {
            if (*c == 'a') {
                for (int k = 0; k < 7; k++)
                    want[k] = true;
                any = 1;
                continue;
            }
            const char *p = strchr(letters, *c);
            if (!p)
                goto usage;
            want[p - letters] = true;
            any = 1;
        }
    }
    if (!any)
        want[0] = true;
    for (int k = 0, first = 1; k < 7; k++)
        if (want[k]) {
            printf("%s%s", first ? "" : " ", fields[k]);
            first = 0;
        }
    printf("\n");
    return 0;
usage:
    dprintf(STDERR_FILENO, "usage: uname [-snrvmpia]\n");
    return 2;
}
