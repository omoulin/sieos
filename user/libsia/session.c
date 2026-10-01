/*
 * session.c - The conversation engine: the model and the tools take turns
 * until the model answers.  Presentation is left to the caller (sia_io).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "internal.h"

#define MAX_STEPS   16                 /* model calls per request */
#define HISTORY_MAX 60000              /* bytes of conversation kept */

volatile bool sia_interrupted;

void sia_interrupt(void)
{
    sia_interrupted = true;
}

/* ---------------- lifecycle ---------------- */

struct sia_session *sia_session_new(const struct sia_config *cfg, enum sia_role role, const struct sia_io *io,
                                    char *err, size_t errlen)
{
    struct sia_session *s = calloc(1, sizeof(*s));
    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    struct model_cfg mc;
    memset(&mc, 0, sizeof(mc));
    snprintf(mc.endpoint, sizeof(mc.endpoint), "%s", cfg->endpoint);
    snprintf(mc.model, sizeof(mc.model), "%s", cfg->model);
    snprintf(mc.api_key, sizeof(mc.api_key), "%s", cfg->api_key);
    if (!model_init(&s->mdl, &mc, err, errlen)) {
        memset(&mc, 0, sizeof(mc));
        free(s);
        return NULL;
    }
    memset(&mc, 0, sizeof(mc));
    s->role = role;
    s->io = *io;
    s->auto_approve = cfg->auto_approve;
    s->vision = cfg->vision;
    s->max_steps = MAX_STEPS;
    s->history_max = HISTORY_MAX;
    s->img_hist = -1;
    sb_init(&s->img);
    sb_init(&s->tooljson);
    s->tools_dirty = true;
    return s;
}

void sia_session_free(struct sia_session *s)
{
    if (!s)
        return;
    sia_clear(s);
    free(s->hist);
    sb_free(&s->tooljson);
    sb_free(&s->img);
    memset(s, 0, sizeof(*s));
    free(s);
}

bool sia_ping(struct sia_session *s, char *err, size_t errlen)
{
    return model_ping(&s->mdl, err, errlen);
}

void sia_set_auto_approve(struct sia_session *s, bool on) { s->auto_approve = on; }

void sia_set_instructions(struct sia_session *s, void (*fn)(struct sia_session *s, struct sbuf *out, void *ctx),
                          void *ctx)
{
    s->instructions = fn;
    s->instructions_ctx = ctx;
}

void sia_set_limits(struct sia_session *s, int max_steps, size_t history_bytes, int silence_ms)
{
    if (max_steps > 0)
        s->max_steps = max_steps;
    if (history_bytes > 0)
        s->history_max = history_bytes;
    if (silence_ms > 0)
        s->mdl.timeout_ms = silence_ms;
}

bool sia_was_interrupted(void) { return sia_interrupted; }
bool sia_auto_approve(const struct sia_session *s) { return s->auto_approve; }
const char *sia_model_name(const struct sia_session *s) { return s->mdl.cfg.model; }
const char *sia_endpoint_kind(const struct sia_session *s) { return s->mdl.kind; }

void sia_request_url(const struct sia_session *s, char *buf, size_t n)
{
    snprintf(buf, n, "%s://%s%s", s->mdl.url.https ? "https" : "http", s->mdl.url.host, s->mdl.url.path);
}

/* ---------------- tools registry ---------------- */

bool sia_add_tool(struct sia_session *s, const struct sia_tool *t)
{
    if (s->ntools >= SIA_MAX_TOOLS || sia_find_tool(s, t->name))
        return false;
    s->tools[s->ntools++] = *t;
    s->tools_dirty = true;
    return true;
}

int sia_tool_count(const struct sia_session *s) { return s->ntools; }
const struct sia_tool *sia_tool_at(const struct sia_session *s, int i) { return i >= 0 && i < s->ntools ? &s->tools[i] : NULL; }

const struct sia_tool *sia_find_tool(const struct sia_session *s, const char *name)
{
    for (int i = 0; i < s->ntools; i++)
        if (!strcmp(s->tools[i].name, name))
            return &s->tools[i];
    return NULL;
}

static const char *tools_json(struct sia_session *s)
{
    if (!s->tools_dirty)
        return s->ntools ? s->tooljson.s : NULL;
    sb_free(&s->tooljson);
    sb_init(&s->tooljson);
    struct sbuf *out = &s->tooljson;
    sb_putc(out, '[');
    for (int i = 0; i < s->ntools; i++) {
        const struct sia_tool *t = &s->tools[i];
        if (i)
            sb_putc(out, ',');
        sb_puts(out, "{\"type\":\"function\",\"function\":{\"name\":");
        sb_json_str(out, t->name);
        sb_puts(out, ",\"description\":");
        if (t->description) {
            sb_json_str(out, t->description);
        } else {
            char d[96];
            snprintf(d, sizeof(d), "Run the %s command.", t->name);
            sb_json_str(out, d);
        }
        sb_puts(out, ",\"parameters\":");
        sb_puts(out, t->parameters ? t->parameters : "{\"type\":\"object\",\"properties\":{}}");
        sb_puts(out, "}}");
    }
    sb_putc(out, ']');
    s->tools_dirty = false;
    return s->ntools ? s->tooljson.s : NULL;
}

/* ---------------- output helpers ---------------- */

void sia__output(struct sia_session *s, const char *buf, size_t n)
{
    if (s->io.output && n)
        s->io.output(s->io.ctx, buf, n);
}

void sia_output(struct sia_session *s, const char *buf, size_t n) { sia__output(s, buf, n); }

void sia_emit(struct sia_session *s, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sia__output(s, tmp, n < (int)sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

static void report_error(struct sia_session *s, const char *msg)
{
    if (s->io.error)
        s->io.error(s->io.ctx, msg);
}

/* ---------------- history ---------------- */

static void hist_add(struct sia_session *s, const char *json)
{
    if (s->nhist == s->caphist) {
        s->caphist = s->caphist ? s->caphist * 2 : 32;
        s->hist = realloc(s->hist, s->caphist * sizeof(*s->hist));
    }
    sb_init(&s->hist[s->nhist]);
    sb_puts(&s->hist[s->nhist], json);
    s->nhist++;
}

void sia_clear(struct sia_session *s)
{
    for (int i = 0; i < s->nhist; i++)
        sb_free(&s->hist[i]);
    s->nhist = 0;
    s->img_hist = -1;
}

/* Drop the oldest exchanges (whole user turns) while the history is too big. */
static void hist_trim(struct sia_session *s)
{
    for (;;) {
        size_t total = 0;
        for (int i = 0; i < s->nhist; i++)
            if (i != s->img_hist)                       /* (an image is shown once, then dropped) */
                total += s->hist[i].len;
        if (total <= s->history_max || s->nhist < 2)
            return;
        int cut = 1;
        while (cut < s->nhist && strncmp(s->hist[cut].s, "{\"role\":\"user\"", 14))
            cut++;
        if (cut >= s->nhist)
            return;
        for (int i = 0; i < cut; i++)
            sb_free(&s->hist[i]);
        memmove(s->hist, s->hist + cut, (s->nhist - cut) * sizeof(*s->hist));
        s->nhist -= cut;
        s->img_hist = s->img_hist >= cut ? s->img_hist - cut : -1;
    }
}

static void system_prompt(struct sia_session *s, struct sbuf *b)
{
    char cwd[256] = "/", host[64] = "sieos";
    getcwd(cwd, sizeof(cwd));
    struct utsname u;
    if (uname(&u) == 0)
        snprintf(host, sizeof(host), "%s", u.nodename);
    struct passwd *pw = getpwuid(getuid());
    struct tm tm;
    time_t now_t = time(NULL);
    gmtime_r(&now_t, &tm);
    struct sbuf p;
    sb_init(&p);
    if (s->role == SIA_ROLE_APP) {                     /* a program's own instructions, then where it is */
        if (s->instructions)
            s->instructions(s, &p, s->instructions_ctx);
        sb_printf(&p, "The user is '%s' on host '%s'; the date is %04d-%02d-%02d %02d:%02d UTC.\n",
                  pw ? pw->pw_name : "user", host, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min);
    } else {
        sb_printf(&p,
                  "You are sia, the assistant built into SIEOS (Synthetic Intelligence Enhanced Operating System), "
                  "a small Unix-like operating system (Solaris/BSD flavour, x86_64). The user is '%s' on host '%s'; "
                  "the current directory is %s; the date is %04d-%02d-%02d %02d:%02d UTC.\n",
                  pw ? pw->pw_name : "user", host, cwd, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min);
    }
    if (s->role == SIA_ROLE_TERMINAL)
        sb_puts(&p,
                "You run inside a terminal. The user types commands directly; you receive either a request "
                "they addressed to you, or a command of theirs that failed, with its error output.\n"
                "- For a request, work it out step by step with the tools, then give a short answer. If the "
                "request is itself a command line, run exactly that command.\n"
                "- For a failed command, run the corrected command only when there is one clear fix; otherwise "
                "explain the problem briefly and list the possible alternatives.\n"
                "- Command output is shown to the user as it runs, so do not repeat it; summarise or answer the "
                "question instead.\n"
                "- Reply in plain ASCII text without Markdown: the terminal is 80 columns wide.\n");
    else if (s->role == SIA_ROLE_DESKTOP)
        sb_puts(&p,
                "You run in the command strip of the Facet desktop. Your replies appear in a small panel above "
                "the strip, so keep them to one or two short sentences of plain ASCII text without Markdown.\n"
                "- Use the desktop tools to open applications and terminals, and to list, focus, close or move "
                "windows and switch workspaces. Terminals run the sia assistant unless a plain shell is asked "
                "for; to run an interactive or long command for the user to watch, open a terminal with that "
                "command.\n"
                "- Use the command tools to look things up or change files; their output is shown briefly in "
                "the panel.\n"
                "- Do not ask for confirmation for harmless actions; just do them and say what you did.\n");
    if (s->role != SIA_ROLE_APP) {
        sb_puts(&p,
                "- Only the commands available as tools exist on this system (there is no editor or Python). Use "
                "write_file to create files and cd to change directory.");
        if (sia_find_tool(s, "open_app") && access(SIA_MIR_PROGRAM, X_OK) == 0)   /* (MiR: a package) */
            sb_puts(&p, " When the user wants an application made, open MiR (open_app with app mir and their "
                        "request as path): it writes, builds and tests applications.");
    }
    sb_puts(b, "{\"role\":\"system\",\"content\":");
    sb_json_str(b, p.s);
    sb_puts(b, "}");
    sb_free(&p);
}

static void messages_json(struct sia_session *s, struct sbuf *b)
{
    sb_putc(b, '[');
    system_prompt(s, b);
    for (int i = 0; i < s->nhist; i++) {
        sb_putc(b, ',');
        sb_putn(b, s->hist[i].s, s->hist[i].len);
    }
    sb_putc(b, ']');
}

/* ---------------- one request ---------------- */

/* Compare a tool call with what the user typed, ignoring quotes and spacing. */
static void normalize(const char *in, char *out, size_t n)
{
    size_t k = 0;
    bool space = true;
    for (; *in && k + 1 < n; in++) {
        if (*in == '\'' || *in == '"')
            continue;
        if (*in == ' ' || *in == '\t') {
            if (!space)
                out[k++] = ' ';
            space = true;
            continue;
        }
        out[k++] = *in;
        space = false;
    }
    while (k && out[k - 1] == ' ')
        k--;
    out[k] = 0;
}

static bool same_command(const char *a, const char *b)
{
    static char x[2048], y[2048];
    normalize(a, x, sizeof(x));
    normalize(b, y, sizeof(y));
    return x[0] && !strcmp(x, y);
}

static void default_describe(const struct sia_tool *t, const struct json *args, struct sbuf *out)
{
    sb_puts(out, t->name);
    if (args && args->type == JSON_OBJECT && args->n) {
        sb_putc(out, ' ');
        json_write(out, args);
    }
}

static bool needs_confirm(struct sia_session *s, const struct sia_tool *t, const struct json *args)
{
    if (t->needs_confirm)
        return t->needs_confirm(s, t, args);
    return !t->readonly;
}

static void thinking(struct sia_session *s, bool on)
{
    if (s->io.thinking)
        s->io.thinking(s->io.ctx, on);
    sia_status_write(on ? "busy" : "idle", s->last_action);
}

static void run_call(struct sia_session *s, const struct json *call, const char *request)
{
    const char *id = json_get_str(call, "id");
    const struct json *fn = json_get(call, "function");
    const char *name = json_get_str(fn, "name");
    const char *argstr = json_get_str(fn, "arguments");
    struct json *args = argstr && *argstr ? json_parse(argstr, strlen(argstr)) : NULL;
    const struct sia_tool *t = name ? sia_find_tool(s, name) : NULL;
    struct sbuf result, desc;
    sb_init(&result);
    sb_init(&desc);
    if (!t) {
        sb_printf(&result, "error: there is no tool named '%s'", name ? name : "?");
    } else if (argstr && *argstr && !args) {
        sb_puts(&result, "error: the arguments are not valid JSON");
    } else {
        (t->describe ? t->describe : default_describe)(t, args, &desc);
        if (s->io.tool)
            s->io.tool(s->io.ctx, desc.s);
        bool typed = same_command(desc.s, request);
        int answer = 1;
        if (sia_interrupted) {
            answer = -1;
        } else if (needs_confirm(s, t, args) && !typed && !s->auto_approve) {
            answer = s->io.confirm ? s->io.confirm(s->io.ctx, desc.s) : 0;
            if (answer == 2)
                s->auto_approve = true;
        }
        if (answer < 0) {
            sb_puts(&result, "not run: the user interrupted the request");
        } else if (answer == 0) {
            sb_puts(&result, "not run: the user declined");
        } else {
            sia_status_write("busy", s->last_action);
            t->run(s, t, args, &result);
            int code = 0;
            if (!strncmp(result.s, "exit status: ", 13))
                code = atoi(result.s + 13);
            char shortd[60];
            snprintf(shortd, sizeof(shortd), "%s", desc.s);
            if (strlen(desc.s) >= sizeof(shortd))
                memcpy(shortd + sizeof(shortd) - 4, "...", 4);
            if (code)
                snprintf(s->last_action, sizeof(s->last_action), "%s (exit %d)", shortd, code);
            else
                snprintf(s->last_action, sizeof(s->last_action), "%s (ok)", shortd);
            sia_status_write("idle", s->last_action);
        }
    }
    json_free(args);
    struct sbuf m;
    sb_init(&m);
    sb_puts(&m, "{\"role\":\"tool\",\"tool_call_id\":");
    sb_json_str(&m, id ? id : "");
    sb_puts(&m, ",\"content\":");
    sb_json_strn(&m, result.s, result.len);
    sb_puts(&m, "}");
    hist_add(s, m.s);
    sb_free(&m);
    sb_free(&result);
    sb_free(&desc);
}

struct streaming {
    struct sia_session *s;
    bool any;
};

static bool on_delta(void *ctx, const char *text)
{
    struct streaming *st = ctx;
    if (!st->any) {
        thinking(st->s, false);
        st->any = true;
    }
    st->s->io.delta(st->s->io.ctx, text);
    return !sia_interrupted;
}

bool sia_ask(struct sia_session *s, const char *request)
{
    struct sbuf m;
    sb_init(&m);
    sb_puts(&m, "{\"role\":\"user\",\"content\":");
    sb_json_str(&m, request);
    sb_puts(&m, "}");
    hist_add(s, m.s);
    sb_free(&m);
    sia_interrupted = false;
    const char *tools = tools_json(s);

    for (int step = 0; step < s->max_steps; step++) {
        hist_trim(s);
        struct sbuf msgs;
        sb_init(&msgs);
        messages_json(s, &msgs);
        thinking(s, true);
        char err[512];
        struct streaming st = { s, false };
        struct json *reply = s->io.delta ? model_chat_stream(&s->mdl, msgs.s, tools, on_delta, &st, err, sizeof(err))
                                         : model_chat(&s->mdl, msgs.s, tools, err, sizeof(err));
        sb_free(&msgs);
        if (s->img_hist >= 0 && s->img_hist < s->nhist) {   /* the image was seen: keep only its caption */
            struct sbuf t;
            sb_init(&t);
            sb_puts(&t, "{\"role\":\"user\",\"content\":");
            char cap[300];
            snprintf(cap, sizeof(cap), "%s [the image was shown once and is no longer attached]", s->img_caption);
            sb_json_str(&t, cap);
            sb_puts(&t, "}");
            sb_free(&s->hist[s->img_hist]);
            s->hist[s->img_hist] = t;
            s->img_hist = -1;
        }
        if (st.any)
            s->io.delta(s->io.ctx, NULL);                /* the end of this reply */
        else
            thinking(s, false);
        if (!reply) {
            report_error(s, sia_interrupted ? "interrupted" : err);
            /* keep the history consistent: forget the unanswered request */
            if (s->nhist && !strncmp(s->hist[s->nhist - 1].s, "{\"role\":\"user\"", 14))
                sb_free(&s->hist[--s->nhist]);
            return false;
        }
        const char *content = json_get_str(reply, "content");
        struct json *calls = json_get(reply, "tool_calls");
        bool has_calls = calls && calls->type == JSON_ARRAY && calls->n;
        struct sbuf a;
        sb_init(&a);
        sb_puts(&a, "{\"role\":\"assistant\",\"content\":");
        if (content)
            sb_json_str(&a, content);
        else
            sb_puts(&a, "null");
        if (has_calls) {
            sb_puts(&a, ",\"tool_calls\":");
            json_write(&a, calls);
        }
        sb_puts(&a, "}");
        hist_add(s, a.s);
        sb_free(&a);
        if (content && *content && s->io.text && !s->io.delta)
            s->io.text(s->io.ctx, content);
        if (!has_calls) {
            json_free(reply);
            return true;
        }
        for (int i = 0; i < calls->n; i++)
            run_call(s, calls->items[i], request);
        json_free(reply);
        if (s->img.len) {                                /* a tool attached an image: the model sees it next */
            hist_add(s, s->img.s);
            s->img_hist = s->nhist - 1;
            sb_free(&s->img);
            sb_init(&s->img);
        }
        if (sia_interrupted) {
            report_error(s, "interrupted");
            return false;
        }
    }
    report_error(s, "stopping after too many steps");
    return false;
}
