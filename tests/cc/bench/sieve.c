/* Primes below 20 million, 5 times. Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "../t.h"
static char flag[20000000];
int main(void) {
    long count = 0;
    for (int r = 0; r < 5; r++) {
        memset(flag, 1, sizeof flag);
        count = 0;
        for (long i = 2; i < (long)sizeof flag; i++) {
            if (!flag[i]) continue;
            count++;
            for (long j = i * i; j < (long)sizeof flag; j += i) flag[j] = 0;
        }
    }
    printf("%ld\n", count);
    return 0;
}
