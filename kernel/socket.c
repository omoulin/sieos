/*
 * socket.c - BSD sockets: the socket layer, UDP, raw ICMP sockets and the
 * networking system calls.
 */
#include "net.h"
#include "proc.h"
#include "fs.h"
#include "mm.h"
#include "poll.h"

#define NSOCK 64
#define UDP_QMAX (64 * 1024)
#define EPHEMERAL_LO 49152

struct dgram {
    struct dgram *next;
    uint32_t ip;
    uint16_t port;
    size_t len;
    uint8_t data[];
};

struct socket {
    bool used;
    int type, proto;
    int uid;
    uint32_t lip, rip;
    uint16_t lport, rport;
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

static void enqueue(struct socket *s, uint32_t ip, uint16_t port, const uint8_t *data, size_t len)
{
    if (s->qbytes + len > UDP_QMAX) {
        netif.rx_dropped++;
        return;
    }
    struct dgram *d = kmalloc(sizeof(*d) + len);
    if (!d)
        return;
    d->next = NULL;
    d->ip = ip;
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

void udp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len)
{
    if (len < 8)
        return;
    uint16_t sport = (seg[0] << 8) | seg[1], dport = (seg[2] << 8) | seg[3];
    size_t ulen = (seg[4] << 8) | seg[5];
    if (ulen < 8 || ulen > len)
        return;
    if ((seg[6] | seg[7]) && csum_fold(csum_add(pseudo_sum(src, dst, IPPROTO_UDP, ulen), seg, ulen)) != 0)
        return;
    if (dport == 68) {
        dhcp_input(seg + 8, ulen - 8);
        return;
    }
    for (int i = 0; i < NSOCK; i++) {
        struct socket *s = &socks[i];
        if (!s->used || s->type != SOCK_DGRAM || !s->bound || s->lport != dport)
            continue;
        if (s->lip && s->lip != dst && dst != INADDR_BROADCAST)
            continue;
        if (s->connected && (s->rip != src || s->rport != sport))
            continue;
        enqueue(s, src, sport, seg + 8, ulen - 8);
        return;
    }
}

void icmp_deliver_raw(uint32_t src, const uint8_t *msg, size_t len)
{
    for (int i = 0; i < NSOCK; i++) {
        struct socket *s = &socks[i];
        if (s->used && s->type == SOCK_RAW && s->proto == IPPROTO_ICMP)
            enqueue(s, src, 0, msg, len);
    }
}

static int udp_send(struct socket *s, uint32_t dst, uint16_t dport, const void *data, size_t len)
{
    if (len > ETH_MTU - IP_HLEN - 8)
        return -EMSGSIZE;
    if (!s->bound) {
        s->lport = ephemeral_port(false);
        s->bound = true;
    }
    uint8_t seg[ETH_MTU];
    size_t ulen = 8 + len;
    uint32_t src = s->lip ? s->lip : (ip_is_local(dst) ? (dst >> 24 == 127 ? dst : netif.ip) : netif.ip);
    seg[0] = s->lport >> 8;
    seg[1] = s->lport & 0xFF;
    seg[2] = dport >> 8;
    seg[3] = dport & 0xFF;
    seg[4] = ulen >> 8;
    seg[5] = ulen & 0xFF;
    seg[6] = seg[7] = 0;
    memcpy(seg + 8, data, len);
    uint16_t c = csum_fold(csum_add(pseudo_sum(src, dst, IPPROTO_UDP, ulen), seg, ulen));
    if (c == 0)
        c = 0xFFFF;
    memcpy(seg + 6, &c, 2);
    int r = ip_send(src, dst, IPPROTO_UDP, seg, ulen);
    return r < 0 ? r : (int)len;
}

/* ------------------------------------------------------------------ */
/* Generic socket I/O                                                  */
/* ------------------------------------------------------------------ */

static long dgram_recv(struct socket *s, void *buf, size_t n, bool nonblock, uint32_t *ip, uint16_t *port)
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
        return ip_send(0, s->rip, IPPROTO_ICMP, buf, n) < 0 ? -EHOSTUNREACH : (long)n;
    return udp_send(s, s->rip, s->rport, buf, n);
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
        return s->tcb && tcp_writable(s->tcb);
    return true;
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

static int get_addr(const struct sockaddr_in *ua, size_t len, uint32_t *ip, uint16_t *port)
{
    if (!ua || len < sizeof(struct sockaddr_in) || !uok(ua, sizeof(*ua), false))
        return -EFAULT;
    if (ua->sin_family != AF_INET)
        return -EAFNOSUPPORT;
    *ip = ntohl(ua->sin_addr);
    *port = ntohs(ua->sin_port);
    return 0;
}

static int put_addr(struct sockaddr_in *ua, unsigned int *ulen, uint32_t ip, uint16_t port)
{
    if (!ua)
        return 0;
    if (!ulen || !uok(ulen, sizeof(*ulen), true) || *ulen < sizeof(struct sockaddr_in) ||
        !uok(ua, sizeof(*ua), true))
        return -EFAULT;
    memset(ua, 0, sizeof(*ua));
    ua->sin_family = AF_INET;
    ua->sin_port = htons(port);
    ua->sin_addr = htonl(ip);
    *ulen = sizeof(*ua);
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
    if (domain != AF_INET)
        return -EAFNOSUPPORT;
    if (type == SOCK_STREAM && (proto == 0 || proto == IPPROTO_TCP))
        proto = IPPROTO_TCP;
    else if (type == SOCK_DGRAM && (proto == 0 || proto == IPPROTO_UDP))
        proto = IPPROTO_UDP;
    else if (type == SOCK_RAW && proto == IPPROTO_ICMP) {
        if (current->euid != 0)
            return -EPERM;                   /* raw sockets are privileged */
    } else
        return -EPROTONOSUPPORT;
    struct socket *s = socket_alloc(type, proto);
    if (!s)
        return -ENFILE;
    int fd = fd_install(s);
    if (fd < 0)
        socket_close(s);
    return fd;
}

static long sys_bind(int fd, const struct sockaddr_in *ua, unsigned int len)
{
    struct socket *s = sockfd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    uint32_t ip;
    uint16_t port;
    int r = get_addr(ua, len, &ip, &port);
    if (r < 0)
        return r;
    if (s->bound)
        return -EINVAL;
    if (ip && !ip_is_local(ip) && ip != INADDR_BROADCAST)
        return -EADDRNOTAVAIL;
    if (port && port < 1024 && current->euid != 0)
        return -EACCES;                      /* privileged port */
    if (s->type == SOCK_DGRAM) {
        if (!port)
            port = ephemeral_port(false);
        else if (udp_port_in_use(port))
            return -EADDRINUSE;
    } else if (s->type == SOCK_STREAM) {
        if (!port)
            port = ephemeral_port(true);
        else if (tcp_port_in_use(port))
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
    return tcp_listen(s->tcb, s->lip, s->lport);
}

static long sys_accept(int fd, struct sockaddr_in *ua, unsigned int *ulen)
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
    ns->tcb = c;
    tcp_set_owner(c, ns);
    tcp_endpoints(c, &ns->lip, &ns->lport, &ns->rip, &ns->rport);
    ns->bound = ns->connected = true;
    int nfd = fd_install(ns);
    if (nfd < 0) {
        socket_close(ns);
        return nfd;
    }
    put_addr(ua, ulen, ns->rip, ns->rport);
    return nfd;
}

static long sys_connect(int fd, const struct sockaddr_in *ua, unsigned int len)
{
    struct socket *s = sockfd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    uint32_t ip;
    uint16_t port;
    int r = get_addr(ua, len, &ip, &port);
    if (r < 0)
        return r;
    if (s->type != SOCK_STREAM) {            /* datagram: just remember the peer */
        s->rip = ip;
        s->rport = port;
        s->connected = true;
        if (s->type == SOCK_DGRAM && !s->bound) {
            s->lport = ephemeral_port(false);
            s->bound = true;
        }
        return 0;
    }
    if (s->tcb)
        return -EISCONN;
    if (!netif.up && !ip_is_local(ip))
        return -ENETUNREACH;
    if (!s->bound) {
        s->lport = ephemeral_port(true);
        s->bound = true;
    }
    s->tcb = tcp_alloc();
    if (!s->tcb)
        return -ENOBUFS;
    tcp_set_owner(s->tcb, s);
    uint32_t lip = s->lip ? s->lip : (ip >> 24 == 127 ? ip : netif.ip);
    r = tcp_connect(s->tcb, lip, s->lport, ip, port);
    if (r < 0) {
        tcp_set_owner(s->tcb, NULL);
        tcp_abort(s->tcb);
        s->tcb = NULL;
        return r;
    }
    s->lip = lip;
    s->rip = ip;
    s->rport = port;
    s->connected = true;
    return 0;
}

static long sys_sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr_in *ua, unsigned int alen)
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
    uint32_t ip;
    uint16_t port;
    int r = get_addr(ua, alen, &ip, &port);
    if (r < 0)
        return r;
    if (s->type == SOCK_RAW)
        return ip_send(0, ip, IPPROTO_ICMP, buf, n) < 0 ? -EHOSTUNREACH : (long)n;
    if (ip == INADDR_BROADCAST && current->euid != 0)
        return -EACCES;
    return udp_send(s, ip, port, buf, n);
}

static long sys_recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr_in *ua, unsigned int *alen)
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
            put_addr(ua, alen, s->rip, s->rport);
        return r;
    }
    uint32_t ip = 0;
    uint16_t port = 0;
    long r = dgram_recv(s, buf, n, nb, &ip, &port);
    if (r >= 0 && ua) {
        int e = put_addr(ua, alen, ip, port);
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

static long sys_getname(int fd, struct sockaddr_in *ua, unsigned int *alen, bool peer)
{
    struct socket *s = sockfd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    if (peer && !s->connected)
        return -ENOTCONN;
    return put_addr(ua, alen, peer ? s->rip : s->lip, peer ? s->rport : s->lport);
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

/* Socket state for the ABI v2 option calls: which = 0 type, 1 rcvtimeo, 2 sndtimeo (ms), 3 listening. */
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
    }
    return -EINVAL;
}

static long sys_netinfo(struct netinfo *ni)
{
    if (!uok(ni, sizeof(*ni), true))
        return -EFAULT;
    memset(ni, 0, sizeof(*ni));
    strcpy(ni->name, netif.present ? "eth0" : "lo");
    ni->up = netif.up;
    ni->dhcp = netif.dhcp;
    memcpy(ni->mac, netif.mac, 6);
    ni->ip = netif.ip;
    ni->netmask = netif.netmask;
    ni->gateway = netif.gateway;
    ni->dns = netif.dns;
    ni->rx_packets = netif.rx_packets;
    ni->tx_packets = netif.tx_packets;
    ni->rx_bytes = netif.rx_bytes;
    ni->tx_bytes = netif.tx_bytes;
    ni->rx_dropped = netif.rx_dropped;
    strlcpy(ni->driver, netif.nic ? netif.nic->name : "none", sizeof(ni->driver));
    return netif.present ? 0 : -ENODEV;
}

static long sys_netstat(struct sockinfo *out, int max)
{
    if (max < 0 || !uok(out, max * sizeof(*out), true))
        return -EFAULT;
    int n = 0;
    for (int i = 0; i < NSOCK && n < max; i++) {
        struct socket *s = &socks[i];
        if (!s->used || s->type == SOCK_STREAM)
            continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].proto = s->proto;
        out[n].lip = s->lip;
        out[n].lport = s->lport;
        out[n].rip = s->rip;
        out[n].rport = s->rport;
        out[n].rxq = s->qbytes;
        out[n].uid = s->uid;
        n++;
    }
    n += tcp_info(out + n, max - n, 0);
    return n;
}

long net_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    switch (nr) {
    case SYS_socket:      return sys_socket(a1, a2, a3);
    case SYS_bind:        return sys_bind(a1, (const struct sockaddr_in *)a2, a3);
    case SYS_listen:      return sys_listen(a1, a2);
    case SYS_accept:      return sys_accept(a1, (struct sockaddr_in *)a2, (unsigned int *)a3);
    case SYS_connect:     return sys_connect(a1, (const struct sockaddr_in *)a2, a3);
    case SYS_sendto:      return sys_sendto(a1, (const void *)a2, a3, a4, (const struct sockaddr_in *)a5, a6);
    case SYS_recvfrom:    return sys_recvfrom(a1, (void *)a2, a3, a4, (struct sockaddr_in *)a5, (unsigned int *)a6);
    case SYS_shutdown:    return sys_shutdown(a1, a2);
    case SYS_getsockname: return sys_getname(a1, (struct sockaddr_in *)a2, (unsigned int *)a3, false);
    case SYS_getpeername: return sys_getname(a1, (struct sockaddr_in *)a2, (unsigned int *)a3, true);
    case SYS_setsockopt:  return sys_setsockopt(a1, a2, a3, (const void *)a4, a5);
    case SYS_netinfo:     return sys_netinfo((struct netinfo *)a1);
    case SYS_netstat:     return sys_netstat((struct sockinfo *)a1, a2);
    }
    return -ENOSYS;
}
