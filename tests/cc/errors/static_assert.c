/* expect: 3:16: error: static assertion failed: "too small" */
/* Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
_Static_assert(sizeof(int) == 8, "too small");
