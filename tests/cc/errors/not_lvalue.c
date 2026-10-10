/* expect: 3:18: error: not assignable */
/* Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
int f(int a) { 3 = a; return a; }
