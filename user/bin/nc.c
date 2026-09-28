/*
 * nc - netcat: connect to or listen on a TCP/UDP port and relay stdin/stdout.
 *   nc [-u] host port        connect
 *   nc -l [-u] port          listen for one connection (or datagrams)
 */
#include "sieos.h"

static int relay(int sock, bool udp, struct sockaddr_in *peer, bool have_peer)
{
    struct pollfd p[2] = { { STDIN_FILENO, POLLIN, 0 }, { sock, POLLIN, 0 } };
    bool stdin_open = true;
    char buf[4096];
    for (;;) {
        int n = poll(p, 2, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        if (p[1].revents) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            long r = udp ? recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl) : read(sock, buf, sizeof(buf));
            if (r <= 0)
                return r < 0;
            if (udp && !have_peer) {
                *peer = from;
                have_peer = true;
            }
            write(STDOUT_FILENO, buf, r);
        }
        if (stdin_open && p[0].revents) {
            long r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r <= 0) {
                stdin_open = false;
                p[0].fd = -1;
                if (!udp)
                    shutdown(sock, SHUT_WR);
                continue;
            }
            long w = udp ? (have_peer ? sendto(sock, buf, r, 0, (struct sockaddr *)peer, sizeof(*peer)) : r) : write(sock, buf, r);
            if (w < 0) {
                perror("nc: send");
                return 1;
            }
        }
    }
}

int main(int argc, char **argv)
{
    bool listen_mode = false, udp = false;
    const char *args[2];
    int na = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) listen_mode = true;
        else if (!strcmp(argv[i], "-u")) udp = true;
        else if (!strcmp(argv[i], "-lu") || !strcmp(argv[i], "-ul")) listen_mode = udp = true;
        else if (na < 2) args[na++] = argv[i];
    }
    if ((listen_mode && na != 1) || (!listen_mode && na != 2)) {
        dprintf(STDERR_FILENO, "usage: nc [-u] host port | nc -l [-u] port\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    int sock = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (sock < 0) {
        perror("nc: socket");
        return 1;
    }
    struct sockaddr_in peer;
    if (listen_mode) {
        struct sockaddr_in a = make_addr(0, atoi(args[0]));
        if (bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
            perror("nc: bind");
            return 1;
        }
        if (udp)
            return relay(sock, true, &peer, false);
        if (listen(sock, 1) < 0) {
            perror("nc: listen");
            return 1;
        }
        socklen_t l = sizeof(peer);
        int c = accept(sock, (struct sockaddr *)&peer, &l);
        if (c < 0) {
            perror("nc: accept");
            return 1;
        }
        close(sock);
        dprintf(STDERR_FILENO, "nc: connection from %s:%d\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        return relay(c, false, &peer, true);
    }
    unsigned int addr;
    if (resolve_host(args[0], &addr) < 0) {
        dprintf(STDERR_FILENO, "nc: unknown host %s\n", args[0]);
        return 1;
    }
    peer = make_addr(addr, atoi(args[1]));
    if (connect(sock, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        dprintf(STDERR_FILENO, "nc: %s port %s: %s\n", args[0], args[1], strerror(errno));
        return 1;
    }
    return relay(sock, udp, &peer, true);
}
