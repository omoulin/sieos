/* expect: 4:25: error: too few arguments */
/* Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
int g(int a, int b);
int f(void) { return g(1); }
