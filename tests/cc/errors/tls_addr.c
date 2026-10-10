/* expect: 4:11: error: not a compile-time constant */
/* Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
_Thread_local int t;
int *p = &t;
