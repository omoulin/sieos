/*
 * net.h - HTTPS requests. Linux: net_curl.c runs the curl program (its
 * settings, cookie included, go through a pipe, never the command line).
 * SIEOS: its own http.c will do.
 */
#ifndef NET_H
#define NET_H

#include <stddef.h>

#define SITE "https://www.churchofjesuschrist.org"

typedef struct {
    int status;                 /* the HTTP status; 0: no answer (see err) */
    char *body;
    size_t len;
    char *err;
} http_t;

int  http_do(const char *method, const char *url, const char *cookie, const char *body, http_t *out);
void http_free(http_t *h);
char *url_escape(const char *s);

#endif
