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
 *       (a program with its own tools: SIA_ROLE_APP, sia_set_instructions, sia_add_tool)
 *       sia_ask(s, "which process uses the most memory?");
 *   }
 *
 * The caller decides how things are shown through struct sia_io callbacks.
 *
 * Desktop channel: Facet gives every process it starts a pipe pair and sets
 * SIEOS_DESKTOP="RFD,WFD".  Requests and replies are single JSON lines:
 *   -> {"op":"open","app":"terminal|shell|files|monitor|network|browser|sipm|mir|clock|settings|about",
 *       "path":"...", "command":"..."}
 *   -> {"op":"windows"}   {"op":"focus","id":N}   {"op":"close","id":N}
 *   -> {"op":"workspace","n":1..4}   {"op":"move","id":N,"workspace":1..4}
 *   MiR's tests of an application it started (process pid):
 *   -> {"op":"windows","pid":N}           its windows: id, title, content size
 *   -> {"op":"snapshot","pid":N,"path":"..."}   a PNG of its window's content
 *   -> {"op":"input","pid":N,"text":"..."|"key":"enter"|"click":[x,y],"button":"left"}
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
    char profile[64];                 /* the name of the model profile in use ("" none) */
    int  vision;                      /* the model sees images: 1 yes, 0 no, -1 not tested */
};

/* ~/.sia/config: the model in use.  Returns 1 if a model is registered, 0 if
 * the file exists without one (the user skipped setup), -1 if there is no
 * file. */
int  sia_config_load(struct sia_config *c);
bool sia_config_save(const struct sia_config *c, char *err, size_t errlen);
const char *sia_config_path(void);

/* ~/.sia/models: the models the user has recorded, one of them in use
 * (copied into ~/.sia/config, which every program reads).  The file holds
 * "[name]" sections of endpoint=, model=, api_key= and vision= lines. */
#define SIA_MAX_PROFILES 16
struct sia_profile {
    char name[64];
    char endpoint[512];
    char model[128];
    char api_key[512];
    int  vision;                      /* 1 yes, 0 no, -1 not tested */
};
/* The profiles (a model in ~/.sia/config that none of them is becomes one);
 * active gets the name of the one in use ("" none).  Returns the count. */
int  sia_profiles_load(struct sia_profile *p, int max, char *active, size_t alen);
/* Write them, and the active one (by name; none: "") to ~/.sia/config. */
bool sia_profiles_save(const struct sia_profile *p, int n, const char *active, char *err, size_t errlen);
/* Record what the vision test found for the profile named name. */
void sia_profile_set_vision(const char *name, int vision);

/* Does the model see images?  Shows it a picture of a number and checks
 * the answer: 1 yes, 0 no (err says why), -1 the test could not be made. */
int  sia_vision_test(const struct sia_config *c, char *err, size_t errlen);

/* A PNG of 0x00RRGGBB pixels (stride in pixels), appended to out. */
void sia_png(const uint32_t *px, int w, int h, int stride, struct sbuf *out);
void sb_base64(struct sbuf *out, const unsigned char *data, size_t n);

/* ~/.sia/status, read by the desktop: state is "idle" or "busy". */
void sia_status_write(const char *state, const char *last_action);
bool sia_status_read(char *state, size_t slen, char *last, size_t llen);

/* ---------------- sessions ---------------- */

enum sia_role {
    SIA_ROLE_TERMINAL,                /* answers appear in a terminal with the commands' output */
    SIA_ROLE_DESKTOP,                 /* the Facet strip: short answers in a small panel */
    SIA_ROLE_APP,                     /* a program's own assistant: its instructions (sia_set_instructions)
                                         and tools (MiR's, for one) */
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
    /* Optional: news for the program showing the session (a tool's), e.g.
     * "project" with the application's name. */
    void (*event)(void *ctx, const char *name, const char *value);
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
bool sia_session_vision(const struct sia_session *s);               /* the model sees images */
/* Show the model a PNG image with its next turn (from a tool); false if it does not see images. */
bool sia_attach_image(struct sia_session *s, const char *caption, const void *png, size_t len);
/* Tell the program showing the session something (sia_io.event). */
void sia_notify(struct sia_session *s, const char *name, const char *value);

/* SIA_ROLE_APP: the program's instructions to the model, written into out
 * when a request is made (the user, host and date are added after them). */
void sia_set_instructions(struct sia_session *s, void (*fn)(struct sia_session *s, struct sbuf *out, void *ctx),
                          void *ctx);
/* Model calls per request, bytes of conversation kept, and how long the
 * model may stay silent (ms); 0 keeps the default (16, 60000, 300000). */
void sia_set_limits(struct sia_session *s, int max_steps, size_t history_bytes, int silence_ms);

/* sia-brain, the local model (the package sia-brain): when it is installed,
 * a profile of its own, "sia-brain (local)", and the model sia uses when no
 * other is registered.  Its server (llama-server, run by the sia-brain
 * command) is started when a request finds it not running. */
#define SIA_BRAIN_MODEL   "/usr/pkg/share/sia-brain/sia-brain.gguf"
#define SIA_BRAIN_COMMAND "/usr/pkg/bin/sia-brain"
#define SIA_BRAIN_NAME    "sia-brain (local)"
#define SIA_BRAIN_PORT    8095
#define SIA_BRAIN_URL     "http://127.0.0.1:8095/v1/chat/completions"
bool sia_brain_installed(void);

/* MiR (Make it Real, the package mir): sia opens it when the user wants an application made */
#define SIA_MIR_PROGRAM "/usr/pkg/bin/facet-mir"

/* ---------------- an agent: libsia without a terminal ---------------- */

/* The loop of sia-agent: requests and events in JSON lines on stdin/stdout
 * (user/sia/sia-agent.c lists them).  setup adds the session's tools (and
 * instructions, limits) each time it connects to the model; vision_test:
 * test a model whose sight is not known yet (and record it). */
struct sia_agent {
    enum sia_role role;
    void (*setup)(struct sia_session *s, void *ctx);
    void *ctx;
    bool vision_test;
};
int sia_agent_main(const struct sia_agent *a);
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

/* Helpers for tools: show output to the user through the session's io;
 * whether the user interrupted the request (stop what the tool does). */
void sia_emit(struct sia_session *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sia_output(struct sia_session *s, const char *buf, size_t n);
bool sia_was_interrupted(void);

/* ---------------- desktop channel (client side) ---------------- */

bool sia_desktop_connected(void);
/* Send one request object; on success result holds the reply's "result". */
bool sia_desktop_call(const char *request_json, struct sbuf *result, char *err, size_t errlen);

/* ---------------- text ---------------- */

/* The model's UTF-8 text for an 8-bit terminal: ASCII punctuation, no Markdown emphasis. */
void sia_plain_text(const char *utf8, struct sbuf *out);

#endif
