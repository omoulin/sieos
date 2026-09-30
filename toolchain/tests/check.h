/*
 * check.h
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
// Minimal check helpers for the toolchain tests: print failures, return non-zero.
#pragma once
#include <cstdio>
static int t_fails;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); t_fails++; } } while (0)
#define DONE() (t_fails ? 1 : 0)
