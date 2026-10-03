/* sys/cdefs.h - glibc's, as far as Chromium's code uses it. */
/*
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef _SYS_CDEFS_H
#define _SYS_CDEFS_H
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#define __THROW throw()
#else
#define __BEGIN_DECLS
#define __END_DECLS
#define __THROW
#endif
#define __P(args) args
#define __CONCAT(x, y) x##y
#define __STRING(x) #x
#define __wur __attribute__((__warn_unused_result__))
#define __nonnull(params) __attribute__((__nonnull__ params))
#define __attribute_malloc__ __attribute__((__malloc__))
#define __attribute_pure__ __attribute__((__pure__))
#define __attribute_const__ __attribute__((__const__))
#define __attribute_noinline__ __attribute__((__noinline__))
#define __always_inline __inline __attribute__((__always_inline__))
#endif
