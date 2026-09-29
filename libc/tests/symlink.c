/*
 * symlink.c - symbolic links (M22): following in every path lookup, the
 * no-follow calls, loops, trailing slashes, creation through dangling links,
 * long targets, links in /proc, realpath, exec through a link.  Runs in a
 * scratch directory under /tmp (tmpfs) and under $HOME (ext4).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int fails;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d [%s]: %s (errno %d %s)\n", __FILE__, __LINE__, base, #c, errno, strerror(errno)); fails++; } } while (0)

static char base[PATH_MAX];

static char *P(const char *rel)
{
    static char b[8][PATH_MAX];
    static int k;
    k = (k + 1) % 8;
    snprintf(b[k], sizeof(b[k]), "%s/%s", base, rel);
    return b[k];
}

static void put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, text, strlen(text));
        close(fd);
    }
}

static bool has(const char *path, const char *text)
{
    char buf[256];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n == (int)strlen(text) && !memcmp(buf, text, n);
}

static void run(const char *where)
{
    snprintf(base, sizeof(base), "%s/symlink-test.%d", where, getpid());
    T(mkdir(base, 0755) == 0);
    T(mkdir(P("d"), 0755) == 0);
    T(mkdir(P("d/sub"), 0755) == 0);
    put(P("d/file"), "hello");

    /* following: absolute, relative, chains, in the middle of a path */
    T(symlink(P("d/file"), P("abs")) == 0 && has(P("abs"), "hello"));
    T(symlink("d/file", P("rel")) == 0 && has(P("rel"), "hello"));
    T(symlink("rel", P("chain")) == 0 && has(P("chain"), "hello"));
    T(symlink("d", P("dl")) == 0 && has(P("dl/file"), "hello"));
    T(symlink("../file", P("d/sub/up")) == 0 && has(P("d/sub/up"), "hello"));
    T(symlink("sub/../file", P("d/dotdot")) == 0 && has(P("d/dotdot"), "hello"));
    /* ".." after a link is physical: dl/sub/.. is d, not base */
    struct stat a, b;
    T(stat(P("dl/sub/.."), &a) == 0 && stat(P("d"), &b) == 0 && a.st_ino == b.st_ino);

    /* stat follows, lstat does not; readlink */
    T(lstat(P("rel"), &a) == 0 && S_ISLNK(a.st_mode) && a.st_size == 6);
    T(stat(P("rel"), &a) == 0 && S_ISREG(a.st_mode) && a.st_size == 5);
    char buf[PATH_MAX];
    ssize_t n = readlink(P("rel"), buf, sizeof(buf));
    T(n == 6 && !memcmp(buf, "d/file", 6));
    T(readlink(P("d/file"), buf, sizeof(buf)) < 0 && errno == EINVAL);
    n = readlink(P("rel"), buf, 3);                 /* truncated, not terminated */
    T(n == 3 && !memcmp(buf, "d/f", 3));

    /* no-follow calls act on the link */
    T(open(P("rel"), O_RDONLY | O_NOFOLLOW) < 0 && errno == ELOOP);
    T(fstatat(AT_FDCWD, P("rel"), &a, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(a.st_mode));
    T(lchown(P("rel"), getuid(), getgid()) == 0);
    struct timespec ts[2] = { { 1000000, 0 }, { 2000000, 0 } };
    T(utimensat(AT_FDCWD, P("rel"), ts, AT_SYMLINK_NOFOLLOW) == 0 && lstat(P("rel"), &a) == 0 &&
      a.st_mtime == 2000000 && stat(P("rel"), &b) == 0 && b.st_mtime != 2000000);

    /* loops */
    T(symlink("loop2", P("loop1")) == 0 && symlink("loop1", P("loop2")) == 0);
    T(open(P("loop1"), O_RDONLY) < 0 && errno == ELOOP);
    T(stat(P("loop1"), &a) < 0 && errno == ELOOP);
    T(lstat(P("loop1"), &a) == 0);
    T(symlink("self/x", P("self")) == 0 && stat(P("self"), &a) < 0 && errno == ELOOP);

    /* dangling links: open fails, O_CREAT creates the target, O_EXCL refuses */
    T(symlink("d/newfile", P("dangling")) == 0);
    T(open(P("dangling"), O_RDONLY) < 0 && errno == ENOENT);
    int fd = open(P("dangling"), O_WRONLY | O_CREAT, 0644);
    T(fd >= 0);
    if (fd >= 0) {
        write(fd, "made", 4);
        close(fd);
    }
    T(has(P("d/newfile"), "made"));
    T(symlink("d/other", P("dangling2")) == 0);
    T(open(P("dangling2"), O_WRONLY | O_CREAT | O_EXCL, 0644) < 0 && errno == EEXIST);
    T(access(P("d/other"), F_OK) < 0);

    /* trailing slashes: a link to a directory with a slash is the directory */
    T(lstat(P("dl/"), &a) == 0 && S_ISDIR(a.st_mode));
    T(fstatat(AT_FDCWD, P("dl/"), &a, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(a.st_mode));
    T(open(P("rel/"), O_RDONLY) < 0 && errno == ENOTDIR);

    /* operations on the link itself: unlink, rename, link (not followed) */
    T(symlink("d/file", P("tmp")) == 0 && rename(P("tmp"), P("tmp2")) == 0);
    T(lstat(P("tmp2"), &a) == 0 && S_ISLNK(a.st_mode) && has(P("d/file"), "hello"));
    T(unlink(P("tmp2")) == 0 && has(P("d/file"), "hello"));
    T(link(P("rel"), P("hard")) == 0 && lstat(P("hard"), &a) == 0 && S_ISLNK(a.st_mode));
    T(linkat(AT_FDCWD, P("rel"), AT_FDCWD, P("hard2"), AT_SYMLINK_FOLLOW) == 0 &&
      lstat(P("hard2"), &a) == 0 && S_ISREG(a.st_mode));
    T(symlink("x", P("rel")) < 0 && errno == EEXIST);
    T(rmdir(P("dl")) < 0 && errno == ENOTDIR);

    /* making things through a link to a directory */
    T(mkdir(P("dl/made"), 0755) == 0 && stat(P("d/made"), &a) == 0 && S_ISDIR(a.st_mode));
    put(P("dl/through"), "via");
    T(has(P("d/through"), "via"));

    /* chdir through a link; getcwd is the physical path */
    char cwd[PATH_MAX], real[PATH_MAX];
    T(getcwd(cwd, sizeof(cwd)) != NULL);
    T(chdir(P("dl/sub")) == 0);
    T(getcwd(buf, sizeof(buf)) != NULL && !strcmp(buf, P("d/sub")));
    T(has("up", "hello"));
    chdir(cwd);

    /* realpath */
    T(realpath(P("chain"), real) != NULL && !strcmp(real, P("d/file")));
    T(realpath(P("dl/sub/up"), real) != NULL && !strcmp(real, P("d/file")));

    /* long targets: inline (under 60 bytes) and in a block, up to PATH_MAX - 1 */
    char longt[4096];
    memset(longt, 'x', sizeof(longt) - 1);
    longt[sizeof(longt) - 1] = 0;
    T(symlink(longt, P("long")) == 0);
    n = readlink(P("long"), buf, sizeof(buf));
    T(n == 4095 && !memcmp(buf, longt, 4095));
    T(lstat(P("long"), &a) == 0 && a.st_size == 4095);
    char mid[200];
    snprintf(mid, sizeof(mid), "%s", P("d/./sub/../file"));
    T(symlink(mid, P("mid")) == 0 && has(P("mid"), "hello"));

    /* exec through a link, and a script's interpreter through a link */
    T(symlink("/bin/echo", P("echo-link")) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        dup2(dn, 1);
        execl(P("echo-link"), "echo-link", "hi", (char *)NULL);
        _exit(127);
    }
    int st;
    T(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    T(symlink("/bin/sh", P("sh-link")) == 0);
    char script[PATH_MAX + 16];
    snprintf(script, sizeof(script), "#!%s\nexit 7\n", P("sh-link"));
    put(P("script"), script);
    chmod(P("script"), 0755);
    pid = fork();
    if (pid == 0) {
        execl(P("script"), "script", (char *)NULL);
        _exit(127);
    }
    T(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7);

    /* /proc: self, and the descriptors */
    T(readlink("/proc/self", buf, sizeof(buf)) > 0 && atoi(buf) == getpid());
    fd = open(P("d/file"), O_RDONLY);
    char fdp[64];
    snprintf(fdp, sizeof(fdp), "/proc/self/fd/%d", fd);
    n = readlink(fdp, buf, sizeof(buf));
    T(n > 0 && (buf[n] = 0, !strcmp(buf, P("d/file"))));
    close(fd);

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
    system(cmd);
}

int main(void)
{
    run("/tmp");
    const char *home = getenv("HOME");
    run(home && *home ? home : "/root");
    printf("symlink: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
