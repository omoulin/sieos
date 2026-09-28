/*
 * model.h - Chat-completions client for Azure AI Foundry models.
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

/* Cheap request to check the endpoint, model name and key. */
bool model_ping(struct model *m, char *err, size_t errlen);

#endif
