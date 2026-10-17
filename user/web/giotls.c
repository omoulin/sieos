/*
 * giotls.c - GIO's TLS on SIEOS's own TLS client (user/lib/tls.c, x509.c):
 * what libsoup, and so WebKit, use for https, instead of glib-networking
 * with GnuTLS or OpenSSL (docs/webkit-plan.md, decision 6).
 *
 *   libsoup ──► GTlsClientConnection (SieTlsConnection, a GIOStream)
 *                 │  its streams: plaintext, blocking or not (pollable)
 *                 ▼
 *               tls.c: records, the handshake ──► net_send / net_recv
 *                 │      (defined here: the transport is the base stream)
 *                 ▼
 *               the base GIOStream (a GSocketConnection, through netd)
 *
 * tls.c reads and writes through net_send and net_recv on the "socket" it
 * is given: here the connection itself, whose base stream they use.
 * Reading without blocking: net_recv answers -EAGAIN, and tls.c keeps the
 * part of a record already read. Writing without blocking: a record, once
 * encrypted, must go out whole, so what the base stream does not take yet
 * waits in `pending` and leaves when the base stream can take it.
 *
 * The handshake runs in a thread for handshake_async (blocking reads and
 * writes there). tls.c checks the server's signatures; the chain to a
 * trusted root (/etc/ssl/roots) is checked here (TLS_CHAIN), so that a
 * refusal can be offered to "accept-certificate", as GIO wants. ALPN: "h2"
 * is offered when the caller lists it (libsoup's HTTP/2).
 *
 * Loaded by being linked in (pkg-config gio-tls-sieos: -u g_io_sieos_tls_load):
 * a constructor makes it GIO's TLS backend; GObject's own constructor must
 * have run first, so the .pc links GObject's gtype.o before this. Client
 * connections only.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <gio/gio.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include "net.h"

/* ---- What tls.c and x509.c need of SIEOS's library. */
long sys_random(void *buf, size_t n) { return getrandom(buf, n, 0); }
long net_info(net_info_t *in) { in->time = time(0); return 0; }
long net_close(long s) { (void)s; return 0; }    /* the GIOStream closes its base itself */
char *file_get(const char *path, size_t *len)
{
    gchar *b;
    gsize n;
    if (!g_file_get_contents(path, &b, &n, 0)) return 0;
    *len = n;
    return b;
}
int ct_equal(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    while (n--) d |= x[n] ^ y[n];
    return d == 0;
}
void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

/* ---- The certificate: DER, and its issuer. */
typedef struct { GTlsCertificate parent; GByteArray *der; GTlsCertificate *issuer; } SieTlsCertificate;
typedef struct { GTlsCertificateClass parent; } SieTlsCertificateClass;
static void cert_initable_init(GInitableIface *iface);
G_DEFINE_TYPE_WITH_CODE(SieTlsCertificate, sie_tls_certificate, G_TYPE_TLS_CERTIFICATE,
                        G_IMPLEMENT_INTERFACE(G_TYPE_INITABLE, cert_initable_init))
enum { C_CERT = 1, C_PEM, C_KEY, C_KEY_PEM, C_ISSUER };

static GTlsCertificate *cert_new(const uint8_t *der, size_t n, GTlsCertificate *issuer)
{
    SieTlsCertificate *c = g_object_new(sie_tls_certificate_get_type(), 0);
    c->der = g_byte_array_new();
    g_byte_array_append(c->der, der, (guint)n);
    c->issuer = issuer;                          /* the reference is handed over */
    return G_TLS_CERTIFICATE(c);
}

static void cert_get(GObject *o, guint id, GValue *v, GParamSpec *ps)
{
    SieTlsCertificate *c = (SieTlsCertificate *)o;
    switch (id) {
    case C_CERT: g_value_set_boxed(v, c->der); break;
    case C_PEM: {
        gchar *b = g_base64_encode(c->der->data, c->der->len);
        GString *s = g_string_new("-----BEGIN CERTIFICATE-----\n");
        for (size_t i = 0, n = strlen(b); i < n; i += 64) { g_string_append_len(s, b + i, MIN(64, (gssize)(n - i))); g_string_append_c(s, '\n'); }
        g_string_append(s, "-----END CERTIFICATE-----\n");
        g_free(b);
        g_value_take_string(v, g_string_free(s, FALSE));
        break;
    }
    case C_KEY: g_value_set_boxed(v, 0); break;
    case C_KEY_PEM: g_value_set_string(v, 0); break;
    case C_ISSUER: g_value_set_object(v, c->issuer); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
    }
}

static void cert_set(GObject *o, guint id, const GValue *v, GParamSpec *ps)
{
    SieTlsCertificate *c = (SieTlsCertificate *)o;
    switch (id) {
    case C_CERT: {
        GByteArray *b = g_value_get_boxed(v);
        if (b && !c->der) { c->der = g_byte_array_new(); g_byte_array_append(c->der, b->data, b->len); }
        break;
    }
    case C_PEM: {                                /* the first certificate in the text */
        const char *s = g_value_get_string(v), *b, *e;
        if (!s || c->der || !(b = strstr(s, "-----BEGIN CERTIFICATE-----")) || !(e = strstr(b, "-----END CERTIFICATE-----"))) break;
        b += 27;
        gchar *t = g_strndup(b, (gsize)(e - b));
        gsize n;
        guchar *d = g_base64_decode(t, &n);
        g_free(t);
        c->der = g_byte_array_new_take(d, n);
        break;
    }
    case C_KEY: case C_KEY_PEM: break;           /* a client: no keys */
    case C_ISSUER: g_set_object(&c->issuer, g_value_get_object(v)); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
    }
}

static gboolean cert_init(GInitable *i, GCancellable *cancel, GError **error)
{
    SieTlsCertificate *c = (SieTlsCertificate *)i;
    x509_t x;
    (void)cancel;
    if (!c->der || x509_parse(&x, c->der->data, c->der->len)) {
        g_set_error_literal(error, G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE, "Could not parse the certificate");
        return FALSE;
    }
    return TRUE;
}
static void cert_initable_init(GInitableIface *iface) { iface->init = cert_init; }

/* The chain from a certificate through its issuers, against the trusted roots */
static GTlsCertificateFlags verify(const uint8_t **certs, const size_t *lens, int n, const char *host)
{
    const char *why;
    if (!host) return G_TLS_CERTIFICATE_BAD_IDENTITY;
    if (!x509_verify_chain(certs, lens, n, host, time(0), "/etc/ssl/roots", &why)) return 0;
    if (strstr(why, "another name")) return G_TLS_CERTIFICATE_BAD_IDENTITY;
    if (strstr(why, "expired")) return G_TLS_CERTIFICATE_EXPIRED;
    if (strstr(why, "issuer") || strstr(why, "roots")) return G_TLS_CERTIFICATE_UNKNOWN_CA;
    return G_TLS_CERTIFICATE_GENERIC_ERROR;
}

static const char *identity_host(GSocketConnectable *id, char **owned)
{
    *owned = 0;
    if (G_IS_NETWORK_ADDRESS(id)) return g_network_address_get_hostname(G_NETWORK_ADDRESS(id));
    if (G_IS_NETWORK_SERVICE(id)) return g_network_service_get_domain(G_NETWORK_SERVICE(id));
    if (G_IS_INET_SOCKET_ADDRESS(id))
        return *owned = g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(id)));
    return 0;
}

static GTlsCertificateFlags cert_verify(GTlsCertificate *cert, GSocketConnectable *id, GTlsCertificate *ca)
{
    const uint8_t *certs[10];
    size_t lens[10];
    int n = 0;
    char *owned = 0;
    (void)ca;
    for (GTlsCertificate *c = cert; c && n < 10; c = ((SieTlsCertificate *)c)->issuer, n++) {
        certs[n] = ((SieTlsCertificate *)c)->der->data;
        lens[n] = ((SieTlsCertificate *)c)->der->len;
    }
    const char *host = id ? identity_host(id, &owned) : 0;
    GTlsCertificateFlags f = verify(certs, lens, n, host ? host : "");
    if (!id) f &= ~G_TLS_CERTIFICATE_BAD_IDENTITY;
    g_free(owned);
    return f;
}

static void cert_finalize(GObject *o)
{
    SieTlsCertificate *c = (SieTlsCertificate *)o;
    if (c->der) g_byte_array_unref(c->der);
    g_clear_object(&c->issuer);
    G_OBJECT_CLASS(sie_tls_certificate_parent_class)->finalize(o);
}

static void sie_tls_certificate_class_init(SieTlsCertificateClass *k)
{
    GObjectClass *o = G_OBJECT_CLASS(k);
    o->get_property = cert_get; o->set_property = cert_set; o->finalize = cert_finalize;
    k->parent.verify = cert_verify;
    g_object_class_override_property(o, C_CERT, "certificate");
    g_object_class_override_property(o, C_PEM, "certificate-pem");
    g_object_class_override_property(o, C_KEY, "private-key");
    g_object_class_override_property(o, C_KEY_PEM, "private-key-pem");
    g_object_class_override_property(o, C_ISSUER, "issuer");
}
static void sie_tls_certificate_init(SieTlsCertificate *c) { (void)c; }

/* ---- The connection. */
typedef struct SieTlsConnection {
    GTlsConnection parent;
    GIOStream *base;
    GInputStream *base_in, *in;
    GOutputStream *base_out, *out;
    GSocketConnectable *identity;
    GTlsDatabase *database;
    GTlsInteraction *interaction;
    GTlsCertificate *certificate, *peer;
    GTlsCertificateFlags peer_errors, validation_flags;
    gboolean require_close_notify, use_system_certdb;
    int rehandshake;                             /* (deprecated in GIO: kept, unused) */
    char **advertised;
    const char *negotiated, *cipher;
    GTlsProtocolVersion version;
    tls_t *t;
    int hs;                                      /* 0: not yet; 1: done; -1: failed */
    GError *hs_error;
    GMutex hs_lock, rd_lock, wr_lock;
    int rd_nb, wr_nb;                            /* the operation under way: blocking or not */
    GCancellable *rd_cancel, *wr_cancel;
    GError *rd_err, *wr_err;                     /* the base stream's errors, for the caller */
    GByteArray *pending;                         /* encrypted, not yet taken by the base stream */
    GSource *flusher;
} SieTlsConnection;
typedef struct { GTlsConnectionClass parent; } SieTlsConnectionClass;
static void conn_initable_init(GInitableIface *iface);
static void conn_client_init(GTlsClientConnectionInterface *iface);
G_DEFINE_TYPE_WITH_CODE(SieTlsConnection, sie_tls_connection, G_TYPE_TLS_CONNECTION,
                        G_IMPLEMENT_INTERFACE(G_TYPE_INITABLE, conn_initable_init)
                        G_IMPLEMENT_INTERFACE(G_TYPE_TLS_CLIENT_CONNECTION, conn_client_init))
enum { P_BASE = 1, P_SYSDB, P_CLOSE_NOTIFY, P_REHS, P_CERT, P_DB, P_INTERACTION, P_PEER, P_PEER_ERRORS,
       P_VALIDATION, P_IDENTITY, P_SSL3, P_CAS, P_ADVERTISED, P_NEGOTIATED, P_VERSION, P_CIPHER };

/* tls.c's transport: the base stream, blocking or not as the operation is */
static void flush_pending(SieTlsConnection *c, int blocking)
{
    while (c->pending->len && !c->wr_err) {
        GError *e = 0;
        gssize r = blocking ? g_output_stream_write(c->base_out, c->pending->data, c->pending->len, c->wr_cancel, &e)
                            : g_pollable_output_stream_write_nonblocking(G_POLLABLE_OUTPUT_STREAM(c->base_out),
                                                                         c->pending->data, c->pending->len, 0, &e);
        if (r < 0) {
            if (!blocking && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) { g_error_free(e); return; }
            c->wr_err = e;
            return;
        }
        g_byte_array_remove_range(c->pending, 0, (guint)r);
    }
}

static gboolean flush_later(GObject *base, gpointer data)   /* the base stream can take more (a pollable source's callback) */
{
    SieTlsConnection *c = data;
    g_mutex_lock(&c->wr_lock);
    c->wr_nb = 1;
    flush_pending(c, 0);
    gboolean again = c->pending->len && !c->wr_err;
    if (!again) c->flusher = 0;
    g_mutex_unlock(&c->wr_lock);
    return again;
}

long net_send(long s, const void *buf, size_t n)
{
    SieTlsConnection *c = (SieTlsConnection *)s;
    if (c->wr_err) return -EIO;
    g_byte_array_append(c->pending, buf, (guint)n);
    flush_pending(c, !c->wr_nb);
    if (c->pending->len && !c->wr_err && !c->flusher) {
        c->flusher = g_pollable_output_stream_create_source(G_POLLABLE_OUTPUT_STREAM(c->base_out), 0);
        g_source_set_callback(c->flusher, (GSourceFunc)(void (*)(void))flush_later, g_object_ref(c), g_object_unref);
        g_source_attach(c->flusher, g_main_context_get_thread_default());
        g_source_unref(c->flusher);
    }
    return c->wr_err ? -EIO : (long)n;
}

long net_recv(long s, void *buf, size_t n, unsigned timeout_ms)
{
    SieTlsConnection *c = (SieTlsConnection *)s;
    GError *e = 0;
    (void)timeout_ms;
    gssize r = c->rd_nb ? g_pollable_input_stream_read_nonblocking(G_POLLABLE_INPUT_STREAM(c->base_in), buf, n, 0, &e)
                        : g_input_stream_read(c->base_in, buf, n, c->rd_cancel, &e);
    if (r >= 0) return r;
    if (c->rd_nb && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) { g_error_free(e); return -EAGAIN; }
    if (!c->rd_err) c->rd_err = e; else g_error_free(e);
    return -EIO;
}

/* The handshake itself: tls.c's, then the chain checked. Blocking. */
static gboolean handshake_run(SieTlsConnection *c, GCancellable *cancel, GError **error)
{
    char *owned;
    const char *host = c->identity ? identity_host(c->identity, &owned) : (owned = 0);
    int flags = TLS_CHAIN;
    for (char **p = c->advertised; p && *p; p++) if (!strcmp(*p, "h2")) flags |= TLS_H2;
    long e;
    g_mutex_lock(&c->rd_lock); g_mutex_lock(&c->wr_lock);
    c->rd_nb = c->wr_nb = 0; c->rd_cancel = c->wr_cancel = cancel;
    c->t = tls_open((long)c, host ? host : "", flags, 0, &e);
    c->rd_cancel = c->wr_cancel = 0;
    GError *io = c->rd_err ? c->rd_err : c->wr_err;
    c->rd_err = c->wr_err = 0;
    g_mutex_unlock(&c->wr_lock); g_mutex_unlock(&c->rd_lock);
    if (!c->t) {
        g_free(owned);
        if (io) { g_propagate_error(error, io); return FALSE; }
        g_set_error(error, G_TLS_ERROR, e == -EIO ? G_TLS_ERROR_BAD_CERTIFICATE : G_TLS_ERROR_HANDSHAKE,
                    "TLS handshake failed (%ld)", e);
        return FALSE;
    }
    if (io) g_error_free(io);
    const uint8_t **certs;
    const size_t *lens;
    int n = tls_certs(c->t, &certs, &lens);
    GTlsCertificate *issuer = 0;
    for (int i = n - 1; i >= 0; i--) issuer = cert_new(certs[i], lens[i], issuer);
    c->peer = issuer;
    c->peer_errors = verify(certs, lens, n, host);
    g_free(owned);
    c->negotiated = tls_alpn(c->t);
    const char *cs = tls_cipher(c->t);           /* "TLS 1.3 TLS_AES_128_GCM_SHA256" */
    c->version = cs[6] == '3' ? G_TLS_PROTOCOL_VERSION_TLS_1_3 : G_TLS_PROTOCOL_VERSION_TLS_1_2;
    c->cipher = cs + 8;
    return TRUE;
}

/* Then, in the caller's thread: the certificate offered if it failed the
 * check, the properties announced. */
static gboolean handshake_end(SieTlsConnection *c, gboolean ok, GError *err, GError **error)
{
    g_mutex_lock(&c->hs_lock);
    if (c->hs) {                                 /* someone else finished it */
        ok = c->hs > 0;
        if (err) g_error_free(err);
        err = c->hs_error ? g_error_copy(c->hs_error) : 0;
        g_mutex_unlock(&c->hs_lock);
        if (err) g_propagate_error(error, err);
        return ok;
    }
    if (ok && c->peer_errors && !g_tls_connection_emit_accept_certificate(G_TLS_CONNECTION(c), c->peer, c->peer_errors)) {
        ok = FALSE;
        err = g_error_new_literal(G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE, "Unacceptable TLS certificate");
    }
    c->hs = ok ? 1 : -1;
    c->hs_error = err ? g_error_copy(err) : 0;
    g_mutex_unlock(&c->hs_lock);
    if (ok) {
        g_object_notify(G_OBJECT(c), "peer-certificate");
        g_object_notify(G_OBJECT(c), "peer-certificate-errors");
        g_object_notify(G_OBJECT(c), "negotiated-protocol");
        g_object_notify(G_OBJECT(c), "protocol-version");
        g_object_notify(G_OBJECT(c), "ciphersuite-name");
    }
    if (err) g_propagate_error(error, err);
    return ok;
}

static gboolean conn_handshake(GTlsConnection *tc, GCancellable *cancel, GError **error)
{
    SieTlsConnection *c = (SieTlsConnection *)tc;
    GError *err = 0;
    gboolean ok = c->hs || c->t ? TRUE : handshake_run(c, cancel, &err);
    return handshake_end(c, ok, err, error);
}

static void handshake_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    SieTlsConnection *c = src;
    GError *err = 0;
    (void)data;
    if (c->hs || c->t || handshake_run(c, cancel, &err)) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, err);
}

static void conn_handshake_async(GTlsConnection *tc, int prio, GCancellable *cancel, GAsyncReadyCallback cb, gpointer data)
{
    GTask *task = g_task_new(tc, cancel, cb, data);
    g_task_set_priority(task, prio);
    g_task_run_in_thread(task, handshake_thread);
    g_object_unref(task);
}

static gboolean conn_handshake_finish(GTlsConnection *tc, GAsyncResult *r, GError **error)
{
    GError *err = 0;
    gboolean ok = g_task_propagate_boolean(G_TASK(r), &err);
    return handshake_end((SieTlsConnection *)tc, ok, err, error);
}

static gboolean handshaken(SieTlsConnection *c, GCancellable *cancel, GError **error)
{
    if (c->hs > 0 && c->t) return TRUE;
    if (!c->hs) return conn_handshake(G_TLS_CONNECTION(c), cancel, error);
    if (c->hs_error) g_propagate_error(error, g_error_copy(c->hs_error));
    else g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Connection is closed");
    return FALSE;
}

/* Plaintext in and out */
static gssize conn_read(SieTlsConnection *c, void *buf, gsize n, int blocking, GCancellable *cancel, GError **error)
{
    if (!handshaken(c, cancel, error)) return -1;
    g_mutex_lock(&c->rd_lock);
    if (!blocking) g_mutex_lock(&c->wr_lock);    /* a key update answered: the writing side's */
    long r = -EPIPE;
    if (c->t) {
        c->rd_nb = !blocking; c->rd_cancel = cancel;
        if (!blocking) c->wr_nb = 1;
        r = tls_read(c->t, buf, n, 0);
        c->rd_cancel = 0;
    }
    GError *io = c->rd_err;
    c->rd_err = 0;
    if (!blocking) g_mutex_unlock(&c->wr_lock);
    g_mutex_unlock(&c->rd_lock);
    if (r >= 0) return r;
    if (io) { g_propagate_error(error, io); return -1; }
    if (r == -EAGAIN) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK, "Operation would block");
    else if (r == -ECONNRESET && !c->require_close_notify) return 0;       /* closed without close_notify */
    else if (r == -ECONNRESET) g_set_error_literal(error, G_TLS_ERROR, G_TLS_ERROR_EOF, "TLS connection closed unexpectedly");
    else if (r == -EPIPE) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Connection is closed");
    else g_set_error(error, G_TLS_ERROR, G_TLS_ERROR_MISC, "TLS error (%ld)", r);
    return -1;
}

static gssize conn_write(SieTlsConnection *c, const void *buf, gsize n, int blocking, GCancellable *cancel, GError **error)
{
    if (!handshaken(c, cancel, error)) return -1;
    g_mutex_lock(&c->wr_lock);
    c->wr_nb = !blocking; c->wr_cancel = cancel;
    long r = -EPIPE;
    if (c->t) {
        flush_pending(c, blocking);
        if (c->pending->len && !c->wr_err) r = -EAGAIN;       /* still the last record */
        else if (!c->wr_err) {
            if (!blocking) n = MIN(n, 16384);    /* one record waits at most */
            r = tls_write(c->t, buf, n);
        }
    }
    c->wr_cancel = 0;
    GError *io = c->wr_err;
    c->wr_err = 0;
    g_mutex_unlock(&c->wr_lock);
    if (io) { g_propagate_error(error, io); return -1; }
    if (r >= 0) return (gssize)n;
    if (r == -EAGAIN) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK, "Operation would block");
    else if (r == -EPIPE) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Connection is closed");
    else g_set_error(error, G_TLS_ERROR, G_TLS_ERROR_MISC, "TLS error (%ld)", r);
    return -1;
}

/* ---- The two streams: the connection's plaintext. They may outlive it
 * (libsoup keeps them): a weak reference, and "closed" once it is gone. */
typedef struct { GInputStream parent; GWeakRef c; } SieTlsInput;
typedef struct { GInputStreamClass parent; } SieTlsInputClass;
typedef struct { GOutputStream parent; GWeakRef c; } SieTlsOutput;
typedef struct { GOutputStreamClass parent; } SieTlsOutputClass;
static void in_pollable_init(GPollableInputStreamInterface *iface);
static void out_pollable_init(GPollableOutputStreamInterface *iface);
G_DEFINE_TYPE_WITH_CODE(SieTlsInput, sie_tls_input, G_TYPE_INPUT_STREAM,
                        G_IMPLEMENT_INTERFACE(G_TYPE_POLLABLE_INPUT_STREAM, in_pollable_init))
G_DEFINE_TYPE_WITH_CODE(SieTlsOutput, sie_tls_output, G_TYPE_OUTPUT_STREAM,
                        G_IMPLEMENT_INTERFACE(G_TYPE_POLLABLE_OUTPUT_STREAM, out_pollable_init))

static SieTlsConnection *conn_of(GWeakRef *w, GError **error)   /* a reference, or 0 and "closed" */
{
    SieTlsConnection *c = g_weak_ref_get(w);
    if (!c) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Connection is closed");
    return c;
}
#define IN_C(s) conn_of(&((SieTlsInput *)(s))->c, 0)
#define OUT_C(s) conn_of(&((SieTlsOutput *)(s))->c, 0)

static gssize in_read_any(gpointer s, void *buf, gsize n, int blocking, GCancellable *cancel, GError **error)
{
    SieTlsConnection *c = conn_of(&((SieTlsInput *)s)->c, error);
    gssize r = c ? conn_read(c, buf, n, blocking, cancel, error) : -1;
    if (c) g_object_unref(c);
    return r;
}
static gssize in_read(GInputStream *s, void *buf, gsize n, GCancellable *cancel, GError **error) { return in_read_any(s, buf, n, 1, cancel, error); }
static gssize in_read_nb(GPollableInputStream *s, void *buf, gsize n, GError **error) { return in_read_any(s, buf, n, 0, 0, error); }
static gboolean in_close(GInputStream *s, GCancellable *cancel, GError **error) { (void)s; (void)cancel; (void)error; return TRUE; }
static gboolean in_can_poll(GPollableInputStream *s)
{
    SieTlsConnection *c = IN_C(s);
    gboolean r = c && G_IS_POLLABLE_INPUT_STREAM(c->base_in) && g_pollable_input_stream_can_poll(G_POLLABLE_INPUT_STREAM(c->base_in));
    if (c) g_object_unref(c);
    return r;
}
static int in_pending(SieTlsConnection *c)      /* a read needs no network */
{
    int p = 0;
    if (g_mutex_trylock(&c->rd_lock)) { p = c->t && tls_pending(c->t); g_mutex_unlock(&c->rd_lock); }
    return p;
}
static gboolean in_readable(GPollableInputStream *s)
{
    SieTlsConnection *c = IN_C(s);
    gboolean r = !c || in_pending(c) || g_pollable_input_stream_is_readable(G_POLLABLE_INPUT_STREAM(c->base_in));
    if (c) g_object_unref(c);
    return r;                                    /* (gone: a read says so at once) */
}
static GSource *in_source(GPollableInputStream *s, GCancellable *cancel)
{
    SieTlsConnection *c = IN_C(s);
    GSource *child = !c || in_pending(c) ? g_timeout_source_new(0)
                                         : g_pollable_input_stream_create_source(G_POLLABLE_INPUT_STREAM(c->base_in), 0);
    GSource *src = g_pollable_source_new_full(s, child, cancel);
    g_source_unref(child);
    if (c) g_object_unref(c);
    return src;
}
static void in_pollable_init(GPollableInputStreamInterface *i)
{
    i->can_poll = in_can_poll; i->is_readable = in_readable; i->create_source = in_source; i->read_nonblocking = in_read_nb;
}
static void in_finalize(GObject *o) { g_weak_ref_clear(&((SieTlsInput *)o)->c); G_OBJECT_CLASS(sie_tls_input_parent_class)->finalize(o); }
static void sie_tls_input_class_init(SieTlsInputClass *k)
{
    k->parent.read_fn = in_read; k->parent.close_fn = in_close; G_OBJECT_CLASS(k)->finalize = in_finalize;
}
static void sie_tls_input_init(SieTlsInput *s) { g_weak_ref_init(&s->c, 0); }

static gssize out_write_any(gpointer s, const void *buf, gsize n, int blocking, GCancellable *cancel, GError **error)
{
    SieTlsConnection *c = conn_of(&((SieTlsOutput *)s)->c, error);
    gssize r = c ? conn_write(c, buf, n, blocking, cancel, error) : -1;
    if (c) g_object_unref(c);
    return r;
}
static gssize out_write(GOutputStream *s, const void *buf, gsize n, GCancellable *cancel, GError **error) { return out_write_any(s, buf, n, 1, cancel, error); }
static gssize out_write_nb(GPollableOutputStream *s, const void *buf, gsize n, GError **error) { return out_write_any(s, buf, n, 0, 0, error); }
static gboolean out_flush(GOutputStream *s, GCancellable *cancel, GError **error)
{
    SieTlsConnection *c = conn_of(&((SieTlsOutput *)s)->c, error);
    if (!c) return FALSE;
    g_mutex_lock(&c->wr_lock);
    c->wr_cancel = cancel;
    flush_pending(c, 1);
    c->wr_cancel = 0;
    GError *io = c->wr_err;
    c->wr_err = 0;
    g_mutex_unlock(&c->wr_lock);
    gboolean ok = io ? (g_propagate_error(error, io), FALSE) : g_output_stream_flush(c->base_out, cancel, error);
    g_object_unref(c);
    return ok;
}
static gboolean out_close(GOutputStream *s, GCancellable *cancel, GError **error) { (void)s; (void)cancel; (void)error; return TRUE; }
static gboolean out_can_poll(GPollableOutputStream *s)
{
    SieTlsConnection *c = OUT_C(s);
    gboolean r = c && G_IS_POLLABLE_OUTPUT_STREAM(c->base_out) && g_pollable_output_stream_can_poll(G_POLLABLE_OUTPUT_STREAM(c->base_out));
    if (c) g_object_unref(c);
    return r;
}
static gboolean out_writable(GPollableOutputStream *s)
{
    SieTlsConnection *c = OUT_C(s);
    gboolean r = !c || g_pollable_output_stream_is_writable(G_POLLABLE_OUTPUT_STREAM(c->base_out));
    if (c) g_object_unref(c);
    return r;
}
static GSource *out_source(GPollableOutputStream *s, GCancellable *cancel)
{
    SieTlsConnection *c = OUT_C(s);
    GSource *child = c ? g_pollable_output_stream_create_source(G_POLLABLE_OUTPUT_STREAM(c->base_out), 0) : g_timeout_source_new(0);
    GSource *src = g_pollable_source_new_full(s, child, cancel);
    g_source_unref(child);
    if (c) g_object_unref(c);
    return src;
}
static void out_pollable_init(GPollableOutputStreamInterface *i)
{
    i->can_poll = out_can_poll; i->is_writable = out_writable; i->create_source = out_source; i->write_nonblocking = out_write_nb;
}
static void out_finalize(GObject *o) { g_weak_ref_clear(&((SieTlsOutput *)o)->c); G_OBJECT_CLASS(sie_tls_output_parent_class)->finalize(o); }
static void sie_tls_output_class_init(SieTlsOutputClass *k)
{
    k->parent.write_fn = out_write; k->parent.flush = out_flush; k->parent.close_fn = out_close;
    G_OBJECT_CLASS(k)->finalize = out_finalize;
}
static void sie_tls_output_init(SieTlsOutput *s) { g_weak_ref_init(&s->c, 0); }

/* ---- The connection as a GObject. */
static GInputStream *conn_in(GIOStream *s) { return ((SieTlsConnection *)s)->in; }
static GOutputStream *conn_out(GIOStream *s) { return ((SieTlsConnection *)s)->out; }

static gboolean conn_close(GIOStream *s, GCancellable *cancel, GError **error)
{
    SieTlsConnection *c = (SieTlsConnection *)s;
    g_mutex_lock(&c->rd_lock); g_mutex_lock(&c->wr_lock);
    if (c->t) {                                  /* what waits, then close_notify */
        c->wr_nb = 0; c->wr_cancel = cancel;
        flush_pending(c, 1);
        tls_close(c->t);
        c->t = 0;
        c->wr_cancel = 0;
        g_clear_error(&c->wr_err);
    }
    if (c->flusher) { g_source_destroy(c->flusher); c->flusher = 0; }
    g_mutex_unlock(&c->wr_lock); g_mutex_unlock(&c->rd_lock);
    return g_io_stream_close(c->base, cancel, error);
}

static const char *conn_negotiated(GTlsConnection *tc) { return ((SieTlsConnection *)tc)->negotiated; }

static void conn_get(GObject *o, guint id, GValue *v, GParamSpec *ps)
{
    SieTlsConnection *c = (SieTlsConnection *)o;
    switch (id) {
    case P_BASE: g_value_set_object(v, c->base); break;
    case P_SYSDB: g_value_set_boolean(v, c->use_system_certdb); break;
    case P_CLOSE_NOTIFY: g_value_set_boolean(v, c->require_close_notify); break;
    case P_REHS: g_value_set_enum(v, c->rehandshake); break;
    case P_CERT: g_value_set_object(v, c->certificate); break;
    case P_DB: g_value_set_object(v, c->database); break;
    case P_INTERACTION: g_value_set_object(v, c->interaction); break;
    case P_PEER: g_value_set_object(v, c->peer); break;
    case P_PEER_ERRORS: g_value_set_flags(v, c->peer_errors); break;
    case P_VALIDATION: g_value_set_flags(v, c->validation_flags); break;
    case P_IDENTITY: g_value_set_object(v, c->identity); break;
    case P_SSL3: g_value_set_boolean(v, FALSE); break;
    case P_CAS: g_value_set_pointer(v, 0); break;
    case P_ADVERTISED: g_value_set_boxed(v, c->advertised); break;
    case P_NEGOTIATED: g_value_set_string(v, c->negotiated); break;
    case P_VERSION: g_value_set_enum(v, c->version); break;
    case P_CIPHER: g_value_set_string(v, c->cipher); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
    }
}

static void conn_set(GObject *o, guint id, const GValue *v, GParamSpec *ps)
{
    SieTlsConnection *c = (SieTlsConnection *)o;
    switch (id) {
    case P_BASE: g_set_object(&c->base, g_value_get_object(v)); break;
    case P_SYSDB: c->use_system_certdb = g_value_get_boolean(v); break;
    case P_CLOSE_NOTIFY: c->require_close_notify = g_value_get_boolean(v); break;
    case P_REHS: c->rehandshake = g_value_get_enum(v); break;
    case P_CERT: g_set_object(&c->certificate, g_value_get_object(v)); break;
    case P_DB: g_set_object(&c->database, g_value_get_object(v)); break;
    case P_INTERACTION: g_set_object(&c->interaction, g_value_get_object(v)); break;
    case P_VALIDATION: c->validation_flags = g_value_get_flags(v); break;
    case P_IDENTITY: g_set_object(&c->identity, g_value_get_object(v)); break;
    case P_SSL3: break;
    case P_ADVERTISED: g_strfreev(c->advertised); c->advertised = g_value_dup_boxed(v); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
    }
}

static gboolean conn_init(GInitable *i, GCancellable *cancel, GError **error)
{
    SieTlsConnection *c = (SieTlsConnection *)i;
    (void)cancel;
    if (!c->base) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No base stream"); return FALSE; }
    c->base_in = g_io_stream_get_input_stream(c->base);
    c->base_out = g_io_stream_get_output_stream(c->base);
    SieTlsInput *in = g_object_new(sie_tls_input_get_type(), 0);
    SieTlsOutput *out = g_object_new(sie_tls_output_get_type(), 0);
    g_weak_ref_set(&in->c, c); g_weak_ref_set(&out->c, c);
    c->in = G_INPUT_STREAM(in); c->out = G_OUTPUT_STREAM(out);
    return TRUE;
}
static void conn_initable_init(GInitableIface *iface) { iface->init = conn_init; }
static void conn_copy_session(GTlsClientConnection *a, GTlsClientConnection *b) { (void)a; (void)b; }  /* no resumption */
static void conn_client_init(GTlsClientConnectionInterface *iface) { iface->copy_session_state = conn_copy_session; }

static void conn_finalize(GObject *o)
{
    SieTlsConnection *c = (SieTlsConnection *)o;
    if (c->t) {                                  /* never closed: freed, nothing sent */
        g_clear_error(&c->wr_err);
        c->wr_err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CLOSED, "");
        tls_close(c->t);
    }
    g_clear_error(&c->wr_err); g_clear_error(&c->rd_err); g_clear_error(&c->hs_error);
    g_clear_object(&c->in); g_clear_object(&c->out);
    g_clear_object(&c->base); g_clear_object(&c->identity); g_clear_object(&c->database);
    g_clear_object(&c->interaction); g_clear_object(&c->certificate); g_clear_object(&c->peer);
    g_strfreev(c->advertised);
    g_byte_array_unref(c->pending);
    g_mutex_clear(&c->hs_lock); g_mutex_clear(&c->rd_lock); g_mutex_clear(&c->wr_lock);
    G_OBJECT_CLASS(sie_tls_connection_parent_class)->finalize(o);
}

static void sie_tls_connection_class_init(SieTlsConnectionClass *k)
{
    GObjectClass *o = G_OBJECT_CLASS(k);
    GIOStreamClass *io = G_IO_STREAM_CLASS(k);
    o->get_property = conn_get; o->set_property = conn_set; o->finalize = conn_finalize;
    io->get_input_stream = conn_in; io->get_output_stream = conn_out; io->close_fn = conn_close;
    k->parent.handshake = conn_handshake;
    k->parent.handshake_async = conn_handshake_async;
    k->parent.handshake_finish = conn_handshake_finish;
    k->parent.get_negotiated_protocol = conn_negotiated;
    static const char *const props[] = { 0, "base-io-stream", "use-system-certdb", "require-close-notify",
        "rehandshake-mode", "certificate", "database", "interaction", "peer-certificate", "peer-certificate-errors",
        "validation-flags", "server-identity", "use-ssl3", "accepted-cas", "advertised-protocols",
        "negotiated-protocol", "protocol-version", "ciphersuite-name" };
    for (guint i = 1; i < G_N_ELEMENTS(props); i++) g_object_class_override_property(o, i, props[i]);
}

static void sie_tls_connection_init(SieTlsConnection *c)
{
    g_mutex_init(&c->hs_lock); g_mutex_init(&c->rd_lock); g_mutex_init(&c->wr_lock);
    c->pending = g_byte_array_new();
    c->require_close_notify = TRUE;
    c->use_system_certdb = TRUE;
    c->validation_flags = G_TLS_CERTIFICATE_VALIDATE_ALL;
}

/* ---- The backend, made GIO's. */
typedef struct { GObject parent; } SieTlsBackend;
typedef struct { GObjectClass parent; } SieTlsBackendClass;
static void backend_iface_init(GTlsBackendInterface *iface);
G_DEFINE_TYPE_WITH_CODE(SieTlsBackend, sie_tls_backend, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_TLS_BACKEND, backend_iface_init))
static void sie_tls_backend_class_init(SieTlsBackendClass *k) { (void)k; }
static void sie_tls_backend_init(SieTlsBackend *b) { (void)b; }
static GType no_type(void) { return G_TYPE_INVALID; }   /* servers, file databases: not here */
static void backend_iface_init(GTlsBackendInterface *i)
{
    i->get_certificate_type = sie_tls_certificate_get_type;
    i->get_client_connection_type = sie_tls_connection_get_type;
    i->get_server_connection_type = no_type;
    i->get_file_database_type = no_type;
}

void g_io_sieos_tls_load(void)
{
    g_io_extension_point_register(G_TLS_BACKEND_EXTENSION_POINT_NAME);
    g_io_extension_point_implement(G_TLS_BACKEND_EXTENSION_POINT_NAME, sie_tls_backend_get_type(), "sieos", 100);
}
__attribute__((constructor)) static void load(void) { g_io_sieos_tls_load(); }
