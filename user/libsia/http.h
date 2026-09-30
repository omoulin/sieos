/*
 * http.h - Minimal HTTP/1.1 client over TCP or TLS 1.3.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIA_HTTP_H
#define SIA_HTTP_H

#include "json.h"

struct url {
    bool https;
    char host[256];
    int port;
    char path[1024];             /* path and query, starting with '/' */
};

bool url_parse(const char *s, struct url *u);

/*
 * Send one request ("Connection: close") and collect the response body.
 * extra_headers: zero or more "Name: value\r\n" lines.
 * Returns the HTTP status, or -1 with err set.
 */
int http_request(const char *method, const struct url *u, const char *extra_headers, const char *body,
                 size_t bodylen, struct sbuf *resp, char *err, size_t errlen, int timeout_ms);

/*
 * The same, but a 200 response's body is handed to sink as it arrives
 * (de-chunked) instead of being collected; other responses are collected
 * in resp as before.  sink returns false to stop reading (the request is
 * then abandoned and -1 returned with err "interrupted").
 */
typedef bool (*http_sink)(void *ctx, const char *data, size_t n);
int http_request_stream(const char *method, const struct url *u, const char *extra_headers, const char *body,
                        size_t bodylen, http_sink sink, void *ctx, struct sbuf *resp, char *err, size_t errlen,
                        int timeout_ms);

#endif
