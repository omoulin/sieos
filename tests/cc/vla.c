/* vla.c - variable length arrays.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"

static int sum2(int n, int m, int a[n][m]) {
    int s = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < m; j++) s += a[i][j] * (i + 1);
    return s + (int)sizeof(a[0]) + (int)sizeof(*a) / (int)sizeof(int);
}

static long sizes(int n) {
    char c[n];
    int i3[n][3];
    double d[2][n];
    typedef short row[n + 1];
    row r[2];
    n = 100;                      /* sizes were fixed at the declaration */
    return sizeof c * 1000000L + sizeof i3 * 10000 + sizeof d * 100 + sizeof r + sizeof(row);
}

static void *addr[64];
static int loop_sp(int k) {
    int depth = 0;
    for (int i = 0; i < 1000; i++) {
        char buf[k + i % 7];
        memset(buf, i, sizeof buf);
        if (i % 3 == 0) continue;
        if (i == 999) break;
        depth += buf[0] & 1;
    }
    /* The stack must not grow: address of a fresh VLA is stable. */
    for (int i = 0; i < 64; i++) { char v[32]; addr[i] = v; }
    int stable = 1;
    for (int i = 1; i < 64; i++) stable &= addr[i] == addr[0];
    return depth * 2 + stable;
}

static int with_goto(int n) {
    int count = 0;
    void *first = 0;
again:
    {
        int v[n];
        v[n - 1] = count;
        if (!first) first = v;
        if ((void *)v != first) return -1;
        count++;
        if (count < 50) goto again;
    }
    return count;
}

static int pointer_to_vla(int n) {
    int a[4][n];
    int (*p)[n] = a;
    for (int i = 0; i < 4; i++) for (int j = 0; j < n; j++) a[i][j] = i * 10 + j;
    p++;
    return p[1][n - 1] + (int)(sizeof *p) + (int)(&a[3][0] - &a[0][0]);
}

static int stmt_expr(int n) {
    return ({ int t[n]; for (int i = 0; i < n; i++) t[i] = i; t[n - 1]; });
}

static int recurse(int n) {
    if (!n) return 0;
    char b[n * 16];
    b[0] = (char)n;
    return b[0] + recurse(n - 1);
}

int main(void) {
    int n = 3, m = 5;
    int a[n][m];
    for (int i = 0; i < n; i++) for (int j = 0; j < m; j++) a[i][j] = i * m + j;
    printf("sum2 %d\n", sum2(n, m, a));
    printf("sizes %ld %ld\n", sizes(4), sizes(9));
    printf("loop %d\n", loop_sp(5));
    printf("goto %d\n", with_goto(7));
    printf("ptr %d\n", pointer_to_vla(6));
    printf("stmt %d\n", stmt_expr(8));
    printf("rec %d\n", recurse(30));
    int (*q)[m] = a;
    printf("q %d %zu\n", q[2][4], sizeof q[0]);
    return 0;
}
