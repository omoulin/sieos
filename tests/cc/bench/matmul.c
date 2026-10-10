/* 300x300 matrix products, integers and doubles. Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "../t.h"
#define N 300
static int a[N][N], b[N][N], c[N][N];
static double x[N][N], y[N][N], z[N][N];
int main(void) {
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) { a[i][j] = i + j; b[i][j] = i - j; x[i][j] = i * 0.5; y[i][j] = j * 0.25; }
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++) {
            int aik = a[i][k]; double xik = x[i][k];
            for (int j = 0; j < N; j++) { c[i][j] += aik * b[k][j]; z[i][j] += xik * y[k][j]; }
        }
    long s = 0; double d = 0;
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) { s += c[i][j]; d += z[i][j]; }
    printf("%ld %.1f\n", s, d);
    return 0;
}
