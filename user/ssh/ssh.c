/*
 * ssh - The SSH client (docs/ssh.md), and ssh-keygen (the same program,
 * under that name: /bin/ssh-keygen is a link to it).
 *
 *   ssh [-p PORT] [-i KEY] [USER@]HOST [COMMAND]
 *   ssh-keygen [-t ed25519] [-f FILE] [-C COMMENT]
 *
 * The transport is user/lib/ssh.c. Then: the server's key is checked
 * against ~/.ssh/known_hosts (asked about on first sight, refused if it
 * changed), the user authenticates with ~/.ssh/id_ed25519 (or -i) if the
 * server takes it, else a password (read without echo, three tries), and
 * one channel runs a shell or the command. A shell gets a terminal on the
 * server ("pty-req"), and the console goes raw (CON_RAW: every key goes to
 * the server as typed, the server echoes); a second thread reads the keys
 * while the first shows what comes back. "~." after Enter disconnects.
 * The exit status is the remote command's.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ssh.h"
#include "mk/crypto.h"

#define WINDOW (1 << 20)                  /* how much the server may send before we ask for more */

static ssh_t *s;
static acct_user_t me;
static uint32_t peer_chan, peer_win;      /* the server's channel number, the room it gives us */
static volatile int closing;

static void fail(const char *what, const char *why)
{
    printf("ssh: %s: %s\n", what, why);
    sys_exit(255);
}

static void path_in_home(char *p, const char *name)
{
    strlcpy(p, me.home, FS_PATH);
    strlcpy(p + strlen(p), "/.ssh", FS_PATH - strlen(p));
    fs_mkdir(p, 0700);
    strlcpy(p + strlen(p), name, FS_PATH - strlen(p));
}

static long ask(const char *prompt, char *buf, size_t size, int hidden)
{
    printf("%s", prompt);
    if (hidden) con_echo(0);
    long n = con_read(buf, size - 1);
    if (hidden) { con_echo(1); printf("\n"); }
    if (n < 0) n = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = 0;
    return n;
}

/* known_hosts: "[host]:port" (or "host" for port 22) ssh-ed25519 KEY. */
static void check_host(const char *host, int port)
{
    char name[96], path[FS_PATH], fp[52], line[200], yes[16];
    if (port == 22) strlcpy(name, host, sizeof name);
    else { name[0] = '['; strlcpy(name + 1, host, sizeof name - 8); ssh_fmt(name + strlen(name), 8, "]:%d", port); }
    path_in_home(path, "/known_hosts");
    char *t = file_get(path, 0);
    int k = t ? ssh_key_listed(t, name, s->hostkey) : 0;
    free(t);
    if (k == 1) return;
    ssh_fingerprint(fp, s->hostkey);
    if (k < 0) {
        printf("WARNING: THE HOST KEY OF %s HAS CHANGED.\n"
               "Someone may be listening in (a man-in-the-middle attack), or the host has a new key.\n"
               "Its key now: %s. If the change is expected, remove its line from %s.\n", name, fp, path);
        sys_exit(255);
    }
    printf("The authenticity of host '%s' can't be established.\nED25519 key fingerprint is %s.\n", name, fp);
    ask("Are you sure you want to continue connecting (yes/no)? ", yes, sizeof yes, 0);
    if (strcmp(yes, "yes")) fail(host, "host key not accepted");
    uint8_t blob[SSH_BLOB];
    ssh_blob(blob, s->hostkey);
    size_t n = strlcpy(line, name, sizeof line - 80);
    n += strlcpy(line + n, " ssh-ed25519 ", sizeof line - n);
    n += b64_enc(line + n, blob, SSH_BLOB);
    line[n++] = '\n';
    siefs_stat_t st;
    long h = fs_open(path, FS_WRONLY | FS_CREAT, 0644);
    if (h >= 0 && !fs_fstat(h, &st) && fs_write(h, st.size, line, n) == (long)n)
        printf("Warning: Permanently added '%s' (ED25519) to the list of known hosts.\n", name);
    if (h >= 0) fs_close(h);
}

/* The answer to an authentication request: SUCCESS or FAILURE (banners shown). */
static int auth_answer(int *password_ok)
{
    for (;;) {
        ssh_rd_t r;
        uint32_t n;
        int t = ssh_recv(s, &r, 0);
        if (t < 0) fail("authentication", "connection closed");
        if (t == SSH_USERAUTH_BANNER) { const uint8_t *m = ssh_str(&r, &n); con_write(m, n); continue; }
        if (t == SSH_USERAUTH_FAILURE) {
            const uint8_t *m = ssh_str(&r, &n);
            *password_ok = 0;
            for (uint32_t i = 0; i + 8 <= n; i++) if (!memcmp(m + i, "password", 8)) *password_ok = 1;
        }
        if (t == SSH_USERAUTH_SUCCESS || t == SSH_USERAUTH_FAILURE) return t;
    }
}

static void request(const char *user, const char *method)
{
    ssh_wr_t *w = ssh_begin(s, SSH_USERAUTH_REQUEST);
    ssh_puts(w, user);
    ssh_puts(w, "ssh-connection");
    ssh_puts(w, method);
}

static void authenticate(const char *user, const char *host, const char *keyfile)
{
    uint8_t seed[32], pub[32], blob[SSH_BLOB], sig[64], data[256];
    char path[FS_PATH], pw[128], prompt[160];
    ssh_rd_t r;
    int pw_ok = 1;
    ssh_puts(ssh_begin(s, SSH_SERVICE_REQUEST), "ssh-userauth");
    if (ssh_end(s) < 0 || ssh_recv(s, &r, 0) != SSH_SERVICE_ACCEPT) fail(host, "no authentication service");
    if (!keyfile) path_in_home(path, "/id_ed25519");
    else strlcpy(path, keyfile, sizeof path);
    int e = ssh_key_load(path, seed, pub);
    if (e == -EACCES) printf("ssh: %s: keys with a passphrase are not supported\n", path);
    else if (e && keyfile) printf("ssh: %s: not an Ed25519 key (OpenSSH format)\n", path);
    if (!e) {                                     /* publickey: the signature over session id + request */
        ssh_blob(blob, pub);
        ssh_wr_t d = { data, 0, sizeof data };
        ssh_putstr(&d, s->sid, 32);
        ssh_put8(&d, SSH_USERAUTH_REQUEST);
        ssh_puts(&d, user); ssh_puts(&d, "ssh-connection"); ssh_puts(&d, "publickey");
        ssh_put8(&d, 1);
        ssh_puts(&d, "ssh-ed25519");
        ssh_putstr(&d, blob, SSH_BLOB);
        ed25519_sign(sig, data, d.n, seed, pub);
        wipe(seed, sizeof seed);
        request(user, "publickey");
        ssh_put8(&s->w, 1);
        ssh_puts(&s->w, "ssh-ed25519");
        ssh_putstr(&s->w, blob, SSH_BLOB);
        ssh_put32(&s->w, 4 + 11 + 4 + 64);
        ssh_puts(&s->w, "ssh-ed25519");
        ssh_putstr(&s->w, sig, 64);
        ssh_end(s);
        if (auth_answer(&pw_ok) == SSH_USERAUTH_SUCCESS) return;
    }
    ssh_fmt(prompt, sizeof prompt, "%s@%s's password: ", user, host);
    for (int i = 0; i < 3 && pw_ok; i++) {
        if (i) printf("Permission denied, please try again.\n");
        ask(prompt, pw, sizeof pw, 1);
        request(user, "password");
        ssh_put8(&s->w, 0);
        ssh_puts(&s->w, pw);
        ssh_end(s);
        wipe(pw, sizeof pw);
        if (auth_answer(&pw_ok) == SSH_USERAUTH_SUCCESS) return;
    }
    printf("%s@%s: Permission denied.\n", user, host);
    sys_exit(255);
}

static void global_request(ssh_rd_t *r)        /* (keep-alives, host keys): none done */
{
    uint32_t n;
    ssh_str(r, &n);
    if (ssh_u8(r)) { ssh_begin(s, SSH_REQUEST_FAILURE); ssh_end(s); }
}

static void send_data(const char *b, size_t n)
{
    while (__atomic_load_n(&peer_win, __ATOMIC_ACQUIRE) < n && !closing) sys_sleep(10000000);
    __atomic_sub_fetch(&peer_win, n, __ATOMIC_ACQ_REL);
    ssh_wr_t *w = ssh_begin(s, SSH_CHANNEL_DATA);
    ssh_put32(w, peer_chan);
    ssh_putstr(w, b, n);
    ssh_end(s);
}

/* The second thread: keys, as typed, to the server. */
static void keys(void *arg)
{
    char b[256], o[260];
    int nl = 1, tilde = 0;
    (void)arg;
    for (;;) {
        long n = con_read(b, sizeof b);
        if (n <= 0 || closing) return;
        size_t k = 0;
        for (long i = 0; i < n; i++) {            /* "~." after Enter: disconnect */
            char c = b[i];
            if (tilde) {
                tilde = 0;
                if (c == '.') { closing = 1; ssh_disconnect(s, "disconnected by user"); net_close(s->sock); return; }
                if (c != '~') o[k++] = '~';
            } else if (nl && c == '~') { tilde = 1; continue; }
            o[k++] = c;
            nl = c == '\r';
        }
        if (k) send_data(o, k);
    }
}

int main(int ac, char **av)
{
    const char *keyfile = 0, *base = av[0] + strlen(av[0]);
    char host[128], user[32], cmd[512] = "", why_buf[160];
    int port = 22, i = 1;
    while (base > av[0] && base[-1] != '/') base--;
    mk_ident_t id;
    sys_ident(0, &id);
    if (acct_user(0, id.uid, &me)) { strlcpy(me.name, id.uid ? "nobody" : "root", sizeof me.name); strlcpy(me.home, "/", 2); }

    if (!strcmp(base, "ssh-keygen")) {            /* a new key pair */
        char path[FS_PATH], comment[96], hn[32] = "sieos";
        char *h = file_get("/etc/hostname", 0);
        if (h) { strlcpy(hn, h, sizeof hn); char *e = strchr(hn, '\n'); if (e) *e = 0; free(h); }
        ssh_fmt(comment, sizeof comment, "%s@%s", me.name, hn);
        path_in_home(path, "/id_ed25519");
        for (; i + 1 < ac && av[i][0] == '-'; i += 2)
            if (!strcmp(av[i], "-t") && strcmp(av[i + 1], "ed25519")) fail(av[i + 1], "only ed25519 keys");
            else if (!strcmp(av[i], "-f")) strlcpy(path, av[i + 1], sizeof path);
            else if (!strcmp(av[i], "-C")) strlcpy(comment, av[i + 1], sizeof comment);
        if (i < ac) { printf("usage: ssh-keygen [-t ed25519] [-f FILE] [-C COMMENT]\n"); return 1; }
        long e = ssh_key_new(path, comment);
        if (e == -EEXIST) fail(path, "already exists");
        if (e) fail(path, "cannot write it");
        char *pub = (strlcpy(cmd, path, sizeof cmd - 4), strlcpy(cmd + strlen(cmd), ".pub", 5), file_get(cmd, 0));
        printf("Your identification has been saved in %s\nYour public key has been saved in %s:\n%s", path, cmd, pub ? pub : "");
        free(pub);
        return 0;
    }

    for (; i + 1 < ac && av[i][0] == '-'; i += 2)
        if (!strcmp(av[i], "-p")) port = (int)strnum(av[i + 1]);
        else if (!strcmp(av[i], "-i")) keyfile = av[i + 1];
        else break;
    if (i >= ac || av[i][0] == '-' || port <= 0 || port > 65535) {
        printf("usage: ssh [-p PORT] [-i KEY] [USER@]HOST [COMMAND]\n"
               "       ssh-keygen [-t ed25519] [-f FILE] [-C COMMENT]\n");
        return 255;
    }
    char *at = strchr(av[i], '@');
    strlcpy(user, at ? av[i] : me.name, at && (size_t)(at - av[i]) < sizeof user ? (size_t)(at - av[i]) + 1 : sizeof user);
    strlcpy(host, at ? at + 1 : av[i], sizeof host);
    for (size_t n = 0; ++i < ac; ) {              /* the command: the remaining words */
        if (n) cmd[n++] = ' ';
        n += strlcpy(cmd + n, av[i], sizeof cmd - n);
        if (n >= sizeof cmd) fail("command", "too long");
    }

    uint32_t ip;
    long e = net_resolve(host, &ip, 0);
    if (e < 0) fail(host, net_strerror(e));
    long sock = net_connect(ip, (uint16_t)port, 0);
    if (sock < 0) fail(host, net_strerror(sock));
    if (!(s = malloc(sizeof *s))) fail(host, "out of memory");
    memset(s, 0, sizeof *s);
    s->sock = sock;
    const char *why;
    if (ssh_kex(s, 0, 0, &why)) fail(host, why);
    check_host(host, port);
    authenticate(user, host, keyfile);

    ssh_wr_t *w = ssh_begin(s, SSH_CHANNEL_OPEN);    /* one session channel */
    ssh_puts(w, "session");
    ssh_put32(w, 0); ssh_put32(w, WINDOW); ssh_put32(w, SSH_MAXDATA);
    ssh_end(s);
    ssh_rd_t r;
    uint32_t n, lwin = WINDOW;
    int t, status = 255, tty = !cmd[0];
    while ((t = ssh_recv(s, &r, 0)) == SSH_GLOBAL_REQUEST) global_request(&r);   /* (OpenSSH: its host keys) */
    if (t != SSH_CHANNEL_OPEN_CONFIRMATION) fail(host, "the server refused a session");
    ssh_u32(&r);
    peer_chan = ssh_u32(&r);
    peer_win = ssh_u32(&r);
    if (tty) {                                    /* a terminal, 80 x 24 (no other size known), no modes */
        w = ssh_begin(s, SSH_CHANNEL_REQUEST);
        ssh_put32(w, peer_chan); ssh_puts(w, "pty-req"); ssh_put8(w, 0);
        ssh_puts(w, "vt100");
        ssh_put32(w, 80); ssh_put32(w, 24); ssh_put32(w, 0); ssh_put32(w, 0);
        ssh_putstr(w, "", 1);
        ssh_end(s);
    }
    w = ssh_begin(s, SSH_CHANNEL_REQUEST);
    ssh_put32(w, peer_chan); ssh_puts(w, tty ? "shell" : "exec"); ssh_put8(w, 1);
    if (!tty) ssh_puts(w, cmd);
    ssh_end(s);
    if (tty) { con_echo(CON_RAW); thread_start(keys, 0, 16384); }
    else { w = ssh_begin(s, SSH_CHANNEL_EOF); ssh_put32(w, peer_chan); ssh_end(s); }   /* no input */

    for (int open = 1; open; ) {
        t = ssh_recv(s, &r, 0);
        const uint8_t *d;
        switch (t) {
        case SSH_CHANNEL_DATA:
        case SSH_CHANNEL_EXTENDED_DATA:           /* (stderr: shown the same) */
            ssh_u32(&r);
            if (t == SSH_CHANNEL_EXTENDED_DATA) ssh_u32(&r);
            d = ssh_str(&r, &n);
            con_write(d, n);
            lwin -= n < lwin ? n : lwin;
            if (lwin < WINDOW / 2) {
                w = ssh_begin(s, SSH_CHANNEL_WINDOW_ADJUST);
                ssh_put32(w, peer_chan); ssh_put32(w, WINDOW - lwin);
                ssh_end(s);
                lwin = WINDOW;
            }
            break;
        case SSH_CHANNEL_WINDOW_ADJUST:
            ssh_u32(&r);
            __atomic_add_fetch(&peer_win, ssh_u32(&r), __ATOMIC_ACQ_REL);
            break;
        case SSH_CHANNEL_REQUEST:                 /* exit-status: the command's; others: not done */
            ssh_u32(&r);
            if (ssh_is(&r, "exit-status")) { ssh_u8(&r); status = (int)ssh_u32(&r); break; }
            if (ssh_u8(&r)) { w = ssh_begin(s, SSH_CHANNEL_FAILURE); ssh_put32(w, peer_chan); ssh_end(s); }
            break;
        case SSH_GLOBAL_REQUEST:
            global_request(&r);
            break;
        case SSH_CHANNEL_FAILURE:
            printf("ssh: %s refused to run the %s\n", host, tty ? "shell" : "command");
            open = 0;
            break;
        case SSH_CHANNEL_CLOSE:
            w = ssh_begin(s, SSH_CHANNEL_CLOSE); ssh_put32(w, peer_chan); ssh_end(s);
            open = 0;
            break;
        case SSH_KEXINIT:                         /* new keys (OpenSSH: after 1 GiB): not done */
            ssh_disconnect(s, "re-keying is not supported");
            printf("ssh: %s asked for new keys: not supported, disconnected\n", host);
            open = 0;
            break;
        case -1:
            open = 0;
            break;
        }
    }
    closing = 1;
    if (tty) {
        con_echo(1);                              /* (also ends the keys thread's read) */
        ssh_fmt(why_buf, sizeof why_buf, "\r\nConnection to %s closed.\r\n", host);
        con_write(why_buf, strlen(why_buf));
    }
    net_close(s->sock);
    return status;
}
