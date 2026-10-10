/*
 * sia - Talk to the assistant (the server siad, port "sia").
 *   sia QUESTION...          one answer, shown as it is written (continues the last conversation)
 *   sia                      a conversation: one message per line, an empty line ends it
 *   sia new [QUESTION...]    the same, in a new conversation
 *   sia resume N             continue conversation N
 *   sia list                 the saved conversations;   sia forget N|all
 *   sia memory               what sia remembers about you;   sia remember "FACT"
 *   sia memory forget N      forget fact N;   sia memory clear
 *   sia info                 the backend, the model, its memory and speed
 *   sia config KEY=VALUE     (root) backend=local|remote, model=/models/x.gguf, ctx=N,
 *                            threads=N, url=https://.../v1, api_model=NAME, api_key=KEY
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

static long port;

static long call(msg_t *m)
{
    long e = call_named(&port, "sia", m, 0);
    return e < 0 ? e : (long)m->w[0];
}

static int fail(long e)
{
    printf("sia: %s\n", e == -ENOENT ? "no model: put a .gguf file in /models, or use a remote one (sia config backend=remote)"
                      : e == -EIO ? "the remote model cannot be reached (sia info; sia config url=...)"
                      : e == -EPIPE ? "the assistant restarted during the answer; ask again"
                      : e == -ESRCH ? "the assistant is stopped (svc restart sia starts it)"
                      : e == -ENOMEM ? "not enough memory for another conversation"
                      : e == -EBUSY ? "too many conversations at once"
                      : e == -EPERM ? "only root may do that" : "failed");
    return 1;
}

/* One message, and its answer written as it comes. */
static long ask(long sess, const char *text)
{
    msg_t m = { .w = { SIA_ASK, sess }, .sbuf = text, .slen = strlen(text) };
    long r = call(&m);
    if (r < 0) return r;
    static char piece[SIA_MAX];
    for (;;) {
        msg_t n = { .w = { SIA_NEXT, sess }, .rbuf = piece, .rlen = sizeof piece };
        r = call(&n);
        if (r <= 0) break;
        con_write(piece, n.rlen);
    }
    con_write("\n", 1);
    return r;
}

/* The arguments from argv[i] on, rejoined with spaces (quotes removed:
 * the shell keeps "quoted words" together, quotes included). */
static const char *join(int argc, char **argv, int i)
{
    static char q[SIA_MAX];
    size_t n = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        size_t l = strlen(a);
        if (*a == '"') a++, l--;
        if (l && a[l - 1] == '"') l--;
        if (n + l + 2 >= sizeof q) break;
        if (n) q[n++] = ' ';
        memcpy(q + n, a, l);
        n += l;
    }
    q[n] = 0;
    return q;
}

/* After an answer: facts the model offered to remember ("[remember: ...]").
 * In a conversation, ask; for a single question, say how to keep them. */
static void offers(int interactive)
{
    static char o[1024], line[16];
    msg_t m = { .w = { SIA_FACTS, SIA_F_OFFERED }, .rbuf = o, .rlen = sizeof o - 1 };
    if (call(&m) < 0 || !m.rlen) return;
    o[m.rlen] = 0;
    for (char *p = o, *e; *p; p = e + 1) {
        if (!(e = strchr(p, '\n'))) break;
        *e = 0;
        if (!interactive) { printf("(sia offers to remember: %s -- to keep it: sia remember \"%s\")\n", p, p); continue; }
        printf("sia offers to remember: %s\nkeep it? [y/N] ", p);
        long n = con_read(line, sizeof line - 1);
        if (n > 0 && (line[0] == 'y' || line[0] == 'Y')) {
            msg_t a = { .w = { SIA_FACTS, SIA_F_ADD }, .sbuf = p, .slen = strlen(p) + 1 };
            long r = call(&a);
            printf(r < 0 ? "not kept (%ld)\n" : "kept\n", r);
        }
    }
}

static int facts_cmd(int op, long line, const char *text)
{
    static char f[2100];
    msg_t m = { .w = { SIA_FACTS, op, line }, .sbuf = text, .slen = text ? strlen(text) + 1 : 0, .rbuf = f, .rlen = sizeof f - 1 };
    long r = call(&m);
    if (r < 0) return r == -ENOSPC ? (printf("sia: the memory is full (2 KB): forget something first\n"), 1) : fail(r);
    if (op != SIA_F_GET) { printf("sia: done\n"); return 0; }
    f[m.rlen] = 0;
    if (!m.rlen) { printf("sia remembers nothing about you yet (sia remember \"...\")\n"); return 0; }
    int k = 1;
    for (char *p = f, *e; *p; p = e + 1, k++) {
        e = strchr(p, '\n');
        if (e) *e = 0;
        printf("%3d  %s\n", k, p);
        if (!e) break;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "list")) {
        static char l[SIA_MAX + 1];
        msg_t m = { .w = { SIA_LIST }, .rbuf = l, .rlen = SIA_MAX };
        long r = call(&m);
        if (r < 0) return fail(r);
        l[m.rlen] = 0;
        printf(m.rlen ? "   N  TURNS  ~TOKENS  FIRST QUESTION\n" : "no saved conversation\n");
        for (char *p = l, *e; *p; p = e + 1) {
            char *f[4] = { p, 0, 0, 0 };
            if (!(e = strchr(p, '\n'))) break;
            *e = 0;
            for (int i = 1; i < 4 && f[i - 1]; i++) if ((f[i] = strchr(f[i - 1], '\t'))) *f[i]++ = 0;
            printf("%4s  %5s  %7s  %s\n", f[0], f[1] ? f[1] : "", f[2] ? f[2] : "", f[3] ? f[3] : "");
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "forget")) {
        long id = strcmp(argv[2], "all") ? strnum(argv[2]) : 0;
        if (id < 0) { printf("sia forget N|all\n"); return 1; }
        msg_t m = { .w = { SIA_FORGET, id } };
        long r = call(&m);
        if (r < 0) return fail(r);
        printf("sia: forgotten\n");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "memory")) return facts_cmd(SIA_F_GET, 0, 0);
    if (argc == 3 && !strcmp(argv[1], "memory") && !strcmp(argv[2], "clear")) return facts_cmd(SIA_F_CLEAR, 0, 0);
    if (argc == 4 && !strcmp(argv[1], "memory") && !strcmp(argv[2], "forget")) return facts_cmd(SIA_F_DEL, strnum(argv[3]), 0);
    if (argc == 2 && !strcmp(argv[1], "info")) {
        static sia_info_t in;
        msg_t m = { .w = { SIA_INFO }, .rbuf = &in, .rlen = sizeof in };
        long r = call(&m);
        if (r < 0) return fail(r);
        printf("backend: %s (%s)\nmodel: %s\ncontext: %d tokens; conversations open: %d\n"
               "memory: %lu MiB\nlast answer: %u.%02u tok/s\n",
               in.backend == SIA_LOCAL ? "local" : "remote", in.ready ? "ready" : "NOT READY",
               in.model[0] ? in.model : "-", in.ctx, in.sessions, in.mem >> 20, in.tok_s_x100 / 100, in.tok_s_x100 % 100);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "config")) {
        msg_t m = { .w = { SIA_CONFIG }, .sbuf = argv[2], .slen = strlen(argv[2]) + 1 };
        long r = call(&m);
        if (r < 0) return fail(r);
        printf("sia: saved; the assistant restarts with it\n");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "remember")) return facts_cmd(SIA_F_ADD, 0, join(argc, argv, 2));
    /* A conversation: the last one (sia, sia QUESTION), a new one (sia new),
     * or number N (sia resume N). */
    int first = 1;
    long id = 0, flags = SIA_RESUME;
    if (argc >= 2 && !strcmp(argv[1], "new")) flags = 0, first = 2;
    else if (argc == 3 && !strcmp(argv[1], "resume")) {
        if ((id = strnum(argv[2])) <= 0) { printf("sia resume N (see: sia list)\n"); return 1; }
        first = 3;
    }
    msg_t o = { .w = { SIA_OPEN, flags, id } };
    long sess = call(&o);
    if (sess < 0) return sess == -ENOENT && id ? (printf("sia: no conversation %ld (sia list)\n", id), 1) : fail(sess);
    if (o.w[2]) printf("(conversation %lu, continued)\n", o.w[1]);
    long r = 0;
    if (argc > first) {                               /* the question: the arguments, rejoined */
        r = ask(sess, join(argc, argv, first));
        if (r >= 0) offers(0);
    } else {
        static char line[1024];
        for (;;) {
            printf("you> ");
            long n = con_read(line, sizeof line - 1);
            if (n <= 0 || line[0] == '\n') break;
            line[n] = 0;
            if (line[n - 1] == '\n') line[n - 1] = 0;
            con_write("sia> ", 5);
            if ((r = ask(sess, line)) < 0) break;
            offers(1);
        }
    }
    msg_t c = { .w = { SIA_CLOSE, sess } };
    call(&c);
    return r < 0 ? fail(r) : 0;
}
