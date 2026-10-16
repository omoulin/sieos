/*
 * mail.h - What the mail program's two files share (docs/desktop.md, Mail):
 * a growing text buffer, and the message decoding of mime.c.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "net.h"

#define MAXMSG (8 << 20)                 /* the largest message read or kept */

typedef struct { char *s; size_t len, cap; } buf_t;   /* s is kept 0-terminated */
void b_add(buf_t *b, const void *s, size_t n);
void b_str(buf_t *b, const char *s);
void b_fmt(buf_t *b, const char *fmt, ...);
void b_free(buf_t *b);

int  lower(int c);
int  hexv(int c);                                      /* a hex digit's value, or -1 */
int  same_ci(const char *a, const char *b, size_t n);  /* the first n characters, any case */
const char *find_ci(const char *s, const char *end, const char *what);   /* or 0 */

/* mime.c: reading messages (RFC 5322, MIME, RFC 2047). */
int  mime_header(const char *msg, size_t len, const char *name, char *out, size_t cap);   /* unfolded -> 1 if there */
void mime_words(const char *in, char *out, size_t cap);   /* =?charset?B|Q?...?= words decoded, UTF-8 */
void mime_date(const char *date, char out[17]);           /* -> "YYYY-MM-DD HH:MM" (or "") */
void mime_read(const char *msg, size_t len, buf_t *out);  /* headers, attachments' names, blank line, the text */
int  mime_part(const char *msg, size_t len, int n, char *name, size_t cap, buf_t *data);  /* attachment n decoded -> how many */
size_t b64_encode(const void *in, size_t n, char *out);   /* -> length; out: 4/3 n + 4 bytes */
