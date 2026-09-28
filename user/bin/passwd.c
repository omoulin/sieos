/*
 * passwd - change a password (installed set-user-ID root).
 *   passwd [user]
 */
#include "sieos.h"

#define MAXLINES 64

static void make_salt(char *salt)
{
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";
    unsigned long seed = time(NULL) ^ (uptime_ms() << 16) ^ getpid();
    for (int i = 0; i < 12; i++) {
        seed = seed * 6364136223846793005UL + 1442695040888963407UL;
        salt[i] = chars[(seed >> 33) % 64];
    }
    salt[12] = 0;
}

int main(int argc, char **argv)
{
    uid_t uid = getuid();
    struct passwd *me = getpwuid(uid);
    char user[64];
    strlcpy(user, argc > 1 ? argv[1] : (me ? me->pw_name : ""), sizeof(user));
    struct passwd *pw = getpwnam(user);
    if (!pw) {
        dprintf(STDERR_FILENO, "passwd: unknown user %s\n", user);
        return 1;
    }
    if (uid != 0 && pw->pw_uid != uid) {
        dprintf(STDERR_FILENO, "passwd: permission denied\n");
        return 1;
    }
    struct spwd *sp = getspnam(user);
    char old_hash[160];
    strlcpy(old_hash, sp ? sp->sp_pwdp : "", sizeof(old_hash));

    printf("Changing password for %s\n", user);
    if (uid != 0) {
        char *cur = getpass("Current password: ");
        if (!cur || !check_password(cur, old_hash)) {
            sleep(1);
            dprintf(STDERR_FILENO, "passwd: authentication failed\n");
            return 1;
        }
    }
    char p1[128];
    char *p = getpass("New password: ");
    if (!p || strlen(p) < 4) {
        dprintf(STDERR_FILENO, "passwd: password too short (minimum 4 characters)\n");
        return 1;
    }
    strlcpy(p1, p, sizeof(p1));
    p = getpass("Re-enter new password: ");
    if (!p || strcmp(p, p1) != 0) {
        dprintf(STDERR_FILENO, "passwd: passwords do not match\n");
        return 1;
    }
    char salt[16];
    make_salt(salt);
    char *hash = crypt_password(p1, salt);

    /* Rewrite /etc/shadow with the new entry. */
    static char buf[8192];
    int fd = open("/etc/shadow", O_RDONLY);
    long n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : 0;
    if (fd >= 0)
        close(fd);
    buf[n > 0 ? n : 0] = 0;
    static char out[8192 + 256];
    size_t o = 0;
    bool replaced = false;
    char *rest = buf;
    while (rest && *rest) {
        char *line = strsep(&rest, "\n");
        size_t ul = strlen(user);
        if (!strncmp(line, user, ul) && line[ul] == ':') {
            char *tail = strchr(line + ul + 1, ':');
            o += snprintf(out + o, sizeof(out) - o, "%s:%s%s\n", user, hash, tail ? tail : "");
            replaced = true;
        } else if (*line) {
            o += snprintf(out + o, sizeof(out) - o, "%s\n", line);
        }
    }
    if (!replaced)
        o += snprintf(out + o, sizeof(out) - o, "%s:%s:::::::\n", user, hash);
    fd = open("/etc/shadow", O_WRONLY | O_TRUNC);
    if (fd < 0 || write(fd, out, o) != (long)o) {
        perror("passwd: /etc/shadow");
        return 1;
    }
    close(fd);
    printf("passwd: password updated successfully\n");
    return 0;
}
