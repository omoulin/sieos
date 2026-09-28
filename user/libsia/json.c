/*
 * json.c - JSON parser/serialiser and string buffer.
 */
#include "json.h"

/* ---------------- string buffer ---------------- */

void sb_init(struct sbuf *b)
{
    b->s = NULL;
    b->len = b->cap = 0;
    sb_putn(b, "", 0);
}

void sb_free(struct sbuf *b)
{
    free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}

void sb_putn(struct sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + n + 1)
            nc *= 2;
        char *p = realloc(b->s, nc);
        if (!p)
            return;
        b->s = p;
        b->cap = nc;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void sb_puts(struct sbuf *b, const char *s)
{
    sb_putn(b, s, strlen(s));
}

void sb_putc(struct sbuf *b, char c)
{
    sb_putn(b, &c, 1);
}

void sb_printf(struct sbuf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_putn(b, tmp, n < (int)sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

void sb_json_strn(struct sbuf *b, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    sb_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        case '\b': sb_puts(b, "\\b"); break;
        case '\f': sb_puts(b, "\\f"); break;
        default:
            if (c < 0x20 || c == 0x7F) {
                char e[7] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0 };
                sb_puts(b, e);
            } else {
                sb_putc(b, (char)c);
            }
        }
    }
    sb_putc(b, '"');
}

void sb_json_str(struct sbuf *b, const char *s)
{
    sb_json_strn(b, s ? s : "", s ? strlen(s) : 0);
}

/* ---------------- parser ---------------- */

struct parser {
    const char *p, *end;
    int depth;
};

static void skip_ws(struct parser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static struct json *new_value(enum json_type t)
{
    struct json *v = calloc(1, sizeof(*v));
    if (v)
        v->type = t;
    return v;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool parse_hex4(struct parser *ps, unsigned *out)
{
    if (ps->end - ps->p < 4)
        return false;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(ps->p[i]);
        if (h < 0)
            return false;
        v = v << 4 | (unsigned)h;
    }
    ps->p += 4;
    *out = v;
    return true;
}

static void put_utf8(struct sbuf *b, unsigned cp)
{
    if (cp < 0x80) {
        sb_putc(b, (char)cp);
    } else if (cp < 0x800) {
        sb_putc(b, (char)(0xC0 | cp >> 6));
        sb_putc(b, (char)(0x80 | (cp & 63)));
    } else if (cp < 0x10000) {
        sb_putc(b, (char)(0xE0 | cp >> 12));
        sb_putc(b, (char)(0x80 | ((cp >> 6) & 63)));
        sb_putc(b, (char)(0x80 | (cp & 63)));
    } else {
        sb_putc(b, (char)(0xF0 | cp >> 18));
        sb_putc(b, (char)(0x80 | ((cp >> 12) & 63)));
        sb_putc(b, (char)(0x80 | ((cp >> 6) & 63)));
        sb_putc(b, (char)(0x80 | (cp & 63)));
    }
}

static char *parse_string(struct parser *ps)
{
    if (ps->p >= ps->end || *ps->p != '"')
        return NULL;
    ps->p++;
    struct sbuf b;
    sb_init(&b);
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if (c != '\\') {
            sb_putc(&b, c);
            continue;
        }
        if (ps->p >= ps->end)
            break;
        c = *ps->p++;
        switch (c) {
        case 'n': sb_putc(&b, '\n'); break;
        case 't': sb_putc(&b, '\t'); break;
        case 'r': sb_putc(&b, '\r'); break;
        case 'b': sb_putc(&b, '\b'); break;
        case 'f': sb_putc(&b, '\f'); break;
        case 'u': {
            unsigned cp;
            if (!parse_hex4(ps, &cp)) {
                sb_free(&b);
                return NULL;
            }
            if (cp >= 0xD800 && cp < 0xDC00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned lo;
                ps->p += 2;
                if (parse_hex4(ps, &lo) && lo >= 0xDC00 && lo < 0xE000)
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            put_utf8(&b, cp);
            break;
        }
        default: sb_putc(&b, c);
        }
    }
    if (ps->p >= ps->end) {
        sb_free(&b);
        return NULL;
    }
    ps->p++;
    return b.s;
}

static struct json *parse_value(struct parser *ps);

static bool add_item(struct json *v, char *key, struct json *item)
{
    struct json **items = realloc(v->items, (v->n + 1) * sizeof(*items));
    if (!items)
        return false;
    v->items = items;
    if (v->type == JSON_OBJECT) {
        char **keys = realloc(v->keys, (v->n + 1) * sizeof(*keys));
        if (!keys)
            return false;
        v->keys = keys;
        keys[v->n] = key;
    }
    items[v->n++] = item;
    return true;
}

static struct json *parse_value(struct parser *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end || ++ps->depth > 64)
        return NULL;
    struct json *v = NULL;
    char c = *ps->p;
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        char close = obj ? '}' : ']';
        v = new_value(obj ? JSON_OBJECT : JSON_ARRAY);
        ps->p++;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == close) {
            ps->p++;
        } else {
            for (;;) {
                char *key = NULL;
                skip_ws(ps);
                if (obj) {
                    key = parse_string(ps);
                    skip_ws(ps);
                    if (!key || ps->p >= ps->end || *ps->p != ':') {
                        free(key);
                        goto bad;
                    }
                    ps->p++;
                }
                struct json *item = parse_value(ps);
                if (!item || !add_item(v, key, item)) {
                    free(key);
                    json_free(item);
                    goto bad;
                }
                skip_ws(ps);
                if (ps->p < ps->end && *ps->p == ',') {
                    ps->p++;
                    continue;
                }
                if (ps->p < ps->end && *ps->p == close) {
                    ps->p++;
                    break;
                }
                goto bad;
            }
        }
    } else if (c == '"') {
        v = new_value(JSON_STRING);
        if (!(v->str = parse_string(ps)))
            goto bad;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        const char *start = ps->p;
        if (*ps->p == '-')
            ps->p++;
        while (ps->p < ps->end && ((*ps->p >= '0' && *ps->p <= '9') || *ps->p == '.' || *ps->p == 'e' ||
                                   *ps->p == 'E' || *ps->p == '+' || *ps->p == '-'))
            ps->p++;
        v = new_value(JSON_NUMBER);
        size_t n = ps->p - start;
        v->str = malloc(n + 1);
        memcpy(v->str, start, n);
        v->str[n] = 0;
        long num = 0;
        const char *q = start[0] == '-' ? start + 1 : start;
        while (*q >= '0' && *q <= '9' && q < ps->p)
            num = num * 10 + (*q++ - '0');
        v->num = start[0] == '-' ? -num : num;
    } else if (ps->end - ps->p >= 4 && !memcmp(ps->p, "true", 4)) {
        v = new_value(JSON_TRUE);
        ps->p += 4;
    } else if (ps->end - ps->p >= 5 && !memcmp(ps->p, "false", 5)) {
        v = new_value(JSON_FALSE);
        ps->p += 5;
    } else if (ps->end - ps->p >= 4 && !memcmp(ps->p, "null", 4)) {
        v = new_value(JSON_NULL);
        ps->p += 4;
    }
    ps->depth--;
    return v;
bad:
    json_free(v);
    return NULL;
}

struct json *json_parse(const char *text, size_t len)
{
    struct parser ps = { text, text + len, 0 };
    struct json *v = parse_value(&ps);
    skip_ws(&ps);
    if (v && ps.p != ps.end) {
        json_free(v);
        return NULL;
    }
    return v;
}

void json_free(struct json *v)
{
    if (!v)
        return;
    for (int i = 0; i < v->n; i++) {
        json_free(v->items[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->str);
    free(v);
}

struct json *json_get(const struct json *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT)
        return NULL;
    for (int i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key))
            return obj->items[i];
    return NULL;
}

struct json *json_at(const struct json *arr, int i)
{
    if (!arr || arr->type != JSON_ARRAY || i < 0 || i >= arr->n)
        return NULL;
    return arr->items[i];
}

const char *json_str(const struct json *v)
{
    return v && v->type == JSON_STRING ? v->str : NULL;
}

const char *json_get_str(const struct json *obj, const char *key)
{
    return json_str(json_get(obj, key));
}

void json_write(struct sbuf *b, const struct json *v)
{
    if (!v) {
        sb_puts(b, "null");
        return;
    }
    switch (v->type) {
    case JSON_NULL: sb_puts(b, "null"); break;
    case JSON_TRUE: sb_puts(b, "true"); break;
    case JSON_FALSE: sb_puts(b, "false"); break;
    case JSON_NUMBER: sb_puts(b, v->str); break;
    case JSON_STRING: sb_json_str(b, v->str); break;
    case JSON_ARRAY:
    case JSON_OBJECT:
        sb_putc(b, v->type == JSON_ARRAY ? '[' : '{');
        for (int i = 0; i < v->n; i++) {
            if (i)
                sb_putc(b, ',');
            if (v->type == JSON_OBJECT) {
                sb_json_str(b, v->keys[i]);
                sb_putc(b, ':');
            }
            json_write(b, v->items[i]);
        }
        sb_putc(b, v->type == JSON_ARRAY ? ']' : '}');
        break;
    }
}
