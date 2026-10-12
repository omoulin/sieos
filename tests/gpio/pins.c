/* pins.c - a GPIO program as a user writes one (gpio.h), built on SIEOS by
 * cc and run by tools/gpiotest.py: outputs read back, an input's pull, a
 * device's pin refused, a pattern played by the server (it loops, it
 * stops). On a machine without GPIO the server simulates the pins.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "mk.h"
#include "gpio.h"

static int fails;
static void check(const char *what, int ok) { printf("%s: %s\n", ok ? "ok" : "FAIL", what); fails += !ok; }

int main(void)
{
    char board[64];
    int real = 0, n = gpio_info(board, sizeof board, &real);
    printf("pins: %d on %s\n", n, board);
    check("the server answers", n == GPIO_PINS);
    check("an output", gpio_mode(17, GPIO_OUT, GPIO_PULL_NONE) == 0);
    gpio_write(17, 1);
    check("it reads 1", gpio_read(17) == 1);
    gpio_write(17, 0);
    check("it reads 0", gpio_read(17) == 0);
    check("an input with a pull-up", gpio_mode(27, GPIO_IN, GPIO_PULL_UP) == 0);
    if (!real) check("it reads 1 (simulated)", gpio_read(27) == 1);
    check("the serial console's pin refused", gpio_mode(14, GPIO_OUT, GPIO_PULL_NONE) == -EBUSY);
    check("no pin 40", gpio_write(40, 1) < 0);
    uint32_t steps[4] = { 1u << 17, 1u << 18, 1u << 17 | 1u << 18, 0 };   /* a little running light */
    check("a pattern plays", gpio_play(1u << 17 | 1u << 18, steps, 4, 5000, 1) == 0);
    sleep_ms(100);
    uint32_t rounds = 0;
    int at = gpio_playing(&rounds);
    printf("playing step %d, %u rounds\n", at, rounds);
    check("it is playing, and looped", at >= 0 && at < 4 && rounds >= 2);
    uint32_t rec[4];
    int nr = gpio_record(rec, 4), all = nr == 4;
    for (int i = 0; i < nr; i++) if (!real && !(rec[i] >> 27 & 1)) all = 0;   /* (27: an input, pulled up) */
    check("the levels recorded at each step", all);
    gpio_pin_t st[GPIO_PINS];
    gpio_state(st);
    check("its pins are outputs", st[17].mode == GPIO_OUT && st[18].mode == GPIO_OUT);
    check("it stops", gpio_stop() == 0 && gpio_playing(0) == -1);
    printf(fails ? "gpio test: FAIL\n" : "gpio test: ok\n");
    return fails != 0;
}
