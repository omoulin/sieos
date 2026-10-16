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
 * Raw mode (CON_ECHO with CON_RAW: ssh's interactive sessions): no editing,
 * no echo; each key goes to the reader as it comes, as a terminal sends
 * it (Enter is CR, Backspace DEL, arrows ESC [ A..D), for the program on
 * the other end to interpret.
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
    t->lines += t->echo != CON_RAW;               /* (raw keys are not lines) */
    tty_deliver(t);
}

void tty_input(tty_t *t, char c)
{
    if (t->echo == CON_RAW) {
        if (c == '\n') c = '\r';
        if (c == '\b') c = 0x7F;
        if (t->len < (int)sizeof t->line) t->line[t->len++] = c;
        if (t->reader) finish(t);
        return;
    }
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
    if (t->echo == CON_RAW && t->len && t->dlen < 0) finish(t);
    tty_deliver(t);
    return 0;
}

/* A key from a keyboard: a character, or (raw mode only) a KEY_... that
 * has a terminal's escape sequence. */
void tty_key(tty_t *t, int code)
{
    static const char seq[] = "ABDC\0\0HF";      /* KEY_UP, DOWN, LEFT, RIGHT, -, -, HOME, END */
    if (code < 0x100) tty_input(t, (char)code);
    else if (t->echo == CON_RAW && code >= KEY_UP && code <= KEY_END && seq[code - KEY_UP]) {
        tty_input(t, 27); tty_input(t, '['); tty_input(t, seq[code - KEY_UP]);
    }
}

/* CON_ECHO: show, hide (passwords) or raw. Leaving raw mode answers a
 * waiting reader with nothing (the program that asked for raw keys is
 * done with them, or gone). */
void tty_mode(tty_t *t, int mode)
{
    if (t->echo == CON_RAW && mode != CON_RAW) {
        t->len = 0;
        if (t->reader && t->dlen < 0) { t->dlen = 0; tty_deliver(t); }
    }
    t->echo = mode;
}
