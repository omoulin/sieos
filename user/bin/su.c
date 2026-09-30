/*
 * su - become another user (installed set-user-ID root).
 *   su [-] [user] [-c command]
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(int argc, char **argv)
{
    bool login = false;
    const char *target = "root", *cmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-") || !strcmp(argv[i], "-l"))
            login = true;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc)
            cmd = argv[++i];
        else
            target = argv[i];
    }
    struct passwd *pwp = getpwnam(target);
    if (!pwp) {
        dprintf(STDERR_FILENO, "su: unknown user %s\n", target);
        return 1;
    }
    struct passwd pw = *pwp;
    char name[64], dir[128], shell[64], hash[160];
    strlcpy(name, pw.pw_name, sizeof(name));
    strlcpy(dir, pw.pw_dir, sizeof(dir));
    strlcpy(shell, pw.pw_shell, sizeof(shell));
    strlcpy(hash, pw.pw_passwd, sizeof(hash));
    if (!strcmp(hash, "x")) {
        struct spwd *sp = getspnam(name);
        strlcpy(hash, sp ? sp->sp_pwdp : "!", sizeof(hash));
    }

    if (getuid() != 0) {
        char *pass = getpass("Password: ");
        if (!pass || hash[0] == '!' || hash[0] == '*' || !check_password(pass, hash)) {
            sleep(1);
            dprintf(STDERR_FILENO, "su: Sorry\n");
            return 1;
        }
    }
    if (initgroups(name, pw.pw_gid) < 0 || setgid(pw.pw_gid) < 0 || setuid(pw.pw_uid) < 0) {
        perror("su");
        return 1;
    }
    if (login) {
        if (chdir(dir) < 0)
            chdir("/");
        const char *term = getenv("TERM");
        char termbuf[32];
        strlcpy(termbuf, term ? term : "sieos", sizeof(termbuf));
        clearenv();
        setenv("TERM", termbuf, 1);
        setenv("PATH", pw.pw_uid == 0 ? "/sbin:/bin:/usr/bin:/usr/gnu/bin" : "/bin:/usr/bin:/usr/gnu/bin:/sbin", 1);
    }
    setenv("HOME", dir, 1);
    setenv("SHELL", shell, 1);
    setenv("ENV", "/etc/shrc", 1);
    setenv("USER", name, 1);
    setenv("LOGNAME", name, 1);
    char *args[4] = { login ? "-sh" : "sh", NULL, NULL, NULL };
    if (cmd) {
        args[1] = "-c";
        args[2] = (char *)cmd;
    }
    execv(shell, args);
    perror(shell);
    return 1;
}
