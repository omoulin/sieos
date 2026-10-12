/* calc.c - compiled on SIEOS by its own compiler (tools/cctest.py): integers
 * of every width, 128-bit division (sicc's runtime), floating point, a
 * struct, recursion, a table; each result checked against its known value. */
#include "mk.h"

typedef struct { int x, y; double w; } pt;
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static double area(const pt *p, int n)       /* the shoelace formula */
{
    double s = 0;
    for (int i = 0; i < n; i++) s += (double)p[i].x * p[(i + 1) % n].y - (double)p[(i + 1) % n].x * p[i].y;
    return s < 0 ? -s / 2 : s / 2;
}

int main(void)
{
    int bad = 0;
    unsigned __int128 big = (unsigned __int128)0x123456789abcdefULL * 0xfedcba987654321ULL;
    unsigned __int128 q = big / 1000000007ULL;
    unsigned long r = (unsigned long)(big % 1000000007ULL);
    pt sq[4] = { { 0, 0, 1 }, { 4, 0, 1 }, { 4, 3, 1 }, { 0, 3, 1 } };
    long long acc = 0;
    for (int i = 1; i <= 1000; i++) acc += (long long)i * i * i;
    double h = 0;
    for (int i = 1; i <= 10; i++) h += 1.0 / i;
    bad |= fib(20) != 6765;
    bad |= acc != 250500250000LL;
    bad |= area(sq, 4) != 12.0;
    bad |= (int)(h * 1000000) != 2928968;
    bad |= q * 1000000007ULL + r != big || r != 89593453ULL;
    printf("calc: fib %d, cubes %ld, area %d, harmonic*1e6 %d, 128-bit %s\n", fib(20), (long)acc,
           (int)area(sq, 4), (int)(h * 1000000), bad ? "WRONG" : "ok");
    return bad;
}
