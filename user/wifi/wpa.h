/*
 * wpa.h - WPA2-PSK on the host (wpa.c), for a Wi-Fi chip whose firmware does
 * not run the handshake itself (the Intel AX210, iwl.c): the passphrase to
 * the key (PBKDF2), the 4-way and group handshakes (EAPOL-Key frames, their
 * MICs, the pairwise keys, the wrapped group key), and the RSN element.
 * CCMP and PSK only (no TKIP, no SAE/WPA3, no management frame protection).
 * No SIEOS calls: tests/net/crypto.c checks it on the host.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t pmk[32], ptk[48];          /* ptk: KCK 16, KEK 16, TK 16 */
    uint8_t anonce[32], snonce[32];    /* snonce: random, set by the caller before joining */
    uint8_t replay[8];                 /* the last replay counter accepted */
    uint8_t aa[6], spa[6];             /* the access point, us */
    const uint8_t *ap_rsn;             /* the AP's RSN element (beacon): msg 3 must carry the same */
    uint8_t gtk[32];
    int gtklen, gtkid, have_replay;
} wpa_t;
enum { WPA_PTK = 1, WPA_GTK = 2 };     /* wpa_rx's *done: keys to install now */

/* Our RSN element: CCMP group and pairwise, PSK (also sent in the association request). */
extern const uint8_t wpa_ie[22];
/* The pairwise master key from the passphrase and the network's name. */
void wpa_pmk(uint8_t pmk[32], const char *pass, const char *ssid);
/* 1 if an AP's RSN element (id, length, body) is one we can join (CCMP, PSK). */
int  wpa_rsn_ok(const uint8_t *ie);
/* An EAPOL-Key frame received (from the 802.1X header): the answer into out
 * (from the 802.1X header) -> its length; 0 nothing to send; -1 refused (bad
 * MIC, replay, ...). *done: WPA_PTK after message 3 (install the pairwise then
 * the group key, after sending the answer), WPA_GTK after a group rekey. */
int  wpa_rx(wpa_t *w, const uint8_t *in, int len, uint8_t *out, int *done);
/* The first element with this id in a list of elements, or 0. */
const uint8_t *ie_find(const uint8_t *ie, int len, int id);
