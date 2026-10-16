/*
 * mpconfigport.h - What MicroPython (the Python interpreter of /bin/python)
 * includes on SIEOS: the core and the "extra features" level, floats as
 * doubles, big integers, and the modules SIEOS adds (os, socket, ssl,
 * hashlib, in this folder). The rest of MicroPython's options stay off.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdint.h>
#include <stdlib.h>                    /* (malloc, free: the heap grows and shrinks by areas) */

#define MICROPY_CONFIG_ROM_LEVEL          (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)
#define MICROPY_FLOAT_IMPL                (MICROPY_FLOAT_IMPL_DOUBLE)
#define MICROPY_LONGINT_IMPL              (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_FLOAT_USE_NATIVE_FLT16    (0)     /* (struct "e": in software) */
#define MP_SSIZE_MAX                      (0x7fffffffffffffffL)
#define MICROPY_OPT_COMPUTED_GOTO         (1)
#define MICROPY_ERROR_REPORTING           (MICROPY_ERROR_REPORTING_DETAILED)
#define MICROPY_USE_INTERNAL_PRINTF       (0)     /* (SIEOS's printf: the console) */
#define MICROPY_USE_INTERNAL_ERRNO        (1)     /* (the POSIX numbers, as SIEOS's) */
#define MICROPY_ALLOC_PATH_MAX            (512)
#define MICROPY_EPOCH_IS_1970             (1)

/* Memory (main.c): a 1 MiB heap to start with, more taken from malloc as
 * needed (and given back); Python's frames on a 256 KiB stack of their own. */
#define MICROPY_ENABLE_GC                 (1)
#define MICROPY_GC_SPLIT_HEAP             (1)
#define MICROPY_GC_SPLIT_HEAP_AUTO        (1)
#define MICROPY_STACK_CHECK               (1)
#define MICROPY_STACK_CHECK_MARGIN        (8192)
#define MICROPY_ENABLE_PYSTACK            (1)     /* (Python frames there: sicc has no alloca) */

/* Files: import reads .py files from SieFS (main.c); no VFS layer. */
#define MICROPY_ENABLE_EXTERNAL_IMPORT    (1)
#define MICROPY_HELPER_REPL               (1)
#define MICROPY_REPL_AUTO_INDENT          (0)     /* (lines come typed whole, from the terminal) */
#define MICROPY_REPL_EMACS_KEYS           (0)
#define MICROPY_KBD_EXCEPTION             (1)     /* (Ctrl-C: SIGINT -> KeyboardInterrupt, main.c) */
#define MICROPY_ENABLE_SCHEDULER          (0)

/* sys */
#define MICROPY_PY_SYS_PLATFORM           "sieos"
#define MICROPY_PY_SYS_PATH               (1)
#define MICROPY_PY_SYS_ARGV               (1)
#define MICROPY_PY_SYS_EXIT               (1)
#define MICROPY_PY_SYS_STDFILES           (1)
#define MICROPY_PY_SYS_STDIO_BUFFER       (0)

/* The modules: MicroPython's own (json, re, binascii, random, time, struct,
 * collections, errno, math, heapq, io...) and SIEOS's (os, socket, ssl, hashlib). */
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_TIME_TIME_NS      (1)
#define MICROPY_PY_TIME_INCLUDEFILE       "sieostime.h"
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC  (sieos_random_u32())
#define MICROPY_PY_CMATH                  (0)
#define MICROPY_PY_OS                     (0)     /* (ours: modos.c) */
#define MICROPY_PY_HASHLIB                (0)     /* (ours: modhashlib.c, on lib/sha2.c) */
#define MICROPY_PY_ASYNCIO                (0)
#define MICROPY_PY_SELECT                 (0)
#define MICROPY_PY_UCTYPES                (0)
#define MICROPY_PY_DEFLATE                (0)
#define MICROPY_PY_FRAMEBUF               (0)
#define MICROPY_PY_PLATFORM               (0)
#define MICROPY_PY_MICROPYTHON_RINGIO     (0)

typedef long mp_off_t;
uint32_t sieos_random_u32(void);
#define MP_STATE_PORT MP_STATE_VM
