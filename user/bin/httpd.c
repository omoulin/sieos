/*
 * httpd - a small HTTP/1.0 file server.
 *   httpd [-p port] [root]       (default: port 80 as root, else 8080; root /var/www)
 * Serves files and directory listings; one child process per connection.
 */
#include "sieos.h"

static const char *mime(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "text/plain";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "text/html";
    if (!strcmp(dot, ".css")) return "text/css";
    if (!strcmp(dot, ".js")) return "application/javascript";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".txt") || !strcmp(dot, ".c") || !strcmp(dot, ".h")) return "text/plain";
    return "application/octet-stream";
}

static void reply(int c, int code, const char *status, const char *type, const char *body)
{
    dprintf(c, "HTTP/1.0 %d %s\r\nServer: SIEOS-httpd/1.0\r\nContent-Type: %s\r\nContent-Length: %lu\r\n\r\n%s",
            code, status, type, (unsigned long)strlen(body), body);
}

static void serve(int c, const char *root)
{
    char req[2048];
    long len = 0;
    while (len < (long)sizeof(req) - 1) {
        long r = read(c, req + len, sizeof(req) - 1 - len);
        if (r <= 0)
            break;
        len += r;
        req[len] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
            break;
    }
    req[len] = 0;
    char method[8], path[512];
    char *sp1 = strchr(req, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2 || sp1 - req >= (long)sizeof(method) || sp2 - sp1 - 1 >= (long)sizeof(path)) {
        reply(c, 400, "Bad Request", "text/plain", "bad request\n");
        return;
    }
    memcpy(method, req, sp1 - req);
    method[sp1 - req] = 0;
    memcpy(path, sp1 + 1, sp2 - sp1 - 1);
    path[sp2 - sp1 - 1] = 0;
    char *q = strchr(path, '?');
    if (q)
        *q = 0;
    printf("httpd: %s %s\n", method, path);
    if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
        reply(c, 501, "Not Implemented", "text/plain", "only GET is supported\n");
        return;
    }
    if (strstr(path, "..")) {
        reply(c, 403, "Forbidden", "text/plain", "forbidden\n");
        return;
    }
    char full[768];
    snprintf(full, sizeof(full), "%s%s", root, path);
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        char idx[800];
        snprintf(idx, sizeof(idx), "%s/index.html", full);
        if (stat(idx, &st) == 0) {
            strlcpy(full, idx, sizeof(full));
        } else {
            int dfd = open(full, O_RDONLY | O_DIRECTORY);
            if (dfd < 0) {
                reply(c, 403, "Forbidden", "text/plain", "forbidden\n");
                return;
            }
            dprintf(c, "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n<html><body><h2>Index of %s</h2><ul>",
                    path);
            char buf[2048];
            long n;
            while ((n = getdents(dfd, (struct dirent *)buf, sizeof(buf))) > 0)
                for (long off = 0; off < n;) {
                    struct dirent *d = (struct dirent *)(buf + off);
                    off += d->d_reclen;
                    if (strcmp(d->d_name, "."))
                        dprintf(c, "<li><a href=\"%s%s%s\">%s</a></li>", path,
                                path[strlen(path) - 1] == '/' ? "" : "/", d->d_name, d->d_name);
                }
            dprintf(c, "</ul><hr>SIEOS httpd</body></html>\n");
            close(dfd);
            return;
        }
    }
    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        reply(c, errno == EACCES ? 403 : 404, errno == EACCES ? "Forbidden" : "Not Found", "text/html",
              "<html><body><h2>404 Not Found</h2></body></html>\n");
        return;
    }
    fstat(fd, &st);
    dprintf(c, "HTTP/1.0 200 OK\r\nServer: SIEOS-httpd/1.0\r\nContent-Type: %s\r\nContent-Length: %lu\r\n\r\n",
            mime(full), st.st_size);
    if (!strcmp(method, "GET")) {
        char buf[8192];
        long n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            if (write(c, buf, n) < 0)
                break;
    }
    close(fd);
}

int main(int argc, char **argv)
{
    int port = geteuid() == 0 ? 80 : 8080;
    const char *root = "/var/www";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc)
            port = atoi(argv[++i]);
        else
            root = argv[i];
    }
    signal(SIGPIPE, SIG_IGN);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = make_addr(0, port);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 8) < 0) {
        dprintf(STDERR_FILENO, "httpd: port %d: %s\n", port, strerror(errno));
        return 1;
    }
    printf("httpd: serving %s on port %d\n", root, port);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t l = sizeof(peer);
        int c = accept(s, (struct sockaddr *)&peer, &l);
        if (c < 0) {
            if (errno == EINTR)
                continue;
            perror("httpd: accept");
            return 1;
        }
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        pid_t pid = fork();
        if (pid == 0) {
            close(s);
            serve(c, root);
            close(c);
            exit(0);
        }
        close(c);
    }
}
