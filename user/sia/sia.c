/*
 * sia - the SIEOS terminal harness, built on libsia.
 *
 * Lines are commands: they run directly with /bin/sh -c, attached to the
 * terminal, without involving the model.  cd, export and unset change the
 * harness itself.
 *
 *   /sia REQUEST   ask the model (it can run commands and, inside Facet,
 *                  open applications and manage windows)
 *
 * When a command fails (not found, or an error message with a non-zero exit
 * status), the command and its error go to the model: it runs the fix if
 * there is one clear correction, otherwise it explains the problem.
 *
 * First use asks for the Azure AI Foundry endpoint, model and API key
 * (~/.sia/config).  Without a model, the terminal starts /bin/sh instead.
 *
 *   sia            start the harness (the Facet terminal runs this)
 *   sia --setup    (re)configure the model connection
 *   sia --off      unregister the model: terminals start the plain shell
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "libsia.h"

#define ERR_KEEP 4000                /* bytes of a failed command's error output sent to the model */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static struct sia_session *S;
static struct sia_config cfg;
static char home[128], user[64], host[64];
static bool thinking_shown;

/* ---------------- terminal helpers ---------------- */

static void out(const char *s)
{
    write(1, s, strlen(s));
}

static bool read_line(const char *prompt, char *buf, size_t n)
{
    out(prompt);
    int r = read_line_fd(0, buf, n);
    if (r < 0)
        return false;
    size_t len = strlen(buf);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' '))
        buf[--len] = 0;
    return true;
}

/* ---- the prompt's line editor: history (Up, Down, kept in ~/.sia/history),
 * Left, Right, Home/End (Ctrl-A, Ctrl-E), Backspace, Delete, Ctrl-U and
 * Ctrl-K (delete before, after the cursor), Ctrl-L (clear), Ctrl-C (drop the
 * line), Ctrl-D (end, on an empty line).  The terminal is raw while it runs,
 * so nothing typed is echoed by the kernel: an arrow never moves the cursor. */

#define HIST 200
static char *hist[HIST];
static int nhist;
static bool hist_loaded;

static void hist_push(const char *l)
{
    if (nhist && !strcmp(hist[nhist - 1], l))
        return;                                  /* (the same line again: once) */
    if (nhist == HIST) {
        free(hist[0]);
        memmove(hist, hist + 1, (HIST - 1) * sizeof(char *));
        nhist--;
    }
    hist[nhist++] = strdup(l);
}

static void hist_path(char *path, size_t n)
{
    snprintf(path, n, "%s/.sia/history", home);
}

static void hist_load(void)
{
    hist_loaded = true;
    char path[256], l[1024];
    hist_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(l, sizeof(l), f)) {
        l[strcspn(l, "\n")] = 0;
        if (l[0])
            hist_push(l);
    }
    fclose(f);
}

static void hist_add(const char *l)
{
    if (!l[0] || (nhist && !strcmp(hist[nhist - 1], l)))
        return;
    hist_push(l);
    char path[256];
    hist_path(path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        dprintf(fd, "%s\n", l);
        close(fd);
    }
}

static void redraw(const char *prompt, const char *buf, size_t len, size_t cur)
{
    char mv[32];
    out("\r");
    out(prompt);
    write(1, buf, len);
    out("\033[K");
    if (len > cur) {
        snprintf(mv, sizeof(mv), "\033[%zuD", len - cur);
        out(mv);
    }
}

static bool edit_line(const char *prompt, char *buf, size_t n)
{
    if (!isatty(0))
        return read_line(prompt, buf, n);
    if (!hist_loaded)
        hist_load();
    struct termios old, raw;
    tcgetattr(0, &old);
    raw = old;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
    size_t len = 0, cur = 0;
    int hpos = nhist;
    char saved[1024] = "";                       /* the line being typed, while browsing history */
    bool ok = false;
    buf[0] = 0;
    out(prompt);
    for (;;) {
        unsigned char c;
        ssize_t r = read(0, &c, 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            errno = 0;
            break;
        }
        int key = c;
        if (c == 27) {                           /* an escape sequence: the arrows, Home, End, Delete */
            unsigned char a = 0, b = 0;
            if (read(0, &a, 1) != 1 || (a != '[' && a != 'O') || read(0, &b, 1) != 1)
                continue;
            if (b >= '0' && b <= '9') {
                unsigned char t = 0;
                if (read(0, &t, 1) != 1 || t != '~')
                    continue;
                key = b == '1' || b == '7' ? 1 : b == '4' || b == '8' ? 5 : b == '3' ? 0x7F00 : 0;
            } else {
                key = b == 'A' ? 0x4100 : b == 'B' ? 0x4200 : b == 'C' ? 6 : b == 'D' ? 2 : b == 'H' ? 1 : b == 'F' ? 5 : 0;
            }
        }
        if (key == '\r' || key == '\n') {
            out("\n");
            buf[len] = 0;
            ok = true;
            break;
        }
        if (key == 3) {                          /* Ctrl-C: the caller starts a new line */
            out("^C");
            errno = EINTR;
            break;
        }
        if (key == 4 && !len) {                  /* Ctrl-D on an empty line: the end */
            errno = 0;
            break;
        }
        if (key == 0x4100 || key == 0x4200) {    /* Up, Down: history */
            if (key == 0x4100 && hpos > 0) {
                if (hpos == nhist)
                    snprintf(saved, sizeof(saved), "%.*s", (int)len, buf);
                hpos--;
                snprintf(buf, n, "%s", hist[hpos]);
            } else if (key == 0x4200 && hpos < nhist) {
                hpos++;
                snprintf(buf, n, "%s", hpos == nhist ? saved : hist[hpos]);
            } else {
                continue;
            }
            len = cur = strlen(buf);
        } else if (key == 2) {                   /* Left, Ctrl-B */
            if (!cur)
                continue;
            cur--;
        } else if (key == 6) {                   /* Right, Ctrl-F */
            if (cur == len)
                continue;
            cur++;
        } else if (key == 1) {                   /* Home, Ctrl-A */
            cur = 0;
        } else if (key == 5) {                   /* End, Ctrl-E */
            cur = len;
        } else if (key == 127 || key == 8) {     /* Backspace */
            if (!cur)
                continue;
            memmove(buf + cur - 1, buf + cur, len - cur);
            cur--, len--;
        } else if (key == 0x7F00 || key == 4) {  /* Delete, Ctrl-D */
            if (cur == len)
                continue;
            memmove(buf + cur, buf + cur + 1, len - cur - 1);
            len--;
        } else if (key == 21) {                  /* Ctrl-U: before the cursor */
            memmove(buf, buf + cur, len - cur);
            len -= cur;
            cur = 0;
        } else if (key == 11) {                  /* Ctrl-K: after the cursor */
            len = cur;
        } else if (key == 12) {                  /* Ctrl-L: clear the screen */
            out("\033[H\033[J");
        } else if (key >= 32 && key < 256 && len + 1 < n) {
            memmove(buf + cur + 1, buf + cur, len - cur);
            buf[cur++] = (char)key;
            len++;
        } else {
            continue;
        }
        buf[len] = 0;
        redraw(prompt, buf, len, cur);
    }
    tcsetattr(0, TCSANOW, &old);
    buf[len] = 0;
    if (!ok)
        return false;
    while (len && (buf[len - 1] == ' '))
        buf[--len] = 0;
    return true;
}

static bool read_secret(const char *prompt, char *buf, size_t n)
{
    struct termios old, t;
    bool tty = tcgetattr(0, &old) == 0;
    if (tty) {
        t = old;
        t.c_lflag &= ~ECHO;
        t.c_lflag |= ECHONL;
        tcsetattr(0, TCSAFLUSH, &t);
    }
    bool ok = read_line(prompt, buf, n);
    if (tty)
        tcsetattr(0, TCSANOW, &old);
    return ok;
}

/* ---------------- libsia callbacks ---------------- */

static void io_text(void *ctx, const char *utf8)
{
    (void)ctx;
    struct sbuf b;
    sb_init(&b);
    sia_plain_text(utf8, &b);
    if (b.len && b.s[b.len - 1] != '\n')
        sb_putc(&b, '\n');
    out("\033[33m");
    write(1, b.s, b.len);
    out("\033[0m");
    sb_free(&b);
}

/* The reply as it is written; NULL ends it. */
static void io_delta(void *ctx, const char *utf8)
{
    (void)ctx;
    static bool at_line_start = true, coloured;
    if (!utf8) {
        if (coloured)
            out("\033[0m");
        if (!at_line_start)
            out("\n");
        at_line_start = true;
        coloured = false;
        return;
    }
    struct sbuf b;
    sb_init(&b);
    sia_plain_text(utf8, &b);
    if (b.len) {
        if (!coloured)
            out("\033[33m");                           /* one colour for the whole reply */
        coloured = true;
        write(1, b.s, b.len);
        at_line_start = b.s[b.len - 1] == '\n';
    }
    sb_free(&b);
}

static void io_tool(void *ctx, const char *cmdline)
{
    (void)ctx;
    printf("\033[1;34m$ %s\033[0m\n", cmdline);
}

static void io_output(void *ctx, const char *buf, size_t n)
{
    (void)ctx;
    write(1, buf, n);
}

static int io_confirm(void *ctx, const char *cmdline)
{
    (void)ctx;
    printf("\033[33mRun '%s'? [Y/n/a=always] \033[0m", cmdline);
    char ans[16];
    if (read_line_fd(0, ans, sizeof(ans)) < 0)
        return 0;
    if (ans[0] == 'a' || ans[0] == 'A')
        return 2;
    if (ans[0] == 'n' || ans[0] == 'N') {
        out("(skipped)\n");
        return 0;
    }
    return 1;
}

static void io_thinking(void *ctx, bool on)
{
    (void)ctx;
    if (on && !thinking_shown)
        out("\033[90m...\033[0m");
    else if (!on && thinking_shown)
        out("\r\033[K");
    thinking_shown = on;
}

static void io_error(void *ctx, const char *msg)
{
    (void)ctx;
    if (!strcmp(msg, "interrupted"))
        out("[interrupted]\n");
    else
        printf("\033[31msia: %s\033[0m\n", msg);
}

static const struct sia_io term_io = { NULL, io_text, io_tool, io_output, io_confirm, io_thinking, io_error, io_delta, NULL };

/* ---------------- setup ---------------- */

static void start_shell(const char *why) __attribute__((noreturn));
static void start_shell(const char *why)
{
    if (why)
        printf("%s\n", why);
    char *argv[] = { "sh", NULL };
    execv("/bin/sh", argv);
    printf("sia: cannot start /bin/sh\n");
    _exit(1);
}

static bool open_session(const struct sia_config *c, bool announce, char *err, size_t errlen)
{
    struct sia_session *n = sia_session_new(c, SIA_ROLE_TERMINAL, &term_io, err, errlen);
    if (!n)
        return false;
    if (announce)
        printf("Connecting to %s...\n", sia_endpoint_kind(n));
    else
        out("\033[90mConnecting to the model...\033[0m");
    bool ok = sia_ping(n, err, errlen);
    if (!announce)
        out("\r\033[K");
    if (!ok) {
        sia_session_free(n);
        return false;
    }
    if (S)
        sia_session_free(S);
    S = n;
    sia_add_command_tools(S);
    sia_add_desktop_tools(S);
    return true;
}

/* Interactive setup; returns true if a model was registered and works. */
static bool setup(void)
{
    out("\n\033[1;33mConnect SIEOS to an Azure AI Foundry model\033[0m\n"
        "The terminal can use a model to carry out tasks you describe (/sia ...) and to fix\n"
        "commands that fail. You need, from the Azure AI Foundry portal:\n"
        "  - the endpoint, e.g. https://NAME.openai.azure.com/ or https://NAME.services.ai.azure.com/\n"
        "  - the model or deployment name, e.g. gpt-4o\n"
        "  - an API key\n"
        "Leave the endpoint empty to skip; terminals will then open the standard shell\n"
        "(run 'sia --setup' later to connect a model).\n\n");
    for (;;) {
        struct sia_config n;
        memset(&n, 0, sizeof(n));
        if (!read_line("Endpoint: ", n.endpoint, sizeof(n.endpoint)))
            return false;
        if (!n.endpoint[0]) {
            memset(&cfg, 0, sizeof(cfg));
            sia_config_save(&cfg, NULL, 0);
            out("No model registered.\n");
            return false;
        }
        if (strncmp(n.endpoint, "https://", 8) && strncmp(n.endpoint, "http://", 7)) {
            char fixed[sizeof(n.endpoint)];
            snprintf(fixed, sizeof(fixed), "https://%s", n.endpoint);
            snprintf(n.endpoint, sizeof(n.endpoint), "%s", fixed);
        }
        if (!strncmp(n.endpoint, "http://", 7))
            out("\033[33mWarning: http:// sends the API key unencrypted.\033[0m\n");
        if (!read_line("Model / deployment name: ", n.model, sizeof(n.model)) || !n.model[0])
            continue;
        if (!read_secret("API key (hidden): ", n.api_key, sizeof(n.api_key)) || !n.api_key[0])
            continue;
        char err[400];
        if (open_session(&n, true, err, sizeof(err))) {
            cfg = n;
            if (sia_config_save(&cfg, err, sizeof(err)))
                printf("\033[32mConnected.\033[0m Settings saved in %s\n\n", sia_config_path());
            else
                printf("sia: %s\n", err);
            return true;
        }
        printf("\033[31mConnection failed: %s\033[0m\n", err);
        char ans[16];
        if (!read_line("Try again? [Y/n] ", ans, sizeof(ans)) || ans[0] == 'n' || ans[0] == 'N') {
            memset(&cfg, 0, sizeof(cfg));
            sia_config_save(&cfg, NULL, 0);
            return false;
        }
    }
}

/* ---------------- direct commands ---------------- */

static void on_sigint(int sig)
{
    (void)sig;
    sia_interrupt();
}

/* Run a command line on the terminal; stderr is also kept for the model. */
static int run_direct(const char *line, struct sbuf *errout, bool *signaled)
{
    int pe[2];
    if (pipe(pe) < 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pe[1], 2);
        for (int fd = 3; fd < 64; fd++)
            close(fd);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        char *argv[] = { "sh", "-c", (char *)line, NULL };
        execv("/bin/sh", argv);
        _exit(127);
    }
    close(pe[1]);
    char buf[512];
    long n;
    while ((n = read(pe[0], buf, sizeof(buf))) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        write(2, buf, n);
        if (errout->len < ERR_KEEP)
            sb_putn(errout, buf, MIN((size_t)n, ERR_KEEP - errout->len));
    }
    close(pe[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    /* a program may have moved the terminal to another process group */
    signal(SIGTTOU, SIG_IGN);
    pid_t me = getpgrp();
    ioctl(0, TIOCSPGRP, &me);
    signal(SIGTTOU, SIG_DFL);
    *signaled = !WIFEXITED(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* A failed command goes to the model for a fix or an explanation. */
static void fix_command(const char *line, int status, const struct sbuf *errout)
{
    out("\033[90msia: that did not work; asking the model...\033[0m\n");
    struct sbuf req;
    sb_init(&req);
    sb_printf(&req, "The user typed this command in the terminal:\n  %s\n", line);
    sb_printf(&req, "It failed with exit status %d", status);
    if (errout->len) {
        sb_puts(&req, " and this error output:\n");
        sb_putn(&req, errout->s, errout->len);
    } else {
        sb_puts(&req, " and printed no error message.\n");
    }
    sb_puts(&req,
            "\nIf there is one clear correction (a typo in the command name, a wrong or unsupported option, a "
            "wrong path), run the corrected command with the tools and say in one line what you changed. If it "
            "cannot be fixed automatically, or the intent is ambiguous and it could be corrected in several "
            "ways, do not run anything: explain the problem in one or two lines and list the likely "
            "alternatives.");
    sia_ask(S, req.s);
    sb_free(&req);
}

static bool builtin(char *l)
{
    if (!strncmp(l, "cd", 2) && (l[2] == 0 || l[2] == ' ')) {
        char *p = l + 2;
        while (*p == ' ')
            p++;
        if (!*p)
            p = home;
        if (chdir(p) < 0)
            printf("cd: %s: %s\n", p, strerror(errno));
        return true;
    }
    if (!strncmp(l, "export ", 7) || !strncmp(l, "unset ", 6)) {
        bool set = l[0] == 'e';
        char *p = l + (set ? 7 : 6);
        while (*p == ' ')
            p++;
        char *eq = strchr(p, '=');
        if (set && eq) {
            *eq = 0;
            setenv(p, eq + 1, 1);
        } else if (!set) {
            unsetenv(p);
        } else {
            printf("usage: export NAME=VALUE\n");
        }
        return true;
    }
    return false;
}

static void run_shell(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        char *argv[] = { "sh", NULL };
        execv("/bin/sh", argv);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    signal(SIGTTOU, SIG_IGN);
    pid_t me = getpgrp();
    ioctl(0, TIOCSPGRP, &me);
    signal(SIGTTOU, SIG_DFL);
}

static void help(void)
{
    out("Lines are commands and run directly (pipes, redirections and variables work).\n"
        "  /sia REQUEST   ask the model, e.g. /sia which process uses the most memory?\n"
        "                 (inside Facet it can also open apps: /sia open a terminal)\n"
        "  A command that fails is sent to the model, which fixes it or explains why.\n"
        "  cd, export NAME=VALUE, unset NAME   change this terminal's directory / environment\n"
        "  /shell      start the standard shell (with job control; exit returns here)\n"
        "  /clear      forget the conversation     /auto on|off  run changes without asking\n"
        "  /model      show the model connection   /setup        change it\n"
        "  /tools      list what the model can use\n"
        "  exit        close the terminal\n"
        "Ctrl-C interrupts the running command or the model.\n");
}

int main(int argc, char **argv)
{
    struct passwd *pw = getpwuid(getuid());
    snprintf(home, sizeof(home), "%s", getenv("HOME") ? getenv("HOME") : pw ? pw->pw_dir : "/");
    snprintf(user, sizeof(user), "%s", pw ? pw->pw_name : "user");
    struct utsname u;
    if (uname(&u) == 0)
        snprintf(host, sizeof(host), "%s", u.nodename);

    bool want_setup = argc > 1 && !strcmp(argv[1], "--setup");
    if (argc > 1 && !strcmp(argv[1], "--off")) {
        memset(&cfg, 0, sizeof(cfg));
        sia_config_save(&cfg, NULL, 0);
        out("sia: model unregistered; terminals will start the standard shell.\n");
        return 0;
    }
    if (argc > 1 && !want_setup) {
        out("usage: sia [--setup | --off]\n");
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;                       /* no SA_RESTART: interrupts network waits */
    sigaction(SIGINT, &sa, NULL);

    int r = sia_config_load(&cfg);
    if (want_setup || r < 0) {
        if (!setup())
            start_shell(NULL);
    } else if (r == 0) {
        start_shell(NULL);
    } else {
        char err[512];
        if (!open_session(&cfg, false, err, sizeof(err))) {
            printf("\033[31msia: cannot use the model: %s\033[0m\n", err);
            start_shell("Starting the standard shell. Run 'sia' to try again or 'sia --setup' to change "
                        "the settings.");
        }
    }
    sia_status_write("idle", "");
    printf("\033[1;33msia\033[0m - SIEOS terminal with \033[1m%s\033[0m. Commands run directly; "
           "/sia REQUEST asks the model. /help\n", sia_model_name(S));

    char line[2048];
    for (;;) {
        char cwd[256] = "?", prompt[400];
        getcwd(cwd, sizeof(cwd));
        size_t hl = strlen(home);
        const char *shown = cwd;
        char tilde[260];
        if (hl > 1 && !strncmp(cwd, home, hl) && (cwd[hl] == 0 || cwd[hl] == '/')) {
            snprintf(tilde, sizeof(tilde), "~%s", cwd + hl);
            shown = tilde;
        }
        snprintf(prompt, sizeof(prompt), "\033[33msia\033[0m \033[1;32m%s@%s\033[0m:\033[1;34m%s\033[0m%s ", user,
                 host, shown, getuid() == 0 ? "#" : "$");
        if (!edit_line(prompt, line, sizeof(line))) {
            if (errno == EINTR) {                    /* Ctrl-C at the prompt */
                out("\n");
                continue;
            }
            out("\n");
            break;
        }
        char *l = line;
        while (*l == ' ')
            l++;
        if (!*l)
            continue;
        hist_add(l);
        if (!strcmp(l, "exit") || !strcmp(l, "logout"))
            break;
        if (!strncmp(l, "/sia", 4) && (l[4] == 0 || l[4] == ' ')) {
            char *q = l + 4;
            while (*q == ' ')
                q++;
            if (*q)
                sia_ask(S, q);
            else
                out("usage: /sia REQUEST   e.g. /sia show the three largest files in /bin\n");
        } else if (!strcmp(l, "/help") || !strcmp(l, "help")) {
            help();
        } else if (!strcmp(l, "/shell")) {
            run_shell();
        } else if (!strcmp(l, "/clear")) {
            sia_clear(S);
            out("Conversation cleared.\n");
        } else if (!strncmp(l, "/auto", 5)) {
            if (strstr(l, "on"))
                cfg.auto_approve = true;
            else if (strstr(l, "off"))
                cfg.auto_approve = false;
            sia_set_auto_approve(S, cfg.auto_approve);
            sia_config_save(&cfg, NULL, 0);
            printf("Changing commands %s.\n", cfg.auto_approve ? "run without asking" : "ask for confirmation");
        } else if (!strcmp(l, "/model")) {
            char url[1200];
            sia_request_url(S, url, sizeof(url));
            printf("model %s (%s)\n%s\n", sia_model_name(S), sia_endpoint_kind(S), url);
        } else if (!strcmp(l, "/setup")) {
            if (setup())
                sia_clear(S);
        } else if (!strcmp(l, "/tools")) {
            for (int i = 0; i < sia_tool_count(S); i++)
                printf("%-17s%s", sia_tool_at(S, i)->name, (i % 4 == 3) ? "\n" : "");
            out("\n");
        } else if (l[0] == '/' && !strchr(l, ' ') && access(l, X_OK) != 0 && l[1] && strchr("abcdefghijklmnopqrstuvwxyz", l[1]) &&
                   !strchr(l + 1, '/')) {
            printf("sia: unknown command %s (/help lists them)\n", l);
        } else if (!builtin(l)) {
            struct sbuf errout;
            sb_init(&errout);
            bool signaled = false;
            int status = run_direct(l, &errout, &signaled);
            bool failed = !signaled && status != 0 && (status == 127 || status == 126 || errout.len > 0);
            if (failed)
                fix_command(l, status, &errout);
            sb_free(&errout);
        }
    }
    return 0;
}
