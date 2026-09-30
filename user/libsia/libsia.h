/*
 * libsia.h - The SIEOS assistant library.
 *
 * Any program can use a model deployed in Azure AI Foundry through this
 * library: the terminal harness (/bin/sia), the headless agent used by the
 * Facet desktop's sia strip (/bin/sia-agent), or any other component.
 *
 *   struct sia_config cfg;
 *   if (sia_config_load(&cfg) == 1) {
 *       struct sia_session *s = sia_session_new(&cfg, SIA_ROLE_TERMINAL, &io, err, sizeof(err));
 *       sia_add_command_tools(s);          one tool per program in /bin and /sbin, plus sh/cd/write_file
 *       sia_add_desktop_tools(s);          open apps, list/focus/close windows (inside Facet)
 *       sia_ask(s, "which process uses the most memory?");
 *   }
 *
 * The caller decides how things are shown through struct sia_io callbacks.
 *
 * Desktop channel: Facet gives every process it starts a pipe pair and sets
 * SIEOS_DESKTOP="RFD,WFD".  Requests and replies are single JSON lines:
 *   -> {"op":"open","app":"terminal|shell|files|monitor|network|clock|about",
 *       "path":"...", "command":"..."}
 *   -> {"op":"windows"}   {"op":"focus","id":N}   {"op":"close","id":N}
 *   -> {"op":"workspace","n":1..4}   {"op":"move","id":N,"workspace":1..4}
 *   <- {"ok":true,"result":"..."}  or  {"ok":false,"error":"..."}
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef LIBSIA_H
#define LIBSIA_H

#include "json.h"

/* ---------------- configuration and status ---------------- */

struct sia_config {
    char endpoint[512];
    char model[128];
    char api_key[512];
    bool auto_approve;                /* run changing commands without asking */
};

/* ~/.sia/config.  Returns 1 if a model is registered, 0 if the file exists
 * without one (the user skipped setup), -1 if there is no file. */
int  sia_config_load(struct sia_config *c);
bool sia_config_save(const struct sia_config *c, char *err, size_t errlen);
const char *sia_config_path(void);

/* ~/.sia/status, read by the desktop: state is "idle" or "busy". */
void sia_status_write(const char *state, const char *last_action);
bool sia_status_read(char *state, size_t slen, char *last, size_t llen);

/* ---------------- sessions ---------------- */

enum sia_role {
    SIA_ROLE_TERMINAL,                /* answers appear in a terminal with the commands' output */
    SIA_ROLE_DESKTOP,                 /* the Facet strip: short answers in a small panel */
};

struct sia_io {
    void *ctx;
    void (*text)(void *ctx, const char *utf8);                 /* the model's reply */
    void (*tool)(void *ctx, const char *cmdline);              /* a tool is about to run */
    void (*output)(void *ctx, const char *buf, size_t n);      /* a command's output, as it arrives */
    int  (*confirm)(void *ctx, const char *cmdline);           /* 0 no, 1 yes, 2 yes and stop asking */
    void (*thinking)(void *ctx, bool on);                      /* waiting for the model */
    void (*error)(void *ctx, const char *msg);
    /* Optional: the reply as it is written (streamed).  When set, pieces of
     * the reply come here instead of one text() call, and a NULL piece ends
     * each reply. */
    void (*delta)(void *ctx, const char *utf8);
};

struct sia_session;

struct sia_session *sia_session_new(const struct sia_config *cfg, enum sia_role role, const struct sia_io *io,
                                    char *err, size_t errlen);
void sia_session_free(struct sia_session *s);
bool sia_ping(struct sia_session *s, char *err, size_t errlen);      /* check endpoint, model and key */

/* One request: the model and the tools take turns until the model answers.
 * Returns false if it failed (io->error has been called). */
bool sia_ask(struct sia_session *s, const char *request);
void sia_clear(struct sia_session *s);                                 /* forget the conversation */

void sia_set_auto_approve(struct sia_session *s, bool on);
bool sia_auto_approve(const struct sia_session *s);
const char *sia_model_name(const struct sia_session *s);
const char *sia_endpoint_kind(const struct sia_session *s);        /* e.g. "Azure OpenAI deployment" */
void sia_request_url(const struct sia_session *s, char *buf, size_t n);

/* Interrupt the current request (safe from a signal handler). */
void sia_interrupt(void);

/* ---------------- tools ---------------- */

struct sia_tool;
typedef void (*sia_tool_fn)(struct sia_session *s, const struct sia_tool *t, const struct json *args,
                            struct sbuf *result);

struct sia_tool {
    char name[32];
    const char *description;
    const char *parameters;           /* JSON schema of the arguments */
    bool readonly;                    /* safe to run without asking */
    sia_tool_fn run;
    void (*describe)(const struct sia_tool *t, const struct json *args, struct sbuf *out);   /* optional */
    bool (*needs_confirm)(struct sia_session *s, const struct sia_tool *t, const struct json *args); /* optional */
    char path[64];                    /* for program tools */
    void *arg;                        /* for the tool's own use */
};

bool sia_add_tool(struct sia_session *s, const struct sia_tool *t);
void sia_add_command_tools(struct sia_session *s);
bool sia_add_desktop_tools(struct sia_session *s);                  /* false outside Facet */
int  sia_tool_count(const struct sia_session *s);
const struct sia_tool *sia_tool_at(const struct sia_session *s, int i);
const struct sia_tool *sia_find_tool(const struct sia_session *s, const char *name);

/* Helpers for tools: show output to the user through the session's io. */
void sia_emit(struct sia_session *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---------------- desktop channel (client side) ---------------- */

bool sia_desktop_connected(void);
/* Send one request object; on success result holds the reply's "result". */
bool sia_desktop_call(const char *request_json, struct sbuf *result, char *err, size_t errlen);

/* ---------------- text ---------------- */

/* The model's UTF-8 text for an 8-bit terminal: ASCII punctuation, no Markdown emphasis. */
void sia_plain_text(const char *utf8, struct sbuf *out);

#endif
