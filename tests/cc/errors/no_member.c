/* expect: 4:36: error: no member named 'z' */
/* Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
struct P { int x, y; };
int f(struct P p) { return p.x + p.z; }
