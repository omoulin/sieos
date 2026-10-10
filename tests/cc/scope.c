/* Scopes, shadowing, static locals, recursion, typedefs, enums, strings. Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
typedef int T;
static int depth(int n) { static int calls; calls++; return n ? depth(n - 1) : calls; }
static const char *names[] = { "zero", "one", "two" };
enum { A = 3, B = A * 2, C };
int x = 1;
int main(void) {
    printf("%d\n", x);
    { int x = 2; printf("%d\n", x); { T x = 3; printf("%d\n", x); } printf("%d\n", x); }
    printf("%d %d\n", x, depth(5));
    for (int x = 10; x < 12; x++) printf("%d ", x);
    printf("\n%d %d %d %s\n", A, B, C, names[2]);
    typedef long T; T big = 1L << 40; printf("%ld\n", big);
    const char *s = "tab\there\\ \"quoted\" \x41\101 \0hidden";
    printf("%s %d\n", s, (int)sizeof("ab" "cd"));
    printf("%d %d %d\n", 'a', '\n', '\x7f');
    int arr[3][2] = { 1, 2, 3, 4, 5, 6 }; printf("%d %d\n", arr[1][1], arr[2][0]);
    char *p = (char[]){ "compound" }; printf("%s\n", p + 3);
    int i = 0; switch (i) { default: printf("default "); /* fall */ case 1: printf("one\n"); }
    goto skip; printf("never\n"); skip: printf("skipped\n");
    unsigned u = -1; long l = u; unsigned long ul = (unsigned)-1; printf("%ld %lu %d\n", l, ul, (int)(u >> 31));
    printf("%ld %ld\n", -9223372036854775807L - 1, (-9223372036854775807L - 1) / 2);
    return 0;
}
