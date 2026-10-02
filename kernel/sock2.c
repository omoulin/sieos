/*
 * sock2.c - ABI v2 sockets: Solaris constants (SOCK_DGRAM = 1,
 * SOCK_STREAM = 2, SOL_SOCKET = 0xffff, MSG_DONTWAIT = 0x80, ...) over the
 * kernel's IPv4 and IPv6 sockets.  sockaddr_in has the same layout in both
 * ABIs; AF_INET6 and sockaddr_in6 are the Solaris ones.
 * AF_UNIX sockets are in unix.c.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "proc.h"
#include "fs.h"
#include "net.h"
#include "abi2.h"
#include "sieos/syscall.h"
#include "sieos/socket.h"
#include "sieos/errno.h"
#include "sieos/sysinfo.h"

static long net(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    return net_syscall(nr, a1, a2, a3, a4, a5, a6);
}

static int type_to_k(int t)
{
    switch (t) {
    case SIEOS_SOCK_DGRAM:  return SOCK_DGRAM;
    case SIEOS_SOCK_STREAM: return SOCK_STREAM;
    case SIEOS_SOCK_RAW:    return SOCK_RAW;
    }
    return -1;
}

static int type_from_k(int t)
{
    return t == SOCK_DGRAM ? SIEOS_SOCK_DGRAM : t == SOCK_STREAM ? SIEOS_SOCK_STREAM : SIEOS_SOCK_RAW;
}

static void fd_flags(long fd, long flags)
{
    if (fd < 0)
        return;
    if (flags & (SIEOS_SOCK_NONBLOCK | SIEOS_SOCK_NDELAY))
        current->ofile[fd]->flags |= O_NONBLOCK_K;
    if (flags & SIEOS_SOCK_CLOEXEC)
        current->fdflags[fd] = FD_CLOEXEC;
}

static long msg_flags(long f, bool *ok)
{
    *ok = !(f & ~(long)(SIEOS_MSG_DONTWAIT | SIEOS_MSG_NOSIGNAL | SIEOS_MSG_WAITALL | SIEOS_MSG_EOR |
                        SIEOS_MSG_OOB | SIEOS_MSG_PEEK));
    return ((f & SIEOS_MSG_DONTWAIT) ? MSG_DONTWAIT : 0) | ((f & SIEOS_MSG_NOSIGNAL) ? MSG_NOSIGNAL : 0) |
           ((f & SIEOS_MSG_OOB) ? MSG_OOB : 0) | ((f & SIEOS_MSG_PEEK) ? MSG_PEEK : 0);
}

static long do_socket(long domain, long type, long proto)
{
    if (domain != SIEOS_AF_INET && domain != SIEOS_AF_INET6 && domain != SIEOS_AF_UNIX)
        return -EAFNOSUPPORT;
    if (type & ~(long)(SIEOS_SOCK_TYPE_MASK | SIEOS_SOCK_CLOEXEC | SIEOS_SOCK_NONBLOCK | SIEOS_SOCK_NDELAY))
        return -EINVAL;
    if (domain == SIEOS_AF_UNIX) {
        if (proto != 0)
            return -EPROTONOSUPPORT;
        long fd = unix_socket(type & SIEOS_SOCK_TYPE_MASK);
        fd_flags(fd, type);
        return fd;
    }
    int kt = type_to_k(type & SIEOS_SOCK_TYPE_MASK);
    if (kt < 0)
        return -EPROTONOSUPPORT;
    long fd = net(SYS_socket, domain, kt, proto, 0, 0, 0);
    fd_flags(fd, type);
    return fd;
}

static long do_recv(long fd, void *buf, size_t n, long flags, void *addr, unsigned int *alen)
{
    bool ok;
    long kf = msg_flags(flags, &ok);
    if (!ok)
        return -EOPNOTSUPP;
    if (!(flags & SIEOS_MSG_WAITALL) || (flags & (SIEOS_MSG_PEEK | SIEOS_MSG_OOB)))
        return net(SYS_recvfrom, fd, (uint64_t)buf, n, kf, (uint64_t)addr, (uint64_t)alen);
    size_t got = 0;                                  /* MSG_WAITALL: until n bytes, EOF or error */
    while (got < n) {
        long r = net(SYS_recvfrom, fd, (uint64_t)buf + got, n - got, kf, (uint64_t)addr, (uint64_t)alen);
        if (r <= 0)
            return got ? (long)got : r;
        got += r;
    }
    return got;
}

static long do_send(long fd, const void *buf, size_t n, long flags, const void *addr, long alen)
{
    bool ok;
    long kf = msg_flags(flags, &ok);
    if (!ok)
        return -EOPNOTSUPP;
    return net(SYS_sendto, fd, (uint64_t)buf, n, kf, (uint64_t)addr, alen);
}

/* sendmsg's ancillary data (IPv6: IPV6_HOPLIMIT, IPV6_TCLASS, for this datagram). */
static long msg_control_in(long fd, const struct sieos_msghdr *m)
{
    if (!m->msg_control || !m->msg_controllen)
        return 0;
    if (!user_ok(m->msg_control, m->msg_controllen, false))
        return -EFAULT;
    const uint8_t *p = m->msg_control, *end = p + m->msg_controllen;
    while (p + sizeof(struct sieos_cmsghdr) <= end) {
        const struct sieos_cmsghdr *c = (const void *)p;
        if (c->cmsg_len < sizeof(*c) || c->cmsg_len > (size_t)(end - p))
            return -EINVAL;
        if (c->cmsg_level != SIEOS_IPPROTO_IPV6 ||
            (c->cmsg_type != SIEOS_IPV6_HOPLIMIT && c->cmsg_type != SIEOS_IPV6_TCLASS) ||
            c->cmsg_len != sizeof(*c) + sizeof(int))
            return -EINVAL;
        int v = *(const int *)(c + 1);
        long r = socket_kopt(fd, c->cmsg_type == SIEOS_IPV6_HOPLIMIT ? 11 : 12, true, &v);
        if (r < 0)
            return r;
        p += (c->cmsg_len + 7) & ~7u;
    }
    return 0;
}

/* One control message out, as Linux's put_cmsg: cut short (MSG_CTRUNC) if it does not fit. */
static void cmsg_put(struct sieos_msghdr *um, unsigned int *used, int level, int type, int value, int *mflags)
{
    unsigned int room = um->msg_controllen - *used, len = sizeof(struct sieos_cmsghdr) + sizeof(int);
    if (room < sizeof(struct sieos_cmsghdr)) {
        *mflags |= SIEOS_MSG_CTRUNC;
        return;
    }
    if (room < len) {
        *mflags |= SIEOS_MSG_CTRUNC;
        len = room;
    }
    struct sieos_cmsghdr c = { len, 0, level, type };
    uint8_t *at = (uint8_t *)um->msg_control + *used;
    memcpy(at, &c, sizeof(c));
    memcpy(at + sizeof(c), &value, len - sizeof(c));
    unsigned int adv = (sizeof(c) + sizeof(int) + 7) & ~7u;
    *used += adv < room ? adv : room;
}

static long do_msg(long fd, struct sieos_msghdr *um, long flags, bool send)
{
    if (!user_ok(um, sizeof(*um), !send))
        return -EFAULT;
    struct sieos_msghdr m = *um;
    if (m.msg_iovlen <= 0 || m.msg_iovlen > 1024 || !user_ok(m.msg_iov, m.msg_iovlen * sizeof(*m.msg_iov), false))
        return -EINVAL;
    int type = 0;
    socket_kopt(fd, 0, false, &type);
    if (type != SOCK_STREAM && m.msg_iovlen > 1)
        return -EOPNOTSUPP;                          /* datagrams: one buffer (kept atomic) */
    if (send) {
        long r = msg_control_in(fd, &m);
        if (r < 0)
            return r;
    }
    long total = 0;
    for (int i = 0; i < m.msg_iovlen; i++) {
        struct sieos_iovec v = m.msg_iov[i];
        if (!v.iov_len)
            continue;
        long r;
        if (send)
            r = do_send(fd, v.iov_base, v.iov_len, flags, i == 0 ? m.msg_name : NULL, m.msg_namelen);
        else
            r = do_recv(fd, v.iov_base, v.iov_len, flags, i == 0 ? m.msg_name : NULL,
                        i == 0 && m.msg_name ? &um->msg_namelen : NULL);
        if (r < 0) {
            total = total ? total : r;
            break;
        }
        total += r;
        if ((size_t)r < v.iov_len)
            break;
    }
    if (send) {
        int none = -1;                               /* (sendmsg's values were for this datagram only) */
        socket_kopt(fd, 11, true, &none);
        socket_kopt(fd, 12, true, &none);
        return total;
    }
    int mflags = 0, opts = 0, fam = 0, hops = -1, tc = -1, trunc = 0;
    unsigned int used = 0;
    socket_kopt(fd, 8, false, &opts);
    if (total >= 0 && type != SOCK_STREAM && socket_kopt(fd, 15, false, &trunc) == 0 && trunc)
        mflags |= SIEOS_MSG_TRUNC;                   /* the datagram was longer than the buffer */
    socket_kopt(fd, 5, false, &fam);
    if (total >= 0 && type == SOCK_DGRAM && fam == SIEOS_AF_INET6 && (opts & (SOPT_RECVHOPLIMIT | SOPT_RECVTCLASS))) {
        socket_kopt(fd, 13, false, &hops);
        socket_kopt(fd, 14, false, &tc);
        bool want_h = (opts & SOPT_RECVHOPLIMIT) && hops >= 0, want_t = (opts & SOPT_RECVTCLASS) && tc >= 0;
        if (!m.msg_control || !m.msg_controllen || !user_ok(m.msg_control, m.msg_controllen, true)) {
            if (want_h || want_t)
                mflags |= SIEOS_MSG_CTRUNC;          /* nowhere to put them */
        } else {
            if (want_h)
                cmsg_put(&m, &used, SIEOS_IPPROTO_IPV6, SIEOS_IPV6_HOPLIMIT, hops, &mflags);
            if (want_t)
                cmsg_put(&m, &used, SIEOS_IPPROTO_IPV6, SIEOS_IPV6_TCLASS, tc, &mflags);
        }
    }
    um->msg_controllen = used;
    um->msg_flags = mflags;
    return total;
}

/*
 * Flag options: kept per socket and reported by getsockopt, as programs check
 * what they set (the stack's behaviour does not change: TCP sends at once).
 */
static int opt_bit(long level, long name)
{
    if (level == SIEOS_IPPROTO_TCP)
        return name == SIEOS_TCP_NODELAY ? 1 << 7 : name == SIEOS_TCP_QUICKACK ? 1 << 10 : 0;
    if (level == SIEOS_IPPROTO_IPV6)
        return name == SIEOS_IPV6_RECVHOPLIMIT ? SOPT_RECVHOPLIMIT : name == SIEOS_IPV6_RECVTCLASS ? SOPT_RECVTCLASS : 0;
    if (level != SIEOS_SOL_SOCKET)
        return 0;
    switch (name) {
    case SIEOS_SO_DEBUG:     return 1 << 0;
    case SIEOS_SO_REUSEADDR: return 1 << 1;
    case SIEOS_SO_KEEPALIVE: return 1 << 2;
    case SIEOS_SO_BROADCAST: return 1 << 3;
    case SIEOS_SO_DONTROUTE: return 1 << 4;
    case SIEOS_SO_OOBINLINE: return SOPT_OOBINLINE;
    case SIEOS_SO_REUSEPORT: return 1 << 6;
    }
    return 0;
}

static long do_setsockopt(long fd, long level, long name, const void *val, long len)
{
    if (fd < 0 || fd >= NOFILE || !current->ofile[fd])
        return -EBADF;
    if (current->ofile[fd]->type != FD_SOCKET)
        return -ENOTSOCK;
    int bit = opt_bit(level, name);
    if (bit) {
        if (len < (long)sizeof(int) || !user_ok(val, sizeof(int), false))
            return -EINVAL;
        int opts = 0;
        socket_kopt(fd, 8, false, &opts);
        opts = *(const int *)val ? opts | bit : opts & ~bit;
        return socket_kopt(fd, 8, true, &opts);
    }
    if (level == SIEOS_IPPROTO_TCP)
        return -ENOPROTOOPT_K;
    if (level == SIEOS_IPPROTO_IP)                   /* (the type of service: ssh, curl and git set it) */
        return name == SIEOS_IP_TOS ? 0 : -ENOPROTOOPT_K;
    if (level == SIEOS_IPPROTO_IPV6) {
        int fam = 0;
        socket_kopt(fd, 5, false, &fam);
        if (fam != SIEOS_AF_INET6)
            return -ENOPROTOOPT_K;
        if (len < (long)sizeof(int) || !user_ok(val, sizeof(int), false))
            return -EINVAL;
        int v = *(const int *)val;
        switch (name) {
        case SIEOS_IPV6_V6ONLY:
            return socket_kopt(fd, 4, true, &v);
        case SIEOS_IPV6_TCLASS:
            return socket_kopt(fd, 10, true, &v);
        case SIEOS_IPV6_UNICAST_HOPS:
            return socket_kopt(fd, 9, true, &v);
        case SIEOS_IPV6_MULTICAST_HOPS:
        case SIEOS_IPV6_MULTICAST_LOOP:
        case SIEOS_IPV6_MULTICAST_IF:
            return 0;                                /* accepted; hop limits are the router's */
        }
        return -ENOPROTOOPT_K;
    }
    if (level != SIEOS_SOL_SOCKET)
        return -ENOPROTOOPT_K;
    switch (name) {
    case SIEOS_SO_RCVTIMEO:
    case SIEOS_SO_SNDTIMEO: {
        const struct sieos_timeval *tv = val;
        if (len < (long)sizeof(*tv) || !user_ok(tv, sizeof(*tv), false))
            return -EINVAL;
        if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000)
            return -EDOM;
        long ms = tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
        int v = ms > 0x7FFFFFFF ? 0x7FFFFFFF : (int)ms;
        return socket_kopt(fd, name == SIEOS_SO_RCVTIMEO ? 1 : 2, true, &v);
    }
    case SIEOS_SO_LINGER:
    case SIEOS_SO_SNDBUF:
    case SIEOS_SO_RCVBUF:
        if (len < (long)sizeof(int) || !user_ok(val, sizeof(int), false))
            return -EINVAL;
        return 0;                                    /* accepted; the stack has fixed behaviour */
    }
    return -ENOPROTOOPT_K;
}

static long do_getsockopt(long fd, long level, long name, void *val, unsigned int *ulen)
{
    if (fd < 0 || fd >= NOFILE || !current->ofile[fd])
        return -EBADF;
    if (current->ofile[fd]->type != FD_SOCKET)
        return -ENOTSOCK;
    if (!user_ok(ulen, sizeof(*ulen), true))
        return -EFAULT;
    unsigned int len = *ulen;
    int iv = 0, t;
    int bit = opt_bit(level, name);
    if (bit) {
        socket_kopt(fd, 8, false, &iv);
        iv = (iv & bit) != 0;
    } else if (level == SIEOS_IPPROTO_IPV6) {
        int fam = 0;
        socket_kopt(fd, 5, false, &fam);
        if (fam != SIEOS_AF_INET6)
            return -ENOPROTOOPT_K;
        if (name == SIEOS_IPV6_V6ONLY)
            socket_kopt(fd, 4, false, &iv);
        else if (name == SIEOS_IPV6_UNICAST_HOPS || name == SIEOS_IPV6_TCLASS) {
            socket_kopt(fd, name == SIEOS_IPV6_TCLASS ? 10 : 9, false, &iv);
            if (iv < 0)
                iv = name == SIEOS_IPV6_TCLASS ? 0 : 64;   /* the defaults */
        }
        else if (name == SIEOS_IPV6_MULTICAST_HOPS || name == SIEOS_IPV6_MULTICAST_LOOP)
            iv = 1;
        else
            return -ENOPROTOOPT_K;
    } else if (level != SIEOS_SOL_SOCKET) {
        return -ENOPROTOOPT_K;
    } else {
        switch (name) {
        case SIEOS_SO_TYPE:
            socket_kopt(fd, 0, false, &t);
            iv = type_from_k(t);
            break;
        case SIEOS_SO_ERROR:
            socket_kopt(fd, 6, false, &iv);
            iv = iv ? sieos_errno(iv) : 0;
            break;
        case SIEOS_SO_ACCEPTCONN:
            socket_kopt(fd, 3, false, &iv);
            break;
        case SIEOS_SO_DOMAIN:                    /* (Python's socket(fileno=...) asks both) */
            socket_kopt(fd, 5, false, &iv);
            break;
        case SIEOS_SO_PROTOCOL:
            socket_kopt(fd, 7, false, &iv);
            break;
        case SIEOS_SO_SNDBUF:
        case SIEOS_SO_RCVBUF:
            iv = 65536;
            break;
        case SIEOS_SO_LINGER: {
            struct sieos_linger l = { 0, 0 };
            if (len < sizeof(l) || !user_ok(val, sizeof(l), true))
                return -EINVAL;
            memcpy(val, &l, sizeof(l));
            *ulen = sizeof(l);
            return 0;
        }
        case SIEOS_SO_RCVTIMEO:
        case SIEOS_SO_SNDTIMEO: {
            int ms = 0;
            socket_kopt(fd, name == SIEOS_SO_RCVTIMEO ? 1 : 2, false, &ms);
            struct sieos_timeval tv = { ms / 1000, (ms % 1000) * 1000 };
            if (len < sizeof(tv) || !user_ok(val, sizeof(tv), true))
                return -EINVAL;
            memcpy(val, &tv, sizeof(tv));
            *ulen = sizeof(tv);
            return 0;
        }
        default:
            return -ENOPROTOOPT_K;
        }
    }
    if (len < sizeof(int) || !user_ok(val, sizeof(int), true))
        return -EINVAL;
    memcpy(val, &iv, sizeof(int));
    *ulen = sizeof(int);
    return 0;
}

static long sock_call(struct trapframe *tf, bool *handled);

/* A call on a descriptor holds its file while it runs (it may sleep: accept,
 * connect, recv ...): another thread closing the descriptor meanwhile must not
 * free the socket under it. */
long syscall_sock_v2(struct trapframe *tf, bool *handled)
{
    struct file *held = NULL;
    switch (tf->rax) {
    case SIEOS_SYS_bind: case SIEOS_SYS_listen: case SIEOS_SYS_accept: case SIEOS_SYS_connect:
    case SIEOS_SYS_shutdown: case SIEOS_SYS_recvfrom: case SIEOS_SYS_sendto: case SIEOS_SYS_recvmsg:
    case SIEOS_SYS_sendmsg: case SIEOS_SYS_getsockname: case SIEOS_SYS_getpeername:
    case SIEOS_SYS_setsockopt: case SIEOS_SYS_getsockopt:
        if ((long)tf->rdi >= 0 && (long)tf->rdi < NOFILE && current->ofile[tf->rdi])
            held = file_dup(current->ofile[tf->rdi]);
        break;
    }
    long r = sock_call(tf, handled);
    if (held)
        file_close(held);
    return r;
}

static long sock_call(struct trapframe *tf, bool *handled)
{
    uint64_t a1 = tf->rdi, a2 = tf->rsi, a3 = tf->rdx, a4 = tf->r10, a5 = tf->r8, a6 = tf->r9;
    *handled = true;
    switch (tf->rax) {                               /* calls on an AF_UNIX descriptor */
    case SIEOS_SYS_bind: case SIEOS_SYS_listen: case SIEOS_SYS_accept: case SIEOS_SYS_connect:
    case SIEOS_SYS_shutdown: case SIEOS_SYS_recvfrom: case SIEOS_SYS_sendto: case SIEOS_SYS_recvmsg:
    case SIEOS_SYS_sendmsg: case SIEOS_SYS_getsockname: case SIEOS_SYS_getpeername:
    case SIEOS_SYS_setsockopt: case SIEOS_SYS_getsockopt:
        if ((long)a1 < 0 || (long)a1 >= NOFILE || !current->ofile[a1])
            return -EBADF;                           /* (not open: before "not a socket") */
        if (unix_fd(a1))
            return unix_syscall(tf->rax, a1, a2, a3, a4, a5, a6);
        break;
    }
    switch (tf->rax) {
    case SIEOS_SYS_so_socket:   return do_socket(a1, a2, a3);
    case SIEOS_SYS_bind:        return net(SYS_bind, a1, a2, a3, 0, 0, 0);
    case SIEOS_SYS_listen:      return net(SYS_listen, a1, a2, 0, 0, 0, 0);
    case SIEOS_SYS_accept: {
        if (a4 & ~(uint64_t)(SIEOS_SOCK_CLOEXEC | SIEOS_SOCK_NONBLOCK | SIEOS_SOCK_NDELAY))
            return -EINVAL;
        long fd = net(SYS_accept, a1, a2, a3, 0, 0, 0);
        fd_flags(fd, a4);
        return fd;
    }
    case SIEOS_SYS_connect:     return net(SYS_connect, a1, a2, a3, 0, 0, 0);
    case SIEOS_SYS_shutdown:
        if (a2 > SIEOS_SHUT_RDWR)
            return -EINVAL;
        return net(SYS_shutdown, a1, a2, 0, 0, 0, 0);
    case SIEOS_SYS_recvfrom:    return do_recv(a1, (void *)a2, a3, a4, (void *)a5, (unsigned int *)a6);
    case SIEOS_SYS_sendto:      return do_send(a1, (const void *)a2, a3, a4, (const void *)a5, a6);
    case SIEOS_SYS_recvmsg:     return do_msg(a1, (struct sieos_msghdr *)a2, a3, false);
    case SIEOS_SYS_sendmsg:     return do_msg(a1, (struct sieos_msghdr *)a2, a3, true);
    case SIEOS_SYS_getsockname: return net(SYS_getsockname, a1, a2, a3, 0, 0, 0);
    case SIEOS_SYS_getpeername: return net(SYS_getpeername, a1, a2, a3, 0, 0, 0);
    case SIEOS_SYS_setsockopt:  return do_setsockopt(a1, a2, a3, (const void *)a4, a5);
    case SIEOS_SYS_getsockopt:  return do_getsockopt(a1, a2, a3, (void *)a4, (unsigned int *)a5);
    case SIEOS_SYS_so_socketpair: {                  /* (domain, type, protocol, int sv[2]) */
        if (a1 != SIEOS_AF_UNIX)
            return a1 == SIEOS_AF_INET || a1 == SIEOS_AF_INET6 ? -EOPNOTSUPP : -EAFNOSUPPORT;
        if (a3 != 0)
            return -EPROTONOSUPPORT;
        if (a2 & ~(uint64_t)(SIEOS_SOCK_TYPE_MASK | SIEOS_SOCK_CLOEXEC | SIEOS_SOCK_NONBLOCK | SIEOS_SOCK_NDELAY))
            return -EINVAL;
        long r = unix_socketpair(a2 & SIEOS_SOCK_TYPE_MASK, (int *)a4);
        if (r == 0) {
            fd_flags(((int *)a4)[0], a2);
            fd_flags(((int *)a4)[1], a2);
        }
        return r;
    }
    case SIEOS_SYS_netinfo:     return net(SYS_netinfo, a1, a2, 0, 0, 0, 0);
    case SIEOS_SYS_netstat:     return net(SYS_netstat, a1, a2, 0, 0, 0, 0);
    case SIEOS_SYS_netinfo6:    return socket_netinfo6((struct sieos_netinfo6 *)a1, (long)a2);
    case SIEOS_SYS_netstat6:    return socket_netstat6((struct sieos_sockinfo6 *)a1, (int)a2);
    }
    *handled = false;
    return -ENOSYS;
}
