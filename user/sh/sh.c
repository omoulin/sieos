/*
 * sh - The SIEOS shell. Built-in commands for the system and the files;
 * any other word is a program: /bin/NAME (or a path with a '/') is read
 * from the disk, started, and waited for.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static char io[16384];          /* file contents pass through here */

static const char *errstr(long e)
{
    switch (-e) {
    case EPERM: return "not allowed";             case ENOENT: return "no such file or directory";
    case EIO: return "input/output error";        case EBADF: return "bad handle";
    case EACCES: return "permission denied";      case EEXIST: return "already exists";
    case ENOTDIR: return "not a directory";       case EISDIR: return "is a directory";
    case EINVAL: return "invalid argument";       case ENOSPC: return "disk full";
    case ENAMETOOLONG: return "name too long";    case ENOTEMPTY: return "directory not empty";
    case ELOOP: return "too many symbolic links"; case EBUSY: return "busy";
    case 61: return "no such attribute";          case ENOMEM: return "out of memory";
    }
    return "error";
}
static int err(const char *what, long e) { if (e < 0) printf("%s: %s\n", what, errstr(e)); return e < 0; }

/* ---- System */
static int cmd_help(int, char **);
static int cmd_clear(int ac, char **av) { (void)ac; (void)av; printf("\033[H\033[J"); return 0; }

static int cmd_ps(int ac, char **av)
{
    static const char *st[] = { "free", "ready", "run", "send", "recv", "reply", "lookup", "dead", "sleep", "wait" };
    static mk_task_t t[32];
    (void)ac; (void)av;
    printf("  PID PPID  UID  CPU THR  STATE      MEM  NAME\n");
    long n;
    int after = 0;
    do {                                          /* 32 processes at a time, any number of them */
        n = sys_tasks(t, 32, after);
        for (long i = 0; i < n; i++)
            printf("%5d %4d %4d %4d %3d  %-6s %5lu K  %s\n", t[i].pid, t[i].ppid, t[i].uid, t[i].cpu,
                   t[i].threads, st[t[i].state], t[i].pages * 4, t[i].name);
        if (n > 0) after = t[n - 1].pid;
    } while (n == 32);
    return err("ps", n);
}

static int cmd_mem(int ac, char **av)
{
    mk_info_t in;
    (void)ac; (void)av;
    sys_info(&in);
    printf("memory: %lu KiB total, %lu KiB used, %lu KiB free\n", in.pages_total * 4,
           (in.pages_total - in.pages_free) * 4, in.pages_free * 4);
    printf("processes: %d, threads: %d, processors: %d\n", in.nprocs, in.nthreads, in.ncpus);
    return 0;
}

static int cmd_uptime(int ac, char **av)
{
    mk_info_t in;
    (void)ac; (void)av;
    sys_info(&in);
    uint64_t s = in.uptime_ns / 1000000000;
    printf("up %lu:%02lu:%02lu.%03lu, %d CPUs\n", s / 3600, s / 60 % 60, s % 60,
           in.uptime_ns / 1000000 % 1000, in.ncpus);
    return 0;
}

/* sleep SECONDS (decimals allowed: "sleep 0.25") */
static int cmd_sleep(int ac, char **av)
{
    uint64_t ns = 0, unit = 1000000000;
    if (ac < 2) return err("sleep", -EINVAL);
    const char *p = av[1];
    for (; *p >= '0' && *p <= '9'; p++) ns = ns * 10 + (uint64_t)(*p - '0') * unit;
    if (*p == '.') for (p++; *p >= '0' && *p <= '9' && unit > 1; p++) ns += (uint64_t)(*p - '0') * (unit /= 10);
    return err("sleep", sys_sleep(ns));
}

/* poweroff and reboot: commit the files first, so nothing written is lost. */
static int cmd_power(int ac, char **av) { (void)ac; fs_sync(); return err(av[0], sys_power(av[0][0] == 'r')); }

/* ---- Who am I. The kernel knows the numbers (SYS_IDENT); the names are
 * in /etc/users and /etc/groups. */
static mk_ident_t me;
static acct_user_t user;
static int cmd_whoami(int ac, char **av) { (void)ac; (void)av; printf("%s\n", user.name); return 0; }
static int cmd_id(int ac, char **av)
{
    char b[32];
    (void)ac; (void)av;
    printf("uid=%d(%s) gid=%d(%s) groups=", me.uid, user.name, me.gid, acct_group_name(me.gid, b, sizeof b));
    for (int i = 0; i < me.groups.n; i++) printf("%s%d(%s)", i ? "," : "", me.groups.g[i], acct_group_name(me.groups.g[i], b, sizeof b));
    printf("\n");
    return 0;
}
static int cmd_groups(int ac, char **av)
{
    char b[32];
    (void)ac; (void)av;
    printf("%s", acct_group_name(me.gid, b, sizeof b));
    for (int i = 0; i < me.groups.n; i++) printf(" %s", acct_group_name(me.groups.g[i], b, sizeof b));
    printf("\n");
    return 0;
}
static int cmd_logout(int ac, char **av) { (void)ac; (void)av; sys_exit(0); }
static int cmd_kill(int ac, char **av)
{
    int bad = ac < 2;
    for (int i = 1; i < ac; i++) { long pid = strnum(av[i]); bad |= err(av[i], pid > 0 ? sys_kill((int)pid) : -EINVAL); }
    return bad;
}

/* ---- Files */
static int cmd_pwd(int ac, char **av) { (void)ac; (void)av; printf("%s\n", fs_getcwd()); return 0; }
static int cmd_cd(int ac, char **av)  { return err("cd", fs_chdir(ac > 1 ? av[1] : user.home)); }

/* "drwxr-xr-x", and "2026-10-09 15:42" from ns since 1970 (UTC). */
static void mode_str(uint32_t m, char *s)
{
    uint32_t t = m & SIEFS_IFMT;
    s[0] = t == SIEFS_IFDIR ? 'd' : t == SIEFS_IFLNK ? 'l' : '-';
    for (int i = 0; i < 9; i++) s[1 + i] = m >> (8 - i) & 1 ? "rwxrwxrwx"[i] : '-';
    if (m & 01000) s[9] = s[9] == 'x' ? 't' : 'T';
    s[10] = 0;
}
static void date_str(int64_t ns, char *s)
{
    int64_t t = ns / 1000000000, d = t / 86400 + 719468, sec = t % 86400;   /* days -> civil date */
    int64_t era = d / 146097, doe = d - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t day = doy - (153 * mp + 2) / 5 + 1, mon = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (mon <= 2);
    int v[5] = { (int)y, (int)mon, (int)day, (int)(sec / 3600), (int)(sec / 60 % 60) };
    for (int i = 0; i < 5; i++) {
        int w = i ? 2 : 4, x = v[i];
        for (int k = w - 1; k >= 0; k--) { s[k] = (char)('0' + x % 10); x /= 10; }
        s += w;
        if (i < 4) *s++ = "-- :"[i];
    }
    *s = 0;
}

static void ls_one(const char *path, const char *name, int lng)
{
    siefs_stat_t st;
    if (!lng) { printf("%s\n", name); return; }
    if (err(name, fs_stat(path, &st, 1))) return;
    char m[11], d[20], link[256] = "";
    mode_str(st.mode, m);
    date_str(st.mtime, d);
    if ((st.mode & SIEFS_IFMT) == SIEFS_IFLNK) {
        long n = fs_readlink(path, link + 4, sizeof link - 5);
        if (n >= 0) { memcpy(link, " -> ", 4); link[4 + n] = 0; }
    }
    printf("%s %2u %4u %4u %8lu %s %s%s\n", m, st.nlink, st.uid, st.gid, st.size, d, name, link);
}

static int cmd_ls(int ac, char **av)
{
    int lng = 0, i = 1, bad = 0;
    if (i < ac && !strcmp(av[i], "-l")) { lng = 1; i++; }
    char *one[] = { ".", 0 };
    char **paths = i < ac ? av + i : one;
    for (; *paths; paths++) {
        siefs_stat_t st;
        if (err(*paths, fs_stat(*paths, &st, 0))) { bad = 1; continue; }
        if ((st.mode & SIEFS_IFMT) != SIEFS_IFDIR) { ls_one(*paths, *paths, lng); continue; }
        long h = fs_open(*paths, FS_RDONLY | FS_DIRECTORY, 0), n;
        if (err(*paths, h)) { bad = 1; continue; }
        uint64_t cur = 0, ino;
        int type;
        char name[256], full[FS_PATH + 256];
        while ((n = fs_readdir(h, &cur, io, sizeof io)) > 0)
            for (const char *p = io; fs_dirent(&p, io + n, &ino, &type, name); ) {
                if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) continue;
                if (lng) { size_t k = strlcpy(full, *paths, FS_PATH); full[k] = '/'; strlcpy(full + k + 1, name, 256); }
                ls_one(full, name, lng);
            }
        fs_close(h);
        bad |= err(*paths, n);
    }
    return bad;
}

static int cmd_cat(int ac, char **av)
{
    int bad = 0;
    for (int i = 1; i < ac; i++) {
        long h = fs_open(av[i], FS_RDONLY, 0), n;
        if (err(av[i], h)) { bad = 1; continue; }
        for (uint64_t off = 0; (n = fs_read(h, off, io, sizeof io)) > 0; off += n) con_write(io, n);
        fs_close(h);
        bad |= err(av[i], n);
    }
    return bad;
}

/* echo WORDS [> FILE | >> FILE] */
static int cmd_echo(int ac, char **av)
{
    const char *file = 0;
    int append = 0, n = 0;
    for (int i = 1; i < ac; i++) {
        if (av[i][0] != '>') continue;
        append = av[i][1] == '>';
        file = av[i] + 1 + append;
        if (!*file) file = i + 1 < ac ? av[i + 1] : 0;
        ac = i;
        if (!file) return err("echo", -EINVAL);
    }
    for (int i = 1; i < ac && n < (int)sizeof io - 2; i++) {
        n += strlcpy(io + n, av[i], sizeof io - n - 1);
        io[n++] = i < ac - 1 ? ' ' : '\n';
    }
    if (ac == 1) io[n++] = '\n';
    if (!file) return con_write(io, n) < 0;
    long h = fs_open(file, FS_WRONLY | FS_CREAT | (append ? FS_APPEND : FS_TRUNC), 0644);
    if (err(file, h)) return 1;
    long r = fs_write(h, 0, io, n);
    fs_close(h);
    return err(file, r);
}

/* The commands that call one file function per argument. */
static int cmd_simple(int ac, char **av)
{
    const char *c = av[0];
    long r = -EINVAL;
    long (*each)(const char *) = !strcmp(c, "rmdir") ? fs_rmdir : !strcmp(c, "rm") ? fs_unlink : 0;
    if (!strcmp(c, "mkdir")) { for (int i = 1; i < ac; i++) if (err(av[i], fs_mkdir(av[i], 0755))) return 1; return ac < 2 && err(c, r); }
    if (each) { for (int i = 1; i < ac; i++) if (err(av[i], each(av[i]))) return 1; return ac < 2 && err(c, r); }
    if (!strcmp(c, "mv") && ac == 3) r = fs_rename(av[1], av[2]);
    if (!strcmp(c, "ln") && ac == 3) r = fs_link(av[1], av[2]);
    if (!strcmp(c, "ln") && ac == 4 && !strcmp(av[1], "-s")) r = fs_symlink(av[2], av[3]);
    if (!strcmp(c, "chmod") && ac == 3) {
        int m = 0;
        for (const char *p = av[1]; *p >= '0' && *p <= '7'; p++) m = m * 8 + *p - '0';
        r = fs_chmod(av[2], m);
    }
    if (!strcmp(c, "chown") && ac == 3) {               /* chown UID[:GID] PATH */
        int u = 0, g = -1;
        const char *p = av[1];
        for (; *p >= '0' && *p <= '9'; p++) u = u * 10 + *p - '0';
        if (*p == ':') for (g = 0, p++; *p >= '0' && *p <= '9'; p++) g = g * 10 + *p - '0';
        r = fs_chown(av[2], u, g < 0 ? u : g);
    }
    if (!strcmp(c, "sync")) r = fs_sync();
    return err(c, r);
}

static int cmd_stat(int ac, char **av)
{
    for (int i = 1; i < ac; i++) {
        siefs_stat_t st;
        if (err(av[i], fs_stat(av[i], &st, 1))) return 1;
        char m[11], d[3][20];
        mode_str(st.mode, m);
        date_str(st.mtime, d[0]); date_str(st.ctime, d[1]); date_str(st.btime, d[2]);
        printf("%s: object %lu, %s, %lu bytes in %lu blocks, %u links, owner %u:%u\n"
               "  modified %s, changed %s, created %s\n",
               av[i], st.ino, m, st.size, st.blocks, st.nlink, st.uid, st.gid, d[0], d[1], d[2]);
    }
    return 0;
}

static int cmd_df(int ac, char **av)
{
    siefs_statfs_t s;
    (void)ac; (void)av;
    if (err("df", fs_statfs(&s))) return 1;
    printf("\"%s\": %lu KiB, %lu KiB free (+%lu KiB after the next commits), %lu objects\n",
           s.label, s.blocks * 4, s.free * 4, s.pending * 4, s.inodes);
    return 0;
}

/* ---- Attributes (tags): the base of Atlas's islands.
 *   tag FILE KEY=VALUE   tags FILE   untag FILE KEY   find KEY=VALUE */
static int cmd_tag(int ac, char **av)
{
    char *eq = ac == 3 ? strchr(av[2], '=') : 0;
    if (!eq) return err("tag", -EINVAL);
    *eq = 0;
    return err(av[1], fs_setxattr(av[1], av[2], eq + 1, strlen(eq + 1)));
}
static int cmd_tags(int ac, char **av)
{
    static char names[4096];
    if (ac != 2) return err("tags", -EINVAL);
    long n = fs_listxattr(av[1], names, sizeof names);
    if (err(av[1], n)) return 1;
    for (char *p = names; p < names + n; p += strlen(p) + 1) {
        long v = fs_getxattr(av[1], p, io, sizeof io - 1);
        if (v >= 0) { io[v] = 0; printf("%s=%s\n", p, io); }
    }
    return 0;
}
static int cmd_untag(int ac, char **av) { return ac == 3 ? err(av[1], fs_rmxattr(av[1], av[2])) : err("untag", -EINVAL); }
static int cmd_find(int ac, char **av)
{
    char *eq = ac == 2 ? strchr(av[1], '=') : 0;
    if (!eq) return err("find", -EINVAL);
    *eq = 0;
    uint64_t cur = 0;
    long n;
    do {
        if (err("find", n = fs_find(av[1], eq + 1, &cur, io, sizeof io))) return 1;
        for (char *p = io; p < io + n; p += strlen(p) + 1) printf("%s\n", p);
    } while (cur);
    return 0;
}

static const struct { const char *name, *help; int (*run)(int, char **); } cmds[] = {
    { "help",   "this list",                                cmd_help },
    { "ls",     "[-l] [PATH...]  list a directory",         cmd_ls },
    { "cd",     "[DIR]  change directory",                  cmd_cd },
    { "pwd",    "show the current directory",               cmd_pwd },
    { "cat",    "FILE...  show files",                      cmd_cat },
    { "echo",   "WORDS [> FILE | >> FILE]  print or write", cmd_echo },
    { "mkdir",  "DIR...",                                   cmd_simple },
    { "rmdir",  "DIR...",                                   cmd_simple },
    { "rm",     "FILE...",                                  cmd_simple },
    { "mv",     "FROM TO  rename or move",                  cmd_simple },
    { "ln",     "[-s] FROM TO  link (-s: symbolic)",        cmd_simple },
    { "chmod",  "MODE PATH  (octal: 644)",                  cmd_simple },
    { "chown",  "UID[:GID] PATH  (root only)",              cmd_simple },
    { "stat",   "PATH...  details",                         cmd_stat },
    { "df",     "disk space",                               cmd_df },
    { "sync",   "write everything to the disk now",         cmd_simple },
    { "tag",    "FILE KEY=VALUE  set an attribute",         cmd_tag },
    { "tags",   "FILE  list its attributes",                cmd_tags },
    { "untag",  "FILE KEY  remove an attribute",            cmd_untag },
    { "find",   "KEY=VALUE  files with this attribute",     cmd_find },
    { "ps",     "the running programs",                     cmd_ps },
    { "kill",   "PID...  end programs (yours; root: any)",  cmd_kill },
    { "mem",    "memory, processes, CPUs",                  cmd_mem },
    { "uptime", "time since start",                         cmd_uptime },
    { "sleep",  "SECONDS  wait (0.5 allowed)",              cmd_sleep },
    { "clear",  "clear the screen",                         cmd_clear },
    { "whoami", "your user name",                           cmd_whoami },
    { "id",     "your user and group numbers",              cmd_id },
    { "groups", "the groups you belong to",                 cmd_groups },
    { "logout", "end this session (also: exit)",            cmd_logout },
    { "PROGRAM","[ARGS] [&]  run /bin/PROGRAM (&: do not wait)", 0 },
    { "exit",   "end this session",                         cmd_logout },
    { "poweroff", "turn the machine off (root)",            cmd_power },
    { "reboot", "restart the machine (root)",               cmd_power },
};
#define NCMD (sizeof cmds / sizeof cmds[0])

static int cmd_help(int ac, char **av)
{
    (void)ac; (void)av;
    for (size_t i = 0; i < NCMD; i++) printf("  %-8s %s\n", cmds[i].name, cmds[i].help);
    printf("  Anything else runs a program: /bin/NAME, or a path with a '/'.\n");
    return 0;
}

/* A program from the disk; it gets "name args" as its argument string. */
static void program(int ac, char **av)
{
    int bg = ac > 1 && !strcmp(av[ac - 1], "&");
    if (bg) av[--ac] = 0;
    char path[FS_PATH], args[256];
    size_t n = 0;
    const char *base = av[0] + strlen(av[0]);
    while (base > av[0] && base[-1] != '/') base--;
    if (base != av[0]) strlcpy(path, av[0], sizeof path);
    else { strlcpy(path, "/bin/", sizeof path); strlcpy(path + 5, av[0], sizeof path - 5); }
    for (int i = 0; i < ac && n < sizeof args - 1; i++) {
        if (i) args[n++] = ' ';
        n += strlcpy(args + n, i ? av[i] : base, sizeof args - n);
    }
    if (n >= sizeof args) n = sizeof args - 1;
    args[n] = 0;
    long r;
    if (bg) {                                        /* "cmd &": do not wait for it */
        if ((r = spawn_file(path, args, -1, -1, 0)) > 0) { printf("[%ld]\n", r); return; }
    } else r = run(path, args);
    if (r == -ENOENT) printf("%s: unknown command (try 'help')\n", av[0]);
    else if (r == EXIT_KILLED) printf("%s: killed\n", av[0]);
    else if (r < 0) printf("%s: %s\n", av[0], errstr(r));
    else if (r) printf("%s: exit status %ld\n", av[0], r);
}

/* Programs started with '&' that have ended since the last prompt. */
static void reap(void)
{
    int st;
    long pid;
    while ((pid = sys_wait(-1, &st, WAIT_NOHANG)) > 0)
        if (st == EXIT_KILLED) printf("[%ld] killed\n", pid);
        else printf("[%ld] done, status %d\n", pid, st);
}

int main(int ac, char **av)
{
    char line[512], *v[32], host[32] = "sieos";
    long n;
    sys_ident(0, &me);
    if (acct_user(0, me.uid, &user)) { strlcpy(user.name, me.uid ? "?" : "root", sizeof user.name); strlcpy(user.home, "/", 2); }
    char *hn = file_get("/etc/hostname", 0);
    if (hn) { strlcpy(host, hn, sizeof host); char *e = strchr(host, '\n'); if (e) *e = 0; free(hn); }
    if (ac > 1 && !strcmp(av[1], "-l")) {          /* a login shell: start at home, say welcome */
        long h = fs_open("/etc/motd", FS_RDONLY, 0);
        printf("\n");
        if (h >= 0) { if ((n = fs_read(h, 0, io, sizeof io)) > 0) con_write(io, n); fs_close(h); }
        if (fs_chdir(user.home)) fs_chdir("/");
    }
    for (;;) {
        reap();
        printf("%s@%s:%s%s ", user.name, host, fs_getcwd(), me.uid ? "$" : "#");
        n = con_read(line, sizeof line - 1);
        if (n == -EIO) return 0;                      /* our terminal is gone (a closed window) */
        if (n <= 0) { if (n == 0) printf("\n"); continue; }
        line[n] = 0;
        int c = 0;                                    /* split into words; "..." keeps spaces */
        for (char *p = line, *o; *p && c < 31; ) {
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            if (!*p) break;
            v[c++] = o = p;
            for (int q = 0; *p && (q || (*p != ' ' && *p != '\t' && *p != '\n')); p++)
                if (*p == '"') q = !q; else *o++ = *p;
            if (*p) p++;
            *o = 0;
        }
        v[c] = 0;
        if (!c) continue;
        size_t i = 0;
        while (i < NCMD && strcmp(cmds[i].name, v[0])) i++;
        if (i < NCMD && cmds[i].run) cmds[i].run(c, v);
        else program(c, v);
    }
}
