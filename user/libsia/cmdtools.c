/*
 * cmdtools.c - Terminal commands as tools: one per program in /bin and
 * /sbin, plus sh (a full command line), cd and write_file.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "internal.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define RUN_TIMEOUT_MS  60000       /* commands are stopped after a minute */
#define OUTPUT_KEEP     64000       /* bytes of output collected per command */
#define RESULT_MAX      12000       /* bytes of output sent back to the model */
#define MAX_FD          64

static const char *ARGS_SCHEMA =
    "{\"type\":\"object\",\"properties\":{\"args\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
    "\"description\":\"command-line arguments\"},\"stdin\":{\"type\":\"string\","
    "\"description\":\"optional text for standard input\"}}}";

static const struct {
    const char *name, *desc;
    bool readonly;
} known[] = {
    { "cat", "Print the contents of files (args: file names).", true },
    { "chgrp", "Change the group of files: chgrp GROUP FILE...", false },
    { "chmod", "Change file permissions: chmod MODE FILE... (octal or symbolic).", false },
    { "chown", "Change the owner of files: chown USER[:GROUP] FILE...", false },
    { "cp", "Copy files: cp SRC DST, or cp SRC... DIR.", false },
    { "date", "Show the current date and time.", true },
    { "df", "Show file system usage.", true },
    { "echo", "Print the arguments.", true },
    { "env", "Show the environment, or run a command with a modified environment.", true },
    { "free", "Show memory usage.", true },
    { "grep", "Search for a pattern in files or stdin: grep [-i] [-v] [-n] [-c] PATTERN [FILE...].", true },
    { "groups", "Show the groups of a user.", true },
    { "halt", "Stop the system (power off).", false },
    { "head", "Show the first lines of files: head [-n N] FILE...", true },
    { "hexdump", "Show a file in hexadecimal.", true },
    { "host", "Look up a host name in DNS: host NAME.", true },
    { "id", "Show user and group ids.", true },
    { "ifconfig", "Show network interfaces (no arguments) or configure them.", true },
    { "kill", "Send a signal to processes: kill [-SIGNAL] PID...", false },
    { "ls", "List directory contents: ls [-l] [-a] [PATH...] (no sorting options).", true },
    { "lscpu", "Show information about the processors.", true },
    { "mkdir", "Create directories: mkdir [-p] DIR...", false },
    { "mv", "Move or rename files: mv SRC DST.", false },
    { "nc", "Netcat: open a TCP connection (nc HOST PORT); stdin is sent, the reply printed.", false },
    { "netstat", "Show network connections and sockets.", true },
    { "nproc", "Show the number of processors.", true },
    { "ping", "Send ICMP echo requests: ping [-c COUNT] HOST (always pass -c).", true },
    { "printenv", "Print environment variables.", true },
    { "ps", "List processes.", true },
    { "reboot", "Restart the system.", false },
    { "rm", "Remove files: rm [-r] [-f] PATH...", false },
    { "rmdir", "Remove empty directories.", false },
    { "sifetch", "Show a summary of the system.", true },
    { "sleep", "Wait for a number of seconds.", true },
    { "stat", "Show file status (size, mode, owner, times).", true },
    { "tail", "Show the last lines of files: tail [-n N] FILE...", true },
    { "touch", "Create files or update their times.", false },
    { "uname", "Show system information: uname [-a].", true },
    { "uptime", "Show how long the system has been running.", true },
    { "useradd", "Create a user account (root only).", false },
    { "wc", "Count lines, words and bytes.", true },
    { "wget", "Download a URL to a file: wget URL [-O FILE] (http only).", false },
    { "whoami", "Show the current user name.", true },
};

/* Programs that are interactive, never end, or are part of the system. */
static const char *excluded[] = {
    "sh", "login", "su", "passwd", "init", "sdm", "facet", "sia", "sia-agent", "clear", "tty", "yes", "true",
    "false", "httpd", "forktest", "fstest", "sigtest", NULL,
};

/* ---------------- execution ---------------- */

static void keep(struct sbuf *out, const char *buf, size_t n)
{
    if (out->len < OUTPUT_KEEP)
        sb_putn(out, buf, MIN(n, OUTPUT_KEEP - out->len));
}

/* Run argv with stdout+stderr captured and shown; returns the exit status. */
static int run_argv(struct sia_session *s, char *const argv[], const char *input, struct sbuf *out)
{
    int po[2], pi[2] = { -1, -1 };
    if (pipe(po) < 0)
        return -1;
    if (input && pipe(pi) < 0) {
        close(po[0]);
        close(po[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        sb_puts(out, "fork failed\n");
        return -1;
    }
    if (pid == 0) {
        if (input) {
            dup2(pi[0], 0);
        } else {
            int nul = open("/dev/null", O_RDONLY);
            dup2(nul, 0);
        }
        dup2(po[1], 1);
        dup2(po[1], 2);
        for (int fd = 3; fd < MAX_FD; fd++)            /* nothing else leaks into commands */
            close(fd);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execv(argv[0], argv);
        dprintf(2, "%s: cannot execute\n", argv[0]);
        _exit(127);
    }
    close(po[1]);
    if (input) {
        close(pi[0]);
        write(pi[1], input, strlen(input));
        close(pi[1]);
    }
    long start = uptime_ms();
    char buf[1024];
    bool killed = false;
    for (;;) {
        struct pollfd p = { po[0], POLLIN, 0 };
        int r = poll(&p, 1, 250);
        if (r > 0) {
            long n = read(po[0], buf, sizeof(buf));
            if (n <= 0)
                break;
            sia__output(s, buf, n);
            keep(out, buf, n);
        }
        if (!killed && (sia_interrupted || uptime_ms() - start > RUN_TIMEOUT_MS)) {
            kill(pid, SIGTERM);
            killed = true;
            const char *why = sia_interrupted ? "\n[interrupted]\n" : "\n[stopped after 60 seconds]\n";
            sia__output(s, why, strlen(why));
            keep(out, why, strlen(why));
        }
    }
    close(po[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* Copy text without ANSI escape sequences (colours are for the terminal only). */
static void strip_ansi(struct sbuf *dst, const char *str, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (str[i] == '\033' && i + 1 < n && str[i + 1] == '[') {
            i += 2;
            while (i < n && !(str[i] >= '@' && str[i] <= '~'))
                i++;
            continue;
        }
        sb_putc(dst, str[i]);
    }
}

static void result_text(struct sia_session *s, struct sbuf *result, int status, const struct sbuf *raw)
{
    struct sbuf clean;
    sb_init(&clean);
    strip_ansi(&clean, raw->s, raw->len);
    sb_printf(result, "exit status: %d\n", status);
    if (clean.len <= RESULT_MAX) {
        sb_putn(result, clean.s, clean.len);
    } else {
        sb_putn(result, clean.s, RESULT_MAX / 2);
        sb_printf(result, "\n[... %lu bytes omitted ...]\n", (unsigned long)(clean.len - RESULT_MAX));
        sb_putn(result, clean.s + clean.len - RESULT_MAX / 2, RESULT_MAX / 2);
    }
    if (raw->len && raw->s[raw->len - 1] != '\n')
        sia__output(s, "\n", 1);
    if (status)
        sia_emit(s, "[exit %d]\n", status);
    sb_free(&clean);
}

/* ---------------- program tools ---------------- */

static bool needs_quotes(const char *str)
{
    if (!*str)
        return true;
    for (; *str; str++)
        if (!(isalnum((unsigned char)*str) || strchr("-_./=:,+@%", *str)))
            return true;
    return false;
}

static void program_describe(const struct sia_tool *t, const struct json *args, struct sbuf *out)
{
    sb_puts(out, t->name);
    const struct json *a = json_get(args, "args");
    for (int i = 0; a && i < a->n; i++) {
        const char *str = json_str(a->items[i]);
        if (!str)
            continue;
        sb_putc(out, ' ');
        if (needs_quotes(str)) {
            sb_putc(out, '\'');
            sb_puts(out, str);
            sb_putc(out, '\'');
        } else {
            sb_puts(out, str);
        }
    }
    if (json_get_str(args, "stdin"))
        sb_puts(out, " < (text)");
}

static bool program_confirm(struct sia_session *s, const struct sia_tool *t, const struct json *args)
{
    (void)s;
    if (!t->readonly)
        return true;
    if (!strcmp(t->name, "ifconfig")) {             /* read-only only without arguments */
        const struct json *a = json_get(args, "args");
        return a && a->n > 0;
    }
    return false;
}

static void program_run(struct sia_session *s, const struct sia_tool *t, const struct json *args, struct sbuf *result)
{
    char *argv[40];
    int argc = 0;
    argv[argc++] = (char *)t->path;
    const struct json *a = json_get(args, "args");
    for (int i = 0; a && i < a->n && argc < 39; i++)
        if (json_str(a->items[i]))
            argv[argc++] = a->items[i]->str;
    argv[argc] = NULL;
    struct sbuf out;
    sb_init(&out);
    int status = run_argv(s, argv, json_get_str(args, "stdin"), &out);
    result_text(s, result, status, &out);
    sb_free(&out);
}

/* ---------------- sh, cd, write_file ---------------- */

static void sh_describe(const struct sia_tool *t, const struct json *args, struct sbuf *out)
{
    (void)t;
    const char *c = json_get_str(args, "command");
    sb_puts(out, c ? c : "");
}

/* A shell line is read-only if every command in it is a read-only program
 * and nothing is redirected or substituted. */
static bool sh_confirm(struct sia_session *s, const struct sia_tool *t, const struct json *args)
{
    (void)t;
    const char *cmd = json_get_str(args, "command");
    if (!cmd)
        return true;
    for (const char *c = cmd; *c; c++)
        if (strchr("><`$\\", *c))
            return true;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", cmd);
    char *p = buf;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        char word[32];
        int n = 0;
        while (*p && !strchr(" \t|;&", *p) && n < (int)sizeof(word) - 1)
            word[n++] = *p++;
        word[n] = 0;
        const struct sia_tool *w = n ? sia_find_tool(s, word) : NULL;
        if (!w || !w->readonly || !w->path[0] || !strcmp(word, "ifconfig"))
            return true;
        while (*p && !strchr("|;&", *p))
            p++;
        if (!*p)
            return false;
        while (*p && strchr("|;&", *p))
            p++;
    }
}

static void sh_run(struct sia_session *s, const struct sia_tool *t, const struct json *args, struct sbuf *result)
{
    (void)t;
    const char *c = json_get_str(args, "command");
    char *argv[] = { "/bin/sh", "-c", (char *)(c ? c : "true"), NULL };
    struct sbuf out;
    sb_init(&out);
    int status = run_argv(s, argv, NULL, &out);
    result_text(s, result, status, &out);
    sb_free(&out);
}

static void cd_describe(const struct sia_tool *t, const struct json *args, struct sbuf *out)
{
    (void)t;
    const char *p = json_get_str(args, "path");
    sb_printf(out, "cd %s", p ? p : "");
}

static void cd_run(struct sia_session *s, const struct sia_tool *t, const struct json *args, struct sbuf *result)
{
    (void)t;
    const char *p = json_get_str(args, "path");
    if (!p || !*p)
        p = getenv("HOME") ? getenv("HOME") : "/";
    if (chdir(p) < 0) {
        sb_printf(result, "cd: %s: %s\n", p, strerror(errno));
        sia_emit(s, "cd: %s: %s\n", p, strerror(errno));
    } else {
        char cwd[256];
        getcwd(cwd, sizeof(cwd));
        sb_printf(result, "now in %s\n", cwd);
    }
}

static void wf_describe(const struct sia_tool *t, const struct json *args, struct sbuf *out)
{
    (void)t;
    const char *p = json_get_str(args, "path");
    const struct json *ap = json_get(args, "append");
    sb_printf(out, "%s file %s", ap && ap->type == JSON_TRUE ? "append to" : "write", p ? p : "?");
}

static void wf_run(struct sia_session *s, const struct sia_tool *t, const struct json *args, struct sbuf *result)
{
    (void)t;
    const char *p = json_get_str(args, "path"), *c = json_get_str(args, "content");
    const struct json *ap = json_get(args, "append");
    bool append = ap && ap->type == JSON_TRUE;
    if (!p || !c) {
        sb_puts(result, "error: path and content are required\n");
        return;
    }
    int fd = open(p, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0 || write(fd, c, strlen(c)) != (long)strlen(c)) {
        sb_printf(result, "error: %s: %s\n", p, strerror(errno));
        sia_emit(s, "write_file: %s: %s\n", p, strerror(errno));
    } else {
        sb_printf(result, "wrote %lu bytes to %s\n", (unsigned long)strlen(c), p);
        sia_emit(s, "(wrote %lu bytes to %s)\n", (unsigned long)strlen(c), p);
    }
    if (fd >= 0)
        close(fd);
}

/* ---------------- registration ---------------- */

static bool is_excluded(const char *name)
{
    for (int i = 0; excluded[i]; i++)
        if (!strcmp(excluded[i], name))
            return true;
    return false;
}

static bool valid_name(const char *str)
{
    if (!*str || strlen(str) > 30)
        return false;
    for (; *str; str++)
        if (!(isalnum((unsigned char)*str) || *str == '_' || *str == '-'))
            return false;
    return true;
}

static void scan_dir(struct sia_session *s, const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return;
    static char buf[4096];
    long n;
    while ((n = getdents(fd, (struct dirent *)buf, sizeof(buf))) > 0) {
        for (long off = 0; off < n;) {
            struct dirent *d = (struct dirent *)(buf + off);
            off += d->d_reclen;
            if (d->d_name[0] == '.' || is_excluded(d->d_name) || !valid_name(d->d_name))
                continue;
            struct sia_tool t;
            memset(&t, 0, sizeof(t));
            snprintf(t.path, sizeof(t.path), "%s/%s", dir, d->d_name);
            struct stat st;
            if (stat(t.path, &st) < 0 || !S_ISREG(st.st_mode) || !(st.st_mode & 0111))
                continue;
            snprintf(t.name, sizeof(t.name), "%s", d->d_name);
            for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
                if (!strcmp(known[i].name, d->d_name)) {
                    t.description = known[i].desc;
                    t.readonly = known[i].readonly;
                }
            t.parameters = ARGS_SCHEMA;
            t.run = program_run;
            t.describe = program_describe;
            t.needs_confirm = program_confirm;
            sia_add_tool(s, &t);
        }
    }
    close(fd);
}

void sia_add_command_tools(struct sia_session *s)
{
    struct sia_tool t;
    memset(&t, 0, sizeof(t));
    snprintf(t.name, sizeof(t.name), "sh");
    t.description = "Run a full shell command line with /bin/sh -c (pipes |, redirections > >> <, ;, &&, ||, "
                    "variables). Use it when a single command is not enough.";
    t.parameters = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\","
                   "\"description\":\"the command line\"}},\"required\":[\"command\"]}";
    t.run = sh_run;
    t.describe = sh_describe;
    t.needs_confirm = sh_confirm;
    sia_add_tool(s, &t);

    memset(&t, 0, sizeof(t));
    snprintf(t.name, sizeof(t.name), "cd");
    t.description = "Change the current working directory of this session.";
    t.parameters = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
                   "\"description\":\"directory (default: home)\"}}}";
    t.readonly = true;
    t.run = cd_run;
    t.describe = cd_describe;
    sia_add_tool(s, &t);

    memset(&t, 0, sizeof(t));
    snprintf(t.name, sizeof(t.name), "write_file");
    t.description = "Create or overwrite a text file with the given content (or append to it).";
    t.parameters = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
                   "\"content\":{\"type\":\"string\"},\"append\":{\"type\":\"boolean\"}},"
                   "\"required\":[\"path\",\"content\"]}";
    t.run = wf_run;
    t.describe = wf_describe;
    sia_add_tool(s, &t);

    scan_dir(s, "/bin");
    scan_dir(s, "/sbin");
}
