/* Variable arguments. Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
struct S { long a, b; }; struct T { double x; long y; };
static long isum(int n, ...) { va_list ap; va_start(ap, n); long s = 0; for (int i = 0; i < n; i++) s += va_arg(ap, int); va_end(ap); return s; }
static double dsum(int n, ...) { va_list ap; va_start(ap, n); double s = 0; for (int i = 0; i < n; i++) s += va_arg(ap, double); va_end(ap); return s; }
static long mixed(const char *f, ...) {
    va_list ap; va_start(ap, f); long r = 0;
    for (; *f; f++) {
        if (*f == 'i') r += va_arg(ap, int);
        else if (*f == 'l') r += va_arg(ap, long) * 2;
        else if (*f == 'd') r += (long)(va_arg(ap, double) * 10);
        else if (*f == 's') { struct S s = va_arg(ap, struct S); r += s.a * 100 + s.b; }
        else if (*f == 't') { struct T t = va_arg(ap, struct T); r += (long)t.x + t.y; }
        else if (*f == 'p') r += strlen(va_arg(ap, char *));
    }
    va_end(ap); return r;
}
static void fmt(char *out, const char *f, ...) { va_list ap; va_start(ap, f); vsprintf(out, f, ap); va_end(ap); }
int main(void) {
    printf("%ld %ld\n", isum(3, 1, 2, 3), isum(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    printf("%.2f %.2f\n", dsum(2, 1.5, 2.5), dsum(10, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.5));
    struct S s = { 3, 4 }; struct T t = { 2.5, 9 };
    printf("%ld\n", mixed("ildsptiilldd", 1, 2L, 0.5, s, "four", t, 3, 4, 5L, 6L, 1.1, 2.2));
    char buf[64]; fmt(buf, "%s=%d,%.1f", "k", 42, 3.5); printf("%s\n", buf);
    return 0;
}
