/*
 * pkg - Add-on packages (docs/packages.md): the browser, Python, the
 * assistant's model, Scriptures... are not in the base system; each is one
 * package file, made by the build (make pkg-web...; tools/pkg.py says the
 * format: a manifest, then the files' data in zlib pieces).
 *
 *   pkg install FILE|URL   install a package (a file, or http(s)://...):
 *                          each file's SHA-256 checked against the manifest;
 *                          what it replaces of an older version goes first
 *   pkg remove NAME        remove exactly what it installed
 *   pkg list               the installed packages
 *
 * What a package installed is recorded in /etc/pkg/NAME (its manifest, with
 * the directories it made). A file two packages carry (a font) stays until
 * the last one goes. Root only, but list.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "net.h"
#include <mk/crypto.h>
#include <mk/zlib.h>

#define PIECE  (1 << 20)
#define MAXMAN (64 << 10)
#define REC    "/etc/pkg/"

/* ---- the package's bytes: a file, or an HTTP(S) response's body */
static http_t http;
static long fh = -1;
static uint64_t foff;
static uint8_t rb[32768];
static size_t rpos, rlen;

static int get(void *out, size_t n)                 /* exactly n bytes -> 0, or -1 */
{
    for (uint8_t *o = out; n; ) {
        if (rpos == rlen) {
            long r = fh >= 0 ? fs_read(fh, foff, rb, sizeof rb) : http_read(&http, rb, sizeof rb);
            if (r <= 0) return -1;
            foff += (uint64_t)r; rpos = 0; rlen = (size_t)r;
        }
        size_t k = rlen - rpos < n ? rlen - rpos : n;
        memcpy(o, rb + rpos, k);
        rpos += k; o += k; n -= k;
    }
    return 0;
}

/* ---- manifests: "key word word...\n" lines */
static char *word(char **p)                         /* the next word of a line (cut there) */
{
    char *w = *p;
    while (**p && **p != ' ') (*p)++;
    if (**p) *(*p)++ = 0;
    return w;
}

static char *field(const char *m, const char *key, char *out, size_t n)   /* "key VALUE" -> VALUE */
{
    size_t k = strlen(key);
    out[0] = 0;
    for (const char *p = m; *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : p + strlen(p))
        if (!memcmp(p, key, k) && p[k] == ' ') {
            size_t i = 0;
            for (p += k + 1; *p && *p != '\n' && i < n - 1; ) out[i++] = *p++;
            out[i] = 0;
            break;
        }
    return out;
}

static char *slurp(const char *path)                /* a whole (small) file, 0-terminated */
{
    long h = fs_open(path, FS_RDONLY, 0);
    if (h < 0) return 0;
    char *b = malloc(MAXMAN + 1);
    long n = b ? fs_read(h, 0, b, MAXMAN) : -1;
    fs_close(h);
    if (n < 0) { free(b); return 0; }
    b[n] = 0;
    return b;
}

/* Does an installed package other than `skip` list path (a file or a link)? */
static int owned(const char *path, const char *skip)
{
    long h = fs_open(REC, FS_RDONLY | FS_DIRECTORY, 0);
    if (h < 0) return 0;
    static char buf[4096];
    char name[256], rec[300];
    uint64_t cs = 0, ino;
    int type, found = 0;
    long n;
    size_t pl = strlen(path);
    while (!found && (n = fs_readdir(h, &cs, buf, sizeof buf)) > 0)
        for (const char *p = buf; !found && fs_dirent(&p, buf + n, &ino, &type, name); ) {
            if (type != 8 || (skip && !strcmp(name, skip))) continue;
            strlcpy(rec, REC, sizeof rec); strlcpy(rec + sizeof REC - 1, name, sizeof rec - sizeof REC + 1);
            char *m = slurp(rec);
            for (char *l = m; l && *l && !found; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : l + strlen(l)) {
                char *e = strchr(l, '\n') ? strchr(l, '\n') : l + strlen(l);
                found = (e - l > (long)pl && e[-pl - 1] == ' ' && !memcmp(e - pl, path, pl) &&
                         (!memcmp(l, "file ", 5) || !memcmp(l, "link ", 5)));
            }
            free(m);
        }
    fs_close(h);
    return found;
}

/* Undo a record's lines, last first: its files and links (unless another
 * package has them too), then the directories it made, if empty. */
static void undo(char *m, const char *name)
{
    static char *lines[4096];
    int n = 0;
    for (char *l = m; *l && n < 4096; ) {
        lines[n++] = l;
        char *e = strchr(l, '\n');
        if (!e) break;
        *e = 0; l = e + 1;
    }
    while (n--) {
        char *p = lines[n], *k = word(&p), *path = p;
        while (*path && strchr(path, ' ')) path = strchr(path, ' ') + 1;   /* (the last word) */
        if (!strcmp(k, "dir")) fs_rmdir(path);
        else if ((!strcmp(k, "file") || !strcmp(k, "link")) && !owned(path, name)) fs_unlink(path);
    }
}

static int remove(const char *name, int quiet)
{
    char rec[300];
    strlcpy(rec, REC, sizeof rec); strlcpy(rec + sizeof REC - 1, name, sizeof rec - sizeof REC + 1);
    char *m = strchr(name, '/') ? 0 : slurp(rec);
    if (!m) { if (!quiet) printf("pkg: %s is not installed\n", name); return 1; }
    undo(m, name);
    free(m);
    fs_unlink(rec);
    if (!quiet) printf("pkg: %s removed\n", name);
    return 0;
}

/* One file's data: its pieces, inflated, hashed and written -> 0, BADSHA or -error */
enum { BADSHA = 1 };
static long put_file(const char *path, int mode, uint64_t size, const char *sha)
{
    static uint8_t *z, *p;
    if (!z && (!(z = malloc(ZBOUND(PIECE))) || !(p = malloc(PIECE)))) return -ENOMEM;
    fs_unlink(path);                                /* (a link, or a file another package has: replaced) */
    long h = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC, mode), e = 0;
    if (h < 0) return h;
    sha_t s;
    sha_init(&s, SHA256);
    for (uint64_t off = 0; off < size && !e; ) {
        uint32_t hd;
        size_t want = size - off < PIECE ? (size_t)(size - off) : PIECE, n = 0;
        if (get(&hd, 4) || (hd & 0x7fffffff) > ZBOUND(PIECE) || get(z, hd & 0x7fffffff)) { e = -EIO; break; }
        if (hd >> 31) { memcpy(p, z, hd & 0x7fffffff); n = hd & 0x7fffffff; }
        else { size_t used; long r = zinflate(z, hd, p, PIECE, &used); n = r < 0 ? 0 : (size_t)r; }
        if (n != want) { e = -EIO; break; }
        sha_update(&s, p, n);
        for (size_t w = 0; w < n && !e; w += FS_MAX)
            if ((e = fs_write(h, off + w, p + w, n - w < FS_MAX ? n - w : FS_MAX)) > 0) e = 0;
        off += n;
    }
    fs_close(h);
    uint8_t d[32];
    char hex[65];
    sha_final(&s, d);
    for (int i = 0; i < 32; i++) { hex[2 * i] = "0123456789abcdef"[d[i] >> 4]; hex[2 * i + 1] = "0123456789abcdef"[d[i] & 15]; }
    hex[64] = 0;
    if (!e && strcmp(hex, sha)) e = BADSHA;
    if (!e) fs_chmod(path, mode);
    return e;
}

static int install(const char *src)
{
    if (!memcmp(src, "http://", 7) || !memcmp(src, "https://", 8)) {
        long e = http_request(&http, "GET", src, 0, 0, 0, 0, 0);
        if (e < 0 || http.status != 200) { printf("pkg: %s: %s\n", src, e < 0 ? net_strerror(e) : "not found"); return 1; }
    } else if ((fh = fs_open(src, FS_RDONLY, 0)) < 0) { printf("pkg: %s: cannot read\n", src); return 1; }
    /* the manifest: lines up to "end" */
    static char man[MAXMAN + 1], rec[MAXMAN + 1], name[64], ver[32], tmp[MAXMAN + 1];
    size_t n = 0, rn = 0, ln = 0;
    while (n < MAXMAN && !get(man + n, 1))
        if (man[n++] == '\n') {
            if (n - ln == 4 && !memcmp(man + ln, "end\n", 4)) break;
            ln = n;
        }
    if (n < 16 || memcmp(man, "SIEOS-PKG 1\n", 12) || memcmp(man + n - 4, "end\n", 4)) { printf("pkg: %s: not a SIEOS package\n", src); return 1; }
    man[n - 4] = 0;
    field(man, "name", name, sizeof name);
    field(man, "version", ver, sizeof ver);
    if (!name[0] || strchr(name, '/')) { printf("pkg: %s: no name\n", src); return 1; }
    /* what it would overwrite: only its own older files, or another package's */
    strlcpy(tmp, man, sizeof tmp);
    for (char *l = tmp, *e; *l; l = e + 1) {
        if (!(e = strchr(l, '\n'))) e = l + strlen(l) - 1;
        else *e = 0;
        char *p = l, *k = word(&p), *path = p;
        while (*path && strchr(path, ' ')) path = strchr(path, ' ') + 1;
        siefs_stat_t st;
        if ((!strcmp(k, "file") || !strcmp(k, "link")) && !fs_stat(path, &st, 1) && !owned(path, 0)) {
            printf("pkg: %s: %s is already there (not from a package): nothing installed\n", name, path);
            return 1;
        }
    }
    if (!remove(name, 1)) printf("pkg: %s: the version installed removed first\n", name);
    fs_mkdir("/etc/pkg", 0755);
    /* its lines, in order; the record grows as they are done */
    long err = 0;
    uint64_t files = 0, bytes = 0;
    for (char *l = man, *e; *l && !err; l = e + 1) {
        if (!(e = strchr(l, '\n'))) e = l + strlen(l) - 1;
        else *e = 0;
        char line[600], *p = line, *k;
        strlcpy(line, l, sizeof line);
        k = word(&p);
        siefs_stat_t st;
        int keep = 1;
        if (!strcmp(k, "dir")) {
            keep = fs_stat(p, &st, 0) < 0;
            if (keep && (err = fs_mkdir(p, 0755)) < 0) printf("pkg: %s: cannot make it\n", p);
        } else if (!strcmp(k, "file")) {
            char *m = word(&p), *sz = word(&p), *sha = word(&p);
            int mode = 0;
            for (; *m >= '0' && *m <= '7'; m++) mode = mode * 8 + *m - '0';
            long size = strnum(sz);
            if ((err = size < 0 ? -EINVAL : put_file(p, mode, (uint64_t)size, sha)))
                printf("pkg: %s: %s\n", p, err == BADSHA ? "bad SHA-256 (a damaged package)" : err == -EIO ? "the package is cut short or damaged" : "cannot write it");
            files++; bytes += (uint64_t)(size > 0 ? size : 0);
        } else if (!strcmp(k, "link")) {
            char *target = word(&p);
            fs_unlink(p);
            if ((err = fs_symlink(target, p)) < 0) printf("pkg: %s: cannot make the link\n", p);
        }
        if (keep) { rn += strlcpy(rec + rn, l, sizeof rec - rn - 1); rec[rn++] = '\n'; rec[rn] = 0; }
    }
    if (fh >= 0) fs_close(fh); else http_close(&http);
    if (err) { undo(rec, name); printf("pkg: %s not installed\n", name); return 1; }
    char path[300];
    strlcpy(path, REC, sizeof path); strlcpy(path + sizeof REC - 1, name, sizeof path - sizeof REC + 1);
    long h = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC, 0644);
    if (h < 0 || fs_write(h, 0, rec, rn) != (long)rn || fs_write(h, rn, "end\n", 4) != 4) {
        printf("pkg: %s: cannot record it\n", path);
        if (h >= 0) fs_close(h);
        undo(rec, name);
        return 1;
    }
    fs_close(h);
    printf("pkg: %s %s installed: %lu files, %lu KiB\n", name, ver, files, bytes >> 10);
    return 0;
}

static int list(void)
{
    long h = fs_open(REC, FS_RDONLY | FS_DIRECTORY, 0);
    static char buf[4096];
    char name[256], path[300], ver[32], size[24];
    uint64_t cs = 0, ino;
    int type, any = 0;
    long n;
    while (h >= 0 && (n = fs_readdir(h, &cs, buf, sizeof buf)) > 0)
        for (const char *p = buf; fs_dirent(&p, buf + n, &ino, &type, name); ) {
            if (type != 8) continue;
            strlcpy(path, REC, sizeof path); strlcpy(path + sizeof REC - 1, name, sizeof path - sizeof REC + 1);
            char *m = slurp(path);
            if (!m) continue;
            long s = strnum(field(m, "size", size, sizeof size));
            printf("%-14s %-12s %8ld KiB\n", name, field(m, "version", ver, sizeof ver), s > 0 ? s >> 10 : 0);
            free(m);
            any = 1;
        }
    if (h >= 0) fs_close(h);
    if (!any) printf("no packages installed (the base system only)\n");
    return 0;
}

int main(int argc, char **argv)
{
    mk_ident_t me;
    sys_ident(0, &me);
    if (argc == 2 && !strcmp(argv[1], "list")) return list();
    if (argc != 3 || (strcmp(argv[1], "install") && strcmp(argv[1], "remove"))) {
        printf("usage: pkg install FILE|URL, pkg remove NAME, pkg list\n");
        return 2;
    }
    if (me.uid) { printf("pkg: only root installs and removes packages\n"); return 1; }
    return argv[1][0] == 'i' ? install(argv[2]) : remove(argv[2], 0);
}
