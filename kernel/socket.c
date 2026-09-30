/*
 * socket.c - BSD sockets: the socket layer, UDP, raw ICMP and ICMPv6
 * sockets and the networking system calls.
 *
 * AF_INET6 sockets are dual-stack unless IPV6_V6ONLY is set: they talk IPv4
 * too, with IPv4 addresses mapped (::ffff:a.b.c.d).  Unbound AF_INET
 * sockets have the local address 0.0.0.0 (mapped), AF_INET6 ones ::.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "net.h"
#include "proc.h"
#include "fs.h"
#include "mm.h"
#include "poll.h"
#include "abi2.h"
#include "sieos/socket.h"
#include "sieos/sysinfo.h"

#define NSOCK 64
#define UDP_QMAX (256 * 1024)            /* queued datagrams per socket (a few at 64 KB) */
#define EPHEMERAL_LO 49152

struct dgram {
    struct dgram *next;
    naddr_t ip;
    uint16_t port;
    size_t len;
    uint8_t data[];
};

struct socket {
    bool used;
    int family;                      /* AF_INET or SIEOS_AF_INET6 */
    int type, proto;
    int uid;
    naddr_t lip, rip;                /* IPv4 ones mapped */
    uint16_t lport, rport;
    bool v6only;                     /* IPV6_V6ONLY */
    bool bound, connected, shut_rd;
    struct dgram *qh, *qt;
    size_t qbytes;
    struct tcb *tcb;
    int rcvtimeo, sndtimeo;          /* ms, 0 = forever */
};

static struct socket socks[NSOCK];
static uint16_t next_ephemeral = EPHEMERAL_LO;

void socket_wake(struct socket *s)
{
    wakeup(s);
    poll_wakeup();
}

struct socket *socket_alloc(int type, int proto)
{
    for (int i = 0; i < NSOCK; i++) {
        if (!socks[i].used) {
            memset(&socks[i], 0, sizeof(socks[i]));
            socks[i].used = true;
            socks[i].type = type;
            socks[i].proto = proto;
            socks[i].uid = current ? current->euid : 0;
            return &socks[i];
        }
    }
    return NULL;
}

static bool udp_port_in_use(uint16_t port)
{
    for (int i = 0; i < NSOCK; i++)
        if (socks[i].used && socks[i].type == SOCK_DGRAM && socks[i].bound && socks[i].lport == port)
            return true;
    return false;
}

/* May s bind port?  Another socket of its type there conflicts, unless one
 * is AF_INET and the other AF_INET6 with IPV6_V6ONLY, or both are bound to
 * different specific addresses. */
static bool port_conflict(struct socket *s, uint16_t port, const naddr_t *ip)
{
    for (int i = 0; i < NSOCK; i++) {
        struct socket *o = &socks[i];
        if (o == s || !o->used || o->type != s->type || !o->bound || o->lport != port)
            continue;
        bool v4a = s->family == AF_INET, v4b = o->family == AF_INET;
        if ((v4a && !v4b && o->v6only) || (v4b && !v4a && s->v6only))
            continue;
        if (!na_any(ip) && !na_any(&o->lip) && !na_eq(ip, &o->lip))
            continue;
        return true;
    }
    return false;
}

static uint16_t ephemeral_port(bool tcp)
{
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = next_ephemeral++;
        if (next_ephemeral < EPHEMERAL_LO)
            next_ephemeral = EPHEMERAL_LO;
        if (tcp ? !tcp_port_in_use(p) : !udp_port_in_use(p))
            return p;
    }
    return 0;
}

static void free_queue(struct socket *s)
{
    while (s->qh) {
        struct dgram *d = s->qh;
        s->qh = d->next;
        kfree(d);
    }
    s->qt = NULL;
    s->qbytes = 0;
}

void socket_close(struct socket *s)
{
    if (s->tcb) {
        tcp_set_owner(s->tcb, NULL);
        tcp_close(s->tcb);
        s->tcb = NULL;
    }
    free_queue(s);
    s->used = false;
}

/* ------------------------------------------------------------------ */
/* UDP and raw ICMP input                                              */
/* ------------------------------------------------------------------ */

static void enqueue(struct socket *s, const naddr_t *ip, uint16_t port, const uint8_t *data, size_t len)
{
    if (s->qbytes + len > UDP_QMAX) {
        net_count_drop();
        return;
    }
    struct dgram *d = kmalloc(sizeof(*d) + len);
    if (!d)
        return;
    d->next = NULL;
    d->ip = *ip;
    d->port = port;
    d->len = len;
    memcpy(d->data, data, len);
    if (s->qt)
        s->qt->next = d;
    else
        s->qh = d;
    s->qt = d;
    s->qbytes += len;
    socket_wake(s);
}

/* Does socket s take traffic of dst's IP version? */
static bool family_ok(struct socket *s, const naddr_t *a)
{
    if (s->family == AF_INET)
        return na_is_v4(a);
    return !(s->v6only && na_is_v4(a));
}

void udp_input(const naddr_t *src, const naddr_t *dst, const uint8_t *seg, size_t len)
{
    if (len < 8)
        return;
    uint16_t sport = (seg[0] << 8) | seg[1], dport = (seg[2] << 8) | seg[3];
    size_t ulen = (seg[4] << 8) | seg[5];
    if (ulen < 8 || ulen > len)
        return;
    bool v4 = na_is_v4(dst);
    if (!v4 && !(seg[6] | seg[7]))
        return;                                  /* the checksum is mandatory over IPv6 */
    if ((seg[6] | seg[7]) && csum_fold(csum_add(net_pseudo_sum(src, dst, IPPROTO_UDP, ulen), seg, ulen)) != 0)
        return;
    if (dport == 68 && v4) {
        dhcp_input(seg + 8, ulen - 8);
        return;
    }
    bool bcast = v4 && na_to_v4(dst) == INADDR_BROADCAST;
    for (int i = 0; i < NSOCK; i++) {
        struct socket *s = &socks[i];
        if (!s->used || s->type != SOCK_DGRAM || !s->bound || s->lport != dport || !family_ok(s, dst))
            continue;
        if (!na_any(&s->lip) && !na_eq(&s->lip, dst) && !bcast)
            continue;
        if (s->connected && (!na_eq(&s->rip, src) || s->rport != sport))
            continue;
        enqueue(s, src, sport, seg + 8, ulen - 8);
        return;
    }
}

void icmp_deliver_raw(const naddr_t *src, int proto, const uint8_t *msg, size_t len)
{
    for (int i = 0; i < NSOCK; i++) {
        struct socket *s = &socks[i];
        if (s->used && s->type == SOCK_RAW && s->proto == proto)
            enqueue(s, src, 0, msg, len);
    }
}

static int raw_send(struct socket *s, const naddr_t *dst, const void *buf, size_t n)
{
    int r;
    if (s->proto == IPPROTO_ICMP)
        r = ip_send(0, na_to_v4(dst), IPPROTO_ICMP, buf, n);
    else
        r = icmp6_send(dst, buf, n);
    return r < 0 ? (r == -EMSGSIZE ? r : -EHOSTUNREACH) : (int)n;
}

static int udp_send(struct socket *s, const naddr_t *dst, uint16_t dport, const void *data, size_t len)
{
    if (len > (na_is_v4(dst) ? 65535 - 20 - 8 : 65535 - 8))
        return -EMSGSIZE;                        /* (larger than the MTU: IP fragments it) */
    if (!family_ok(s, dst) || (!na_any(&s->lip) && na_is_v4(&s->lip) != na_is_v4(dst)))
        return -ENETUNREACH;                     /* (bound to an address of the other version) */
    naddr_t src = na_any(&s->lip) ? net_source(dst) : s->lip;
    if (na_any(&src) && !na_is_v4(dst))
        return -ENETUNREACH;                     /* no IPv6 address yet */
    if (!s->bound) {
        s->lport = ephemeral_port(false);
        s->bound = true;
    }
    uint8_t small[ETH_MTU];
    size_t ulen = 8 + len;
    uint8_t *seg = ulen <= sizeof(small) ? small : kmalloc(ulen);
    if (!seg)
        return -ENOBUFS;
    seg[0] = s->lport >> 8;
    seg[1] = s->lport & 0xFF;
    seg[2] = dport >> 8;
    seg[3] = dport & 0xFF;
    seg[4] = ulen >> 8;
    seg[5] = ulen & 0xFF;
    seg[6] = seg[7] = 0;
    memcpy(seg + 8, data, len);
    uint16_t c = csum_fold(csum_add(net_pseudo_sum(&src, dst, IPPROTO_UDP, ulen), seg, ulen));
    if (c == 0)
        c = 0xFFFF;
    memcpy(seg + 6, &c, 2);
    int r = net_send(&src, dst, IPPROTO_UDP, seg, ulen);
    if (seg != small)
        kfree(seg);
    return r < 0 ? r : (int)len;
}

/* ------------------------------------------------------------------ */
/* Generic socket I/O                                                  */
/* ------------------------------------------------------------------ */

static long dgram_recv(struct socket *s, void *buf, size_t n, bool nonblock, naddr_t *ip, uint16_t *port)
{
    uint64_t deadline = s->rcvtimeo ? ticks + (uint64_t)s->rcvtimeo * TIMER_HZ / 1000 + 1 : 0;
    while (!s->qh) {
        if (nonblock)
            return -EAGAIN;
        if (deadline && ticks >= deadline)
            return -EAGAIN;
        if (signal_pending(current))
            return -EINTR;
        curlwp->wake_tick = deadline;
        sleep_on(s);
        curlwp->wake_tick = 0;
    }
    struct dgram *d = s->qh;
    s->qh = d->next;
    if (!s->qh)
        s->qt = NULL;
    s->qbytes -= d->len;
    size_t c = MIN(n, d->len);
    memcpy(buf, d->data, c);
    if (ip)
        *ip = d->ip;
    if (port)
        *port = d->port;
    kfree(d);
    return c;
}

long socket_read(struct socket *s, void *buf, size_t n)
{
    if (s->type == SOCK_STREAM) {
        if (!s->tcb)
            return -ENOTCONN;
        return tcp_recv(s->tcb, buf, n, false, s->rcvtimeo);
    }
    return dgram_recv(s, buf, n, false, NULL, NULL);
}

long socket_write(struct socket *s, const void *buf, size_t n)
{
    if (s->type == SOCK_STREAM) {
        if (!s->tcb)
            return -ENOTCONN;
        return tcp_send(s->tcb, buf, n, false);
    }
    if (!s->connected)
        return -EDESTADDRREQ;
    if (s->type == SOCK_RAW)
        return raw_send(s, &s->rip, buf, n);
    return udp_send(s, &s->rip, s->rport, buf, n);
}

bool socket_readable(struct socket *s)
{
    if (s->type == SOCK_STREAM)
        return s->tcb && tcp_readable(s->tcb);
    return s->qh != NULL;
}

bool socket_writable(struct socket *s)
{
    if (s->type == SOCK_STREAM)
        return s->tcb && (tcp_writable(s->tcb) || socket_failed(s));
    return true;
}

/* A stream socket with an error pending (a connect that failed): POLLERR */
bool socket_failed(struct socket *s)
{
    return s->type == SOCK_STREAM && s->tcb && tcp_error(s->tcb);
}

/* ------------------------------------------------------------------ */
/* System calls                                                        */
/* ------------------------------------------------------------------ */

static bool uok(const void *p, size_t n, bool w)
{
    return user_range_ok(current->pml4, (uint64_t)p, n ? n : 1, w);
}

static struct socket *sockfd(int fd, struct file **fp)
{
    if (fd < 0 || fd >= NOFILE || !current->ofile[fd])
        return NULL;
    struct file *f = current->ofile[fd];
    if (f->type != FD_SOCKET)
        return NULL;
    if (fp)
        *fp = f;
    return f->sock;
}

/* sockaddr_in6 without __sin6_src_id: the least an address passed in needs */
#define SIN6_MIN 28

/* A socket address from the user, of the socket's family. */
static int get_addr(struct socket *s, const void *ua, size_t len, naddr_t *ip, uint16_t *port)
{
    if (!ua || len < sizeof(struct sockaddr_in) || !uok(ua, sizeof(struct sockaddr_in), false))
        return -EFAULT;
    int family = *(const uint16_t *)ua;
    if (s->family == AF_INET) {
        const struct sockaddr_in *a = ua;
        if (family != AF_INET)
            return -EAFNOSUPPORT;
        *ip = na_v4(ntohl(a->sin_addr));
        *port = ntohs(a->sin_port);
        return 0;
    }
    const struct sieos_sockaddr_in6 *a = ua;
    if (family != SIEOS_AF_INET6)
        return -EAFNOSUPPORT;
    if (len < SIN6_MIN || !uok(ua, SIN6_MIN, false))
        return -EINVAL;
    memcpy(ip, &a->sin6_addr, 16);
    *port = ntohs(a->sin6_port);
    return 0;
}

static int put_addr(int family, void *ua, unsigned int *ulen, const naddr_t *ip, uint16_t port)
{
    if (!ua)
        return 0;
    if (!ulen || !uok(ulen, sizeof(*ulen), true))
        return -EFAULT;
    if (family == AF_INET) {
        struct sockaddr_in a;
        if (*ulen < sizeof(a) || !uok(ua, sizeof(a), true))
            return -EFAULT;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr = htonl(na_to_v4(ip));
        memcpy(ua, &a, sizeof(a));
        *ulen = sizeof(a);
        return 0;
    }
    struct sieos_sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = SIEOS_AF_INET6;
    a.sin6_port = htons(port);
    memcpy(&a.sin6_addr, ip, 16);
    if (na_linklocal(ip))
        a.sin6_scope_id = 2;                     /* eth0 (lo is 1) */
    size_t n = MIN(*ulen, sizeof(a));            /* truncated if the buffer is short */
    if (!uok(ua, n, true))
        return -EFAULT;
    memcpy(ua, &a, n);
    *ulen = sizeof(a);
    return 0;
}

static int fd_install(struct socket *s)
{
    struct file *f = file_alloc();
    if (!f)
        return -ENFILE;
    f->type = FD_SOCKET;
    f->flags = O_RDWR;
    f->sock = s;
    int fd = fsys_fdalloc(f, 0);
    if (fd >= 0)
        return fd;
    f->sock = NULL;
    file_close(f);
    return -EMFILE;
}

static long sys_socket(int domain, int type, int proto)
{
    if (domain != AF_INET && domain != SIEOS_AF_INET6)
        return -EAFNOSUPPORT;
    int icmp = domain == AF_INET ? IPPROTO_ICMP : IPPROTO_ICMPV6_K;
    if (type == SOCK_STREAM && (proto == 0 || proto == IPPROTO_TCP))
        proto = IPPROTO_TCP;
    else if (type == SOCK_DGRAM && (proto == 0 || proto == IPPROTO_UDP))
        proto = IPPROTO_UDP;
    else if (type == SOCK_RAW && proto == icmp) {
        if (current->euid != 0)
            return -EPERM;                   /* raw sockets are privileged */
    } else
        return -EPROTONOSUPPORT;
    struct socket *s = socket_alloc(type, proto);
    if (!s)
        return -ENFILE;
    s->family = domain;
    if (domain == AF_INET)
        s->lip = na_v4(0);
    int fd = fd_install(s);
    if (fd < 0)
        socket_close(s);
    return fd;
}

static long sys_bind(int fd, const void *ua, unsigned int len)
{
    struct socket *s = sockfd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    naddr_t ip;
    uint16_t port;
    int r = get_addr(s, ua, len, &ip, &port);
    if (r < 0)
        return r;
    if (s->bound)
        return -EINVAL;
    bool bcast = na_is_v4(&ip) && na_to_v4(&ip) == INADDR_BROADCAST;
    if (!na_any(&ip) && !net_is_local(&ip) && !bcast)
        return -EADDRNOTAVAIL;
    if (s->v6only && na_is_v4(&ip))
        return -EADDRNOTAVAIL;
    if (port && port < 1024 && current->euid != 0)
        return -EACCES;                      /* privileged port */
    if (s->type == SOCK_DGRAM) {
        if (!port)
            port = ephemeral_port(false);
        else if (port_conflict(s, port, &ip))
            return -EADDRINUSE;
    } else if (s->type == SOCK_STREAM) {
        if (!port)
            port = ephemeral_port(true);
        else if (port_conflict(s, port, &ip))
            return -EADDRINUSE;
    }
    s->lip = ip;
    s->lport = port;
    s->bound = true;
    return 0;
}

static long sys_listen(int fd, int backlog)
{
    (void)backlog;
    struct socket *s = sockfd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    if (s->tcb)
        return tcp_state(s->tcb) == TCP_LISTEN ? 0 : -EISCONN;
    if (!s->bound) {
        s->lport = ephemeral_port(true);
        s->bound = true;
    }
    s->tcb = tcp_alloc();
    if (!s->tcb)
        return -ENOBUFS;
    tcp_set_owner(s->tcb, s);
    return tcp_listen(s->tcb, &s->lip, s->lport, s->v6only);
}

static long sys_accept(int fd, void *ua, unsigned int *ulen)
{
    struct socket *s = sockfd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    if (!s->tcb || tcp_state(s->tcb) != TCP_LISTEN)
        return -EINVAL;
    struct tcb *c;
    while (!(c = tcp_accept_ready(s->tcb))) {
        if (current->ofile[fd]->flags & O_NONBLOCK_K)
            return -EAGAIN;
        if (signal_pending(current))
            return -ERESTART;
        sleep_on(s);
    }
    struct socket *ns = socket_alloc(SOCK_STREAM, IPPROTO_TCP);
    if (!ns) {
        tcp_abort(c);
        return -ENFILE;
    }
    ns->family = s->family;
    ns->tcb = c;
    tcp_set_owner(c, ns);
    tcp_endpoints(c, &ns->lip, &ns->lport, &ns->rip, &ns->rport);
    ns->bound = ns->connected = true;
    int nfd = fd_install(ns);
    if (nfd < 0) {
        socket_close(ns);
        return nfd;
    }
    put_addr(ns->family, ua, ulen, &ns->rip, ns->rport);
    return nfd;
}

static long sys_connect(int fd, const void *ua, unsigned int len)
{
    struct socket *s = sockfd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    naddr_t ip;
    uint16_t port;
    int r = get_addr(s, ua, len, &ip, &port);
    if (r < 0)
        return r;
    if (!family_ok(s, &ip) || (!na_any(&s->lip) && na_is_v4(&s->lip) != na_is_v4(&ip)))
        return -ENETUNREACH;
    if (s->type != SOCK_STREAM) {            /* datagram: remember the peer, choose our address */
        if (na_any(&s->lip)) {
            naddr_t src = net_source(&ip);
            if (!na_is_v4(&ip) && na_zero(&src))
                return -ENETUNREACH;             /* no IPv6 address yet */
            if (!na_any(&src))
                s->lip = src;                    /* (getsockname shows it, as RFC 6724 sorting needs) */
        }
        s->rip = ip;
        s->rport = port;
        s->connected = true;
        if (s->type == SOCK_DGRAM && !s->bound) {
            s->lport = ephemeral_port(false);
            s->bound = true;
        }
        return 0;
    }
    if (s->tcb) {                                /* again, after a non-blocking one */
        if (tcp_connecting(s->tcb))
            return -EALREADY;
        int e = tcp_take_error(s->tcb);
        return e ? -e : -EISCONN;
    }
    naddr_t lip = na_any(&s->lip) ? net_source(&ip) : s->lip;
    if (!net_reachable(&ip) || (!na_is_v4(&ip) && na_zero(&lip)))
        return -ENETUNREACH;
    if (!s->bound) {
        s->lport = ephemeral_port(true);
        s->bound = true;
    }
    s->tcb = tcp_alloc();
    if (!s->tcb)
        return -ENOBUFS;
    tcp_set_owner(s->tcb, s);
    r = tcp_connect(s->tcb, &lip, s->lport, &ip, port, current->ofile[fd]->flags & O_NONBLOCK_K);
    if (r < 0 && r != -EINPROGRESS) {
        tcp_set_owner(s->tcb, NULL);
        tcp_abort(s->tcb);
        s->tcb = NULL;
        return r;
    }
    s->lip = lip;
    s->rip = ip;
    s->rport = port;
    s->connected = true;
    return r;
}

static long sys_sendto(int fd, const void *buf, size_t n, int flags, const void *ua, unsigned int alen)
{
    struct file *f;
    struct socket *s = sockfd(fd, &f);
    if (!s)
        return -ENOTSOCK;
    if (!uok(buf, n, false))
        return -EFAULT;
    if (f->flags & O_NONBLOCK_K)
        flags |= MSG_DONTWAIT;
    if (s->type == SOCK_STREAM || !ua)
        return s->type == SOCK_STREAM && s->tcb ? tcp_send(s->tcb, buf, n, flags & MSG_DONTWAIT)
                                                : socket_write(s, buf, n);
    naddr_t ip;
    uint16_t port;
    int r = get_addr(s, ua, alen, &ip, &port);
    if (r < 0)
        return r;
    if (s->type == SOCK_RAW)
        return raw_send(s, &ip, buf, n);
    if (na_is_v4(&ip) && na_to_v4(&ip) == INADDR_BROADCAST && current->euid != 0)
        return -EACCES;
    return udp_send(s, &ip, port, buf, n);
}

static long sys_recvfrom(int fd, void *buf, size_t n, int flags, void *ua, unsigned int *alen)
{
    struct file *f;
    struct socket *s = sockfd(fd, &f);
    if (!s)
        return -ENOTSOCK;
    if (!uok(buf, n, true))
        return -EFAULT;
    bool nb = (flags & MSG_DONTWAIT) || (f->flags & O_NONBLOCK_K);
    if (s->type == SOCK_STREAM) {
        if (!s->tcb)
            return -ENOTCONN;
        long r = tcp_recv(s->tcb, buf, n, nb, s->rcvtimeo);
        if (r >= 0 && ua)
            put_addr(s->family, ua, alen, &s->rip, s->rport);
        return r;
    }
    naddr_t ip = { { 0 } };
    uint16_t port = 0;
    long r = dgram_recv(s, buf, n, nb, &ip, &port);
    if (r >= 0 && ua) {
        int e = put_addr(s->family, ua, alen, &ip, port);
        if (e < 0)
            return e;
    }
    return r;
}

static long sys_shutdown(int fd, int how)
{
    struct socket *s = sockfd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    if (how == SHUT_RD || how == SHUT_RDWR)
        s->shut_rd = true;
    if ((how == SHUT_WR || how == SHUT_RDWR) && s->tcb)
        tcp_shutdown_write(s->tcb);
    return 0;
}

static long sys_getname(int fd, void *ua, unsigned int *alen, bool peer)
{
    struct socket *s = sockfd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    if (peer && !s->connected)
        return -ENOTCONN;
    return put_addr(s->family, ua, alen, peer ? &s->rip : &s->lip, peer ? s->rport : s->lport);
}

static long sys_setsockopt(int fd, int level, int opt, const void *val, unsigned int len)
{
    struct socket *s = sockfd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    if (level != SOL_SOCKET)
        return -EINVAL;
    if (opt == SO_REUSEADDR)
        return 0;
    if (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO) {
        if (len < sizeof(int) || !uok(val, sizeof(int), false))
            return -EFAULT;
        int ms = *(const int *)val;
        if (opt == SO_RCVTIMEO)
            s->rcvtimeo = ms < 0 ? 0 : ms;
        else
            s->sndtimeo = ms < 0 ? 0 : ms;
        return 0;
    }
    return -EINVAL;
}

/* Socket state for the ABI v2 option calls: which = 0 type, 1 rcvtimeo, 2 sndtimeo (ms), 3 listening,
 * 4 IPV6_V6ONLY (AF_INET6 sockets only), 5 the family. */
long socket_kopt(int fd, int which, bool set, int *val)
{
    if (fd < 0 || fd >= NOFILE)
        return -EBADF;
    struct socket *s = sockfd(fd, NULL);
    if (!s)
        return current->ofile[fd] ? -ENOTSOCK : -EBADF;
    switch (which) {
    case 0:
        *val = s->type;
        return 0;
    case 1:
    case 2: {
        int *slot = which == 1 ? &s->rcvtimeo : &s->sndtimeo;
        if (set)
            *slot = *val < 0 ? 0 : *val;
        else
            *val = *slot;
        return 0;
    }
    case 3:
        *val = s->tcb && tcp_state(s->tcb) == TCP_LISTEN;
        return 0;
    case 4:
        if (s->family != SIEOS_AF_INET6)
            return -ENOPROTOOPT_K;
        if (!set) {
            *val = s->v6only;
            return 0;
        }
        if (s->bound)
            return -EINVAL;
        s->v6only = *val != 0;
        return 0;
    case 5:
        *val = s->family;
        return 0;
    case 6:                                      /* SO_ERROR (ABI v1 errno), cleared */
        *val = s->type == SOCK_STREAM && s->tcb ? tcp_take_error(s->tcb) : 0;
        return 0;
    }
    return -EINVAL;
}

/* Interface idx (0 eth0, 1 eth1, ...); -ENODEV past the last. */
static long sys_netinfo(struct netinfo *ni, long idx)
{
    if (!uok(ni, sizeof(*ni), true))
        return -EFAULT;
    memset(ni, 0, sizeof(*ni));
    struct netif *ifp = netif_by_index((int)idx);
    if (!ifp) {
        strcpy(ni->name, "lo");
        strlcpy(ni->driver, "none", sizeof(ni->driver));
        return -ENODEV;
    }
    strlcpy(ni->name, ifp->name, sizeof(ni->name));
    ni->up = ifp->up;
    ni->dhcp = ifp->dhcp;
    memcpy(ni->mac, ifp->mac, 6);
    ni->ip = ifp->ip;
    ni->netmask = ifp->netmask;
    ni->gateway = ifp->gateway;
    ni->dns = ifp->dns;
    ni->rx_packets = ifp->rx_packets;
    ni->tx_packets = ifp->tx_packets;
    ni->rx_bytes = ifp->rx_bytes;
    ni->tx_bytes = ifp->tx_bytes;
    ni->rx_dropped = ifp->rx_dropped;
    strlcpy(ni->driver, ifp->nic ? ifp->nic->name : "none", sizeof(ni->driver));
    return 0;
}

/* All sockets, IPv4 and IPv6, into out (kernel memory). */
static int collect(struct sieos_sockinfo6 *out, int max)
{
    int n = 0;
    for (int i = 0; i < NSOCK && n < max; i++) {
        struct socket *s = &socks[i];
        if (!s->used || s->type == SOCK_STREAM)
            continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].family = s->family;
        out[n].proto = s->proto;
        memcpy(out[n].laddr, &s->lip, 16);
        out[n].lport = s->lport;
        memcpy(out[n].raddr, &s->rip, 16);
        out[n].rport = s->rport;
        out[n].rxq = s->qbytes;
        out[n].uid = s->uid;
        n++;
    }
    return n + tcp_info(out + n, max - n);
}

#define NSOCKINFO (NSOCK + 64)                   /* sockets and TCP connections */

long socket_netstat6(struct sieos_sockinfo6 *uout, int max)
{
    if (max < 0 || !uok(uout, max * sizeof(*uout), true))
        return -EFAULT;
    struct sieos_sockinfo6 *k = kmalloc(NSOCKINFO * sizeof(*k));
    if (!k)
        return -ENOMEM;
    int n = collect(k, MIN(max, NSOCKINFO));
    memcpy(uout, k, n * sizeof(*k));
    kfree(k);
    return n;
}

/* The IPv4 endpoints only, in the older format. */
static long sys_netstat(struct sockinfo *out, int max)
{
    if (max < 0 || !uok(out, max * sizeof(*out), true))
        return -EFAULT;
    struct sieos_sockinfo6 *k = kmalloc(NSOCKINFO * sizeof(*k));
    if (!k)
        return -ENOMEM;
    int all = collect(k, NSOCKINFO), n = 0;
    for (int i = 0; i < all && n < max; i++) {
        naddr_t l, r;
        memcpy(&l, k[i].laddr, 16);
        memcpy(&r, k[i].raddr, 16);
        if (!na_is_v4(&l) && !na_zero(&l))
            continue;
        if (!na_is_v4(&r) && !na_zero(&r))
            continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].proto = k[i].proto;
        out[n].state = k[i].state;
        out[n].lip = na_is_v4(&l) ? na_to_v4(&l) : 0;
        out[n].rip = na_is_v4(&r) ? na_to_v4(&r) : 0;
        out[n].lport = k[i].lport;
        out[n].rport = k[i].rport;
        out[n].rxq = k[i].rxq;
        out[n].txq = k[i].txq;
        out[n].uid = k[i].uid;
        n++;
    }
    kfree(k);
    return n;
}

/* IPv6 on interface idx; ::1 (lo0) is listed with eth0. */
long socket_netinfo6(struct sieos_netinfo6 *u, long idx)
{
    if (!uok(u, sizeof(*u), true))
        return -EFAULT;
    struct sieos_netinfo6 ni;
    memset(&ni, 0, sizeof(ni));
    struct netif *ifp = netif_by_index((int)idx);
    if (idx == 0) {
        naddr_t lo = { { 0 } };
        lo.b[15] = 1;
        memcpy(ni.addr[0].addr, &lo, 16);        /* lo0 */
        ni.addr[0].prefixlen = 128;
        ni.naddr = 1;
    }
    if (ifp) {
        const struct net6_state *v = &ifp->v6;
        ni.up = v->up;
        for (int i = 0; i < NET6_ADDRS && v->up; i++) {
            if (na_zero(&v->addr[i]))
                continue;
            memcpy(ni.addr[ni.naddr].addr, &v->addr[i], 16);
            ni.addr[ni.naddr].prefixlen = v->plen[i];
            ni.addr[ni.naddr].flags = i == 0 ? SIEOS_NET6_LINKLOCAL : SIEOS_NET6_AUTOCONF;
            ni.naddr++;
        }
        memcpy(ni.router, &v->router, 16);
        memcpy(ni.dns, &v->dns, 16);
        ni.mtu = v->mtu;
        ni.hoplimit = v->hoplimit;
        ni.rx_packets = v->rx_packets;
        ni.tx_packets = v->tx_packets;
    }
    memcpy(u, &ni, sizeof(ni));
    return ifp ? 0 : -ENODEV;
}

long net_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    switch (nr) {
    case SYS_socket:      return sys_socket(a1, a2, a3);
    case SYS_bind:        return sys_bind(a1, (const void *)a2, a3);
    case SYS_listen:      return sys_listen(a1, a2);
    case SYS_accept:      return sys_accept(a1, (void *)a2, (unsigned int *)a3);
    case SYS_connect:     return sys_connect(a1, (const void *)a2, a3);
    case SYS_sendto:      return sys_sendto(a1, (const void *)a2, a3, a4, (const void *)a5, a6);
    case SYS_recvfrom:    return sys_recvfrom(a1, (void *)a2, a3, a4, (void *)a5, (unsigned int *)a6);
    case SYS_shutdown:    return sys_shutdown(a1, a2);
    case SYS_getsockname: return sys_getname(a1, (void *)a2, (unsigned int *)a3, false);
    case SYS_getpeername: return sys_getname(a1, (void *)a2, (unsigned int *)a3, true);
    case SYS_setsockopt:  return sys_setsockopt(a1, a2, a3, (const void *)a4, a5);
    case SYS_netinfo:     return sys_netinfo((struct netinfo *)a1, (long)a2);
    case SYS_netstat:     return sys_netstat((struct sockinfo *)a1, a2);
    }
    return -ENOSYS;
}
