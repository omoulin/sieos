/* Initializers of globals and locals. Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
struct Item { const char *name; int qty; double price; };
struct Item items[] = { { "apple", 3, 0.5 }, { .name = "pear", .price = 1.25 }, [3] = { "plum", 7, 2 } };
int nums[10] = { 1, 2, [7] = 8, 9 };
char str[] = "abc", str2[8] = "xy";
const char *ptrs[] = { "one", "two", str + 1 };
int *pn = &nums[7];
long diff = (char *)&nums[5] - (char *)&nums[0];
struct { int a; int b[3]; } nested = { 1, { 2, 3 } };
static int counter(void) { static int c = 10; return c++; }
union { int i; char c[4]; } un = { 0x01020304 };
double dd[] = { 1.5, -2, 1e3 };
int (*fp)(const char *, ...) = printf;
int main(void) {
    for (int i = 0; i < 4; i++) printf("%s %d %.2f\n", items[i].name ? items[i].name : "-", items[i].qty, items[i].price);
    for (int i = 0; i < 10; i++) printf("%d ", nums[i]);
    printf("\n%s %s %d %d %s %s %d %ld\n", str, str2, (int)sizeof str, (int)sizeof items, ptrs[1], ptrs[2], *pn, diff);
    printf("%d %d %d %d\n", nested.a, nested.b[0], nested.b[1], nested.b[2]);
    int c1 = counter(); int c2 = counter(); int c3 = counter(); printf("%d %d %d\n", c1, c2, c3);
    printf("%d %.1f %d\n", un.c[0], dd[2], (int)(sizeof dd / sizeof dd[0]));
    fp("via pointer\n");
    int loc[5] = { [1] = 4, [3] = 6 }; struct Item it = { "x" };
    printf("%d %d %d %d %s %d\n", loc[0], loc[1], loc[3], loc[4], it.name, it.qty);
    char lstr[10] = "hi"; printf("%s %d\n", lstr, lstr[5]);
    int m[2][3] = { { 1, 2 }, { 4 } }; printf("%d %d %d\n", m[0][1], m[1][0], m[1][2]);
    return 0;
}
