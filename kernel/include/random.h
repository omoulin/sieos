/*
 * random.h - Kernel random number generator.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_RANDOM_H
#define SIEOS_RANDOM_H

#include "kernel.h"

void random_init(void);
void random_add_entropy(uint64_t v);
void random_bytes(void *buf, size_t n);
bool random_hw_available(void);

#endif
