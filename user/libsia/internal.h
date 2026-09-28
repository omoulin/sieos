/*
 * internal.h - Shared between the parts of libsia (not a public interface).
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
};

extern volatile bool sia_interrupted;

void sia__output(struct sia_session *s, const char *buf, size_t n);

#endif
