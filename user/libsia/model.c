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

static struct json *request(struct model *m, const char *body, size_t bodylen, char *err, size_t errlen)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        struct sbuf hdr, resp;
        sb_init(&hdr);
        sb_init(&resp);
        auth_header(m, &hdr);
        int status = http_request("POST", &m->url, hdr.s, body, bodylen, &resp, err, errlen, TIMEOUT_MS);
        memset(hdr.s, 0, hdr.len);
        sb_free(&hdr);
        if (status < 0) {
            sb_free(&resp);
            return NULL;
        }
        if (status == 401 && attempt == 0) {             /* try the other way of passing the key */
            m->bearer = !m->bearer;
            sb_free(&resp);
            continue;
        }
        if (status == 429 && attempt < 2) {             /* rate limited: brief back-off */
            sb_free(&resp);
            sleep(2 + attempt * 3);
            continue;
        }
        if (status != 200) {
            if (status == 401)
                m->bearer = !m->bearer;
            api_error(status, &resp, err, errlen);
            if (status == 404)
                snprintf(err + strlen(err), errlen - strlen(err), " (check the endpoint and model/deployment name)");
            sb_free(&resp);
            return NULL;
        }
        struct json *j = json_parse(resp.s, resp.len);
        sb_free(&resp);
        if (!j)
            snprintf(err, errlen, "the model returned invalid JSON");
        return j;
    }
    return NULL;
}

struct json *model_chat(struct model *m, const char *messages, const char *tools, char *err, size_t errlen)
{
    struct sbuf body;
    sb_init(&body);
    sb_puts(&body, "{");
    if (m->send_model) {
        sb_puts(&body, "\"model\":");
        sb_json_str(&body, m->cfg.model);
        sb_puts(&body, ",");
    }
    sb_puts(&body, "\"messages\":");
    sb_puts(&body, messages);
    if (tools) {
        sb_puts(&body, ",\"tools\":");
        sb_puts(&body, tools);
        sb_puts(&body, ",\"tool_choice\":\"auto\"");
    }
    sb_puts(&body, "}");
    struct json *j = request(m, body.s, body.len, err, errlen);
    sb_free(&body);
    if (!j)
        return NULL;
    struct json *choice = json_at(json_get(j, "choices"), 0);
    struct json *msg = json_get(choice, "message");
    if (!msg) {
        snprintf(err, errlen, "unexpected response (no choices[0].message)");
        json_free(j);
        return NULL;
    }
    const char *finish = json_get_str(choice, "finish_reason");
    /* detach the message from the response tree */
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

bool model_ping(struct model *m, char *err, size_t errlen)
{
    struct json *r = model_chat(m, "[{\"role\":\"user\",\"content\":\"Reply with the single word: ready\"}]", NULL,
                                err, errlen);
    if (!r)
        return false;
    json_free(r);
    return true;
}
