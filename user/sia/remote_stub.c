/*
 * remote_stub.c - Stands in for remote.c (the OpenAI-compatible backend,
 * which needs the network) until it exists: the Makefile links remote.c
 * when it is there, this file otherwise.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "backend.h"

static int r_init(sia_backend_t *b, const char *conf) { (void)b; (void)conf; return -ENOSYS; }
static void *r_new(sia_backend_t *b) { (void)b; return 0; }
static void r_free(sia_backend_t *b, void *s) { (void)b; (void)s; }
static int r_begin(sia_backend_t *b, void *s, const sia_turn_t *t, int n) { (void)b; (void)s; (void)t; (void)n; return -ENOSYS; }
static int r_next(sia_backend_t *b, void *s, char *buf, int cap) { (void)b; (void)s; (void)buf; (void)cap; return -ENOSYS; }
static void r_stop(sia_backend_t *b, void *s) { (void)b; (void)s; }
static void r_info(sia_backend_t *b, char *model, int cap, uint64_t *mem, int *ctx, uint32_t *sp)
{ (void)b; strlcpy(model, "(no network yet)", cap); *mem = 0; *ctx = 0; *sp = 0; }

sia_backend_t sia_remote = { "remote", r_init, r_new, r_free, r_begin, r_next, r_stop, r_info, 0, 0 };   /* (tokens: estimated by siad) */
