/*
 * auth - The accounts server (port "auth"; started as root by init).
 *
 *   login ──AUTH_LOGIN "alice\0pw"──► auth: Argon2id(pw, salt) = stored hash?
 *                                      └► starts /bin/sh as alice (her uid,
 *                                         gid, groups) as a child of login,
 *                                         and answers its pid at once;
 *                                         login waits for its end itself
 *
 * So a session does not depend on auth: if auth crashes and init restarts
 * it, the sessions it started carry on.
 *
 * Why a server: a program run by a user has that user's identity, so it
 * can neither read the password hashes nor start a program as someone
 * else. Instead it asks auth, which knows who asks (the kernel stamps the
 * caller's uid on every message) and decides. That is how login, su and
 * passwd work without any program that changes its own identity.
 *
 * The first start (no account yet) is decided here, not by the screen nor
 * the terminal: the desktop says "I am a screen, I do it" (AUTH_DISPLAY 1)
 * or "there is no screen" (AUTH_DISPLAY 0); login waits (AUTH_STATE with
 * AUTH_WAIT) and is told either "accounts exist" or "no screen: you do it".
 * Only that owner may then create the accounts, all at once (AUTH_SETUP),
 * and only while root has no account.
 *
 * It alone reads /etc/secrets (mode 0600) and rewrites the account files:
 * each change is written to a new file that then replaces the old one in a
 * single rename, so a crash never leaves half a file. It hashes passwords
 * with Argon2id (16 MiB), and gives that memory back right after.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "mk/crypto.h"

#define HASH_KIB 16384                  /* Argon2id memory, KiB (16 MiB) */
#define HASH_T   24                     /* passes: about 0.25 s per check (docs/accounts.md) */
#define SALT     16
#define TAG      32

static long port;
static char req[1024];

static const char H_USERS[]   = "# SIEOS users: name, uid, gid, home, shell, full name (tab-separated)\n";
static const char H_GROUPS[]  = "# SIEOS groups: name, gid, members (comma-separated)\n";
static const char H_SECRETS[] = "# SIEOS passwords: name, method, memory (KiB), passes, lanes, salt, hash (hex)\n";

/* A line for the kernel log (the serial port): who logged in, and when. */
typedef struct { char b[160]; size_t n; } line_t;
static void lput(void *c, char ch) { line_t *l = c; if (l->n < sizeof l->b - 1) l->b[l->n++] = ch; }
static void note(const char *fmt, ...)
{
    line_t l = { .n = 0 };
    va_list ap;
    va_start(ap, fmt);
    vformat(lput, &l, fmt, ap);
    va_end(ap);
    sys_debug(l.b, l.n);
}

/* ---- Growing text, to rebuild a file. */
typedef struct { char *p; size_t n, cap; } text_t;
static void add(text_t *t, const char *s)
{
    size_t l = strlen(s);
    if (t->n + l + 1 > t->cap) { t->cap = (t->n + l + 1) * 2; t->p = realloc(t->p, t->cap); }
    if (!t->p) return;
    memcpy(t->p + t->n, s, l + 1);
    t->n += l;
}
static void addf(text_t *t, char **f, int n)          /* a record: fields, tabs, newline */
{
    for (int i = 0; i < n; i++) { if (i) add(t, "\t"); add(t, f[i]); }
    add(t, "\n");
}

/* Replace a whole file at once: write "path.new", then rename it over. */
static int file_put(const char *path, const text_t *t, int mode)
{
    char tmp[64];
    strlcpy(tmp, path, sizeof tmp - 4);
    strlcpy(tmp + strlen(tmp), ".new", 5);
    if (!t->p) return -ENOMEM;
    fs_unlink(tmp);
    long h = fs_open(tmp, FS_WRONLY | FS_CREAT | FS_EXCL, mode), r;
    if (h < 0) return (int)h;
    r = fs_write(h, 0, t->p, t->n);
    fs_close(h);
    if (r != (long)t->n) { fs_unlink(tmp); return r < 0 ? (int)r : -EIO; }
    return (int)fs_rename(tmp, path);
}

/* Rewrite a file: every record except the one named `drop`, then `extra`. */
static int file_edit(const char *path, const char *head, int nf, const char *drop, const char *extra, int mode)
{
    char *old = file_get(path, 0), *p = old, *f[8];
    text_t t = { 0 };
    add(&t, head);
    int n;
    while (old && (n = acct_next(&p, f, nf)) >= 0)
        if (!drop || strcmp(f[0], drop)) addf(&t, f, n);
    if (extra) add(&t, extra);
    int r = file_put(path, &t, mode);
    free(old); free(t.p);
    return r;
}

/* /etc/groups: add `name` to group `to`'s members, or (to 0) remove it everywhere. */
static int members(const char *name, const char *to)
{
    char *old = file_get("/etc/groups", 0), *p = old, *f[3], m[512];
    size_t nl = strlen(name);
    text_t t = { 0 };
    add(&t, H_GROUPS);
    while (old && acct_next(&p, f, 3) >= 2) {
        size_t ml = 0;
        for (char *s = f[2]; *s; ) {                  /* the members, without `name` */
            char *e = strchr(s, ',');
            size_t l = e ? (size_t)(e - s) : strlen(s);
            if ((l != nl || memcmp(s, name, l)) && ml + l + 2 < sizeof m) {
                if (ml) m[ml++] = ',';
                memcpy(m + ml, s, l);
                ml += l;
            }
            s += l + (e != 0);
        }
        if (to && !strcmp(f[0], to) && ml + nl + 2 < sizeof m) {   /* ... and with it, in `to` */
            if (ml) m[ml++] = ',';
            memcpy(m + ml, name, nl);
            ml += nl;
        }
        m[ml] = 0;
        f[2] = m;
        addf(&t, f, 3);
    }
    int r = file_put("/etc/groups", &t, 0644);
    free(old); free(t.p);
    return r;
}

/* ---- Passwords. */
static void tohex(char *o, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) { o[2 * i] = "0123456789abcdef"[p[i] >> 4]; o[2 * i + 1] = "0123456789abcdef"[p[i] & 15]; } o[2 * n] = 0; }
static int fromhex(uint8_t *o, const char *s, size_t n)
{
    for (size_t i = 0; i < 2 * n; i++) {
        int c = s[i], v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return -1;
        o[i / 2] = (uint8_t)(i % 2 ? o[i / 2] << 4 | v : v);
    }
    return s[2 * n] ? -1 : 0;
}
static void itoa_(char *b, long v) { char t[20]; int n = 0; do t[n++] = (char)('0' + v % 10); while (v /= 10); for (int i = 0; i < n; i++) b[i] = t[n - 1 - i]; b[n] = 0; }
static uint32_t num(const char *s) { uint32_t v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0'); return v; }

/* Argon2id, with memory taken for the time of the hash only. */
static int hash(const char *pw, const uint8_t *salt, uint32_t kib, uint32_t t, uint8_t *out)
{
    if (kib < 8 || kib > (1 << 20) || !t || t > 64) return -EINVAL;
    void *mem = malloc((size_t)kib << 10);
    if (!mem) return -ENOMEM;
    argon2_t a = { pw, salt, 0, 0, (uint32_t)strlen(pw), SALT, 0, 0, t, kib, 1, ARGON2_ID };
    int r = argon2(&a, out, TAG, mem);
    wipe(mem, (size_t)kib << 10);
    free(mem);                                          /* back to the system (lib.c) */
    return r ? -EINVAL : 0;
}

static int random_bytes(void *b, size_t n)
{
    for (int i = 0; i < 100; i++) {                     /* the kernel may still be gathering */
        long r = sys_random(b, n);
        if (r == (long)n) return 0;
        if (r != -EAGAIN) return (int)r;
        sys_sleep(50000000);
    }
    return -EAGAIN;
}

/* A new /etc/secrets record for name with password pw. */
static int secret_line(const char *name, const char *pw, char *line, size_t size)
{
    uint8_t salt[SALT], tag[TAG];
    char hs[2 * SALT + 1], ht[2 * TAG + 1];
    int r;
    if ((r = random_bytes(salt, SALT)) || (r = hash(pw, salt, HASH_KIB, HASH_T, tag))) return r;
    tohex(hs, salt, SALT); tohex(ht, tag, TAG);
    char m[12], t[12];
    itoa_(m, HASH_KIB); itoa_(t, HASH_T);
    char *f[] = { (char *)name, "argon2id", m, t, "1", hs, ht };
    text_t x = { 0 };
    addf(&x, f, 7);
    r = x.p && x.n < size ? (strlcpy(line, x.p, size), 0) : -ENOMEM;
    free(x.p);
    wipe(tag, sizeof tag);
    return r;
}

/* Is pw name's password? Unknown names cost the same time (no hint). */
static int verify(const char *name, const char *pw)
{
    char *text = file_get("/etc/secrets", 0), *p = text, *f[7];
    uint8_t salt[SALT] = { 0 }, want[TAG], got[TAG];
    uint32_t kib = HASH_KIB, t = HASH_T;
    int found = 0;
    while (text && !found && acct_next(&p, f, 7) == 7)
        if (!strcmp(f[0], name) && !strcmp(f[1], "argon2id") && num(f[4]) == 1 &&
            !fromhex(salt, f[5], SALT) && !fromhex(want, f[6], TAG)) { kib = num(f[2]); t = num(f[3]); found = 1; }
    if (text) { wipe(text, strlen(text)); free(text); }
    int ok = !hash(pw, salt, kib, t, got) && found && ct_equal(got, want, TAG);
    wipe(got, sizeof got);
    return ok;
}

/* ---- Accounts. */
static int valid_name(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 31 || !((*s >= 'a' && *s <= 'z') || *s == '_')) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return 0;
    return 1;
}
static int valid_text(const char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n') return 0; return 1; }

/* The accounts exist once root's does: the first start writes it last, so
 * a first start cut short (power cut) leaves none, and is simply redone. */
static int accounts_exist(void)
{
    acct_user_t u;
    return !acct_user("root", 0, &u);
}

/* A new password: at least AUTH_PWMIN characters, and not the name itself. */
static int pw_ok(const char *name, const char *pw) { return strlen(pw) >= AUTH_PWMIN && strcmp(pw, name); }

/* Create a user: records, group, home folder (0700, theirs). */
static int user_create(const char *name, const char *full, const char *pw, int uid)
{
    char line[256], u[12], home[64];
    int r;
    if (!valid_name(name) || !valid_text(full) || strlen(full) > 63 || !pw_ok(name, pw)) return -EINVAL;
    acct_user_t x;
    if (!acct_user(name, 0, &x)) return -EEXIST;
    if (uid < 0) {                                      /* the next free uid from 1000 */
        char *text = file_get("/etc/users", 0), *p = text, *f[3];
        uid = 1000;
        while (text && acct_next(&p, f, 3) >= 2) { int v = (int)num(f[1]); if (v >= uid) uid = v + 1; }
        free(text);
    }
    itoa_(u, uid);
    strlcpy(home, uid ? "/home/" : "/", sizeof home);
    strlcpy(home + strlen(home), uid ? name : "root", sizeof home - strlen(home));
    if ((r = secret_line(name, pw, line, sizeof line)) ||
        (r = file_edit("/etc/secrets", H_SECRETS, 7, name, line, 0600))) return r;
    char *g[] = { (char *)name, u, "" };
    text_t t = { 0 };
    addf(&t, g, 3);
    r = file_edit("/etc/groups", H_GROUPS, 3, name, t.p, 0644);
    free(t.p);
    if (r || (uid && (r = members(name, "users")))) return r;
    r = (int)fs_mkdir(home, 0700);
    if (r && r != -EEXIST) return r;
    if ((r = (int)fs_chown(home, uid, uid))) return r;
    char *f[] = { (char *)name, u, u, home, "/bin/sh", (char *)full };
    t = (text_t){ 0 };
    addf(&t, f, 6);
    r = file_edit("/etc/users", H_USERS, 6, 0, t.p, 0644);   /* last: the account now exists */
    free(t.p);
    return r ? r : uid;
}

/* Start name's shell as a child of process `parent` (the caller, which
 * waits for it) -> its pid. console: its terminal ("" : the caller's). */
static long login(const char *name, const char *pw, int nopw, int parent, const char *console)
{
    acct_user_t u;
    uint64_t t0 = sys_clock();
    int known = !acct_user(name, 0, &u);
    if (!nopw && !verify(name, pw)) known = 0;
    if (!known) {
        sys_sleep(1000000000);                          /* slow down guessing */
        note("auth: failed login for %s\n", name);
        return -EACCES;
    }
    mk_spawn_t o = { .flags = SPAWN_GROUPS | SPAWN_PARENT, .parent = parent };
    acct_groups(name, &o.groups);
    if (*console) { o.flags |= SPAWN_CONSOLE; strlcpy(o.console, console, sizeof o.console); }
    const char *base = u.shell + strlen(u.shell);
    while (base > u.shell && base[-1] != '/') base--;
    char args[80];
    strlcpy(args, base, sizeof args - 4);
    strlcpy(args + strlen(args), " -l", 4);
    long pid = spawn_file(u.shell, args, u.uid, u.gid, &o);
    if (pid < 0) return pid;
    note("auth: %s logged in (uid %d, pid %ld, password check %lu ms)\n", name, u.uid, pid,
         nopw ? 0 : (sys_clock() - t0) / 1000000);
    return pid;
}

/* The strings of a request: s[0..n), split at the 0s. */
static int split(char **s, int n, size_t len)
{
    req[len < sizeof req ? len : sizeof req - 1] = 0;
    char *p = req, *end = req + (len < sizeof req ? len : sizeof req - 1);
    for (int i = 0; i < n; i++) {
        if (p > end) return -EINVAL;
        s[i] = p;
        p += strlen(p) + 1;
    }
    return 0;
}

/* How many strings the request holds (at most n), in s. */
static int split_all(char **s, int n, size_t len)
{
    size_t l = len < sizeof req ? len : sizeof req - 1;
    req[l] = 0;
    int k = 0;
    for (char *p = req; p < req + l && k < n; p += strlen(p) + 1) s[k++] = p;
    return k;
}

/* ---- The first start. screen: 0 not known yet, 1 a screen does it (its
 * greeter, process `owner`), 2 no screen: the terminal does it (login,
 * process `owner`). Callers of AUTH_STATE + AUTH_WAIT wait in `waiting`. */
static int screen, owner;
static long waiting[8];                 /* reply tokens */
static int waiting_pid[8], nwaiting;

static void answer_waiting(long v)
{
    for (int i = 0; i < nwaiting; i++) reply_val(waiting[i], v);
    nwaiting = 0;
}

/* AUTH_SETUP: "rootpw\0" then (name, full name, password) for 1 to
 * AUTH_SETUP_MAX users. Everything is checked first; then the users, and
 * root last: until root's record is written, nothing exists. */
static long setup(char **s, int k)
{
    if (k < 4 || (k - 1) % 3 || (k - 1) / 3 > AUTH_SETUP_MAX || !pw_ok("root", s[0])) return -EINVAL;
    for (int i = 1; i < k; i += 3) {
        if (!valid_name(s[i]) || !strcmp(s[i], "root") || !valid_text(s[i + 1]) ||
            strlen(s[i + 1]) > 63 || !pw_ok(s[i], s[i + 2])) return -EINVAL;
        for (int j = 1; j < i; j += 3) if (!strcmp(s[i], s[j])) return -EINVAL;
    }
    for (int i = 1; i < k; i += 3) {                    /* leftovers of a first start cut short */
        acct_user_t u;
        if (acct_user(s[i], 0, &u)) continue;
        file_edit("/etc/users", H_USERS, 6, s[i], 0, 0644);
        file_edit("/etc/secrets", H_SECRETS, 7, s[i], 0, 0600);
        file_edit("/etc/groups", H_GROUPS, 3, s[i], 0, 0644);
        members(s[i], 0);
    }
    long r = file_edit("/etc/groups", H_GROUPS, 3, "users", "users\t100\t\n", 0644);
    for (int i = 1; i < k && r >= 0; i += 3) r = user_create(s[i], *s[i + 1] ? s[i + 1] : s[i], s[i + 2], -1);
    if (r >= 0) r = user_create("root", "Administrator", s[0], 0);   /* last: now the accounts exist */
    if (r < 0) return r;
    note("auth: first start: root and %d user(s) created, %s\n", (k - 1) / 3,
         screen == 1 ? "on the screen" : "on the terminal");
    answer_waiting(1);
    return 0;
}

#define HELD (1L << 40)                     /* serve(): the answer comes later */

static long serve(msg_t *m, long token)
{
    char *s[1 + 3 * AUTH_SETUP_MAX];
    int root = m->uid == 0;
    switch (m->w[0]) {
    case AUTH_STATE:
        if (accounts_exist()) return 1;
        if (!(m->w[1] & AUTH_WAIT)) return 0;
        if (screen == 2) { owner = m->pid; return 2; }  /* no screen: the terminal does it */
        if (nwaiting == 8) return -EBUSY;
        waiting_pid[nwaiting] = m->pid;
        waiting[nwaiting++] = token;                    /* until it is decided */
        return HELD;
    case AUTH_DISPLAY:
        if (!root) return -EPERM;
        if (accounts_exist()) return 0;
        screen = m->w[1] ? 1 : 2;
        owner = m->w[1] ? m->pid : 0;
        note("auth: first start: %s\n", m->w[1] ? "the screen does it" : "no screen, the terminal does it");
        if (screen == 2 && nwaiting) { owner = waiting_pid[0]; answer_waiting(2); }   /* login: yours */
        return 0;
    case AUTH_SETUP:
        if (!root || accounts_exist() || !screen || (owner && m->pid != owner)) return -EPERM;
        return setup(s, split_all(s, 1 + 3 * AUTH_SETUP_MAX, m->rlen));
    case AUTH_LOGIN: {                          /* "name\0password[\0console]" */
        if (split(s, 2, m->rlen)) return -EINVAL;
        const char *console = s[1] + strlen(s[1]) + 1;
        if (console >= req + (m->rlen < sizeof req ? m->rlen : sizeof req - 1)) console = "";
        return login(s[0], s[1], root && (m->w[1] & AUTH_NOPW), m->pid, console);
    }
    case AUTH_PASSWD: {
        acct_user_t u;
        char line[256];
        if (split(s, 3, m->rlen) || !pw_ok(s[0], s[2]) || acct_user(s[0], 0, &u)) return -EINVAL;
        if (!root && (m->uid != u.uid || !verify(s[0], s[1]))) { sys_sleep(1000000000); return -EACCES; }
        long r = secret_line(s[0], s[2], line, sizeof line);
        if (!r) r = file_edit("/etc/secrets", H_SECRETS, 7, s[0], line, 0600);
        if (!r) note("auth: password of %s changed\n", s[0]);
        return r;
    }
    case AUTH_USERADD: {
        if (!root) return -EPERM;
        if (split(s, 3, m->rlen)) return -EINVAL;
        long r = user_create(s[0], s[1], s[2], -1);
        if (r >= 0) note("auth: user %s added (uid %ld)\n", s[0], r);
        return r;
    }
    case AUTH_USERDEL: {
        acct_user_t u;
        if (!root) return -EPERM;
        if (split(s, 1, m->rlen) || acct_user(s[0], 0, &u)) return -ENOENT;
        if (!u.uid) return -EPERM;
        long r = file_edit("/etc/users", H_USERS, 6, s[0], 0, 0644);
        if (!r) r = file_edit("/etc/secrets", H_SECRETS, 7, s[0], 0, 0600);
        if (!r) r = file_edit("/etc/groups", H_GROUPS, 3, s[0], 0, 0644);
        if (!r) r = members(s[0], 0);
        if (!r) note("auth: user %s removed\n", s[0]);
        return r;
    }
    }
    return -ENOSYS;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    port = port_create("auth");
    uint64_t asked = 0, seen = 0;                       /* requests served; at init's last question */
    for (;;) {
        msg_t m = { .rbuf = req, .rlen = sizeof req - 1 };
        long token = ipc_recv(port, &m);
        if (token <= 0) continue;
        if (m.w[0] == SVC_MAYSTOP) {                    /* started on demand: unused, it ends */
            /* (sessions never depend on auth; the first start's state does) */
            long r = maystop_answer(&m, asked, &seen, nwaiting || !accounts_exist());
            reply_val(token, r);
            if (!r) return 0;
            continue;
        }
        asked++;
        long r = serve(&m, token);
        wipe(req, sizeof req);                          /* passwords */
        if (r != HELD) reply_val(token, r);
    }
}
