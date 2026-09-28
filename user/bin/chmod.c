/*
 * chmod - change file mode bits.
 *   chmod MODE file...     MODE is octal (755) or symbolic (u+x,go-w,a=r,+t)
 */
#include "sieos.h"

int umask_value(void);

static int apply_symbolic(const char *spec, int mode, bool is_dir)
{
    const char *p = spec;
    while (*p) {
        int who = 0;
        for (; *p && strchr("ugoa", *p); p++)
            who |= *p == 'u' ? 04700 : *p == 'g' ? 02070 : *p == 'o' ? 01007 : 07777;
        if (!who)
            who = 07777 & ~umask_value();
        while (*p && strchr("+-=", *p)) {
            char op = *p++;
            int bits = 0;
            for (; *p && strchr("rwxXst", *p); p++) {
                switch (*p) {
                case 'r': bits |= 0444; break;
                case 'w': bits |= 0222; break;
                case 'x': bits |= 0111; break;
                case 'X': if (is_dir || (mode & 0111)) bits |= 0111; break;
                case 's': bits |= 06000; break;
                case 't': bits |= 01000; break;
                }
            }
            bits &= who;
            if (op == '+')
                mode |= bits;
            else if (op == '-')
                mode &= ~bits;
            else
                mode = (mode & ~who) | bits;
        }
        if (*p == ',')
            p++;
        else if (*p)
            return -1;
    }
    return mode;
}

int umask_value(void)
{
    int m = umask(0);
    umask(m);
    return m;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        dprintf(STDERR_FILENO, "usage: chmod MODE file...\n");
        return 2;
    }
    const char *spec = argv[1];
    bool octal = true;
    for (const char *p = spec; *p; p++)
        if (*p < '0' || *p > '7')
            octal = false;
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            dprintf(STDERR_FILENO, "chmod: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        int mode = octal ? (int)strtol(spec, NULL, 8)
                         : apply_symbolic(spec, st.st_mode & 07777, S_ISDIR(st.st_mode));
        if (mode < 0) {
            dprintf(STDERR_FILENO, "chmod: invalid mode '%s'\n", spec);
            return 2;
        }
        if (chmod(argv[i], mode) < 0) {
            dprintf(STDERR_FILENO, "chmod: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
