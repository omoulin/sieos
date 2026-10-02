/*
 * printk.c - Formatted output for the kernel.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "kernel.h"
#include "smp.h"
#include "proc.h"

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
        bool zero = false, left = false, alt = false;
        int width = 0, lng = 0, prec = -1;
        for (;; fmt++) {
            if (*fmt == '0') zero = true;
            else if (*fmt == '-') left = true;
            else if (*fmt == '#') alt = true;       /* 0x before hexadecimal */
            else break;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {                          /* precision: at most so many characters of a string */
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9')
                prec = prec * 10 + (*fmt++ - '0');
        }
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
            if (alt && v) {
                out_char(&o, '0');
                out_char(&o, 'x');
                width = width > 2 ? width - 2 : 0;
            }
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
            if (prec >= 0 && len > prec)
                len = prec;
            if (!left)
                for (; width > len; width--)
                    out_char(&o, ' ');
            for (int i = 0; i < len; i++)
                out_char(&o, s[i]);
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

/* The kernel's messages, the last 64 KiB (/proc/msgbuf, dmesg). */
#define KLOG_SIZE 65536
static char klog[KLOG_SIZE];
static uint64_t klog_total;                          /* bytes ever written */

static struct spinlock klog_lock;

static void klog_add(const char *s, size_t n)
{
    spin_lock(&klog_lock);
    for (size_t i = 0; i < n; i++)
        klog[klog_total++ % KLOG_SIZE] = s[i];
    spin_unlock(&klog_lock);
}

size_t klog_size(void)
{
    return klog_total < KLOG_SIZE ? klog_total : KLOG_SIZE;
}

long klog_read(void *dst, uint64_t off, size_t n)
{
    size_t size = klog_size();
    if (off >= size)
        return 0;
    n = MIN(n, size - off);
    uint64_t first = klog_total - size;              /* the oldest byte kept */
    for (size_t i = 0; i < n; i++)
        ((char *)dst)[i] = klog[(first + off + i) % KLOG_SIZE];
    return n;
}

void kprintf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = MIN((size_t)n, sizeof(buf) - 1);
    klog_add(buf, len);
    console_write(buf, len);
}

void panic(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    cli();
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_panic();                       /* (this processor may have failed holding it) */
    console_set_color(15, 4);
    kprintf("\n*** KERNEL PANIC: %s (CPU %d, LWP %s)\n", buf, mycpu()->id, mycpu()->lwp ? mycpu()->lwp->name : "-");
    kprintf("    called from:");                 /* (the return addresses: build/kernel.nm resolves them) */
    uint64_t *fp = __builtin_frame_address(0);
    for (int i = 0; i < 16 && (uint64_t)fp >= 0xffff800000000000UL && !((uint64_t)fp & 7); i++) {
        kprintf(" %lx", fp[1]);
        uint64_t *next = (uint64_t *)fp[0];
        if (next <= fp)
            break;
        fp = next;
    }
    kprintf("\n");
    for (;;)
        hlt();
}
