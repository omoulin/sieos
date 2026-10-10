/* other.c - the second source (see main.c).
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#ifdef X
#error X leaked
#endif
static int calls = 10;
int g(int a) { return a ?: calls; }
