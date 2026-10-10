/* tls_gcc.c - a thread-local variable defined by the host compiler (for tls.c).
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
__thread int other_tls = 100;
int bump_other(void) { return ++other_tls; }
