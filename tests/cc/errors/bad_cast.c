/* expect: 4:29: error: invalid cast */
/* Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
struct S { int a; };
int f(int a) { struct S s = (struct S)a; return s.a; }
