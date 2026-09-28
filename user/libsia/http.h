/*
 * http.h - Minimal HTTP/1.1 client over TCP or TLS 1.3.
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

#endif
