/* CRC-32 of 200 MB (table-driven). Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "../t.h"
static unsigned table[256];
static unsigned char buf[1 << 20];
static unsigned crc32(unsigned crc, const unsigned char *p, long n) {
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 255] ^ crc >> 8;
    return ~crc;
}
int main(void) {
    for (unsigned i = 0; i < 256; i++) { unsigned c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ c >> 1 : c >> 1; table[i] = c; }
    for (long i = 0; i < (long)sizeof buf; i++) buf[i] = i * 7 + (i >> 9);
    unsigned c = 0;
    for (int r = 0; r < 200; r++) c = crc32(c, buf, sizeof buf);
    printf("%08x\n", c);
    return 0;
}
