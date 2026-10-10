/* Statements: loops, switch, goto, conditions. Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
static int classify(int x) {
    switch (x) {
    case 0: return 10;
    case 1: case 2: return 20;
    case 3: x += 100; /* fall through */
    case 4: return x;
    case 10 ... 20: return 30;
    case -5: return -50;
    default: return -1;
    }
}
static const char *dense(int x) {
    switch (x) { case 1: return "one"; case 2: return "two"; case 3: return "three"; case 4: return "four";
                 case 5: return "five"; case 7: return "seven"; default: return "?"; }
}
static long sparse(long x) { switch (x) { case 1000000: return 1; case -1: return 2; case 0x7fffffffffffL: return 3; } return 0; }
static int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
int main(void) {
    for (int i = -6; i < 23; i += 3) printf("%d:%d ", i, classify(i));
    printf("\n");
    for (int i = 0; i < 9; i++) printf("%s ", dense(i));
    printf("%ld %ld %ld %ld\n", sparse(1000000), sparse(-1), sparse(0x7fffffffffffL), sparse(5));
    int s = 0, i = 0;
    while (i < 100) { i++; if (i % 3 == 0) continue; if (i > 50) break; s += i; }
    printf("%d %d\n", s, i);
    do { s--; } while (s > 1000);
    printf("%d\n", s);
    int n = 0;
again:
    n++;
    if (n < 5) goto again;
    printf("%d %d\n", n, fact(10));
    int a = 3, b = 0;
    printf("%d %d %d %d\n", a && b, a || b, a && !b, (a > 2) ? (b ? 1 : 2) : 3);
    int cnt = 0;
    for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) { if (y > x) break; cnt += x * y; }
    printf("%d\n", cnt);
    int k = 0;
    for (;;) { if (++k == 7) break; }
    printf("%d\n", k);
    int z = (a = 5, b = a * 2, a + b);
    printf("%d\n", z);
    return 0;
}
