/*
 * sock.c - The Linux personality's pipes, sockets and waiting (included by
 * linux.c, whose fd table it shares):
 *
 *   pipes, unix socket pairs   ends of unixd's (port "unix"); descriptors
 *                              passed along (SCM_RIGHTS): ends, memfds
 *   memfd                      a shared memory object of the kernel's
 *                              (SYS_SHM_*), made when it is first sized
 *   eventfd                    a counter in this process
 *   TCP and UDP (IPv4)         sockets of netd's (port "net")
 *   poll, select               each server asked what is ready (UNIX_POLL,
 *                              NET_POLL); if nothing is, they note this
 *                              process (SYS_NOTE) when something is, and the
 *                              caller sleeps on its note word until then
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */

static long unix_port, net_port;
static long unixd(msg_t *m) { return call(&unix_port, "unix", m); }
static long netd(msg_t *m) { return call(&net_port, "net", m); }
static int used_unixd;                           /* (at the end: tell unixd we are gone) */
typedef struct conn conn_t;
static void conn_free(conn_t *c);
static long self_pid(void);

/* How many fds refer to the same thing as f (not f itself). */
static int others(const fd_t *f)
{
    int n = 0;
    for (int i = 0; i < NFD; i++)
        if (&fds[i] != f && fds[i].kind == f->kind && (f->kind == F_EVENT ? fds[i].obj == f->obj : fds[i].id == f->id)) n++;
    return n;
}
/* f is closed: what it held goes when no other fd refers to it. */
static void release(fd_t *f)
{
    if (f->kind == F_EVENT) { if (!others(f)) free(f->obj); return; }
    if (others(f)) return;
    if (f->kind == F_UNIX) { msg_t m = { .w = { UNIX_CLOSE, f->id } }; unixd(&m); }
    else if (f->kind == F_SHM && f->type) S1(SYS_SHM_CLOSE, f->id);
    else if (f->kind == F_SOCK) { msg_t m = { .w = { NET_CLOSE, f->id } }; netd(&m); if (f->obj) conn_free(f->obj); }
}

/* ---- Pipes and unix socket pairs */
static long new_pair(int type, int flags, int *fd)
{
    msg_t m = { .w = { UNIX_PAIR, (uint64_t)type } };
    long r = unixd(&m);
    if (r < 0) return r;
    used_unixd = 1;
    int nb = flags & O_NONBLOCK ? O_NONBLOCK : 0, ce = !!(flags & O_CLOEXEC);
    fd_t a = { .kind = F_UNIX, .flags = (type == UNIX_PIPE ? O_RDONLY : O_RDWR) | nb, .cloexec = ce, .id = m.w[1], .type = type };
    fd_t b = { .kind = F_UNIX, .flags = (type == UNIX_PIPE ? O_WRONLY : O_RDWR) | nb, .cloexec = ce, .id = m.w[2], .type = type };
    long x = fd_new(0, a), y = x < 0 ? x : fd_new(0, b);
    if (x < 0 || y < 0) {
        msg_t c1 = { .w = { UNIX_CLOSE, m.w[1] } }, c2 = { .w = { UNIX_CLOSE, m.w[2] } };
        unixd(&c1); unixd(&c2);
        if (x >= 0) fds[x].kind = F_FREE;
        return -EMFILE;
    }
    fd[0] = (int)x; fd[1] = (int)y;
    return 0;
}

/* Descriptors to pass (SCM_RIGHTS), and back. */
static long to_desc(int fd, unix_desc_t *d)
{
    fd_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind == F_UNIX) { *d = (unix_desc_t){ UD_END, (uint32_t)f->type, f->id, 0 }; return 0; }
    if (f->kind == F_SHM && f->type) { *d = (unix_desc_t){ UD_SHM, 0, f->id, f->off }; return 0; }
    return -EOPNOTSUPP;                                /* (files, sockets of netd's: not yet) */
}
static long from_desc(const unix_desc_t *d, int cloexec)
{
    fd_t f = { .cloexec = cloexec };
    if (d->kind == UD_END) { f.kind = F_UNIX; f.id = d->id; f.type = (int)d->extra; f.flags = O_RDWR; }
    else if (d->kind == UD_SHM) { f.kind = F_SHM; f.id = d->id; f.off = d->size; f.type = 1; f.flags = O_RDWR; }
    else return -EINVAL;
    for (int i = 0; i < NFD; i++)                       /* already held through another fd: once is enough */
        if (fds[i].kind == f.kind && fds[i].id == f.id) {
            if (f.kind == F_UNIX) { msg_t m = { .w = { UNIX_CLOSE, f.id } }; unixd(&m); }
            else S1(SYS_SHM_CLOSE, f.id);
            break;
        }
    long r = fd_new(0, f);
    return r;
}
static void drop_desc(const unix_desc_t *d)            /* received but not wanted (read(), or no room) */
{
    unix_desc_t x = *d;
    long fd = from_desc(&x, 0);
    if (fd >= 0) do_close(fd);
}

static long unix_send(fd_t *f, const void *data, size_t n, const unix_desc_t *d, int nd)
{
    static __thread uint8_t buf[UNIX_MAX + 8 * sizeof(unix_desc_t)];
    size_t done = 0;
    if (!f->id || (f->type == UNIX_PIPE && (f->flags & O_ACCMODE) == O_RDONLY)) return -EBADF;
    if (f->type == UNIX_SEQPACKET && n > UNIX_MAX) return -EMSGSIZE;
    do {
        size_t c = n - done < UNIX_MAX ? n - done : UNIX_MAX, dl = nd * sizeof(unix_desc_t);
        memcpy(buf, d, dl);
        memcpy(buf + dl, (const char *)data + done, c);
        msg_t m = { .w = { UNIX_SEND, f->id, (uint64_t)nd, f->flags & O_NONBLOCK ? UNIX_NONBLOCK : 0 }, .sbuf = buf, .slen = dl + c };
        long r = unixd(&m);
        if (r < 0) return done ? (long)done : r;
        done += r;
        nd = 0;
    } while (done < n);
    return done;
}
/* -> data bytes; *nd: descriptors received (now fds in fdv, at most maxfd; the rest dropped) */
static long unix_recv(fd_t *f, void *data, size_t n, int flags, int *fdv, int maxfd, int *nd, int cloexec)
{
    static __thread uint8_t buf[UNIX_MAX + 8 * sizeof(unix_desc_t)];
    if (n > UNIX_MAX) n = UNIX_MAX;
    int fl = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT) ? UNIX_NONBLOCK : 0;
    if (flags & MSG_PEEK) fl |= UNIX_PEEK;
    msg_t m = { .w = { UNIX_RECV, f->id, n, (uint64_t)fl }, .rbuf = buf, .rlen = sizeof buf };
    long r = unixd(&m);
    if (r < 0) return r;
    int k = (int)m.w[1];
    unix_desc_t *d = (unix_desc_t *)buf;
    *nd = 0;
    for (int i = 0; i < k; i++) {
        if (*nd < maxfd) { long fd = from_desc(&d[i], cloexec); if (fd >= 0) fdv[(*nd)++] = (int)fd; }
        else drop_desc(&d[i]);
    }
    memcpy(data, buf + k * sizeof(unix_desc_t), r);
    return r;
}

/* ---- eventfd: a counter of this process's; a write notes the process (poll) */
typedef struct { volatile uint64_t count; int sem; } event_t;
static long event_rw(fd_t *f, int write, void *buf, size_t n)
{
    event_t *e = f->obj;
    if (n < 8) return -EINVAL;
    if (write) {
        uint64_t v;
        memcpy(&v, buf, 8);
        if (v == ~0ULL) return -EINVAL;
        __atomic_add_fetch(&e->count, v, __ATOMIC_SEQ_CST);
        S4(SYS_FUTEX, FUTEX_WAKE, &e->count, 1 << 30, 0);
        S1(SYS_NOTE, self_pid());
        return 8;
    }
    for (;;) {
        uint64_t c = e->count;
        if (c) {
            uint64_t take = e->sem ? 1 : c;
            if (!__atomic_compare_exchange_n(&e->count, &c, c - take, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) continue;
            memcpy(buf, &take, 8);
            return 8;
        }
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        long r = S4(SYS_FUTEX, FUTEX_WAIT, &e->count, 0, 0);   /* (low 32 bits: enough to sleep on) */
        if (r == -EINTR) return r;
    }
}

/* ---- memfd: its object's number is chosen at once, the object made (SYS_SHM_CREATE)
 * when its size is first set (type: 1 once made; off: its size) */
static long memfd_new(int flags)
{
    fd_t f = { .kind = F_SHM, .flags = O_RDWR, .cloexec = !!(flags & 1) };   /* (MFD_CLOEXEC) */
    __sieos_random(&f.id, sizeof f.id);
    f.id = f.id >> 1 | 1;
    return fd_new(0, f);
}
static long shm_size(fd_t *f, uint64_t size)
{
    if (f->type) return size <= f->off ? 0 : -EINVAL;  /* (an object does not grow) */
    long r = S2(SYS_SHM_CREATE, f->id, size ? size : 1);
    if (r) return r;
    for (int i = 0; i < NFD; i++) if (fds[i].kind == F_SHM && fds[i].id == f->id) { fds[i].type = 1; fds[i].off = size; }
    return 0;
}

/* ---- Sockets of netd's (IPv4) */
struct conn { volatile int done; long result; long sh; uint32_t ip; uint16_t port; volatile uint32_t alive; char *stack; };   /* a connect going on */
static void conn_free(conn_t *c)                       /* (its thread has ended: the kernel cleared alive) */
{
    while (__atomic_load_n(&c->alive, __ATOMIC_ACQUIRE)) S4(SYS_FUTEX, FUTEX_WAIT, &c->alive, c->alive, 0);
    free(c->stack);
    free(c);
}
static long sock_new(int domain, int type, int proto)
{
    int base = type & 0xf, ce = !!(type & SOCK_CLOEXEC), nb = type & SOCK_NONBLOCK ? O_NONBLOCK : 0;
    (void)proto;
    if (domain == AF_UNIX) return -EAFNOSUPPORT;       /* (named unix sockets: not yet; socketpair works) */
    if (domain != AF_INET) return -EAFNOSUPPORT;
    if (base != SOCK_STREAM && base != SOCK_DGRAM) return -EPROTONOSUPPORT;
    msg_t m = { .w = { NET_SOCKET, base == SOCK_STREAM ? NET_TCP : NET_UDP } };
    long sh = netd(&m);
    if (sh < 0) return sh;
    fd_t f = { .kind = F_SOCK, .flags = O_RDWR | nb, .cloexec = ce, .id = (uint64_t)sh, .type = base };
    long fd = fd_new(0, f);
    if (fd < 0) { msg_t c = { .w = { NET_CLOSE, (uint64_t)sh } }; netd(&c); }
    return fd;
}
static void put_addr(void *addr, socklen_t *len, uint32_t ip, uint16_t port)
{
    if (!addr || !len) return;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = __builtin_bswap16(port), .sin_addr.s_addr = __builtin_bswap32(ip) };
    memcpy(addr, &a, *len < sizeof a ? *len : sizeof a);
    *len = sizeof a;
}
static long get_addr(const void *addr, socklen_t len, uint32_t *ip, uint16_t *port)
{
    struct sockaddr_in a;
    if (!addr || len < sizeof a) return -EINVAL;
    memcpy(&a, addr, sizeof a);
    if (a.sin_family != AF_INET) return -EAFNOSUPPORT;
    *ip = __builtin_bswap32(a.sin_addr.s_addr);
    *port = __builtin_bswap16(a.sin_port);
    return 0;
}
static int net_ready(fd_t *f)                          /* NET_IN | NET_OUT | NET_ERR, noting us later if 0 */
{
    conn_t *c = f->obj;
    if (c && !c->done) return 0;
    if (c && c->result < 0) return NET_ERR | NET_OUT;
    msg_t m = { .w = { NET_POLL, f->id } };
    long r = netd(&m);
    return r < 0 ? NET_ERR : (int)r;
}
static void connector(conn_t *c)                       /* a non-blocking connect, waited for here */
{
    S1(SYS_THREAD_SELF, &c->alive);                    /* (0 when this thread has ended) */
    msg_t m = { .w = { NET_CONNECT, (uint64_t)c->sh, c->ip, c->port } };
    c->result = netd(&m);
    __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
    S1(SYS_NOTE, self_pid());
    S0(SYS_THREAD_EXIT);
}
static long sock_connect(fd_t *f, const void *addr, socklen_t len)
{
    uint32_t ip; uint16_t port;
    long r = get_addr(addr, len, &ip, &port);
    if (r) return r;
    f->ip = ip; f->port = port;
    if (f->type == SOCK_DGRAM) return 0;                /* (UDP: the default destination) */
    if (f->obj) return ((conn_t *)f->obj)->done ? -EISCONN : -EALREADY;
    if (!(f->flags & O_NONBLOCK)) {
        msg_t m = { .w = { NET_CONNECT, f->id, ip, port } };
        return netd(&m);
    }
    conn_t *c = malloc(sizeof *c);
    char *st = malloc(16384);
    if (!c || !st) { free(c); free(st); return -ENOMEM; }
    *c = (conn_t){ 0, 0, (long)f->id, ip, port, 1, st };
    f->obj = c;
    if (S3(SYS_THREAD_CREATE, connector, st + 16384 - 64, c) < 0) { f->obj = 0; free(st); free(c); return -EAGAIN; }
    return -EINPROGRESS;
}
static long sock_send(fd_t *f, const void *buf, size_t n, int flags, uint32_t ip, uint16_t port)
{
    if (f->type == SOCK_DGRAM) {
        if (!ip) { ip = f->ip; port = f->port; }
        if (!ip) return -EDESTADDRREQ;
        msg_t m = { .w = { NET_SENDTO, f->id, ip, port }, .sbuf = buf, .slen = n };
        return netd(&m);
    }
    conn_t *c = f->obj;
    if (c && (!c->done || c->result < 0)) return c->done ? c->result : -EAGAIN;
    size_t done = 0;
    while (done < n) {
        if (((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) && !(net_ready(f) & NET_OUT)) return done ? (long)done : -EAGAIN;
        size_t k = n - done < NET_MAX ? n - done : NET_MAX;
        msg_t m = { .w = { NET_SEND, f->id }, .sbuf = (const char *)buf + done, .slen = k };
        long r = netd(&m);
        if (r < 0) return done ? (long)done : r == -ENOTCONN && !c ? -ENOTCONN : r;
        done += r;
    }
    return done;
}
static long sock_recv(fd_t *f, void *buf, size_t n, int flags, uint32_t *ip, uint16_t *port)
{
    if (((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) && !(net_ready(f) & (NET_IN | NET_ERR))) return -EAGAIN;
    if (n > NET_MAX) n = NET_MAX;
    msg_t m = { .w = { f->type == SOCK_DGRAM ? NET_RECVFROM : NET_RECV, f->id, n, 0 }, .rbuf = buf, .rlen = n };
    long r = netd(&m);
    if (r < 0) return r;
    if (f->type == SOCK_DGRAM) { if (ip) *ip = (uint32_t)m.w[1]; if (port) *port = (uint16_t)m.w[2]; }
    return (long)m.rlen;
}

static long do_socket_call(long n, long a, long b, long c, long d, long e, long g)
{
    fd_t *f;
    switch (n) {
    case SYS_socket: return sock_new((int)a, (int)b, (int)c);
    case SYS_socketpair: {
        int base = b & 0xf;
        if (a != AF_UNIX) return -EAFNOSUPPORT;
        if (base != SOCK_STREAM && base != SOCK_SEQPACKET && base != SOCK_DGRAM) return -EPROTONOSUPPORT;
        return new_pair(base == SOCK_STREAM ? UNIX_STREAM : UNIX_SEQPACKET,
                        (b & SOCK_NONBLOCK ? O_NONBLOCK : 0) | (b & SOCK_CLOEXEC ? O_CLOEXEC : 0), (int *)d);
    }
    }
    if (!(f = fd_get(a))) return -EBADF;
    if (f->kind != F_SOCK && f->kind != F_UNIX) return -ENOTSOCK;
    int un = f->kind == F_UNIX;
    switch (n) {
    case SYS_connect: return un ? -EISCONN : sock_connect(f, (void *)b, (socklen_t)c);
    case SYS_bind: {
        uint32_t ip; uint16_t port;
        long r = un ? -EINVAL : get_addr((void *)b, (socklen_t)c, &ip, &port);
        if (r) return r;
        f->lport = port;
        if (f->type == SOCK_DGRAM) { msg_t m = { .w = { NET_BIND, f->id, port } }; return netd(&m); }
        return 0;                                       /* (TCP: the port is given at listen) */
    }
    case SYS_listen: {
        if (un || f->type != SOCK_STREAM) return -EOPNOTSUPP;
        msg_t m = { .w = { NET_LISTEN, f->id, f->lport, (uint64_t)(b > 0 && b < 16 ? b : 8) } };
        return netd(&m);
    }
    case SYS_accept: case SYS_accept4: {
        if (un) return -EOPNOTSUPP;
        if ((f->flags & O_NONBLOCK) && !(net_ready(f) & NET_IN)) return -EAGAIN;
        msg_t m = { .w = { NET_ACCEPT, f->id, 0, 0 } };
        long sh = netd(&m);
        if (sh < 0) return sh;
        int fl = n == SYS_accept4 ? (int)d : 0;
        fd_t nf = { .kind = F_SOCK, .flags = O_RDWR | (fl & SOCK_NONBLOCK ? O_NONBLOCK : 0), .cloexec = !!(fl & SOCK_CLOEXEC),
                    .id = (uint64_t)sh, .type = SOCK_STREAM, .ip = (uint32_t)m.w[1], .port = (uint16_t)m.w[2], .lport = f->lport };
        put_addr((void *)b, (socklen_t *)c, nf.ip, nf.port);
        long fd = fd_new(0, nf);
        if (fd < 0) { msg_t cl = { .w = { NET_CLOSE, (uint64_t)sh } }; netd(&cl); }
        return fd;
    }
    case SYS_sendto: {
        uint32_t ip = 0; uint16_t port = 0;
        if (e && !un) { long r = get_addr((void *)e, (socklen_t)g, &ip, &port); if (r) return r; }
        return un ? unix_send(f, (void *)b, c, 0, 0) : sock_send(f, (void *)b, c, (int)d, ip, port);
    }
    case SYS_recvfrom: {
        uint32_t ip = 0; uint16_t port = 0;
        int nd = 0;
        long r = un ? unix_recv(f, (void *)b, c, (int)d, 0, 0, &nd, 0) : sock_recv(f, (void *)b, c, (int)d, &ip, &port);
        if (r >= 0 && e && !un) put_addr((void *)e, (socklen_t *)g, ip ? ip : f->ip, ip ? port : f->port);
        return r;
    }
    case SYS_sendmsg: {
        struct msghdr *h = (void *)b;
        size_t len = 0;
        for (int i = 0; i < (int)h->msg_iovlen; i++) len += h->msg_iov[i].iov_len;
        if (len > UNIX_MAX && (un ? f->type == UNIX_SEQPACKET : f->type == SOCK_DGRAM)) return -EMSGSIZE;
        char *buf = h->msg_iovlen == 1 ? h->msg_iov[0].iov_base : malloc(len ? len : 1), *own = h->msg_iovlen == 1 ? 0 : buf;
        if (!buf) return -ENOMEM;
        for (size_t i = 0, o = 0; own && i < (size_t)h->msg_iovlen; o += h->msg_iov[i++].iov_len)
            memcpy(buf + o, h->msg_iov[i].iov_base, h->msg_iov[i].iov_len);
        long res;
        unix_desc_t dv[8];
        int nd = 0;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(h); cm; cm = CMSG_NXTHDR(h, cm)) {
            if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
            int *fv = (int *)CMSG_DATA(cm);
            for (size_t i = 0; i < (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int); i++) {
                if (nd == 8) { free(own); return -ETOOMANYREFS; }
                long r = to_desc(fv[i], &dv[nd]);
                if (r) { free(own); return r; }
                nd++;
            }
        }
        if (!un) {
            uint32_t ip = 0; uint16_t port = 0;
            res = nd ? -EOPNOTSUPP : 0;
            if (!res && h->msg_name) res = get_addr(h->msg_name, h->msg_namelen, &ip, &port);
            if (!res) res = sock_send(f, buf, len, (int)c, ip, port);
        } else res = unix_send(f, buf, len, dv, nd);
        free(own);
        return res;
    }
    case SYS_recvmsg: {
        struct msghdr *h = (void *)b;
        size_t want = 0;
        for (int i = 0; i < (int)h->msg_iovlen; i++) want += h->msg_iov[i].iov_len;
        if (want > UNIX_MAX) want = UNIX_MAX;
        char *buf = h->msg_iovlen == 1 ? h->msg_iov[0].iov_base : malloc(want ? want : 1), *own = h->msg_iovlen == 1 ? 0 : buf;
        if (!buf) return -ENOMEM;
        int fv[8], nd = 0, maxfd = 0;
        if (h->msg_control && h->msg_controllen >= CMSG_LEN(sizeof(int)))
            maxfd = (int)((h->msg_controllen - CMSG_LEN(0)) / sizeof(int));
        if (maxfd > 8) maxfd = 8;
        uint32_t ip = 0; uint16_t port = 0;
        long r = un ? unix_recv(f, buf, want, (int)c, fv, maxfd, &nd, !!(c & MSG_CMSG_CLOEXEC))
                    : sock_recv(f, buf, want, (int)c, &ip, &port);
        if (r < 0) { free(own); return r; }
        size_t off = 0;
        for (int i = 0; own && i < (int)h->msg_iovlen && off < (size_t)r; i++) {
            size_t k = (size_t)r - off < h->msg_iov[i].iov_len ? (size_t)r - off : h->msg_iov[i].iov_len;
            memcpy(h->msg_iov[i].iov_base, buf + off, k);
            off += k;
        }
        free(own);
        h->msg_flags = 0;
        if (nd) {
            struct cmsghdr *cm = CMSG_FIRSTHDR(h);
            cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(nd * sizeof(int));
            memcpy(CMSG_DATA(cm), fv, nd * sizeof(int));
            h->msg_controllen = CMSG_SPACE(nd * sizeof(int));
        } else h->msg_controllen = 0;
        if (!un && h->msg_name) put_addr(h->msg_name, &h->msg_namelen, ip, port);
        return r;
    }
    case SYS_shutdown:
        if (un) { msg_t m = { .w = { UNIX_SHUTDOWN, f->id, (uint64_t)b } }; return unixd(&m); }
        return 0;                                       /* (TCP: closing only, at close) */
    case SYS_getsockname: put_addr((void *)b, (socklen_t *)c, 0, f->lport); return 0;
    case SYS_getpeername:
        if (un) { struct sockaddr *s = (void *)b; if (*(socklen_t *)c >= 2) s->sa_family = AF_UNIX; *(socklen_t *)c = 2; return 0; }
        if (!f->ip) return -ENOTCONN;
        put_addr((void *)b, (socklen_t *)c, f->ip, f->port);
        return 0;
    case SYS_setsockopt: return 0;
    case SYS_getsockopt: {
        int v = 0;
        if (b == SOL_SOCKET && c == SO_ERROR) {
            conn_t *cn = f->obj;
            if (cn && cn->done && cn->result < 0) { v = (int)-cn->result; cn->result = 0; }
        } else if (b == SOL_SOCKET && c == SO_TYPE) v = un ? (f->type == UNIX_SEQPACKET ? SOCK_SEQPACKET : SOCK_STREAM) : f->type;
        else if (b == SOL_SOCKET && (c == SO_RCVBUF || c == SO_SNDBUF)) v = 256 << 10;
        if (d && e && *(socklen_t *)e >= sizeof v) { memcpy((void *)d, &v, sizeof v); *(socklen_t *)e = sizeof v; }
        return 0;
    }
    }
    return -ENOSYS;
}

/* ---- poll: ask, and if nothing is ready, sleep on the note word until a
 * server says something changed (or the time is up), then ask again. */
static long do_poll(struct pollfd *p, unsigned long n, int64_t ns)   /* ns < 0: no limit */
{
    static __thread uint64_t ids[256];
    static __thread int where[256];
    uint64_t end = ns > 0 ? now_ns(CLOCK_MONOTONIC) + ns : 0;
    for (;;) {
        uint32_t gen = __atomic_load_n(&note_word, __ATOMIC_ACQUIRE);
        long ready = 0;
        int nu = 0;
        for (unsigned long i = 0; i < n; i++) {
            int ev = p[i].events | POLLHUP | POLLERR, r = 0;
            p[i].revents = 0;
            if (p[i].fd < 0) continue;
            fd_t *f = fd_get(p[i].fd);
            if (!f) { p[i].revents = POLLNVAL; ready++; continue; }
            switch (f->kind) {
            case F_UNIX:
                if (nu < 256) { ids[nu] = f->id; where[nu++] = (int)i; }
                continue;
            case F_SOCK: r = net_ready(f); break;
            case F_EVENT: r = (((event_t *)f->obj)->count ? NET_IN : 0) | NET_OUT; break;
            default: r = NET_IN | NET_OUT;                /* files, the terminal, devices: always */
            }
            p[i].revents = (short)(((r & NET_IN ? POLLIN | POLLRDNORM : 0) | (r & NET_OUT ? POLLOUT | POLLWRNORM : 0) |
                                    (r & NET_ERR ? (f->kind == F_SOCK ? POLLERR | POLLHUP : POLLHUP) : 0)) & ev);
            ready += p[i].revents != 0;
        }
        if (nu) {
            static __thread uint8_t bits[256];
            msg_t m = { .w = { UNIX_POLL }, .sbuf = ids, .slen = nu * 8, .rbuf = bits, .rlen = sizeof bits };
            long r = unixd(&m);
            for (int k = 0; k < nu; k++) {
                int b = r < 0 ? NET_ERR : bits[k], i = where[k];
                p[i].revents = (short)(((b & NET_IN ? POLLIN | POLLRDNORM : 0) | (b & NET_OUT ? POLLOUT | POLLWRNORM : 0) |
                                        (b & NET_ERR ? POLLHUP : 0)) & (p[i].events | POLLHUP | POLLERR));
                ready += p[i].revents != 0;
            }
        }
        if (ready || !ns) return ready;
        uint64_t now = now_ns(CLOCK_MONOTONIC), left = 0;
        if (ns > 0) { if (now >= end) return 0; left = end - now; }
        long r = S4(SYS_FUTEX, FUTEX_WAIT, &note_word, gen, left);
        if (r == -EINTR) return -EINTR;
    }
}
static long do_select(int n, fd_set *rd, fd_set *wr, fd_set *ex, int64_t ns)
{
    struct pollfd p[256];
    int k = 0;
    if (n > 256) return -EINVAL;
    for (int i = 0; i < n; i++) {
        short ev = (rd && FD_ISSET(i, rd) ? POLLIN : 0) | (wr && FD_ISSET(i, wr) ? POLLOUT : 0) | (ex && FD_ISSET(i, ex) ? POLLPRI : 0);
        if (ev) p[k++] = (struct pollfd){ i, ev, 0 };
    }
    long r = do_poll(p, k, ns);
    if (r < 0) return r;
    if (rd) FD_ZERO(rd);
    if (wr) FD_ZERO(wr);
    if (ex) FD_ZERO(ex);
    long c = 0;
    for (int j = 0; j < k; j++) {
        if (p[j].revents & POLLNVAL) return -EBADF;
        if (rd && (p[j].revents & (POLLIN | POLLHUP | POLLERR))) { FD_SET(p[j].fd, rd); c++; }
        if (wr && (p[j].revents & (POLLOUT | POLLERR))) { FD_SET(p[j].fd, wr); c++; }
    }
    return c;
}
static int64_t ts_or_inf(const struct timespec *t) { return t ? (int64_t)ts_ns(t) : -1; }
