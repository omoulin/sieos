/*
 * ui_sys.c - What the UI toolkit asks of the system: calls to atlas, shared
 * memory, the time, a folder's entries, the home folder. From SIEOS's own
 * library (user/lib), or from libposix's (UI_POSIX: sieos/sieos.h, POSIX).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ui_int.h"
#ifdef UI_POSIX
#include <dirent.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <sieos/sieos.h>

long ui_sys_call(uint64_t op, uint64_t a, uint64_t b, uint64_t c, const void *s, size_t slen, void *r, size_t rlen)
{
    sieos_msg_t m = { .w = { op, a, b, c }, .sbuf = s, .slen = slen, .rbuf = r, .rlen = rlen };
    return sieos_call("atlas", &m);
}
void *ui_sys_shm(size_t size, uint64_t *id)
{
    if (getrandom(id, sizeof *id, 0) != sizeof *id) return 0;
    *id &= ~(1ULL << 63);
    return sieos_shm_create(*id, size) < 0 ? 0 : sieos_shm_map(*id);
}
void ui_sys_shm_close(uint64_t id) { sieos_shm_close(id); }
void ui_sys_unmap(void *p, size_t size) { munmap(p, size); }
long ui_sys_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int ui_sys_list(const char *dir, void (*fn)(void *ctx, const char *name, int dir), void *ctx)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (!d) return -1;
    while ((e = readdir(d))) {
        char path[1024];
        struct stat st;
        size_t l = strlen(dir);
        if (e->d_name[0] == '.' || l + strlen(e->d_name) + 2 > sizeof path) continue;
        memcpy(path, dir, l); path[l] = '/'; strcpy(path + l + 1, e->d_name);
        fn(ctx, e->d_name, !stat(path, &st) && S_ISDIR(st.st_mode));
        n++;
    }
    closedir(d);
    return n;
}
const char *ui_sys_home(void) { const char *h = getenv("HOME"); return h && *h ? h : "/"; }

#else

static long port;
long ui_sys_call(uint64_t op, uint64_t a, uint64_t b, uint64_t c, const void *s, size_t slen, void *r, size_t rlen)
{
    msg_t m = { .w = { op, a, b, c }, .sbuf = s, .slen = slen, .rbuf = r, .rlen = rlen };
    long e = call_named(&port, "atlas", &m, 0);
    return e < 0 ? e : (long)m.w[0];
}
void *ui_sys_shm(size_t size, uint64_t *id)
{
    sys_random(id, sizeof *id);
    *id &= ~(1ULL << 63);
    if (sys_shm_create(*id, size) < 0) return 0;
    void *p = sys_shm_map(*id, 0, PROT_READ | PROT_WRITE);
    return (long)p <= 0 ? 0 : p;
}
void ui_sys_shm_close(uint64_t id) { sys_shm_close(id); }
void ui_sys_unmap(void *p, size_t size) { sys_munmap(p, size, 0); }
long ui_sys_ms(void) { return sys_clock() / 1000000; }
int ui_sys_list(const char *dir, void (*fn)(void *ctx, const char *name, int dir), void *ctx)
{
    long h = fs_open(dir, FS_RDONLY | FS_DIRECTORY, 0);
    if (h < 0) return -1;
    char buf[2048], name[256];
    uint64_t cs = 0, ino;
    long n;
    int type, k = 0;
    while ((n = fs_readdir(h, &cs, buf, sizeof buf)) > 0)
        for (const char *p = buf; fs_dirent(&p, buf + n, &ino, &type, name); )
            if (name[0] != '.' && (type == 4 || type == 8)) { fn(ctx, name, type == 4); k++; }
    fs_close(h);
    return k;
}
const char *ui_sys_home(void)
{
    static acct_user_t u;
    mk_info_t in;
    if (!u.home[0]) { sys_info(&in); if (acct_user(0, in.self_uid, &u) || !u.home[0]) strlcpy(u.home, "/", 2); }
    return u.home;
}
#endif
