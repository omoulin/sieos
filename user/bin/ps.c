/* ps - report process status:  ps [-e] (default: processes of this session / user) */
#include "sieos.h"

int main(int argc, char **argv)
{
    bool all = argc > 1 && (!strcmp(argv[1], "-e") || !strcmp(argv[1], "-ef") || !strcmp(argv[1], "aux"));
    struct procinfo p[64];
    int n = procinfo(p, 64);
    if (n < 0) {
        perror("ps");
        return 1;
    }
    pid_t sid = getsid(0);
    uid_t uid = getuid();
    printf("%-8s %5s %5s %5s %5s %-4s %3s %6s %6s %s\n", "USER", "PID", "PPID", "PGID", "SID", "STAT",
           "CPU", "RSS", "TIME", "COMMAND");
    for (int i = 0; i < n; i++) {
        if (!all && p[i].sid != sid && p[i].uid != (int)uid)
            continue;
        if (p[i].pid == 0 && !all)
            continue;
        static const char st[] = "?-RRSZT";
        char stat[5] = { st[p[i].state < 7 ? p[i].state : 0], 0, 0, 0, 0 };
        int k = 1;
        if (p[i].pid == p[i].sid)
            stat[k++] = 's';
        if (p[i].tty_fg)
            stat[k++] = '+';
        struct passwd *pw = getpwuid(p[i].euid);
        char user[16];
        if (pw)
            strlcpy(user, pw->pw_name, sizeof(user));
        else
            snprintf(user, sizeof(user), "%d", p[i].euid);
        unsigned long secs = p[i].ticks / 100;
        char cpu[8];
        if (p[i].cpu >= 0)
            snprintf(cpu, sizeof(cpu), "%d", p[i].cpu);
        else
            strcpy(cpu, "-");
        printf("%-8s %5d %5d %5d %5d %-4s %3s %5luK %3lu:%02lu %s\n", user, p[i].pid, p[i].ppid,
               p[i].pgid, p[i].sid, stat, cpu, p[i].mem_kb, secs / 60, secs % 60, p[i].name);
    }
    return 0;
}
