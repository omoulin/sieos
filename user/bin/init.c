/*
 * init - first user process.
 *
 * Runs /etc/rc, then keeps a login session on the console: the graphical
 * login (/sbin/sdm) by default, or the text login (/bin/login) when
 *   - the kernel command line contains "text" (GRUB "text console" entry),
 *   - /etc/default/init sets CONSOLE=text,
 *   - there is no usable framebuffer, or sdm keeps failing,
 *   - the user chose "Console Login" on the graphical login screen.
 * The session's exit status can ask for a restart or a halt (the desktop's
 * "Exit..." menu: SESSION_EXIT_REBOOT, SESSION_EXIT_HALT in sieos.h).
 * Also adopts and reaps orphaned processes.
 */
#include "sieos.h"

#define SDM_CONSOLE_REQUEST SESSION_EXIT_CONSOLE   /* sdm exit status: run a text login once */

static pid_t spawn(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("init: fork");
        return -1;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);             /* ignored signals survive exec */
        signal(SIGTSTP, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        execv(argv[0], argv);
        perror(argv[0]);
        exit(127);
    }
    return pid;
}

/* Run argv and wait for it, reaping orphans meanwhile; returns its status. */
static int run_and_wait(char *const argv[])
{
    pid_t pid = spawn(argv);
    if (pid < 0) {
        sleep(1);
        return -1;
    }
    int status = 0;
    pid_t w;
    while ((w = wait(&status)) != pid)
        if (w < 0 && errno == ECHILD)
            break;
    return status;
}

static bool config_says_text(void)
{
    int fd = open("/etc/default/init", O_RDONLY);
    if (fd < 0)
        return false;
    char line[128];
    bool text = false;
    while (read_line_fd(fd, line, sizeof(line)) >= 0)
        if (!strcmp(line, "CONSOLE=text"))
            text = true;
    close(fd);
    return text;
}

static void text_login(void)
{
    struct stat st;
    char *login[] = { "/bin/login", NULL };
    char *sh[] = { "/bin/sh", NULL };
    run_and_wait(stat(login[0], &st) == 0 ? login : sh);
}

int main(int argc, char **argv)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    umask(022);

    bool graphical = true;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "text") || !strcmp(argv[i], "single") || !strcmp(argv[i], "-s"))
            graphical = false;
    if (config_says_text())
        graphical = false;

    struct stat st;
    if (stat("/etc/rc", &st) == 0) {
        char *rc[] = { "/bin/sh", "/etc/rc", NULL };
        run_and_wait(rc);
    }

    int failures = 0;
    for (;;) {
        if (graphical && stat("/sbin/sdm", &st) == 0 && failures < 3) {
            char *sdm[] = { "/sbin/sdm", NULL };
            long t0 = uptime_ms();
            int status = run_and_wait(sdm);
            if (WIFEXITED(status) && WEXITSTATUS(status) == SDM_CONSOLE_REQUEST) {
                text_login();
                continue;
            }
            if (WIFEXITED(status) && (WEXITSTATUS(status) == SESSION_EXIT_REBOOT ||
                                      WEXITSTATUS(status) == SESSION_EXIT_HALT)) {   /* the desktop's Exit menu */
                printf("init: %s\n", WEXITSTATUS(status) == SESSION_EXIT_REBOOT ? "restarting" : "shutting down");
                sieos_reboot(WEXITSTATUS(status) == SESSION_EXIT_REBOOT ? REBOOT_RESTART : REBOOT_HALT);
            }
            /* A display manager that dies immediately is broken: count it. */
            if (uptime_ms() - t0 < 3000 && !(WIFEXITED(status) && WEXITSTATUS(status) == 0))
                failures++;
            else
                failures = 0;
            if (failures >= 3)
                printf("init: graphical login failed, using the text console\n");
            continue;
        }
        text_login();
    }
}
