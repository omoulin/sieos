/*
 * acct - The account commands, one program under several names (on the
 * disk, /bin/passwd, /bin/su, /bin/useradd and /bin/userdel are links to
 * it; it does what its name says):
 *   passwd [NAME]          change a password (one's own; root: anyone's)
 *   su [NAME]              a shell as NAME (default root), with NAME's password
 *   useradd NAME ["Full"]  add a user (root)
 *   userdel NAME           remove a user (root; the home folder stays)
 * Each only asks the accounts server (port "auth"), which decides from the
 * caller's identity: no program here has any power of its own.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "mk/crypto.h"

static long auth;
static char buf[400];

static long ask(const char *prompt, char *b, size_t size, int hidden)
{
    printf("%s", prompt);
    if (hidden) con_echo(0);
    long n = con_read(b, size - 1);
    if (hidden) { con_echo(1); printf("\n"); }
    if (n < 0) n = 0;
    while (n && b[n - 1] == '\n') n--;
    b[n] = 0;
    return n;
}

static long call(int op, uint64_t flags, const char **s, int n)
{
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(s[i]) + 1;
        if (len + l > sizeof buf) return -EINVAL;
        memcpy(buf + len, s[i], l);
        len += l;
    }
    msg_t m = { .w = { op, flags }, .sbuf = buf, .slen = len };
    long e = call_named(&auth, "auth", &m, op != AUTH_LOGIN);   /* (a login is never repeated) */
    wipe(buf, sizeof buf);
    return e < 0 ? e : (long)m.w[0];
}

static int new_password(char *pw, size_t size)
{
    char again[128];
    if (ask("New password: ", pw, size, 1) < AUTH_PWMIN) {      /* (auth checks it too) */
        printf("At least %d characters, and not the name itself: nothing changed.\n", AUTH_PWMIN);
        return -1;
    }
    ask("Again: ", again, sizeof again, 1);
    int same = !strcmp(pw, again);
    wipe(again, sizeof again);
    if (!same) printf("They differ: nothing changed.\n");
    return same ? 0 : -1;
}

static int fail(const char *what, long r)
{
    if (r == -EACCES) printf("%s: wrong password\n", what);
    else if (r == -EPERM) printf("%s: only root may do that\n", what);
    else if (r == -EEXIST) printf("%s: that user already exists\n", what);
    else if (r == -ENOENT) printf("%s: no such user\n", what);
    else if (r == -EINVAL) printf("%s: invalid name, or password (at least %d characters, not the name)\n", what, AUTH_PWMIN);
    else printf("%s: error %ld\n", what, -r);
    return 1;
}

int main(int argc, char **argv)
{
    char pw[128], old[128] = "";
    mk_ident_t me;
    acct_user_t u;
    const char *cmd = argv[0];
    sys_ident(0, &me);
    if (acct_user(0, me.uid, &u)) strlcpy(u.name, "?", sizeof u.name);

    if (!strcmp(cmd, "passwd")) {
        const char *who = argc > 1 ? argv[1] : u.name;
        if (me.uid && strcmp(who, u.name)) return fail("passwd", -EPERM);
        printf("Changing the password of %s.\n", who);
        if (me.uid) ask("Current password: ", old, sizeof old, 1);
        if (new_password(pw, sizeof pw)) return 1;
        const char *s[] = { who, old, pw };
        long r = call(AUTH_PASSWD, 0, s, 3);
        wipe(pw, sizeof pw); wipe(old, sizeof old);
        if (r < 0) return fail("passwd", r);
        printf("Password changed.\n");
        return 0;
    }
    if (!strcmp(cmd, "su")) {
        const char *who = argc > 1 ? argv[1] : "root";
        if (me.uid) ask("Password: ", pw, sizeof pw, 1); else pw[0] = 0;
        const char *s[] = { who, pw };
        long r = call(AUTH_LOGIN, me.uid ? 0 : AUTH_NOPW, s, 2);   /* -> the shell's pid: our child */
        wipe(pw, sizeof pw);
        if (r < 0) return fail("su", r);
        int st = 0;
        sys_wait((int)r, &st, 0);
        return st;
    }
    if (!strcmp(cmd, "useradd")) {
        if (argc < 2) { printf("usage: useradd NAME [\"Full Name\"]\n"); return 1; }
        if (me.uid) return fail("useradd", -EPERM);
        if (new_password(pw, sizeof pw)) return 1;
        const char *s[] = { argv[1], argc > 2 ? argv[2] : argv[1], pw };
        long r = call(AUTH_USERADD, 0, s, 3);
        wipe(pw, sizeof pw);
        if (r < 0) return fail("useradd", r);
        printf("User %s added: uid %ld, home /home/%s\n", argv[1], r, argv[1]);
        return 0;
    }
    if (!strcmp(cmd, "userdel")) {
        if (argc < 2) { printf("usage: userdel NAME\n"); return 1; }
        const char *s[] = { argv[1] };
        long r = call(AUTH_USERDEL, 0, s, 1);
        if (r < 0) return fail("userdel", r);
        printf("User %s removed (the folder /home/%s is kept).\n", argv[1], argv[1]);
        return 0;
    }
    printf("acct: run me as passwd, su, useradd or userdel\n");
    return 1;
}
