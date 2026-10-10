/* expect: 3:33: error: a thread-local variable in a block must be static or extern */
/* Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
int f(void) { _Thread_local int x = 1; return x; }
