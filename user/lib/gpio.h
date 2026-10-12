/*
 * gpio.h - The pins of a Raspberry Pi (or simulated ones elsewhere), for C
 * programs on SIEOS: a few calls to the GPIO server (/bin/gpiod; protocol:
 * mk/proto.h). Each returns >= 0, or -error (-EBUSY: a device's pin;
 * -EINVAL: no such pin or value). Pins are BCM numbers (0..27).
 *
 *   #include "gpio.h"
 *   gpio_mode(17, GPIO_OUT, GPIO_PULL_NONE);       // an LED on BCM 17 (header pin 11)
 *   for (int i = 0; i < 10; i++) { gpio_write(17, i & 1); sleep_ms(500); }
 *   gpio_mode(27, GPIO_IN, GPIO_PULL_UP);          // a button to ground on BCM 27
 *   if (!gpio_read(27)) printf("pressed\n");
 *
 * Patterns: the server plays them with steady timing, on its own:
 *   uint32_t blink[] = { 1u << 17, 0 };            // two steps: on, off
 *   gpio_play(1u << 17, blink, 2, 250000, 1);      // 250 ms a step, looping
 *   ...
 *   gpio_stop();
 *
 * Built with gcc (in the system) and with cc on SIEOS: the library is in
 * libsieos.a, which every program links.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stdint.h>
#include "mk/proto.h"

int  gpio_info(char *board, int cap, int *real);     /* -> pins; the board's name; real: not simulated */
int  gpio_mode(int pin, int mode, int pull);         /* GPIO_IN / GPIO_OUT; GPIO_PULL_NONE / _DOWN / _UP */
int  gpio_write(int pin, int level);                 /* an output: 0 or 1 */
int  gpio_read(int pin);                             /* -> 0 or 1 */
long gpio_state(gpio_pin_t pins[GPIO_PINS]);         /* every pin's mode, pull, level -> the levels (a bit each) */
int  gpio_play(uint32_t mask, const uint32_t *steps, int n, uint32_t step_us, int loop);
int  gpio_stop(void);
int  gpio_playing(uint32_t *rounds);                 /* -> the step being played, or -1 */
int  gpio_record(uint32_t *levels, int max);          /* the levels at each step as played (bit p: pin p) -> steps */
void sleep_ms(long ms);
