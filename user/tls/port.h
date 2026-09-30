/*
 * port.h - The few system services the TLS library needs.
 *
 * The library is built for SIEOS (libc.h) and, for testing, for the host
 * (-DTLS_HOSTED with the host C library).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
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
static inline uint64_t tls_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
#else
#include "sieos.h"
static inline long tls_now(void) { return time(NULL); }
static inline uint64_t tls_ms(void) { return (uint64_t)uptime_ms(); }   /* monotonic milliseconds */
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
