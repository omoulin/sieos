/*
 * mphalport.h - What MicroPython asks of the system, beyond py/mphal.h's
 * functions (main.c): input() and the prompt read whole typed lines.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "py/misc.h"
int sieos_readline(vstr_t *line, const char *prompt);
#define mp_hal_readline sieos_readline
static inline void mp_hal_set_interrupt_char(int c) { (void)c; }
void sieos_flush(void);                 /* the console output so far, now (main.c) */
