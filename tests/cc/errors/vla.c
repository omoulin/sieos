/* expect: 3:15: error: a jump into the scope of a variable length array */
/* Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
int f(int n){ goto in; { int v[n]; in: v[0] = 1; return v[0]; } }
