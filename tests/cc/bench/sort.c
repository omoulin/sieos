/* Quicksort of 5 million integers, 3 times. Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "../t.h"
static int v[5000000];
static void qs(int *a, long lo, long hi) {
    while (lo < hi) {
        int p = a[(lo + hi) / 2];
        long i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) { int t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { qs(a, lo, j); lo = i; } else { qs(a, i, hi); hi = j; }
    }
}
int main(void) {
    unsigned long x = 88172645463325252UL; long s = 0;
    for (int r = 0; r < 3; r++) {
        for (long i = 0; i < 5000000; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; v[i] = (int)x; }
        qs(v, 0, 5000000 - 1);
        s += v[0] + v[2500000] + v[4999999];
    }
    printf("%ld\n", s);
    return 0;
}
