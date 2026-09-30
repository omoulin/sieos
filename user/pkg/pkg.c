/*
 * pkg - SIEOS's package manager: software added to the system as packages,
 * under /usr/pkg, next to (never over) the base system.
 *
 *   pkg update                 fetch the repositories' signed indexes
 *   pkg search [WORD]          packages in the indexes (name or summary)
 *   pkg info NAME              a package's details
 *   pkg install NAME...        install from the repositories, with dependencies
 *   pkg upgrade [NAME...]      install newer versions of installed packages
 *   pkg remove [-f] NAME...    remove (-f: even if others depend on it)
 *   pkg list                   the installed packages
 *   pkg files NAME             the files of an installed package
 *   pkg add [-u] FILE.spkg...  install package files (-u: not in a signed index)
 *   pkg create -n NAME -v VERSION [-s SUMMARY] [-d "DEPS"] -o FILE.spkg DIR
 *                              package software installed (DESTDIR) under DIR/usr/pkg
 *
 * A package (.spkg) is a gzip-compressed ustar archive: +MANIFEST first
 * (name, version, summary, depends, size), then its files, all under
 * usr/pkg/.  A repository (a base URL in /etc/pkg/repos) serves INDEX, its
 * packages' records (with each file's SHA-256), INDEX.sig, an ECDSA P-256
 * signature of INDEX (DER, over its SHA-256), checked against the keys in
 * /etc/pkg/keys (04||X||Y, in hexadecimal), and the .spkg files.  The
 * indexes are kept in /var/cache/pkg, the installed packages' manifests and
 * file lists in /var/lib/pkg/NAME.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "http.h"
#include "json.h"
#include "crypto.h"

#define PREFIX     "usr/pkg/"             /* every packaged file is under it */
#define REPOS      "/etc/pkg/repos"
#define KEYS       "/etc/pkg/keys"
#define CACHE      "/var/cache/pkg"
#define DB         "/var/lib/pkg"
#define TIMEOUT_MS 60000

static const char *root = "/";           /* PKG_ROOT: install into another tree (tests) */

/* ---------------------------------------------------------------- utilities */

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("pkg: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("pkg: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

static char *xstrdup(const char *s)
{
    char *p = xmalloc(strlen(s) + 1);
    strcpy(p, s);
    return p;
}

/* root + path (path relative to the root, or absolute) */
static const char *rpath(const char *path)
{
    static char buf[4][4096];
    static int k;
    char *b = buf[k++ & 3];
    while (*path == '/')
        path++;
    snprintf(b, sizeof(buf[0]), "%s%s%s", root, root[strlen(root) - 1] == '/' ? "" : "/", path);
    return b;
}

static void mkdirs(const char *path, mode_t mode)
{
    char p[4096];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, mode);
            *s = '/';
        }
    mkdir(p, mode);
}

static char *read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct sbuf b;
    sb_init(&b);
    char tmp[8192];
    long n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0)
        sb_putn(&b, tmp, n);
    close(fd);
    if (len)
        *len = b.len;
    if (!b.s)
        sb_putn(&b, "", 0);
    return b.s;
}

static bool write_file(const char *path, const void *data, size_t len)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    bool ok = write(fd, data, len) == (long)len;
    ok &= fsync(fd) == 0 || true;
    close(fd);
    return ok && rename(tmp, path) == 0;
}

static void hex(const uint8_t *d, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", d[i]);
}

static int unhex(const char *s, uint8_t *out, size_t max)
{
    size_t n = 0;
    while (s[0] && s[1] && n < max) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1)
            return -1;
        out[n++] = v;
        s += 2;
        while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')
            s++;
    }
    return (int)n;
}

/* A "name version" comparison: numbers compare as numbers ("1.10" > "1.9"). */
static int vercmp(const char *a, const char *b)
{
    while (*a || *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            unsigned long x = strtoul(a, (char **)&a, 10), y = strtoul(b, (char **)&b, 10);
            if (x != y)
                return x < y ? -1 : 1;
        } else {
            if (*a != *b)
                return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
            a++, b++;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- records */

/* A package's record: INDEX entries, +MANIFEST and the database share it. */
struct rec {
    char name[64], version[64], summary[160], depends[512], file[160], sha256[65];
    unsigned long size;
    int repo;                                    /* the index it came from */
};

/* Parse "key: value" lines into r; returns the end of the record (a blank line). */
static const char *parse_rec(const char *s, struct rec *r)
{
    memset(r, 0, sizeof(*r));
    while (*s && *s != '\n') {
        const char *e = strchr(s, '\n');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        const char *colon = memchr(s, ':', len);
        if (colon) {
            size_t kl = colon - s;
            const char *v = colon + 1;
            while (v < s + len && *v == ' ')
                v++;
            size_t vl = s + len - v;
            char *dst = NULL;
            size_t cap = 0;
#define FIELD(k, f) if (kl == sizeof(k) - 1 && !memcmp(s, k, kl)) { dst = r->f; cap = sizeof(r->f); }
            FIELD("name", name) FIELD("version", version) FIELD("summary", summary)
            FIELD("depends", depends) FIELD("file", file) FIELD("sha256", sha256)
#undef FIELD
            if (dst) {
                if (vl >= cap)
                    vl = cap - 1;
                memcpy(dst, v, vl);
                dst[vl] = 0;
            } else if (kl == 4 && !memcmp(s, "size", 4)) {
                r->size = strtoul(v, NULL, 10);
            }
        }
        s += len;
        if (*s == '\n')
            s++;
    }
    while (*s == '\n')
        s++;
    return s;
}

static void put_rec(struct sbuf *b, const struct rec *r, bool with_file)
{
    sb_printf(b, "name: %s\nversion: %s\nsummary: %s\ndepends: %s\nsize: %lu\n", r->name, r->version, r->summary,
              r->depends, r->size);
    if (with_file)
        sb_printf(b, "file: %s\nsha256: %s\n", r->file, r->sha256);
}

static bool valid_name(const char *n)
{
    if (!*n || strlen(n) > 60)
        return false;
    for (const char *s = n; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.' ||
              *s == '+'))
            return false;
    return n[0] != '.';
}

/* ---------------------------------------------------------------- repositories and indexes */

#define MAXREPO 8
static char repos[MAXREPO][512];
static int nrepo;
static struct rec *idx;                           /* every index's records */
static int nidx;

static void load_repos(void)
{
    char *s = read_file(rpath(REPOS), NULL);
    if (!s)
        return;
    for (char *line = strtok(s, "\n"); line && nrepo < MAXREPO; line = strtok(NULL, "\n")) {
        while (*line == ' ' || *line == '\t')
            line++;
        if (!*line || *line == '#')
            continue;
        char *e = line + strlen(line);
        while (e > line && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '/'))
            *--e = 0;
        snprintf(repos[nrepo++], sizeof(repos[0]), "%s", line);
    }
    free(s);
}

static void load_indexes(void)
{
    load_repos();
    for (int i = 0; i < nrepo; i++) {
        char path[128];
        snprintf(path, sizeof(path), CACHE "/INDEX.%d", i);
        char *s = read_file(rpath(path), NULL);
        if (!s)
            continue;
        for (const char *p = s; *p;) {
            struct rec r;
            p = parse_rec(p, &r);
            if (!valid_name(r.name))
                continue;
            r.repo = i;
            idx = realloc(idx, (nidx + 1) * sizeof(*idx));
            if (!idx)
                die("out of memory");
            idx[nidx++] = r;
        }
        free(s);
    }
}

/* The newest version of name in the indexes. */
static const struct rec *index_find(const char *name)
{
    const struct rec *best = NULL;
    for (int i = 0; i < nidx; i++)
        if (!strcmp(idx[i].name, name) && (!best || vercmp(idx[i].version, best->version) > 0))
            best = &idx[i];
    return best;
}

static bool sig_ok(const char *data, size_t len, const uint8_t *sig, size_t siglen)
{
    uint8_t digest[32];
    hash_once(HASH_SHA256, data, len, digest);
    DIR *d = opendir(rpath(KEYS));
    if (!d)
        return false;
    bool ok = false;
    struct dirent *e;
    while (!ok && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", rpath(KEYS), e->d_name);
        char *k = read_file(path, NULL);
        if (!k)
            continue;
        uint8_t pub[65];
        if (unhex(k, pub, sizeof(pub)) == 65 && pub[0] == 4)
            ok = ecdsa_verify(ECDSA_P256, pub, 65, digest, 32, sig, siglen);
        free(k);
    }
    closedir(d);
    return ok;
}

static int fetch_mem(const char *url, struct sbuf *out, char *err, size_t errlen)
{
    struct url u;
    if (!url_parse(url, &u)) {
        snprintf(err, errlen, "bad URL");
        return -1;
    }
    return http_request("GET", &u, "", NULL, 0, out, err, errlen, TIMEOUT_MS);
}

static int cmd_update(void)
{
    load_repos();
    if (!nrepo)
        die("no repository in %s", REPOS);
    mkdirs(rpath(CACHE), 0755);
    int bad = 0;
    for (int i = 0; i < nrepo; i++) {
        char url[700], err[256];
        struct sbuf ix, sg;
        sb_init(&ix);
        sb_init(&sg);
        snprintf(url, sizeof(url), "%s/INDEX", repos[i]);
        int st = fetch_mem(url, &ix, err, sizeof(err));
        if (st != 200) {
            warn("%s: %s", url, st < 0 ? err : "not found");
            bad++;
            goto next;
        }
        snprintf(url, sizeof(url), "%s/INDEX.sig", repos[i]);
        st = fetch_mem(url, &sg, err, sizeof(err));
        if (st != 200) {
            warn("%s: %s", url, st < 0 ? err : "not found");
            bad++;
            goto next;
        }
        if (!sig_ok(ix.s ? ix.s : "", ix.len, (const uint8_t *)sg.s, sg.len)) {
            warn("%s: the index's signature does not match a key in %s: ignored", repos[i], KEYS);
            bad++;
            goto next;
        }
        char path[128];
        snprintf(path, sizeof(path), CACHE "/INDEX.%d", i);
        if (!write_file(rpath(path), ix.s ? ix.s : "", ix.len))
            die("%s: %s", rpath(path), strerror(errno));
        int n = 0;
        for (size_t k = 0; k < ix.len; k++)
            n += ix.s[k] == '\n' && (k + 1 == ix.len || ix.s[k + 1] == '\n');
        printf("%s: %d package%s\n", repos[i], n, n == 1 ? "" : "s");
    next:
        sb_free(&ix);
        sb_free(&sg);
    }
    return bad ? 1 : 0;
}

/* ---------------------------------------------------------------- the installed packages */

static bool installed(const char *name, struct rec *r)
{
    char path[256];
    snprintf(path, sizeof(path), DB "/%s/MANIFEST", name);
    char *s = read_file(rpath(path), NULL);
    if (!s)
        return false;
    struct rec tmp;
    parse_rec(s, r ? r : &tmp);
    free(s);
    return true;
}

/* Calls f for each installed package's record. */
static void each_installed(void (*f)(const struct rec *, void *), void *ctx)
{
    DIR *d = opendir(rpath(DB));
    if (!d)
        return;
    struct dirent *e;
    char names[256][64];
    int n = 0;
    while ((e = readdir(d)) && n < 256)
        if (e->d_name[0] != '.' && valid_name(e->d_name))
            snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
    closedir(d);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[64];
                strcpy(t, names[i]);
                strcpy(names[i], names[j]);
                strcpy(names[j], t);
            }
    for (int i = 0; i < n; i++) {
        struct rec r;
        if (installed(names[i], &r))
            f(&r, ctx);
    }
}

static bool has_word(const char *list, const char *w)
{
    size_t n = strlen(w);
    for (const char *s = list; (s = strstr(s, w)); s += n)
        if ((s == list || s[-1] == ' ' || s[-1] == ',') && (!s[n] || s[n] == ' ' || s[n] == ','))
            return true;
    return false;
}

/* The file list of an installed package: "f path", "l path", "d path" lines. */
static char *files_of(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), DB "/%s/FILES", name);
    return read_file(rpath(path), NULL);
}

/* Which installed package owns path ("" if none). */
struct owner_ctx {
    const char *path, *except;
    char owner[64];
};
static void owner_f(const struct rec *r, void *vc)
{
    struct owner_ctx *c = vc;
    if (c->owner[0] || (c->except && !strcmp(r->name, c->except)))
        return;
    char *s = files_of(r->name);
    if (!s)
        return;
    size_t pl = strlen(c->path);
    for (char *p = s; *p;) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == pl + 2 && p[0] != 'd' && !memcmp(p + 2, c->path, pl)) {
            snprintf(c->owner, sizeof(c->owner), "%s", r->name);
            break;
        }
        p += len + (e ? 1 : 0);
    }
    free(s);
}

/* ---------------------------------------------------------------- archives */

struct tar_entry {
    char name[512];
    char type;                                   /* '0' file, '5' directory, '2' symbolic link */
    unsigned long size;
    unsigned mode;
    char link[256];
};

static unsigned long octal(const char *s, int n)
{
    unsigned long v = 0;
    for (int i = 0; i < n && s[i]; i++)
        if (s[i] >= '0' && s[i] <= '7')
            v = v * 8 + (s[i] - '0');
    return v;
}

/* Reads the next member's header (skipping pax extended headers); false at the end. */
static bool tar_next(gzFile z, struct tar_entry *t)
{
    for (;;) {
        unsigned char h[512];
        int n = gzread(z, h, 512);
        if (n != 512)
            return false;
        bool zero = true;
        for (int i = 0; i < 512 && zero; i++)
            zero = !h[i];
        if (zero)
            return false;
        if (memcmp(h + 257, "ustar", 5))
            die("not a ustar archive");
        memset(t, 0, sizeof(*t));
        t->type = h[156] ? h[156] : '0';
        t->size = octal((char *)h + 124, 12);
        t->mode = octal((char *)h + 100, 8);
        char name[101], prefix[156];
        memcpy(name, h, 100);
        name[100] = 0;
        memcpy(prefix, h + 345, 155);
        prefix[155] = 0;
        snprintf(t->name, sizeof(t->name), "%s%s%s", prefix, prefix[0] ? "/" : "", name);
        memcpy(t->link, h + 157, 100);
        t->link[100] = 0;
        if (t->type == 'x' || t->type == 'g') {  /* pax headers: not used, skipped */
            unsigned long skip = (t->size + 511) & ~511UL;
            char buf[512];
            while (skip) {
                if (gzread(z, buf, 512) != 512)
                    return false;
                skip -= 512;
            }
            continue;
        }
        size_t l = strlen(t->name);
        while (l > 1 && t->name[l - 1] == '/')
            t->name[--l] = 0;
        if (!strncmp(t->name, "./", 2))
            memmove(t->name, t->name + 2, strlen(t->name + 2) + 1);
        return true;
    }
}

/* Copies (or, out < 0, skips) a member's data. */
static bool tar_data(gzFile z, const struct tar_entry *t, int out)
{
    unsigned long left = t->size, pad = ((t->size + 511) & ~511UL) - t->size;
    char buf[16384];
    while (left) {
        int want = left < sizeof(buf) ? (int)left : (int)sizeof(buf);
        int n = gzread(z, buf, want);
        if (n <= 0)
            return false;
        if (out >= 0 && write(out, buf, n) != n)
            return false;
        left -= n;
    }
    if (pad && gzread(z, buf, pad) != (int)pad)
        return false;
    return true;
}

/* A packaged path must stay under usr/pkg/ and not climb out of it. */
static bool safe_path(const char *p)
{
    if (strncmp(p, PREFIX, strlen(PREFIX)) && strcmp(p, "usr/pkg"))
        return false;
    for (const char *s = p; *s; s++)
        if (s[0] == '.' && s[1] == '.' && (s == p || s[-1] == '/') && (!s[2] || s[2] == '/'))
            return false;
    return strlen(p) < 400;
}

/* No directory on rel's way (under the root) may be a symbolic link: a package
 * could otherwise ship usr/pkg/x -> /etc, then usr/pkg/x/passwd, and write
 * through the link outside /usr/pkg. */
static bool parents_ok(const char *rel)
{
    char p[512];
    snprintf(p, sizeof(p), "%s", rel);
    for (char *s = p; (s = strchr(s, '/')); s++) {
        *s = 0;
        struct stat st;
        bool link = lstat(rpath(p), &st) == 0 && S_ISLNK(st.st_mode);
        *s = '/';
        if (link)
            return false;
    }
    return true;
}

/* The directories every system has (/usr/pkg, /usr/pkg/bin, ...): never removed. */
static bool base_dir(const char *rel)
{
    int slashes = 0;
    for (const char *s = rel; *s; s++)
        slashes += *s == '/';
    return !strcmp(rel, "usr/pkg") || (!strncmp(rel, PREFIX, strlen(PREFIX)) && slashes == 2);
}

/* Reads +MANIFEST, the first member. */
static bool read_manifest(gzFile z, struct rec *r)
{
    struct tar_entry t;
    if (!tar_next(z, &t) || strcmp(t.name, "+MANIFEST") || t.size > 65536)
        return false;
    char *buf = xmalloc(t.size + 1);
    unsigned long pad = ((t.size + 511) & ~511UL) - t.size;
    if ((unsigned long)gzread(z, buf, t.size) != t.size) {
        free(buf);
        return false;
    }
    char tmp[512];
    if (pad)
        gzread(z, tmp, pad);
    buf[t.size] = 0;
    parse_rec(buf, r);
    free(buf);
    return valid_name(r->name) && r->version[0];
}

/* ---------------------------------------------------------------- install and remove */

static bool file_sha256(const char *path, char out[65])
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    struct hash_ctx c;
    hash_init(&c, HASH_SHA256);
    char buf[16384];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        hash_update(&c, buf, n);
    close(fd);
    uint8_t d[32];
    hash_final(&c, d);
    hex(d, 32, out);
    return n == 0;
}

static void remove_files(const char *files, const char *keep)
{
    /* the files and links first, then the directories, deepest first */
    size_t n = 0;
    for (const char *p = files; *p; p++)
        n += *p == '\n';
    char **dirs = xmalloc((n + 1) * sizeof(char *));
    int nd = 0;
    char *copy = xstrdup(files);
    for (char *line = strtok(copy, "\n"); line; line = strtok(NULL, "\n")) {
        if (strlen(line) < 3)
            continue;
        const char *path = line + 2;
        if (keep) {                              /* (an upgrade: what the new version still has stays) */
            char pat[520];
            snprintf(pat, sizeof(pat), "\n%c %s\n", line[0], path);
            if (strstr(keep, pat))
                continue;
        }
        if (line[0] == 'd')
            dirs[nd++] = line + 2;
        else if (unlink(rpath(path)) < 0 && errno != ENOENT)
            warn("%s: %s", rpath(path), strerror(errno));
    }
    for (int i = 0; i < nd; i++)
        for (int j = i + 1; j < nd; j++)
            if (strlen(dirs[j]) > strlen(dirs[i])) {
                char *t = dirs[i];
                dirs[i] = dirs[j];
                dirs[j] = t;
            }
    for (int i = 0; i < nd; i++)
        if (!base_dir(dirs[i]))
            rmdir(rpath(dirs[i]));               /* (only if empty: shared directories stay) */
    free(dirs);
    free(copy);
}

/*
 * Install the package file at path.  expect: its record from a signed index
 * (the file's SHA-256 is checked against it), or NULL for a local package.
 */
static bool install_file(const char *path, const struct rec *expect, bool force)
{
    if (expect) {
        char sum[65];
        if (!file_sha256(path, sum) || strcmp(sum, expect->sha256)) {
            warn("%s: the SHA-256 does not match the index: not installed", path);
            return false;
        }
    }
    gzFile z = gzopen(path, "rb");
    if (!z) {
        warn("%s: %s", path, strerror(errno));
        return false;
    }
    struct rec r;
    if (!read_manifest(z, &r)) {
        warn("%s: not a SIEOS package (no +MANIFEST)", path);
        gzclose(z);
        return false;
    }
    if (expect && (strcmp(r.name, expect->name) || strcmp(r.version, expect->version))) {
        warn("%s: is %s %s, the index says %s %s", path, r.name, r.version, expect->name, expect->version);
        gzclose(z);
        return false;
    }
    struct rec old;
    bool upgrading = installed(r.name, &old);

    /* first pass: the paths are safe, and nothing but this package's older self owns them */
    struct sbuf files;
    sb_init(&files);
    sb_putc(&files, '\n');
    struct tar_entry t;
    while (tar_next(z, &t)) {
        if (!safe_path(t.name) || (t.type != '0' && t.type != '5' && t.type != '2')) {
            warn("%s: %s: not allowed in a package (only regular files, directories and "
                 "symbolic links under /usr/pkg)", path, t.name);
            goto fail;
        }
        /* nothing may be written through a symbolic link: one already there, or one this
         * package ships earlier (its "l" line in the list so far) */
        bool via_link = !parents_ok(t.name);
        char pre[520];
        for (const char *sl = strchr(t.name, '/'); sl && !via_link; sl = strchr(sl + 1, '/')) {
            snprintf(pre, sizeof(pre), "\nl %.*s\n", (int)(sl - t.name), t.name);
            via_link = strstr(files.s, pre) != NULL;
        }
        if (via_link) {
            warn("%s: %s: a directory on its way is a symbolic link: not installed", path, t.name);
            goto fail;
        }
        if (t.type != '5') {
            struct owner_ctx c = { t.name, r.name, "" };
            each_installed(owner_f, &c);
            struct stat st;
            if (c.owner[0] && !force) {
                warn("%s: %s belongs to %s", r.name, t.name, c.owner);
                goto fail;
            }
            if (!c.owner[0] && lstat(rpath(t.name), &st) == 0 && !S_ISDIR(st.st_mode) && !upgrading && !force) {
                warn("%s: %s exists and belongs to no package (-f to replace it)", r.name, t.name);
                goto fail;
            }
        }
        sb_printf(&files, "%c %s\n", t.type == '5' ? 'd' : t.type == '2' ? 'l' : 'f', t.name);
        if (!tar_data(z, &t, -1)) {
            warn("%s: truncated", path);
            goto fail;
        }
    }
    gzclose(z);

    /* second pass: extract (each file under a temporary name, then renamed over) */
    z = gzopen(path, "rb");
    if (!z || !read_manifest(z, &r))
        goto fail;
    while (tar_next(z, &t)) {
        if (!parents_ok(t.name)) {
            warn("%s: %s: a directory on its way is a symbolic link: not installed", path, t.name);
            goto fail;
        }
        const char *dst = rpath(t.name);
        char dir[4096];
        snprintf(dir, sizeof(dir), "%s", dst);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            mkdirs(dir, 0755);
        }
        if (t.type == '5') {
            mkdirs(dst, 0755);
            chmod(dst, (t.mode & 07777) | 0700);
            continue;
        }
        char tmp[4200];
        snprintf(tmp, sizeof(tmp), "%s.pkgnew", dst);
        unlink(tmp);
        if (t.type == '2') {
            if (symlink(t.link, tmp) < 0 || rename(tmp, dst) < 0) {
                warn("%s: %s", dst, strerror(errno));
                goto fail;
            }
            continue;
        }
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, (t.mode & 07777) ? (t.mode & 07777) : 0644);
        if (fd < 0 || !tar_data(z, &t, fd)) {
            if (fd >= 0)
                close(fd);
            warn("%s: %s", dst, fd < 0 ? strerror(errno) : "truncated package");
            unlink(tmp);
            goto fail;
        }
        fchmod(fd, t.mode & 07777);
        close(fd);
        if (rename(tmp, dst) < 0) {
            warn("%s: %s", dst, strerror(errno));
            goto fail;
        }
    }
    gzclose(z);

    /* the database: an upgrade drops what the new version no longer has */
    char dbdir[256], p[300];
    snprintf(dbdir, sizeof(dbdir), DB "/%s", r.name);
    if (upgrading) {
        char *oldfiles = files_of(r.name);
        if (oldfiles) {
            remove_files(oldfiles, files.s);
            free(oldfiles);
        }
    }
    mkdirs(rpath(dbdir), 0755);
    struct sbuf m;
    sb_init(&m);
    put_rec(&m, &r, false);
    snprintf(p, sizeof(p), "%s/MANIFEST", dbdir);
    bool ok = write_file(rpath(p), m.s, m.len);
    snprintf(p, sizeof(p), "%s/FILES", dbdir);
    ok &= write_file(rpath(p), files.s + 1, files.len - 1);
    sb_free(&m);
    sb_free(&files);
    if (!ok)
        die("%s: %s", rpath(dbdir), strerror(errno));
    if (upgrading && strcmp(old.version, r.version))
        printf("%s %s -> %s\n", r.name, old.version, r.version);
    else
        printf("%s %s installed\n", r.name, r.version);
    return true;
fail:
    if (z)
        gzclose(z);
    sb_free(&files);
    return false;
}

struct dl_ctx {
    int fd;
    struct hash_ctx h;
    unsigned long n;
};
static bool dl_sink(void *vc, const char *data, size_t n)
{
    struct dl_ctx *c = vc;
    hash_update(&c->h, data, n);
    c->n += n;
    return write(c->fd, data, n) == (long)n;
}

/* Downloads r's file into the cache; the path, or NULL. */
static const char *download(const struct rec *r)
{
    static char path[512];
    if (strchr(r->file, '/') || !r->file[0]) {
        warn("%s: bad file name in the index", r->name);
        return NULL;
    }
    mkdirs(rpath(CACHE), 0755);
    snprintf(path, sizeof(path), "%s/%s", rpath(CACHE), r->file);
    char sum[65];
    if (file_sha256(path, sum) && !strcmp(sum, r->sha256))
        return path;                             /* (already there) */
    char url[800], err[256];
    snprintf(url, sizeof(url), "%s/%s", repos[r->repo], r->file);
    struct url u;
    if (!url_parse(url, &u)) {
        warn("%s: bad URL", url);
        return NULL;
    }
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    struct dl_ctx c = { open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644), { 0 }, 0 };
    if (c.fd < 0) {
        warn("%s: %s", tmp, strerror(errno));
        return NULL;
    }
    hash_init(&c.h, HASH_SHA256);
    printf("fetching %s %s (%lu KiB installed)\n", r->name, r->version, (r->size + 1023) / 1024);
    struct sbuf resp;
    sb_init(&resp);
    int st = http_request_stream("GET", &u, "", NULL, 0, dl_sink, &c, &resp, err, sizeof(err), TIMEOUT_MS);
    sb_free(&resp);
    close(c.fd);
    if (st != 200) {
        warn("%s: %s", url, st < 0 ? err : "not found");
        unlink(tmp);
        return NULL;
    }
    uint8_t d[32];
    hash_final(&c.h, d);
    hex(d, 32, sum);
    if (strcmp(sum, r->sha256)) {
        warn("%s: the SHA-256 does not match the index", url);
        unlink(tmp);
        return NULL;
    }
    if (rename(tmp, path) < 0) {
        warn("%s: %s", path, strerror(errno));
        return NULL;
    }
    return path;
}

/* Adds name and, first, its dependencies to the plan (each once). */
static bool plan_add(const char *name, const struct rec **plan, int *n, int max, int depth)
{
    for (int i = 0; i < *n; i++)
        if (!strcmp(plan[i]->name, name))
            return true;
    if (depth > 32) {
        warn("%s: dependency loop", name);
        return false;
    }
    const struct rec *r = index_find(name);
    if (!r) {
        warn("%s: no such package (pkg update, then pkg search)", name);
        return false;
    }
    char deps[512], *dv[32];
    int nd = 0;
    snprintf(deps, sizeof(deps), "%s", r->depends);
    for (char *d = strtok(deps, " ,"); d && nd < 32; d = strtok(NULL, " ,"))
        dv[nd++] = d;                            /* (split first: plan_add recurses, strtok is not reentrant) */
    for (int i = 0; i < nd; i++) {
        struct rec have;
        if (installed(dv[i], &have))
            continue;
        if (!plan_add(dv[i], plan, n, max, depth + 1))
            return false;
    }
    if (*n >= max) {
        warn("too many packages");
        return false;
    }
    plan[(*n)++] = r;
    return true;
}

static int install_plan(const struct rec **plan, int n, bool force)
{
    int bad = 0;
    for (int i = 0; i < n; i++) {
        struct rec have;
        if (installed(plan[i]->name, &have) && !vercmp(have.version, plan[i]->version)) {
            printf("%s %s is installed\n", have.name, have.version);
            continue;
        }
        const char *path = download(plan[i]);
        if (!path || !install_file(path, plan[i], force)) {
            bad++;
            break;                               /* (what depends on it must not go in) */
        }
    }
    return bad ? 1 : 0;
}

static int cmd_install(int argc, char **argv, bool force)
{
    load_indexes();
    if (!nidx)
        die("no package index: pkg update");
    const struct rec *plan[256];
    int n = 0;
    for (int i = 0; i < argc; i++)
        if (!plan_add(argv[i], plan, &n, 256, 0))
            return 1;
    return install_plan(plan, n, force);
}

struct up_ctx {
    const struct rec **plan;
    int n;
};
static void up_f(const struct rec *r, void *vc)
{
    struct up_ctx *c = vc;
    const struct rec *x = index_find(r->name);
    if (x && vercmp(x->version, r->version) > 0 && c->n < 256)
        plan_add(r->name, c->plan, &c->n, 256, 0);
}

static int cmd_upgrade(int argc, char **argv)
{
    load_indexes();
    if (!nidx)
        die("no package index: pkg update");
    const struct rec *plan[256];
    struct up_ctx c = { plan, 0 };
    if (argc) {
        for (int i = 0; i < argc; i++) {
            struct rec r;
            if (!installed(argv[i], &r))
                die("%s is not installed", argv[i]);
            up_f(&r, &c);
        }
    } else {
        each_installed(up_f, &c);
    }
    if (!c.n) {
        printf("everything is up to date\n");
        return 0;
    }
    return install_plan(plan, c.n, false);
}

struct rdep_ctx {
    const char *name;
    char users[512];
};
static void rdep_f(const struct rec *r, void *vc)
{
    struct rdep_ctx *c = vc;
    if (strcmp(r->name, c->name) && has_word(r->depends, c->name) && strlen(c->users) + strlen(r->name) + 2 < 512) {
        strcat(c->users, c->users[0] ? " " : "");
        strcat(c->users, r->name);
    }
}

static int cmd_remove(int argc, char **argv, bool force)
{
    int bad = 0;
    for (int i = 0; i < argc; i++) {
        struct rec r;
        if (!installed(argv[i], &r)) {
            warn("%s is not installed", argv[i]);
            bad++;
            continue;
        }
        struct rdep_ctx c = { argv[i], "" };
        each_installed(rdep_f, &c);
        if (c.users[0] && !force) {
            warn("%s is needed by %s (-f to remove it anyway)", argv[i], c.users);
            bad++;
            continue;
        }
        char *files = files_of(argv[i]);
        if (files) {
            remove_files(files, NULL);
            free(files);
        }
        char p[300];
        snprintf(p, sizeof(p), DB "/%s/MANIFEST", argv[i]);
        unlink(rpath(p));
        snprintf(p, sizeof(p), DB "/%s/FILES", argv[i]);
        unlink(rpath(p));
        snprintf(p, sizeof(p), DB "/%s", argv[i]);
        rmdir(rpath(p));
        printf("%s %s removed\n", r.name, r.version);
    }
    return bad ? 1 : 0;
}

static int cmd_add(int argc, char **argv, bool unsigned_ok, bool force)
{
    load_indexes();
    int bad = 0;
    for (int i = 0; i < argc; i++) {
        /* a file the signed indexes list (same name, version and SHA-256) needs no -u */
        gzFile z = gzopen(argv[i], "rb");
        struct rec r;
        bool ok = z && read_manifest(z, &r);
        if (z)
            gzclose(z);
        if (!ok) {
            warn("%s: not a SIEOS package", argv[i]);
            bad++;
            continue;
        }
        const struct rec *signed_rec = NULL;
        char sum[65];
        if (file_sha256(argv[i], sum))
            for (int k = 0; k < nidx; k++)
                if (!strcmp(idx[k].name, r.name) && !strcmp(idx[k].version, r.version) &&
                    !strcmp(idx[k].sha256, sum))
                    signed_rec = &idx[k];
        if (!signed_rec && !unsigned_ok) {
            warn("%s: %s %s is in no signed index (-u to install it anyway)", argv[i], r.name, r.version);
            bad++;
            continue;
        }
        bad += !install_file(argv[i], signed_rec, force);
    }
    return bad ? 1 : 0;
}

/* ---------------------------------------------------------------- queries */

static void list_f(const struct rec *r, void *vc)
{
    (void)vc;
    printf("%-20s %-14s %s\n", r->name, r->version, r->summary);
}

static int cmd_search(const char *word)
{
    load_indexes();
    if (!nidx)
        die("no package index: pkg update");
    int shown = 0;
    for (int i = 0; i < nidx; i++) {
        const struct rec *r = &idx[i];
        if (index_find(r->name) != r)
            continue;                            /* (older versions) */
        if (word && !strstr(r->name, word) && !strstr(r->summary, word))
            continue;
        struct rec have;
        bool inst = installed(r->name, &have);
        printf("%-20s %-14s %s%s\n", r->name, r->version, r->summary,
               inst ? (vercmp(have.version, r->version) < 0 ? "  [installed, older]" : "  [installed]") : "");
        shown++;
    }
    if (!shown && word)
        printf("no package matches \"%s\"\n", word);
    return 0;
}

static int cmd_info(const char *name)
{
    load_indexes();
    struct rec have;
    bool inst = installed(name, &have);
    const struct rec *r = index_find(name);
    if (!r && !inst)
        die("%s: no such package", name);
    const struct rec *x = r ? r : &have;
    printf("name:      %s\nversion:   %s\nsummary:   %s\ndepends:   %s\nsize:      %lu KiB\n", x->name, x->version,
           x->summary, x->depends[0] ? x->depends : "(none)", (x->size + 1023) / 1024);
    if (r)
        printf("from:      %s\n", repos[r->repo]);
    printf("installed: %s\n", inst ? have.version : "no");
    return 0;
}

static int cmd_files(const char *name)
{
    char *s = files_of(name);
    if (!s)
        die("%s is not installed", name);
    for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n"))
        if (line[0] != 'd')
            printf("/%s\n", line + 2);
    free(s);
    return 0;
}

/* ---------------------------------------------------------------- create */

struct tar_out {
    gzFile z;
    unsigned long size;                          /* the installed size */
    const char *base;                            /* the tree's root directory */
};

static void tar_header(struct tar_out *o, const char *name, char type, unsigned mode, unsigned long size,
                       const char *link)
{
    unsigned char h[512];
    memset(h, 0, sizeof(h));
    size_t l = strlen(name);
    if (l > 100) {                               /* the ustar prefix: split at a '/' */
        const char *cut = name + l - 100;
        while (*cut && *cut != '/')
            cut++;
        if (!*cut || cut - name > 155)
            die("%s: path too long for a package", name);
        memcpy(h + 345, name, cut - name);
        memcpy(h, cut + 1, strlen(cut + 1));
    } else {
        memcpy(h, name, l);
    }
    snprintf((char *)h + 100, 8, "%07o", mode & 07777);
    snprintf((char *)h + 108, 8, "%07o", 0);
    snprintf((char *)h + 116, 8, "%07o", 0);
    snprintf((char *)h + 124, 12, "%011lo", size);
    snprintf((char *)h + 136, 12, "%011lo", 0UL);
    h[156] = type;
    if (link)
        memcpy(h + 157, link, strlen(link) < 100 ? strlen(link) : 99);
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memcpy(h + 265, "root", 4);
    memcpy(h + 297, "root", 4);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++)
        sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    h[155] = ' ';
    gzwrite(o->z, h, 512);
}

static void tar_pad(struct tar_out *o, unsigned long size)
{
    static const char zero[512];
    if (size % 512)
        gzwrite(o->z, zero, 512 - size % 512);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Writes rel (under base) and, for a directory, its contents, sorted. */
static void tar_tree(struct tar_out *o, const char *rel)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", o->base, rel);
    struct stat st;
    if (lstat(path, &st) < 0)
        die("%s: %s", path, strerror(errno));
    if (S_ISLNK(st.st_mode)) {
        char link[256];
        long n = readlink(path, link, sizeof(link) - 1);
        if (n < 0 || n >= 100)
            die("%s: bad symbolic link", path);
        link[n] = 0;
        tar_header(o, rel, '2', 0777, 0, link);
        return;
    }
    if (S_ISREG(st.st_mode)) {
        tar_header(o, rel, '0', st.st_mode, st.st_size, NULL);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            die("%s: %s", path, strerror(errno));
        char buf[16384];
        long n;
        unsigned long total = 0;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            gzwrite(o->z, buf, n);
            total += n;
        }
        close(fd);
        if (total != (unsigned long)st.st_size)
            die("%s: changed while being packaged", path);
        tar_pad(o, total);
        o->size += total;
        return;
    }
    if (!S_ISDIR(st.st_mode))
        die("%s: only regular files, directories and symbolic links can be packaged", path);
    tar_header(o, rel, '5', st.st_mode, 0, NULL);
    DIR *d = opendir(path);
    if (!d)
        die("%s: %s", path, strerror(errno));
    char **names = NULL;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        names = realloc(names, (n + 1) * sizeof(char *));
        names[n++] = xstrdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof(char *), by_name);
    for (int i = 0; i < n; i++) {
        char sub[4096];
        snprintf(sub, sizeof(sub), "%s/%s", rel, names[i]);
        tar_tree(o, sub);
        free(names[i]);
    }
    free(names);
}

static int cmd_create(struct rec *r, const char *out, const char *dir)
{
    if (!valid_name(r->name) || !r->version[0])
        die("create: -n NAME and -v VERSION are needed (a name: a-z 0-9 - _ . +)");
    char top[4096];
    snprintf(top, sizeof(top), "%s/usr/pkg", dir);
    struct stat st;
    if (stat(top, &st) < 0 || !S_ISDIR(st.st_mode))
        die("create: %s: no usr/pkg (install the software with --prefix=/usr/pkg DESTDIR=%s)", dir, dir);
    /* the payload first, into a temporary file, to know the installed size for the manifest */
    char tmp[4200];
    snprintf(tmp, sizeof(tmp), "%s.payload", out);
    struct tar_out o = { gzopen(tmp, "wb9"), 0, dir };
    if (!o.z)
        die("%s: %s", tmp, strerror(errno));
    tar_tree(&o, "usr/pkg");
    gzclose(o.z);
    r->size = o.size;
    struct sbuf m;
    sb_init(&m);
    put_rec(&m, r, false);
    /* the package: +MANIFEST, then the payload's members (re-read uncompressed) */
    struct tar_out p = { gzopen(out, "wb9"), 0, dir };
    if (!p.z)
        die("%s: %s", out, strerror(errno));
    tar_header(&p, "+MANIFEST", '0', 0644, m.len, NULL);
    gzwrite(p.z, m.s, m.len);
    tar_pad(&p, m.len);
    gzFile in = gzopen(tmp, "rb");
    char buf[16384];
    int n;
    while ((n = gzread(in, buf, sizeof(buf))) > 0)
        gzwrite(p.z, buf, n);
    gzclose(in);
    static const char zero[1024];
    gzwrite(p.z, zero, 1024);                    /* the end of the archive */
    gzclose(p.z);
    unlink(tmp);
    sb_free(&m);
    printf("%s: %s %s, %lu KiB installed\n", out, r->name, r->version, (r->size + 1023) / 1024);
    return 0;
}

/* ---------------------------------------------------------------- main */

static void usage(void)
{
    fputs("usage: pkg update | search [WORD] | info NAME | install NAME... | upgrade [NAME...]\n"
          "           | remove [-f] NAME... | list | files NAME | add [-u] [-f] FILE.spkg...\n"
          "           | create -n NAME -v VERSION [-s SUMMARY] [-d \"DEPS\"] -o FILE.spkg DIR\n",
          stderr);
    exit(2);
}

static int lock_db(void)
{
    mkdirs(rpath(DB), 0755);
    int fd = open(rpath(DB "/.lock"), O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        die("%s: %s (root is needed to change packages)", rpath(DB), strerror(errno));
    if (flock(fd, LOCK_EX | LOCK_NB) < 0)
        die("another pkg is running");
    return fd;
}

int main(int argc, char **argv)
{
    if (getenv("PKG_ROOT") && *getenv("PKG_ROOT"))
        root = getenv("PKG_ROOT");
    if (argc < 2)
        usage();
    const char *cmd = argv[1];
    argc -= 2;
    argv += 2;
    bool force = false, unsigned_ok = false;
    struct rec r;
    memset(&r, 0, sizeof(r));
    const char *out = NULL;
    int i = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o = argv[i];
        if (!strcmp(o, "-f"))
            force = true;
        else if (!strcmp(o, "-u"))
            unsigned_ok = true;
        else if (i + 1 < argc && !strcmp(cmd, "create") && strchr("nvsdo", o[1]) && !o[2]) {
            const char *v = argv[++i];
            switch (o[1]) {
            case 'n': snprintf(r.name, sizeof(r.name), "%s", v); break;
            case 'v': snprintf(r.version, sizeof(r.version), "%s", v); break;
            case 's': snprintf(r.summary, sizeof(r.summary), "%s", v); break;
            case 'd': snprintf(r.depends, sizeof(r.depends), "%s", v); break;
            case 'o': out = v; break;
            }
        } else
            usage();
    }
    argc -= i;
    argv += i;

    if (!strcmp(cmd, "search"))
        return cmd_search(argc ? argv[0] : NULL);
    if (!strcmp(cmd, "info") && argc == 1)
        return cmd_info(argv[0]);
    if (!strcmp(cmd, "list") && !argc) {
        each_installed(list_f, NULL);
        return 0;
    }
    if (!strcmp(cmd, "files") && argc == 1)
        return cmd_files(argv[0]);
    if (!strcmp(cmd, "create") && argc == 1 && out)
        return cmd_create(&r, out, argv[0]);
    int lk = -1;
    int rc = 2;
    if (!strcmp(cmd, "update") && !argc) {
        lk = lock_db();
        rc = cmd_update();
    } else if (!strcmp(cmd, "install") && argc) {
        lk = lock_db();
        rc = cmd_install(argc, argv, force);
    } else if (!strcmp(cmd, "upgrade")) {
        lk = lock_db();
        rc = cmd_upgrade(argc, argv);
    } else if (!strcmp(cmd, "remove") && argc) {
        lk = lock_db();
        rc = cmd_remove(argc, argv, force);
    } else if (!strcmp(cmd, "add") && argc) {
        lk = lock_db();
        rc = cmd_add(argc, argv, unsigned_ok, force);
    } else {
        usage();
    }
    if (lk >= 0)
        close(lk);
    return rc;
}
