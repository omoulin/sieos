/*
 * ptest-stdio (make posix-test): the C library itself on SIEOS: printf
 * of integers, doubles and strings; numbers parsed; sorting; strings;
 * the environment and the arguments; malloc, small and large; exit status.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int fails;
static void check(const char *what, int ok)
{
    printf("ptest stdio: %s: %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(int argc, char **argv)
{
    char b[256];
    snprintf(b, sizeof b, "%d %5.2f %s %x %lld %e", -42, 3.14159, "text", 255, 1LL << 40, 6.02e23);
    check("printf", !strcmp(b, "-42  3.14 text ff 1099511627776 6.020000e+23"));
    snprintf(b, sizeof b, "%.17g %g %a", 0.1, 1e300 * 10, 1.0);
    check("doubles printed exactly", !strcmp(b, "0.10000000000000001 1e+301 0x1p+0"));
    check("numbers parsed", strtod("2.5e-3", 0) == 2.5e-3 && strtol("-0x7f", 0, 16) == -127 && atoi("123") == 123);
    check("maths", fabs(sin(M_PI / 6) - 0.5) < 1e-15 && fabs(sqrt(2.0) * sqrt(2.0) - 2) < 1e-15 && pow(2, 10) == 1024);
    int v[100];
    for (int i = 0; i < 100; i++) v[i] = (i * 37) % 100;
    qsort(v, 100, sizeof v[0], cmp);
    int sorted = 1;
    for (int i = 0; i < 100; i++) sorted &= v[i] == i;
    check("qsort", sorted);
    check("strings", strlen("hello") == 5 && !strncmp("abcdef", "abcxyz", 3) && strstr("haystack", "st") && strchr("abc", 'c'));
    setlocale(LC_ALL, "C.UTF-8");
    wchar_t w[8];
    check("UTF-8 to wide characters", mbstowcs(w, "h\xc3\xa9!", 8) == 3 && w[1] == 0xe9);
    check("the environment", getenv("PATH") && !strcmp(getenv("PATH"), "/bin") && !getenv("NOPE"));
    check("the arguments", argc == 3 && !strcmp(argv[1], "one") && !strcmp(argv[2], "two"));
    char *small = malloc(100), *big = malloc(64 << 20);
    int okm = small && big;
    if (big) { memset(big, 7, 64 << 20); okm &= big[(64 << 20) - 1] == 7; }
    free(small); free(big);
    check("malloc: 100 bytes, 64 MiB", okm);
    char *huge = calloc(1, 1UL << 30);             /* given on first touch: costs nothing until used */
    check("calloc 1 GiB, untouched", huge != 0);
    free(huge);
    printf("ptest stdio: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
