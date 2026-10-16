/*
 * sshd - The SSH server (docs/ssh.md): port 22, started at boot by init
 * when /etc/ssh/sshd_config exists (else it ends at once and stays off).
 *
 *   client ──TCP 22──► sshd: transport (user/lib/ssh.c), host key
 *                       │    /etc/ssh/ssh_host_ed25519_key (made on first start)
 *                       ├─ user authentication: ~/.ssh/authorized_keys, or the
 *                       │  password, checked by auth (AUTH_CHECK)
 *                       └─ "shell" / "exec": auth starts the user's sh as our
 *                          child (AUTH_LOGIN, with the password: it opens an
 *                          encrypted home), its terminal "sshdN#session"
 *   sh ──CON_WRITE / CON_READ──► sshd ──channel data──► client
 *
 * The terminal: sshd is to the remote shell what the console server is to
 * the local one. Each connection slot has a port ("sshd0".."sshd7") served
 * by a thread of its own; what the shell writes goes to the client (with
 * CR LF line ends when the client asked for a terminal), and what the
 * client types goes through the same line editing as the console
 * (user/lib/tty.c), echoed by us. The channel number in w[3] is the
 * session's, so a program left over from an earlier session cannot reach
 * a new one; only the session's user (or root) is answered.
 *
 * Threads: the main one accepts connections; per slot, one runs the
 * connection (it reads the client) and one serves the terminal; one reaps
 * the shells (the kernel tells it when a child ends), and reports the exit
 * status to the client. Slots and their threads are made once and reused
 * (a thread's stack is never given back).
 *
 * Rules, as OpenSSH's defaults: root only with a key; 6 tries per
 * connection; 2 minutes to log in.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ssh.h"
#include "mk/crypto.h"

#define MAXC    8                       /* connections at once */
#define WINDOW  (1 << 20)
#define HOSTKEY "/etc/ssh/ssh_host_ed25519_key"

typedef struct {
    int used, slot, tid;
    uint32_t gen;                       /* the session's number: the terminal's channel */
    long sock, port;
    ssh_t *s;
    char user[32], pw[128];             /* the password: kept until the shell starts */
    int uid, pubkey;
    uint32_t peer_chan, peer_win, peer_max;
    int chan, pty, pid, eof, closed;
    tty_t tty;
    char in[4096];                      /* typed ahead: for the line editing when it can take it */
    size_t inlen;
    char lock;                          /* tty, in, pid, eof, closed */
} conn_t;

static conn_t conns[MAXC];
static uint8_t seed[32], pub[32];
static uint32_t sessions;
static long authport;

/* A line for the kernel log (the serial port). */
typedef struct { char b[160]; size_t n; } line_t;
static void lput(void *c, char ch) { line_t *l = c; if (l->n < sizeof l->b) l->b[l->n++] = ch; }
static void note(const char *f, ...)
{
    line_t l = { .n = 0 };
    va_list ap;
    va_start(ap, f);
    vformat(lput, &l, f, ap);
    va_end(ap);
    sys_debug(l.b, l.n);
}

static void lock(conn_t *c)   { while (__atomic_test_and_set(&c->lock, __ATOMIC_ACQUIRE)) sys_yield(); }
static void unlock(conn_t *c) { __atomic_clear(&c->lock, __ATOMIC_RELEASE); }

/* To the client as channel data. wait: for room in its window (the
 * terminal's writes); else (echo, from the reading thread itself) what does
 * not fit is dropped. */
static void output(conn_t *c, const char *d, size_t n, int wait)
{
    char o[4096];
    while (n && c->chan && !c->closed) {
        size_t k = 0, room = sizeof o < c->peer_max ? sizeof o : c->peer_max;
        while (n && k + 2 <= room) {
            if (c->pty && *d == '\n') o[k++] = '\r';
            o[k++] = *d++;
            n--;
        }
        while (__atomic_load_n(&c->peer_win, __ATOMIC_ACQUIRE) < k)
            if (!wait || !c->chan || c->closed) return;
            else sys_sleep(10000000);
        __atomic_sub_fetch(&c->peer_win, k, __ATOMIC_ACQ_REL);
        ssh_wr_t *w = ssh_begin(c->s, SSH_CHANNEL_DATA);
        ssh_put32(w, c->peer_chan);
        ssh_putstr(w, o, k);
        ssh_end(c->s);
    }
}

static void echo_out(tty_t *t, const char *s, size_t n) { output(t->ctx, s, n, 0); }

/* Typed-ahead input into the line editing (c locked), until a line waits
 * for its reader (the tty holds one complete line). */
static void feed(conn_t *c)
{
    size_t i = 0;
    while (i < c->inlen && c->tty.dlen < 0) tty_input(&c->tty, c->in[i++]);
    memmove(c->in, c->in + i, c->inlen - i);
    c->inlen -= i;
    if (c->eof && !c->inlen && c->tty.reader) tty_input(&c->tty, 4);   /* the end: what is there (Ctrl-D) */
}

/* The shell ended (c locked): its status, then the channel closes. */
static void ended(conn_t *c, int st)
{
    ssh_wr_t *w = ssh_begin(c->s, SSH_CHANNEL_REQUEST);
    ssh_put32(w, c->peer_chan); ssh_puts(w, "exit-status"); ssh_put8(w, 0);
    ssh_put32(w, st < 0 ? 255 : (uint32_t)st);
    ssh_end(c->s);
    w = ssh_begin(c->s, SSH_CHANNEL_EOF); ssh_put32(w, c->peer_chan); ssh_end(c->s);
    w = ssh_begin(c->s, SSH_CHANNEL_CLOSE); ssh_put32(w, c->peer_chan); ssh_end(c->s);
    c->pid = 0;
    c->closed = 1;
}

/* A slot's terminal: what its shell (and the programs it runs) ask. */
static void terminal(void *arg)
{
    conn_t *c = arg;
    char buf[16384];
    for (;;) {
        msg_t m = { .rbuf = buf, .rlen = sizeof buf };
        long from = ipc_recv(c->port, &m);
        if (from <= 0) continue;
        if (!c->used || m.w[3] != c->gen || (m.uid != c->uid && m.uid)) { reply_val(from, -EIO); continue; }
        switch (m.w[0]) {
        case CON_WRITE:
            output(c, buf, m.rlen, 1);
            reply_val(from, (long)m.rlen);
            break;
        case CON_READ:
            lock(c);
            if (c->eof && !c->inlen && !c->tty.len && c->tty.dlen < 0) reply_val(from, c->eof++ > 1 ? -EIO : 0);   /* the end, then: gone */
            else if (tty_read(&c->tty, from, m.w[1]) < 0) reply_val(from, -EBUSY);
            else feed(c);
            unlock(c);
            break;
        case CON_ECHO: lock(c); tty_mode(&c->tty, (int)m.w[1]); unlock(c); reply_val(from, 0); break;
        default: reply_val(from, -ENOSYS);
        }
    }
}

/* Ended shells: the kernel's note on our child port. */
static void reaper(void *arg)
{
    long port = (long)arg;
    for (;;) {
        msg_t m = { 0 };
        long from = ipc_recv(port, &m);
        if (from > 0) { reply_val(from, -ENOSYS); continue; }
        for (int i = 0; i < MAXC; i++) {
            conn_t *c = &conns[i];
            int st;
            lock(c);
            if (c->pid && sys_wait(c->pid, &st, WAIT_NOHANG) == c->pid) ended(c, st);
            unlock(c);
        }
    }
}

static long auth_call(int op, int flags, const char *req, size_t len)
{
    msg_t m = { .w = { op, (uint64_t)flags }, .sbuf = req, .slen = len };
    long e = call_named(&authport, "auth", &m, 0);
    return e < 0 ? e : (long)m.w[0];
}

/* "user\0pw[\0more\0...]" in req -> its length. */
static size_t pack(char *req, size_t size, const char **f, int n)
{
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(f[i]) + 1;
        if (len + l > size) return 0;
        memcpy(req + len, f[i], l);
        len += l;
    }
    return len;
}

/* One user authentication request -> 1: accepted. */
static int userauth(conn_t *c, ssh_rd_t *r)
{
    ssh_t *s = c->s;
    uint32_t n, ul, bl, sl;
    const uint8_t *u = ssh_str(r, &ul);
    acct_user_t a;
    char user[32];
    if (ul >= sizeof user) return 0;
    memcpy(user, u, ul);
    user[ul] = 0;
    if (!ssh_is(r, "ssh-connection") || acct_user(user, 0, &a)) { ssh_str(r, &n); return 0; }
    const uint8_t *method = ssh_str(r, &n);
    if (n == 9 && !memcmp(method, "publickey", 9)) {
        int signed_ = ssh_u8(r);
        if (!ssh_is(r, "ssh-ed25519")) return 0;
        const uint8_t *blob = ssh_str(r, &bl);
        if (bl != SSH_BLOB || r->bad) return 0;
        char path[FS_PATH];
        strlcpy(path, a.home, sizeof path);
        strlcpy(path + strlen(path), "/.ssh/authorized_keys", sizeof path - strlen(path));
        char *keys = file_get(path, 0);           /* (an encrypted home that is still locked: none) */
        int listed = keys && ssh_key_listed(keys, 0, blob + 19) == 1;
        free(keys);
        if (!listed) return 0;
        if (!signed_) {                           /* "would this key do?" */
            ssh_wr_t *w = ssh_begin(s, SSH_USERAUTH_PK_OK);
            ssh_puts(w, "ssh-ed25519");
            ssh_putstr(w, blob, SSH_BLOB);
            ssh_end(s);
            return -1;
        }
        ssh_rd_t g;
        g.p = ssh_str(r, &sl); g.end = g.p + sl; g.bad = 0;
        const uint8_t *sig = ssh_is(&g, "ssh-ed25519") ? ssh_str(&g, &sl) : 0;
        if (!sig || sl != 64 || g.bad || r->bad) return 0;
        uint8_t data[256];                        /* what the client signed */
        ssh_wr_t d = { data, 0, sizeof data };
        ssh_putstr(&d, s->sid, 32);
        ssh_put8(&d, SSH_USERAUTH_REQUEST);
        ssh_puts(&d, user); ssh_puts(&d, "ssh-connection"); ssh_puts(&d, "publickey");
        ssh_put8(&d, 1);
        ssh_puts(&d, "ssh-ed25519");
        ssh_putstr(&d, blob, SSH_BLOB);
        if (d.n > sizeof data || ed25519_verify(sig, data, d.n, blob + 19)) return 0;
        c->pubkey = 1;
    } else if (n == 8 && !memcmp(method, "password", 8)) {
        ssh_u8(r);
        const uint8_t *p = ssh_str(r, &n);
        if (r->bad || n >= sizeof c->pw || !a.uid) return 0;   /* root: with a key only */
        memcpy(c->pw, p, n);
        c->pw[n] = 0;
        char req[200];
        const char *f[] = { user, c->pw };
        size_t len = pack(req, sizeof req, f, 2);
        long e = len ? auth_call(AUTH_CHECK, 0, req, len) : -EINVAL;
        wipe(req, sizeof req);
        if (e) { wipe(c->pw, sizeof c->pw); return 0; }
    } else return 0;
    strlcpy(c->user, user, sizeof c->user);
    c->uid = a.uid;
    return 1;
}

/* "shell" or "exec": auth starts the user's shell, on our terminal. */
static int start(conn_t *c, const char *cmd)
{
    char req[600], console[16];
    ssh_fmt(console, sizeof console, "sshd%d#%u", c->slot, c->gen);
    const char *f[] = { c->user, c->pw, console, cmd };
    size_t len = pack(req, sizeof req, f, 4);
    lock(c);
    tty_init(&c->tty, echo_out, c);
    c->tty.echo = c->pty;
    unlock(c);
    long pid = len ? auth_call(AUTH_LOGIN, c->pubkey ? AUTH_NOPW : 0, req, len) : -EINVAL;
    wipe(req, sizeof req);
    wipe(c->pw, sizeof c->pw);
    if (pid <= 0) return 0;
    int st;
    lock(c);
    c->pid = (int)pid;
    if (sys_wait(c->pid, &st, WAIT_NOHANG) == c->pid) ended(c, st);   /* (it may have ended already) */
    unlock(c);
    return 1;
}

/* A connection, from key exchange to its end. */
static void serve(conn_t *c)
{
    ssh_t *s = c->s;
    ssh_rd_t r;
    uint32_t n, lwin = WINDOW;
    const char *why;
    memset(s, 0, sizeof *s);
    s->sock = c->sock;
    s->server = 1;
    if (ssh_kex(s, seed, pub, &why)) { note("sshd: key exchange failed: %s\n", why); return; }
    if (ssh_recv(s, &r, 120000) != SSH_SERVICE_REQUEST || !ssh_is(&r, "ssh-userauth")) return;
    ssh_puts(ssh_begin(s, SSH_SERVICE_ACCEPT), "ssh-userauth");
    ssh_end(s);
    for (int tries = 0;;) {                       /* authentication: 2 minutes, 6 tries */
        if (ssh_recv(s, &r, 120000) != SSH_USERAUTH_REQUEST) return;
        int ok = userauth(c, &r);
        if (ok > 0) break;
        if (ok < 0) continue;
        if (++tries == 6) { ssh_disconnect(s, "too many authentication failures"); return; }
        ssh_wr_t *w = ssh_begin(s, SSH_USERAUTH_FAILURE);
        ssh_puts(w, "publickey,password");
        ssh_put8(w, 0);
        ssh_end(s);
    }
    ssh_begin(s, SSH_USERAUTH_SUCCESS);
    ssh_end(s);
    note("sshd: %s logged in (%s)\n", c->user, c->pubkey ? "key" : "password");

    for (;;) {                                    /* the connection protocol (RFC 4254) */
        int t = ssh_recv(s, &r, 0), want, ok = 0;
        ssh_wr_t *w;
        const uint8_t *d;
        switch (t) {
        case SSH_CHANNEL_OPEN:                    /* one session channel */
            if (ssh_is(&r, "session") && !c->chan) {
                c->peer_chan = ssh_u32(&r);
                c->peer_win = ssh_u32(&r);
                c->peer_max = ssh_u32(&r);
                if (c->peer_max > SSH_MAXDATA) c->peer_max = SSH_MAXDATA;
                c->chan = 1;
                w = ssh_begin(s, SSH_CHANNEL_OPEN_CONFIRMATION);
                ssh_put32(w, c->peer_chan); ssh_put32(w, 0); ssh_put32(w, WINDOW); ssh_put32(w, SSH_MAXDATA);
            } else {
                uint32_t id = ssh_u32(&r);
                w = ssh_begin(s, SSH_CHANNEL_OPEN_FAILURE);
                ssh_put32(w, id); ssh_put32(w, 1); ssh_puts(w, "only one session"); ssh_puts(w, "");
            }
            ssh_end(s);
            break;
        case SSH_CHANNEL_REQUEST:
            ssh_u32(&r);
            d = ssh_str(&r, &n);
            want = ssh_u8(&r);
            if (n == 7 && !memcmp(d, "pty-req", 7)) ok = c->pty = 1;
            else if ((n == 5 && !memcmp(d, "shell", 5)) || (n == 4 && !memcmp(d, "exec", 4))) {
                char cmd[512] = "";
                int bad = 0;
                if (n == 4) {                     /* exec: a command line, not empty */
                    const uint8_t *x = ssh_str(&r, &n);
                    if ((bad = !n || n >= sizeof cmd)) n = 0;
                    memcpy(cmd, x, n);
                    cmd[n] = 0;
                }
                ok = !bad && !r.bad && c->chan && !c->pid && !c->closed && start(c, cmd);
            }
            if (want) { w = ssh_begin(s, ok ? SSH_CHANNEL_SUCCESS : SSH_CHANNEL_FAILURE); ssh_put32(w, c->peer_chan); ssh_end(s); }
            break;
        case SSH_CHANNEL_DATA:                    /* typed: through the line editing */
            ssh_u32(&r);
            d = ssh_str(&r, &n);
            lwin -= n < lwin ? n : lwin;
            lock(c);
            if (n > sizeof c->in - c->inlen) n = (uint32_t)(sizeof c->in - c->inlen);   /* (more: lost) */
            memcpy(c->in + c->inlen, d, n);
            c->inlen += n;
            feed(c);
            unlock(c);
            if (lwin < WINDOW / 2) {
                w = ssh_begin(s, SSH_CHANNEL_WINDOW_ADJUST);
                ssh_put32(w, c->peer_chan); ssh_put32(w, WINDOW - lwin);
                ssh_end(s);
                lwin = WINDOW;
            }
            break;
        case SSH_CHANNEL_EOF:                     /* no more input: the reader gets what is there */
            lock(c);
            c->eof = 1;
            feed(c);
            unlock(c);
            break;
        case SSH_CHANNEL_WINDOW_ADJUST:
            ssh_u32(&r);
            __atomic_add_fetch(&c->peer_win, ssh_u32(&r), __ATOMIC_ACQ_REL);
            break;
        case SSH_GLOBAL_REQUEST:
            ssh_str(&r, &n);
            if (ssh_u8(&r)) { ssh_begin(s, SSH_REQUEST_FAILURE); ssh_end(s); }
            break;
        case SSH_CHANNEL_CLOSE:
            if (!c->closed) { w = ssh_begin(s, SSH_CHANNEL_CLOSE); ssh_put32(w, c->peer_chan); ssh_end(s); }
            return;
        case -1:
            return;
        }
    }
}

/* A slot's connection thread: one connection after another. */
static void slot_main(void *arg)
{
    conn_t *c = arg;
    for (;;) {
        while (!__atomic_load_n(&c->sock, __ATOMIC_ACQUIRE)) sys_sleep(3600000000000UL);   /* until main gives us one (sys_wake) */
        serve(c);
        lock(c);
        int pid = c->pid;
        c->pid = 0;
        c->closed = 1;                            /* (a terminal write waiting for room gives up) */
        unlock(c);
        if (pid) { int st; sys_kill(pid); sys_wait(pid, &st, 0); }   /* the session ends with its connection */
        net_close(c->sock);
        wipe(c->pw, sizeof c->pw);
        wipe(c->s, sizeof *c->s);
        c->sock = 0;
        __atomic_store_n(&c->used, 0, __ATOMIC_RELEASE);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    siefs_stat_t st;
    if (fs_stat("/etc/ssh/sshd_config", &st, 0)) return 0;    /* off: not started again */
    int e = ssh_key_load(HOSTKEY, seed, pub);
    if (e == -ENOENT) {                           /* first start: a host key of our own */
        fs_mkdir("/etc/ssh", 0755);
        e = ssh_key_new(HOSTKEY, "root@sieos");
        if (!e) e = ssh_key_load(HOSTKEY, seed, pub);
    }
    char fp[52];
    if (e) { note("sshd: no host key (%s, error %d)\n", HOSTKEY, -e); return 1; }
    ssh_fingerprint(fp, pub);
    long cport = port_create(0);
    sys_child_port(cport);
    thread_start(reaper, (void *)cport, 8192);
    long ls = net_listen(22, 8);
    if (ls < 0) { note("sshd: cannot listen on port 22 (%s)\n", net_strerror(ls)); return 1; }
    note("sshd: listening on port 22, host key %s\n", fp);
    for (;;) {
        uint32_t ip;
        long sk = net_accept(ls, &ip, 0, 0);
        if (sk < 0) { if (sk == -EPIPE) return 1; continue; }   /* (netd restarted: init starts us again) */
        conn_t *c = 0;
        for (int i = 0; i < MAXC && !c; i++)
            if (!__atomic_load_n(&conns[i].used, __ATOMIC_ACQUIRE)) c = &conns[i];
        if (!c) { net_close(sk); continue; }
        if (!c->s) {                              /* a slot's first use: its port and threads */
            char name[8];
            c->slot = (int)(c - conns);
            ssh_fmt(name, sizeof name, "sshd%d", c->slot);
            if (!(c->s = malloc(sizeof *c->s)) || (c->port = port_create(name)) < 0) { free(c->s); c->s = 0; net_close(sk); continue; }
            thread_start(terminal, c, 24576);
            c->tid = (int)thread_start(slot_main, c, 16384);
        }
        char a[16];
        note("sshd: connection from %s\n", net_ntoa(ip, a));
        c->used = 1;
        c->gen = ++sessions % 1000000000;
        c->uid = -1; c->pubkey = c->chan = c->pty = c->pid = c->eof = c->closed = 0;
        c->inlen = 0;
        __atomic_store_n(&c->sock, sk, __ATOMIC_RELEASE);
        sys_wake(c->tid);
    }
}
