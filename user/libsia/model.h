/*
 * model.h - Chat-completions client for Azure AI Foundry models.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIA_MODEL_H
#define SIA_MODEL_H

#include "http.h"

struct model_cfg {
    char endpoint[512];
    char model[128];
    char api_key[512];
};

struct model {
    struct model_cfg cfg;
    struct url url;
    bool bearer;                 /* Authorization: Bearer instead of api-key */
    bool send_model;             /* put "model" in the request body */
    char kind[48];               /* description of the endpoint type */
};

/* Work out the request URL for the configured endpoint. */
bool model_init(struct model *m, const struct model_cfg *cfg, char *err, size_t errlen);

/*
 * One chat-completions call.  messages/tools are JSON arrays (tools may be
 * NULL).  On success returns the assistant message object (caller frees).
 */
struct json *model_chat(struct model *m, const char *messages, const char *tools, char *err, size_t errlen);

/*
 * The same with "stream": true: the reply's text is passed to delta as it
 * arrives (server-sent events), in pieces that end on UTF-8 character
 * boundaries and never split a Markdown "**" marker; delta returns false
 * to abandon the request.  Returns the assembled assistant message, tool
 * calls included, as model_chat does.  An endpoint that ignores "stream"
 * and answers with one JSON reply is handled too (delta then gets it whole).
 */
typedef bool (*model_delta)(void *ctx, const char *text);
struct json *model_chat_stream(struct model *m, const char *messages, const char *tools, model_delta delta,
                               void *ctx, char *err, size_t errlen);

/* Cheap request to check the endpoint, model name and key. */
bool model_ping(struct model *m, char *err, size_t errlen);

#endif
