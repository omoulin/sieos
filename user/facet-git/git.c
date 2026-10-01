/*
 * facet-git - Git: the user's repositories in a window (the package
 * facet-git, on the package git).
 *
 * The repositories are listed on the left (~/.config/facet-git/repos):
 * Add an existing one, Clone one, start a New one, Remove one from the
 * list.  The toolbar shows the current branch (ahead and behind its
 * upstream) and does Fetch, Pull, Push, Refresh and Settings.  Three tabs:
 *   Changes   the changed files: the box stages or unstages one (Stage all,
 *             Unstage all); a click shows its diff; a message and Commit
 *   History   the commits; a click shows one (git show)
 *   Branches  the local and remote branches: Checkout, Merge into the
 *             current one, Delete; New branch; the remotes
 * The output of what git did shows at the bottom.  Settings keeps the name
 * and email of the commits (git config --global) and a username and token
 * for a host (git's credential store, ~/.git-credentials, mode 0600).
 * Network operations (clone, fetch, pull, push) run in the background; the
 * window stays responsive.  Lines of a diff are selected with the mouse and
 * copied with Ctrl+C.
 *
 * A Facet application (libfacet).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "common.h"
#include <sys/wait.h>

#define GIT        "/usr/pkg/bin/git"
#define TOOLBAR_H  48
#define SIDEBAR_W  220
#define TABS_H     34
#define OUTPUT_H   96
#define ROW_H      24
#define REPO_H     42
#define BTN_H      26
#define PADX       10
#define MAX_REPOS  64
#define MAX_CHANGES 1000
#define MAX_COMMITS 400
#define MAX_BRANCHES 200
#define TEXT_MAX   (2 << 20)

enum { TAB_CHANGES, TAB_HISTORY, TAB_BRANCHES, NTABS };
enum { DLG_NONE, DLG_ADD, DLG_CLONE, DLG_NEW, DLG_SETTINGS };
enum { AFTER_REFRESH, AFTER_ADD };

struct change { char x, y; char path[300]; };
struct commit { char hash[16], subject[200], author[64], when[40]; };
struct branch { char name[120]; bool current, remote; };

/* A text pane (a diff, a commit): its lines, scrolled, lines selected */
struct text {
    char *buf;
    int len;
    int *line;                             /* offsets of the lines */
    int nlines, first;
    int sel_a, sel_b;
    bool selecting, has_sel;
};

static struct {
    char repos[MAX_REPOS][320];
    int nrepos, cur, repo_first;
    int tab;
    /* the current repository */
    char branch[160], upstream[160];
    int ahead, behind;
    bool is_repo;
    struct change ch[MAX_CHANGES];
    int nch, ch_sel, ch_first;
    struct commit log[MAX_COMMITS];
    int nlog, log_sel, log_first;
    struct branch br[MAX_BRANCHES];
    int nbr, br_sel, br_first;
    char remotes[1024];
    struct text detail;
    struct fct_field msg, newbranch;
    int focus;                             /* 0 none, 1 the message, 2 the new branch, 10+ a dialog's field */
    /* the output of the last command */
    char out[16384];
    int outlen;
    char last_cmd[200];
    bool last_ok;
    /* a command running in the background */
    pid_t job;
    int job_fd;
    char job_label[80];
    int job_after;
    char job_path[320];
    int spin;
    /* a dialog */
    int dlg;
    bool have_token;                       /* (Settings: a token is kept for the host) */
    struct fct_field f[5];
    int nf;
    char dlg_msg[200];
} G = { .cur = -1, .job_fd = -1, .ch_sel = -1, .log_sel = -1, .br_sel = -1 };

/* ---------------- running git ---------------- */

static void out_set(const char *cmd, const char *text, size_t n, bool ok)
{
    snprintf(G.last_cmd, sizeof(G.last_cmd), "%s", cmd);
    if (n > sizeof(G.out) - 1) {                   /* (the end of a long output) */
        text += n - (sizeof(G.out) - 1);
        n = sizeof(G.out) - 1;
    }
    memcpy(G.out, text, n);
    G.outlen = (int)n;
    G.out[n] = 0;
    G.last_ok = ok;
}

static void child_env(void)
{
    setenv("GIT_TERMINAL_PROMPT", "0", 1);         /* no questions on a terminal there is not */
    setenv("GIT_PAGER", "cat", 1);
    setenv("PAGER", "cat", 1);
    setenv("GIT_EDITOR", "true", 1);
    setenv("GIT_MERGE_AUTOEDIT", "no", 1);
    unsetenv("SIEOS_DESKTOP");
}

/* git ARGS in dir (NULL: the current repository), its output (malloc'd, NUL
 * ended) in *out; returns the exit status (-1 if it could not run). */
static int git_run(const char *dir, char *const args[], char **out, int *outlen)
{
    int fds[2];
    if (out)
        *out = NULL;
    if (pipe(fds) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        if (dir && chdir(dir) < 0)
            _exit(126);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        int nul = open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        for (int fd = 3; fd < 64; fd++)
            close(fd);
        child_env();
        char *argv[24];
        int n = 0;
        argv[n++] = "git";
        for (int i = 0; args[i] && n < 23; i++)
            argv[n++] = args[i];
        argv[n] = NULL;
        execv(GIT, argv);
        _exit(127);
    }
    close(fds[1]);
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap + 1);
    long r;
    while (buf && (r = read(fds[0], buf + len, cap - len)) > 0) {
        len += r;
        if (len == cap && cap < TEXT_MAX) {
            cap *= 2;
            char *nb = realloc(buf, cap + 1);
            if (!nb)
                break;
            buf = nb;
        } else if (len == cap) {
            char sink[4096];
            while (read(fds[0], sink, sizeof(sink)) > 0)
                ;
            break;
        }
    }
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (buf)
        buf[len] = 0;
    if (out)
        *out = buf;
    else
        free(buf);
    if (outlen)
        *outlen = (int)len;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static const char *repo(void) { return G.cur >= 0 ? G.repos[G.cur] : NULL; }

/* A local command whose output the user sees (stage, commit, checkout ...) */
static bool git_do(const char *label, char *const args[])
{
    char *o;
    int n;
    int st = git_run(repo(), args, &o, &n);
    out_set(label, o ? o : "", o ? n : 0, st == 0);
    free(o);
    return st == 0;
}

/* In the background (network): output arrives through job_fd */
static void job_start(const char *dir, char *const args[], const char *label, int after, const char *path)
{
    if (G.job > 0)
        return;
    int fds[2];
    if (pipe(fds) < 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        if (dir && chdir(dir) < 0)
            _exit(126);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        int nul = open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        for (int fd = 3; fd < 64; fd++)
            close(fd);
        child_env();
        char *argv[24];
        int n = 0;
        argv[n++] = "git";
        for (int i = 0; args[i] && n < 23; i++)
            argv[n++] = args[i];
        argv[n] = NULL;
        execv(GIT, argv);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    G.job = pid;
    G.job_fd = fds[0];
    G.job_after = after;
    snprintf(G.job_label, sizeof(G.job_label), "%s", label);
    snprintf(G.job_path, sizeof(G.job_path), "%s", path ? path : "");
    out_set(label, "", 0, true);
}

/* ---------------- the repositories ---------------- */

static void repos_file(char *p, size_t n, bool make)
{
    const char *home = getenv("HOME");
    snprintf(p, n, "%s/.config", home ? home : "");
    if (make)
        mkdir(p, 0755);
    snprintf(p, n, "%s/.config/facet-git", home ? home : "");
    if (make)
        mkdir(p, 0700);
    snprintf(p, n, "%s/.config/facet-git/repos", home ? home : "");
}

static void repos_load(void)
{
    char p[320];
    repos_file(p, sizeof(p), false);
    FILE *f = fopen(p, "r");
    char line[400];
    G.nrepos = 0;
    while (f && fgets(line, sizeof(line), f) && G.nrepos < MAX_REPOS) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] == '/')
            snprintf(G.repos[G.nrepos++], sizeof(G.repos[0]), "%s", line);
    }
    if (f)
        fclose(f);
}

static void repos_save(void)
{
    char p[320];
    repos_file(p, sizeof(p), true);
    FILE *f = fopen(p, "w");
    if (!f)
        return;
    for (int i = 0; i < G.nrepos; i++)
        fprintf(f, "%s\n", G.repos[i]);
    fclose(f);
}

static const char *base_name(const char *p)
{
    const char *b = strrchr(p, '/');
    return b && b[1] ? b + 1 : p;
}

static bool repos_add(const char *path)
{
    for (int i = 0; i < G.nrepos; i++)
        if (!strcmp(G.repos[i], path)) {
            G.cur = i;
            return true;
        }
    if (G.nrepos == MAX_REPOS)
        return false;
    snprintf(G.repos[G.nrepos], sizeof(G.repos[0]), "%s", path);
    G.cur = G.nrepos++;
    repos_save();
    return true;
}

/* ---------------- the text pane ---------------- */

static void text_set(struct text *t, char *buf, int len)
{
    free(t->buf);
    free(t->line);
    t->buf = buf;
    t->len = len;
    t->nlines = 0;
    t->first = 0;
    t->has_sel = t->selecting = false;
    int cap = 256;
    t->line = malloc(cap * sizeof(int));
    for (int i = 0; buf && i <= len; ) {
        if (t->nlines == cap) {
            cap *= 2;
            t->line = realloc(t->line, cap * sizeof(int));
        }
        t->line[t->nlines++] = i;
        char *nl = memchr(buf + i, '\n', len - i);
        if (!nl)
            break;
        i = (int)(nl - buf) + 1;
        if (i == len)
            break;
    }
}

static void text_set_str(struct text *t, const char *s)
{
    char *b = strdup(s);
    text_set(t, b, (int)strlen(b));
}

/* ---------------- reading the repository ---------------- */

static void read_status(void)
{
    char *o;
    int n;
    char *args[] = { "status", "--porcelain=v1", "-b", "-z", "-uall", NULL };
    G.nch = 0;
    G.branch[0] = G.upstream[0] = 0;
    G.ahead = G.behind = 0;
    G.is_repo = git_run(repo(), args, &o, &n) == 0;
    if (!G.is_repo || !o) {
        free(o);
        return;
    }
    char keep[300] = "";
    if (G.ch_sel >= 0 && G.ch_sel < MAX_CHANGES)
        snprintf(keep, sizeof(keep), "%s", G.ch[G.ch_sel].path);
    for (char *p = o; p < o + n; ) {
        size_t l = strlen(p);
        if (!strncmp(p, "## ", 3)) {               /* ## branch...upstream [ahead 1, behind 2] */
            char *b = p + 3, *dots = strstr(b, "..."), *br = strchr(b, '[');
            if (!strncmp(b, "No commits yet on ", 18))
                b += 18;
            snprintf(G.branch, sizeof(G.branch), "%.*s", (int)(dots ? dots - b : br ? br - b - 1 : (long)strlen(b)), b);
            if (dots) {
                char *e = strchr(dots + 3, ' ');
                snprintf(G.upstream, sizeof(G.upstream), "%.*s", (int)(e ? e - dots - 3 : (long)strlen(dots + 3)), dots + 3);
            }
            if (br) {
                char *a = strstr(br, "ahead "), *bh = strstr(br, "behind ");
                G.ahead = a ? atoi(a + 6) : 0;
                G.behind = bh ? atoi(bh + 7) : 0;
            }
        } else if (l > 3 && G.nch < MAX_CHANGES) {
            struct change *c = &G.ch[G.nch++];
            c->x = p[0];
            c->y = p[1];
            snprintf(c->path, sizeof(c->path), "%s", p + 3);
            if (p[0] == 'R' || p[0] == 'C') {      /* a rename: the old name follows */
                p += l + 1;
                l = strlen(p);
            }
        }
        p += l + 1;
    }
    free(o);
    G.ch_sel = -1;
    for (int i = 0; i < G.nch && keep[0]; i++)
        if (!strcmp(G.ch[i].path, keep))
            G.ch_sel = i;
}

static void read_log(void)
{
    char *o;
    int n;
    char *args[] = { "log", "--max-count=400", "--format=%h%x1f%s%x1f%an%x1f%ar%x1e", NULL };
    G.nlog = 0;
    if (git_run(repo(), args, &o, &n) != 0 || !o) {
        free(o);
        return;
    }
    for (char *rec = o; *rec && G.nlog < MAX_COMMITS; ) {
        char *end = strchr(rec, 0x1e);
        if (!end)
            break;
        *end = 0;
        while (*rec == '\n')
            rec++;
        char *f[4] = { rec, NULL, NULL, NULL };   /* hash, subject, author, when */
        for (int i = 1; i < 4; i++) {
            char *u = f[i - 1] ? strchr(f[i - 1], 0x1f) : NULL;
            if (u) {
                *u = 0;
                f[i] = u + 1;
            }
        }
        struct commit *c = &G.log[G.nlog++];
        snprintf(c->hash, sizeof(c->hash), "%s", f[0]);
        snprintf(c->subject, sizeof(c->subject), "%s", f[1] ? f[1] : "");
        snprintf(c->author, sizeof(c->author), "%s", f[2] ? f[2] : "");
        snprintf(c->when, sizeof(c->when), "%s", f[3] ? f[3] : "");
        rec = end + 1;
    }
    free(o);
}

static void read_branches(void)
{
    char *o;
    int n;
    G.nbr = 0;
    char *local[] = { "branch", "--format=%(HEAD)%(refname:short)", NULL };
    char *remote[] = { "branch", "-r", "--format=%(refname:short)", NULL };
    for (int pass = 0; pass < 2; pass++) {
        if (git_run(repo(), pass ? remote : local, &o, &n) != 0 || !o) {
            free(o);
            continue;
        }
        for (char *l = strtok(o, "\n"); l && G.nbr < MAX_BRANCHES; l = strtok(NULL, "\n")) {
            struct branch *b = &G.br[G.nbr];
            b->remote = pass;
            b->current = !pass && l[0] == '*';
            const char *name = pass ? l : l + 1;
            if (pass && strstr(name, "/HEAD"))
                continue;
            snprintf(b->name, sizeof(b->name), "%s", name);
            G.nbr++;
        }
        free(o);
    }
    char *rv[] = { "remote", "-v", NULL };
    G.remotes[0] = 0;
    if (git_run(repo(), rv, &o, &n) == 0 && o) {
        for (char *t = o; *t; t++)                 /* (name TAB url (fetch)) */
            if (*t == '\t')
                *t = ' ';
        snprintf(G.remotes, sizeof(G.remotes), "%s", o);
    }
    free(o);
    if (G.br_sel >= G.nbr)
        G.br_sel = -1;
}

static void show_change(int i)
{
    if (i < 0 || i >= G.nch) {
        text_set(&G.detail, NULL, 0);
        return;
    }
    struct change *c = &G.ch[i];
    char *o = NULL;
    int n = 0;
    if (c->x == '?') {                             /* a new file: all of it */
        char *args[] = { "diff", "--no-index", "--", "/dev/null", c->path, NULL };
        git_run(repo(), args, &o, &n);
    } else {
        char *args[] = { "diff", "HEAD", "--", c->path, NULL };
        if (git_run(repo(), args, &o, &n) != 0) {  /* (no commit yet) */
            free(o);
            char *a2[] = { "diff", "--cached", "--", c->path, NULL };
            git_run(repo(), a2, &o, &n);
        }
    }
    if (o && !n) {
        free(o);
        o = strdup("(no difference to show)\n");
        n = (int)strlen(o);
    }
    text_set(&G.detail, o, n);
}

static void show_commit(int i)
{
    if (i < 0 || i >= G.nlog) {
        text_set(&G.detail, NULL, 0);
        return;
    }
    char *o;
    int n;
    char *args[] = { "show", "--stat", "--patch", "--format=commit %H%nAuthor: %an <%ae>%nDate:   %ad%n%n    %s%n%n%b",
                     G.log[i].hash, NULL };
    git_run(repo(), args, &o, &n);
    text_set(&G.detail, o, n);
}

static void refresh(void)
{
    if (!repo()) {
        G.is_repo = false;
        G.nch = G.nlog = G.nbr = 0;
        text_set(&G.detail, NULL, 0);
        return;
    }
    read_status();
    if (!G.is_repo) {
        text_set_str(&G.detail, "This folder is not a git repository (or it is gone).");
        return;
    }
    read_log();
    read_branches();
    if (G.tab == TAB_CHANGES)
        show_change(G.ch_sel);
    else if (G.tab == TAB_HISTORY)
        show_commit(G.log_sel);
    else
        text_set(&G.detail, NULL, 0);
}

static void select_repo(struct fct_view *v, int i)
{
    G.cur = i;
    G.ch_sel = G.log_sel = G.br_sel = -1;
    G.ch_first = G.log_first = G.br_first = 0;
    fct_field_set(&G.msg, "");
    refresh();
    char t[160];
    snprintf(t, sizeof(t), "Git - %s", repo() ? base_name(repo()) : "no repository");
    fct_view_set_title(v, t);
}

/* ---------------- settings: name, email, credentials ---------------- */

static void git_config_get(const char *key, char *val, size_t n)
{
    char *o;
    char *args[] = { "config", "--global", "--get", (char *)key, NULL };
    val[0] = 0;
    if (git_run(NULL, args, &o, NULL) == 0 && o) {
        o[strcspn(o, "\n")] = 0;
        snprintf(val, n, "%s", o);
    }
    free(o);
}

static void creds_path(char *p, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(p, n, "%s/.git-credentials", home ? home : "");
}

/* The username stored for host (and whether a token is), from ~/.git-credentials */
static bool creds_find(const char *host, char *user, size_t n)
{
    char p[320], line[600];
    creds_path(p, sizeof(p));
    FILE *f = fopen(p, "r");
    bool found = false;
    user[0] = 0;
    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        char *at = strrchr(line, '@'), *sl = strstr(line, "://");
        if (!at || !sl || strcmp(at + 1, host))
            continue;
        char *colon = strchr(sl + 3, ':');
        if (colon && colon < at)
            snprintf(user, n, "%.*s", (int)(colon - sl - 3), sl + 3);
        found = true;
    }
    if (f)
        fclose(f);
    memset(line, 0, sizeof(line));
    return found;
}

static void url_encode(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; *in && k + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || strchr("-._~", c))
            out[k++] = (char)c;
        else
            k += snprintf(out + k, n - k, "%%%02X", c);
    }
    out[k] = 0;
}

/* Replace host's line in ~/.git-credentials (kept 0600) */
static bool creds_store(const char *host, const char *user, const char *token)
{
    char p[320], tmp[340], line[600];
    creds_path(p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.new", p);
    FILE *in = fopen(p, "r");
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        if (in)
            fclose(in);
        return false;
    }
    FILE *out = fdopen(fd, "w");
    while (in && fgets(line, sizeof(line), in)) {
        char copy[600];
        snprintf(copy, sizeof(copy), "%s", line);
        copy[strcspn(copy, "\n")] = 0;
        char *at = strrchr(copy, '@');
        if (at && !strcmp(at + 1, host))
            continue;                              /* (replaced below) */
        fputs(line, out);
    }
    if (in)
        fclose(in);
    char eu[200], et[400];
    url_encode(user, eu, sizeof(eu));
    url_encode(token, et, sizeof(et));
    fprintf(out, "https://%s:%s@%s\n", eu, et, host);
    memset(et, 0, sizeof(et));
    memset(line, 0, sizeof(line));
    fclose(out);
    return rename(tmp, p) == 0;
}

/* ---------------- dialogs ---------------- */

static const char *const dlg_titles[] = { "", "Add a repository", "Clone a repository", "New repository", "Settings" };

static void dlg_open(int kind)
{
    G.dlg = kind;
    G.dlg_msg[0] = 0;
    for (int i = 0; i < 5; i++) {
        fct_field_set(&G.f[i], "");
        G.f[i].masked = false;
    }
    G.focus = 10;
    const char *home = getenv("HOME");
    char path[320];
    switch (kind) {
    case DLG_ADD:
        G.nf = 1;
        snprintf(path, sizeof(path), "%s/", home ? home : "");
        fct_field_set(&G.f[0], path);
        break;
    case DLG_CLONE:
        G.nf = 2;
        snprintf(path, sizeof(path), "%s/src", home ? home : "");
        fct_field_set(&G.f[1], path);
        break;
    case DLG_NEW:
        G.nf = 1;
        snprintf(path, sizeof(path), "%s/src/", home ? home : "");
        fct_field_set(&G.f[0], path);
        break;
    case DLG_SETTINGS: {
        G.nf = 5;
        char v[200];
        git_config_get("user.name", v, sizeof(v));
        fct_field_set(&G.f[0], v);
        git_config_get("user.email", v, sizeof(v));
        fct_field_set(&G.f[1], v);
        fct_field_set(&G.f[2], "github.com");
        G.have_token = creds_find("github.com", v, sizeof(v));
        fct_field_set(&G.f[3], v);
        G.f[4].masked = true;
        break;
    }
    }
}

static const char *dlg_label(int i)
{
    static const char *const add[] = { "Folder" }, *const clone[] = { "Address (URL)", "Into the folder" },
                             *const nw[] = { "Folder" },
                             *const set[] = { "Your name", "Your email", "Host", "Username", "Token" };
    switch (G.dlg) {
    case DLG_ADD: return add[i];
    case DLG_CLONE: return clone[i];
    case DLG_NEW: return nw[i];
    default: return set[i];
    }
}

static void trim(char *t)
{
    char *b = t;
    while (*b == ' ')
        b++;
    memmove(t, b, strlen(b) + 1);
    size_t l = strlen(t);
    while (l && (t[l - 1] == ' ' || t[l - 1] == '/'))
        t[--l] = 0;
}

static void dlg_ok(struct fct_view *v)
{
    for (int i = 0; i < G.nf; i++)
        if (G.dlg != DLG_SETTINGS || i < 4)
            trim(G.f[i].text);
    if (G.dlg == DLG_ADD) {
        char *o;
        char *args[] = { "rev-parse", "--show-toplevel", NULL };
        if (git_run(G.f[0].text, args, &o, NULL) != 0 || !o) {
            snprintf(G.dlg_msg, sizeof(G.dlg_msg), "That is not a git repository.");
            free(o);
            return;
        }
        o[strcspn(o, "\n")] = 0;
        repos_add(o);
        free(o);
        G.dlg = DLG_NONE;
        select_repo(v, G.cur);
    } else if (G.dlg == DLG_NEW) {
        const char *p = G.f[0].text;
        if (p[0] != '/') {
            snprintf(G.dlg_msg, sizeof(G.dlg_msg), "Give the whole path of the folder (starting with /).");
            return;
        }
        char mk[340];
        snprintf(mk, sizeof(mk), "%s", p);
        for (char *sl = mk + 1; (sl = strchr(sl, '/')); sl++) {   /* mkdir -p */
            *sl = 0;
            mkdir(mk, 0755);
            *sl = '/';
        }
        mkdir(mk, 0755);
        char *args[] = { "init", "-b", "main", (char *)p, NULL };
        char *o;
        int n;
        int st = git_run(NULL, args, &o, &n);
        out_set("git init", o ? o : "", o ? n : 0, st == 0);
        free(o);
        if (st != 0) {
            snprintf(G.dlg_msg, sizeof(G.dlg_msg), "git init failed (see the output).");
            return;
        }
        repos_add(p);
        G.dlg = DLG_NONE;
        select_repo(v, G.cur);
    } else if (G.dlg == DLG_CLONE) {
        const char *url = G.f[0].text, *into = G.f[1].text;
        if (!url[0] || into[0] != '/') {
            snprintf(G.dlg_msg, sizeof(G.dlg_msg), "An address, and a folder starting with /.");
            return;
        }
        char name[160], dest[340];
        snprintf(name, sizeof(name), "%s", base_name(url));
        size_t l = strlen(name);
        if (l > 4 && !strcmp(name + l - 4, ".git"))
            name[l - 4] = 0;
        snprintf(dest, sizeof(dest), "%s/%s", into, name);
        mkdir(into, 0755);
        char *args[] = { "clone", "--progress", (char *)url, dest, NULL };
        char label[200];
        snprintf(label, sizeof(label), "git clone %s", url);
        job_start(NULL, args, label, AFTER_ADD, dest);
        G.dlg = DLG_NONE;
    } else if (G.dlg == DLG_SETTINGS) {
        const char *name = G.f[0].text, *email = G.f[1].text, *host = G.f[2].text, *user = G.f[3].text;
        const char *token = G.f[4].text;
        char msg[300] = "";
        if (name[0]) {
            char *a[] = { "config", "--global", "user.name", (char *)name, NULL };
            git_run(NULL, a, NULL, NULL);
        }
        if (email[0]) {
            char *a[] = { "config", "--global", "user.email", (char *)email, NULL };
            git_run(NULL, a, NULL, NULL);
        }
        if (host[0] && user[0] && token[0]) {
            char *a[] = { "config", "--global", "credential.helper", "store", NULL };
            git_run(NULL, a, NULL, NULL);
            if (!creds_store(host, user, token))
                snprintf(msg, sizeof(msg), "Could not write ~/.git-credentials.");
        } else if (token[0] && (!host[0] || !user[0])) {
            snprintf(G.dlg_msg, sizeof(G.dlg_msg), "A token needs its host and username.");
            return;
        }
        memset(G.f[4].text, 0, sizeof(G.f[4].text));
        out_set("settings", msg[0] ? msg : "Saved: the name and email of your commits, and your credentials.",
                strlen(msg[0] ? msg : "Saved: the name and email of your commits, and your credentials."), !msg[0]);
        G.dlg = DLG_NONE;
    }
}

/* ---------------- actions ---------------- */

static void toggle_stage(int i)
{
    struct change *c = &G.ch[i];
    bool staged = c->x != ' ' && c->x != '?';
    if (staged) {
        char *a[] = { "restore", "--staged", "--", c->path, NULL };
        if (!git_do("git restore --staged", a)) {   /* (no commit yet: rm --cached) */
            char *b[] = { "rm", "--cached", "-q", "--", c->path, NULL };
            git_do("git rm --cached", b);
        }
    } else {
        char *a[] = { "add", "--", c->path, NULL };
        git_do("git add", a);
    }
}

static void commit(void)
{
    char *m = G.msg.text;
    while (*m == ' ')
        m++;
    if (!*m) {
        out_set("commit", "Write a message for the commit first.", 37, false);
        return;
    }
    char *a[] = { "commit", "-m", m, NULL };
    if (git_do("git commit", a))
        fct_field_set(&G.msg, "");
    refresh();
}

static void remote_op(const char *op)
{
    if (!repo() || G.job > 0)
        return;
    char label[80];
    snprintf(label, sizeof(label), "git %s", op);
    if (!strcmp(op, "push") && !G.upstream[0] && G.branch[0]) {   /* a first push: set the upstream */
        char *a[] = { "push", "--progress", "-u", "origin", G.branch, NULL };
        job_start(repo(), a, label, AFTER_REFRESH, NULL);
        return;
    }
    char *a[] = { (char *)op, "--progress", NULL };
    job_start(repo(), a, label, AFTER_REFRESH, NULL);
}

/* ---------------- geometry ---------------- */

static struct rect r_sidebar(struct rect c) { return rect_make(0, TOOLBAR_H, SIDEBAR_W, c.h - TOOLBAR_H); }
static struct rect r_main(struct rect c) { return rect_make(SIDEBAR_W, TOOLBAR_H, c.w - SIDEBAR_W, c.h - TOOLBAR_H - OUTPUT_H); }
static struct rect r_output(struct rect c) { return rect_make(SIDEBAR_W, c.h - OUTPUT_H, c.w - SIDEBAR_W, OUTPUT_H); }
static struct rect r_tab(struct rect c, int i) { struct rect m = r_main(c); return rect_make(m.x + PADX + i * 120, m.y + 6, 114, TABS_H - 6); }
static struct rect r_body(struct rect c) { struct rect m = r_main(c); return rect_make(m.x, m.y + TABS_H, m.w, m.h - TABS_H); }
static struct rect r_list(struct rect c)
{
    struct rect b = r_body(c);
    int w = G.tab == TAB_BRANCHES ? b.w / 2 : MAX(260, b.w * 2 / 5);
    int bottom = G.tab == TAB_CHANGES ? 2 * BTN_H + 26 : 0;
    return rect_make(b.x + PADX, b.y + (G.tab == TAB_CHANGES ? 34 : 6), w - PADX, b.h - (G.tab == TAB_CHANGES ? 34 : 6) - bottom - 6);
}
static struct rect r_detail(struct rect c)
{
    struct rect b = r_body(c), l = r_list(c);
    return rect_make(l.x + l.w + 8, b.y + 6, b.x + b.w - (l.x + l.w + 8) - PADX, b.h - 12);
}
static struct rect r_toolbtn(struct rect c, int i) { return rect_make(c.w - PADX - (5 - i) * 84, (TOOLBAR_H - 30) / 2, 78, 30); }
static struct rect r_sidebtn(struct rect c, int i)
{
    struct rect s = r_sidebar(c);
    int w = (s.w - 3 * PADX) / 2;
    return rect_make(s.x + PADX + (i % 2) * (w + PADX), s.y + s.h - 2 * (BTN_H + 6) - 4 + (i / 2) * (BTN_H + 6), w, BTN_H);
}
static struct rect r_repos(struct rect c) { struct rect s = r_sidebar(c); return rect_make(s.x, s.y + 30, s.w, r_sidebtn(c, 0).y - s.y - 38); }
static struct rect r_chbtn(struct rect c, int i) { struct rect l = r_list(c); return rect_make(l.x + l.w - (2 - i) * 88 + 6, l.y - 30, 82, 24); }
static struct rect r_msg(struct rect c) { struct rect l = r_list(c); return rect_make(l.x, l.y + l.h + 8, l.w, BTN_H + 2); }
static struct rect r_commit(struct rect c) { struct rect m = r_msg(c); return rect_make(m.x + m.w - 110, m.y + m.h + 6, 110, BTN_H); }
static struct rect r_bract(struct rect c, int i) { struct rect d = r_detail(c); return rect_make(d.x, d.y + 30 + i * (BTN_H + 8), 190, BTN_H); }
static struct rect r_brname(struct rect c) { struct rect d = r_detail(c); return rect_make(d.x, d.y + 30 + 3 * (BTN_H + 8) + 30, MIN(260, d.w - 110), BTN_H + 2); }
static struct rect r_brnew(struct rect c) { struct rect f = r_brname(c); return rect_make(f.x + f.w + 8, f.y + 1, 90, BTN_H); }
static struct rect r_dialog(struct rect c) { int h = 90 + G.nf * 40 + 50; return rect_make((c.w - 480) / 2, (c.h - h) / 2, 480, h); }
static struct rect r_dfield(struct rect c, int i) { struct rect d = r_dialog(c); return rect_make(d.x + 140, d.y + 56 + i * 40, d.w - 160, 28); }
static struct rect r_dbtn(struct rect c, int i) { struct rect d = r_dialog(c); return rect_make(d.x + d.w - 20 - (2 - i) * 100 + 10, d.y + d.h - 44, 90, BTN_H); }

static int list_rows(struct rect l, int row_h) { return MAX(1, l.h / row_h); }

/* ---------------- drawing ---------------- */

static void text_clip(struct surface *s, int x, int y, int w, const char *t, color_t c, bool bold)
{
    char buf[320];
    snprintf(buf, sizeof(buf), "%s", t);
    int (*width)(const char *) = bold ? text_width_bold : text_width;
    if (width(buf) > w) {
        size_t n = strlen(buf);
        while (n > 1 && width(buf) > w - 10) {
            buf[--n] = 0;
        }
        if (n > 1)
            buf[n - 1] = 0, strcat(buf, "...");
    }
    if (bold)
        gfx_text_bold(s, x, y, buf, c);
    else
        gfx_text(s, x, y, buf, c);
}

static void draw_text_pane(struct surface *s, struct rect r, struct text *t, const char *empty)
{
    gfx_fill(s, r.x, r.y, r.w, r.h, C_CONTENT);
    gfx_frame(s, r.x, r.y, r.w, r.h, C_LINE);
    struct rect in = rect_make(r.x + 1, r.y + 1, r.w - 2 - SB_W, r.h - 2);
    if (!t->buf || !t->len) {
        if (empty)
            gfx_text(s, in.x + 10, in.y + 10, empty, C_DIM);
        return;
    }
    int lh = gfx_cell_h() + 1, vis = in.h / lh, cw = gfx_cell_w(), maxc = (in.w - 8) / cw;
    struct rect clip = s->clip;
    s->clip = rect_intersect(clip, in);
    int lo = MIN(t->sel_a, t->sel_b), hi = MAX(t->sel_a, t->sel_b);
    for (int i = t->first; i < t->nlines && i < t->first + vis + 1; i++) {
        int y = in.y + 2 + (i - t->first) * lh;
        const char *p = t->buf + t->line[i];
        int n = (i + 1 < t->nlines ? t->line[i + 1] : t->len) - t->line[i];
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r'))
            n--;
        char line[512];
        int k = 0;
        for (int j = 0; j < n && k < maxc && k < (int)sizeof(line) - 1; j++)
            line[k++] = p[j] == '\t' ? ' ' : p[j];
        line[k] = 0;
        color_t c = C_TEXT, bg = 0;
        bool has_bg = false;
        if (line[0] == '+' && strncmp(line, "+++", 3)) {
            c = color_shade(C_GOOD, fct_skin->light ? -30 : 0);
            bg = color_mix(C_CONTENT, C_GOOD, 30), has_bg = true;
        } else if (line[0] == '-' && strncmp(line, "---", 3)) {
            c = color_shade(C_BAD, fct_skin->light ? -20 : 0);
            bg = color_mix(C_CONTENT, C_BAD, 30), has_bg = true;
        } else if (!strncmp(line, "@@", 2)) {
            c = C_BLUE;
        } else if (!strncmp(line, "diff ", 5) || !strncmp(line, "commit ", 7) || !strncmp(line, "index ", 6) ||
                   !strncmp(line, "+++", 3) || !strncmp(line, "---", 3)) {
            c = C_DIM;
        }
        if (t->has_sel && i >= lo && i <= hi)
            bg = C_SELECT, has_bg = true;
        if (has_bg)
            gfx_fill(s, in.x, y - 1, in.w, lh, bg);
        gfx_text_mono(s, in.x + 6, y, line, c);
    }
    s->clip = clip;
    draw_scrollbar(s, rect_make(r.x + r.w - 1 - SB_W, r.y + 1, SB_W, r.h - 2), t->first, vis, MAX(t->nlines, vis));
}

static void status_badge(struct surface *s, int x, int y, char code)
{
    color_t c = code == 'A' || code == '?' ? C_GOOD : code == 'D' ? C_BAD : code == 'R' ? C_BLUE : C_ACCENT;
    char t[2] = { code == '?' ? 'N' : code, 0 };
    gfx_round_rect(s, x, y, 18, 18, 4, c);
    gfx_text_bold(s, x + (18 - text_width_bold(t)) / 2, y + 1, t, fct_skin->light ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x10, 0x10, 0x10));
}

static void draw_changes(struct surface *s, struct rect c)
{
    struct rect l = r_list(c);
    int staged = 0;
    for (int i = 0; i < G.nch; i++)
        staged += G.ch[i].x != ' ' && G.ch[i].x != '?';
    char head[80];
    snprintf(head, sizeof(head), "%d/%d staged", staged, G.nch);
    text_clip(s, l.x, l.y - 25, r_chbtn(c, 0).x - l.x - 4, head, C_TEXT, false);
    ui_button(s, r_chbtn(c, 0), "Stage all", false);
    ui_button(s, r_chbtn(c, 1), "Unstage", false);
    gfx_fill(s, l.x, l.y, l.w, l.h, C_CONTENT);
    gfx_frame(s, l.x, l.y, l.w, l.h, C_LINE);
    int vis = list_rows(l, ROW_H);
    if (!G.nch)
        gfx_text(s, l.x + 10, l.y + 10, G.is_repo ? "Nothing to commit: all is clean." : "", C_DIM);
    for (int i = G.ch_first; i < G.nch && i < G.ch_first + vis; i++) {
        int y = l.y + 1 + (i - G.ch_first) * ROW_H;
        struct change *ch = &G.ch[i];
        if (i == G.ch_sel)
            gfx_fill(s, l.x + 1, y, l.w - 2 - SB_W, ROW_H, C_SELECT);
        bool st = ch->x != ' ' && ch->x != '?';
        struct rect box = rect_make(l.x + 8, y + 5, 14, 14);   /* the stage box */
        gfx_fill(s, box.x, box.y, box.w, box.h, C_CONTENT);
        gfx_frame(s, box.x, box.y, box.w, box.h, C_DIM);
        if (st) {
            gfx_thick_line(s, box.x + 3, box.y + 7, box.x + 6, box.y + 10, 2, C_ACCENT);
            gfx_thick_line(s, box.x + 6, box.y + 10, box.x + 11, box.y + 3, 2, C_ACCENT);
        }
        status_badge(s, l.x + 30, y + 3, st ? ch->x : ch->y == ' ' ? ch->x : ch->y);
        text_clip(s, l.x + 56, y + 4, l.w - 64 - SB_W, ch->path, C_TEXT, false);
    }
    draw_scrollbar(s, rect_make(l.x + l.w - 1 - SB_W, l.y + 1, SB_W, l.h - 2), G.ch_first, vis, MAX(G.nch, vis));
    struct rect m = r_msg(c);
    fct_field_draw(s, m, &G.msg, G.focus == 1, "Commit message");
    text_clip(s, m.x, m.y + m.h + 10, r_commit(c).x - m.x - 6, staged ? "" : "Tick to stage", C_DIM, false);
    ui_button(s, r_commit(c), "Commit", false);
    draw_text_pane(s, r_detail(c), &G.detail, "Select a file to see its changes.");
}

static void draw_history(struct surface *s, struct rect c)
{
    struct rect l = r_list(c);
    gfx_fill(s, l.x, l.y, l.w, l.h, C_CONTENT);
    gfx_frame(s, l.x, l.y, l.w, l.h, C_LINE);
    int rh = 2 * FONT_H + 8, vis = list_rows(l, rh);
    if (!G.nlog)
        gfx_text(s, l.x + 10, l.y + 10, "No commits yet.", C_DIM);
    for (int i = G.log_first; i < G.nlog && i < G.log_first + vis; i++) {
        int y = l.y + 1 + (i - G.log_first) * rh;
        struct commit *k = &G.log[i];
        if (i == G.log_sel)
            gfx_fill(s, l.x + 1, y, l.w - 2 - SB_W, rh, C_SELECT);
        else if (i % 2)
            gfx_fill(s, l.x + 1, y, l.w - 2 - SB_W, rh, C_CONTENT_ALT);
        text_clip(s, l.x + 10, y + 3, l.w - 20 - SB_W, k->subject, C_TEXT, false);
        char meta[160];
        snprintf(meta, sizeof(meta), "%s  %s, %s", k->hash, k->author, k->when);
        text_clip(s, l.x + 10, y + 4 + FONT_H, l.w - 20 - SB_W, meta, C_DIM, false);
    }
    draw_scrollbar(s, rect_make(l.x + l.w - 1 - SB_W, l.y + 1, SB_W, l.h - 2), G.log_first, vis, MAX(G.nlog, vis));
    draw_text_pane(s, r_detail(c), &G.detail, "Select a commit to see it.");
}

static void draw_branches(struct surface *s, struct rect c)
{
    struct rect l = r_list(c);
    gfx_fill(s, l.x, l.y, l.w, l.h, C_CONTENT);
    gfx_frame(s, l.x, l.y, l.w, l.h, C_LINE);
    int vis = list_rows(l, ROW_H);
    bool remote_head = false;
    for (int i = G.br_first, row = 0; i < G.nbr && row < vis; i++, row++) {
        int y = l.y + 1 + row * ROW_H;
        struct branch *b = &G.br[i];
        if (b->remote && !remote_head) {
            remote_head = true;
        }
        if (i == G.br_sel)
            gfx_fill(s, l.x + 1, y, l.w - 2 - SB_W, ROW_H, C_SELECT);
        if (b->current)
            gfx_disc(s, l.x + 14, y + ROW_H / 2, 4, C_ACCENT);
        icon_draw(s, ICON_GIT, l.x + 24, y + 4, 16);
        text_clip(s, l.x + 46, y + 4, l.w - 56 - SB_W, b->name, b->remote ? C_DIM : C_TEXT, b->current);
    }
    draw_scrollbar(s, rect_make(l.x + l.w - 1 - SB_W, l.y + 1, SB_W, l.h - 2), G.br_first, vis, MAX(G.nbr, vis));
    struct rect d = r_detail(c);
    const char *sel = G.br_sel >= 0 ? G.br[G.br_sel].name : NULL;
    char t[180];
    snprintf(t, sizeof(t), "%s", sel ? sel : "Select a branch");
    text_clip(s, d.x, d.y + 4, d.w, t, C_TEXT, true);
    static const char *const acts[] = { "Checkout", "Merge into current", "Delete" };
    for (int i = 0; i < 3; i++)
        ui_button(s, r_bract(c, i), acts[i], false);
    struct rect f = r_brname(c);
    gfx_text_bold(s, f.x, f.y - 22, "New branch (from the current one)", C_TEXT);
    fct_field_draw(s, f, &G.newbranch, G.focus == 2, "name");
    ui_button(s, r_brnew(c), "Create", false);
    int y = f.y + f.h + 24;
    gfx_text_bold(s, d.x, y, "Remotes", C_TEXT);
    y += FONT_H + 6;
    char rem[1024];
    snprintf(rem, sizeof(rem), "%s", G.remotes[0] ? G.remotes : "(none: Clone sets origin; git remote add NAME URL)");
    for (char *ln = strtok(rem, "\n"); ln && y < d.y + d.h - FONT_H; ln = strtok(NULL, "\n")) {
        text_clip(s, d.x, y, d.w, ln, C_DIM, false);
        y += FONT_H + 2;
    }
}

static void git_draw(struct fct_view *v, struct surface *s, struct rect c)
{
    (void)v;
    gfx_fill(s, c.x, c.y, c.w, c.h, C_FACE);
    /* the toolbar */
    gfx_fill(s, 0, 0, c.w, TOOLBAR_H, fct_skin->light ? C_FACE : color_shade(C_FACE, 8));
    gfx_hline(s, 0, TOOLBAR_H - 1, c.w, C_LINE);
    icon_draw(s, ICON_GIT, PADX, (TOOLBAR_H - 32) / 2, 32);
    int x = PADX + 42;
    if (repo()) {
        x += gfx_text_bold(s, x, 6, base_name(repo()), C_TEXT) + 12;
        char b[220];
        if (G.is_repo) {
            snprintf(b, sizeof(b), "%s%s%s", G.branch[0] ? G.branch : "(detached)", G.upstream[0] ? "  ->  " : "",
                     G.upstream);
            int bw = text_width(b) + 16;
            gfx_round_rect(s, PADX + 42, 25, bw, 18, 9, color_mix(C_FACE, C_ACCENT, 60));
            gfx_text(s, PADX + 50, 26, b, C_TEXT);
            char ab[60] = "";
            if (G.ahead || G.behind)
                snprintf(ab, sizeof(ab), "%d to push, %d to pull", G.ahead, G.behind);
            gfx_text(s, PADX + 42 + bw + 10, 26, ab, C_DIM);
        }
        gfx_text(s, x, 6, repo(), C_DIM);
    } else {
        gfx_text_bold(s, x, 14, "Git", C_TEXT);
    }
    static const char *const tb[] = { "Fetch", "Pull", "Push", "Refresh", "Settings" };
    for (int i = 0; i < 5; i++)
        ui_button(s, r_toolbtn(c, i), tb[i], false);

    /* the sidebar */
    struct rect sb = r_sidebar(c);
    gfx_fill(s, sb.x, sb.y, sb.w, sb.h, fct_skin->light ? C_CONTENT_ALT : color_shade(C_FACE, -6));
    gfx_vline(s, sb.x + sb.w - 1, sb.y, sb.h, C_LINE);
    gfx_text_bold(s, PADX, sb.y + 8, "Repositories", C_DIM);
    struct rect rl = r_repos(c);
    int vis = MAX(1, rl.h / REPO_H);
    if (!G.nrepos)
        gfx_text(s, PADX, rl.y + 6, "None yet: Add, Clone or New.", C_DIM);
    for (int i = G.repo_first; i < G.nrepos && i < G.repo_first + vis; i++) {
        int y = rl.y + (i - G.repo_first) * REPO_H;
        if (i == G.cur)
            gfx_fill(s, rl.x + 4, y, rl.w - 8, REPO_H - 2, C_SELECT);
        icon_draw(s, ICON_FOLDER, rl.x + 10, y + 5, 28);
        text_clip(s, rl.x + 44, y + 4, rl.w - 52, base_name(G.repos[i]), C_TEXT, i == G.cur);
        text_clip(s, rl.x + 44, y + 4 + FONT_H, rl.w - 52, G.repos[i], C_DIM, false);
    }
    static const char *const sbtn[] = { "Add", "Clone", "New", "Remove" };
    for (int i = 0; i < 4; i++)
        ui_button(s, r_sidebtn(c, i), sbtn[i], false);

    /* the tabs and their pages */
    struct rect m = r_main(c);
    gfx_hline(s, m.x, m.y + TABS_H - 1, m.w, C_LINE);
    for (int i = 0; i < NTABS; i++) {
        struct rect t = r_tab(c, i);
        char label[40];
        if (i == TAB_CHANGES)
            snprintf(label, sizeof(label), G.nch ? "Changes (%d)" : "Changes", G.nch);
        else
            snprintf(label, sizeof(label), "%s", i == TAB_HISTORY ? "History" : "Branches");
        if (i == G.tab) {
            gfx_fill(s, t.x, t.y, t.w, t.h + 1, C_FACE);
            gfx_frame(s, t.x, t.y, t.w, t.h + 1, C_LINE);
            gfx_fill(s, t.x + 1, t.y + t.h - 1, t.w - 2, 3, C_FACE);
            gfx_fill(s, t.x + 1, t.y, t.w - 2, 3, C_ACCENT);
            gfx_text_bold(s, t.x + (t.w - text_width_bold(label)) / 2, t.y + 7, label, C_TEXT);
        } else {
            gfx_text(s, t.x + (t.w - text_width(label)) / 2, t.y + 7, label, C_DIM);
        }
    }
    if (access(GIT, X_OK) < 0) {
        gfx_text_bold(s, m.x + 20, m.y + TABS_H + 20, "git is not installed: install the package git (SiPM, or pkg install git).",
                      C_BAD);
    } else if (!repo()) {
        gfx_text(s, m.x + 20, m.y + TABS_H + 20, "Add a repository you have, Clone one, or start a New one (on the left).", C_DIM);
    } else if (G.tab == TAB_CHANGES) {
        draw_changes(s, c);
    } else if (G.tab == TAB_HISTORY) {
        draw_history(s, c);
    } else {
        draw_branches(s, c);
    }

    /* the output */
    struct rect o = r_output(c);
    gfx_fill(s, o.x, o.y, o.w, o.h, fct_skin->light ? C_CONTENT_ALT : color_shade(C_FACE, -6));
    gfx_hline(s, o.x, o.y, o.w, C_LINE);
    char head[260];
    if (G.job > 0) {
        static const char *const dots[] = { ".  ", ".. ", "..." };
        snprintf(head, sizeof(head), "%s  running%s", G.job_label, dots[G.spin % 3]);
    } else {
        snprintf(head, sizeof(head), "%s", G.last_cmd[0] ? G.last_cmd : "Output");
    }
    gfx_text_bold(s, o.x + PADX, o.y + 6, head, G.job > 0 || G.last_ok || !G.last_cmd[0] ? C_TEXT : C_BAD);
    /* its last lines (git's progress uses carriage returns: the last of each) */
    int lh = gfx_cell_h() + 1, nl = (o.h - 30) / lh, maxc = (o.w - 2 * PADX) / gfx_cell_w();
    const char *lines[16];
    int n = 0;
    for (const char *p = G.out + G.outlen; p > G.out && n < nl; ) {
        const char *end = p;
        while (end > G.out && (end[-1] == '\n' || end[-1] == '\r'))
            end--;
        const char *st = end;
        while (st > G.out && st[-1] != '\n' && st[-1] != '\r')
            st--;
        if (end > st)
            lines[n++] = st;
        p = st;
        if (st == G.out)
            break;
    }
    for (int i = 0; i < n; i++) {
        const char *st = lines[n - 1 - i];
        char buf[400];
        int k = 0;
        while (st[k] && st[k] != '\n' && st[k] != '\r' && k < maxc && k < (int)sizeof(buf) - 1) {
            buf[k] = st[k];
            k++;
        }
        buf[k] = 0;
        gfx_text_mono(s, o.x + PADX, o.y + 26 + i * lh, buf, C_DIM);
    }

    /* a dialog */
    if (G.dlg) {
        gfx_blend_fill(s, 0, 0, c.w, c.h, RGB(0, 0, 0), 90);
        struct rect d = r_dialog(c);
        gfx_shadow(s, d, 8, 14, 6, 160);
        gfx_fill(s, d.x, d.y, d.w, d.h, C_FACE);
        gfx_frame(s, d.x, d.y, d.w, d.h, C_FACE_DARK);
        icon_draw(s, ICON_GIT, d.x + 16, d.y + 12, 24);
        gfx_text_bold(s, d.x + 48, d.y + 16, dlg_titles[G.dlg], C_TEXT);
        for (int i = 0; i < G.nf; i++) {
            struct rect f = r_dfield(c, i);
            gfx_text(s, d.x + 20, f.y + 6, dlg_label(i), C_TEXT);
            const char *hint = G.dlg == DLG_CLONE && i == 0 ? "https://github.com/USER/REPO.git"
                             : G.dlg == DLG_SETTINGS && i == 4 ? (G.have_token ? "(kept: type a new one to replace it)"
                                                                                 : "a personal access token") : NULL;
            fct_field_draw(s, f, &G.f[i], G.focus == 10 + i, hint);
        }
        if (G.dlg_msg[0])
            gfx_text(s, d.x + 20, d.y + d.h - 40, G.dlg_msg, C_BAD);
        else if (G.dlg == DLG_SETTINGS)
            gfx_text(s, d.x + 20, d.y + d.h - 40, "The token is kept in ~/.git-credentials.", C_DIM);
        ui_button(s, r_dbtn(c, 0), "Cancel", false);
        ui_button(s, r_dbtn(c, 1), G.dlg == DLG_CLONE ? "Clone" : G.dlg == DLG_SETTINGS ? "Save" : "OK", false);
    }
}

/* ---------------- input ---------------- */

static void list_scroll(int *first, int n, int vis, int d)
{
    *first += d;
    if (*first > n - vis)
        *first = n - vis;
    if (*first < 0)
        *first = 0;
}

static void ensure_visible(int sel, int *first, int vis)
{
    if (sel < *first)
        *first = sel;
    else if (sel >= *first + vis)
        *first = sel - vis + 1;
}

static void select_row(int i)
{
    if (G.tab == TAB_CHANGES && i >= 0 && i < G.nch) {
        G.ch_sel = i;
        show_change(i);
    } else if (G.tab == TAB_HISTORY && i >= 0 && i < G.nlog) {
        G.log_sel = i;
        show_commit(i);
    } else if (G.tab == TAB_BRANCHES && i >= 0 && i < G.nbr) {
        G.br_sel = i;
    }
}

static void copy_detail(void)
{
    struct text *t = &G.detail;
    if (!t->has_sel || !t->buf)
        return;
    int lo = MIN(t->sel_a, t->sel_b), hi = MAX(t->sel_a, t->sel_b);
    int a = t->line[lo], b = hi + 1 < t->nlines ? t->line[hi + 1] : t->len;
    fct_clipboard_set(t->buf + a, b - a);
}

static void branch_action(int i)
{
    if (G.br_sel < 0)
        return;
    struct branch *b = &G.br[G.br_sel];
    char name[130];
    snprintf(name, sizeof(name), "%s", b->name);
    if (i == 0) {
        if (b->remote) {                           /* a remote branch: a local one tracking it */
            const char *sl = strchr(name, '/');
            char *a[] = { "switch", "--track", name, NULL };
            (void)sl;
            git_do("git switch --track", a);
        } else {
            char *a[] = { "switch", name, NULL };
            git_do("git switch", a);
        }
    } else if (i == 1) {
        char *a[] = { "merge", "--no-edit", name, NULL };
        git_do("git merge", a);
    } else if (!b->remote && !b->current) {
        char *a[] = { "branch", "-d", name, NULL };
        git_do("git branch -d", a);
    } else {
        out_set("delete", "Only a local branch other than the current one can be deleted here.", 66, false);
    }
    refresh();
}

static void git_mouse(struct fct_view *v, int x, int y, int kind, int buttons)
{
    struct rect c = fct_view_content(v);
    if (kind == FCT_MOUSE_WHEEL) {
        int d = buttons * 3;
        if (rect_contains(r_repos(c), x, y)) {
            list_scroll(&G.repo_first, G.nrepos, MAX(1, r_repos(c).h / REPO_H), buttons);
        } else if (repo() && rect_contains(r_detail(c), x, y) && G.tab != TAB_BRANCHES) {
            int vis = (r_detail(c).h - 2) / (gfx_cell_h() + 1);
            list_scroll(&G.detail.first, G.detail.nlines, vis, d);
        } else if (repo() && rect_contains(r_list(c), x, y)) {
            struct rect l = r_list(c);
            if (G.tab == TAB_CHANGES)
                list_scroll(&G.ch_first, G.nch, list_rows(l, ROW_H), buttons * 2);
            else if (G.tab == TAB_HISTORY)
                list_scroll(&G.log_first, G.nlog, list_rows(l, 2 * FONT_H + 8), buttons * 2);
            else
                list_scroll(&G.br_first, G.nbr, list_rows(l, ROW_H), buttons * 2);
        }
        fct_view_invalidate(v);
        return;
    }
    if (G.dlg) {
        for (int i = 0; i < G.nf; i++)
            if ((kind != FCT_MOUSE_MOVE || G.f[i].dragging) && fct_field_mouse(&G.f[i], r_dfield(c, i), x, y, kind)) {
                G.focus = 10 + i;
                fct_view_invalidate(v);
                return;
            }
        if (kind == FCT_MOUSE_DOWN && (buttons & 1)) {
            if (rect_contains(r_dbtn(c, 0), x, y))
                G.dlg = DLG_NONE;
            else if (rect_contains(r_dbtn(c, 1), x, y))
                dlg_ok(v);
            fct_view_invalidate(v);
        }
        return;
    }
    if (repo() && G.tab == TAB_CHANGES && (kind != FCT_MOUSE_MOVE || G.msg.dragging) &&
        fct_field_mouse(&G.msg, r_msg(c), x, y, kind)) {
        G.focus = 1;
        fct_view_invalidate(v);
        return;
    }
    if (repo() && G.tab == TAB_BRANCHES && (kind != FCT_MOUSE_MOVE || G.newbranch.dragging) &&
        fct_field_mouse(&G.newbranch, r_brname(c), x, y, kind)) {
        G.focus = 2;
        fct_view_invalidate(v);
        return;
    }
    struct text *t = &G.detail;
    struct rect dr = r_detail(c);
    int lh = gfx_cell_h() + 1;
    if (t->selecting && kind == FCT_MOUSE_MOVE) {
        int i = t->first + (y - dr.y - 3) / lh;
        t->sel_b = MAX(0, MIN(t->nlines - 1, i));
        t->has_sel = true;
        fct_view_invalidate(v);
        return;
    }
    if (kind == FCT_MOUSE_UP) {
        t->selecting = false;
        return;
    }
    if ((kind != FCT_MOUSE_DOWN && kind != FCT_MOUSE_DOUBLE) || !(buttons & 1))
        return;
    G.focus = 0;
    /* the toolbar */
    static const char *const ops[] = { "fetch", "pull", "push" };
    for (int i = 0; i < 5; i++)
        if (rect_contains(r_toolbtn(c, i), x, y)) {
            if (i < 3)
                remote_op(ops[i]);
            else if (i == 3)
                refresh();
            else
                dlg_open(DLG_SETTINGS);
            fct_view_invalidate(v);
            return;
        }
    /* the sidebar */
    for (int i = 0; i < 4; i++)
        if (rect_contains(r_sidebtn(c, i), x, y)) {
            if (i == 0)
                dlg_open(DLG_ADD);
            else if (i == 1)
                dlg_open(DLG_CLONE);
            else if (i == 2)
                dlg_open(DLG_NEW);
            else if (G.cur >= 0) {                 /* Remove from the list (the folder stays) */
                memmove(&G.repos[G.cur], &G.repos[G.cur + 1], (G.nrepos - G.cur - 1) * sizeof(G.repos[0]));
                G.nrepos--;
                repos_save();
                select_repo(v, G.nrepos ? 0 : -1);
                out_set("remove", "Removed from the list (the folder is still there).", 50, true);
            }
            fct_view_invalidate(v);
            return;
        }
    struct rect rl = r_repos(c);
    if (rect_contains(rl, x, y)) {
        int i = G.repo_first + (y - rl.y) / REPO_H;
        if (i < G.nrepos && i != G.cur)
            select_repo(v, i);
        fct_view_invalidate(v);
        return;
    }
    if (!repo())
        return;
    for (int i = 0; i < NTABS; i++)
        if (rect_contains(r_tab(c, i), x, y)) {
            G.tab = i;
            refresh();
            fct_view_invalidate(v);
            return;
        }
    struct rect l = r_list(c);
    if (G.tab == TAB_CHANGES) {
        if (rect_contains(r_chbtn(c, 0), x, y)) {
            char *a[] = { "add", "-A", NULL };
            git_do("git add -A", a);
            refresh();
        } else if (rect_contains(r_chbtn(c, 1), x, y)) {
            char *a[] = { "restore", "--staged", ":/", NULL };
            if (!git_do("git restore --staged", a)) {
                char *b[] = { "rm", "--cached", "-r", "-q", ":/", NULL };
                git_do("git rm --cached", b);
            }
            refresh();
        } else if (rect_contains(r_commit(c), x, y)) {
            commit();
        } else if (rect_contains(rect_make(l.x, l.y, l.w - SB_W, l.h), x, y)) {
            int i = G.ch_first + (y - l.y - 1) / ROW_H;
            if (i < G.nch) {
                if (x < l.x + 26) {                /* the box */
                    toggle_stage(i);
                    G.ch_sel = i;
                    refresh();
                } else {
                    select_row(i);
                }
            }
        } else if (rect_contains(rect_make(l.x + l.w - SB_W, l.y, SB_W, l.h), x, y)) {
            G.ch_first = scrollbar_click(rect_make(l.x + l.w - SB_W, l.y, SB_W, l.h), y, G.ch_first, list_rows(l, ROW_H), G.nch);
        }
    } else if (G.tab == TAB_HISTORY) {
        int rh = 2 * FONT_H + 8;
        if (rect_contains(rect_make(l.x, l.y, l.w - SB_W, l.h), x, y)) {
            int i = G.log_first + (y - l.y - 1) / rh;
            select_row(i);
        } else if (rect_contains(rect_make(l.x + l.w - SB_W, l.y, SB_W, l.h), x, y)) {
            G.log_first = scrollbar_click(rect_make(l.x + l.w - SB_W, l.y, SB_W, l.h), y, G.log_first, list_rows(l, rh), G.nlog);
        }
    } else {
        if (rect_contains(rect_make(l.x, l.y, l.w - SB_W, l.h), x, y)) {
            int i = G.br_first + (y - l.y - 1) / ROW_H;
            select_row(i);
            if (kind == FCT_MOUSE_DOUBLE)
                branch_action(0);
        }
        for (int i = 0; i < 3; i++)
            if (rect_contains(r_bract(c, i), x, y))
                branch_action(i);
        if (rect_contains(r_brnew(c), x, y) && G.newbranch.text[0]) {
            char *a[] = { "switch", "-c", G.newbranch.text, NULL };
            if (git_do("git switch -c", a))
                fct_field_set(&G.newbranch, "");
            refresh();
        }
    }
    /* the text pane: lines selected for Ctrl+C */
    if (G.tab != TAB_BRANCHES && rect_contains(dr, x, y) && t->buf) {
        if (x >= dr.x + dr.w - SB_W - 1) {
            int vis = (dr.h - 2) / lh;
            t->first = scrollbar_click(rect_make(dr.x + dr.w - SB_W, dr.y, SB_W, dr.h), y, t->first, vis, t->nlines);
        } else {
            int i = MAX(0, MIN(t->nlines - 1, t->first + (y - dr.y - 3) / lh));
            t->sel_a = t->sel_b = i;
            t->has_sel = false;
            t->selecting = true;
        }
    }
    fct_view_invalidate(v);
}

static void git_key(struct fct_view *v, const struct fct_key *k)
{
    if (!k->value)
        return;
    struct rect c = fct_view_content(v);
    bool ctrl = k->mods & FCT_MOD_CTRL;
    if (G.dlg) {
        if (k->ascii == 27) {
            G.dlg = DLG_NONE;
        } else if (k->ascii == '\t') {
            G.focus = 10 + (G.focus - 10 + 1) % G.nf;
        } else if (k->ascii == '\n' || k->ascii == '\r') {
            dlg_ok(v);
        } else if (G.focus >= 10) {
            fct_field_key(&G.f[G.focus - 10], k);
        }
        fct_view_invalidate(v);
        return;
    }
    if (G.focus == 1 && G.tab == TAB_CHANGES) {
        if (k->ascii == '\n' || k->ascii == '\r')
            commit();
        else if (k->ascii == 27)
            G.focus = 0;
        else
            fct_field_key(&G.msg, k);
        fct_view_invalidate(v);
        return;
    }
    if (G.focus == 2 && G.tab == TAB_BRANCHES) {
        if ((k->ascii == '\n' || k->ascii == '\r') && G.newbranch.text[0]) {
            char *a[] = { "switch", "-c", G.newbranch.text, NULL };
            if (git_do("git switch -c", a))
                fct_field_set(&G.newbranch, "");
            refresh();
        } else if (k->ascii == 27) {
            G.focus = 0;
        } else {
            fct_field_key(&G.newbranch, k);
        }
        fct_view_invalidate(v);
        return;
    }
    if (ctrl && k->code == 0x2E) {                 /* Ctrl+C: the lines selected */
        copy_detail();
        return;
    }
    if (k->code == FCT_KEY_F1 + 4) {               /* F5 */
        refresh();
    } else if (k->code == FCT_KEY_UP || k->code == FCT_KEY_DOWN) {
        int d = k->code == FCT_KEY_UP ? -1 : 1;
        struct rect l = r_list(c);
        if (G.tab == TAB_CHANGES && G.nch) {
            select_row(MAX(0, MIN(G.nch - 1, G.ch_sel + d)));
            ensure_visible(G.ch_sel, &G.ch_first, list_rows(l, ROW_H));
        } else if (G.tab == TAB_HISTORY && G.nlog) {
            select_row(MAX(0, MIN(G.nlog - 1, G.log_sel + d)));
            ensure_visible(G.log_sel, &G.log_first, list_rows(l, 2 * FONT_H + 8));
        } else if (G.tab == TAB_BRANCHES && G.nbr) {
            select_row(MAX(0, MIN(G.nbr - 1, G.br_sel + d)));
            ensure_visible(G.br_sel, &G.br_first, list_rows(l, ROW_H));
        }
    } else if (k->ascii == ' ' && G.tab == TAB_CHANGES && G.ch_sel >= 0) {   /* Space: stage or unstage */
        toggle_stage(G.ch_sel);
        refresh();
    } else if (k->code == FCT_KEY_PGUP || k->code == FCT_KEY_PGDN) {
        int vis = (r_detail(c).h - 2) / (gfx_cell_h() + 1);
        list_scroll(&G.detail.first, G.detail.nlines, vis, k->code == FCT_KEY_PGUP ? -vis : vis);
    }
    fct_view_invalidate(v);
}

/* ---------------- the background job ---------------- */

static int git_pollfd(struct fct_view *v) { (void)v; return G.job_fd; }

static void git_readable(struct fct_view *v)
{
    char buf[4096];
    long n = read(G.job_fd, buf, sizeof(buf));
    if (n > 0) {
        size_t room = sizeof(G.out) - 1 - G.outlen;
        if ((size_t)n > room) {                    /* (keep the end) */
            size_t drop = n - room;
            memmove(G.out, G.out + drop, G.outlen - drop);
            G.outlen -= (int)drop;
        }
        memcpy(G.out + G.outlen, buf, n);
        G.outlen += n;
        G.out[G.outlen] = 0;
        fct_view_invalidate(v);
        return;
    }
    close(G.job_fd);
    G.job_fd = -1;
    int st = 0;
    waitpid(G.job, &st, 0);
    G.job = 0;
    G.last_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    snprintf(G.last_cmd, sizeof(G.last_cmd), "%s: %s", G.job_label, G.last_ok ? "done" : "failed");
    if (G.last_ok && G.job_after == AFTER_ADD && G.job_path[0]) {
        repos_add(G.job_path);
        select_repo(v, G.cur);
    } else {
        refresh();
    }
    fct_view_invalidate(v);
}

static void git_tick(struct fct_view *v)
{
    if (G.job > 0) {
        G.spin++;
        fct_view_invalidate(v);
    }
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (fct_app_init() < 0)
        return 1;
    repos_load();
    if (argc > 1 && argv[1][0] == '/') {           /* facet-git /path/to/repository */
        char *o;
        char *args[] = { "rev-parse", "--show-toplevel", NULL };
        if (git_run(argv[1], args, &o, NULL) == 0 && o) {
            o[strcspn(o, "\n")] = 0;
            repos_add(o);
        }
        free(o);
    }
    int sw, sh;                                     /* (as big as fits beside the desktop's dock) */
    fct_screen(&sw, &sh);
    struct fct_window_attr a = { "Git", FCT_POS_AUTO, FCT_POS_AUTO, MAX(760, MIN(980, sw - 240)), MAX(460, MIN(620, sh - 140)),
                                 720, 440, FCT_WIN_POINTER };
    struct fct_view *v = fct_view_create(&a);
    if (!v)
        return 1;
    select_repo(v, G.cur >= 0 ? G.cur : G.nrepos ? 0 : -1);
    v->draw = git_draw;
    v->mouse = git_mouse;
    v->key = git_key;
    v->tick = git_tick;
    v->pollfd = git_pollfd;
    v->readable = git_readable;
    return fct_main();
}
