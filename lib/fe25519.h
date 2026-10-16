/*
 * fe25519.h - Arithmetic modulo p = 2^255 - 19, shared by X25519 and
 * Ed25519, which live on the same field: five 51-bit limbs per number
 * (lib/x25519.c). For lib/ only.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

typedef uint64_t fe[5];
void fe_load(fe r, const uint8_t b[32]);          /* 32 little-endian bytes (the top bit ignored) */
void fe_store(uint8_t b[32], const fe a);         /* fully reduced */
void fe_add(fe r, const fe a, const fe b);
void fe_sub(fe r, const fe a, const fe b);
void fe_mul(fe r, const fe a, const fe b);
void fe_pow(fe r, const fe z, int top, uint32_t holes);
void fe_cswap(fe a, fe b, uint64_t bit);          /* swap if bit is 1, in constant time */
#define fe_inv(r, z) fe_pow(r, z, 254, 1 << 2 | 1 << 4)   /* z^(p-2) = 1/z */
