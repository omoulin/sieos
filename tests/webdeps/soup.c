/* pkg-config: libsoup-3.0 gio-tls-sieos */
/*
 * wtest-soup PLAIN TLS H2 SELF (make webdeps-test): libsoup as WebKit uses it,
 * asynchronously, against the host's servers (tests/webdeps/servers.py, the
 * four ports): HTTP/1.1; HTTPS through GIO's TLS on SIEOS's tls.c (the test
 * CA in /etc/ssl/roots, TLS 1.3, ALPN), a 1 MiB download and a 256 KiB
 * upload; HTTP/2 (nghttp2) chosen by ALPN; a self-signed certificate refused,
 * then accepted through "accept-certificate"; the public suffix list
 * (libpsl); cookies kept in SQLite across sessions.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <libsoup/soup.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails;
static void check(const char *what, int ok) { printf("wtest soup: %s: %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }

static GMainLoop *loop;
static void sent(GObject *s, GAsyncResult *r, gpointer data)
{
    GError **err = data;
    GBytes *b = soup_session_send_and_read_finish(SOUP_SESSION(s), r, err);
    g_object_set_data_full(G_OBJECT(s), "body", b, b ? (GDestroyNotify)g_bytes_unref : 0);
    g_main_loop_quit(loop);
}
/* A request sent and its answer read, as WebKit does: asynchronously */
static GBytes *fetch(SoupSession *s, SoupMessage *m, GError **err)
{
    soup_session_send_and_read_async(s, m, G_PRIORITY_DEFAULT, 0, sent, err);
    g_main_loop_run(loop);
    GBytes *b = g_object_get_data(G_OBJECT(s), "body");
    return b ? g_bytes_ref(b) : 0;
}

static int is(GBytes *b, const char *text) { return b && g_bytes_get_size(b) == strlen(text) && !memcmp(g_bytes_get_data(b, 0), text, strlen(text)); }
static gboolean accept_cert(SoupMessage *m, GTlsCertificate *c, GTlsCertificateFlags e, gpointer data)
{
    (void)m; (void)c;
    *(GTlsCertificateFlags *)data = e;
    return TRUE;
}

int main(int argc, char **argv)
{
    if (argc != 5) { printf("wtest soup: usage: wtest-soup PLAIN TLS H2 SELF (ports)\nwtest soup: FAIL\n"); return 2; }
    char url[4][64];
    for (int i = 0; i < 4; i++) snprintf(url[i], sizeof url[i], "%s://10.0.2.2:%s", i ? "https" : "http", argv[i + 1]);
    loop = g_main_loop_new(0, FALSE);
    check("GIO's TLS backend is SIEOS's", g_tls_backend_supports_tls(g_tls_backend_get_default()) &&
                                          !strcmp(G_OBJECT_TYPE_NAME(g_tls_backend_get_default()), "SieTlsBackend"));
    SoupSession *s = soup_session_new();
    GError *err = 0;
    char u[96];

    SoupMessage *m = 0;
    GBytes *b = 0;
    for (int i = 0; i < 20 && !b; i++) {        /* (the network may still be starting: DHCP) */
        if (i) { g_clear_object(&m); g_clear_error(&err); sleep(1); }
        m = soup_message_new("GET", strcat(strcpy(u, url[0]), "/small"));
        b = fetch(s, m, &err);
    }
    check("HTTP/1.1", is(b, "hello from the host") && soup_message_get_status(m) == 200);
    g_clear_pointer(&b, g_bytes_unref); g_clear_object(&m); g_clear_error(&err);

    m = soup_message_new("GET", strcat(strcpy(u, url[1]), "/small"));
    b = fetch(s, m, &err);
    check("HTTPS", is(b, "hello from the host"));
    check("HTTPS: TLS 1.3, the certificate checked", soup_message_get_tls_protocol_version(m) == G_TLS_PROTOCOL_VERSION_TLS_1_3 &&
          soup_message_get_tls_peer_certificate(m) && !soup_message_get_tls_peer_certificate_errors(m));
    check("HTTPS: HTTP/1.1 by ALPN", soup_message_get_http_version(m) == SOUP_HTTP_1_1);
    g_clear_pointer(&b, g_bytes_unref); g_clear_object(&m); g_clear_error(&err);

    m = soup_message_new("GET", strcat(strcpy(u, url[1]), "/big"));
    b = fetch(s, m, &err);
    int ok = b && g_bytes_get_size(b) == 1 << 20;
    const guint8 *p = b ? g_bytes_get_data(b, 0) : 0;
    for (size_t i = 0; ok && i < 1 << 20; i++) ok = p[i] == i * 7 % 251;
    check("HTTPS: 1 MiB", ok);
    g_clear_pointer(&b, g_bytes_unref); g_clear_object(&m); g_clear_error(&err);

    guint8 *up = g_malloc(256 << 10);
    unsigned long sum = 0;
    for (size_t i = 0; i < 256 << 10; i++) sum += up[i] = (guint8)(i * 13);
    GBytes *upb = g_bytes_new_take(up, 256 << 10);
    char want[64];
    snprintf(want, sizeof want, "got %d sum %lu", 256 << 10, sum);
    m = soup_message_new("POST", strcat(strcpy(u, url[1]), "/echo"));
    soup_message_set_request_body_from_bytes(m, "application/octet-stream", upb);
    b = fetch(s, m, &err);
    check("HTTPS: 256 KiB up", is(b, want));
    g_clear_pointer(&b, g_bytes_unref); g_clear_object(&m); g_clear_error(&err);

    m = soup_message_new("GET", strcat(strcpy(u, url[2]), "/"));
    b = fetch(s, m, &err);
    ok = b && g_bytes_get_size(b) == 3 * 6667 && !memcmp(g_bytes_get_data(b, 0), "h2 h2 ", 6);
    check("HTTP/2 by ALPN", ok && soup_message_get_http_version(m) == SOUP_HTTP_2_0);
    g_clear_pointer(&b, g_bytes_unref); g_clear_object(&m); g_clear_error(&err);

    m = soup_message_new("POST", strcat(strcpy(u, url[2]), "/"));
    soup_message_set_request_body_from_bytes(m, "application/octet-stream", upb);
    b = fetch(s, m, &err);
    check("HTTP/2: 256 KiB up", is(b, want));
    g_clear_pointer(&b, g_bytes_unref); g_clear_object(&m); g_clear_error(&err);
    g_bytes_unref(upb);

    m = soup_message_new("GET", strcat(strcpy(u, url[3]), "/small"));
    b = fetch(s, m, &err);
    check("a self-signed certificate refused", !b && g_error_matches(err, G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE));
    g_clear_pointer(&b, g_bytes_unref); g_clear_object(&m); g_clear_error(&err);

    GTlsCertificateFlags seen = 0;
    m = soup_message_new("GET", strcat(strcpy(u, url[3]), "/small"));
    g_signal_connect(m, "accept-certificate", G_CALLBACK(accept_cert), &seen);
    b = fetch(s, m, &err);
    check("... then accepted", is(b, "hello from the host") && seen == G_TLS_CERTIFICATE_UNKNOWN_CA &&
                               soup_message_get_tls_peer_certificate_errors(m) == G_TLS_CERTIFICATE_UNKNOWN_CA);
    g_clear_pointer(&b, g_bytes_unref); g_clear_object(&m); g_clear_error(&err);
    g_object_unref(s);

    const char *base = soup_tld_get_base_domain("www.example.co.uk", 0);
    check("public suffixes", base && !strcmp(base, "example.co.uk") && soup_tld_domain_is_public_suffix("co.uk") &&
                             !soup_tld_domain_is_public_suffix("example.com"));

    unlink("/tmp/cookies.db");
    GUri *site = g_uri_parse("https://www.example.org/", G_URI_FLAGS_NONE, 0);
    SoupCookieJar *j = soup_cookie_jar_db_new("/tmp/cookies.db", FALSE);
    soup_cookie_jar_set_cookie(j, site, "session=abc123; Max-Age=3600; Secure");
    g_object_unref(j);
    j = soup_cookie_jar_db_new("/tmp/cookies.db", FALSE);
    char *c = soup_cookie_jar_get_cookies(j, site, FALSE);
    check("cookies kept in SQLite", c && !strcmp(c, "session=abc123"));
    g_free(c);
    g_object_unref(j);
    g_uri_unref(site);

    printf("wtest soup: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
