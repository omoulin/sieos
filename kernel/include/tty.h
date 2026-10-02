/*
 * tty.h
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_TTY_H
#define SIEOS_TTY_H

#include "kernel.h"
#include "abi.h"
#include "sync.h"

#define TTY_BUF 1024

struct file;

struct tty {
    struct termios t;
    struct winsize ws;
    int session;               /* controlling session id, 0 = none */
    int pgrp;                  /* foreground process group */
    bool hungup;               /* pty master went away */
    char rbuf[TTY_BUF];        /* input ready for readers */
    size_t r_head, r_tail;
    int eof_pending;
    char line[TTY_BUF];        /* canonical-mode edit buffer */
    size_t line_len;
    /* output sink (console or pty master buffer); returns bytes accepted */
    size_t (*output)(struct tty *t, const char *s, size_t n);
    void (*oflush)(struct tty *t);   /* tcflush(TCOFLUSH): drop queued output; NULL: none queued */
    void *priv;
    kcondvar_t cv;             /* its readers wait (with tty_lock) */
};

extern struct tty console_tty;
extern kmutex_t tty_lock;                  /* every terminal and pseudo-terminal (tty.c) */

void tty_init(void);
void tty_setup(struct tty *t, size_t (*output)(struct tty *, const char *, size_t), void *priv);
void tty_register(struct tty *t);
void tty_unregister(struct tty *t);
void tty_input(struct tty *t, char c);
void tty_input_locked(struct tty *t, const char *s, size_t n);
void tty_get_termios(struct tty *t, struct termios *out);
long tty_read(struct tty *t, char *buf, size_t n);
long tty_write(struct tty *t, const char *buf, size_t n);
long tty_ioctl(struct tty *t, unsigned long cmd, uint64_t arg);
long tty_set_termios(struct tty *t, const struct termios *kt, unsigned long cmd);
long tty_flush(struct tty *t, int which);
bool tty_readable(struct tty *t);
void tty_hangup(struct tty *t);
void tty_session_exit(int sid);
int  tty_foreground_pgrp(void);
struct tty *tty_of_session(int sid);

/* pty.c */
struct pty;
int  pty_open_master(struct file *f);
int  pty_open_slave(int n, struct file *f);
int  pty_index(struct tty *t);             /* -1 if not a pty slave */
int  pty_master_index(struct pty *p);
bool pty_slot(int n, int *uid, int *gid);
int  pty_count(void);
long pty_master_ioctl(struct pty *p, int op, char *buf);
long pty_master_read(struct pty *p, char *buf, size_t n);
long pty_master_write(struct pty *p, const char *buf, size_t n);
bool pty_master_readable(struct pty *p);
void pty_master_close(struct pty *p);
void pty_slave_close(struct tty *t);
bool tty_is_pty(struct tty *t);
long pty_slave_write(struct tty *t, const char *buf, size_t n);
void pty_slave_ref(struct tty *t);
long pty_winsize(struct pty *p, unsigned long cmd, uint64_t arg);

#endif
