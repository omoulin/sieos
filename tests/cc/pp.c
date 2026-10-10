/* The preprocessor. Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
#define SQ(x) ((x) * (x))
#define CAT(a, b) a ## b
#define STR(x) #x
#define XSTR(x) STR(x)
#define VER 3
#define LOG(fmt, ...) printf("log: " fmt "\n", ##__VA_ARGS__)
#define COUNT(...) count_args(0, ##__VA_ARGS__)
#define OPT(a, ...) a __VA_OPT__(+ __VA_ARGS__)
#define EMPTY
#define F(x) x + 1
#define G F(F(2))
#if VER > 2 && defined(VER) && !defined(NOPE)
#define OK 1
#elif VER == 2
#define OK 2
#else
#define OK 3
#endif
#ifdef NOPE
#error should not be here
#endif
static int count_args(int z, ...) { return z; }
int main(void) {
    int CAT(my, var) = 5;
    printf("%d %d %s %s\n", SQ(3 + 1), myvar, STR(a + b "q"), XSTR(VER));
    LOG("no args");
    LOG("two %d %s", 2, "x");
    printf("%d %d %d\n", OK, G, OPT(10) + OPT(1, 2));
    printf("%d %s %d\n", __LINE__, __FILE__ + 0 ? "file" : "", COUNT());
#if (1 ? 2 : 3) == 2 && (0x10 >> 2) == 4 && -1 < 0 && 'A' == 65
    printf("if ok\n");
#endif
    printf("%s\n", STR(EMPTY));
    return 0;
}
