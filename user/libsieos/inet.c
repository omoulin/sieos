/*
 * inet.c - libsieos: IPv4 helpers and host name resolution (/etc/hosts,
 * then an A query to the DNS server the interface was configured with), and
 * resolve_addrs for IPv4 and IPv6 (getaddrinfo first).
 */
#include "sieos.h"
#include <netdb.h>

const char *ip_to_str(unsigned int ip, char *buf)
{
    snprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return buf;
}

struct sockaddr_in make_addr(unsigned int addr_net, unsigned short port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = addr_net;
    return a;
}

static int dotted(const char *s, unsigned int *addr)
{
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1)
        return 0;
    *addr = a.s_addr;
    return 1;
}

static int hosts_lookup(const char *name, unsigned int *addr)
{
    FILE *f = fopen("/etc/hosts", "re");
    if (!f)
        return 0;
    char line[256];
    int found = 0;
    while (!found && fgets(line, sizeof(line), f)) {
        char *p = strpbrk(line, "#\n");
        if (p)
            *p = 0;
        char *rest = line, *tok, *ip = NULL;
        while ((tok = strsep(&rest, " \t")) != NULL) {
            if (!*tok)
                continue;
            if (!ip)
                ip = tok;
            else if (!strcasecmp(tok, name) && dotted(ip, addr))
                found = 1;
        }
    }
    fclose(f);
    return found;
}

static int dns_lookup(const char *name, unsigned int *addr)
{
    struct netinfo ni;
    if (netinfo(&ni) < 0 || !ni.dns)
        return 0;
    unsigned char q[512];
    int n = 0;
    unsigned short id = (unsigned short)(uptime_ms() ^ getpid());
    q[n++] = id >> 8; q[n++] = id; q[n++] = 0x01; q[n++] = 0x00;     /* recursion desired */
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 0;                   /* 1 question */
    q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        int len = dot ? dot - p : (int)strlen(p);
        if (len == 0 || len > 63 || n + len + 2 > 400)
            return 0;
        q[n++] = len;
        memcpy(q + n, p, len);
        n += len;
        p += len;
        if (*p == '.')
            p++;
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;                                           /* type A */
    q[n++] = 0; q[n++] = 1;                                           /* class IN */

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    struct timeval tv = { 1, 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in to = make_addr(htonl(ni.dns), 53);
    int ok = 0;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        if (sendto(fd, q, n, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
            break;
        unsigned char r[512];
        long got = recv(fd, r, sizeof(r), 0);
        if (got < 12 || ((r[0] << 8) | r[1]) != id)
            continue;
        if ((r[3] & 0x0F) != 0)                  /* rcode: NXDOMAIN etc. */
            break;
        int qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
        long off = 12;
        for (int i = 0; i < qd && off < got; i++) {          /* skip questions */
            while (off < got && r[off] && !(r[off] & 0xC0))
                off += r[off] + 1;
            off += (off < got && (r[off] & 0xC0)) ? 2 : 1;
            off += 4;
        }
        for (int i = 0; i < an && off + 10 <= got; i++) {
            while (off < got && r[off] && !(r[off] & 0xC0))  /* name */
                off += r[off] + 1;
            off += (off < got && (r[off] & 0xC0)) ? 2 : 1;
            if (off + 10 > got)
                break;
            int type = (r[off] << 8) | r[off + 1];
            int rdlen = (r[off + 8] << 8) | r[off + 9];
            off += 10;
            if (type == 1 && rdlen == 4 && off + 4 <= got) {
                memcpy(addr, r + off, 4);
                ok = 1;
                break;
            }
            off += rdlen;
        }
        if (!ok)
            break;
    }
    close(fd);
    return ok;
}

int resolve_host(const char *name, unsigned int *addr)
{
    if (dotted(name, addr) || hosts_lookup(name, addr) || dns_lookup(name, addr))
        return 0;
    errno = ENOENT;
    return -1;
}

int resolve_addrs(const char *name, int family, unsigned short port, struct sockaddr_storage *out,
                  socklen_t *lens, int max)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    int n = 0;
    if (getaddrinfo(name, NULL, &hints, &res) == 0) {
        for (struct addrinfo *r = res; r && n < max; r = r->ai_next) {
            if (r->ai_family != AF_INET && r->ai_family != AF_INET6)
                continue;
            memset(&out[n], 0, sizeof(out[n]));
            memcpy(&out[n], r->ai_addr, r->ai_addrlen);
            lens[n] = r->ai_addrlen;
            if (r->ai_family == AF_INET)
                ((struct sockaddr_in *)&out[n])->sin_port = htons(port);
            else
                ((struct sockaddr_in6 *)&out[n])->sin6_port = htons(port);
            n++;
        }
        freeaddrinfo(res);
    }
    unsigned int a;
    if (!n && max > 0 && family != AF_INET6 && resolve_host(name, &a) == 0) {
        struct sockaddr_in sin = make_addr(a, port);  /* (no resolv.conf: the interface's server) */
        memset(&out[0], 0, sizeof(out[0]));
        memcpy(&out[0], &sin, sizeof(sin));
        lens[0] = sizeof(sin);
        n = 1;
    }
    return n;
}

const char *addr_to_str(const struct sockaddr *sa, char *buf, size_t n)
{
    const void *a = sa->sa_family == AF_INET6 ? (const void *)&((const struct sockaddr_in6 *)sa)->sin6_addr
                                              : (const void *)&((const struct sockaddr_in *)sa)->sin_addr;
    if (!inet_ntop(sa->sa_family, a, buf, n))
        snprintf(buf, n, "?");
    return buf;
}
