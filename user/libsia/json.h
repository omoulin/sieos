/*
 * json.h - JSON values (RFC 8259) and a growable string buffer.
 */
#ifndef SIA_JSON_H
#define SIA_JSON_H

#include "../tls/port.h"

/* ---- string buffer ---- */
struct sbuf {
    char *s;
    size_t len, cap;
};
void sb_init(struct sbuf *b);
void sb_free(struct sbuf *b);
void sb_putn(struct sbuf *b, const char *s, size_t n);
void sb_puts(struct sbuf *b, const char *s);
void sb_putc(struct sbuf *b, char c);
void sb_printf(struct sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_json_str(struct sbuf *b, const char *s);       /* "quoted, escaped" */
void sb_json_strn(struct sbuf *b, const char *s, size_t n);

/* ---- values ---- */
enum json_type { JSON_NULL, JSON_FALSE, JSON_TRUE, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

struct json {
    enum json_type type;
    char *str;                   /* JSON_STRING: UTF-8 text; JSON_NUMBER: literal text */
    long num;                    /* JSON_NUMBER: integer part */
    int n;                       /* array/object size */
    char **keys;                 /* object keys */
    struct json **items;
};

struct json *json_parse(const char *text, size_t len);   /* NULL on syntax error */
void json_free(struct json *v);
struct json *json_get(const struct json *obj, const char *key);           /* NULL if missing */
struct json *json_at(const struct json *arr, int i);
const char *json_str(const struct json *v);                               /* NULL unless string */
const char *json_get_str(const struct json *obj, const char *key);
void json_write(struct sbuf *b, const struct json *v);                    /* re-serialise */

#endif
