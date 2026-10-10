/*
 * svc - The services init supervises (port "init"):
 *   svc                       list them: state, pid, restarts, last status, uptime, memory
 *   svc restart NAME          stop it if running, start it again (root)
 *   svc stop NAME             stop it for good (root)
 *   svc add [-p PORT [-i SECONDS]] NAME PATH [always|on-failure|never] [ARGS...]
 *                             supervise a program from the disk (root); with -p it
 *                             starts on demand, when PORT is looked up
 *   svc idle NAME SECONDS     an on-demand service stops after this long unused
 *                             (0: never) (root)
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static long init;

static long call(msg_t *m)
{
    long e = call_named(&init, "init", m, 0);
    return e < 0 ? e : (long)m->w[0];
}

/* The memory of process pid and its limit (0: none), in KiB. */
static uint64_t mem_of(int pid, uint64_t *limit)
{
    mk_task_t t;
    int on = pid > 0 && sys_tasks(&t, 1, pid - 1) == 1 && t.pid == pid;
    *limit = on ? t.quota * 4 : 0;
    return on ? t.pages * 4 : 0;
}

/* KiB as "512 K", "12 M" or "4608 M" (b: 16 bytes). */
static void kib(char *b, uint64_t k)
{
    char d[24], *p = d + sizeof d;
    uint64_t v = k >= 10240 ? k / 1024 : k;
    *--p = 0; *--p = k >= 10240 ? 'M' : 'K'; *--p = ' ';
    do *--p = '0' + v % 10; while (v /= 10);
    strlcpy(b, p, 16);
}

static int fail(const char *what, long e)
{
    printf("svc: %s: %s\n", what, e == -EPERM ? "only root may do that" : e == -ENOENT ? "no such service" :
           e == -EINVAL ? "invalid" : "failed");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        static svc_info_t s[16];
        static const char *state[] = { "running", "waiting", "GIVEN UP", "stopped", "idle" };
        static const char *policy[] = { "always", "on-failure", "never" };
        msg_t m = { .w = { INIT_LIST }, .rbuf = s, .rlen = sizeof s };
        long n = call(&m);
        if (n < 0) return fail("list", n);
        uint64_t now = sys_clock();
        printf("NAME      STATE      PID  RESTARTS  LAST  POLICY      UPTIME      MEM    LIMIT  STARTS  ON DEMAND\n");
        for (long i = 0; i < n; i++) {
            uint64_t up = s[i].state == SVC_RUNNING ? (now - s[i].started_ns) / 1000000000 : 0;
            uint64_t lim, mem = mem_of(s[i].pid, &lim);
            char a[16], b[16];
            kib(a, mem);
            if (lim) kib(b, lim); else strlcpy(b, "-", sizeof b);
            char d[40] = "-";              /* on demand: the port that starts it, its idle time */
            if (s[i].port[0]) {
                char t[24];
                uint64_t sec = s[i].idle_ns / 1000000000;
                strlcpy(d, s[i].port, sizeof d);
                strlcpy(t, sec ? ", idle " : ", never stops", sizeof t);
                strlcpy(d + strlen(d), t, sizeof d - strlen(d));
                if (sec) { char *q = t + sizeof t; *--q = 0; *--q = 's'; do *--q = '0' + sec % 10; while (sec /= 10);
                           strlcpy(d + strlen(d), q, sizeof d - strlen(d)); }
            }
            printf("%-8s  %-9s %4d  %8d  %4d  %-10s  %3lu:%02lu:%02lu  %6s  %7s  %6d  %s%s\n", s[i].name, state[s[i].state],
                   s[i].pid, s[i].restarts, s[i].last_status, policy[s[i].policy],
                   up / 3600, up / 60 % 60, up % 60, a, b, s[i].starts, d, s[i].driver ? "  driver" : "");
        }
        return 0;
    }
    if (argc == 3 && (!strcmp(argv[1], "restart") || !strcmp(argv[1], "stop"))) {
        msg_t m = { .w = { argv[1][0] == 'r' ? INIT_RESTART : INIT_STOP }, .sbuf = argv[2], .slen = strlen(argv[2]) + 1 };
        long r = call(&m);
        return r < 0 ? fail(argv[2], r) : 0;
    }
    if (argc == 4 && !strcmp(argv[1], "idle")) {
        msg_t m = { .w = { INIT_IDLE, strnum(argv[3]) }, .sbuf = argv[2], .slen = strlen(argv[2]) + 1 };
        long r = call(&m);
        return r < 0 ? fail(argv[2], r) : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "add")) {
        const char *dport = "";            /* -p PORT -i SECONDS: started on demand */
        uint64_t idle = 0;
        while (argc >= 6 && argv[2][0] == '-') {
            if (!strcmp(argv[2], "-p")) dport = argv[3];
            else if (!strcmp(argv[2], "-i")) idle = strnum(argv[3]);
            else break;
            argv += 2; argc -= 2;
        }
        int pol = SVC_ALWAYS, a = 4;
        if (argc > 4) {
            if (!strcmp(argv[4], "on-failure")) pol = SVC_ON_FAILURE, a = 5;
            else if (!strcmp(argv[4], "never")) pol = SVC_NEVER, a = 5;
            else if (!strcmp(argv[4], "always")) a = 5;
        }
        char req[256];
        size_t n = 0;
        n += strlcpy(req + n, argv[2], sizeof req - n) + 1;
        n += strlcpy(req + n, argv[3], sizeof req - n) + 1;
        for (int i = a; i < argc && n < sizeof req - 1; i++) {     /* "name arg arg" */
            if (i == a) { n += strlcpy(req + n, argv[2], sizeof req - n); }
            req[n++] = ' ';
            n += strlcpy(req + n, argv[i], sizeof req - n);
        }
        if (n >= sizeof req) return fail("add", -EINVAL);
        req[n++] = 0;
        n += strlcpy(req + n, dport, sizeof req - n) + 1;   /* "name\0path\0args\0port" */
        if (n > sizeof req) return fail("add", -EINVAL);
        msg_t m = { .w = { INIT_ADD, pol, idle }, .sbuf = req, .slen = n };
        long r = call(&m);
        return r < 0 ? fail(argv[2], r) : 0;
    }
    printf("usage: svc | svc restart NAME | svc stop NAME | svc idle NAME SECONDS\n"
           "       svc add [-p PORT [-i SECONDS]] NAME PATH [always|on-failure|never] [ARGS...]\n");
    return 1;
}
