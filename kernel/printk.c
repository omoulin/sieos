/*
 * printk.c - Formatted output for the kernel.
 */
#include "kernel.h"

struct outbuf {
    char *buf;
    size_t size;
    size_t len;
};

static void out_char(struct outbuf *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void out_num(struct outbuf *o, uint64_t v, int base, bool neg, int width,
                    bool zero, bool left, bool upper)
{
    char tmp[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    int len = n + (neg ? 1 : 0);
    if (!left && !zero)
        for (; width > len; width--)
            out_char(o, ' ');
    if (neg)
        out_char(o, '-');
    if (!left && zero)
        for (; width > len; width--)
            out_char(o, '0');
    while (n)
        out_char(o, tmp[--n]);
    if (left)
        for (; width > len; width--)
            out_char(o, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct outbuf o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_char(&o, *fmt);
            continue;
        }
        fmt++;
        bool zero = false, left = false;
        int width = 0, lng = 0;
        for (;; fmt++) {
            if (*fmt == '0') zero = true;
            else if (*fmt == '-') left = true;
            else break;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') {
            lng++;
            fmt++;
        }
        if (*fmt == 'z') {
            lng = 1;
            fmt++;
        }
        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = lng ? va_arg(ap, long) : va_arg(ap, int);
            out_num(&o, v < 0 ? -(uint64_t)v : (uint64_t)v, 10, v < 0, width, zero, left, false);
            break;
        }
        case 'u': {
            uint64_t v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            out_num(&o, v, 10, false, width, zero, left, false);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            out_num(&o, v, 16, false, width, zero, left, *fmt == 'X');
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)va_arg(ap, void *);
            out_char(&o, '0');
            out_char(&o, 'x');
            out_num(&o, v, 16, false, 16, true, false, false);
            break;
        }
        case 'c':
            out_char(&o, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = strlen(s);
            if (!left)
                for (; width > len; width--)
                    out_char(&o, ' ');
            while (*s)
                out_char(&o, *s++);
            if (left)
                for (; width > len; width--)
                    out_char(&o, ' ');
            break;
        }
        case '%':
            out_char(&o, '%');
            break;
        case 0:
            fmt--;
            break;
        default:
            out_char(&o, '%');
            out_char(&o, *fmt);
        }
    }
    if (size)
        buf[o.len < size ? o.len : size - 1] = 0;
    return o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void kprintf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_write(buf, MIN((size_t)n, sizeof(buf) - 1));
}

void panic(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    cli();
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_set_color(15, 4);
    kprintf("\n*** KERNEL PANIC: %s\n", buf);
    for (;;)
        hlt();
}
