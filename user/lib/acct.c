/*
 * acct.c - Reading the account files (docs/accounts.md):
 *   /etc/users    name, uid, gid, home, shell, full name   (everyone may read)
 *   /etc/groups   name, gid, members (comma-separated)     (everyone may read)
 * One record per line, fields separated by tabs; '#' starts a comment.
 * (/etc/secrets, the password hashes, only the accounts server reads.)
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

/* A whole small file, 0-terminated, in malloc'd memory (0 if missing). */
char *file_get(const char *path, size_t *len)
{
    siefs_stat_t st;
    long h = fs_open(path, FS_RDONLY, 0), n = -1;
    if (h < 0) return 0;
    char *b = fs_fstat(h, &st) || st.size > (1 << 20) ? 0 : malloc(st.size + 1);
    if (b && (n = fs_read(h, 0, b, st.size)) >= 0) b[n] = 0;
    fs_close(h);
    if (n < 0) { free(b); return 0; }
    if (len) *len = (size_t)n;
    return b;
}

/* The next record: its fields (tab-separated) cut in place into f[], at most
 * nf of them; returns how many, or -1 at the end of the text. */
int acct_next(char **p, char **f, int nf)
{
    for (;;) {
        char *s = *p;
        if (!*s) return -1;
        char *e = strchr(s, '\n');
        if (e) { *e = 0; *p = e + 1; } else *p = s + strlen(s);
        if (*s == '#' || !*s) continue;
        int n = 0;
        while (n < nf) {
            f[n++] = s;
            char *t = strchr(s, '\t');
            if (!t) break;
            *t = 0;
            s = t + 1;
        }
        for (int i = n; i < nf; i++) f[i] = "";
        return n;
    }
}

static long num(const char *s)
{
    long v = 0;
    if (*s < '0' || *s > '9') return -1;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + *s - '0';
    return *s ? -1 : v;
}

/* The user called `name`, or (name 0) with this uid. 0 or -ENOENT. */
int acct_user(const char *name, int uid, acct_user_t *u)
{
    char *text = file_get("/etc/users", 0), *p = text, *f[6];
    int r = -ENOENT;
    while (text && acct_next(&p, f, 6) >= 5) {
        if (name ? strcmp(f[0], name) : num(f[1]) != uid) continue;
        strlcpy(u->name, f[0], sizeof u->name);
        u->uid = (int)num(f[1]);
        u->gid = (int)num(f[2]);
        strlcpy(u->home, f[3], sizeof u->home);
        strlcpy(u->shell, f[4], sizeof u->shell);
        strlcpy(u->full, f[5], sizeof u->full);
        r = u->uid < 0 || u->gid < 0 ? -EINVAL : 0;
        break;
    }
    free(text);
    return r;
}

/* The groups that list `name` as a member (the "other" groups of a user). */
int acct_groups(const char *name, mk_groups_t *g)
{
    char *text = file_get("/etc/groups", 0), *p = text, *f[3];
    g->n = 0;
    while (text && acct_next(&p, f, 3) >= 2) {
        for (char *m = f[2]; *m && g->n < NGROUPS; ) {
            char *e = strchr(m, ',');
            size_t l = e ? (size_t)(e - m) : strlen(m);
            if (l == strlen(name) && !memcmp(m, name, l) && num(f[1]) >= 0) { g->g[g->n++] = (int)num(f[1]); break; }
            m += l + (e != 0);
        }
    }
    free(text);
    return 0;
}

/* A group's name (into buf), or its number if it has none. */
const char *acct_group_name(int gid, char *buf, size_t size)
{
    char *text = file_get("/etc/groups", 0), *p = text, *f[3];
    int found = 0;
    while (text && !found && acct_next(&p, f, 3) >= 2)
        if (num(f[1]) == gid) { strlcpy(buf, f[0], size); found = 1; }
    free(text);
    if (!found) { char t[12]; int n = 0; do t[n++] = (char)('0' + gid % 10); while (gid /= 10); for (int i = 0; i < n; i++) buf[i] = t[n - 1 - i]; buf[n] = 0; }
    return buf;
}
