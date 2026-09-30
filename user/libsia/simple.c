/*
 * simple.c - libsia for applications (sia/sia.h): one-shot questions and
 * conversations with the user's registered model.  Answers are streamed
 * from the model.  Asynchronous requests run in a child process, which
 * writes frames to a pipe as the answer arrives: a tag byte, a 4-byte
 * little-endian length and the payload -- 'D' a piece of the answer, then
 * 'A' (done, empty) or 'E' (the error message).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <poll.h>
#include "internal.h"
#include "include/sia/sia.h"
#include <signal.h>
#include <sys/wait.h>

#define DEFAULT_INSTRUCTIONS \
    "You are sia, the assistant of SIEOS (Synthetic Intelligence Enhanced Operating System). " \
    "Answer concisely and helpfully."

struct sia_chat {
    struct model mdl;
    char *instructions;
    struct sbuf *msgs;               /* JSON message objects after the system prompt */
    int n, cap;
    char *pending;                   /* the user's text of the request in progress */
    int fd;                          /* its pipe, or -1 */
    pid_t pid;
    struct sbuf in;                  /* frames read but not yet decoded */
    struct sbuf answer;              /* the answer so far */
    struct sbuf fresh;               /* ... of which not yet returned by sia_chat_poll */
    int state;                       /* 0 running, 1 answered, 2 failed */
    char err[512];
};

static bool load_model(struct model *m, char *err, size_t errlen)
{
    struct sia_config cfg;
    int r = sia_config_load(&cfg);
    if (r != 1) {
        snprintf(err, errlen, "no model is registered for sia (run sia once to set one up)");
        return false;
    }
    struct model_cfg mc;
    memset(&mc, 0, sizeof(mc));
    strlcpy(mc.endpoint, cfg.endpoint, sizeof(mc.endpoint));
    strlcpy(mc.model, cfg.model, sizeof(mc.model));
    strlcpy(mc.api_key, cfg.api_key, sizeof(mc.api_key));
    memset(&cfg, 0, sizeof(cfg));
    bool ok = model_init(m, &mc, err, errlen);
    memset(&mc, 0, sizeof(mc));
    return ok;
}

bool sia_available(void)
{
    return sia_model() != NULL;
}

const char *sia_model(void)
{
    static char name[128];
    struct sia_config cfg;
    if (sia_config_load(&cfg) != 1)
        return NULL;
    strlcpy(name, cfg.model, sizeof(name));
    memset(&cfg, 0, sizeof(cfg));
    return name;
}

static void add_msg(sia_chat *c, const char *role, const char *text)
{
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 16;
        c->msgs = realloc(c->msgs, c->cap * sizeof(*c->msgs));
    }
    struct sbuf *b = &c->msgs[c->n++];
    sb_init(b);
    sb_puts(b, "{\"role\":");
    sb_json_str(b, role);
    sb_puts(b, ",\"content\":");
    sb_json_str(b, text);
    sb_puts(b, "}");
}

/* The messages array: system prompt, history, then the new text. */
static void messages(const sia_chat *c, const char *text, struct sbuf *out)
{
    sb_puts(out, "[{\"role\":\"system\",\"content\":");
    sb_json_str(out, c->instructions);
    sb_puts(out, "}");
    for (int i = 0; i < c->n; i++) {
        sb_putc(out, ',');
        sb_putn(out, c->msgs[i].s, c->msgs[i].len);
    }
    sb_puts(out, ",{\"role\":\"user\",\"content\":");
    sb_json_str(out, text);
    sb_puts(out, "}]");
}

struct piece_cb {
    void (*fn)(void *ctx, const char *utf8);
    void *ctx;
};

static bool on_piece(void *ctx, const char *text)
{
    struct piece_cb *p = ctx;
    if (p->fn)
        p->fn(p->ctx, text);
    return true;
}

/* One call to the model, the answer streamed to piece: the answer (malloc'd) or NULL. */
static char *ask(sia_chat *c, const char *text, void (*piece)(void *, const char *), void *ctx, char *err,
                 size_t errlen)
{
    struct sbuf m;
    sb_init(&m);
    messages(c, text, &m);
    struct piece_cb p = { piece, ctx };
    struct json *msg = model_chat_stream(&c->mdl, m.s, NULL, on_piece, &p, err, errlen);
    sb_free(&m);
    if (!msg)
        return NULL;
    const char *content = json_get_str(msg, "content");
    char *answer = strdup(content ? content : "");
    json_free(msg);
    return answer;
}

sia_chat *sia_chat_new(const char *instructions)
{
    sia_chat *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    char err[256];
    if (!load_model(&c->mdl, err, sizeof(err))) {
        free(c);
        errno = ENOENT;
        return NULL;
    }
    c->instructions = strdup(instructions && *instructions ? instructions : DEFAULT_INSTRUCTIONS);
    c->fd = -1;
    return c;
}

void sia_chat_reset(sia_chat *c)
{
    for (int i = 0; i < c->n; i++)
        sb_free(&c->msgs[i]);
    c->n = 0;
}

/* The request in progress is over: reap the child, reset the state. */
static void finish(sia_chat *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    waitpid(c->pid, NULL, 0);
    free(c->pending);
    c->pending = NULL;
    sb_free(&c->in);
    sb_free(&c->answer);
    sb_free(&c->fresh);
}

void sia_chat_cancel(sia_chat *c)
{
    if (c->fd < 0)
        return;
    kill(c->pid, SIGKILL);
    finish(c);
}

void sia_chat_free(sia_chat *c)
{
    if (!c)
        return;
    sia_chat_cancel(c);
    sia_chat_reset(c);
    free(c->msgs);
    free(c->instructions);
    memset(&c->mdl, 0, sizeof(c->mdl));          /* (the key) */
    free(c);
}

char *sia_chat_send(sia_chat *c, const char *text, char *err, size_t errlen)
{
    return sia_chat_send_stream(c, text, NULL, NULL, err, errlen);
}

char *sia_chat_send_stream(sia_chat *c, const char *text, void (*piece)(void *ctx, const char *utf8), void *ctx,
                           char *err, size_t errlen)
{
    if (c->fd >= 0) {
        snprintf(err, errlen, "a request is already in progress");
        return NULL;
    }
    char *answer = ask(c, text, piece, ctx, err, errlen);
    if (answer) {
        add_msg(c, "user", text);
        add_msg(c, "assistant", answer);
    }
    return answer;
}

/* In the child: one frame to the parent. */
static void frame(int fd, char tag, const char *p, size_t n)
{
    unsigned char h[5] = { (unsigned char)tag, (unsigned char)n, (unsigned char)(n >> 8), (unsigned char)(n >> 16),
                           (unsigned char)(n >> 24) };
    struct sbuf b;
    sb_init(&b);
    sb_putn(&b, (const char *)h, 5);
    sb_putn(&b, p, n);
    for (size_t off = 0; off < b.len;) {
        ssize_t k = write(fd, b.s + off, b.len - off);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            break;
        off += (size_t)k;
    }
    sb_free(&b);
}

static void child_piece(void *ctx, const char *utf8)
{
    frame(*(int *)ctx, 'D', utf8, strlen(utf8));
}

int sia_chat_send_async(sia_chat *c, const char *text, char *err, size_t errlen)
{
    if (c->fd >= 0) {
        snprintf(err, errlen, "a request is already in progress");
        return -1;
    }
    int p[2];
    if (pipe(p) < 0) {
        snprintf(err, errlen, "pipe: %s", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        snprintf(err, errlen, "fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        close(p[0]);
        signal(SIGPIPE, SIG_DFL);                      /* the parent gave up: just die */
        char e[512];
        int wfd = p[1];
        char *a = ask(c, text, child_piece, &wfd, e, sizeof(e));
        if (a)
            frame(wfd, 'A', "", 0);
        else
            frame(wfd, 'E', e, strlen(e));
        _exit(0);
    }
    close(p[1]);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    c->fd = p[0];
    c->pid = pid;
    c->pending = strdup(text);
    c->state = 0;
    c->err[0] = 0;
    sb_init(&c->in);
    sb_init(&c->answer);
    sb_init(&c->fresh);
    return c->fd;
}

/* Decode the complete frames in c->in. */
static void decode(sia_chat *c)
{
    size_t off = 0;
    while (c->state == 0 && c->in.len - off >= 5) {
        const unsigned char *h = (const unsigned char *)c->in.s + off;
        size_t n = (size_t)h[1] | (size_t)h[2] << 8 | (size_t)h[3] << 16 | (size_t)h[4] << 24;
        if (c->in.len - off - 5 < n)
            break;
        const char *payload = c->in.s + off + 5;
        if (h[0] == 'D') {
            sb_putn(&c->answer, payload, n);
            sb_putn(&c->fresh, payload, n);
        } else if (h[0] == 'A') {
            c->state = 1;
        } else {
            snprintf(c->err, sizeof(c->err), "%.*s", (int)n, payload);
            c->state = 2;
        }
        off += 5 + n;
    }
    memmove(c->in.s, c->in.s + off, c->in.len - off);
    c->in.len -= off;
}

/* Read what the child has written: wait for it or not. */
static void pump(sia_chat *c, bool wait)
{
    char buf[4096];
    while (c->state == 0) {
        struct pollfd p = { c->fd, POLLIN, 0 };
        if (!wait && poll(&p, 1, 0) != 1)
            return;
        ssize_t n = read(c->fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            snprintf(c->err, sizeof(c->err), "the request failed");
            c->state = 2;
            return;
        }
        sb_putn(&c->in, buf, n);
        decode(c);
        if (!wait)
            return;
    }
}

/* The request is over: record the exchange and hand back the answer, or the error. */
static char *conclude(sia_chat *c, char *err, size_t errlen)
{
    char *answer = NULL;
    if (c->state == 1) {
        answer = strdup(c->answer.s ? c->answer.s : "");
        add_msg(c, "user", c->pending);
        add_msg(c, "assistant", answer);
    } else {
        snprintf(err, errlen, "%s", c->err);
    }
    finish(c);
    return answer;
}

char *sia_chat_poll(sia_chat *c, bool *done, char *err, size_t errlen)
{
    *done = false;
    if (c->fd < 0) {
        *done = true;
        snprintf(err, errlen, "no request in progress");
        return NULL;
    }
    pump(c, false);
    char *piece = strdup(c->fresh.s ? c->fresh.s : "");
    c->fresh.len = 0;
    if (c->state == 0)
        return piece;
    *done = true;
    if (c->state == 2) {
        free(piece);
        piece = NULL;
    }
    char *all = conclude(c, err, errlen);
    free(all);
    return piece;
}

bool sia_chat_busy(const sia_chat *c)
{
    return c->fd >= 0;
}

char *sia_chat_result(sia_chat *c, char *err, size_t errlen)
{
    if (c->fd < 0) {
        snprintf(err, errlen, "no request in progress");
        return NULL;
    }
    pump(c, true);
    return conclude(c, err, errlen);
}

char *sia_complete(const char *instructions, const char *prompt, char *err, size_t errlen)
{
    sia_chat *c = sia_chat_new(instructions);
    if (!c) {
        snprintf(err, errlen, "no model is registered for sia (run sia once to set one up)");
        return NULL;
    }
    char *a = sia_chat_send(c, prompt, err, errlen);
    sia_chat_free(c);
    return a;
}

char *sia_plain(const char *utf8)
{
    struct sbuf b;
    sb_init(&b);
    sia_plain_text(utf8, &b);
    char *r = strdup(b.s ? b.s : "");
    sb_free(&b);
    return r;
}
