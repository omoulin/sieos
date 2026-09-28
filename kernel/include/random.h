/*
 * random.h - Kernel random number generator.
 */
#ifndef SIEOS_RANDOM_H
#define SIEOS_RANDOM_H

#include "kernel.h"

void random_init(void);
void random_add_entropy(uint64_t v);
void random_bytes(void *buf, size_t n);
bool random_hw_available(void);

#endif
