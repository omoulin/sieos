/* Pointers, arrays, strings, function pointers. Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static int apply(int (*f)(int, int), int a, int b) { return f(a, b); }
static void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }
static int sum(const int *p, int n) { int s = 0; while (n--) s += *p++; return s; }
int grid[3][4];
int main(void) {
    int arr[10];
    for (int i = 0; i < 10; i++) arr[i] = i * i;
    int *p = arr, *q = &arr[7];
    printf("%d %d %d %ld\n", *p, p[3], *(q - 2), (long)(q - p));
    printf("%d %d\n", sum(arr, 10), sum(arr + 5, 3));
    int x = 1, y = 2; swap(&x, &y); printf("%d %d\n", x, y);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) grid[i][j] = i * 10 + j;
    int (*row)[4] = grid + 1;
    printf("%d %d %d\n", grid[2][3], row[0][2], (*(row + 1))[1]);
    char s[] = "hello", *t = "world";
    printf("%s %s %d %c %d\n", s, t, (int)sizeof s, t[1], (int)strlen(t));
    s[0] = 'J'; printf("%s\n", s);
    int (*ops[2])(int, int) = { add, mul };
    printf("%d %d %d\n", ops[0](3, 4), ops[1](3, 4), apply(mul, 6, 7));
    char buf[32]; sprintf(buf, "%d-%s", 42, "x"); printf("%s\n", buf);
    const char *names[] = { "a", "bb", "ccc" };
    int tot = 0; for (int i = 0; i < 3; i++) tot += strlen(names[i]); printf("%d\n", tot);
    void *v = arr; int *ip = (int *)v + 2; printf("%d\n", *ip);
    long la[4] = { 10, 20, 30, 40 }; long *lp = la; lp += 2; printf("%ld %ld\n", *lp, lp[-1]);
    char *cp = (char *)la; printf("%d\n", cp[8]);
    int *np = 0; printf("%d %d\n", np == 0, !np);
    struct { int a[3]; } w = { { 4, 5, 6 } }; int *wp = w.a; printf("%d\n", wp[2]);
    return 0;
}
