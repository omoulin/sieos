/*
 * tty.c - Terminals: line discipline and job control.
 *
 * A struct tty is either the console or the slave side of a pseudo-
 * terminal.  Canonical mode collects a line (echo, erase, kill, EOF)
 * before readers see it; raw mode passes bytes through.  A terminal
 * belongs to at most one session and has a foreground process group:
 * ^C, ^\ and ^Z send SIGINT, SIGQUIT and SIGTSTP to that group, and
 * background processes that read (or write with TOSTOP) get SIGTTIN /
 * SIGTTOU.
 */
#include "proc.h"
#include "tty.h"
#include "mm.h"
#include "poll.h"

#define MAX_TTYS 24

struct tty console_tty;
static struct tty *ttys[MAX_TTYS];

static size_t console_output(struct tty *t, const char *s, size_t n)
{
    UNUSED(t);
    console_write(s, n);
    return n;
}

void tty_setup(struct tty *t, size_t (*output)(struct tty *, const char *, size_t), void *priv)
{
    memset(t, 0, sizeof(*t));
    t->t.c_iflag = ICRNL;
    t->t.c_oflag = OPOST | ONLCR;
    t->t.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | IEXTEN;
    t->t.c_cc[VINTR] = 3;
    t->t.c_cc[VQUIT] = 28;
    t->t.c_cc[VERASE] = 127;
    t->t.c_cc[VKILL] = 21;
    t->t.c_cc[VEOF] = 4;
    t->t.c_cc[VMIN] = 1;
    t->t.c_cc[VSUSP] = 26;
    t->ws.ws_row = 25;
    t->ws.ws_col = 80;
    t->output = output;
    t->priv = priv;
}

void tty_register(struct tty *t)
{
    for (int i = 0; i < MAX_TTYS; i++)
        if (!ttys[i]) {
            ttys[i] = t;
            return;
        }
}

void tty_unregister(struct tty *t)
{
    for (int i = 0; i < MAX_TTYS; i++)
        if (ttys[i] == t)
            ttys[i] = NULL;
}

void tty_init(void)
{
    tty_setup(&console_tty, console_output, NULL);
    console_tty.ws.ws_row = console_rows();
    console_tty.ws.ws_col = console_cols();
    tty_register(&console_tty);
}

int tty_foreground_pgrp(void)
{
    struct tty *t = tty_of_session(current->sid);
    return t ? t->pgrp : console_tty.pgrp;
}

struct tty *tty_of_session(int sid)
{
    for (int i = 0; i < MAX_TTYS; i++)
        if (ttys[i] && sid && ttys[i]->session == sid)
            return ttys[i];
    return NULL;
}

static void commit(struct tty *t, char c)
{
    if ((t->r_tail + 1) % TTY_BUF != t->r_head) {
        t->rbuf[t->r_tail] = c;
        t->r_tail = (t->r_tail + 1) % TTY_BUF;
    }
}

static void echo(struct tty *t, const char *s, size_t n)
{
    if (t->t.c_lflag & ECHO)
        t->output(t, s, n);
}

static void ready(struct tty *t)
{
    wakeup(t);
    poll_wakeup();
}

static void erase_char(struct tty *t)
{
    if (t->line_len > 0) {
        t->line_len--;
        echo(t, "\b \b", 3);
    }
}

static void signal_char(struct tty *t, int sig, const char *echo_str)
{
    if (t->t.c_lflag & ECHO)
        t->output(t, echo_str, strlen(echo_str));
    if (!(t->t.c_lflag & NOFLSH)) {
        t->line_len = 0;
        t->r_head = t->r_tail;
    }
    if (t->pgrp)
        signal_pgrp(t->pgrp, sig);
    ready(t);
}

void tty_input(struct tty *t, char c)
{
    struct termios *tm = &t->t;
    if (c == '\r' && (tm->c_iflag & ICRNL))
        c = '\n';

    if (tm->c_lflag & ISIG) {
        if (c == tm->c_cc[VINTR]) {
            signal_char(t, SIGINT, "^C\n");
            return;
        }
        if (c == tm->c_cc[VQUIT]) {
            signal_char(t, SIGQUIT, "^\\\n");
            return;
        }
        if (c == tm->c_cc[VSUSP]) {
            signal_char(t, SIGTSTP, "^Z\n");
            return;
        }
    }

    if (!(tm->c_lflag & ICANON)) {
        commit(t, c);
        if (c == '\n' ? (tm->c_lflag & (ECHO | ECHONL)) : (tm->c_lflag & ECHO))
            t->output(t, &c, 1);
        ready(t);
        return;
    }

    if (c == tm->c_cc[VERASE] || c == '\b' || c == 127) {
        erase_char(t);
        return;
    }
    if (c == tm->c_cc[VKILL]) {
        while (t->line_len > 0)
            erase_char(t);
        return;
    }
    if (c == tm->c_cc[VEOF]) {
        for (size_t i = 0; i < t->line_len; i++)
            commit(t, t->line[i]);
        if (t->line_len == 0)
            t->eof_pending++;
        t->line_len = 0;
        ready(t);
        return;
    }
    if (c == 12) {                         /* ^L: clear screen, redraw line */
        t->output(t, "\f", 1);
        echo(t, t->line, t->line_len);
        return;
    }
    if (t->line_len < TTY_BUF - 1) {
        t->line[t->line_len++] = c;
        if (c == '\n' ? (tm->c_lflag & (ECHO | ECHONL)) : (tm->c_lflag & ECHO))
            t->output(t, &c, 1);
    }
    if (c == '\n') {
        for (size_t i = 0; i < t->line_len; i++)
            commit(t, t->line[i]);
        t->line_len = 0;
        ready(t);
    }
}

/* Is the current process in a background group of this terminal's session? */
static bool is_background(struct tty *t)
{
    return t->session && current->sid == t->session && current->pgid != t->pgrp;
}

bool tty_readable(struct tty *t)
{
    return t->r_head != t->r_tail || t->eof_pending || t->hungup;
}

long tty_read(struct tty *t, char *buf, size_t n)
{
    if (n == 0)
        return 0;
    if (is_background(t)) {
        if (signal_ignored_or_blocked(current, SIGTTIN))
            return -EIO;
        signal_pgrp(current->pgid, SIGTTIN);
        return -ERESTART;
    }
    while (!tty_readable(t)) {
        if (signal_pending(current))
            return -ERESTART;
        sleep_on(t);
        if (is_background(t) && !signal_pending(current)) {
            signal_pgrp(current->pgid, SIGTTIN);
            return -ERESTART;
        }
    }
    size_t got = 0;
    bool canon = t->t.c_lflag & ICANON;
    while (got < n && t->r_head != t->r_tail) {
        char c = t->rbuf[t->r_head];
        t->r_head = (t->r_head + 1) % TTY_BUF;
        buf[got++] = c;
        if (canon && c == '\n')
            break;
    }
    if (got == 0 && t->eof_pending)
        t->eof_pending--;
    return got;                            /* 0 = EOF (or hang-up) */
}

long tty_write(struct tty *t, const char *buf, size_t n)
{
    if (t->hungup)
        return -EIO;
    if ((t->t.c_lflag & TOSTOP) && is_background(t) && !signal_ignored_or_blocked(current, SIGTTOU)) {
        signal_pgrp(current->pgid, SIGTTOU);
        return -ERESTART;
    }
    if (tty_is_pty(t))
        return pty_slave_write(t, buf, n);
    t->output(t, buf, n);
    return n;
}

void tty_hangup(struct tty *t)
{
    t->hungup = true;
    if (t->session) {
        for (int i = 1; i < NPROC; i++) {
            struct proc *p = &proc_table[i];
            if (p->state != PSTATE_UNUSED && p->state != PSTATE_ZOMBIE && p->sid == t->session) {
                signal_send(p, SIGHUP);
                signal_send(p, SIGCONT);
            }
        }
    }
    t->session = 0;
    t->pgrp = 0;
    ready(t);
}

void tty_session_exit(int sid)
{
    for (int i = 0; i < MAX_TTYS; i++) {
        if (ttys[i] && ttys[i]->session == sid) {
            ttys[i]->session = 0;
            ttys[i]->pgrp = 0;
        }
    }
}

/* Set the terminal attributes from a kernel copy (TCSETS, TCSETSW, TCSETSF). */
long tty_set_termios(struct tty *t, const struct termios *kt, unsigned long cmd)
{
    if (is_background(t) && !signal_ignored_or_blocked(current, SIGTTOU)) {
        signal_pgrp(current->pgid, SIGTTOU);
        return -ERESTART;
    }
    memcpy(&t->t, kt, sizeof(struct termios));
    if (cmd == TCSETSF) {
        t->r_head = t->r_tail;
        t->line_len = 0;
    }
    return 0;
}

/* tcflush: 0 input, 1 output, 2 both (output is never queued). */
long tty_flush(struct tty *t, int which)
{
    if (which < 0 || which > 2)
        return -EINVAL;
    if (which != 1) {
        t->r_head = t->r_tail;
        t->line_len = 0;
    }
    return 0;
}

long tty_ioctl(struct tty *t, unsigned long cmd, uint64_t arg)
{
    uint64_t pml4 = current->pml4;
    switch (cmd) {
    case TCGETS:
        if (!user_range_ok(pml4, arg, sizeof(struct termios), true))
            return -EFAULT;
        memcpy((void *)arg, &t->t, sizeof(struct termios));
        return 0;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (!user_range_ok(pml4, arg, sizeof(struct termios), false))
            return -EFAULT;
        return tty_set_termios(t, (const struct termios *)arg, cmd);
    case TIOCGPGRP:
        if (!user_range_ok(pml4, arg, sizeof(int), true))
            return -EFAULT;
        if (current->sid != t->session)
            return -ENOTTY;
        *(int *)arg = t->pgrp;
        return 0;
    case TIOCSPGRP: {
        if (!user_range_ok(pml4, arg, sizeof(int), false))
            return -EFAULT;
        int pgrp = *(int *)arg;
        if (!t->session || current->sid != t->session)
            return -ENOTTY;
        if (is_background(t) && !signal_ignored_or_blocked(current, SIGTTOU)) {
            signal_pgrp(current->pgid, SIGTTOU);
            return -ERESTART;
        }
        if (pgrp <= 0 || !pgrp_exists_in_session(pgrp, t->session))
            return -EPERM;
        t->pgrp = pgrp;
        return 0;
    }
    case TIOCSCTTY:
        if (current->sid != current->pid)
            return -EPERM;                     /* only session leaders */
        if (t->session == current->sid)
            return 0;
        if (tty_of_session(current->sid))
            return -EPERM;                     /* already has a controlling tty */
        if (t->session && !(current->euid == 0 && arg == 1))
            return -EPERM;
        t->session = current->sid;
        t->pgrp = current->pgid;
        t->hungup = false;
        return 0;
    case TIOCNOTTY:
        if (current->sid != t->session)
            return -ENOTTY;
        if (current->sid == current->pid)
            tty_session_exit(current->sid);
        return 0;
    case TIOCGWINSZ:
        if (!user_range_ok(pml4, arg, sizeof(struct winsize), true))
            return -EFAULT;
        memcpy((void *)arg, &t->ws, sizeof(struct winsize));
        return 0;
    case TIOCSWINSZ:
        if (!user_range_ok(pml4, arg, sizeof(struct winsize), false))
            return -EFAULT;
        memcpy(&t->ws, (void *)arg, sizeof(struct winsize));
        if (t->pgrp)
            signal_pgrp(t->pgrp, SIGWINCH);
        return 0;
    }
    return -ENOTTY;
}
