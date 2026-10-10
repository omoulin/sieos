/* stddef.h - sicc's freestanding header. Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#ifndef __SICC_STDDEF_H
#define __SICC_STDDEF_H
typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef int wchar_t;
typedef struct { long long __ll; long double __ld; } max_align_t;
#define NULL ((void *)0)
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
