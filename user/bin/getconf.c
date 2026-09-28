/*
 * getconf - configuration values (POSIX)
 *   getconf NAME            system variable (sysconf) or string (confstr)
 *   getconf NAME PATH       file-system variable (pathconf)
 *   getconf -a              all of them
 */
#include "sieos.h"

struct var { const char *name; int kind, id; };   /* kind: 0 sysconf, 1 confstr, 2 pathconf, 3 constant */

static const struct var vars[] = {
    { "PATH", 1, _CS_PATH },
    { "ARG_MAX", 0, _SC_ARG_MAX },
    { "CHILD_MAX", 0, _SC_CHILD_MAX },
    { "CLK_TCK", 0, _SC_CLK_TCK },
    { "NGROUPS_MAX", 0, _SC_NGROUPS_MAX },
    { "OPEN_MAX", 0, _SC_OPEN_MAX },
    { "PAGESIZE", 0, _SC_PAGESIZE },
    { "PAGE_SIZE", 0, _SC_PAGESIZE },
    { "LINE_MAX", 0, _SC_LINE_MAX },
    { "HOST_NAME_MAX", 0, _SC_HOST_NAME_MAX },
    { "LOGIN_NAME_MAX", 0, _SC_LOGIN_NAME_MAX },
    { "TTY_NAME_MAX", 0, _SC_TTY_NAME_MAX },
    { "RE_DUP_MAX", 0, _SC_RE_DUP_MAX },
    { "_NPROCESSORS_CONF", 0, _SC_NPROCESSORS_CONF },
    { "_NPROCESSORS_ONLN", 0, _SC_NPROCESSORS_ONLN },
    { "NPROCESSORS_CONF", 0, _SC_NPROCESSORS_CONF },
    { "NPROCESSORS_ONLN", 0, _SC_NPROCESSORS_ONLN },
    { "_PHYS_PAGES", 0, _SC_PHYS_PAGES },
    { "_AVPHYS_PAGES", 0, _SC_AVPHYS_PAGES },
    { "_POSIX_VERSION", 0, _SC_VERSION },
    { "POSIX_VERSION", 0, _SC_VERSION },
    { "_POSIX2_VERSION", 0, _SC_2_VERSION },
    { "POSIX2_VERSION", 0, _SC_2_VERSION },
    { "_XOPEN_VERSION", 0, _SC_XOPEN_VERSION },
    { "SYMLOOP_MAX", 0, _SC_SYMLOOP_MAX },
    { "STREAM_MAX", 0, _SC_STREAM_MAX },
    { "TZNAME_MAX", 0, _SC_TZNAME_MAX },
    { "LINK_MAX", 2, _PC_LINK_MAX },
    { "NAME_MAX", 2, _PC_NAME_MAX },
    { "PATH_MAX", 2, _PC_PATH_MAX },
    { "PIPE_BUF", 2, _PC_PIPE_BUF },
    { "MAX_CANON", 2, _PC_MAX_CANON },
    { "MAX_INPUT", 2, _PC_MAX_INPUT },
    { "FILESIZEBITS", 2, _PC_FILESIZEBITS },
    { "LONG_BIT", 3, 64 },
    { "WORD_BIT", 3, 32 },
    { "CHAR_BIT", 3, 8 },
};
#define NVARS (int)(sizeof(vars) / sizeof(vars[0]))

static int show(const struct var *v, const char *path, bool name)
{
    const char *n = name ? v->name : NULL;
    if (v->kind == 1) {
        char buf[1024];
        size_t len = confstr(v->id, buf, sizeof(buf));
        if (!len)
            return -1;
        printf(n ? "%s: %s\n" : "%s%s\n", n ? n : "", buf);
        return 0;
    }
    long r;
    errno = 0;
    if (v->kind == 0)
        r = sysconf(v->id);
    else if (v->kind == 2)
        r = pathconf(path ? path : "/", v->id);
    else
        r = v->id;
    if (r == -1 && errno)
        return -1;
    if (r == -1)
        printf(n ? "%s: undefined\n" : "%sundefined\n", n ? n : "");
    else
        printf(n ? "%s: %ld\n" : "%s%ld\n", n ? n : "", r);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "-a")) {
        for (int i = 0; i < NVARS; i++)
            show(&vars[i], "/", true);
        return 0;
    }
    if (argc != 2 && argc != 3) {
        dprintf(STDERR_FILENO, "usage: getconf NAME [PATH] | getconf -a\n");
        return 2;
    }
    const char *name = argv[1];
    for (int i = 0; i < NVARS; i++)
        if (!strcmp(vars[i].name, name)) {
            if ((vars[i].kind == 2) != (argc == 3)) {
                dprintf(STDERR_FILENO, "getconf: %s: %s\n", name,
                        vars[i].kind == 2 ? "needs a path" : "takes no path");
                return 2;
            }
            if (show(&vars[i], argc == 3 ? argv[2] : NULL, false) < 0) {
                dprintf(STDERR_FILENO, "getconf: %s: %s\n", name, strerror(errno));
                return 1;
            }
            return 0;
        }
    dprintf(STDERR_FILENO, "getconf: %s: invalid variable name\n", name);
    return 1;
}
