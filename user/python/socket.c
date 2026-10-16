/*
 * socket.c - The socket and ssl modules for Python on SIEOS, over the
 * network server's sockets (user/lib/net.c) and TLS 1.3/1.2 (user/lib/tls.c).
 *
 *   s = socket.socket()                          TCP (socket.SOCK_DGRAM: UDP)
 *   s.connect((host, port)); s.send(b); s.recv(n); s.close()
 *   s.bind(('', port)); s.listen(n); c, addr = s.accept()
 *   socket.getaddrinfo(host, port)               -> [(AF_INET, SOCK_STREAM, 0, '', (ip, port))]
 *   t = ssl.wrap_socket(s, server_hostname=host) certificates checked against
 *                                                /etc/ssl/roots (cert_reqs=ssl.CERT_NONE: not)
 * Both are also streams: read(), readline(), write(). IPv4 only; a timeout
 * (settimeout) is in seconds, None: wait as long as it takes.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "py/runtime.h"
#include "py/stream.h"
#include "py/mperrno.h"
#include "net.h"

enum { AF_INET = 2, SOCK_STREAM = 1, SOCK_DGRAM = 2, SOL_SOCKET = 1, SO_REUSEADDR = 2, CERT_NONE = 0, CERT_REQUIRED = 2 };

typedef struct {
    mp_obj_base_t base;
    long s;                         /* the network server's socket, -1: none (yet) */
    tls_t *tls;                     /* ssl: the TLS session on it */
    int udp;
    uint16_t port;                  /* bind()'s, until listen() */
    unsigned timeout;               /* ms, 0: none */
} sock_t;

/* An error -> OSError; a wait Ctrl-C ended (-EINTR): its KeyboardInterrupt. */
static long intr(long e) { if (e == -EINTR) mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS); return e; }
static long check(long e) { if (intr(e) < 0) mp_raise_OSError((int)-e); return e; }
static sock_t *open_sock(mp_obj_t o)
{
    sock_t *k = MP_OBJ_TO_PTR(o);
    if (k->s < 0) mp_raise_OSError(MP_EBADF);
    return k;
}

/* (host, port) -> the address, resolved */
static uint16_t addr(mp_obj_t a, uint32_t *ip, unsigned timeout)
{
    mp_obj_t *t;
    mp_obj_get_array_fixed_n(a, 2, &t);
    const char *host = mp_obj_str_get_str(t[0]);
    *ip = 0;
    if (*host) check(net_resolve(host, ip, timeout));
    return (uint16_t)mp_obj_get_int(t[1]);
}
static mp_obj_t addr_tuple(uint32_t ip, uint16_t port)
{
    char b[16];
    net_ntoa(ip, b);
    mp_obj_t t[2] = { mp_obj_new_str(b, strlen(b)), MP_OBJ_NEW_SMALL_INT(port) };
    return mp_obj_new_tuple(2, t);
}

extern const mp_obj_type_t socket_type, sslsocket_type;
static sock_t *new_sock(const mp_obj_type_t *type, long s)
{
    sock_t *k = mp_obj_malloc_with_finaliser(sock_t, type);
    k->s = s, k->tls = 0, k->udp = 0, k->port = 0, k->timeout = 0;
    return k;
}

static mp_obj_t sock_make(const mp_obj_type_t *type, size_t n, size_t nkw, const mp_obj_t *args)
{
    mp_arg_check_num(n, nkw, 0, 3, false);
    sock_t *k = new_sock(type, -1);
    k->udp = n > 1 && mp_obj_get_int(args[1]) == SOCK_DGRAM;
    return MP_OBJ_FROM_PTR(k);
}

static mp_obj_t sock_connect(mp_obj_t self, mp_obj_t a)
{
    sock_t *k = MP_OBJ_TO_PTR(self);
    uint32_t ip;
    uint16_t port = addr(a, &ip, k->timeout);
    if (k->s >= 0 || k->udp) mp_raise_OSError(MP_EISCONN);
    k->s = check(net_connect(ip, port, k->timeout));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_connect_obj, sock_connect);

static mp_obj_t sock_bind(mp_obj_t self, mp_obj_t a)
{
    sock_t *k = MP_OBJ_TO_PTR(self);
    uint32_t ip;
    k->port = addr(a, &ip, k->timeout);
    if (k->udp) k->s = check(net_udp(k->port));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_bind_obj, sock_bind);

static mp_obj_t sock_listen(size_t n, const mp_obj_t *a)
{
    sock_t *k = MP_OBJ_TO_PTR(a[0]);
    k->s = check(net_listen(k->port, n > 1 ? (int)mp_obj_get_int(a[1]) : 4));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_listen_obj, 1, 2, sock_listen);

static mp_obj_t sock_accept(mp_obj_t self)
{
    sock_t *k = open_sock(self);
    uint32_t ip;
    uint16_t port;
    mp_obj_t t[2] = { MP_OBJ_FROM_PTR(new_sock(&socket_type, check(net_accept(k->s, &ip, &port, k->timeout)))),
                      addr_tuple(ip, port) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_accept_obj, sock_accept);

/* the stream functions: what read(), readline(), write(), send() and recv() use */
static mp_uint_t sock_read(mp_obj_t self, void *buf, mp_uint_t size, int *err)
{
    sock_t *k = MP_OBJ_TO_PTR(self);
    long n = k->s < 0 ? -EBADF : k->tls ? tls_read(k->tls, buf, size, k->timeout) : net_recv(k->s, buf, size, k->timeout);
    if (intr(n) < 0) { *err = (int)-n; return MP_STREAM_ERROR; }
    return (mp_uint_t)n;
}
static mp_uint_t sock_write(mp_obj_t self, const void *buf, mp_uint_t size, int *err)
{
    sock_t *k = MP_OBJ_TO_PTR(self);
    long n = k->s < 0 ? -EBADF : k->tls ? tls_write(k->tls, buf, size) : net_send(k->s, buf, size);
    if (intr(n) < 0) { *err = (int)-n; return MP_STREAM_ERROR; }
    return size;
}
static mp_uint_t sock_ioctl(mp_obj_t self, mp_uint_t req, uintptr_t arg, int *err)
{
    (void)arg;
    sock_t *k = MP_OBJ_TO_PTR(self);
    if (req == MP_STREAM_CLOSE) {
        if (k->tls) tls_close(k->tls);          /* (closes the socket too) */
        else if (k->s >= 0) net_close(k->s);
        k->tls = 0, k->s = -1;
        return 0;
    }
    *err = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static mp_obj_t sock_send(mp_obj_t self, mp_obj_t data)
{
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    int err = 0;
    if (sock_write(self, b.buf, b.len, &err) == MP_STREAM_ERROR) check(-err);
    return MP_OBJ_NEW_SMALL_INT(b.len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_send_obj, sock_send);

static mp_obj_t sock_recv(mp_obj_t self, mp_obj_t size)
{
    vstr_t v;
    vstr_init_len(&v, (size_t)mp_obj_get_int(size));
    int err = 0;
    mp_uint_t n = sock_read(self, v.buf, v.len, &err);
    if (n == MP_STREAM_ERROR) check(-err);
    v.len = n;
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_recv_obj, sock_recv);

static mp_obj_t sock_sendto(mp_obj_t self, mp_obj_t data, mp_obj_t a)
{
    sock_t *k = MP_OBJ_TO_PTR(self);
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    uint32_t ip;
    uint16_t port = addr(a, &ip, k->timeout);
    if (k->s < 0) k->s = check(net_udp(0));
    return MP_OBJ_NEW_SMALL_INT(check(net_sendto(k->s, ip, port, b.buf, b.len)));
}
static MP_DEFINE_CONST_FUN_OBJ_3(sock_sendto_obj, sock_sendto);

static mp_obj_t sock_recvfrom(mp_obj_t self, mp_obj_t size)
{
    sock_t *k = open_sock(self);
    vstr_t v;
    vstr_init_len(&v, (size_t)mp_obj_get_int(size));
    uint32_t ip;
    uint16_t port;
    v.len = (size_t)check(net_recvfrom(k->s, v.buf, v.len, &ip, &port, k->timeout));
    mp_obj_t t[2] = { mp_obj_new_bytes_from_vstr(&v), addr_tuple(ip, port) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_recvfrom_obj, sock_recvfrom);

static mp_obj_t sock_settimeout(mp_obj_t self, mp_obj_t t)
{
    ((sock_t *)MP_OBJ_TO_PTR(self))->timeout = t == mp_const_none ? 0 : (unsigned)(mp_obj_get_float(t) * 1000) + 1;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_settimeout_obj, sock_settimeout);

static mp_obj_t sock_setsockopt(size_t n, const mp_obj_t *a) { (void)n; (void)a; return mp_const_none; }   /* (SO_REUSEADDR: always so) */
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_setsockopt_obj, 4, 4, sock_setsockopt);

static mp_obj_t sock_makefile(size_t n, const mp_obj_t *a) { (void)n; return a[0]; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_makefile_obj, 1, 3, sock_makefile);

static const mp_rom_map_elem_t sock_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&sock_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_bind), MP_ROM_PTR(&sock_bind_obj) },
    { MP_ROM_QSTR(MP_QSTR_listen), MP_ROM_PTR(&sock_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_accept), MP_ROM_PTR(&sock_accept_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&sock_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendall), MP_ROM_PTR(&sock_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&sock_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendto), MP_ROM_PTR(&sock_sendto_obj) },
    { MP_ROM_QSTR(MP_QSTR_recvfrom), MP_ROM_PTR(&sock_recvfrom_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout), MP_ROM_PTR(&sock_settimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_setsockopt), MP_ROM_PTR(&sock_setsockopt_obj) },
    { MP_ROM_QSTR(MP_QSTR_makefile), MP_ROM_PTR(&sock_makefile_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(sock_locals, sock_locals_table);
static const mp_stream_p_t sock_stream = { .read = sock_read, .write = sock_write, .ioctl = sock_ioctl };
MP_DEFINE_CONST_OBJ_TYPE(socket_type, MP_QSTR_socket, MP_TYPE_FLAG_NONE, make_new, sock_make,
                         protocol, &sock_stream, locals_dict, &sock_locals);
MP_DEFINE_CONST_OBJ_TYPE(sslsocket_type, MP_QSTR_SSLSocket, MP_TYPE_FLAG_NONE,
                         protocol, &sock_stream, locals_dict, &sock_locals);

/* getaddrinfo(host, port, ...) -> one IPv4 TCP address */
static mp_obj_t sock_getaddrinfo(size_t n, const mp_obj_t *a)
{
    mp_obj_t hp[2] = { a[0], a[1] };
    uint32_t ip;
    uint16_t port = addr(mp_obj_new_tuple(2, hp), &ip, 0);
    mp_obj_t t[5] = { MP_OBJ_NEW_SMALL_INT(AF_INET), MP_OBJ_NEW_SMALL_INT(n > 3 ? mp_obj_get_int(a[3]) : SOCK_STREAM),
                      MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_QSTR(MP_QSTR_), addr_tuple(ip, port) };
    mp_obj_t e = mp_obj_new_tuple(5, t);
    return mp_obj_new_list(1, &e);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_getaddrinfo_obj, 2, 6, sock_getaddrinfo);

static const mp_rom_map_elem_t socket_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_socket) },
    { MP_ROM_QSTR(MP_QSTR_socket), MP_ROM_PTR(&socket_type) },
    { MP_ROM_QSTR(MP_QSTR_getaddrinfo), MP_ROM_PTR(&sock_getaddrinfo_obj) },
    { MP_ROM_QSTR(MP_QSTR_AF_INET), MP_ROM_INT(AF_INET) },
    { MP_ROM_QSTR(MP_QSTR_SOCK_STREAM), MP_ROM_INT(SOCK_STREAM) },
    { MP_ROM_QSTR(MP_QSTR_SOCK_DGRAM), MP_ROM_INT(SOCK_DGRAM) },
    { MP_ROM_QSTR(MP_QSTR_SOL_SOCKET), MP_ROM_INT(SOL_SOCKET) },
    { MP_ROM_QSTR(MP_QSTR_SO_REUSEADDR), MP_ROM_INT(SO_REUSEADDR) },
};
static MP_DEFINE_CONST_DICT(socket_globals, socket_table);
const mp_obj_module_t mp_module_socket = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&socket_globals };
MP_REGISTER_MODULE(MP_QSTR_socket, mp_module_socket);

/* ---- ssl.wrap_socket(sock, server_hostname=None, cert_reqs=CERT_REQUIRED) */
static mp_obj_t ssl_wrap(size_t n, const mp_obj_t *a, mp_map_t *kw)
{
    (void)n;
    sock_t *k = open_sock(a[0]);
    mp_map_elem_t *h = mp_map_lookup(kw, MP_OBJ_NEW_QSTR(MP_QSTR_server_hostname), MP_MAP_LOOKUP);
    mp_map_elem_t *r = mp_map_lookup(kw, MP_OBJ_NEW_QSTR(MP_QSTR_cert_reqs), MP_MAP_LOOKUP);
    const char *host = h && h->value != mp_const_none ? mp_obj_str_get_str(h->value) : "";
    long err = 0;
    tls_t *t = tls_connect(k->s, host, r && mp_obj_get_int(r->value) == CERT_NONE ? TLS_INSECURE : 0, &err);
    if (!t) { k->s = -1; check(err < 0 ? err : -MP_EIO); }   /* (tls_connect closed the socket) */
    sock_t *s = new_sock(&sslsocket_type, k->s);
    s->tls = t, s->timeout = k->timeout;
    k->s = -1;                                   /* (the TLS socket has it now) */
    return MP_OBJ_FROM_PTR(s);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ssl_wrap_obj, 1, ssl_wrap);

static const mp_rom_map_elem_t ssl_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ssl) },
    { MP_ROM_QSTR(MP_QSTR_wrap_socket), MP_ROM_PTR(&ssl_wrap_obj) },
    { MP_ROM_QSTR(MP_QSTR_CERT_NONE), MP_ROM_INT(CERT_NONE) },
    { MP_ROM_QSTR(MP_QSTR_CERT_REQUIRED), MP_ROM_INT(CERT_REQUIRED) },
};
static MP_DEFINE_CONST_DICT(ssl_globals, ssl_table);
const mp_obj_module_t mp_module_ssl = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&ssl_globals };
MP_REGISTER_MODULE(MP_QSTR_ssl, mp_module_ssl);
