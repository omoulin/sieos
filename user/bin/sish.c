/*
 * sh - the SIEOS shell, with job control.
 *
 * Syntax:  cmd args  [< in] [> out | >> out] [2> err | 2>&1]
 *          cmd1 | cmd2 | ...        pipelines
 *          list ; list   list && list   list || list   list &
 *          'single' "double" \escape  $? $$ $USER $HOME $PWD ~  # comment
 *          NAME=value   NAME=value cmd   $NAME ${NAME}
 * Built-ins: cd pwd exit logout help jobs fg bg wait kill umask
 *            export unset set
 *
 * Job control follows the POSIX model: every pipeline runs in its own
 * process group; the foreground job owns the terminal (tcsetpgrp); ^Z
 * stops it, 'fg' / 'bg' continue it.
 */
#include "sieos.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAXLINE  1024
#define MAXARGV  32
#define MAXCMDS  8
#define MAXJOBS  16

/* ------------------------------------------------------------------ */
/* Shell state                                                         */
/* ------------------------------------------------------------------ */

static int last_status;
static int in_fd = STDIN_FILENO;
static bool interactive;           /* reading commands from a terminal */
static bool job_control;           /* we own the terminal */
static pid_t shell_pgid;
static struct termios shell_tmodes;
static bool warned_stopped;
static volatile int got_sigint;

static void on_sigint(int sig)
{
    (void)sig;
    got_sigint = 1;
}

struct command {
    char *argv[MAXARGV + 1];
    int argc;
    char *assigns[8];              /* NAME=value prefixes */
    int nassigns;
    char *in, *out, *err;
    bool append, err_to_out;
};

struct pipeline {
    struct command cmds[MAXCMDS];
    int ncmds;
    bool background;
    char text[128];
};

enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE };

struct job {
    int id;                        /* 0 = free slot */
    pid_t pgid;
    int npids;
    pid_t pids[MAXCMDS];
    int status[MAXCMDS];
    bool done[MAXCMDS], stopped[MAXCMDS];
    int state;
    bool notified;
    bool background;
    char text[128];
};

static struct job jobs[MAXJOBS];
static int current_job, previous_job;   /* for %+ / %- */

/* ------------------------------------------------------------------ */
/* Variables                                                           */
/* ------------------------------------------------------------------ */

#define MAXVARS 128

struct var {
    char *name;
    char *value;
    bool exported;
};

static struct var vars[MAXVARS];
static int nvars;

static bool valid_name(const char *s, size_t len)
{
    if (!len || isdigit(s[0]))
        return false;
    for (size_t i = 0; i < len; i++)
        if (!isalnum(s[i]) && s[i] != '_')
            return false;
    return true;
}

static struct var *var_find(const char *name)
{
    for (int i = 0; i < nvars; i++)
        if (!strcmp(vars[i].name, name))
            return &vars[i];
    return NULL;
}

static const char *var_get(const char *name)
{
    struct var *v = var_find(name);
    return v ? v->value : NULL;
}

static void var_set(const char *name, const char *value, bool export)
{
    struct var *v = var_find(name);
    if (!v) {
        if (nvars == MAXVARS) {
            dprintf(STDERR_FILENO, "sh: too many variables\n");
            return;
        }
        v = &vars[nvars++];
        v->name = strdup(name);
        v->value = NULL;
        v->exported = false;
    }
    if (value) {
        char *copy = strdup(value);
        free(v->value);
        v->value = copy;
    }
    if (export)
        v->exported = true;
}

static void var_unset(const char *name)
{
    for (int i = 0; i < nvars; i++) {
        if (!strcmp(vars[i].name, name)) {
            free(vars[i].name);
            free(vars[i].value);
            vars[i] = vars[--nvars];
            return;
        }
    }
}

/* Parse "NAME=value" and assign it; returns false if s is not an assignment. */
static bool assign(const char *s, bool export)
{
    const char *eq = strchr(s, '=');
    if (!eq || !valid_name(s, eq - s))
        return false;
    char name[64];
    size_t n = MIN((size_t)(eq - s), sizeof(name) - 1);
    memcpy(name, s, n);
    name[n] = 0;
    var_set(name, eq + 1, export);
    return true;
}

static void vars_from_environ(void)
{
    for (int i = 0; environ && environ[i]; i++)
        assign(environ[i], true);
}

/* Build an environment vector from exported variables. */
static char **build_envp(void)
{
    static char *envp[MAXVARS + 1];
    static char *storage[MAXVARS];
    int n = 0;
    for (int i = 0; i < nvars; i++) {
        if (!vars[i].exported || !vars[i].value)
            continue;
        free(storage[n]);
        size_t len = strlen(vars[i].name) + strlen(vars[i].value) + 2;
        storage[n] = malloc(len);
        snprintf(storage[n], len, "%s=%s", vars[i].name, vars[i].value);
        envp[n] = storage[n];
        n++;
    }
    envp[n] = NULL;
    return envp;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static char inbuf[512];
static int inpos, inlen;

#define INTERRUPTED (-2)

static int readc(void)
{
    if (inpos == inlen) {
        long n = read(in_fd, inbuf, sizeof(inbuf));
        if (n < 0 && errno == EINTR) {
            if (got_sigint) {
                got_sigint = 0;
                return INTERRUPTED;
            }
            return readc();
        }
        if (n <= 0)
            return EOF;
        inpos = 0;
        inlen = n;
    }
    return (unsigned char)inbuf[inpos++];
}

static int read_line(char *buf, size_t size)
{
    size_t n = 0;
    int c;
    while ((c = readc()) != EOF) {
        if (c == INTERRUPTED)
            return INTERRUPTED;
        if (c == '\n')
            break;
        if (n + 1 < size)
            buf[n++] = c;
    }
    buf[n] = 0;
    return (c == EOF && n == 0) ? -1 : (int)n;
}

static const char *home_dir(void)
{
    static char home[128];
    const char *h = var_get("HOME");
    if (h && *h)
        return h;
    struct passwd *pw = getpwuid(getuid());
    strlcpy(home, pw ? pw->pw_dir : "/", sizeof(home));
    return home;
}

static const char *user_name(void)
{
    static char name[32];
    struct passwd *pw = getpwuid(geteuid());
    if (pw)
        strlcpy(name, pw->pw_name, sizeof(name));
    else
        snprintf(name, sizeof(name), "%d", geteuid());
    return name;
}

static void prompt(void)
{
    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "?");
    const char *home = home_dir();
    size_t hl = strlen(home);
    char shown[256];
    if (hl > 1 && strncmp(cwd, home, hl) == 0 && (cwd[hl] == 0 || cwd[hl] == '/'))
        snprintf(shown, sizeof(shown), "~%s", cwd + hl);
    else
        strlcpy(shown, cwd, sizeof(shown));
    bool root = geteuid() == 0;
    printf("\033[1;%dm%s@sieos\033[0m:\033[1;34m%s\033[0m%c ", root ? 31 : 32, user_name(), shown,
           root ? '#' : '$');
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

enum { T_END, T_WORD, T_PIPE, T_AMP, T_SEMI, T_AND, T_OR, T_LT, T_GT, T_GTGT, T_ERR, T_ERR2OUT };

struct lexer {
    const char *p;
    char *w, *wend;                /* word storage */
    bool error;
};

static void put_str(struct lexer *lx, const char *s)
{
    while (*s && lx->w < lx->wend)
        *lx->w++ = *s++;
}

static bool expand_var(struct lexer *lx)
{
    const char *p = lx->p + 1;
    char num[16], name[32];
    if (*p == '?' || *p == '$') {
        snprintf(num, sizeof(num), "%d", *p == '?' ? last_status : getpid());
        put_str(lx, num);
        lx->p = p + 1;
        return true;
    }
    size_t n = 0;
    bool brace = *p == '{';
    if (brace)
        p++;
    while ((isalnum(*p) || *p == '_') && n + 1 < sizeof(name))
        name[n++] = *p++;
    name[n] = 0;
    if (brace && *p == '}')
        p++;
    if (!n)
        return false;
    const char *v = var_get(name);
    if (v)
        put_str(lx, v);
    lx->p = p;
    return true;
}

static int next_token(struct lexer *lx, char **word)
{
    const char *p = lx->p;
    while (*p == ' ' || *p == '\t')
        p++;
    lx->p = p;
    if (!*p || *p == '#')
        return T_END;
    if (p[0] == '|' && p[1] == '|') { lx->p += 2; return T_OR; }
    if (p[0] == '&' && p[1] == '&') { lx->p += 2; return T_AND; }
    if (*p == '|') { lx->p++; return T_PIPE; }
    if (*p == '&') { lx->p++; return T_AMP; }
    if (*p == ';') { lx->p++; return T_SEMI; }
    if (*p == '<') { lx->p++; return T_LT; }
    if (p[0] == '>' && p[1] == '>') { lx->p += 2; return T_GTGT; }
    if (*p == '>') { lx->p++; return T_GT; }
    if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1') { lx->p += 4; return T_ERR2OUT; }
    if (p[0] == '2' && p[1] == '>') { lx->p += 2; return T_ERR; }

    *word = lx->w;
    char quote = 0;
    if (*p == '~' && (p[1] == 0 || strchr("/ \t;&|<>", p[1]))) {
        put_str(lx, home_dir());
        lx->p = ++p;
    }
    while (*lx->p && lx->w < lx->wend) {
        char ch = *lx->p;
        if (quote) {
            if (ch == quote) {
                quote = 0;
                lx->p++;
                continue;
            }
            if (quote == '"' && ch == '\\' && lx->p[1]) {
                *lx->w++ = lx->p[1];
                lx->p += 2;
                continue;
            }
            if (quote == '"' && ch == '$' && expand_var(lx))
                continue;
        } else {
            if (strchr(" \t;&|<>", ch))
                break;
            if (ch == '\'' || ch == '"') {
                quote = ch;
                lx->p++;
                continue;
            }
            if (ch == '\\' && lx->p[1]) {
                *lx->w++ = lx->p[1];
                lx->p += 2;
                continue;
            }
            if (ch == '$' && expand_var(lx))
                continue;
        }
        *lx->w++ = ch;
        lx->p++;
    }
    if (quote) {
        dprintf(STDERR_FILENO, "sh: unterminated quote\n");
        lx->error = true;
    }
    *lx->w++ = 0;
    return T_WORD;
}

/*
 * Parse one pipeline.  Returns the token that terminated it
 * (T_END, T_SEMI, T_AMP, T_AND, T_OR) or -1 on a syntax error.
 */
static int parse_pipeline(struct lexer *lx, struct pipeline *pl)
{
    memset(pl, 0, sizeof(*pl));
    const char *start = lx->p;
    while (*start == ' ')
        start++;
    struct command *c = &pl->cmds[0];
    pl->ncmds = 1;
    char *word;
    int tok;
    for (;;) {
        tok = next_token(lx, &word);
        if (lx->error)
            return -1;
        if (tok == T_WORD) {
            const char *eq = strchr(word, '=');
            if (c->argc == 0 && eq && valid_name(word, eq - word) && c->nassigns < 8)
                c->assigns[c->nassigns++] = word;
            else if (c->argc < MAXARGV)
                c->argv[c->argc++] = word;
            continue;
        }
        if (tok == T_LT || tok == T_GT || tok == T_GTGT || tok == T_ERR) {
            if (next_token(lx, &word) != T_WORD) {
                dprintf(STDERR_FILENO, "sh: syntax error: missing file name after redirection\n");
                return -1;
            }
            if (tok == T_LT)
                c->in = word;
            else if (tok == T_ERR)
                c->err = word;
            else {
                c->out = word;
                c->append = tok == T_GTGT;
            }
            continue;
        }
        if (tok == T_ERR2OUT) {
            c->err_to_out = true;
            continue;
        }
        if (tok == T_PIPE) {
            if (c->argc == 0 || pl->ncmds == MAXCMDS) {
                dprintf(STDERR_FILENO, "sh: syntax error near '|'\n");
                return -1;
            }
            c = &pl->cmds[pl->ncmds++];
            continue;
        }
        break;
    }
    if (c->argc == 0 && pl->ncmds > 1) {
        dprintf(STDERR_FILENO, "sh: syntax error: empty command after '|'\n");
        return -1;
    }
    if (c->argc == 0 && pl->ncmds == 1 && c->nassigns == 0)
        pl->ncmds = 0;
    pl->background = tok == T_AMP;
    size_t len = lx->p - start;
    const char *end = lx->p;
    if (tok != T_END)
        while (len > 0 && strchr(";&| \t", start[len - 1]))
            len--;
    (void)end;
    if (len >= sizeof(pl->text))
        len = sizeof(pl->text) - 1;
    memcpy(pl->text, start, len);
    pl->text[len] = 0;
    return tok;
}

/* ------------------------------------------------------------------ */
/* Jobs                                                                */
/* ------------------------------------------------------------------ */

static struct job *job_by_id(int id)
{
    for (int i = 0; i < MAXJOBS; i++)
        if (jobs[i].id == id)
            return &jobs[i];
    return NULL;
}

static struct job *job_new(void)
{
    int id = 1;
    for (int i = 0; i < MAXJOBS; i++)
        if (jobs[i].id >= id)
            id = jobs[i].id + 1;
    for (int i = 0; i < MAXJOBS; i++) {
        if (!jobs[i].id) {
            memset(&jobs[i], 0, sizeof(jobs[i]));
            jobs[i].id = id;
            return &jobs[i];
        }
    }
    return NULL;
}

static void job_make_current(struct job *j)
{
    if (current_job != j->id) {
        previous_job = current_job;
        current_job = j->id;
    }
}

static void job_free(struct job *j)
{
    if (current_job == j->id) {
        current_job = previous_job;
        previous_job = 0;
    } else if (previous_job == j->id) {
        previous_job = 0;
    }
    j->id = 0;
}

static void job_update_state(struct job *j)
{
    bool any_running = false, any_stopped = false;
    for (int i = 0; i < j->npids; i++) {
        if (j->done[i])
            continue;
        if (j->stopped[i])
            any_stopped = true;
        else
            any_running = true;
    }
    j->state = any_running ? JOB_RUNNING : any_stopped ? JOB_STOPPED : JOB_DONE;
}

static void record_status(pid_t pid, int status)
{
    for (int k = 0; k < MAXJOBS; k++) {
        struct job *j = &jobs[k];
        if (!j->id)
            continue;
        for (int i = 0; i < j->npids; i++) {
            if (j->pids[i] != pid)
                continue;
            if (WIFSTOPPED(status)) {
                j->stopped[i] = true;
            } else if (WIFCONTINUED(status)) {
                j->stopped[i] = false;
            } else {
                j->done[i] = true;
                j->status[i] = status;
            }
            int old = j->state;
            job_update_state(j);
            if (j->state != old)
                j->notified = false;
            return;
        }
    }
}

static int status_code(int st)
{
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    return WEXITSTATUS(st);
}

static void describe(struct job *j, char *buf, size_t n)
{
    int st = j->status[j->npids - 1];
    if (j->state == JOB_STOPPED) {
        int sig = SIGTSTP;
        for (int i = 0; i < j->npids; i++)
            if (j->stopped[i])
                sig = SIGTSTP;
        snprintf(buf, n, "%s", sig == SIGTSTP ? "Stopped" : strsignal(sig));
    } else if (j->state == JOB_RUNNING) {
        snprintf(buf, n, "Running");
    } else if (WIFSIGNALED(st)) {
        snprintf(buf, n, "%s%s", strsignal(WTERMSIG(st)), WCOREDUMP(st) ? " (core dumped)" : "");
    } else if (WEXITSTATUS(st)) {
        snprintf(buf, n, "Exit %d", WEXITSTATUS(st));
    } else {
        snprintf(buf, n, "Done");
    }
}

static char job_mark(struct job *j)
{
    return j->id == current_job ? '+' : j->id == previous_job ? '-' : ' ';
}

static void print_job(struct job *j, bool with_pids)
{
    char st[48];
    describe(j, st, sizeof(st));
    if (with_pids)
        printf("[%d]%c %d %-24s %s\n", j->id, job_mark(j), j->pgid, st, j->text);
    else
        printf("[%d]%c  %-24s %s\n", j->id, job_mark(j), st, j->text);
}

/* Collect status changes of children without blocking and report them. */
static void reap_and_notify_ex(bool report, bool listing)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG | WUNTRACED)) > 0)
        record_status(pid, st);
    for (int k = 0; k < MAXJOBS; k++) {
        struct job *j = &jobs[k];
        if (!j->id)
            continue;
        if (j->state == JOB_DONE) {
            if (report && j->background)
                print_job(j, false);
            job_free(j);
        } else if (j->state == JOB_STOPPED && !j->notified) {
            if (report && !listing)
                print_job(j, false);
            j->notified = true;
        }
    }
}

static void reap_and_notify(bool report)
{
    reap_and_notify_ex(report, false);
}

static void wait_for_job(struct job *j)
{
    while (j->state == JOB_RUNNING) {
        int st;
        /* With job control the job has its own process group; without it
         * (scripts, sh -c) its processes share ours, so wait for each pid. */
        pid_t target = -j->pgid;
        if (!job_control)
            for (int i = 0; i < j->npids; i++)
                if (!j->done[i]) {
                    target = j->pids[i];
                    break;
                }
        pid_t pid = waitpid(target, &st, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            /* children already gone: mark everything done */
            for (int i = 0; i < j->npids; i++)
                j->done[i] = true;
            job_update_state(j);
            break;
        }
        record_status(pid, st);
    }
}

static void put_in_foreground(struct job *j, bool cont)
{
    j->background = false;
    if (job_control)
        tcsetpgrp(STDIN_FILENO, j->pgid);
    if (cont) {
        for (int i = 0; i < j->npids; i++)
            j->stopped[i] = false;
        j->state = JOB_RUNNING;
        kill(-j->pgid, SIGCONT);
    }
    wait_for_job(j);
    if (job_control) {
        tcsetpgrp(STDIN_FILENO, shell_pgid);
        tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_tmodes);
    }
    if (j->state == JOB_STOPPED) {
        printf("\n");
        job_make_current(j);
        j->notified = true;
        print_job(j, false);
        last_status = 128 + SIGTSTP;
    } else {
        int st = j->status[j->npids - 1];
        last_status = status_code(st);
        if (WIFSIGNALED(st) && WTERMSIG(st) != SIGINT && WTERMSIG(st) != SIGPIPE)
            printf("%s%s\n", strsignal(WTERMSIG(st)), WCOREDUMP(st) ? " (core dumped)" : "");
        job_free(j);
    }
}

static void put_in_background(struct job *j, bool cont)
{
    j->background = true;
    if (cont) {
        for (int i = 0; i < j->npids; i++)
            j->stopped[i] = false;
        j->state = JOB_RUNNING;
        j->notified = true;
        kill(-j->pgid, SIGCONT);
    }
}

/* Parse "%n", "%+", "%-", "%%" or a pid into a job. */
static struct job *find_job(const char *spec)
{
    if (!spec || !strcmp(spec, "%") || !strcmp(spec, "%%") || !strcmp(spec, "%+"))
        return job_by_id(current_job);
    if (!strcmp(spec, "%-"))
        return job_by_id(previous_job);
    if (spec[0] == '%') {
        if (isdigit(spec[1]))
            return job_by_id(atoi(spec + 1));
        for (int i = 0; i < MAXJOBS; i++)       /* %prefix */
            if (jobs[i].id && strncmp(jobs[i].text, spec + 1, strlen(spec + 1)) == 0)
                return &jobs[i];
        return NULL;
    }
    int pid = atoi(spec);
    for (int k = 0; k < MAXJOBS; k++)
        for (int i = 0; jobs[k].id && i < jobs[k].npids; i++)
            if (jobs[k].pids[i] == pid)
                return &jobs[k];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Built-ins                                                           */
/* ------------------------------------------------------------------ */

static void help(void)
{
    printf("SIEOS shell built-ins:\n"
           "  cd [dir]  pwd  exit [n]  logout  umask [mode]  help\n"
           "  jobs [-l]  fg [%%job]  bg [%%job]  wait [%%job]  kill [-SIG] pid|%%job\n"
           "  export [NAME[=value]]  unset NAME  set      (NAME=value to assign)\n"
           "Syntax: a | b   a && b   a || b   a ; b   a &   < > >> 2> 2>&1   ~ $? $USER\n"
           "Keys:   ^C interrupt  ^Z suspend  ^\\ quit  ^D end of input  ^U kill line\n"
           "Programs in /bin:\n  ");
    int fd = open("/bin", O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return;
    char buf[1024];
    long n;
    int col = 2;
    while ((n = getdents(fd, (struct dirent *)buf, sizeof(buf))) > 0) {
        for (long off = 0; off < n;) {
            struct dirent *d = (struct dirent *)(buf + off);
            off += d->d_reclen;
            if (d->d_name[0] == '.')
                continue;
            int len = strlen(d->d_name);
            if (col + len + 1 > 76) {
                printf("\n  ");
                col = 2;
            }
            printf("%s ", d->d_name);
            col += len + 1;
        }
    }
    printf("\n");
    close(fd);
}

static int builtin_kill(struct command *c)
{
    int sig = SIGTERM, i = 1, rc = 0;
    if (c->argc > 1 && strcmp(c->argv[1], "-l") == 0) {
        for (int s = 1; s < NSIG; s++)
            if (strcmp(signame(s), "?") != 0)
                printf("%2d) SIG%-10s%s", s, signame(s), s % 4 == 0 ? "\n" : " ");
        printf("\n");
        return 0;
    }
    if (c->argc > 1 && c->argv[1][0] == '-') {
        const char *s = c->argv[1] + 1;
        if (!strcmp(s, "s") && c->argc > 2) {
            s = c->argv[2];
            i++;
        }
        sig = signum(s);
        if (sig < 0 || sig >= NSIG) {
            dprintf(STDERR_FILENO, "kill: %s: invalid signal\n", s);
            return 2;
        }
        i++;
    }
    if (i >= c->argc) {
        dprintf(STDERR_FILENO, "usage: kill [-SIG] pid|%%job ...\n");
        return 2;
    }
    for (; i < c->argc; i++) {
        pid_t target;
        if (c->argv[i][0] == '%') {
            struct job *j = find_job(c->argv[i]);
            if (!j) {
                dprintf(STDERR_FILENO, "kill: %s: no such job\n", c->argv[i]);
                rc = 1;
                continue;
            }
            target = -j->pgid;
        } else {
            target = atoi(c->argv[i]);
        }
        if (kill(target, sig) < 0) {
            dprintf(STDERR_FILENO, "kill: %s: %s\n", c->argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

static bool is_builtin(const char *name)
{
    static const char *names[] = { "cd", "pwd", "exit", "logout", "help", "jobs", "fg", "bg",
                                   "wait", "kill", "umask", "export", "unset", "set" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0)
            return true;
    return false;
}

static int run_builtin(struct command *c)
{
    const char *name = c->argv[0];
    if (!strcmp(name, "cd")) {
        const char *dir = c->argc > 1 ? c->argv[1] : home_dir();
        bool dash = !strcmp(dir, "-");
        if (dash) {
            dir = var_get("OLDPWD");
            if (!dir) {
                dprintf(STDERR_FILENO, "cd: OLDPWD not set\n");
                return 1;
            }
        }
        char old[256], cwd[256], target[256];
        strlcpy(target, dir, sizeof(target));
        if (!getcwd(old, sizeof(old)))
            old[0] = 0;
        if (chdir(target) < 0) {
            dprintf(STDERR_FILENO, "cd: %s: %s\n", target, strerror(errno));
            return 1;
        }
        var_set("OLDPWD", old, true);
        if (getcwd(cwd, sizeof(cwd))) {
            var_set("PWD", cwd, true);
            if (dash)
                printf("%s\n", cwd);
        }
        return 0;
    }
    if (!strcmp(name, "export")) {
        if (c->argc == 1) {
            for (int i = 0; i < nvars; i++)
                if (vars[i].exported)
                    printf("export %s=\"%s\"\n", vars[i].name, vars[i].value ? vars[i].value : "");
            return 0;
        }
        int rc = 0;
        for (int i = 1; i < c->argc; i++) {
            if (!assign(c->argv[i], true)) {
                if (valid_name(c->argv[i], strlen(c->argv[i]))) {
                    var_set(c->argv[i], var_get(c->argv[i]) ? NULL : "", true);
                } else {
                    dprintf(STDERR_FILENO, "export: '%s': not a valid identifier\n", c->argv[i]);
                    rc = 1;
                }
            }
        }
        return rc;
    }
    if (!strcmp(name, "unset")) {
        for (int i = 1; i < c->argc; i++)
            var_unset(c->argv[i]);
        return 0;
    }
    if (!strcmp(name, "set")) {
        for (int i = 0; i < nvars; i++)
            printf("%s=%s\n", vars[i].name, vars[i].value ? vars[i].value : "");
        return 0;
    }
    if (!strcmp(name, "pwd")) {
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd)))
            printf("%s\n", cwd);
        return 0;
    }
    if (!strcmp(name, "exit") || !strcmp(name, "logout")) {
        bool stopped = false;
        for (int i = 0; i < MAXJOBS; i++)
            if (jobs[i].id && jobs[i].state == JOB_STOPPED)
                stopped = true;
        if (stopped && !warned_stopped) {
            printf("There are stopped jobs.\n");
            warned_stopped = true;
            return 1;
        }
        for (int i = 0; i < MAXJOBS; i++)
            if (jobs[i].id && jobs[i].state == JOB_STOPPED) {
                kill(-jobs[i].pgid, SIGHUP);
                kill(-jobs[i].pgid, SIGCONT);
            }
        exit(c->argc > 1 ? atoi(c->argv[1]) : last_status);
    }
    if (!strcmp(name, "help")) {
        help();
        return 0;
    }
    if (!strcmp(name, "umask")) {
        if (c->argc > 1) {
            umask(strtol(c->argv[1], NULL, 8));
        } else {
            int m = umask(0);
            umask(m);
            printf("%04o\n", m);
        }
        return 0;
    }
    if (!strcmp(name, "jobs")) {
        reap_and_notify_ex(true, true);
        bool l = c->argc > 1 && !strcmp(c->argv[1], "-l");
        for (int i = 0; i < MAXJOBS; i++)
            if (jobs[i].id)
                print_job(&jobs[i], l);
        return 0;
    }
    if (!strcmp(name, "fg") || !strcmp(name, "bg")) {
        if (!job_control) {
            dprintf(STDERR_FILENO, "%s: no job control\n", name);
            return 1;
        }
        struct job *j = find_job(c->argc > 1 ? c->argv[1] : NULL);
        if (!j) {
            dprintf(STDERR_FILENO, "%s: %s: no such job\n", name, c->argc > 1 ? c->argv[1] : "current");
            return 1;
        }
        job_make_current(j);
        if (name[0] == 'f') {
            printf("%s\n", j->text);
            put_in_foreground(j, true);
            return last_status;
        }
        printf("[%d]%c %s &\n", j->id, job_mark(j), j->text);
        put_in_background(j, true);
        return 0;
    }
    if (!strcmp(name, "wait")) {
        if (c->argc > 1) {
            struct job *j = find_job(c->argv[1]);
            if (!j)
                return 127;
            wait_for_job(j);
            int st = j->status[j->npids - 1];
            if (j->state == JOB_DONE)
                job_free(j);
            return status_code(st);
        }
        for (int i = 0; i < MAXJOBS; i++)
            if (jobs[i].id && jobs[i].state == JOB_RUNNING) {
                wait_for_job(&jobs[i]);
                if (jobs[i].state == JOB_DONE)
                    job_free(&jobs[i]);
            }
        return 0;
    }
    if (!strcmp(name, "kill"))
        return builtin_kill(c);
    return 127;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

static int redirect(struct command *c)
{
    if (c->in) {
        int fd = open(c->in, O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "sh: %s: %s\n", c->in, strerror(errno));
            return -1;
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (c->out) {
        int fd = open(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC), 0666);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "sh: %s: %s\n", c->out, strerror(errno));
            return -1;
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    if (c->err) {
        int fd = open(c->err, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "sh: %s: %s\n", c->err, strerror(errno));
            return -1;
        }
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    if (c->err_to_out)
        dup2(STDOUT_FILENO, STDERR_FILENO);
    return 0;
}

static void exec_command(struct command *c)
{
    if (c->argc == 0)
        exit(0);
    if (is_builtin(c->argv[0]))
        exit(run_builtin(c));
    const char *name = c->argv[0];
    for (int i = 0; i < c->nassigns; i++)     /* NAME=value cmd: child-only env */
        assign(c->assigns[i], true);
    environ = build_envp();
    execvp(name, c->argv);
    if (errno == ENOENT)
        dprintf(STDERR_FILENO, "sh: %s: command not found\n", name);
    else
        dprintf(STDERR_FILENO, "sh: %s: %s\n", name, strerror(errno));
    exit(errno == ENOENT ? 127 : 126);
}

static void child_setup(pid_t pgid, bool foreground)
{
    if (job_control) {
        pid_t pid = getpid();
        if (pgid == 0)
            pgid = pid;
        setpgid(pid, pgid);
        if (foreground)
            tcsetpgrp(STDIN_FILENO, pgid);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
    }
    signal(SIGCHLD, SIG_DFL);
}

static void run_pipeline(struct pipeline *pl)
{
    if (pl->ncmds == 0)
        return;

    /* NAME=value with no command sets shell variables. */
    if (pl->ncmds == 1 && pl->cmds[0].argc == 0) {
        for (int i = 0; i < pl->cmds[0].nassigns; i++) {
            const char *s = pl->cmds[0].assigns[i];
            const char *eq = strchr(s, '=');
            char name[64];
            size_t n = MIN((size_t)(eq - s), sizeof(name) - 1);
            memcpy(name, s, n);
            name[n] = 0;
            struct var *v = var_find(name);
            var_set(name, eq + 1, v && v->exported);
        }
        last_status = 0;
        return;
    }

    /* A lone built-in runs inside the shell (with temporary redirections). */
    if (pl->ncmds == 1 && !pl->background && is_builtin(pl->cmds[0].argv[0])) {
        struct command *c = &pl->cmds[0];
        int save[3] = { dup(0), dup(1), dup(2) };
        if (redirect(c) == 0)
            last_status = run_builtin(c);
        else
            last_status = 1;
        for (int i = 0; i < 3; i++) {
            dup2(save[i], i);
            close(save[i]);
        }
        return;
    }

    struct job *j = job_new();
    if (!j) {
        dprintf(STDERR_FILENO, "sh: too many jobs\n");
        last_status = 1;
        return;
    }
    strlcpy(j->text, pl->text, sizeof(j->text));
    j->background = pl->background;

    int prev_read = -1;
    for (int i = 0; i < pl->ncmds; i++) {
        int fds[2] = { -1, -1 };
        if (i + 1 < pl->ncmds && pipe(fds) < 0) {
            perror("sh: pipe");
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("sh: fork");
            break;
        }
        if (pid == 0) {
            child_setup(j->pgid, !pl->background);
            if (prev_read >= 0) {
                dup2(prev_read, STDIN_FILENO);
                close(prev_read);
            } else if (pl->background && !job_control) {
                int null = open("/dev/null", O_RDONLY);
                if (null >= 0) {
                    dup2(null, STDIN_FILENO);
                    close(null);
                }
            }
            if (fds[1] >= 0) {
                dup2(fds[1], STDOUT_FILENO);
                close(fds[1]);
                close(fds[0]);
            }
            if (redirect(&pl->cmds[i]) < 0)
                exit(1);
            exec_command(&pl->cmds[i]);
        }
        if (!j->pgid)
            j->pgid = pid;
        if (job_control)
            setpgid(pid, j->pgid);
        j->pids[j->npids++] = pid;
        if (prev_read >= 0)
            close(prev_read);
        if (fds[1] >= 0)
            close(fds[1]);
        prev_read = fds[0];
    }
    if (prev_read >= 0)
        close(prev_read);
    if (j->npids == 0) {
        job_free(j);
        last_status = 1;
        return;
    }
    j->state = JOB_RUNNING;

    if (pl->background) {
        job_make_current(j);
        if (interactive)
            printf("[%d] %d\n", j->id, j->pids[j->npids - 1]);
        last_status = 0;
    } else {
        put_in_foreground(j, false);
    }
}

static void run_line(const char *line)
{
    static char words[MAXLINE * 2];
    struct lexer lx = { line, words, words + sizeof(words) - 1, false };
    int skip = 0;              /* 1: skip until || or ;  2: skip until && or ; */
    for (;;) {
        struct pipeline pl;
        int tok = parse_pipeline(&lx, &pl);
        if (tok < 0) {
            last_status = 2;
            return;
        }
        if (!skip)
            run_pipeline(&pl);
        if (tok == T_END)
            break;
        if (tok == T_AND)
            skip = last_status != 0 ? 1 : 0;
        else if (tok == T_OR)
            skip = last_status == 0 ? 2 : 0;
        else
            skip = 0;
    }
}

static void init_job_control(void)
{
    if (!isatty(STDIN_FILENO))
        return;
    /* Wait until we are in the foreground. */
    pid_t fg;
    while ((fg = tcgetpgrp(STDIN_FILENO)) >= 0 && fg != (shell_pgid = getpgrp()))
        kill(-shell_pgid, SIGTTIN);
    if (fg < 0)
        return;                          /* no controlling terminal */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;              /* no SA_RESTART: ^C aborts the line */
    sigaction(SIGINT, &sa, NULL);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    shell_pgid = getpid();
    if (getpgrp() != shell_pgid && setpgid(shell_pgid, shell_pgid) < 0) {
        perror("sh: setpgid");
        return;
    }
    tcsetpgrp(STDIN_FILENO, shell_pgid);
    tcgetattr(STDIN_FILENO, &shell_tmodes);
    job_control = true;
}

int main(int argc, char **argv)
{
    int first = 1;
    vars_from_environ();
    if (!var_get("PATH"))
        var_set("PATH", "/bin:/usr/bin:/sbin", true);
    if (argc > 1 && strcmp(argv[1], "-c") == 0 && argc > 2) {
        run_line(argv[2]);
        return last_status;
    }
    if (argc > first) {
        in_fd = open(argv[first], O_RDONLY);
        if (in_fd < 0) {
            dprintf(STDERR_FILENO, "sh: %s: %s\n", argv[first], strerror(errno));
            return 127;
        }
    } else {
        interactive = isatty(STDIN_FILENO);
        if (interactive)
            init_job_control();
    }
    if (argv[0][0] == '-')                   /* login shell */
        chdir(home_dir());
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)))
        var_set("PWD", cwd, true);

    char line[MAXLINE];
    for (;;) {
        if (interactive) {
            reap_and_notify(true);
            prompt();
        }
        int r = read_line(line, sizeof(line));
        if (r == INTERRUPTED) {
            last_status = 130;
            continue;
        }
        if (r < 0) {
            if (interactive)
                printf("logout\n");
            break;
        }
        run_line(line);
        if (interactive)
            warned_stopped = warned_stopped && strncmp(line, "exit", 4) == 0;
    }
    return last_status;
}
