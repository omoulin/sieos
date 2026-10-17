/*
 * ptest-net HOST PORT (make posix-test): sockets of netd's through the Linux
 * personality, against echo servers on the host (tools/ktest.py starts them;
 * QEMU's network shows the host as 10.0.2.2): getaddrinfo of an address,
 * TCP connect, send, receive; a non-blocking connect completed through poll;
 * UDP sendto, poll, recvfrom; a listening socket reached by ourselves.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails;
static void check(const char *what, int ok)
{
    printf("ptest net: %s: %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: ptest-net host port\n"); return 2; }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = 0;
    int e = getaddrinfo(argv[1], argv[2], &hints, &ai);
    check("getaddrinfo of an address", e == 0 && ai && ai->ai_family == AF_INET);
    if (e) { printf("ptest net: FAIL\n"); return 1; }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    char b[64] = { 0 };
    int ok = s >= 0 && connect(s, ai->ai_addr, ai->ai_addrlen) == 0 && send(s, "echo me", 7, 0) == 7;
    size_t n = 0;
    while (ok && n < 7) { long r = recv(s, b + n, sizeof b - n, 0); if (r <= 0) break; n += r; }
    check("TCP: connect, send, the echo back", ok && n == 7 && !memcmp(b, "echo me", 7));
    close(s);
    /* non-blocking connect, finished through poll */
    s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int r = connect(s, ai->ai_addr, ai->ai_addrlen);
    struct pollfd p = { s, POLLOUT, 0 };
    int pr = poll(&p, 1, 5000), err = -1;
    socklen_t el = sizeof err;
    getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
    check("a non-blocking connect: EINPROGRESS, then writable", r == -1 && errno == EINPROGRESS && pr == 1 && (p.revents & POLLOUT) && err == 0);
    send(s, "again", 5, 0);
    p = (struct pollfd){ s, POLLIN, 0 };
    pr = poll(&p, 1, 5000);
    long got = pr == 1 ? recv(s, b, sizeof b, MSG_DONTWAIT) : -1;
    check("poll for the echo, a non-blocking receive", got == 5 && !memcmp(b, "again", 5));
    close(s);
    /* UDP */
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to;
    memcpy(&to, ai->ai_addr, sizeof to);
    to.sin_port = htons((uint16_t)(atoi(argv[2]) + 1));       /* (the UDP echo: the next port) */
    long sr = sendto(u, "datagram", 8, 0, (struct sockaddr *)&to, sizeof to);
    p = (struct pollfd){ u, POLLIN, 0 };
    pr = poll(&p, 1, 5000);
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    got = pr == 1 ? recvfrom(u, b, sizeof b, 0, (struct sockaddr *)&from, &fl) : -1;
    check("UDP: sendto, poll, recvfrom (and who sent it)", sr == 8 && got == 8 && !memcmp(b, "datagram", 8) && from.sin_port == to.sin_port);
    close(u);
    /* a name: musl's resolver asks the name server netd learnt (DHCP: QEMU's, which asks the host's) */
    struct addrinfo *nai = 0;
    e = getaddrinfo("localhost", "80", &hints, &nai);
    check("getaddrinfo of a name, through DNS", e == 0 && nai && ((struct sockaddr_in *)nai->ai_addr)->sin_addr.s_addr == htonl(0x7f000001));
    if (!e) freeaddrinfo(nai);
    else printf("ptest net: (getaddrinfo: %s)\n", gai_strerror(e));
    freeaddrinfo(ai);
    printf("ptest net: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
