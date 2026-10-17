/*
 * json.c - JSON reader and writer (json.h). Recursive descent; strings are
 * decoded to UTF-8 (\uXXXX with surrogate pairs included).
 */
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p, *end; int depth; } rd_t;

static void ws(rd_t *r) { while (r->p < r->end && (*r->p == ' ' || *r->p == '\n' || *r->p == '\r' || *r->p == '\t')) r->p++; }

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = p[i];
        v = v << 4 | (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1000);
    }
    return v;
}

static char *str(rd_t *r)
{
    if (r->p >= r->end || *r->p != '"') return NULL;
    r->p++;
    buf_t b = {0};
    while (r->p < r->end && *r->p != '"') {
        const char *s = r->p;
        while (r->p < r->end && *r->p != '"' && *r->p != '\\') r->p++;
        buf_add(&b, s, r->p - s);
        if (r->p >= r->end || *r->p == '"') break;
        r->p++;                                  /* the backslash */
        if (r->p >= r->end) break;
        char e = *r->p++;
        switch (e) {
        case 'n': buf_putc(&b, '\n'); break;
        case 't': buf_putc(&b, '\t'); break;
        case 'r': buf_putc(&b, '\r'); break;
        case 'b': buf_putc(&b, '\b'); break;
        case 'f': buf_putc(&b, '\f'); break;
        case 'u': {
            if (r->end - r->p < 4) goto bad;
            int cp = hex4(r->p);
            r->p += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && r->end - r->p >= 6 && r->p[0] == '\\' && r->p[1] == 'u') {
                int lo = hex4(r->p + 2);
                if (lo >= 0xdc00 && lo < 0xe000) { cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); r->p += 6; }
            }
            if (cp < 0) goto bad;
            char o[4];
            buf_add(&b, o, utf8_enc(o, cp));
            break;
        }
        default: buf_putc(&b, e);
        }
    }
    if (r->p >= r->end) goto bad;
    r->p++;
    return buf_take(&b);
bad:
    buf_free(&b);
    return NULL;
}

static jnode *value(rd_t *r);

static jnode *container(rd_t *r, int obj)
{
    jnode *j = json_new(obj ? J_OBJ : J_ARR);
    r->p++;
    ws(r);
    if (r->p < r->end && *r->p == (obj ? '}' : ']')) { r->p++; return j; }
    for (;;) {
        char *key = NULL;
        ws(r);
        if (obj) {
            if (!(key = str(r))) goto bad;
            ws(r);
            if (r->p >= r->end || *r->p++ != ':') { free(key); goto bad; }
        }
        jnode *v = value(r);
        if (!v) { free(key); goto bad; }
        v->key = key;
        if (j->last) j->last->next = v; else j->kid = v;
        j->last = v;
        ws(r);
        if (r->p < r->end && *r->p == ',') { r->p++; continue; }
        if (r->p < r->end && *r->p == (obj ? '}' : ']')) { r->p++; return j; }
        goto bad;
    }
bad:
    json_free(j);
    return NULL;
}

static jnode *value(rd_t *r)
{
    ws(r);
    if (r->p >= r->end || ++r->depth > 200) return NULL;
    jnode *j = NULL;
    char c = *r->p;
    if (c == '{' || c == '[') j = container(r, c == '{');
    else if (c == '"') {
        char *s = str(r);
        if (s) { j = json_new(J_STR); j->s = s; }
    } else if (!strncmp(r->p, "true", 4) || !strncmp(r->p, "false", 5)) {
        j = json_new(J_BOOL);
        j->num = c == 't';
        r->p += c == 't' ? 4 : 5;
    } else if (!strncmp(r->p, "null", 4)) {
        j = json_new(J_NULL);
        r->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *e;
        j = json_new(J_NUM);
        j->num = strtod(r->p, &e);
        r->p = e;
    }
    r->depth--;
    return j;
}

jnode *json_parse(const char *text, size_t n)
{
    rd_t r = { text, text + n, 0 };
    jnode *j = value(&r);
    ws(&r);
    if (j && r.p != r.end) { json_free(j); return NULL; }
    return j;
}

void json_free(jnode *j)
{
    while (j) {
        jnode *next = j->next;
        json_free(j->kid);
        free(j->key);
        free(j->s);
        free(j);
        j = next;
    }
}

jnode *json_get(const jnode *o, const char *key)
{
    if (!o || o->t != J_OBJ) return NULL;
    for (jnode *k = o->kid; k; k = k->next)
        if (k->key && !strcmp(k->key, key)) return k;
    return NULL;
}

jnode *json_path(const jnode *j, const char *path)
{
    char part[128];
    while (j && *path) {
        const char *dot = strchr(path, '.');
        size_t n = dot ? (size_t)(dot - path) : strlen(path);
        if (n >= sizeof part) return NULL;
        memcpy(part, path, n);
        part[n] = 0;
        j = json_get(j, part);
        path += n + (dot != NULL);
    }
    return (jnode *)j;
}

const char *json_str(const jnode *o, const char *key)
{
    jnode *k = json_get(o, key);
    return k && k->t == J_STR ? k->s : NULL;
}
double json_num(const jnode *o, const char *key, double d)
{
    jnode *k = json_get(o, key);
    if (!k) return d;
    if (k->t == J_NUM || k->t == J_BOOL) return k->num;
    if (k->t == J_STR) return atof(k->s);
    return d;
}
int json_count(const jnode *a)
{
    int n = 0;
    if (a && (a->t == J_ARR || a->t == J_OBJ))
        for (jnode *k = a->kid; k; k = k->next) n++;
    return n;
}

jnode *json_new(jtype_t t) { jnode *j = xcalloc(1, sizeof *j); j->t = t; return j; }
jnode *json_mkstr(const char *s) { jnode *j = json_new(J_STR); j->s = xstrdup(s ? s : ""); return j; }
jnode *json_mknum(double v) { jnode *j = json_new(J_NUM); j->num = v; return j; }

jnode *json_clone(const jnode *j)
{
    if (!j) return NULL;
    jnode *c = json_new(j->t);
    c->s = xstrdup(j->s);
    c->num = j->num;
    for (jnode *k = j->kid; k; k = k->next) json_add(c, k->key, json_clone(k));
    return c;
}

void json_add(jnode *p, const char *key, jnode *kid)
{
    free(kid->key);
    kid->key = key ? xstrdup(key) : NULL;
    kid->next = NULL;
    if (p->last) p->last->next = kid; else p->kid = kid;
    p->last = kid;
}

void json_del(jnode *o, const char *key)
{
    jnode *prev = NULL;
    for (jnode *k = o->kid; k; prev = k, k = k->next) {
        if (!k->key || strcmp(k->key, key)) continue;
        if (prev) prev->next = k->next; else o->kid = k->next;
        if (o->last == k) o->last = prev;
        k->next = NULL;
        json_free(k);
        return;
    }
}

void json_set(jnode *o, const char *key, jnode *kid)
{
    json_del(o, key);
    json_add(o, key, kid);
}

void json_write_str(buf_t *b, const char *s)
{
    buf_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) buf_printf(b, "\\u%04x", *p);
            else buf_putc(b, *p);
        }
    }
    buf_putc(b, '"');
}

void json_write(buf_t *b, const jnode *j)
{
    if (!j) { buf_puts(b, "null"); return; }
    switch (j->t) {
    case J_NULL: buf_puts(b, "null"); break;
    case J_BOOL: buf_puts(b, j->num ? "true" : "false"); break;
    case J_NUM:
        if (j->num == (double)(long long)j->num) buf_printf(b, "%lld", (long long)j->num);
        else buf_printf(b, "%.17g", j->num);
        break;
    case J_STR: json_write_str(b, j->s); break;
    case J_ARR:
    case J_OBJ:
        buf_putc(b, j->t == J_OBJ ? '{' : '[');
        for (jnode *k = j->kid; k; k = k->next) {
            if (k != j->kid) buf_putc(b, ',');
            if (j->t == J_OBJ) { json_write_str(b, k->key); buf_putc(b, ':'); }
            json_write(b, k);
        }
        buf_putc(b, j->t == J_OBJ ? '}' : ']');
        break;
    }
}
