/*
 * apptools.c - MiR's tools (Make it Real): the model writes an application
 * the user describes, builds it with SIEOS's own compilers, runs and tests
 * it, and installs it.  And MiR's instructions to the model (the guide,
 * share/guide.txt, and the state of things).  On libsia (sia_add_tool).
 *
 * Each application is a project, ~/apps/NAME: its sources, a Makefile and
 * mir.json (name, title, kind, language, summary, installed).  The tools
 * only touch the current project's folder (no "..", no symbolic links), and
 * read reference files under /usr/include and /usr/pkg/share/mir.  What the
 * application does runs as the user, in its folder, with limits (processor
 * time, memory, file size).  A window application stays open after a run,
 * for the model to look at (app_screenshot, when it sees images) and to
 * try (app_input), and for the user.  Installing (~/apps/bin, the desktop
 * menu's "My apps") asks the user first; everything else does not.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "libsia.h"
#include "mir.h"
#include <ctype.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>

#define SHARE        MIR_SHARE
#define FILE_MAX     (256 * 1024)       /* bytes a source file may have */
#define READ_MAX     (64 * 1024)        /* bytes app_read returns */
#define RESULT_MAX   12000              /* bytes of output sent back to the model */
#define BUILD_SECS   300
#define RUN_SECS     20

static struct {
    char root[256];                     /* ~/apps */
    char name[40], dir[320];            /* the current project ("" none) */
    char kind[16], lang[8];
    pid_t app;                          /* a window application running, 0 none */
} A;

/* ---------------- helpers ---------------- */

static bool valid_name(const char *n)
{
    size_t l = strlen(n);
    if (l < 1 || l > 32 || !islower((unsigned char)n[0]))
        return false;
    for (const char *p = n; *p; p++)
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '-'))
            return false;
    return true;
}

/* A path inside the project: relative, plain names (letters, digits, . _ -), not hidden, three levels at most. */
static bool valid_rel(const char *p)
{
    if (!*p || *p == '/' || strlen(p) > 120)
        return false;
    int depth = 0;
    bool start = true;
    for (const char *c = p; *c; c++) {
        if (*c == '/') {
            if (start)
                return false;
            start = true;
            if (++depth > 2)
                return false;
            continue;
        }
        if (start && *c == '.')
            return false;
        start = false;
        if (!(isalnum((unsigned char)*c) || *c == '.' || *c == '_' || *c == '-'))
            return false;
    }
    return !start;
}

static bool need_project(struct sbuf *result)
{
    if (A.name[0])
        return true;
    sb_puts(result, "exit status: 1\nerror: no project yet: app_create makes one, app_open continues one (app_list)\n");
    return false;
}

static char *read_file(const char *path, size_t max, size_t *len)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
    size_t n = (size_t)st.st_size < max ? (size_t)st.st_size : max;
    char *buf = malloc(n + 1);
    size_t got = 0;
    while (buf && got < n) {
        long r = read(fd, buf + got, n - got);
        if (r <= 0)
            break;
        got += r;
    }
    close(fd);
    if (buf)
        buf[got] = 0;
    if (len)
        *len = got;
    return buf;
}

static bool write_file_at(const char *path, const char *text, size_t n)
{
    char tmp[400];
    snprintf(tmp, sizeof(tmp), "%s.mir-new", path);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0)
        return false;
    size_t done = 0;
    while (done < n) {
        long r = write(fd, text + done, n - done);
        if (r <= 0)
            break;
        done += r;
    }
    close(fd);
    struct stat st;
    if (done != n || (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) || rename(tmp, path) < 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

/* mir.json of project dir */
static struct json *manifest(const char *dir)
{
    char p[400];
    snprintf(p, sizeof(p), "%s/mir.json", dir);
    size_t n;
    char *t = read_file(p, 16384, &n);
    struct json *j = t ? json_parse(t, n) : NULL;
    free(t);
    return j;
}

static void save_manifest(const char *title, const char *summary, bool installed)
{
    struct sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"name\":");
    sb_json_str(&b, A.name);
    sb_puts(&b, ",\"title\":");
    sb_json_str(&b, title ? title : A.name);
    sb_puts(&b, ",\"kind\":");
    sb_json_str(&b, A.kind);
    sb_puts(&b, ",\"language\":");
    sb_json_str(&b, A.lang);
    sb_puts(&b, ",\"summary\":");
    sb_json_str(&b, summary ? summary : "");
    sb_printf(&b, ",\"installed\":%s}\n", installed ? "true" : "false");
    char p[400];
    snprintf(p, sizeof(p), "%s/mir.json", A.dir);
    write_file_at(p, b.s, b.len);
    sb_free(&b);
}

static bool select_project(const char *name)
{
    char dir[320];
    snprintf(dir, sizeof(dir), "%s/%s", A.root, name);
    struct json *m = manifest(dir);
    if (!m)
        return false;
    snprintf(A.name, sizeof(A.name), "%s", name);
    snprintf(A.dir, sizeof(A.dir), "%s", dir);
    const char *k = json_get_str(m, "kind"), *l = json_get_str(m, "language");
    snprintf(A.kind, sizeof(A.kind), "%s", k && !strcmp(k, "terminal") ? "terminal" : "window");
    snprintf(A.lang, sizeof(A.lang), "%s", l && !strcmp(l, "c++") ? "c++" : "c");
    json_free(m);
    return true;
}

/* ---------------- running programs ---------------- */

static void limits(int cpu_secs, long mem_mb, long file_mb)
{
    struct rlimit r;
    if (cpu_secs > 0) {
        r.rlim_cur = r.rlim_max = cpu_secs;
        setrlimit(RLIMIT_CPU, &r);
    }
    r.rlim_cur = r.rlim_max = (rlim_t)mem_mb << 20;
    setrlimit(RLIMIT_AS, &r);
    r.rlim_cur = r.rlim_max = (rlim_t)file_mb << 20;
    setrlimit(RLIMIT_FSIZE, &r);
}

static void strip_ansi(struct sbuf *dst, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\033' && i + 1 < n && s[i + 1] == '[') {
            i += 2;
            while (i < n && !(s[i] >= '@' && s[i] <= '~'))
                i++;
            continue;
        }
        sb_putc(dst, s[i]);
    }
}

/* The output, its start and end if it is long */
static void put_output(struct sbuf *result, const struct sbuf *raw)
{
    struct sbuf c;
    sb_init(&c);
    strip_ansi(&c, raw->s ? raw->s : "", raw->len);
    if (c.len <= RESULT_MAX) {
        sb_putn(result, c.s ? c.s : "", c.len);
    } else {
        sb_putn(result, c.s, RESULT_MAX / 2);
        sb_printf(result, "\n[... %lu bytes omitted ...]\n", (unsigned long)(c.len - RESULT_MAX));
        sb_putn(result, c.s + c.len - RESULT_MAX / 2, RESULT_MAX / 2);
    }
    sb_free(&c);
}

/* Run argv in the project's folder, output collected (and shown when show);
 * the exit status, or 128 + the signal; *timed_out when it was stopped. */
static int run_in(struct sia_session *s, char *const argv[], const char *input, int secs, int cpu, long mem_mb,
                  struct sbuf *out, bool show, bool *timed_out)
{
    int po[2], pi[2] = { -1, -1 };
    *timed_out = false;
    if (pipe(po) < 0 || (input && pipe(pi) < 0))
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setpgid(0, 0);
        if (chdir(A.dir) < 0)
            _exit(126);
        if (input) {
            dup2(pi[0], 0);
        } else {
            int nul = open("/dev/null", O_RDONLY);
            dup2(nul, 0);
        }
        dup2(po[1], 1);
        dup2(po[1], 2);
        for (int fd = 3; fd < 64; fd++)
            close(fd);
        unsetenv("SIEOS_DESKTOP");
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        limits(cpu, mem_mb, 256);
        execvp(argv[0], argv);
        dprintf(2, "%s: cannot run it\n", argv[0]);
        _exit(127);
    }
    close(po[1]);
    if (input) {
        close(pi[0]);
        write(pi[1], input, strlen(input));
        close(pi[1]);
    }
    long start = uptime_ms();
    char buf[2048];
    bool killed = false;
    for (;;) {
        struct pollfd p = { po[0], POLLIN, 0 };
        if (poll(&p, 1, 250) > 0) {
            long n = read(po[0], buf, sizeof(buf));
            if (n <= 0)
                break;
            if (show)
                sia_output(s, buf, n);
            if (out->len < 256 * 1024)
                sb_putn(out, buf, n);
        }
        if (!killed && (sia_was_interrupted() || uptime_ms() - start > secs * 1000L)) {
            kill(-pid, SIGKILL);
            killed = true;
            *timed_out = !sia_was_interrupted();
        }
    }
    close(po[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (killed)
        kill(-pid, SIGKILL);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "a segmentation fault (an invalid memory access)";
    case SIGABRT: return "abort (an assertion or a memory error caught by the C library)";
    case SIGFPE:  return "an arithmetic error (a division by zero?)";
    case SIGBUS:  return "a bus error";
    case SIGILL:  return "an illegal instruction";
    case SIGKILL: return "SIGKILL (stopped: too long, or out of memory)";
    case SIGXCPU: return "the processor time limit";
    default:      return "a signal";
    }
}

/* Has the window application ended?  Says how into msg. */
static bool app_ended(char *msg, size_t n)
{
    if (A.app <= 0)
        return true;
    int status;
    pid_t r = waitpid(A.app, &status, WNOHANG);
    if (r == 0)
        return false;
    if (r == A.app) {
        if (WIFSIGNALED(status))
            snprintf(msg, n, "it crashed: %s (signal %d)", signal_name(WTERMSIG(status)), WTERMSIG(status));
        else
            snprintf(msg, n, "it ended, exit status %d", WEXITSTATUS(status));
    } else {
        snprintf(msg, n, "it has ended");
    }
    A.app = 0;
    return true;
}

static void stop_app(void)
{
    if (A.app <= 0)
        return;
    kill(-A.app, SIGTERM);
    for (int i = 0; i < 20; i++) {
        if (waitpid(A.app, NULL, WNOHANG) == A.app) {
            A.app = 0;
            return;
        }
        usleep(50000);
    }
    kill(-A.app, SIGKILL);
    waitpid(A.app, NULL, 0);
    A.app = 0;
}

static void run_log_tail(struct sbuf *result)
{
    char p[400];
    snprintf(p, sizeof(p), "%s/.mir/run.log", A.dir);
    size_t n;
    char *t = read_file(p, 256 * 1024, &n);
    if (t && n) {
        struct sbuf raw = { t, n, n + 1 };
        sb_puts(result, "its output (stdout and stderr):\n");
        put_output(result, &raw);
        if (t[n - 1] != '\n')
            sb_putc(result, '\n');
    }
    free(t);
}

/* A desktop request about the running application's window. */
static bool app_desktop(const char *op, const char *extra, struct sbuf *out, char *err, size_t errlen)
{
    struct sbuf req;
    sb_init(&req);
    sb_printf(&req, "{\"op\":\"%s\",\"pid\":%d%s%s}", op, (int)A.app, extra ? "," : "", extra ? extra : "");
    bool ok = sia_desktop_call(req.s, out, err, errlen);
    sb_free(&req);
    return ok;
}

/* ---------------- the tools ---------------- */

static void t_list(struct sia_session *s, struct sbuf *result)
{
    (void)s;
    DIR *d = opendir(A.root);
    sb_puts(result, "exit status: 0\n");
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!valid_name(e->d_name))
            continue;
        char dir[320];
        snprintf(dir, sizeof(dir), "%s/%s", A.root, e->d_name);
        struct json *m = manifest(dir);
        if (!m)
            continue;
        const struct json *inst = json_get(m, "installed");
        sb_printf(result, "%s (%s, %s%s): %s\n", e->d_name, json_get_str(m, "kind") ? json_get_str(m, "kind") : "?",
                  json_get_str(m, "language") ? json_get_str(m, "language") : "?",
                  inst && inst->type == JSON_TRUE ? ", installed" : "",
                  json_get_str(m, "summary") ? json_get_str(m, "summary") : "");
        json_free(m);
        n++;
    }
    if (d)
        closedir(d);
    if (!n)
        sb_puts(result, "no projects yet\n");
}

static void t_files(struct sbuf *result)
{
    DIR *d = opendir(A.dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char p[400];
        snprintf(p, sizeof(p), "%s/%s", A.dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            DIR *d2 = opendir(p);
            struct dirent *e2;
            while (d2 && (e2 = readdir(d2))) {
                char p2[520];
                snprintf(p2, sizeof(p2), "%s/%s", p, e2->d_name);
                if (e2->d_name[0] != '.' && lstat(p2, &st) == 0 && S_ISREG(st.st_mode))
                    sb_printf(result, "  %s/%s (%ld bytes)\n", e->d_name, e2->d_name, (long)st.st_size);
            }
            if (d2)
                closedir(d2);
        } else if (S_ISREG(st.st_mode)) {
            sb_printf(result, "  %s (%ld bytes%s)\n", e->d_name, (long)st.st_size,
                      !strcmp(e->d_name, A.name) ? ", the program" : "");
        }
    }
    if (d)
        closedir(d);
}

static void template_text(const char *file, struct sbuf *out, const char *title)
{
    char p[200];
    snprintf(p, sizeof(p), SHARE "/templates/%s", file);
    size_t n;
    char *t = read_file(p, FILE_MAX, &n);
    for (const char *c = t ? t : ""; *c; c++) {
        if (!strncmp(c, "@TITLE@", 7)) {
            sb_puts(out, title);
            c += 6;
        } else if (!strncmp(c, "@NAME@", 6)) {
            sb_puts(out, A.name);
            c += 5;
        } else {
            sb_putc(out, *c);
        }
    }
    free(t);
}

static void t_create(struct sia_session *s, const struct json *args, struct sbuf *result)
{
    const char *name = json_get_str(args, "name"), *title = json_get_str(args, "title");
    const char *kind = json_get_str(args, "kind"), *lang = json_get_str(args, "language");
    const char *summary = json_get_str(args, "summary");
    if (!name || !valid_name(name)) {
        sb_puts(result, "exit status: 1\nerror: name: 1 to 32 lower-case letters, digits or '-', starting with a letter\n");
        return;
    }
    char dir[320];
    snprintf(dir, sizeof(dir), "%s/%s", A.root, name);
    struct stat st;
    if (lstat(dir, &st) == 0) {
        sb_printf(result, "exit status: 1\nerror: %s exists already: app_open %s continues it, or choose another name\n",
                  name, name);
        return;
    }
    mkdir(A.root, 0755);
    if (mkdir(dir, 0755) < 0) {
        sb_printf(result, "exit status: 1\nerror: cannot make %s: %s\n", dir, strerror(errno));
        return;
    }
    stop_app();
    snprintf(A.name, sizeof(A.name), "%s", name);
    snprintf(A.dir, sizeof(A.dir), "%s", dir);
    snprintf(A.kind, sizeof(A.kind), "%s", kind && !strcmp(kind, "terminal") ? "terminal" : "window");
    snprintf(A.lang, sizeof(A.lang), "%s", lang && (!strcmp(lang, "c++") || !strcmp(lang, "cpp")) ? "c++" : "c");
    if (!title || !*title)
        title = name;
    bool cxx = !strcmp(A.lang, "c++"), win = !strcmp(A.kind, "window");
    char p[400];
    struct sbuf b;
    sb_init(&b);
    sb_printf(&b,
              "# %s, made with MiR.  make: build it; make test: its tests (test.sh); make clean\n"
              "PROG = %s\n"
              "SRCS = $(wildcard *.%s)\n"
              "%s\n"
              "FLAGS = -O2 -Wall -Wextra\n"
              "LIBS = %s\n\n"
              "$(PROG): $(SRCS) $(wildcard *.h)\n"
              "\t$(%s) $(FLAGS) -o $@ $(SRCS) $(LIBS)\n\n"
              "test: $(PROG)\n"
              "\t@if [ -f test.sh ]; then sh ./test.sh; else echo 'no tests yet (test.sh)'; fi\n\n"
              "clean:\n"
              "\trm -f $(PROG)\n\n"
              ".PHONY: test clean\n",
              title, name, cxx ? "cpp" : "c", cxx ? "CXX = c++" : "CC = cc", win ? "-lfacet -lm" : "-lm",
              cxx ? "CXX" : "CC");
    snprintf(p, sizeof(p), "%s/Makefile", dir);
    write_file_at(p, b.s, b.len);
    sb_free(&b);
    sb_init(&b);
    char tfile[40];
    snprintf(tfile, sizeof(tfile), "%s.%s", win ? "window" : "terminal", cxx ? "cpp" : "c");
    template_text(tfile, &b, title);
    snprintf(p, sizeof(p), "%s/main.%s", dir, cxx ? "cpp" : "c");
    write_file_at(p, b.s, b.len);
    sb_free(&b);
    save_manifest(title, summary, false);
    sb_printf(result, "exit status: 0\ncreated the project %s in %s (a %s application in %s) with:\n", name, dir,
              A.kind, cxx ? "C++" : "C");
    t_files(result);
    sb_puts(result, "main.c* is a template that builds: change it (app_read shows it) into the application.\n");
    sia_notify(s, "project", name);
}

static void t_open(struct sia_session *s, const struct json *args, struct sbuf *result)
{
    const char *name = json_get_str(args, "name");
    if (!name || !valid_name(name) || !select_project(name)) {
        sb_puts(result, "exit status: 1\nerror: no such project (app_list shows them)\n");
        return;
    }
    stop_app();
    struct json *m = manifest(A.dir);
    sb_printf(result, "exit status: 0\nthe project %s: ", name);
    json_write(result, m);
    sb_puts(result, "\nfiles:\n");
    t_files(result);
    json_free(m);
    sia_notify(s, "project", name);
}

static void t_read(const struct json *args, struct sbuf *result)
{
    const char *path = json_get_str(args, "path");
    char full[400];
    if (path && (!strncmp(path, "/usr/include/", 13) || !strncmp(path, SHARE "/", strlen(SHARE) + 1)) &&
        !strstr(path, "..")) {
        snprintf(full, sizeof(full), "%s", path);
    } else if (path && valid_rel(path) && need_project(result)) {
        snprintf(full, sizeof(full), "%s/%s", A.dir, path);
    } else {
        if (!result->len)
            sb_puts(result, "exit status: 1\nerror: a file of the project (a relative path), or a reference file under "
                            "/usr/include or " SHARE "\n");
        return;
    }
    size_t n;
    char *t = read_file(full, READ_MAX, &n);
    if (!t) {
        sb_printf(result, "exit status: 1\nerror: cannot read %s\n", path);
        return;
    }
    sb_puts(result, "exit status: 0\n");
    sb_putn(result, t, n);
    if (n == READ_MAX)
        sb_puts(result, "\n[... the rest of the file is not shown ...]\n");
    free(t);
}

static void t_write(struct sia_session *s, const struct json *args, struct sbuf *result)
{
    const char *path = json_get_str(args, "path"), *content = json_get_str(args, "content");
    if (!need_project(result))
        return;
    if (!path || !valid_rel(path) || !content) {
        sb_puts(result, "exit status: 1\nerror: path (relative, in the project) and content are needed\n");
        return;
    }
    size_t n = strlen(content);
    if (n > FILE_MAX) {
        sb_puts(result, "exit status: 1\nerror: files are limited to 256 KB: split it\n");
        return;
    }
    char full[400];
    snprintf(full, sizeof(full), "%s/%s", A.dir, path);
    for (char *sl = full + strlen(A.dir) + 1; (sl = strchr(sl, '/')); sl++) {   /* its folders */
        *sl = 0;
        struct stat st;
        if (lstat(full, &st) == 0 ? !S_ISDIR(st.st_mode) : mkdir(full, 0755) < 0) {
            *sl = '/';
            sb_printf(result, "exit status: 1\nerror: cannot make the folder for %s\n", path);
            return;
        }
        *sl = '/';
    }
    if (!write_file_at(full, content, n)) {
        sb_printf(result, "exit status: 1\nerror: cannot write %s: %s\n", path, strerror(errno));
        return;
    }
    int lines = 0;
    for (const char *c = content; *c; c++)
        lines += *c == '\n';
    sb_printf(result, "exit status: 0\nwrote %s (%d lines)\n", path, lines);
    sia_emit(s, "wrote %s (%d lines)\n", path, lines);
}

static void t_build(struct sia_session *s, struct sbuf *result)
{
    if (!need_project(result))
        return;
    stop_app();
    if (access("/usr/bin/cc", X_OK) < 0) {
        sb_puts(result, "exit status: 1\nerror: this SIEOS has no compiler (/usr/bin/cc): applications can be made on "
                        "SIEOS installed on a disk or a USB drive, not on the live CD\n");
        return;
    }
    char *argv[] = { "make", NULL };
    struct sbuf out;
    sb_init(&out);
    bool timed_out;
    int st = run_in(s, argv, NULL, BUILD_SECS, BUILD_SECS, 2048, &out, true, &timed_out);
    sb_printf(result, "exit status: %d\n", st);
    if (st == 0)
        sb_puts(result, "the build succeeded\n");
    else if (timed_out)
        sb_puts(result, "the build was stopped after 5 minutes\n");
    else
        sb_puts(result, "the build FAILED: fix the errors (file:line: message) and build again\n");
    put_output(result, &out);
    sb_free(&out);
    sia_notify(s, "built", st == 0 ? "ok" : "failed");
}

static void t_run(struct sia_session *s, const struct json *args, struct sbuf *result)
{
    if (!need_project(result))
        return;
    char prog[400];
    snprintf(prog, sizeof(prog), "%s/%s", A.dir, A.name);
    if (access(prog, X_OK) < 0) {
        sb_puts(result, "exit status: 1\nerror: the program is not built yet (app_build)\n");
        return;
    }
    const struct json *argl = json_get(args, "args"), *sv = json_get(args, "seconds");
    const char *input = json_get_str(args, "stdin");
    int secs = sv && sv->type == JSON_NUMBER ? (int)sv->num : 0;
    char *argv[18];
    int argc = 0;
    char self[48];
    snprintf(self, sizeof(self), "./%s", A.name);
    argv[argc++] = self;
    for (int i = 0; argl && argl->type == JSON_ARRAY && i < argl->n && argc < 17; i++)
        if (json_str(argl->items[i]))
            argv[argc++] = (char *)json_str(argl->items[i]);
    argv[argc] = NULL;
    stop_app();
    if (!strcmp(A.kind, "terminal")) {                   /* run it to the end: its output and how it ended */
        if (secs <= 0 || secs > 120)
            secs = RUN_SECS;
        struct sbuf out;
        sb_init(&out);
        bool timed_out;
        int st = run_in(s, argv, input ? input : "", secs, secs + 1, 1024, &out, true, &timed_out);
        sb_printf(result, "exit status: %d\n", st);
        if (timed_out)
            sb_printf(result, "it was still running after %d seconds and was stopped (waiting for input? a loop?)\n", secs);
        else if (st > 128)
            sb_printf(result, "it crashed: %s (signal %d)\n", signal_name(st - 128), st - 128);
        sb_puts(result, out.len ? "its output (stdout and stderr):\n" : "(no output)\n");
        put_output(result, &out);
        sb_free(&out);
        return;
    }
    if (!sia_desktop_connected()) {
        sb_puts(result, "exit status: 1\nerror: not on the Facet desktop: a window application cannot be run here\n");
        return;
    }
    char logdir[400], logp[420];
    snprintf(logdir, sizeof(logdir), "%s/.mir", A.dir);
    mkdir(logdir, 0700);
    snprintf(logp, sizeof(logp), "%s/run.log", logdir);
    pid_t pid = fork();
    if (pid < 0) {
        sb_puts(result, "exit status: 1\nerror: fork failed\n");
        return;
    }
    if (pid == 0) {
        setpgid(0, 0);
        if (chdir(A.dir) < 0)
            _exit(126);
        int nul = open("/dev/null", O_RDONLY), log = open(logp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        dup2(nul, 0);
        dup2(log, 1);
        dup2(log, 2);
        for (int fd = 3; fd < 64; fd++)
            close(fd);
        unsetenv("SIEOS_DESKTOP");
        limits(0, 1024, 64);
        execv(self, argv);
        _exit(127);
    }
    A.app = pid;
    char msg[160] = "";
    struct sbuf win;
    sb_init(&win);
    char err[200] = "";
    bool shown = false;
    for (int i = 0; i < 40 && !shown; i++) {             /* its window: up to 8 seconds */
        if (app_ended(msg, sizeof(msg)))
            break;
        sb_free(&win);
        sb_init(&win);
        shown = app_desktop("windows", NULL, &win, err, sizeof(err)) && strstr(win.s, "window ");
        if (!shown)
            usleep(200000);
    }
    if (shown) {
        if (secs <= 0 || secs > 30)
            secs = 3;
        for (int i = 0; i < secs * 5 && !msg[0]; i++) {  /* it must stay up */
            if (app_ended(msg, sizeof(msg)))
                break;
            usleep(200000);
        }
    }
    if (!msg[0] && !shown) {
        stop_app();
        snprintf(msg, sizeof(msg), "no window appeared in 8 seconds (stopped): it must call fct_app_init, create a "
                                   "view and run fct_main");
    }
    sb_printf(result, "exit status: %d\n", msg[0] ? 1 : 0);
    if (msg[0]) {
        sb_printf(result, "%s\n", msg);
    } else {
        sb_printf(result, "it is running, its window open: %s\n", win.s);
        sb_puts(result, "it stays open (app_screenshot shows it to you, app_input types or clicks in it, app_stop "
                        "closes it; the user can try it too)\n");
        sia_notify(s, "running", A.name);
    }
    run_log_tail(result);
    sb_free(&win);
}

static void t_stop(struct sbuf *result)
{
    char msg[160] = "";
    if (A.app > 0 && app_ended(msg, sizeof(msg))) {
        sb_printf(result, "exit status: 0\n%s before it was stopped\n", msg);
        return;
    }
    bool was = A.app > 0;
    stop_app();
    sb_puts(result, was ? "exit status: 0\nstopped\n" : "exit status: 0\nit was not running\n");
}

static bool running_window(struct sbuf *result)
{
    char msg[160] = "";
    if (A.app <= 0 || app_ended(msg, sizeof(msg))) {
        sb_printf(result, "exit status: 1\nerror: the application is not running%s%s (app_run starts it)\n",
                  msg[0] ? ": " : "", msg);
        if (msg[0])
            run_log_tail(result);
        return false;
    }
    return true;
}

static void t_input(const struct json *args, struct sbuf *result)
{
    if (!need_project(result) || !running_window(result))
        return;
    const char *text = json_get_str(args, "text"), *key = json_get_str(args, "key");
    const struct json *x = json_get(args, "x"), *y = json_get(args, "y");
    struct sbuf extra, out;
    sb_init(&extra);
    sb_init(&out);
    if (x && y && x->type == JSON_NUMBER && y->type == JSON_NUMBER) {
        const char *btn = json_get_str(args, "button");
        sb_printf(&extra, "\"click\":[%d,%d],\"button\":\"%s\"", (int)x->num, (int)y->num,
                  btn && !strcmp(btn, "right") ? "right" : "left");
    } else if (text) {
        sb_puts(&extra, "\"text\":");
        sb_json_str(&extra, text);
    } else if (key) {
        sb_puts(&extra, "\"key\":");
        sb_json_str(&extra, key);
    } else {
        sb_puts(result, "exit status: 1\nerror: text, key, or x and y (a click) are needed\n");
        sb_free(&extra);
        return;
    }
    char err[200];
    bool ok = app_desktop("input", extra.s, &out, err, sizeof(err));
    usleep(400000);                                      /* (time to react and redraw) */
    char msg[160] = "";
    if (app_ended(msg, sizeof(msg))) {
        sb_printf(result, "exit status: 1\nafter the input, %s\n", msg);
        run_log_tail(result);
    } else if (ok) {
        sb_printf(result, "exit status: 0\n%s\n", out.s);
    } else {
        sb_printf(result, "exit status: 1\nerror: %s\n", err);
    }
    sb_free(&extra);
    sb_free(&out);
}

static void t_screenshot(struct sia_session *s, struct sbuf *result)
{
    if (!need_project(result) || !running_window(result))
        return;
    char path[400], err[200], extra[480];
    snprintf(path, sizeof(path), "%s/.mir/screenshot.png", A.dir);
    struct sbuf out;
    sb_init(&out);
    struct sbuf q;
    sb_init(&q);
    sb_puts(&q, "\"path\":");
    sb_json_str(&q, path);
    snprintf(extra, sizeof(extra), "%s", q.s);
    sb_free(&q);
    if (!app_desktop("snapshot", extra, &out, err, sizeof(err))) {
        sb_printf(result, "exit status: 1\nerror: %s\n", err);
        sb_free(&out);
        return;
    }
    size_t n;
    char *png = read_file(path, 8 << 20, &n);
    char cap[200];
    snprintf(cap, sizeof(cap), "Screenshot of the window of %s (its content, %s pixels), from app_screenshot.", A.name,
             out.s);
    if (!png || !sia_attach_image(s, cap, png, n))
        sb_puts(result, "exit status: 1\nerror: the model in use does not see images: the user has to look at the "
                        "window (tell them what to check)\n");
    else
        sb_printf(result, "exit status: 0\nthe screenshot (%s) follows in the next message\n", out.s);
    free(png);
    sb_free(&out);
}

static void t_test(struct sia_session *s, struct sbuf *result)
{
    if (!need_project(result))
        return;
    char *argv[] = { "make", "-s", "test", NULL };
    struct sbuf out;
    sb_init(&out);
    bool timed_out;
    int st = run_in(s, argv, NULL, 120, 120, 1024, &out, true, &timed_out);
    sb_printf(result, "exit status: %d\n%s", st, timed_out ? "the tests were stopped after 2 minutes\n" : "");
    put_output(result, &out);
    sb_free(&out);
}

static void t_install(struct sia_session *s, struct sbuf *result)
{
    if (!need_project(result))
        return;
    char prog[400], bindir[300], dest[400];
    snprintf(prog, sizeof(prog), "%s/%s", A.dir, A.name);
    if (access(prog, X_OK) < 0) {
        sb_puts(result, "exit status: 1\nerror: build it first (app_build)\n");
        return;
    }
    snprintf(bindir, sizeof(bindir), "%s/bin", A.root);
    mkdir(bindir, 0755);
    snprintf(dest, sizeof(dest), "%s/%s", bindir, A.name);
    size_t n;
    char *bin = read_file(prog, 64 << 20, &n);
    bool ok = bin && write_file_at(dest, bin, n) && chmod(dest, 0755) == 0;
    free(bin);
    if (!ok) {
        sb_printf(result, "exit status: 1\nerror: cannot write %s\n", dest);
        return;
    }
    struct json *m = manifest(A.dir);
    char title[80], summary[300];
    snprintf(title, sizeof(title), "%s", json_get_str(m, "title") ? json_get_str(m, "title") : A.name);
    snprintf(summary, sizeof(summary), "%s", json_get_str(m, "summary") ? json_get_str(m, "summary") : "");
    json_free(m);
    save_manifest(title, summary, true);
    sb_printf(result, "exit status: 0\ninstalled %s as %s%s\n", A.name, dest,
              strcmp(A.kind, "window") ? " (run it in a terminal by its name: ~/apps/bin is on the PATH)"
                                       : ", in the desktop menu's My apps");
    sia_notify(s, "installed", A.name);
}

static void t_describe(struct sbuf *result, const struct json *args)
{
    const char *title = json_get_str(args, "title"), *summary = json_get_str(args, "summary");
    if (!need_project(result))
        return;
    struct json *m = manifest(A.dir);
    const struct json *inst = json_get(m, "installed");
    char t[80], su[300];
    snprintf(t, sizeof(t), "%s", title ? title : json_get_str(m, "title") ? json_get_str(m, "title") : A.name);
    snprintf(su, sizeof(su), "%s", summary ? summary : json_get_str(m, "summary") ? json_get_str(m, "summary") : "");
    save_manifest(t, su, inst && inst->type == JSON_TRUE);
    json_free(m);
    sb_puts(result, "exit status: 0\nsaved\n");
}

/* ---------------- registration ---------------- */

enum { T_LIST, T_CREATE, T_OPEN, T_FILES, T_READ, T_WRITE, T_BUILD, T_RUN, T_STOP, T_INPUT, T_SHOT, T_TEST, T_INSTALL,
       T_DESCRIBE };

static void app_run_tool(struct sia_session *s, const struct sia_tool *t, const struct json *args, struct sbuf *result)
{
    switch ((int)(intptr_t)t->arg) {
    case T_LIST:     t_list(s, result); break;
    case T_CREATE:   t_create(s, args, result); break;
    case T_OPEN:     t_open(s, args, result); break;
    case T_FILES:    if (need_project(result)) { sb_puts(result, "exit status: 0\n"); t_files(result); } break;
    case T_READ:     t_read(args, result); break;
    case T_WRITE:    t_write(s, args, result); break;
    case T_BUILD:    t_build(s, result); break;
    case T_RUN:      t_run(s, args, result); break;
    case T_STOP:     t_stop(result); break;
    case T_INPUT:    t_input(args, result); break;
    case T_SHOT:     t_screenshot(s, result); break;
    case T_TEST:     t_test(s, result); break;
    case T_INSTALL:  t_install(s, result); break;
    case T_DESCRIBE: t_describe(result, args); break;
    }
}

static void app_tool_describe(const struct sia_tool *t, const struct json *args, struct sbuf *out)
{
    const char *path = json_get_str(args, "path"), *name = json_get_str(args, "name");
    switch ((int)(intptr_t)t->arg) {
    case T_CREATE:  sb_printf(out, "create the project %s", name ? name : "?"); break;
    case T_OPEN:    sb_printf(out, "open the project %s", name ? name : "?"); break;
    case T_READ:    sb_printf(out, "read %s", path ? path : "?"); break;
    case T_WRITE:   sb_printf(out, "write %s", path ? path : "?"); break;
    case T_BUILD:   sb_puts(out, "build (make)"); break;
    case T_RUN:     sb_puts(out, "run the application"); break;
    case T_STOP:    sb_puts(out, "stop the application"); break;
    case T_INPUT:   sb_puts(out, "try the application (input)"); break;
    case T_SHOT:    sb_puts(out, "look at the application (screenshot)"); break;
    case T_TEST:    sb_puts(out, "test (make test)"); break;
    case T_INSTALL: sb_printf(out, "install %s", A.name[0] ? A.name : "the application"); break;
    default:        sb_puts(out, t->name);
    }
}

static void add(struct sia_session *s, const char *name, int op, bool readonly, const char *desc, const char *params)
{
    struct sia_tool t;
    memset(&t, 0, sizeof(t));
    snprintf(t.name, sizeof(t.name), "%s", name);
    t.description = desc;
    t.parameters = params;
    t.readonly = readonly;
    t.run = app_run_tool;
    t.describe = app_tool_describe;
    t.arg = (void *)(intptr_t)op;
    sia_add_tool(s, &t);
}

#define NOARGS "{\"type\":\"object\",\"properties\":{}}"

void mir_add_tools(struct sia_session *s, const char *project)
{
    const char *home = getenv("HOME");
    snprintf(A.root, sizeof(A.root), "%s/apps", home && *home ? home : "/tmp");
    if (project && valid_name(project))
        select_project(project);
    add(s, "app_list", T_LIST, true, "List the user's application projects (~/apps).", NOARGS);
    add(s, "app_create", T_CREATE, true,
        "Create a new application project and make it the current one: a folder with a Makefile and a main source "
        "from a template that builds. kind: window (a Facet desktop application, libfacet) or terminal (a program "
        "run in a terminal: standard input and output).",
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"short program name: "
        "lower-case letters, digits, '-'\"},\"title\":{\"type\":\"string\",\"description\":\"the name people see "
        "(window title, menu)\"},\"kind\":{\"type\":\"string\",\"enum\":[\"window\",\"terminal\"]},\"language\":"
        "{\"type\":\"string\",\"enum\":[\"c\",\"c++\"]},\"summary\":{\"type\":\"string\",\"description\":\"one "
        "sentence: what it does\"}},\"required\":[\"name\",\"title\",\"kind\",\"language\",\"summary\"]}");
    add(s, "app_open", T_OPEN, true, "Continue an existing project (make it the current one): shows its description "
        "and files.", "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}");
    add(s, "app_describe", T_DESCRIBE, true, "Change the current project's title or summary.",
        "{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\"},\"summary\":{\"type\":\"string\"}}}");
    add(s, "app_files", T_FILES, true, "List the current project's files.", NOARGS);
    add(s, "app_read", T_READ, true,
        "Read a file: one of the project's (a relative path), or a reference file: the headers under /usr/include "
        "(e.g. /usr/include/facet/facet.h, /usr/include/facet/gfx.h) or MiR's guide and examples under " SHARE ".",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}");
    add(s, "app_write", T_WRITE, true,
        "Write a whole file of the current project (created or replaced): a relative path such as main.c, "
        "view.h, test.sh or the Makefile.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\","
        "\"description\":\"the complete file\"}},\"required\":[\"path\",\"content\"]}");
    add(s, "app_build", T_BUILD, true, "Build the current project (make, with SIEOS's gcc or g++): the compiler's "
        "messages when it fails.", NOARGS);
    add(s, "app_run", T_RUN, true,
        "Run the built application. A terminal program runs to its end (args, stdin; stopped after 'seconds', 20 "
        "by default): its exit status and output. A window application is started, its window awaited, and "
        "watched for 'seconds' (3 by default): whether it is up or crashed, and its output; it stays open.",
        "{\"type\":\"object\",\"properties\":{\"args\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"stdin\":{\"type\":\"string\"},\"seconds\":{\"type\":\"integer\"}}}");
    add(s, "app_stop", T_STOP, true, "Close the running window application.", NOARGS);
    add(s, "app_input", T_INPUT, true,
        "Try the running window application: type text into it, press a key (enter, tab, escape, backspace, delete, "
        "up, down, left, right, home, end, space), or click at x, y (pixels in its content area, from the top left; "
        "button left or right).",
        "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},\"key\":{\"type\":\"string\"},"
        "\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"button\":{\"type\":\"string\",\"enum\":"
        "[\"left\",\"right\"]}}}");
    if (sia_session_vision(s))
        add(s, "app_screenshot", T_SHOT, true, "See the running window application: a screenshot of its window's "
            "content is shown to you in the next message.", NOARGS);
    add(s, "app_test", T_TEST, true, "Run the project's tests (make test: runs test.sh, which you write).", NOARGS);
    add(s, "app_install", T_INSTALL, false, "Install the application for the user: ~/apps/bin, and for a window "
        "application the desktop menu's My apps. Only when the user asks for it.", NOARGS);
}

/* ---------------- the instructions ---------------- */

void mir_instructions(struct sia_session *s, struct sbuf *p, void *ctx)
{
    (void)ctx;
    size_t n;
    char *guide = read_file(SHARE "/guide.txt", 128 * 1024, &n);
    if (guide)
        sb_putn(p, guide, n);
    else
        sb_puts(p, "You are MiR (Make it Real), part of sia: you make applications for SIEOS that the user "
                   "describes, in C or C++, with the app_* tools: create, write, build, run, test, fix.\n");
    free(guide);
    sb_printf(p, "\nProjects are in %s. ", A.root);
    if (A.name[0])
        sb_printf(p, "The current project is %s (a %s application in %s).\n", A.name, A.kind, A.lang);
    else
        sb_puts(p, "There is no current project yet.\n");
    if (sia_session_vision(s))
        sb_puts(p, "You see images: after app_run, use app_screenshot to check how a window application looks, "
                   "and again after app_input.\n");
    else
        sb_puts(p, "The model you run on does not see images (there is no app_screenshot): check a window "
                   "application with app_run and app_input, and tell the user what to look at in its window. They "
                   "can choose a model that sees images in Settings, Assistant.\n");
    if (access("/usr/bin/cc", X_OK) < 0)
        sb_puts(p, "WARNING: this SIEOS has no compiler (the live CD): tell the user that applications can only be "
                   "made on SIEOS installed on a disk or a USB drive.\n");
}
