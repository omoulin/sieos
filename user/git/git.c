/*
 * git - Version control, written for SIEOS, in git's own formats: host git
 * reads a repository made here, and this git one made there (docs/git.md).
 *
 *   git init [DIR]                   git clone URL [DIR]
 *   git status [-s | --porcelain]    git add PATH... | -A    git rm [--cached] PATH...
 *   git reset [PATH...]              git restore [--staged] PATH...
 *   git commit [-a] -m MESSAGE       (the words after -m are the message)
 *   git log [--oneline] [-n N] [REV] git show [REV]
 *   git diff [--cached] [PATH...]    git branch [-a | -d NAME | NAME [REV]]
 *   git checkout [-b] BRANCH | -- PATH...   git switch [-c] BRANCH
 *   git fetch [REMOTE]   git pull [REMOTE [BRANCH]]   git push [-u] [REMOTE [BRANCH]]
 *   git remote [-v | add NAME URL]   git config [--global] KEY [VALUE]
 *   git -C DIR ...                   (first: work in DIR)
 * URLs: git://host[:port]/path, http(s)://[user[:token]@]host/path.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "git.h"

/* ---- output: buffered, one console message per 4 KiB */
static char ob[4096];
static size_t on;
void oflush(void) { if (on) con_write(ob, on); on = 0; }
static void oput(void *ctx, char c) { (void)ctx; if (on == sizeof ob) oflush(); ob[on++] = c; }
void out(const void *s, size_t n) { for (const char *p = s; n--; ) oput(0, *p++); }
void outf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vformat(oput, 0, fmt, ap); va_end(ap); }
int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out("fatal: ", 7); vformat(oput, 0, fmt, ap); out("\n", 1);
    va_end(ap);
    return 1;
}

/* ---- buffers and formatting */
void bput(buf_t *b, const void *p, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->n + n + 1) c *= 2;
        char *q = realloc(b->p, c);
        if (!q) { fail("out of memory"); oflush(); sys_exit(1); }
        b->p = q; b->cap = c;
    }
    if (n) memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bchar(void *ctx, char c) { bput(ctx, &c, 1); }
void bfmt(buf_t *b, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vformat(bchar, b, fmt, ap); va_end(ap); }
struct sb { char *o; size_t cap, n; };
static void schar(void *ctx, char c) { struct sb *s = ctx; if (s->n + 1 < s->cap) s->o[s->n++] = c; }
int sfmt(char *o, size_t cap, const char *fmt, ...)
{
    struct sb s = { o, cap, 0 };
    va_list ap;
    va_start(ap, fmt);
    vformat(schar, &s, fmt, ap);
    va_end(ap);
    o[s.n] = 0;
    return (int)s.n;
}
void hex(char *o, const uint8_t *id) { for (int i = 0; i < 20; i++) { o[2 * i] = "0123456789abcdef"[id[i] >> 4]; o[2 * i + 1] = "0123456789abcdef"[id[i] & 15]; } o[40] = 0; }
int unhex(uint8_t *id, const char *s)
{
    for (int i = 0; i < 40; i++) {
        int c = s[i], v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return -1;
        id[i / 2] = (uint8_t)(i & 1 ? id[i / 2] << 4 | v : v);
    }
    return 0;
}
static const char *base_of(const char *p) { const char *b = p; for (; *p; p++) if (*p == '/') b = p + 1; return b; }
static const char *abbr(const uint8_t *id) { static char h[2][41]; static int k; char *s = h[k ^= 1]; hex(s, id); s[7] = 0; return s; }

/* Shell sort: few lines, fast enough for a pack's names. */
void sort(void *base, int n, size_t size, int (*cmp)(const void *, const void *))
{
    char *a = base, t[64];
    for (int gap = n / 2; gap > 0; gap /= 2)
        for (int i = gap; i < n; i++)
            for (int j = i; j >= gap && cmp(a + (size_t)(j - gap) * size, a + (size_t)j * size) > 0; j -= gap) {
                memcpy(t, a + (size_t)j * size, size);
                memcpy(a + (size_t)j * size, a + (size_t)(j - gap) * size, size);
                memcpy(a + (size_t)(j - gap) * size, t, size);
            }
}

/* ---- files */
char *get_file(const char *path, size_t *n)
{
    siefs_stat_t st;
    long h = fs_open(path, FS_RDONLY, 0);
    if (h < 0) return 0;
    char *b = fs_fstat(h, &st) ? 0 : malloc(st.size + 1);
    long r = b ? fs_read(h, 0, b, st.size) : -1;
    fs_close(h);
    if (r != (long)st.size) { free(b); return 0; }
    b[r] = 0;
    if (n) *n = (size_t)r;
    return b;
}
static void mkdirs(const char *path)             /* the folders above path */
{
    char p[FS_PATH];
    strlcpy(p, path, sizeof p);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; fs_mkdir(p, 0755); *s = '/'; }
}
int put_file(const char *path, const void *p, size_t n, int mode)
{
    long h = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC, mode);
    if (h == -ENOENT) { mkdirs(path); h = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC, mode); }
    if (h < 0) return (int)h;
    long r = n ? fs_write(h, 0, p, n) : 0;
    fs_close(h);
    return r == (long)n ? 0 : -EIO;
}

/* ---- the configuration: .git/config and ~/.gitconfig, "[section "sub"]"
 * then "key = value" lines. Section and key names compare in lower case. */
static char gcfg[96];                            /* ~/.gitconfig */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static void norm_key(const char *k, char *o, size_t cap)
{
    const char *dot = strchr(k, '.'), *last = k + strlen(k);
    while (last > k && last[-1] != '.') last--;
    size_t i = 0;
    for (const char *p = k; *p && i < cap - 1; p++) o[i++] = (char)(p < dot || p >= last ? lower(*p) : *p);
    o[i] = 0;
}
/* The next line of a configuration text: 1 a key (its full name in key, its value), 2 a section, 3 other, 0 the end. */
typedef struct { char *ls, *le, *val; size_t vl; char sec[128], key[200]; } cline_t;
static int cfg_line(char **pp, cline_t *c)
{
    char *p = *pp;
    if (!*p) return 0;
    c->ls = p;
    char *e = strchr(p, '\n');
    c->le = *pp = e ? e + 1 : p + strlen(p);
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    if (*p == '[') {
        for (p++; *p && *p != ']' && *p != ' ' && *p != '"' && n < sizeof c->sec - 1; ) c->sec[n++] = (char)lower(*p++);
        while (*p == ' ') p++;
        if (*p == '"') for (c->sec[n++] = '.', p++; *p && *p != '"' && n < sizeof c->sec - 1; ) c->sec[n++] = *p++;
        c->sec[n] = 0;
        return 2;
    }
    if (!*p || *p == '#' || *p == ';' || *p == '\n' || *p == '\r') return 3;
    n = (size_t)sfmt(c->key, sizeof c->key, "%s.", c->sec);
    while (*p && *p != '=' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && n < sizeof c->key - 1) c->key[n++] = (char)lower(*p++);
    c->key[n] = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '=') p++;
    while (*p == ' ' || *p == '\t') p++;
    char *q = p;
    while (*q && *q != '\n') q++;
    while (q > p && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r')) q--;
    if (q - p >= 2 && *p == '"' && q[-1] == '"') { p++; q--; }
    c->val = p; c->vl = (size_t)(q - p);
    return 1;
}
char *cfg_get(const char *key)
{
    char k[200], *v = 0;
    norm_key(key, k, sizeof k);
    const char *files[2] = { gcfg, ".git/config" };
    for (int f = 0; f < 2; f++) {
        char *t = files[f][0] ? get_file(files[f], 0) : 0, *p = t;
        cline_t c;
        c.sec[0] = 0;
        for (int r; t && (r = cfg_line(&p, &c)); )
            if (r == 1 && !strcmp(c.key, k)) {
                free(v);
                v = malloc(c.vl + 1);
                memcpy(v, c.val, c.vl); v[c.vl] = 0;
            }
        free(t);
    }
    return v;
}
int cfg_set(const char *file, const char *key, const char *val)
{
    char k[200], sec[200];
    norm_key(key, k, sizeof k);
    strlcpy(sec, k, sizeof sec);
    char *name = sec + strlen(sec);
    while (name > sec && name[-1] != '.') name--;
    if (name == sec) return fail("bad key %s", key);
    name[-1] = 0;
    char *t = get_file(file, 0), *p, *at = 0, *ls = 0, *le = 0;
    if (!t) { t = malloc(1); t[0] = 0; }
    cline_t c;
    c.sec[0] = 0;
    p = t;
    for (int r; (r = cfg_line(&p, &c)); ) {
        if (!strcmp(c.sec, sec)) at = c.le;          /* the end of that section, so far */
        if (r == 1 && !strcmp(c.key, k)) { ls = c.ls; le = c.le; }
    }
    buf_t b = { 0 };
    int quote = val[0] == ' ' || strchr(val, '#') || strchr(val, ';');
    if (!ls) ls = le = at;
    if (ls) bput(&b, t, (size_t)(ls - t));
    else {                                           /* a new section */
        bput(&b, t, strlen(t));
        if (b.n && b.p[b.n - 1] != '\n') bput(&b, "\n", 1);
        char *dot = strchr(sec, '.');
        if (dot) { *dot = 0; bfmt(&b, "[%s \"%s\"]\n", sec, dot + 1); } else bfmt(&b, "[%s]\n", sec);
    }
    bfmt(&b, quote ? "\t%s = \"%s\"\n" : "\t%s = %s\n", name, val);
    if (ls) bput(&b, le, strlen(le));
    int e = put_file(file, b.p, b.n, 0644);
    free(t); free(b.p);
    return e ? fail("cannot write %s", file) : 0;
}

/* ---- the repository: found from the current folder up; paths given are
 * made relative to its top (where git works) */
static char prefix[FS_PATH];                     /* the current folder, from the top ("sub/") */
static int find_repo(int quiet)
{
    static char root[FS_PATH];
    siefs_stat_t st;
    strlcpy(root, fs_getcwd(), sizeof root);
    size_t start = strlen(root);
    for (;;) {
        char g[FS_PATH + 8];
        sfmt(g, sizeof g, "%s/.git", strcmp(root, "/") ? root : "");
        if (!fs_stat(g, &st, 0) && (st.mode & SIEFS_IFMT) == SIEFS_IFDIR) break;
        char *s = root + strlen(root);
        if (!strcmp(root, "/")) return quiet ? 1 : fail("not a git repository (or any of the parent folders): .git");
        while (s > root && *s != '/') s--;
        if (s == root) s++;
        *s = 0;
    }
    const char *cwd = fs_getcwd();
    size_t rl = strcmp(root, "/") ? strlen(root) : 0;
    prefix[0] = 0;
    if (start > rl) sfmt(prefix, sizeof prefix, "%s/", cwd + rl + 1);
    fs_chdir(root);
    return 0;
}
static char *rel(const char *a)                  /* an argument -> a path from the top ("" = all) */
{
    char t[FS_PATH * 2], *o = malloc(FS_PATH);
    sfmt(t, sizeof t, "%s%s", prefix, a);
    size_t n = 0;
    for (char *p = t; *p; ) {
        char *e = p;
        while (*e && *e != '/') e++;
        size_t l = (size_t)(e - p);
        if (l == 2 && p[0] == '.' && p[1] == '.') { while (n && o[--n] != '/') ; }
        else if (l && !(l == 1 && p[0] == '.') && n + l + 2 < FS_PATH) { if (n) o[n++] = '/'; memcpy(o + n, p, l); n += l; }
        p = *e ? e + 1 : e;
    }
    o[n] = 0;
    return o;
}
static int inspec(const char *path, char **spec, int n)
{
    if (!n) return 1;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(spec[i]);
        if (!l || (!memcmp(path, spec[i], l) && (!path[l] || path[l] == '/'))) return 1;
    }
    return 0;
}

/* ---- .gitignore (the top folder's) and .git/info/exclude: globs with * ? **;
 * a trailing / for folders only; a / elsewhere: matched from the top. */
typedef struct { char *p; int dir, anch; } pat_t;
static pat_t *pats;
static int npats;
static void load_ignore(const char *file)
{
    char *t = get_file(file, 0);
    for (char *p = t; p && *p; ) {
        char *e = strchr(p, '\n'), *s = p;
        if (e) *e = 0;
        p = e ? e + 1 : p + strlen(p);
        size_t l = strlen(s);
        while (l && (s[l - 1] == ' ' || s[l - 1] == '\r')) s[--l] = 0;
        if (!l || *s == '#' || *s == '!') continue;
        pats = realloc(pats, sizeof(pat_t) * (size_t)(npats + 1));
        pat_t *q = &pats[npats++];
        if ((q->dir = s[l - 1] == '/')) s[--l] = 0;
        q->anch = strchr(s, '/') != 0;
        q->p = *s == '/' ? s + 1 : s;
    }
}
static int glob(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            int any = p[1] == '*';                   /* "**": across folders too */
            p += any;
            for (;; s++) { if (glob(p + 1, s)) return 1; if (!*s || (*s == '/' && !any)) return 0; }
        }
        if (!*s || (*p == '?' ? *s == '/' : *p != *s)) return 0;
    }
    return !*s;
}
static int ignored(const char *path, int dir)
{
    for (int i = 0; i < npats; i++)
        if ((dir || !pats[i].dir) && glob(pats[i].p, pats[i].anch ? path : base_of(path))) return 1;
    return 0;
}

/* ---- the three states: HEAD's files, the index, the work tree */
static ents_t H, I, W;
static oid_t head_id;
static int have_head, refreshed;
static uint32_t mode_of(const siefs_stat_t *st)
{
    return (st->mode & SIEFS_IFMT) == SIEFS_IFLNK ? 0120000 : st->mode & 0100 ? 0100755 : 0100644;
}
static void scan(const char *dir)
{
    long h = fs_open(*dir ? dir : ".", FS_RDONLY | FS_DIRECTORY, 0);
    if (h < 0) return;
    char *buf = malloc(4096), name[256], path[FS_PATH];
    uint64_t cs = 0, ino;
    long n;
    int type;
    while ((n = fs_readdir(h, &cs, buf, 4096)) > 0)
        for (const char *p = buf; fs_dirent(&p, buf + n, &ino, &type, name); ) {
            if (!strcmp(name, ".git") || !strcmp(name, ".") || !strcmp(name, "..")) continue;
            sfmt(path, sizeof path, "%s%s", dir, name);
            if (ignored(path, type == 4) && !ents_find(&I, path)) continue;
            siefs_stat_t st;
            if (type == 4) { strlcpy(path + strlen(path), "/", 2); scan(path); }
            else if ((type == 8 || type == 10) && !fs_stat(path, &st, 1)) {
                ent_t *e = ents_put(&W, path);
                e->mode = mode_of(&st); e->size = (uint32_t)st.size; e->mtime = st.mtime;
            }
        }
    free(buf);
    fs_close(h);
}
static int load(int wt)
{
    have_head = !ref_read("HEAD", head_id);
    oid_t t;
    if (have_head && (commit_tree(head_id, t) || tree_flat(t, &H))) return fail("cannot read the tree of HEAD");
    if (index_read(&I)) return 1;
    if (wt) { load_ignore(".gitignore"); load_ignore(".git/info/exclude"); scan(""); }
    return 0;
}
static uint8_t *wt_read(const char *path, uint32_t mode, size_t *n)   /* a file's content (a link's target) */
{
    if (mode != 0120000) return (uint8_t *)get_file(path, n);
    char *t = malloc(FS_PATH);
    long l = fs_readlink(path, t, FS_PATH - 1);
    if (l < 0) { free(t); return 0; }
    t[l] = 0; *n = (size_t)l;
    return (uint8_t *)t;
}
static int wt_hash(ent_t *w, int write)          /* w->id from its file (stored if write) */
{
    size_t n;
    uint8_t *d = wt_read(w->path, w->mode, &n);
    if (!d) return -1;
    int e = write ? obj_write(OBJ_BLOB, d, n, w->id) : (obj_hash(OBJ_BLOB, d, n, w->id), 0);
    free(d);
    return e;
}
/* An index entry against its file: 0 the same, 'M' changed, 'D' gone.
 * Same content, other time: the index learns the new time (refreshed). */
static int wt_state(ent_t *e)
{
    ent_t *w = ents_find(&W, e->path);
    if (!w) return 'D';
    if (w->size == e->size && w->mtime == e->mtime && w->mode == e->mode) { memcpy(w->id, e->id, 20); return 0; }
    if (wt_hash(w, 0)) return 'D';
    if (memcmp(w->id, e->id, 20) || w->mode != e->mode) return 'M';
    e->size = w->size; e->mtime = w->mtime;
    refreshed = 1;
    return 0;
}
static int staged(const ent_t *e)                /* an index entry against HEAD: 0, 'A', 'M' */
{
    ent_t *h = ents_find(&H, e->path);
    return !h ? 'A' : memcmp(h->id, e->id, 20) || h->mode != e->mode ? 'M' : 0;
}
static int dirty(void)                           /* changes to what git follows (not new files) */
{
    for (int i = 0; i < I.n; i++) if (staged(&I.e[i]) || wt_state(&I.e[i])) return 1;
    for (int i = 0; i < H.n; i++) if (!ents_find(&I, H.e[i].path)) return 1;
    return 0;
}
static const char *branch_name(void)             /* "main", or 0: detached */
{
    const char *r = head_ref();
    return r && !memcmp(r, "refs/heads/", 11) ? r + 11 : 0;
}

/* ---- status */
static int cmd_status(int ac, char **av)
{
    int shrt = ac > 1 && (!strcmp(av[1], "-s") || !strcmp(av[1], "--short") || !strcmp(av[1], "--porcelain"));
    if (load(1)) return 1;
    ents_t c = { 0 };                                /* each changed path: mode = X << 8 | Y */
    for (int i = 0; i < I.n; i++) {
        int x = staged(&I.e[i]), y = wt_state(&I.e[i]);
        if (x || y) ents_put(&c, I.e[i].path)->mode = (uint32_t)((x ? x : ' ') << 8 | (y ? y : ' '));
    }
    for (int i = 0; i < H.n; i++) if (!ents_find(&I, H.e[i].path)) ents_put(&c, H.e[i].path)->mode = 'D' << 8 | ' ';
    for (int i = 0; i < W.n; i++) if (!ents_find(&I, W.e[i].path)) ents_put(&c, W.e[i].path)->mode = '?' << 8 | '?';
    if (refreshed) index_write(&I);
    if (shrt) {
        for (int i = 0; i < c.n; i++) outf("%c%c %s\n", c.e[i].mode >> 8, c.e[i].mode & 255, c.e[i].path);
        return 0;
    }
    const char *b = branch_name();
    if (b) outf("On branch %s\n", b); else outf("HEAD detached at %s\n", abbr(head_id));
    if (!have_head) outf("\nNo commits yet\n");
    static const char *const title[3] = { "Changes to be committed:", "Changes not staged for commit:", "Untracked files:" };
    for (int s = 0; s < 3; s++) {
        int shown = 0;
        for (int i = 0; i < c.n; i++) {
            int x = (int)(c.e[i].mode >> 8), y = (int)(c.e[i].mode & 255), k = s == 0 ? x : y;
            if (s == 2 ? x != '?' : k == ' ' || k == '?') continue;
            if (!shown++) outf("\n%s\n", title[s]);
            outf(s == 2 ? "\t%s%s\n" : "\t%-12s%s\n", s == 2 ? "" : k == 'A' ? "new file:" : k == 'D' ? "deleted:" : "modified:", c.e[i].path);
        }
    }
    if (!c.n) outf("nothing to commit, working tree clean\n");
    return 0;
}

/* ---- add, rm, reset, restore */
static int specs(int ac, char **av, int from, char ***out)   /* the path arguments, from the top */
{
    char **s = malloc(sizeof(char *) * (size_t)(ac + 1));
    int n = 0;
    for (int i = from; i < ac; i++)
        if (!strcmp(av[i], "-A") || !strcmp(av[i], "--all")) s[n++] = "";
        else if (av[i][0] != '-') s[n++] = rel(av[i]);
    *out = s;
    return n;
}
static int matched(char **s, int n)              /* each path argument names something */
{
    for (int k = 0; k < n; k++) {
        int any = !s[k][0];
        for (int i = 0; i < I.n && !any; i++) any = inspec(I.e[i].path, &s[k], 1);
        for (int i = 0; i < W.n && !any; i++) any = inspec(W.e[i].path, &s[k], 1);
        if (!any) return fail("pathspec '%s' did not match any files", s[k]);
    }
    return 0;
}
static int stage(ent_t *w)                       /* a work tree file into the index */
{
    ent_t *e = ents_find(&I, w->path);
    if (e && e->size == w->size && e->mtime == w->mtime && e->mode == w->mode) return 0;
    if (wt_hash(w, 1)) return fail("cannot read %s", w->path);
    e = ents_put(&I, w->path);
    e->mode = w->mode; e->size = w->size; e->mtime = w->mtime;
    memcpy(e->id, w->id, 20);
    return 0;
}
static int cmd_add(int ac, char **av)
{
    char **s;
    int n = specs(ac, av, 1, &s);
    if (!n) return fail("nothing specified, nothing added (git add PATH... or -A)");
    if (load(1) || matched(s, n)) return 1;
    for (int i = 0; i < W.n; i++) if (inspec(W.e[i].path, s, n) && stage(&W.e[i])) return 1;
    for (int i = I.n - 1; i >= 0; i--) if (inspec(I.e[i].path, s, n) && !ents_find(&W, I.e[i].path)) ents_del(&I, &I.e[i]);
    return index_write(&I) ? fail("cannot write the index") : 0;
}
static int cmd_rm(int ac, char **av)
{
    char **s;
    int cached = 0, n = specs(ac, av, 1, &s), hit = 0;
    for (int i = 1; i < ac; i++) cached |= !strcmp(av[i], "--cached");
    if (load(0)) return 1;
    for (int i = I.n - 1; i >= 0; i--)
        if (n && inspec(I.e[i].path, s, n)) {
            outf("rm '%s'\n", I.e[i].path);
            if (!cached) fs_unlink(I.e[i].path);
            ents_del(&I, &I.e[i]);
            hit++;
        }
    if (!hit) return fail("pathspec did not match any tracked files");
    return index_write(&I) ? fail("cannot write the index") : 0;
}
static int unstage(char **s, int n)              /* the index's entries for those paths: as in HEAD */
{
    for (int i = I.n - 1; i >= 0; i--) {
        if (!inspec(I.e[i].path, s, n)) continue;
        ent_t *h = ents_find(&H, I.e[i].path);
        if (!h) { ents_del(&I, &I.e[i]); continue; }
        if (memcmp(h->id, I.e[i].id, 20) || h->mode != I.e[i].mode) { memcpy(I.e[i].id, h->id, 20); I.e[i].mode = h->mode; I.e[i].mtime = 0; }
    }
    for (int i = 0; i < H.n; i++)
        if (inspec(H.e[i].path, s, n) && !ents_find(&I, H.e[i].path)) {
            ent_t *e = ents_put(&I, H.e[i].path);
            memcpy(e->id, H.e[i].id, 20); e->mode = H.e[i].mode;
        }
    return index_write(&I) ? fail("cannot write the index") : 0;
}
static int write_entry(const char *path, uint32_t mode, const oid_t id, ent_t *ix)   /* a file from an object */
{
    int t;
    size_t n;
    uint8_t *d = obj_read(id, &t, &n);
    if (!d || t != OBJ_BLOB) { free(d); return fail("cannot read the object of %s", path); }
    int e;
    if (mode == 0120000) { fs_unlink(path); mkdirs(path); e = (int)fs_symlink((char *)d, path); }
    else if (mode == 0160000) e = 0;                 /* (a submodule: not checked out) */
    else {
        e = put_file(path, d, n, mode == 0100755 ? 0755 : 0644);
        if (!e) fs_chmod(path, mode == 0100755 ? 0755 : 0644);
    }
    free(d);
    siefs_stat_t st;
    if (e || fs_stat(path, &st, 1)) return fail("cannot write %s", path);
    if (ix) { ix->mode = mode; ix->size = (uint32_t)st.size; ix->mtime = st.mtime; memcpy(ix->id, id, 20); }
    return 0;
}
static int cmd_restore(int ac, char **av)        /* (also checkout -- PATH and reset PATH) */
{
    char **s;
    int st = 0, n;
    for (int i = 1; i < ac; i++) {
        st |= !strcmp(av[i], "--staged");
        if (!strcmp(av[i], "--hard") || !strcmp(av[i], "--soft")) return fail("reset: only the index (reset [PATH...])");
    }
    st |= !strcmp(av[0], "reset");
    if (!strcmp(av[0], "reset") && ac > 1 && !strcmp(av[1], "HEAD")) av[1] = "-";
    n = specs(ac, av, 1, &s);
    if (load(0)) return 1;
    if (st) return n || !strcmp(av[0], "reset") ? unstage(s, n) : fail("restore: no path given");
    if (!n) return fail("restore: no path given");
    for (int i = 0; i < I.n; i++)
        if (inspec(I.e[i].path, s, n) && write_entry(I.e[i].path, I.e[i].mode, I.e[i].id, &I.e[i])) return 1;
    return index_write(&I);
}

/* ---- commit */
static int64_t now;                              /* (the commit's time: COMMIT_EDITMSG's) */
static char *ident(void)                          /* "Name <mail> time +0000" */
{
    char *nm = cfg_get("user.name"), *em = cfg_get("user.email"), host[32] = "sieos";
    acct_user_t u;
    mk_ident_t me;
    sys_ident(0, &me);
    int known = !acct_user(0, me.uid, &u);
    char *hn = get_file("/etc/hostname", 0);
    if (hn) { strlcpy(host, hn, sizeof host); char *e = strchr(host, '\n'); if (e) *e = 0; free(hn); }
    buf_t b = { 0 };
    if (nm) bfmt(&b, "%s", nm); else bfmt(&b, "%s", known ? (u.full[0] ? u.full : u.name) : "SIEOS user");
    if (em) bfmt(&b, " <%s>", em); else bfmt(&b, " <%s@%s>", known ? u.name : "user", host);
    bfmt(&b, " %ld +0000", (long)now);
    free(nm); free(em);
    return b.p;
}
static int make_commit(const oid_t tree, const uint8_t (*par)[20], int np, const char *msg, oid_t id)
{
    siefs_stat_t st;
    put_file(".git/COMMIT_EDITMSG", msg, strlen(msg), 0644);
    now = fs_stat(".git/COMMIT_EDITMSG", &st, 0) ? 0 : st.mtime / 1000000000;
    buf_t b = { 0 };
    char h[41], *who = ident();
    hex(h, tree); bfmt(&b, "tree %s\n", h);
    for (int i = 0; i < np; i++) { hex(h, par[i]); bfmt(&b, "parent %s\n", h); }
    bfmt(&b, "author %s\ncommitter %s\n\n%s\n", who, who, msg);
    int e = obj_write(OBJ_COMMIT, b.p, b.n, id);
    free(b.p); free(who);
    return e ? fail("cannot write the commit") : 0;
}
static int cmd_commit(int ac, char **av)
{
    buf_t msg = { 0 };
    int all = 0;
    for (int i = 1; i < ac; i++) {
        if (!strcmp(av[i], "-a") || !strcmp(av[i], "--all")) all = 1;
        else if (!strcmp(av[i], "-am")) { all = 1; av[i] = "-m"; i--; }
        else if (!strcmp(av[i], "-m")) {            /* the rest is the message (the shell splits words) */
            for (i++; i < ac; i++) bfmt(&msg, msg.n ? " %s" : "%s", av[i]);
        } else return fail("commit: unknown option %s", av[i]);
    }
    if (!msg.n) return fail("no message: git commit -m MESSAGE");
    if (load(all)) return 1;
    if (all)
        for (int i = I.n - 1; i >= 0; i--) {
            int st = wt_state(&I.e[i]);
            if (st == 'D') ents_del(&I, &I.e[i]);
            else if (st == 'M' && stage(ents_find(&W, I.e[i].path))) return 1;
        }
    oid_t tree, ht, id;
    if (tree_write(&I, tree)) return fail("cannot write the tree");
    if (have_head && !commit_tree(head_id, ht) && !memcmp(ht, tree, 20)) { outf("nothing to commit, working tree clean\n"); return 1; }
    if (make_commit(tree, (const uint8_t (*)[20])head_id, have_head, msg.p, id) || ref_write("HEAD", id)) return fail("cannot update HEAD");
    if (all || refreshed) index_write(&I);
    const char *b = branch_name();
    char *nl = strchr(msg.p, '\n');
    if (nl) *nl = 0;
    outf("[%s%s %s] %s\n", b ? b : "detached HEAD", have_head ? "" : " (root-commit)", abbr(id), msg.p);
    return 0;
}

/* ---- log, show, diff */
static void show_date(const char *who)           /* "Name <mail> 1700000000 +0100" -> its date */
{
    const char *gt = strchr(who, '>');
    long t = 0, z = 0;
    char d[20], sign = '+';
    if (!gt) return;
    for (gt += 2; *gt >= '0' && *gt <= '9'; gt++) t = t * 10 + *gt - '0';
    if (*gt == ' ' && (gt[1] == '+' || gt[1] == '-')) { sign = gt[1]; for (gt += 2; *gt >= '0' && *gt <= '9'; gt++) z = z * 10 + *gt - '0'; }
    date_str(((int64_t)t + (sign == '-' ? -1 : 1) * (z / 100 * 3600 + z % 100 * 60)) * 1000000000, d);
    outf("Date:   %s %c%04ld\n", d, sign, z);
}
static void show_commit(const oid_t id, commit_t *c, int oneline)
{
    char h[41];
    hex(h, id);
    if (oneline) {
        char *e = strchr(c->msg, '\n');
        outf("%s ", abbr(id));
        out(c->msg, e ? (size_t)(e - c->msg) : strlen(c->msg));
        out("\n", 1);
        return;
    }
    outf("commit %s\n", h);
    if (c->np > 1) outf("Merge: %s %s\n", abbr(c->parent[0]), abbr(c->parent[1]));
    const char *gt = c->author ? strchr(c->author, '>') : 0;
    outf("Author: "); out(c->author, gt ? (size_t)(gt - c->author) + 1 : 0); out("\n", 1);
    if (c->author) show_date(c->author);
    out("\n", 1);
    for (char *p = c->msg; *p; ) {
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) + 1 : strlen(p);
        out("    ", 4); out(p, l);
        if (!e) out("\n", 1);
        p += l;
    }
}
static int cmd_log(int ac, char **av)
{
    int oneline = 0;
    long max = -1;
    const char *rev = "HEAD";
    for (int i = 1; i < ac; i++) {
        if (!strcmp(av[i], "--oneline")) oneline = 1;
        else if (!strcmp(av[i], "-n") && i + 1 < ac) max = strnum(av[++i]);
        else if (av[i][0] == '-' && strnum(av[i] + 1) >= 0) max = strnum(av[i] + 1);
        else rev = av[i];
    }
    oid_t id;
    if (rev_parse(rev, id)) return !strcmp(rev, "HEAD") ? fail("your current branch does not have any commits yet") : fail("unknown revision %s", rev);
    set_t seen = { 0 };
    buf_t q = { 0 };                                 /* the commits to show: name + time, newest picked first */
    commit_t c;
    if (commit_read(id, &c)) return fail("%s is not a commit", rev);
    bput(&q, id, 20); bput(&q, &c.time, 8);
    free(c.raw);
    set_add(&seen, id);
    for (long shown = 0; q.n && shown != max; shown++) {
        size_t best = 0;
        int64_t bt, t;
        memcpy(&bt, q.p + 20, 8);
        for (size_t i = 28; i < q.n; i += 28) { memcpy(&t, q.p + i + 20, 8); if (t > bt) { bt = t; best = i; } }
        memcpy(id, q.p + best, 20);
        memmove(q.p + best, q.p + best + 28, q.n - best - 28);
        q.n -= 28;
        if (commit_read(id, &c)) continue;
        if (shown && !oneline) out("\n", 1);
        show_commit(id, &c, oneline);
        for (int i = 0; i < c.np; i++)
            if (set_add(&seen, c.parent[i])) {
                commit_t p;
                if (commit_read(c.parent[i], &p)) continue;
                bput(&q, c.parent[i], 20); bput(&q, &p.time, 8);
                free(p.raw);
            }
        free(c.raw);
    }
    return 0;
}
static uint8_t *blob(const oid_t id, size_t *n)
{
    int t;
    uint8_t *d = obj_read(id, &t, n);
    if (!d) { *n = 0; d = (uint8_t *)malloc(1); }
    return d;
}
/* Two lists of files: a diff for each path that differs. */
static void diff_lists(const ents_t *a, const ents_t *b, char **s, int n)
{
    ents_t u = { 0 };
    for (int i = 0; i < a->n; i++) if (inspec(a->e[i].path, s, n)) ents_put(&u, a->e[i].path);
    for (int i = 0; i < b->n; i++) if (inspec(b->e[i].path, s, n)) ents_put(&u, b->e[i].path);
    static const oid_t none;
    for (int i = 0; i < u.n; i++) {
        ent_t *x = ents_find(a, u.e[i].path), *y = ents_find(b, u.e[i].path);
        if (x && y && !memcmp(x->id, y->id, 20) && x->mode == y->mode) continue;
        size_t xn = 0, yn = 0;
        uint8_t *xd = x ? blob(x->id, &xn) : 0, *yd = y ? blob(y->id, &yn) : 0;
        diff_text(u.e[i].path, xd, xn, x ? x->id : none, x ? x->mode : 0, yd, yn, y ? y->id : none, y ? y->mode : 0);
        free(xd); free(yd);
    }
}
static int cmd_diff(int ac, char **av)
{
    char **s;
    int cached = 0, n = specs(ac, av, 1, &s);
    for (int i = 1; i < ac; i++) cached |= !strcmp(av[i], "--cached") || !strcmp(av[i], "--staged");
    if (load(!cached)) return 1;
    if (cached) { diff_lists(&H, &I, s, n); return 0; }
    static const oid_t none;
    for (int i = 0; i < I.n; i++) {                  /* the index against the files */
        ent_t *e = &I.e[i];
        if (!inspec(e->path, s, n)) continue;
        int st = wt_state(e);
        if (!st) continue;
        ent_t *w = ents_find(&W, e->path);
        size_t an, bn = 0;
        uint8_t *a = blob(e->id, &an), *b = w ? wt_read(w->path, w->mode, &bn) : 0;
        diff_text(e->path, a, an, e->id, e->mode, b, bn, w ? w->id : none, w ? w->mode : 0);
        free(a); free(b);
    }
    if (refreshed) index_write(&I);
    return 0;
}
static int cmd_show(int ac, char **av)
{
    oid_t id, pt;
    commit_t c;
    const char *rev = ac > 1 ? av[1] : "HEAD";
    if (rev_parse(rev, id) || commit_read(id, &c)) return fail("unknown revision %s", rev);
    show_commit(id, &c, 0);
    ents_t a = { 0 }, b = { 0 };
    if (c.np && !commit_tree(c.parent[0], pt)) tree_flat(pt, &a);
    tree_flat(c.tree, &b);
    out("\n", 1);
    diff_lists(&a, &b, 0, 0);
    return 0;
}

/* ---- branches and checkout */
static void list_ref(const char *name, const oid_t id, void *ctx)
{
    (void)id;
    const char *cur = ctx, *shown = name + (memcmp(name, "refs/heads/", 11) ? 5 : 11);
    outf("%c %s\n", cur && !strcmp(cur, name) ? '*' : ' ', shown);
}
static int new_branch(const char *name, const char *start)
{
    char ref[300];
    oid_t id;
    sfmt(ref, sizeof ref, "refs/heads/%s", name);
    if (!ref_read(ref, id)) return fail("a branch named '%s' already exists", name);
    if (rev_parse(start ? start : "HEAD", id)) return fail("not a valid commit: %s", start ? start : "HEAD");
    return ref_write(ref, id) ? fail("cannot write %s", ref) : 0;
}
static int cmd_branch(int ac, char **av)
{
    const char *cur = head_ref();
    if (ac < 2 || !strcmp(av[1], "-a")) {
        ref_each("refs/heads/", list_ref, (void *)cur);
        if (ac > 1) ref_each("refs/remotes/", list_ref, 0);
        return 0;
    }
    char ref[300];
    if (!strcmp(av[1], "-d") || !strcmp(av[1], "-D")) {
        if (ac < 3) return fail("branch -d NAME");
        sfmt(ref, sizeof ref, "refs/heads/%s", av[2]);
        if (cur && !strcmp(cur, ref)) return fail("cannot delete the current branch '%s'", av[2]);
        if (ref_delete(ref)) return fail("branch '%s' not found", av[2]);
        outf("Deleted branch %s\n", av[2]);
        return 0;
    }
    return new_branch(av[1], ac > 2 ? av[2] : 0);
}
/* The work tree and index from H (clean) to the tree `to`: files of H not
 * in it removed, the others written where they differ. */
static void rm_file(const char *path)
{
    char p[FS_PATH];
    fs_unlink(path);
    strlcpy(p, path, sizeof p);
    for (char *s = p + strlen(p); s > p; s--) if (*s == '/') { *s = 0; if (fs_rmdir(p)) break; }   /* (folders left empty) */
}
static int checkout_tree(const oid_t to)
{
    ents_t t = { 0 };
    if (tree_flat(to, &t)) return fail("cannot read the tree %s", abbr(to));
    for (int i = 0; i < t.n; i++) {                  /* new files that are there, not followed: refuse */
        ent_t *w = ents_find(&W, t.e[i].path);
        if (w && !ents_find(&I, w->path) && (wt_hash(w, 0) || memcmp(w->id, t.e[i].id, 20)))
            return fail("the untracked file %s would be overwritten: move it first", w->path);
    }
    for (int i = 0; i < H.n; i++) if (!ents_find(&t, H.e[i].path)) rm_file(H.e[i].path);
    for (int i = 0; i < t.n; i++) {
        ent_t *h = ents_find(&H, t.e[i].path), *x = ents_find(&I, t.e[i].path);
        if (h && x && !memcmp(h->id, t.e[i].id, 20) && h->mode == t.e[i].mode) { t.e[i].size = x->size; t.e[i].mtime = x->mtime; continue; }
        if (write_entry(t.e[i].path, t.e[i].mode, t.e[i].id, &t.e[i])) return 1;
    }
    if (index_write(&t)) return fail("cannot write the index");
    I = t;
    return 0;
}
static int cmd_checkout(int ac, char **av)
{
    const char *create = 0, *target = 0, *start = 0;
    for (int i = 1; i < ac; i++) {
        if (!strcmp(av[i], "--")) { av[i] = "-"; return cmd_restore(ac, av); }
        if ((!strcmp(av[i], "-b") || !strcmp(av[i], "-c")) && i + 1 < ac) create = av[++i];
        else if (!target && !create) target = av[i];
        else start = av[i];
    }
    if (create) { if (new_branch(create, start ? start : target)) return 1; target = create; }
    if (!target) return fail("checkout: which branch?");
    char ref[300], rr[300];
    oid_t id, tree;
    sfmt(ref, sizeof ref, "refs/heads/%s", target);
    sfmt(rr, sizeof rr, "refs/remotes/origin/%s", target);
    int branch = !ref_read(ref, id);
    if (!branch && !ref_read(rr, id)) {              /* origin's branch: a branch that follows it */
        char k[300];
        if (ref_write(ref, id)) return fail("cannot write %s", ref);
        sfmt(k, sizeof k, "branch.%s.remote", target); cfg_set(".git/config", k, "origin");
        sfmt(k, sizeof k, "branch.%s.merge", target); cfg_set(".git/config", k, ref);
        outf("branch '%s' set up to track 'origin/%s'.\n", target, target);
        branch = 1;
    }
    if (!branch && rev_parse(target, id)) return fail("'%s' is not a branch or a commit", target);
    if (load(1)) return 1;
    if (dirty()) return fail("you have changes: commit them first (git status)");
    if (commit_tree(id, tree) || checkout_tree(tree)) return 1;
    char h[64];
    int e;
    if (branch) { sfmt(h, sizeof h, "ref: %s\n", ref); e = put_file(".git/HEAD", h, strlen(h), 0644); }
    else { hex(h, id); h[40] = '\n'; e = put_file(".git/HEAD", h, 41, 0644); }
    if (e) return fail("cannot write HEAD");
    if (branch) outf("Switched to branch '%s'\n", target); else outf("HEAD is now at %s\n", abbr(id));
    return 0;
}

/* ---- remotes: fetch, pull (fast-forward or a merge of whole files), push */
static char *remote_url(const char *remote)
{
    char k[300];
    sfmt(k, sizeof k, "remote.%s.url", remote);
    char *u = cfg_get(k);
    if (!u) fail("'%s' is not a remote (git remote add %s URL)", remote, remote);
    return u;
}
static int cmd_fetch(int ac, char **av)
{
    const char *remote = ac > 1 ? av[1] : "origin";
    char *url = remote_url(remote), head[256];
    return url ? fetch(remote, url, head, sizeof head) : 1;
}
static void upstream(const char *b, char *remote, char *branch)   /* what the branch follows */
{
    char k[300], *v;
    sfmt(k, sizeof k, "branch.%s.remote", b);
    strlcpy(remote, (v = cfg_get(k)) ? v : "origin", 128); free(v);
    sfmt(k, sizeof k, "branch.%s.merge", b);
    strlcpy(branch, (v = cfg_get(k)) && !memcmp(v, "refs/heads/", 11) ? v + 11 : b, 256); free(v);
}
static int same(const ent_t *a, const ent_t *b) { return !a ? !b : b && !memcmp(a->id, b->id, 20) && a->mode == b->mode; }
static int merge(const oid_t theirs, const char *what)
{
    oid_t base, tree;
    if (!have_head) {                                /* nothing here yet: theirs */
        if (commit_tree(theirs, tree) || checkout_tree(tree) || ref_write("HEAD", theirs)) return 1;
        return 0;
    }
    if (is_ancestor(theirs, head_id)) { outf("Already up to date.\n"); return 0; }
    if (dirty()) return fail("you have changes: commit them first (git status)");
    if (is_ancestor(head_id, theirs)) {
        outf("Updating %s..%s\nFast-forward\n", abbr(head_id), abbr(theirs));
        if (commit_tree(theirs, tree) || checkout_tree(tree)) return 1;
        return ref_write("HEAD", theirs) ? fail("cannot update HEAD") : 0;
    }
    if (merge_base(head_id, theirs, base)) return fail("refusing to merge unrelated histories");
    ents_t b = { 0 }, t = { 0 }, u = { 0 }, r = { 0 };
    if (commit_tree(base, tree) || tree_flat(tree, &b) || commit_tree(theirs, tree) || tree_flat(tree, &t)) return fail("cannot read the trees to merge");
    const ents_t *l[3] = { &b, &H, &t };
    for (int k = 0; k < 3; k++) for (int i = 0; i < l[k]->n; i++) ents_put(&u, l[k]->e[i].path);
    int conflicts = 0;
    for (int i = 0; i < u.n; i++) {                  /* each file: the side that changed it */
        ent_t *x = ents_find(&b, u.e[i].path), *o = ents_find(&H, u.e[i].path), *y = ents_find(&t, u.e[i].path), *take;
        if (same(o, x)) take = y;
        else if (same(y, x) || same(o, y)) take = o;
        else { outf("CONFLICT: %s changed on both sides\n", u.e[i].path); conflicts++; continue; }
        if (take) { ent_t *e = ents_put(&r, take->path); e->mode = take->mode; memcpy(e->id, take->id, 20); }
    }
    if (conflicts) return fail("not merged: %d file(s) changed on both sides; nothing was changed (merge them by hand)", conflicts);
    oid_t id, par[2];
    char msg[400];
    memcpy(par[0], head_id, 20); memcpy(par[1], theirs, 20);
    sfmt(msg, sizeof msg, "Merge %s", what);
    if (tree_write(&r, tree) || make_commit(tree, (const uint8_t (*)[20])par, 2, msg, id)) return 1;
    if (checkout_tree(tree) || ref_write("HEAD", id)) return 1;
    outf("Merge made: %s (no file changed on both sides)\n", abbr(id));
    return 0;
}
static int cmd_pull(int ac, char **av)
{
    const char *b = branch_name();
    char remote[128], branch[256], head[256], ref[400], what[600];
    if (!b) return fail("not on a branch (detached HEAD)");
    upstream(b, remote, branch);
    if (ac > 1) strlcpy(remote, av[1], sizeof remote);
    if (ac > 2) strlcpy(branch, av[2], sizeof branch);
    char *url = remote_url(remote);
    if (!url || fetch(remote, url, head, sizeof head)) return 1;
    oid_t theirs;
    sfmt(ref, sizeof ref, "refs/remotes/%s/%s", remote, branch);
    if (ref_read(ref, theirs)) return fail("the remote has no branch '%s'", branch);
    sfmt(what, sizeof what, "branch '%s' of %s", branch, url);
    if (load(1)) return 1;
    return merge(theirs, what);
}
static int cmd_push(int ac, char **av)
{
    const char *b = branch_name(), *arg[2] = { 0, 0 };
    int set_up = 0, na = 0;
    for (int i = 1; i < ac; i++)
        if (!strcmp(av[i], "-u") || !strcmp(av[i], "--set-upstream")) set_up = 1;
        else if (na < 2) arg[na++] = av[i];
    char remote[128], branch[256], ref[300], k[300];
    if (!b && !arg[1]) return fail("not on a branch (detached HEAD)");
    upstream(b ? b : arg[1], remote, branch);
    if (arg[0]) strlcpy(remote, arg[0], sizeof remote);
    if (arg[1]) strlcpy(branch, arg[1], sizeof branch);
    else if (b) strlcpy(branch, b, sizeof branch);
    char *url = remote_url(remote);
    sfmt(ref, sizeof ref, "refs/heads/%s", branch);
    if (!url || push(url, ref)) return 1;
    oid_t id;
    if (!ref_read(ref, id)) { sfmt(k, sizeof k, "refs/remotes/%s/%s", remote, branch); ref_write(k, id); }
    if (set_up) {
        sfmt(k, sizeof k, "branch.%s.remote", branch); cfg_set(".git/config", k, remote);
        sfmt(k, sizeof k, "branch.%s.merge", branch); cfg_set(".git/config", k, ref);
    }
    return 0;
}
static int cmd_remote(int ac, char **av)
{
    if (ac > 3 && !strcmp(av[1], "add")) {
        char k[300], v[300];
        sfmt(k, sizeof k, "remote.%s.url", av[2]);
        char *old = cfg_get(k);
        if (old) { free(old); return fail("remote %s already exists", av[2]); }
        if (cfg_set(".git/config", k, av[3])) return 1;
        sfmt(k, sizeof k, "remote.%s.fetch", av[2]);
        sfmt(v, sizeof v, "+refs/heads/*:refs/remotes/%s/*", av[2]);
        return cfg_set(".git/config", k, v);
    }
    int v = ac > 1 && !strcmp(av[1], "-v");
    char *t = get_file(".git/config", 0), *p = t;
    cline_t c;
    c.sec[0] = 0;
    for (int r; t && (r = cfg_line(&p, &c)); ) {
        size_t l = strlen(c.key);
        if (r != 1 || memcmp(c.key, "remote.", 7) || l < 12 || strcmp(c.key + l - 4, ".url")) continue;
        out(c.key + 7, l - 11);
        if (v) { outf("\t"); out(c.val, c.vl); outf(" (fetch)\n"); out(c.key + 7, l - 11); outf("\t"); out(c.val, c.vl); outf(" (push)"); }
        out("\n", 1);
    }
    free(t);
    return 0;
}

/* ---- init, clone, config */
static int init_repo(int quiet)
{
    siefs_stat_t st;
    int again = !fs_stat(".git", &st, 0);
    static const char *const dirs[] = { ".git", ".git/objects", ".git/objects/pack", ".git/objects/info", ".git/refs", ".git/refs/heads", ".git/refs/tags" };
    for (int i = 0; i < 7; i++) fs_mkdir(dirs[i], 0755);
    if (!again) {
        char h[300], *b = cfg_get("init.defaultBranch");
        sfmt(h, sizeof h, "ref: refs/heads/%s\n", b ? b : "main");
        free(b);
        static const char cfg[] = "[core]\n\trepositoryformatversion = 0\n\tfilemode = true\n\tbare = false\n";
        if (put_file(".git/HEAD", h, strlen(h), 0644) || put_file(".git/config", cfg, sizeof cfg - 1, 0644))
            return fail("cannot write in %s", fs_getcwd());
    }
    if (!quiet) outf("%s empty Git repository in %s/.git/\n", again ? "Reinitialized existing" : "Initialized", strcmp(fs_getcwd(), "/") ? fs_getcwd() : "");
    return 0;
}
static int cmd_init(int ac, char **av)
{
    if (ac > 1) { fs_mkdir(av[1], 0755); if (fs_chdir(av[1])) return fail("cannot enter %s", av[1]); }
    return init_repo(0);
}
static int cmd_clone(int ac, char **av)
{
    if (ac < 2) return fail("clone URL [DIR]");
    const char *url = av[1];
    char dir[FS_PATH], head[256], ref[300], k[300];
    if (ac > 2) strlcpy(dir, av[2], sizeof dir);
    else {                                           /* the URL's last part, without ".git" */
        strlcpy(dir, base_of(url), sizeof dir);
        size_t l = strlen(dir);
        if (!l) return fail("say where: git clone URL DIR");
        if (l > 4 && !strcmp(dir + l - 4, ".git")) dir[l - 4] = 0;
    }
    if (fs_mkdir(dir, 0755) && (fs_rmdir(dir) || fs_mkdir(dir, 0755))) return fail("destination path '%s' already exists", dir);
    if (fs_chdir(dir) || init_repo(1)) return fail("cannot make %s", dir);
    outf("Cloning into '%s'...\n", dir);
    oflush();
    if (cfg_set(".git/config", "remote.origin.url", url) ||
        cfg_set(".git/config", "remote.origin.fetch", "+refs/heads/*:refs/remotes/origin/*")) return 1;
    if (fetch("origin", url, head, sizeof head)) return 1;
    oid_t id, tree;
    if (!head[0] || memcmp(head, "refs/heads/", 11)) { outf("warning: You appear to have cloned an empty repository.\n"); return 0; }
    const char *b = head + 11;
    sfmt(ref, sizeof ref, "refs/remotes/origin/%s", b);
    if (ref_read(ref, id)) return fail("the remote's HEAD (%s) is missing", head);
    sfmt(k, sizeof k, "ref: %s\n", ref);
    put_file(".git/refs/remotes/origin/HEAD", k, strlen(k), 0644);
    sfmt(k, sizeof k, "ref: %s\n", head);
    if (ref_write(head, id) || put_file(".git/HEAD", k, strlen(k), 0644)) return fail("cannot write HEAD");
    sfmt(k, sizeof k, "branch.%s.remote", b); cfg_set(".git/config", k, "origin");
    sfmt(k, sizeof k, "branch.%s.merge", b); cfg_set(".git/config", k, head);
    return commit_tree(id, tree) || checkout_tree(tree);
}
static int cmd_config(int ac, char **av)
{
    int global = ac > 1 && !strcmp(av[1], "--global");
    if (global) { av++; ac--; }
    if (ac < 2) return fail("config [--global] KEY [VALUE]");
    if (find_repo(1) && !global && ac > 2) return fail("not in a git repository (use --global)");
    if (ac == 2) {
        char *v = cfg_get(av[1]);
        if (!v) return 1;
        outf("%s\n", v);
        free(v);
        return 0;
    }
    buf_t v = { 0 };                                 /* (the shell splits words: they are put back) */
    for (int i = 2; i < ac; i++) bfmt(&v, i > 2 ? " %s" : "%s", av[i]);
    return cfg_set(global ? gcfg : ".git/config", av[1], v.p);
}

static const struct { const char *name; int (*run)(int, char **); int repo; } cmds[] = {
    { "init", cmd_init, 0 }, { "clone", cmd_clone, 0 }, { "config", cmd_config, 0 },
    { "status", cmd_status, 1 }, { "add", cmd_add, 1 }, { "rm", cmd_rm, 1 }, { "reset", cmd_restore, 1 },
    { "restore", cmd_restore, 1 }, { "commit", cmd_commit, 1 }, { "log", cmd_log, 1 }, { "show", cmd_show, 1 },
    { "diff", cmd_diff, 1 }, { "branch", cmd_branch, 1 }, { "checkout", cmd_checkout, 1 }, { "switch", cmd_checkout, 1 },
    { "fetch", cmd_fetch, 1 }, { "pull", cmd_pull, 1 }, { "push", cmd_push, 1 }, { "remote", cmd_remote, 1 },
};

int main(int argc, char **argv)
{
    acct_user_t u;
    mk_ident_t me;
    if (!sys_ident(0, &me) && !acct_user(0, me.uid, &u)) sfmt(gcfg, sizeof gcfg, "%s/.gitconfig", u.home);
    while (argc > 2 && !strcmp(argv[1], "-C")) {
        if (fs_chdir(argv[2])) { fail("cannot change to '%s'", argv[2]); oflush(); return 128; }
        argv += 2; argc -= 2;
    }
    if (argc < 2) {
        outf("usage: git COMMAND [ARGS]\ncommands:");
        for (size_t i = 0; i < sizeof cmds / sizeof *cmds; i++) outf(" %s", cmds[i].name);
        outf("\n");
        oflush();
        return 1;
    }
    for (size_t i = 0; i < sizeof cmds / sizeof *cmds; i++)
        if (!strcmp(argv[1], cmds[i].name)) {
            int r = cmds[i].repo && find_repo(0) ? 128 : cmds[i].run(argc - 1, argv + 1);
            oflush();
            return r;
        }
    fail("'%s' is not a git command", argv[1]);
    oflush();
    return 1;
}
