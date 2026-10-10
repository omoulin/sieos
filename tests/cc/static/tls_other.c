/* tls_other.c - thread-local variables defined apart from tls.c: reached there
 * through initial-exec code, which the static link turns into local-exec.
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only */
_Thread_local int other = 77;
_Thread_local long other_zero[2];
