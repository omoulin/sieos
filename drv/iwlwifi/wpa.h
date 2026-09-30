/*
 * wpa.h - WPA2-Personal's cryptography (wpa.c).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef WPA_H
#define WPA_H

#include "kernel.h"

void hmac_sha1(const uint8_t *key, size_t klen, const void *msg, size_t len, uint8_t out[20]);
void wpa_passphrase_pmk(const char *pass, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32]);
void wpa_prf(const uint8_t *key, size_t klen, const char *label, const uint8_t *data, size_t dlen, uint8_t *out,
             size_t olen);
bool wpa_aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t len, uint8_t *out);

#endif
