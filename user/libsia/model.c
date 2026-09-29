/*
 * model.c - Azure AI Foundry chat completions.
 *
 * Endpoints accepted (what the Azure portal shows):
 *   https://NAME.openai.azure.com/                 Azure OpenAI deployment
 *   https://NAME.cognitiveservices.azure.com/      (same API)
 *   https://NAME.openai.azure.com/openai/v1/       Azure OpenAI v1 API
 *   https://NAME.services.ai.azure.com/...         Foundry model inference (any model)
 *   https://X.REGION.models.ai.azure.com           serverless deployment
 *   any full ".../chat/completions" URL            used as given
 */
#include "model.h"

#define API_VERSION_OPENAI "2024-10-21"
#define API_VERSION_FOUNDRY "2024-05-01-preview"
#define TIMEOUT_MS 120000

static bool ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

bool model_init(struct model *m, const struct model_cfg *cfg, char *err, size_t errlen)
{
    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;
    char base[512];
    snprintf(base, sizeof(base), "%s", cfg->endpoint);
    size_t n = strlen(base);
    while (n && (base[n - 1] == '/' || base[n - 1] == ' '))
        base[--n] = 0;
    struct url u;
    if (!url_parse(base, &u)) {
        snprintf(err, errlen, "the endpoint must start with https://");
        return false;
    }
    const char *scheme = u.https ? "https" : "http";
    char hostport[300];
    if ((u.https && u.port == 443) || (!u.https && u.port == 80))
        snprintf(hostport, sizeof(hostport), "%s", u.host);
    else
        snprintf(hostport, sizeof(hostport), "%s:%d", u.host, u.port);
    char full[1200];
    bool openai_host = ends_with(u.host, ".openai.azure.com") || ends_with(u.host, ".cognitiveservices.azure.com");
    if (strstr(base, "/chat/completions")) {
        snprintf(full, sizeof(full), "%s", base);
        m->send_model = true;
        snprintf(m->kind, sizeof(m->kind), "chat completions URL");
    } else if ((openai_host || ends_with(u.host, ".services.ai.azure.com")) && strstr(u.path, "/openai/v1")) {
        snprintf(full, sizeof(full), "%s/chat/completions", base);
        m->send_model = true;
        snprintf(m->kind, sizeof(m->kind), "Azure OpenAI (v1 API)");
    } else if (openai_host || (ends_with(u.host, ".services.ai.azure.com") && !strncmp(u.path, "/openai", 7))) {
        snprintf(full, sizeof(full), "%s://%s/openai/deployments/%s/chat/completions?api-version=%s", scheme,
                 hostport, cfg->model, API_VERSION_OPENAI);
        snprintf(m->kind, sizeof(m->kind), "Azure OpenAI deployment");
    } else if (ends_with(u.host, ".services.ai.azure.com")) {
        snprintf(full, sizeof(full), "%s://%s/models/chat/completions?api-version=%s", scheme, hostport,
                 API_VERSION_FOUNDRY);
        m->send_model = true;
        snprintf(m->kind, sizeof(m->kind), "Azure AI Foundry model inference");
    } else {
        snprintf(full, sizeof(full), "%s/chat/completions", base);
        m->send_model = true;
        m->bearer = !ends_with(u.host, ".azure.com") || ends_with(u.host, ".models.ai.azure.com");
        snprintf(m->kind, sizeof(m->kind), "serverless / OpenAI-compatible endpoint");
    }
    if (!url_parse(full, &m->url)) {
        snprintf(err, errlen, "invalid endpoint URL");
        return false;
    }
    return true;
}

static void auth_header(const struct model *m, struct sbuf *h)
{
    if (m->bearer)
        sb_printf(h, "Authorization: Bearer %s\r\n", m->cfg.api_key);
    else
        sb_printf(h, "api-key: %s\r\n", m->cfg.api_key);
}

/* Error text from an {"error": {...}} body, or the start of the body. */
static void api_error(int status, const struct sbuf *body, char *err, size_t errlen)
{
    struct json *j = json_parse(body->s, body->len);
    const struct json *e = json_get(j, "error");
    const char *msg = json_get_str(e, "message");
    if (!msg)
        msg = json_str(e);
    if (!msg)
        msg = json_get_str(j, "message");
    if (msg)
        snprintf(err, errlen, "HTTP %d: %.300s", status, msg);
    else
        snprintf(err, errlen, "HTTP %d: %.200s", status, body->len ? body->s : "(empty response)");
    json_free(j);
}

/* POST body; true with a 200 response (its body in resp, or passed to sink). */
static bool request(struct model *m, const char *body, size_t bodylen, http_sink sink, void *ctx,
                    struct sbuf *resp, char *err, size_t errlen)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        struct sbuf hdr;
        sb_init(&hdr);
        sb_free(resp);
        sb_init(resp);
        auth_header(m, &hdr);
        int status = http_request_stream("POST", &m->url, hdr.s, body, bodylen, sink, ctx, resp, err, errlen,
                                         TIMEOUT_MS);
        memset(hdr.s, 0, hdr.len);
        sb_free(&hdr);
        if (status < 0)
            return false;
        if (status == 401 && attempt == 0) {             /* try the other way of passing the key */
            m->bearer = !m->bearer;
            continue;
        }
        if (status == 429 && attempt < 2) {             /* rate limited: brief back-off */
            sleep(2 + attempt * 3);
            continue;
        }
        if (status != 200) {
            if (status == 401)
                m->bearer = !m->bearer;
            api_error(status, resp, err, errlen);
            if (status == 404)
                snprintf(err + strlen(err), errlen - strlen(err), " (check the endpoint and model/deployment name)");
            return false;
        }
        return true;
    }
    return false;
}

static void request_body(struct model *m, const char *messages, const char *tools, bool stream, struct sbuf *body)
{
    sb_puts(body, "{");
    if (m->send_model) {
        sb_puts(body, "\"model\":");
        sb_json_str(body, m->cfg.model);
        sb_puts(body, ",");
    }
    if (stream)
        sb_puts(body, "\"stream\":true,");
    sb_puts(body, "\"messages\":");
    sb_puts(body, messages);
    if (tools) {
        sb_puts(body, ",\"tools\":");
        sb_puts(body, tools);
        sb_puts(body, ",\"tool_choice\":\"auto\"");
    }
    sb_puts(body, "}");
}

/* choices[0].message of a complete (non-streamed) reply, detached; NULL with err set. */
static struct json *reply_message(const struct sbuf *resp, char *err, size_t errlen)
{
    struct json *j = json_parse(resp->s, resp->len);
    if (!j) {
        snprintf(err, errlen, "the model returned invalid JSON");
        return NULL;
    }
    struct json *choice = json_at(json_get(j, "choices"), 0);
    struct json *msg = json_get(choice, "message");
    if (!msg) {
        snprintf(err, errlen, "unexpected response (no choices[0].message)");
        json_free(j);
        return NULL;
    }
    const char *finish = json_get_str(choice, "finish_reason");
    for (int i = 0; i < choice->n; i++)
        if (choice->items[i] == msg)
            choice->items[i] = NULL;
    if (finish && !strcmp(finish, "content_filter") && !json_get_str(msg, "content")) {
        json_free(msg);
        snprintf(err, errlen, "the response was blocked by the content filter");
        json_free(j);
        return NULL;
    }
    json_free(j);
    return msg;
}

struct json *model_chat(struct model *m, const char *messages, const char *tools, char *err, size_t errlen)
{
    struct sbuf body, resp;
    sb_init(&body);
    sb_init(&resp);
    request_body(m, messages, tools, false, &body);
    bool ok = request(m, body.s, body.len, NULL, NULL, &resp, err, errlen);
    sb_free(&body);
    struct json *msg = ok ? reply_message(&resp, err, errlen) : NULL;
    sb_free(&resp);
    return msg;
}

/* ---------------- streaming (server-sent events) ---------------- */

#define MAX_CALLS 16
#define RAW_MAX   (4 << 20)

struct stream {
    model_delta delta;
    void *ctx;
    struct sbuf line, data;          /* the SSE line being read; the event's data */
    struct sbuf content, held;       /* the reply so far; text not yet passed to delta */
    struct { struct sbuf id, name, args; } calls[MAX_CALLS];
    int ncalls;
    char finish[32];
    bool events, done, stopped;
    struct sbuf raw;                 /* the body while no event has been seen (a non-streamed reply) */
    char err[300];
};

/* Pass on what can be shown: not an incomplete UTF-8 character or a trailing '*', '_' or '`'. */
static void flush(struct stream *st, bool all)
{
    size_t n = st->held.len;
    if (!all) {
        size_t k = n, back = 0;
        while (k && back < 4 && ((unsigned char)st->held.s[k - 1] & 0xC0) == 0x80)
            k--, back++;
        if (k && (unsigned char)st->held.s[k - 1] >= 0xC0) {
            unsigned char c = (unsigned char)st->held.s[k - 1];
            size_t need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            n = back < need ? k - 1 : n;
        }
        while (n && (st->held.s[n - 1] == '*' || st->held.s[n - 1] == '_' || st->held.s[n - 1] == '`') &&
               st->held.len - n < 3)
            n--;
    }
    if (!n)
        return;
    char save = st->held.s[n];
    st->held.s[n] = 0;
    if (!st->stopped && st->delta && !st->delta(st->ctx, st->held.s))
        st->stopped = true;
    st->held.s[n] = save;
    memmove(st->held.s, st->held.s + n, st->held.len - n + 1);
    st->held.len -= n;
}

static void event(struct stream *st)
{
    if (!strcmp(st->data.s, "[DONE]")) {
        st->done = true;
        return;
    }
    struct json *j = json_parse(st->data.s, st->data.len);
    if (!j)
        return;
    const struct json *e = json_get(j, "error");
    if (e && !st->err[0]) {
        const char *msg = json_get_str(e, "message");
        snprintf(st->err, sizeof(st->err), "%.280s", msg ? msg : "the model reported an error");
    }
    const struct json *choice = json_at(json_get(j, "choices"), 0);   /* (Azure sends some with none) */
    const struct json *d = json_get(choice, "delta");
    const char *text = json_get_str(d, "content");
    if (text && *text) {
        sb_puts(&st->content, text);
        sb_puts(&st->held, text);
        flush(st, false);
    }
    const struct json *calls = json_get(d, "tool_calls");
    for (int i = 0; calls && calls->type == JSON_ARRAY && i < calls->n; i++) {
        const struct json *c = calls->items[i], *idx = json_get(c, "index"), *fn = json_get(c, "function");
        int k = idx && idx->type == JSON_NUMBER ? (int)idx->num : i;
        if (k < 0 || k >= MAX_CALLS)
            continue;
        while (st->ncalls <= k) {
            sb_init(&st->calls[st->ncalls].id);
            sb_init(&st->calls[st->ncalls].name);
            sb_init(&st->calls[st->ncalls].args);
            st->ncalls++;
        }
        const char *v;
        if ((v = json_get_str(c, "id")))
            sb_puts(&st->calls[k].id, v);
        if ((v = json_get_str(fn, "name")))
            sb_puts(&st->calls[k].name, v);
        if ((v = json_get_str(fn, "arguments")))
            sb_puts(&st->calls[k].args, v);
    }
    const char *fin = json_get_str(choice, "finish_reason");
    if (fin)
        snprintf(st->finish, sizeof(st->finish), "%s", fin);
    json_free(j);
}

static bool stream_sink(void *ctx, const char *p, size_t n)
{
    struct stream *st = ctx;
    if (!st->events && st->raw.len + n <= RAW_MAX)
        sb_putn(&st->raw, p, n);
    for (size_t i = 0; i < n && !st->done; i++) {
        if (p[i] != '\n') {
            sb_putc(&st->line, p[i]);
            continue;
        }
        char *l = st->line.s ? st->line.s : "";
        size_t len = st->line.len;
        if (len && l[len - 1] == '\r')
            l[--len] = 0;
        if (!len) {                                    /* end of an event */
            if (st->data.len) {
                st->events = true;
                event(st);
            }
            st->data.len = 0;
            if (st->data.s)
                st->data.s[0] = 0;
        } else if (!strncmp(l, "data:", 5)) {
            const char *v = l + 5 + (l[5] == ' ');
            if (st->data.len)
                sb_putc(&st->data, '\n');
            sb_puts(&st->data, v);
        }                                              /* comments (':') and other fields: ignored */
        st->line.len = 0;
        if (st->line.s)
            st->line.s[0] = 0;
    }
    return !st->stopped;
}

static void stream_free(struct stream *st)
{
    sb_free(&st->line);
    sb_free(&st->data);
    sb_free(&st->content);
    sb_free(&st->held);
    sb_free(&st->raw);
    for (int i = 0; i < st->ncalls; i++) {
        sb_free(&st->calls[i].id);
        sb_free(&st->calls[i].name);
        sb_free(&st->calls[i].args);
    }
}

struct json *model_chat_stream(struct model *m, const char *messages, const char *tools, model_delta delta,
                               void *ctx, char *err, size_t errlen)
{
    static struct stream st;
    memset(&st, 0, sizeof(st));
    st.delta = delta;
    st.ctx = ctx;
    struct sbuf body, resp;
    sb_init(&body);
    sb_init(&resp);
    request_body(m, messages, tools, true, &body);
    bool ok = request(m, body.s, body.len, stream_sink, &st, &resp, err, errlen);
    sb_free(&body);
    sb_free(&resp);
    struct json *msg = NULL;
    if (ok && !st.events) {                            /* "stream" ignored: one ordinary reply */
        msg = reply_message(&st.raw, err, errlen);
        const char *text = json_get_str(msg, "content");
        if (msg && text && *text && delta)
            delta(ctx, text);
    } else if (ok) {
        stream_sink(&st, "\n\n", 2);                   /* an event not terminated by a blank line */
        flush(&st, true);
        if (st.err[0]) {
            snprintf(err, errlen, "%s", st.err);
        } else if (!strcmp(st.finish, "content_filter") && !st.content.len) {
            snprintf(err, errlen, "the response was blocked by the content filter");
        } else {
            struct sbuf j;
            sb_init(&j);
            sb_puts(&j, "{\"role\":\"assistant\",\"content\":");
            if (st.content.len)
                sb_json_strn(&j, st.content.s, st.content.len);
            else
                sb_puts(&j, "null");
            if (st.ncalls) {
                sb_puts(&j, ",\"tool_calls\":[");
                for (int i = 0; i < st.ncalls; i++) {
                    sb_puts(&j, i ? ",{\"id\":" : "{\"id\":");
                    sb_json_strn(&j, st.calls[i].id.s ? st.calls[i].id.s : "", st.calls[i].id.len);
                    sb_puts(&j, ",\"type\":\"function\",\"function\":{\"name\":");
                    sb_json_strn(&j, st.calls[i].name.s ? st.calls[i].name.s : "", st.calls[i].name.len);
                    sb_puts(&j, ",\"arguments\":");
                    sb_json_strn(&j, st.calls[i].args.s ? st.calls[i].args.s : "", st.calls[i].args.len);
                    sb_puts(&j, "}}");
                }
                sb_puts(&j, "]");
            }
            sb_puts(&j, "}");
            msg = json_parse(j.s, j.len);
            sb_free(&j);
            if (!msg)
                snprintf(err, errlen, "could not assemble the streamed reply");
        }
    }
    stream_free(&st);
    return msg;
}

bool model_ping(struct model *m, char *err, size_t errlen)
{
    struct json *r = model_chat(m, "[{\"role\":\"user\",\"content\":\"Reply with the single word: ready\"}]", NULL,
                                err, errlen);
    if (!r)
        return false;
    json_free(r);
    return true;
}
