/*
 * backend.h - What the sia server asks of a backend: the local model (our
 * engine in llm/, local.c) or a remote one with an OpenAI-compatible API
 * (remote.c). The server owns sessions and the message protocol (SIA_* in
 * mk/proto.h); a backend only turns a conversation into a stream of text.
 *
 * A conversation is the list of turns so far. A backend may keep its own
 * state per session (the local model keeps its attention cache, so a new
 * turn only processes the new text; the remote one resends the history).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum { SIA_SYSTEM, SIA_USER, SIA_ASSISTANT };

typedef struct {
    int role;                    /* SIA_SYSTEM, SIA_USER, SIA_ASSISTANT */
    const char *text;            /* UTF-8, 0-terminated */
} sia_turn_t;

typedef struct sia_backend {
    const char *name;            /* "local" or "remote" */
    /* Read the configuration ("key=value" lines of /etc/sia.conf) and get
     * ready: load the model, or check the endpoint. 0 or -error. */
    int  (*init)(struct sia_backend *b, const char *conf);
    /* Per-session state: created on SIA_OPEN, freed on SIA_CLOSE. */
    void *(*session_new)(struct sia_backend *b);
    void  (*session_free)(struct sia_backend *b, void *s);
    /* Start answering: turns[0..n-1] is the whole conversation, the last one
     * the user's new message. 0 or -error. */
    int  (*begin)(struct sia_backend *b, void *s, const sia_turn_t *turns, int n);
    /* The next piece of the answer into buf (at most cap bytes, never
     * splitting a UTF-8 character): bytes written, 0 when the answer is
     * complete, or -error. Called repeatedly by the server (SIA_NEXT). */
    int  (*next)(struct sia_backend *b, void *s, char *buf, int cap);
    /* Stop the current answer early (SIA_STOP). */
    void (*stop)(struct sia_backend *b, void *s);
    /* For SIA_INFO: model name, memory used, context length, speed. */
    void (*info)(struct sia_backend *b, char *model, int cap, uint64_t *mem, int *ctx,
                 uint32_t *tok_s_x100);
    void *priv;                  /* the backend's own data */
    /* About how many tokens a text takes for this model (to know when a
     * conversation must be compacted). 0: unknown, the server estimates. */
    int  (*tokens)(struct sia_backend *b, const char *text);
} sia_backend_t;

extern sia_backend_t sia_local, sia_remote;

/* The server's log (the kernel log, on the serial port), for backends too. */
void sia_log(const char *fmt, ...);
