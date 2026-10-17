/*
 * eld.h - The ELD ("EDID-like data", HDA 1.0a 7.3.3.34; CEA-861-D layout as
 * Linux's drm_edid_to_eld): what a monitor's EDID says about its sound. The
 * display driver (Atlas's igfx.c) builds it from the EDID and writes it into
 * the HDMI codec through its own registers; the audio server (audiod) reads
 * it back from the codec's pin (the monitor's name). Header only.
 *
 *   0  version 2 << 3         4  CEA revision << 5 | name length (MNL)
 *   2  baseline length / 4    5  SAD count << 4 | 1 << 2 for DisplayPort
 *   7  speaker allocation     16 manufacturer, product (EDID bytes 8-11)
 *   20 the name (MNL bytes), then the short audio descriptors (3 bytes each)
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#define ELD_MAX 84                               /* (the display's ELD buffer) */

/* e: the EDID (base block and one extension) -> the ELD's size (whole
 * dwords; eld zeroed up to ELD_MAX), 0 if the monitor has no sound (no CEA
 * block saying "basic audio"). dp: on DisplayPort. */
static inline int eld_build(const uint8_t *e, int dp, uint8_t *eld)
{
    const uint8_t *c = e + 128;
    if (!e[126] || c[0] != 2 || c[1] < 2 || !(c[3] & 0x40)) return 0;
    memset(eld, 0, ELD_MAX);
    int mnl = 0, ns = 0;
    for (int i = 54; i < 126 && !mnl; i += 18)   /* the monitor name descriptor */
        if (!e[i] && !e[i + 1] && e[i + 3] == 0xFC)
            for (; mnl < 13 && e[i + 5 + mnl] != '\n'; mnl++) eld[20 + mnl] = e[i + 5 + mnl];
    for (int i = 4, n; i < c[2] && i < 127; i += n + 1) {   /* the data blocks: audio (1), speakers (4) */
        n = c[i] & 31;
        for (int k = 1; c[i] >> 5 == 1 && k + 2 <= n && ns < 15; k += 3) memcpy(eld + 20 + mnl + 3 * ns++, c + i + k, 3);
        if (c[i] >> 5 == 4 && n) eld[7] = c[i + 1];
    }
    eld[0] = 2 << 3;
    eld[2] = (uint8_t)((16 + mnl + 3 * ns + 3) / 4);
    eld[4] = (uint8_t)(c[1] << 5 | mnl);
    eld[5] = (uint8_t)(ns << 4 | (dp ? 4 : 0));
    memcpy(eld + 16, e + 8, 4);
    return 4 + eld[2] * 4;
}

/* The name in an ELD's first n bytes -> name (up to 16 characters), 0; -1:
 * not an ELD. */
static inline int eld_name(const uint8_t *eld, int n, char *name)
{
    int mnl = n > 4 ? eld[4] & 31 : 99;
    if (eld[0] >> 3 != 2 || mnl > 16 || n < 20 + mnl) return -1;
    memcpy(name, eld + 20, mnl);
    name[mnl] = 0;
    return 0;
}
