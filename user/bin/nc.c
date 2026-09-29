/*
 * nc - netcat: connect to or listen on a TCP/UDP port and relay stdin/stdout.
 *   nc [-4|-6] [-u] host port        connect (IPv4 or IPv6, as the name resolves)
 *   nc -l [-4|-6] [-u] port          listen for one connection (or datagrams);
 *                                    on IPv4 and IPv6 unless -4 or -6
 */
#include "sieos.h"

/*
 * Copy stdin to the socket and the socket to stdout.  Either side may end
 * first (a half-close): the other keeps going; nc exits when both are done
 * (for UDP, when stdin ends).
 */
static int relay(int sock, bool udp, struct sockaddr_storage *peer, socklen_t *peerlen, bool have_peer)
{
    struct pollfd p[2] = { { STDIN_FILENO, POLLIN, 0 }, { sock, POLLIN, 0 } };
    char buf[4096];
    while (p[0].fd >= 0 || (p[1].fd >= 0 && !udp)) {
        int n = poll(p, 2, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        if (p[1].fd >= 0 && p[1].revents) {
            struct sockaddr_storage from;
            socklen_t fl = sizeof(from);
            long r = udp ? recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl) : read(sock, buf, sizeof(buf));
            if (r < 0 && errno != EINTR && errno != EAGAIN)
                return 1;
            if (r == 0 && !udp) {
                p[1].fd = -1;                        /* the peer is done sending */
            } else if (r > 0) {
                if (udp && !have_peer) {
                    *peer = from;
                    *peerlen = fl;
                    have_peer = true;
                }
                for (long off = 0; off < r;) {
                    long w = write(STDOUT_FILENO, buf + off, r - off);
                    if (w <= 0)
                        return 1;
                    off += w;
                }
            }
        }
        if (p[0].fd >= 0 && p[0].revents) {
            long r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r <= 0) {
                p[0].fd = -1;
                if (!udp)
                    shutdown(sock, SHUT_WR);
                continue;
            }
            for (long off = 0; off < r;) {
                long w = udp ? (have_peer ? sendto(sock, buf + off, r - off, 0, (struct sockaddr *)peer, *peerlen) : r - off)
                             : write(sock, buf + off, r - off);
                if (w < 0) {
                    perror("nc: send");
                    return 1;
                }
                off += w;
            }
        }
    }
    return 0;
}

static int port_of(const struct sockaddr_storage *a)
{
    return ntohs(a->ss_family == AF_INET6 ? ((const struct sockaddr_in6 *)a)->sin6_port
                                          : ((const struct sockaddr_in *)a)->sin_port);
}

int main(int argc, char **argv)
{
    bool listen_mode = false, udp = false;
    int family = AF_UNSPEC;
    const char *args[2];
    int na = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            for (const char *f = a + 1; *f; f++) {
                if (*f == 'l') listen_mode = true;
                else if (*f == 'u') udp = true;
                else if (*f == '4') family = AF_INET;
                else if (*f == '6') family = AF_INET6;
                else na = 99;
            }
        } else if (na < 2) {
            args[na++] = a;
        } else {
            na = 99;
        }
    }
    if ((listen_mode && na != 1) || (!listen_mode && na != 2)) {
        dprintf(STDERR_FILENO, "usage: nc [-4|-6] [-u] host port | nc -l [-4|-6] [-u] port\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    int type = udp ? SOCK_DGRAM : SOCK_STREAM;
    struct sockaddr_storage peer;
    socklen_t peerlen = sizeof(peer);
    char ip[INET6_ADDRSTRLEN];
    if (listen_mode) {
        int port = atoi(args[0]);
        int sock;
        if (family == AF_INET) {
            sock = socket(AF_INET, type, 0);
            struct sockaddr_in a = make_addr(0, port);
            if (sock < 0 || bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
                perror("nc: bind");
                return 1;
            }
        } else {                                     /* IPv6, and IPv4 mapped unless -6 */
            sock = socket(AF_INET6, type, 0);
            int only = family == AF_INET6;
            setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof(only));
            struct sockaddr_in6 a;
            memset(&a, 0, sizeof(a));
            a.sin6_family = AF_INET6;
            a.sin6_port = htons(port);
            if (sock < 0 || bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
                perror("nc: bind");
                return 1;
            }
        }
        if (udp)
            return relay(sock, true, &peer, &peerlen, false);
        if (listen(sock, 1) < 0) {
            perror("nc: listen");
            return 1;
        }
        int c = accept(sock, (struct sockaddr *)&peer, &peerlen);
        if (c < 0) {
            perror("nc: accept");
            return 1;
        }
        close(sock);
        addr_to_str((struct sockaddr *)&peer, ip, sizeof(ip));
        dprintf(STDERR_FILENO, peer.ss_family == AF_INET6 ? "nc: connection from [%s]:%d\n" : "nc: connection from %s:%d\n",
                ip, port_of(&peer));
        return relay(c, false, &peer, &peerlen, true);
    }
    struct sockaddr_storage addrs[8];
    socklen_t lens[8];
    int n = resolve_addrs(args[0], family, atoi(args[1]), addrs, lens, 8);
    if (!n) {
        dprintf(STDERR_FILENO, "nc: unknown host %s\n", args[0]);
        return 1;
    }
    int err = 0;
    for (int i = 0; i < n; i++) {
        int sock = socket(addrs[i].ss_family, type, 0);
        if (sock < 0) {
            err = errno;
            continue;
        }
        if (connect(sock, (struct sockaddr *)&addrs[i], lens[i]) == 0) {
            peer = addrs[i];
            peerlen = lens[i];
            return relay(sock, udp, &peer, &peerlen, true);
        }
        err = errno;
        close(sock);
    }
    dprintf(STDERR_FILENO, "nc: %s port %s: %s\n", args[0], args[1], strerror(err));
    return 1;
}
