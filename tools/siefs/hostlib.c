/*
 * hostlib.c - What the SieFS library takes from SIEOS's lib/string.c
 * beyond the C library, for the host tools and tests: erasing a secret,
 * and comparing two in constant time (lib/aead.c uses them).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stddef.h>
#include <stdint.h>

void wipe(void *p, size_t n)
{
    volatile uint8_t *q = p;
    while (n--) *q++ = 0;
}

int ct_equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= x[i] ^ y[i];
    return d == 0;
}
