/*
 * memory.h - sia's memory on the disk (memory.c): saved conversations and
 * what sia remembers about each user. siad alone uses it, for the uid the
 * kernel stamped on the request; the files are root's (docs/sia.md).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "backend.h"

#define MEM_MAXTURN 64                   /* turns kept in memory for one conversation */

typedef struct {                         /* a conversation, as read back from the disk */
    char *ctx;                           /* extra instructions (SIA_CONTEXT), or 0 */
    char *digest;                        /* summary of the older turns (compaction), or 0 */
    char *title;                         /* the first question, shortened */
    sia_turn_t turn[MEM_MAXTURN];        /* the turns after the digest (texts malloc'd) */
    int nturn;
} conv_t;

void  mem_setup(int max_convs, int max_kb);       /* limits, from /etc/sia.conf */
int   mem_new(int uid);                           /* a new conversation -> its id (> 0) */
int   mem_last(int uid);                          /* the most recent one, 0 if none */
int   mem_exists(int uid, int id);
long  mem_add(int uid, int id, char kind, const char *text, size_t n);  /* one record */
int   mem_load(int uid, int id, conv_t *c);       /* 0, or -error (c is then empty) */
void  mem_free(conv_t *c);
int   mem_rewrite(int uid, int id, const conv_t *c);   /* the whole log anew (compaction) */
long  mem_list(int uid, char *out, size_t cap, int (*tokens)(const conv_t *));
int   mem_forget(int uid, int id);                /* id 0: all of them */
char *mem_facts(int uid);                         /* malloc'd, or 0 */
int   mem_facts_put(int uid, const char *text);   /* replace them all */
