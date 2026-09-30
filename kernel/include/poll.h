/*
 * poll.h
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_POLL_H
#define SIEOS_POLL_H

#include "kernel.h"
#include "abi.h"

void poll_wakeup(void);                       /* something may have become ready */
long sys_poll(struct pollfd *fds, int nfds, int timeout_ms);

/* input.c */
extern bool ps2_present;               /* keyboard.c: there is an i8042 */
bool input_grabbed(void);
void input_key(uint16_t code, bool pressed, char ascii, unsigned mods);
long input_read(char *buf, size_t n);
bool input_readable(void);
bool input_absolute(void);
void input_set_absolute(bool on);        /* (drv/i8042: the vmmouse) */
/* Pointer motion from any mouse: relative, or absolute (0..65535 across the screen). */
void input_mouse(int dx, int dy, unsigned buttons);
void input_mouse_abs(int x, int y, unsigned buttons);
/* The wheel turned: notches, > 0 down (towards the user). */
void input_wheel(int notches);
/* A key as a set 1 scancode, 0x100 | code for E0 keys (keyboard.c). */
void kbd_key(uint16_t code, bool release);
int  input_open(void);
void input_close(void);


#endif
