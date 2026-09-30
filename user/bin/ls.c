/*
 * ls - list directory contents (-l long format, -a show hidden)
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"
#include <sys/sysmacros.h>

static bool opt_long, opt_all, opt_dir;

static void mode_str(unsigned int m, char *s)
{
    s[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' :
           S_ISFIFO(m) ? 'p' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        s[i + 1] = (m & (0400 >> i)) ? rwx[i] : '-';
    if (m & S_ISUID)
        s[3] = (m & 0100) ? 's' : 'S';
    if (m & S_ISGID)
        s[6] = (m & 0010) ? 's' : 'S';
    if (m & S_ISVTX)
        s[9] = (m & 0001) ? 't' : 'T';
    s[10] = 0;
}

static const char *uname_of(unsigned int uid)
{
    static char buf[16];
    struct passwd *pw = getpwuid(uid);
    if (pw)
        return strlcpy(buf, pw->pw_name, sizeof(buf)), buf;
    snprintf(buf, sizeof(buf), "%u", uid);
    return buf;
}

static const char *gname_of(unsigned int gid)
{
    static char buf[16];
    struct group *gr = getgrgid(gid);
    if (gr)
        return strlcpy(buf, gr->gr_name, sizeof(buf)), buf;
    snprintf(buf, sizeof(buf), "%u", gid);
    return buf;
}

static void print_entry(const char *dir, const char *name, int dtype)
{
    char path[512];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    bool have = lstat(path, &st) == 0;
    bool is_dir = have ? S_ISDIR(st.st_mode) : dtype == DT_DIR;
    bool is_link = have && S_ISLNK(st.st_mode);
    bool is_exec = have && S_ISREG(st.st_mode) && (st.st_mode & 0111);
    const char *color = is_dir ? "\033[1;34m" : is_link ? "\033[1;36m" : is_exec ? "\033[1;32m" : "";
    const char *reset = (is_dir || is_link || is_exec) ? "\033[0m" : "";
    if (opt_long) {
        char ms[11], date[32];
        if (!have) {
            printf("?????????? %s\n", name);
            return;
        }
        mode_str(st.st_mode, ms);
        strftime_simple(date, sizeof(date), st.st_mtime);
        date[16] = 0;                                  /* drop seconds */
        char size[24];
        if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))
            snprintf(size, sizeof(size), "%u, %u", major(st.st_rdev), minor(st.st_rdev));
        else
            snprintf(size, sizeof(size), "%lu", st.st_size);
        printf("%s %2u %-8s ", ms, st.st_nlink, uname_of(st.st_uid));
        char target[512] = "";
        if (is_link) {
            ssize_t n = readlink(path, target + 4, sizeof(target) - 5);
            if (n >= 0) {
                memcpy(target, " -> ", 4);
                target[4 + n] = 0;
            }
        }
        printf("%-8s %8s %s %s%s%s%s\n", gname_of(st.st_gid), size, date, color, name, reset, target);
    } else {
        printf("%s%s%s  ", color, name, reset);
    }
}

static int list(const char *path, bool header)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        dprintf(STDERR_FILENO, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode) || opt_dir) {
        const char *slash = strrchr(path, '/');
        char dir[256] = ".";
        if (slash) {
            size_t n = slash - path;
            memcpy(dir, path, n ? n : 1);
            dir[n ? n : 1] = 0;
        }
        print_entry(dir, slash ? slash + 1 : path, DT_REG);
        if (!opt_long)
            printf("\n");
        return 0;
    }
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (header)
        printf("%s:\n", path);
    char buf[2048];
    long n;
    int count = 0;
    while ((n = getdents(fd, (struct dirent *)buf, sizeof(buf))) > 0) {
        for (long off = 0; off < n;) {
            struct dirent *d = (struct dirent *)(buf + off);
            off += d->d_reclen;
            if (d->d_name[0] == '.' && !opt_all)
                continue;
            print_entry(path, d->d_name, d->d_type);
            count++;
        }
    }
    if (!opt_long && count)
        printf("\n");
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        for (char *o = argv[first] + 1; *o; o++) {
            if (*o == 'l') opt_long = true;
            else if (*o == 'a') opt_all = true;
            else if (*o == 'd') opt_dir = true;
            else {
                dprintf(STDERR_FILENO, "usage: ls [-lad] [path...]\n");
                return 2;
            }
        }
    }
    if (first == argc)
        return list(".", false);
    int rc = 0;
    for (int i = first; i < argc; i++) {
        struct stat st;
        bool dir = stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode) && !opt_dir;
        rc |= list(argv[i], argc - first > 1 && dir);
        if (i + 1 < argc && dir)
            printf("\n");
    }
    return rc;
}
