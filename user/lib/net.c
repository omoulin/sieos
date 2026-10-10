/*
 * net.c - Sockets for programs: thin calls to the network server (port
 * "net", mk/proto.h). Requests that are safe to repeat (resolve, info, ping)
 * are sent again if netd restarts during the call; a connection is not, its
 * state died with the old server: the caller gets -EPIPE and starts again.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "net.h"

static long netport;

static long call(msg_t *m, int repeat)
{
    long e = call_named(&netport, "net", m, repeat);
    return e < 0 ? e : (long)m->w[0];
}

long net_resolve(const char *host, uint32_t *ip, unsigned timeout_ms)
{
    msg_t m = { .w = { NET_RESOLVE, 0, 0, (uint64_t)timeout_ms << 16 }, .sbuf = host, .slen = strlen(host) };
    long r = call(&m, 1);
    if (r >= 0) *ip = (uint32_t)m.w[1];
    return r;
}

static long sock(int type)
{
    msg_t m = { .w = { NET_SOCKET, (uint64_t)type } };
    return call(&m, 0);
}

long net_connect(uint32_t ip, uint16_t port, unsigned timeout_ms)
{
    long s = sock(NET_TCP);
    if (s < 0) return s;
    msg_t m = { .w = { NET_CONNECT, (uint64_t)s, ip, port | (uint64_t)timeout_ms << 16 } };
    long r = call(&m, 0);
    if (r < 0) { net_close(s); return r; }
    return s;
}

long net_listen(uint16_t port, int backlog)
{
    long s = sock(NET_TCP);
    if (s < 0) return s;
    msg_t m = { .w = { NET_LISTEN, (uint64_t)s, port, (uint64_t)backlog } };
    long r = call(&m, 0);
    if (r < 0) { net_close(s); return r; }
    return s;
}

long net_accept(long s, uint32_t *ip, uint16_t *port, unsigned timeout_ms)
{
    msg_t m = { .w = { NET_ACCEPT, (uint64_t)s, 0, (uint64_t)timeout_ms << 16 } };
    long r = call(&m, 0);
    if (r >= 0) { if (ip) *ip = (uint32_t)m.w[1]; if (port) *port = (uint16_t)m.w[2]; }
    return r;
}

long net_send(long s, const void *buf, size_t n)
{
    const char *p = buf;
    size_t done = 0;
    while (done < n) {                           /* NET_MAX at a time; 0 = full, try again */
        size_t c = n - done < NET_MAX ? n - done : NET_MAX;
        msg_t m = { .w = { NET_SEND, (uint64_t)s }, .sbuf = p + done, .slen = c };
        long r = call(&m, 0);
        if (r < 0) return r;
        done += (size_t)r;
    }
    return (long)n;
}

long net_recv(long s, void *buf, size_t n, unsigned timeout_ms)
{
    if (n > NET_MAX) n = NET_MAX;
    msg_t m = { .w = { NET_RECV, (uint64_t)s, n, (uint64_t)timeout_ms << 16 }, .rbuf = buf, .rlen = n };
    long r = call(&m, 0);
    return r < 0 ? r : (long)m.rlen;
}

long net_udp(uint16_t port)
{
    long s = sock(NET_UDP);
    if (s < 0) return s;
    msg_t m = { .w = { NET_BIND, (uint64_t)s, port } };
    long r = call(&m, 0);
    if (r < 0) { net_close(s); return r; }
    return s;
}

long net_sendto(long s, uint32_t ip, uint16_t port, const void *buf, size_t n)
{
    msg_t m = { .w = { NET_SENDTO, (uint64_t)s, ip, port }, .sbuf = buf, .slen = n };
    return call(&m, 0);
}

long net_recvfrom(long s, void *buf, size_t n, uint32_t *ip, uint16_t *port, unsigned timeout_ms)
{
    if (n > NET_MAX) n = NET_MAX;
    msg_t m = { .w = { NET_RECVFROM, (uint64_t)s, n, (uint64_t)timeout_ms << 16 }, .rbuf = buf, .rlen = n };
    long r = call(&m, 0);
    if (r >= 0) { if (ip) *ip = (uint32_t)m.w[1]; if (port) *port = (uint16_t)m.w[2]; r = (long)m.rlen; }
    return r;
}

long net_close(long s)
{
    msg_t m = { .w = { NET_CLOSE, (uint64_t)s } };
    return call(&m, 0);
}

long net_ping(uint32_t ip, unsigned timeout_ms)
{
    msg_t m = { .w = { NET_PING, ip, 0, (uint64_t)timeout_ms << 16 } };
    return call(&m, 1);
}

long net_info(net_info_t *in)
{
    msg_t m = { .w = { NET_INFO }, .rbuf = in, .rlen = sizeof *in };
    return call(&m, 1);
}

const char *net_ntoa(uint32_t ip, char b[16])
{
    char *p = b;
    for (int i = 3; i >= 0; i--) {
        unsigned v = ip >> (8 * i) & 255;
        if (v >= 100) *p++ = (char)('0' + v / 100);
        if (v >= 10) *p++ = (char)('0' + v / 10 % 10);
        *p++ = (char)('0' + v % 10);
        if (i) *p++ = '.';
    }
    *p = 0;
    return b;
}

const char *net_strerror(long e)
{
    switch (-e) {
    case ETIMEDOUT:    return "timed out";
    case ECONNREFUSED: return "connection refused";
    case ECONNRESET:   return "connection reset";
    case ENETUNREACH:  return "no network";
    case EHOSTUNREACH: return "host unreachable";
    case ENOENT:       return "no such host";
    case EPIPE:        return "the network server restarted";
    case EPROTO:       return "protocol error";
    case EACCES:       return "not allowed";
    case EIO:          return "certificate or TLS error";
    default:           return "error";
    }
}
