/*
 * config.c - ~/.sia/config (model connection), ~/.sia/status (for the
 * desktop) and plain-text conversion of model replies.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "libsia.h"

static char cfg_path[300], status_path[300], dir_path[300];

static void paths(void)
{
    if (cfg_path[0])
        return;
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/";
    snprintf(dir_path, sizeof(dir_path), "%s/.sia", home);
    snprintf(cfg_path, sizeof(cfg_path), "%s/.sia/config", home);
    snprintf(status_path, sizeof(status_path), "%s/.sia/status", home);
}

const char *sia_config_path(void)
{
    paths();
    return cfg_path;
}

int sia_config_load(struct sia_config *c)
{
    paths();
    memset(c, 0, sizeof(*c));
    int fd = open(cfg_path, O_RDONLY);
    if (fd < 0)
        return -1;
    char line[700];
    while (read_line_fd(fd, line, sizeof(line)) >= 0) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(line, "endpoint"))
            snprintf(c->endpoint, sizeof(c->endpoint), "%s", v);
        else if (!strcmp(line, "model"))
            snprintf(c->model, sizeof(c->model), "%s", v);
        else if (!strcmp(line, "api_key"))
            snprintf(c->api_key, sizeof(c->api_key), "%s", v);
        else if (!strcmp(line, "auto_approve"))
            c->auto_approve = !strcmp(v, "yes");
    }
    close(fd);
    memset(line, 0, sizeof(line));
    return c->endpoint[0] && c->model[0] && c->api_key[0] ? 1 : 0;
}

bool sia_config_save(const struct sia_config *c, char *err, size_t errlen)
{
    paths();
    mkdir(dir_path, 0700);
    chmod(dir_path, 0700);
    int fd = open(cfg_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        if (err)
            snprintf(err, errlen, "cannot write %s: %s", cfg_path, strerror(errno));
        return false;
    }
    fchmod(fd, 0600);
    dprintf(fd, "# sia model connection (Azure AI Foundry). Keep this file private.\n");
    dprintf(fd, "endpoint=%s\nmodel=%s\napi_key=%s\nauto_approve=%s\n", c->endpoint, c->model, c->api_key,
            c->auto_approve ? "yes" : "no");
    close(fd);
    return true;
}

void sia_status_write(const char *state, const char *last_action)
{
    paths();
    int fd = open(status_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    dprintf(fd, "state=%s\nlast=%s\n", state, last_action ? last_action : "");
    close(fd);
}

bool sia_status_read(char *state, size_t slen, char *last, size_t llen)
{
    paths();
    int fd = open(status_path, O_RDONLY);
    if (fd < 0)
        return false;
    char line[256];
    state[0] = last[0] = 0;
    while (read_line_fd(fd, line, sizeof(line)) >= 0) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!strncmp(line, "state=", 6))
            snprintf(state, slen, "%s", line + 6);
        else if (!strncmp(line, "last=", 5))
            snprintf(last, llen, "%s", line + 5);
    }
    close(fd);
    return true;
}

void sia_plain_text(const char *s, struct sbuf *b)
{
    bool line_start = true;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (line_start && !strncmp(s, "```", 3)) {        /* drop code fence lines */
            while (*s && *s != '\n')
                s++;
            if (*s)
                s++;
            continue;
        }
        if (!strncmp(s, "**", 2) || !strncmp(s, "__", 2)) {
            s += 2;
            continue;
        }
        if (c < 0x80) {
            sb_putc(b, *s++);
            line_start = c == '\n';
            continue;
        }
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        unsigned cp = c & (0x3F >> extra);
        s++;
        for (int i = 0; i < extra && (*s & 0xC0) == 0x80; i++)
            cp = cp << 6 | (*s++ & 0x3F);
        const char *rep = "?";
        switch (cp) {
        case 0x2018: case 0x2019: case 0x2032: rep = "'"; break;
        case 0x201C: case 0x201D: case 0x2033: rep = "\""; break;
        case 0x2013: case 0x2014: case 0x2212: rep = "-"; break;
        case 0x2026: rep = "..."; break;
        case 0x2022: case 0x25CF: case 0x25AA: rep = "*"; break;
        case 0x2192: rep = "->"; break;
        case 0x2190: rep = "<-"; break;
        case 0x00A0: case 0x2009: case 0x202F: rep = " "; break;
        case 0x00D7: rep = "x"; break;
        case 0x2264: rep = "<="; break;
        case 0x2265: rep = ">="; break;
        case 0x2713: case 0x2714: case 0x2705: rep = "[ok]"; break;
        case 0x274C: case 0x2717: rep = "[x]"; break;
        case 0x00B0: rep = " deg"; break;
        default:
            if (cp >= 0x1F000)                           /* emoji */
                rep = "";
        }
        sb_puts(b, rep);
        line_start = false;
    }
}
