/*
 * sia-agent - libsia without a terminal: one JSON object per line on
 * stdin/stdout.  Facet runs it for the sia strip; any program can.
 *
 * Requests (stdin):
 *   {"op":"ask","text":"open a terminal for me"}
 *   {"op":"confirm","answer":0|1|2}     reply to a "confirm" event (no, yes, always)
 *   {"op":"clear"}                      forget the conversation
 *   {"op":"reload"}                     re-read ~/.sia/config
 * Events (stdout):
 *   {"ev":"ready","model":"..."}   {"ev":"unavailable","text":"why"}
 *   {"ev":"thinking","on":true}    {"ev":"tool","text":"ls -l /etc"}
 *   {"ev":"output","text":"..."}   {"ev":"text","text":"..."}
 *   {"ev":"confirm","text":"rm x"} {"ev":"error","text":"..."}
 *   {"ev":"done","ok":true}
 * SIGINT interrupts the request in progress.
 */
#include "libsia.h"

static struct sia_session *S;

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

static const struct sia_io agent_io = { NULL, io_text, io_tool, io_output, io_confirm, io_thinking, io_error };

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
    struct sia_session *n = sia_session_new(&cfg, SIA_ROLE_DESKTOP, &agent_io, err, sizeof(err));
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
    sia_add_desktop_tools(S);
    sia_add_command_tools(S);
    emit_str("ready", sia_model_name(S));
    return true;
}

int main(void)
{
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
