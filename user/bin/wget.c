/*
 * wget - download a URL over HTTP/1.0.
 *   wget [-q] [-O file|-] http://host[:port]/path     (host: a name, a.b.c.d or [IPv6])
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
    char hosthdr[160];                               /* for the Host header: as in the URL */
    strlcpy(hosthdr, host, sizeof(hosthdr));
    char *hname = host, *colon;
    if (host[0] == '[') {                            /* [IPv6 address] */
        char *close = strchr(host, ']');
        if (!close) {
            dprintf(STDERR_FILENO, "wget: bad address %s\n", host);
            return 2;
        }
        *close = 0;
        hname = host + 1;
        colon = close[1] == ':' ? close + 1 : NULL;
    } else {
        colon = strchr(host, ':');
    }
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
    }
    struct sockaddr_storage addrs[8];
    socklen_t lens[8];
    int na = resolve_addrs(hname, AF_UNSPEC, port, addrs, lens, 8);
    if (!na) {
        dprintf(STDERR_FILENO, "wget: unable to resolve host %s\n", hname);
        return 1;
    }
    int s = -1;
    for (int i = 0; i < na && s < 0; i++) {
        char ip[INET6_ADDRSTRLEN];
        addr_to_str((struct sockaddr *)&addrs[i], ip, sizeof(ip));
        if (!quiet)
            dprintf(STDERR_FILENO, addrs[i].ss_family == AF_INET6 ? "Connecting to %s ([%s]):%d... " :
                    "Connecting to %s (%s):%d... ", hname, ip, port);
        s = socket(addrs[i].ss_family, SOCK_STREAM, 0);
        if (s >= 0 && connect(s, (struct sockaddr *)&addrs[i], lens[i]) < 0) {
            if (!quiet || i == na - 1)
                dprintf(STDERR_FILENO, "failed: %s\n", strerror(errno));
            close(s);
            s = -1;
        }
    }
    if (s < 0)
        return 1;
    if (!quiet)
        dprintf(STDERR_FILENO, "connected.\n");
    char req[768];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: SIEOS-wget/1.0\r\n"
                     "Connection: close\r\n\r\n", path, hosthdr);
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
