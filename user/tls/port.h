/*
 * port.h - The few system services the TLS library needs.
 *
 * The library is built for SIEOS (libc.h) and, for testing, for the host
 * (-DTLS_HOSTED with the host C library).
 */
#ifndef TLS_PORT_H
#define TLS_PORT_H

#ifdef TLS_HOSTED
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
static inline long tls_now(void) { return (long)time(NULL); }
#else
#include "sieos.h"
static inline long tls_now(void) { return time(NULL); }
#endif

/* Fill buf from /dev/urandom; false if the device is missing. */
static inline bool tls_random(void *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return false;
    long got = read(fd, buf, n);
    close(fd);
    return got == (long)n;
}

#endif
