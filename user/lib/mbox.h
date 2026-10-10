/*
 * mbox.h - The Raspberry Pis' firmware "mailbox" (property channel): how the
 * ARM side asks the VideoCore firmware for things (a framebuffer, a clock's
 * rate, the board's serial number...). mbox.c.
 *
 * A request is a buffer of 32-bit words: its size, a code (0 = request),
 * then "tags" {tag id, value size, request/response size, values...}, then
 * 0. The firmware writes its answers into the same buffer. The buffer must
 * be in the first GiB (the firmware sees no more) and 16-byte aligned.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>

/* Send buf (n words, laid out as above) and wait for the answer, which
 * replaces it: 0 if the firmware answered "success", -1 otherwise (no
 * mailbox in the device tree, or an error). Several programs may use it,
 * one request at a time each (the firmware answers in order). */
int mbox_call(uint32_t *buf, int n);
