/*
 * mk/zlib.h - Compression for git's objects and packs (lib/zlib.c): zlib
 * streams (RFC 1950) of deflate data (RFC 1951), in memory, in one call.
 * Checked against the host's zlib (tests/crypto).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Inflate a zlib stream (in, n bytes) into out (cap bytes) -> the bytes
 * produced, or -1 for bad data. *used: the stream's length in `in` (its
 * checksum included); 0 if out filled up before the stream's end (then the
 * first cap bytes are there: enough to read a header). */
long zinflate(const void *in, size_t n, void *out, size_t cap, size_t *used);
/* Deflate n bytes into out (ZBOUND(n) bytes) -> the stream's length.
 * Fixed Huffman codes and LZ77 matches: small code; C source shrinks to
 * 46% of its size (zlib -9: 32%). */
#define ZBOUND(n) ((n) + (n) / 8 + 16)
size_t zdeflate(const void *in, size_t n, void *out);
/* CRC-32 (ISO 3309, as zlib and git's pack indexes use it): crc32(0, p, n). */
uint32_t crc32(uint32_t crc, const void *p, size_t n);
