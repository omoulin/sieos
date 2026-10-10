/* main.c - several sources on one command line: macros and statics stay per file.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#define X 1
int printf(const char *, ...);
int g(int);
static int calls;
static int f(void) { return calls++ ? 5 : 0; }
int main(void) { int x = f() ?: 7; int y = f() ?: 9; printf("%d %d %d %d\n", x, y, g(3), calls); return 0; }
