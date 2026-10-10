/* vla2.c - more variable length array cases.
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
struct S { int a; double b; };
void proto(int n, int a[*][*]);
void proto(int n, int a[n][n]) { a[n - 1][n - 1] = 42; }
static double tr(int n, double (*m)[n]) { double s = 0; for (int i = 0; i < n; i++) s += m[i][i]; return s; }
static int st(int n, int a[static n]) { return a[n - 1]; }
static int three(int x, int y, int z) {
    int c[x][y][z];
    int k = 0;
    for (int i = 0; i < x; i++) for (int j = 0; j < y; j++) for (int l = 0; l < z; l++) c[i][j][l] = k++;
    return c[x - 1][y - 1][z - 1] + (int)sizeof c[1] + (int)sizeof c[1][1];
}
static int side(int n) {
    int k = n;
    size_t s = sizeof(int[k++]);          /* a VLA type: its size expression runs */
    size_t t = sizeof(int[4]);
    int v[n]; size_t u = sizeof v[k++];   /* not a VLA: no evaluation */
    return (int)s + (int)t + (int)u + k * 1000;
}
static int structs(int n) {
    struct S s[n];
    for (int i = 0; i < n; i++) s[i] = (struct S){ i, i * 0.5 };
    __typeof__(s) *p = &s;
    return (int)((*p)[n - 1].b * 10) + (int)sizeof *p;
}
static int sw(int n, int sel) {
    int r = 0;
    switch (sel) {
    case 0: { int v[n]; v[0] = 7; r = v[0]; break; }
    case 1: for (;;) { int w[n * 2]; w[1] = 9; r = w[1]; break; } break;
    default: r = -1;
    }
    return r;
}
static long many(int n) {
    long acc = 0;
    for (int it = 0; it < 100000; it++) {
        long big[n];
        big[it % n] = it;
        acc += big[it % n];
        if (it & 1) continue;
    }
    return acc;
}
int main(void) {
    int n = 4;
    int a[n][n];
    proto(n, a);
    double m[3][3] = { { 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 } };
    int arr[5] = { 1, 2, 3, 4, 5 };
    printf("%d %g %d %d\n", a[3][3], tr(3, m), st(5, arr), three(2, 3, 4));
    printf("%d %d %d %ld\n", side(6), structs(5), sw(3, 0) + sw(3, 1) * 10 + sw(3, 2) * 100, many(1000));
    return 0;
}
