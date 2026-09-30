/*
 * settings.c - The desktop's settings: ~/.facet/settings over
 * /etc/facet/settings (facet/settings.h).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "facet/settings.h"

#define MAXFILE 4096

static char *slurp(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    char *b = malloc(MAXFILE + 1);
    ssize_t n = b ? read(fd, b, MAXFILE) : -1;
    close(fd);
    if (n < 0) {
        free(b);
        return NULL;
    }
    b[n] = 0;
    *len = (size_t)n;
    return b;
}

static bool lookup(const char *path, const char *key, char *out, size_t n)
{
    size_t len, kl = strlen(key);
    char *b = slurp(path, &len);
    if (!b)
        return false;
    bool found = false;
    for (char *line = b, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        while (*line == ' ' || *line == '\t')
            line++;
        if (*line == '#' || strncmp(line, key, kl) || line[kl] != '=')
            continue;
        char *v = line + kl + 1;
        v[strcspn(v, "\r")] = 0;
        snprintf(out, n, "%s", v);
        found = true;                            /* (the last one wins) */
    }
    free(b);
    return found;
}

const char *fct_settings_path(void)
{
    static char path[256];
    const char *home = getenv("HOME");
    snprintf(path, sizeof(path), "%s/.facet/settings", home && *home ? home : "");
    return path;
}

bool fct_setting_get_system(const char *key, char *out, size_t n)
{
    return lookup(FCT_SETTINGS_SYSTEM, key, out, n);
}

bool fct_setting_get(const char *key, char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (home && *home && lookup(fct_settings_path(), key, out, n))
        return true;
    return fct_setting_get_system(key, out, n);
}

bool fct_setting_set(const char *key, const char *value)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return false;
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/.facet", home);
    mkdir(dir, 0755);
    const char *path = fct_settings_path();
    size_t len = 0, kl = strlen(key);
    char *old = slurp(path, &len);
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(old);
        return false;
    }
    bool ok = true;
    for (char *line = old, *next; line && *line; line = next) {   /* every other line, kept */
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (!strncmp(line, key, kl) && line[kl] == '=')
            continue;
        ok &= write(fd, line, strlen(line)) >= 0 && write(fd, "\n", 1) == 1;
    }
    if (value) {
        char l[512];
        int n = snprintf(l, sizeof(l), "%s=%s\n", key, value);
        ok &= write(fd, l, n) == n;
    }
    free(old);
    ok &= fsync(fd) == 0;
    close(fd);
    return ok && rename(tmp, path) == 0;
}
