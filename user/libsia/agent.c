/*
 * agent.c - libsia without a terminal: requests and events in JSON lines on
 * stdin/stdout (sia_agent_main).  sia-agent (the Facet strip's) is this loop
 * with the desktop's and the commands' tools; a program with its own tools
 * (MiR's agent, for one) runs it with them.  The protocol is in
 * user/sia/sia-agent.c.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "libsia.h"

static struct sia_session *S;
static const struct sia_agent *A;

static void emit(const char *ev, const char *key, const char *text, size_t len)
{
    struct sbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ev\":\"%s\"", ev);
    if (key) {
        sb_printf(&b, ",\"%s\":", key);
        sb_json_strn(&b, text ? text : "", text ? len : 0);
    }
    sb_puts(&b, "}\n");
    write(1, b.s, b.len);
    sb_free(&b);
}

static void emit_str(const char *ev, const char *text)
{
    emit(ev, "text", text, text ? strlen(text) : 0);
}

/* One line from stdin (NULL on end of file). */
static struct json *next_request(void)
{
    static struct sbuf line;
    for (;;) {
        sb_free(&line);
        sb_init(&line);
        char c;
        long r;
        while ((r = read(0, &c, 1)) == 1 && c != '\n')
            sb_putc(&line, c);
        if (r < 0 && errno == EINTR && !line.len)
            continue;
        if (r <= 0 && !line.len)
            return NULL;
        struct json *j = json_parse(line.s, line.len);
        if (j)
            return j;
    }
}

static void io_text(void *ctx, const char *utf8)
{
    (void)ctx;
    struct sbuf b;
    sb_init(&b);
    sia_plain_text(utf8, &b);
    emit("text", "text", b.s, b.len);
    sb_free(&b);
}

/* The reply as it is written: "delta" events, then "delta_end". */
static void io_delta(void *ctx, const char *utf8)
{
    (void)ctx;
    if (!utf8) {
        emit_str("delta_end", "");
        return;
    }
    struct sbuf b;
    sb_init(&b);
    sia_plain_text(utf8, &b);
    if (b.len)
        emit("delta", "text", b.s, b.len);
    sb_free(&b);
}

static void io_tool(void *ctx, const char *cmdline) { (void)ctx; emit_str("tool", cmdline); }

static void io_output(void *ctx, const char *buf, size_t n)
{
    (void)ctx;
    while (n) {
        size_t k = n > 2048 ? 2048 : n;
        emit("output", "text", buf, k);
        buf += k;
        n -= k;
    }
}

static int io_confirm(void *ctx, const char *cmdline)
{
    (void)ctx;
    emit_str("confirm", cmdline);
    for (;;) {
        struct json *j = next_request();
        if (!j)
            return 0;
        const char *op = json_get_str(j, "op");
        if (op && !strcmp(op, "confirm")) {
            const struct json *a = json_get(j, "answer");
            int answer = a && a->type == JSON_NUMBER ? (int)a->num : 0;
            json_free(j);
            return answer;
        }
        json_free(j);                               /* anything else waits */
    }
}

static void io_thinking(void *ctx, bool on)
{
    (void)ctx;
    const char *m = on ? "{\"ev\":\"thinking\",\"on\":true}\n" : "{\"ev\":\"thinking\",\"on\":false}\n";
    write(1, m, strlen(m));
}

static void io_error(void *ctx, const char *msg) { (void)ctx; emit_str("error", msg); }

static void io_event(void *ctx, const char *name, const char *value) { (void)ctx; emit_str(name, value); }

static const struct sia_io agent_io = { NULL, io_text, io_tool, io_output, io_confirm, io_thinking, io_error,
                                        io_delta, io_event };

static void on_sigint(int sig)
{
    (void)sig;
    sia_interrupt();
}

/* (Re)connect; reports ready or unavailable. */
static bool connect_model(void)
{
    struct sia_config cfg;
    int r = sia_config_load(&cfg);
    if (r != 1) {
        emit_str("unavailable", r < 0 ? "no model registered yet (open a Terminal to set one up)"
                                      : "no model registered (run 'sia --setup' in a terminal)");
        return false;
    }
    char err[512];
    if (A->vision_test && cfg.vision < 0) {         /* does the model see images?  Tested once */
        emit_str("tool", "checking whether the model sees images");
        int v = sia_vision_test(&cfg, err, sizeof(err));
        if (v >= 0) {
            cfg.vision = v;
            sia_profile_set_vision(cfg.profile[0] ? cfg.profile : cfg.model, v);
        }
    }
    struct sia_session *n = sia_session_new(&cfg, A->role, &agent_io, err, sizeof(err));
    int vision = cfg.vision;
    memset(&cfg, 0, sizeof(cfg));
    if (!n || !sia_ping(n, err, sizeof(err))) {
        if (n)
            sia_session_free(n);
        emit_str("unavailable", err);
        return false;
    }
    if (S)
        sia_session_free(S);
    S = n;
    A->setup(S, A->ctx);
    struct sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"ev\":\"ready\",\"model\":");
    sb_json_str(&b, sia_model_name(S));
    sb_printf(&b, ",\"vision\":%s}\n", vision > 0 ? "true" : "false");
    write(1, b.s, b.len);
    sb_free(&b);
    return true;
}

int sia_agent_main(const struct sia_agent *a)
{
    A = a;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;                      /* no SA_RESTART: interrupts network waits */
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_DFL);
    connect_model();
    for (;;) {
        struct json *j = next_request();
        if (!j)
            return 0;
        const char *op = json_get_str(j, "op");
        if (op && !strcmp(op, "ask")) {
            const char *text = json_get_str(j, "text");
            bool ok = false;
            if (!S)
                connect_model();
            if (S && text && *text)
                ok = sia_ask(S, text);
            const char *m = ok ? "{\"ev\":\"done\",\"ok\":true}\n" : "{\"ev\":\"done\",\"ok\":false}\n";
            write(1, m, strlen(m));
        } else if (op && !strcmp(op, "clear")) {
            if (S)
                sia_clear(S);
        } else if (op && !strcmp(op, "reload")) {
            connect_model();
        }
        json_free(j);
    }
}
