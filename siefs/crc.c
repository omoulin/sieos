/*
 * crc.c - CRC-32C (Castagnoli), the checksum of every SieFS block.
 *
 * "Slice by 8": eight 256-entry tables let us consume 8 bytes per step
 * instead of 1 (about 1-2 bytes per CPU cycle in plain C). x86 CPUs also
 * have a crc32 instruction for this polynomial; the tables keep the code
 * portable and free of SSE (the SIEOS kernel and servers use none).
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "siefs_int.h"

static uint32_t T[8][256];
static int ready;

static void init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? c >> 1 ^ 0x82F63B78 : c >> 1;   /* reflected polynomial */
        T[0][i] = c;
    }
    for (int i = 0; i < 256; i++)
        for (int k = 1; k < 8; k++) T[k][i] = T[k - 1][i] >> 8 ^ T[0][T[k - 1][i] & 0xFF];
    ready = 1;
}

uint32_t siefs_crc32c(uint32_t crc, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    if (!ready) init();
    crc = ~crc;
    for (; n && (uintptr_t)p & 7; n--) crc = crc >> 8 ^ T[0][(crc ^ *p++) & 0xFF];
    for (; n >= 8; n -= 8, p += 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        v ^= crc;
        crc = T[7][v & 0xFF] ^ T[6][v >> 8 & 0xFF] ^ T[5][v >> 16 & 0xFF] ^ T[4][v >> 24 & 0xFF] ^
              T[3][v >> 32 & 0xFF] ^ T[2][v >> 40 & 0xFF] ^ T[1][v >> 48 & 0xFF] ^ T[0][v >> 56];
    }
    for (; n; n--) crc = crc >> 8 ^ T[0][(crc ^ *p++) & 0xFF];
    return ~crc;
}
