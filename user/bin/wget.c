/*
 * wget - download a URL over HTTP/1.0.
 *   wget [-q] [-O file|-] http://host[:port]/path
 * Without -O the file is saved under the last path component
 * (index.html for "/").
 */
#include "sieos.h"

int main(int argc, char **argv)
{
    const char *url = NULL, *out = NULL;
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-q")) quiet = true;
        else url = argv[i];
    }
    if (!url) {
        dprintf(STDERR_FILENO, "usage: wget [-q] [-O file|-] http://host[:port]/path\n");
        return 2;
    }
    if (!strncmp(url, "http://", 7))
        url += 7;
    else if (strstr(url, "://")) {
        dprintf(STDERR_FILENO, "wget: only http:// URLs are supported\n");
        return 2;
    }
    char host[128], path[512];
    const char *slash = strchr(url, '/');
    size_t hl = slash ? (size_t)(slash - url) : strlen(url);
    if (hl >= sizeof(host))
        return 2;
    memcpy(host, url, hl);
    host[hl] = 0;
    strlcpy(path, slash ? slash : "/", sizeof(path));
    int port = 80;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
    }
    unsigned int addr;
    if (resolve_host(host, &addr) < 0) {
        dprintf(STDERR_FILENO, "wget: unable to resolve host %s\n", host);
        return 1;
    }
    if (!quiet)
        dprintf(STDERR_FILENO, "Connecting to %s (%s):%d... ", host, inet_ntoa((struct in_addr){ addr }), port);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = make_addr(addr, port);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        dprintf(STDERR_FILENO, "failed: %s\n", strerror(errno));
        return 1;
    }
    if (!quiet)
        dprintf(STDERR_FILENO, "connected.\n");
    char req[768];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: SIEOS-wget/1.0\r\n"
                     "Connection: close\r\n\r\n", path, host);
    write(s, req, n);

    /* Read the response header. */
    static char buf[16384];
    long len = 0, hdr_end = -1;
    while (hdr_end < 0 && len < (long)sizeof(buf) - 1) {
        long r = read(s, buf + len, sizeof(buf) - 1 - len);
        if (r <= 0)
            break;
        len += r;
        buf[len] = 0;
        char *e = strstr(buf, "\r\n\r\n");
        if (e)
            hdr_end = e - buf + 4;
    }
    if (hdr_end < 0) {
        dprintf(STDERR_FILENO, "wget: malformed response\n");
        return 1;
    }
    char *eol = strstr(buf, "\r\n");
    *eol = 0;
    int status = 0;
    char *sp = strchr(buf, ' ');
    if (sp)
        status = atoi(sp + 1);
    if (!quiet)
        dprintf(STDERR_FILENO, "HTTP request sent, response: %s\n", sp ? sp + 1 : buf);

    char name[256];
    if (!out) {
        const char *base = strrchr(path, '/');
        base = base && base[1] ? base + 1 : "index.html";
        strlcpy(name, base, sizeof(name));
        char *q = strchr(name, '?');
        if (q)
            *q = 0;
        out = name;
    }
    int fd = strcmp(out, "-") ? open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644) : STDOUT_FILENO;
    if (fd < 0) {
        dprintf(STDERR_FILENO, "wget: %s: %s\n", out, strerror(errno));
        return 1;
    }
    long total = len - hdr_end;
    write(fd, buf + hdr_end, total);
    long r;
    while ((r = read(s, buf, sizeof(buf))) > 0) {
        write(fd, buf, r);
        total += r;
    }
    if (fd != STDOUT_FILENO)
        close(fd);
    close(s);
    if (!quiet)
        dprintf(STDERR_FILENO, "Saved %ld bytes to '%s'\n", total, out);
    return status >= 200 && status < 400 ? 0 : 1;
}
