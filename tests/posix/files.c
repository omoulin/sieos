/*
 * ptest-files (make posix-test): files and folders through the Linux
 * personality: stdio streams, POSIX calls, offsets and O_APPEND, stat,
 * directories listed, renames, links, the current folder, descriptors
 * duplicated, a file mapped into memory.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
static void check(const char *what, int ok)
{
    printf("ptest files: %s: %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

int main(void)
{
    mkdir("/tmp/pt", 0755);
    check("chdir and getcwd", chdir("/tmp/pt") == 0 && getcwd((char[64]){ 0 }, 64) && !strcmp(getcwd((char[64]){ 0 }, 64), "/tmp/pt"));
    FILE *f = fopen("a.txt", "w");
    int okw = f && fprintf(f, "line one\nline two\n") == 18;
    for (int i = 0; f && i < 1000; i++) fprintf(f, "%04d\n", i);   /* 5000 bytes more: past stdio's buffer */
    okw &= f && fclose(f) == 0;
    check("fopen, fprintf, fclose", okw);
    char line[64];
    f = fopen("/tmp/pt/a.txt", "r");
    int okr = f && fgets(line, sizeof line, f) && !strcmp(line, "line one\n");
    okr &= f && fseek(f, -5, SEEK_END) == 0 && fgets(line, sizeof line, f) && !strcmp(line, "0999\n") && ftell(f) == 5018;
    if (f) fclose(f);
    check("fgets, fseek, ftell", okr);
    struct stat st;
    check("stat", stat("a.txt", &st) == 0 && st.st_size == 5018 && S_ISREG(st.st_mode));
    int fd = open("a.txt", O_WRONLY | O_APPEND);
    int oka = fd >= 0 && write(fd, "end\n", 4) == 4 && lseek(fd, 0, SEEK_CUR) == 5022;
    close(fd);
    check("O_APPEND", oka && stat("a.txt", &st) == 0 && st.st_size == 5022);
    fd = open("a.txt", O_RDWR);
    char b[8] = { 0 };
    int okp = fd >= 0 && pwrite(fd, "LINE", 4, 0) == 4 && pread(fd, b, 8, 0) == 8 && !memcmp(b, "LINE one", 8);
    int fd2 = dup(fd);
    okp &= fd2 > fd && lseek(fd2, 5, SEEK_SET) == 5 && read(fd2, b, 3) == 3 && !memcmp(b, "one", 3);
    check("pread, pwrite, dup", okp);
    check("dup2 onto a used number", dup2(fd2, fd) == fd && close(fd) == 0 && close(fd2) == 0);
    check("open: missing file, exclusive create", open("nope", O_RDONLY) == -1 && errno == ENOENT &&
          (fd = open("a.txt", O_CREAT | O_EXCL | O_WRONLY, 0644)) == -1 && errno == EEXIST);
    mkdir("sub", 0755);
    close(open("sub/x", O_CREAT | O_WRONLY, 0644));
    close(open("sub/y", O_CREAT | O_WRONLY, 0644));
    DIR *d = opendir("sub");
    int seen = 0;
    for (struct dirent *e; d && (e = readdir(d)); )
        seen |= !strcmp(e->d_name, "x") ? 1 : !strcmp(e->d_name, "y") ? 2 : 0;
    if (d) closedir(d);
    check("opendir, readdir", seen == 3);
    check("rename, unlink, rmdir", rename("sub/x", "sub/z") == 0 && access("sub/z", F_OK) == 0 && access("sub/x", F_OK) == -1 &&
          unlink("sub/z") == 0 && unlink("sub/y") == 0 && rmdir("sub") == 0 && access("sub", F_OK) == -1);
    char t[64];
    ssize_t n = (symlink("a.txt", "ln") == 0) ? readlink("ln", t, sizeof t) : -1;
    check("symlink, readlink", n == 5 && !memcmp(t, "a.txt", 5) && lstat("ln", &st) == 0 && S_ISLNK(st.st_mode));
    fd = open("a.txt", O_RDONLY);
    char *m = fd >= 0 ? mmap(0, 5022, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    check("mmap of a file", m != MAP_FAILED && !memcmp(m, "LINE one\n", 9) && !memcmp(m + 5018, "end\n", 4));
    if (m != MAP_FAILED) munmap(m, 5022);
    close(fd);
    check("/dev/null", (fd = open("/dev/null", O_RDWR)) >= 0 && write(fd, "x", 1) == 1 && read(fd, b, 1) == 0 && close(fd) == 0);
    check("isatty", isatty(1) && !isatty(open("a.txt", O_RDONLY)));
    unlink("ln"); unlink("a.txt"); chdir("/"); rmdir("/tmp/pt");
    printf("ptest files: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
