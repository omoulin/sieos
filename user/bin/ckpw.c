/*
 * ckpw - check one's own password (for the screen lock of the desktop):
 * the password on the standard input's first line, checked against the
 * shadow entry of the user running ckpw (its real user ID), never another
 * one's.  Exit status 0 if it is right, 1 if not (after a second: guesses
 * stay slow), 2 on a usage error.  Installed set-user-ID root: the shadow
 * file is readable by root only.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"

int main(void)
{
    char pass[128];
    if (!fgets(pass, sizeof(pass), stdin))
        return 2;
    pass[strcspn(pass, "\r\n")] = 0;
    struct passwd *pw = getpwuid(getuid());
    bool ok = false;
    if (pw) {
        char hash[160];
        snprintf(hash, sizeof(hash), "%s", pw->pw_passwd);
        if (!strcmp(hash, "x")) {
            struct spwd *sp = getspnam(pw->pw_name);
            snprintf(hash, sizeof(hash), "%s", sp ? sp->sp_pwdp : "!");
        }
        ok = hash[0] != '!' && hash[0] != '*' && check_password(pass, hash);
    }
    memset(pass, 0, sizeof(pass));
    if (!ok) {
        sleep(1);
        return 1;
    }
    return 0;
}
