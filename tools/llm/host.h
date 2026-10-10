/*
 * host.h - The engine's environment for host tools (host.c).
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdio.h>
#include "../../llm/llm.h"

typedef struct { FILE *f; uint64_t size; llm_env_t env; } host_t;
int host_env(host_t *h, const char *path, int threads);
