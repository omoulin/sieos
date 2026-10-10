/* expect: 3:55: error: a jump into the scope of a variable length array */
/* Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
int f(int n, int s){ switch (s) { case 0: { int v[n]; case 1: v[0] = 1; return v[0]; } } return 0; }
