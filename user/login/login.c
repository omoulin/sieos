/*
 * login - The console's login prompt (started by init). It only talks: the
 * accounts server checks the password and starts the user's shell as a
 * child of login, which waits for it to end; then the prompt comes back.
 * (If login itself crashes, init adopts the running session and starts a
 * new login only once that session has ended: never two on the console.)
 *
 * On the very first start there is no account at all (no password is ever
 * built into the system). The accounts are then created on the screen, by
 * the desktop's welcome screen; login just says so and waits. Only when
 * there is no screen (no display card: the desktop ended without one, or
 * keeps failing) does login ask for root's password and a first user
 * itself. The accounts server decides who does it (docs/accounts.md).
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "mk/crypto.h"

static long auth;

/* A line from the console, without its newline; hidden for passwords. */
static long ask(const char *prompt, char *buf, size_t size, int hidden)
{
    printf("%s", prompt);
    if (hidden) con_echo(0);
    long n = con_read(buf, size - 1);
    if (hidden) { con_echo(1); printf("\n"); }
    if (n < 0) n = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = 0;
    return n;
}

static long call(int op, uint64_t flags, const char *data, size_t len)
{
    msg_t m = { .w = { op, flags }, .sbuf = data, .slen = len };
    /* auth may have been restarted: find it again. A login is not repeated
     * if auth died during it (it may have started the shell already). */
    long e = call_named(&auth, "auth", &m, op != AUTH_LOGIN);
    return e < 0 ? e : (long)m.w[0];
}

/* "a\0b\0c..." in out; returns its length. */
static size_t pack(char *out, size_t size, const char **s, int n)
{
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(s[i]) + 1;
        if (len + l > size) return 0;
        memcpy(out + len, s[i], l);
        len += l;
    }
    return len;
}

/* Ask for a new password twice (the rule: docs/accounts.md). */
static int new_password(const char *who, char *pw, size_t size)
{
    char again[128], p[96];
    for (;;) {
        strlcpy(p, "New password for ", sizeof p);
        strlcpy(p + strlen(p), who, sizeof p - strlen(p));
        strlcpy(p + strlen(p), ": ", sizeof p - strlen(p));
        ask(p, pw, size, 1);
        if (strlen(pw) < AUTH_PWMIN || !strcmp(pw, who)) {
            printf("At least %d characters, and not the name itself.\n", AUTH_PWMIN);
            continue;
        }
        ask("Again: ", again, sizeof again, 1);
        int same = !strcmp(pw, again);
        wipe(again, sizeof again);
        if (same) return 0;
        printf("They differ, try again.\n");
    }
}

/* The terminal's first start: only when there is no screen. -> 0 done,
 * -EPERM: the screen does it after all. */
static long first_start_here(void)
{
    char rootpw[128], name[32], full[64], pw[128], buf[400];
    long r;
    printf("\nWelcome to SIEOS. This is the first start, and there is no screen:\n"
           "choose the password of root, the administrator, then create your own account.\n\n");
    new_password("root", rootpw, sizeof rootpw);
    for (;;) {
        ask("Your user name (lower case, e.g. alice): ", name, sizeof name, 0);
        ask("Your full name: ", full, sizeof full, 0);
        new_password(name, pw, sizeof pw);
        const char *s[] = { rootpw, name, full, pw };
        size_t len = pack(buf, sizeof buf, s, 4);
        r = len ? call(AUTH_SETUP, 0, buf, len) : -EINVAL;
        wipe(buf, sizeof buf); wipe(pw, sizeof pw);
        if (r != -EINVAL) break;
        printf("That name is not allowed: 1 to 31 lower-case letters, digits, '_' or '-',\n"
               "starting with a letter, and not \"root\".\n");
    }
    wipe(rootpw, sizeof rootpw);
    if (r >= 0) printf("\nDone. You can log in now.\n");
    return r;
}

/* Is the desktop (the screen's greeter) still able to do the first start?
 * Not when init says it ended for good: no display card, or given up. */
static int screen_possible(void)
{
    static long init;
    static svc_info_t s[32];
    msg_t m = { .w = { INIT_LIST }, .rbuf = s, .rlen = sizeof s };
    long n = call_named(&init, "init", &m, 1);
    n = n < 0 ? n : (long)m.w[0];
    for (long i = 0; i < n && i < 32; i++)
        if (!strcmp(s[i].name, "atlas")) return s[i].state != SVC_STOPPED && s[i].state != SVC_GIVEN_UP;
    return 0;                                   /* no desktop at all */
}

/* No account yet: wait while the screen creates them, or create them here
 * when there is no screen. Returns once accounts exist. */
static void first_start(void)
{
    int told = 0;
    for (;;) {
        if (!screen_possible()) call(AUTH_DISPLAY, 0, 0, 0);    /* tell auth: no screen */
        else if (!told) {
            printf("\nFirst start: please complete the setup on the screen.\n");
            told = 1;
        }
        msg_t m = { .w = { AUTH_STATE, AUTH_WAIT } };
        long r = call_named(&auth, "auth", &m, 0);      /* (not resent: we check again first) */
        r = r < 0 ? r : (long)m.w[0];
        if (r == 1) return;
        if (r == 2 && first_start_here() >= 0) return;
        if (r < 0) sys_sleep(100000000);                /* auth restarting: in a moment */
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char host[32] = "sieos", name[32], pw[128], buf[200];
    char *h = file_get("/etc/hostname", 0);
    if (h) { strlcpy(host, h, sizeof host); char *e = strchr(host, '\n'); if (e) *e = 0; free(h); }
    for (;;) {
        if (call(AUTH_STATE, 0, 0, 0) == 0) first_start();
        printf("\n%s login: ", host);
        if (!ask("", name, sizeof name, 0)) continue;
        ask("Password: ", pw, sizeof pw, 1);
        const char *s[] = { name, pw };
        size_t len = pack(buf, sizeof buf, s, 2);
        long r = len ? call(AUTH_LOGIN, 0, buf, len) : -EINVAL;   /* -> the shell's pid: our child */
        wipe(buf, sizeof buf); wipe(pw, sizeof pw);
        if (r > 0) { int st; sys_wait((int)r, &st, 0); continue; }   /* the session, until it ends */
        if (r == -EACCES || r == -EINVAL) printf("Login incorrect.\n");
        else if (r < 0) printf("login: error %ld\n", -r);
    }
}
