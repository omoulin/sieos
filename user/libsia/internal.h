/*
 * internal.h - Shared between the parts of libsia (not a public interface).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef LIBSIA_INTERNAL_H
#define LIBSIA_INTERNAL_H

#include "libsia.h"
#include "model.h"

#define SIA_MAX_TOOLS 128

struct sia_session {
    struct model mdl;
    enum sia_role role;
    struct sia_io io;
    bool auto_approve;
    struct sia_tool tools[SIA_MAX_TOOLS];
    int ntools;
    struct sbuf tooljson;
    bool tools_dirty;
    struct sbuf *hist;                 /* JSON message objects after the system prompt */
    int nhist, caphist;
    char last_action[96];
    int vision;                        /* the model sees images: 1, 0, -1 not tested */
    struct sbuf img;                   /* an image for the model's next turn (a user message), or empty */
    char img_caption[200];
    int img_hist;                      /* the history entry holding the image shown, -1 none */
    void (*instructions)(struct sia_session *s, struct sbuf *out, void *ctx);   /* SIA_ROLE_APP */
    void *instructions_ctx;
    int max_steps;
    size_t history_max;
};

extern volatile bool sia_interrupted;

void sia__output(struct sia_session *s, const char *buf, size_t n);

#endif
