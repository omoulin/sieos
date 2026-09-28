/*
 * login - authenticate a user on the console and start their shell.
 */
#include "sieos.h"

/* Apply /etc/logindevperm: hand console-owned devices to the user. */
static void logindevperm(struct passwd *pw)
{
    int fd = open("/etc/logindevperm", O_RDONLY);
    if (fd < 0)
        return;
    char line[256];
    while (read_line_fd(fd, line, sizeof(line)) >= 0) {
        if (line[0] == '#' || !line[0])
            continue;
        char *rest = line, *tok;
        int field = 0, mode = 0600;
        while ((tok = strsep(&rest, " \t")) != NULL) {
            if (!*tok)
                continue;
            if (field == 1)
                mode = strtol(tok, NULL, 8);
            else if (field >= 2) {
                chown(tok, pw->pw_uid, pw->pw_gid);
                chmod(tok, mode);
            }
            field++;
        }
    }
    close(fd);
}

static void start_session(struct passwd *pw)
{
    /* Give the console to the user, like a real login(1). */
    chown("/dev/console", pw->pw_uid, pw->pw_gid);
    logindevperm(pw);
    if (initgroups(pw->pw_name, pw->pw_gid) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
        perror("login: cannot set credentials");
        exit(1);
    }
    if (chdir(pw->pw_dir) < 0) {
        printf("No directory %s, logging in with HOME=/\n", pw->pw_dir);
        chdir("/");
    }
    int fd = open("/etc/motd", O_RDONLY);
    if (fd >= 0) {
        char buf[512];
        long n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(STDOUT_FILENO, buf, n);
        close(fd);
    }
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    clearenv();
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", pw->pw_shell, 1);
    setenv("PATH", pw->pw_uid == 0 ? "/sbin:/bin:/usr/bin:/usr/gnu/bin" : "/bin:/usr/bin:/usr/gnu/bin:/sbin", 1);
    setenv("TERM", "sieos", 1);
    setenv("ENV", "/etc/shrc", 1);           /* interactive /bin/sh: the prompt */
    char *argv[] = { "-sh", NULL };
    execv(pw->pw_shell, argv);
    perror(pw->pw_shell);
    exit(1);
}

int main(void)
{
    /* New session with the console as controlling terminal. */
    setsid();
    ioctl(STDIN_FILENO, TIOCSCTTY, 1);
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    for (int attempt = 0; attempt < 5; attempt++) {
        char user[64];
        printf("\nsieos login: ");
        if (read_line_fd(STDIN_FILENO, user, sizeof(user)) < 0)
            exit(0);
        if (!user[0]) {
            attempt--;
            continue;
        }
        char *pass = getpass("Password: ");
        struct passwd *pwp = getpwnam(user);
        struct passwd pw;
        char name[64], dir[128], shell[64];
        bool ok = false;
        if (pwp) {
            pw = *pwp;
            strlcpy(name, pw.pw_name, sizeof(name));
            strlcpy(dir, pw.pw_dir, sizeof(dir));
            strlcpy(shell, pw.pw_shell, sizeof(shell));
            pw.pw_name = name;
            pw.pw_dir = dir;
            pw.pw_shell = shell;
            const char *hash = pw.pw_passwd;
            struct spwd *sp = NULL;
            if (strcmp(hash, "x") == 0 && (sp = getspnam(user)))
                hash = sp->sp_pwdp;
            ok = pass && hash[0] != '!' && hash[0] != '*' && check_password(pass, hash);
        }
        if (ok)
            start_session(&pw);
        sleep(1);
        printf("Login incorrect\n");
    }
    return 1;
}
