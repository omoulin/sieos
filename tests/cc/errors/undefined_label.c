/* expect: 3:28: error: label 'nowhere' is not defined */
/* Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
int f(int x) { if (x) goto nowhere; return 0; }
