/*
 * neon_dot.c - neon.c again, with the dot product instructions (sdot,
 * ARMv8.2: the Pi 5). Built with -march=armv8.2-a+dotprod; used only when
 * the CPU has them (pick_kernels).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#define NEON_SDOT
#include "neon.c"
