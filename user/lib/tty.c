/*
 * tty.c - Typed lines ("line discipline"), shared by the text console (con)
 * and the terminal windows of the desktop (atlas).
 *
 * Characters collect in `line`, shown as they come (unless echo is off,
 * for passwords); backspace edits. At Enter the line is complete: it goes
 * to the program waiting in CON_READ (its reply token in `reader`), or
 * waits in `done` for the next CON_READ. Ctrl-C drops the line, Ctrl-D
 * sends what is there (nothing: end of input).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

void tty_init(tty_t *t, void (*out)(tty_t *, const char *, size_t), void *ctx)
{
    memset(t, 0, sizeof *t);
    t->dlen = -1;
    t->echo = 1;
    t->out = out;
    t->ctx = ctx;
}

/* Answer the waiting reader, if there is one and a line is complete. */
void tty_deliver(tty_t *t)
{
    if (!t->reader || t->dlen < 0) return;
    size_t n = (size_t)t->dlen < t->want ? (size_t)t->dlen : t->want;
    msg_t m = { .w = { n }, .sbuf = t->done, .slen = n };
    ipc_reply(t->reader, &m);
    t->reader = 0;
    t->dlen = -1;
}

static void finish(tty_t *t)
{
    memcpy(t->done, t->line, t->len);
    t->dlen = t->len;
    t->len = 0;
    t->lines++;
    tty_deliver(t);
}

void tty_input(tty_t *t, char c)
{
    if (c == '\r') c = '\n';
    if (c == 0x7F) c = '\b';                  /* a serial terminal's backspace */
    if (c == '\b') { if (t->len) { t->len--; if (t->echo) t->out(t, "\b \b", 3); } return; }
    if (c == 3)  { t->out(t, "^C\n", 3); t->len = 0; t->line[t->len++] = '\n'; finish(t); return; }
    if (c == 4)  { finish(t); return; }
    if (t->len >= (int)sizeof t->line - 1 && c != '\n') return;
    t->line[t->len++] = c;
    if (t->echo) t->out(t, &c, 1);
    if (c == '\n') finish(t);
}

/* A CON_READ from `from`: answered now if a line is ready, else later.
 * Returns -EBUSY if another program is already waiting. */
long tty_read(tty_t *t, long from, size_t want)
{
    if (t->reader) return -EBUSY;
    t->reader = from;
    t->want = want;
    tty_deliver(t);
    return 0;
}
