/*
 * gpio.c - /bin/gpio: the pins from the shell (through the GPIO server).
 *
 *   gpio                      every pin: its mode, pull and level
 *   gpio in PIN [up|down]     an input (with a pull)
 *   gpio out PIN [0|1]        an output (and its level)
 *   gpio get PIN              its level: 0 or 1
 *   gpio blink PIN MS         on and off every MS milliseconds (the server plays it)
 *   gpio stop                 the pattern stops
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "gpio.h"

static int fail(const char *what, int e)
{
    printf("gpio: %s: %s\n", what, e == -EBUSY ? "used by a device" : e == -EINVAL ? "no such pin or value" :
           e == -EPERM ? "not an output" : "error");
    return 1;
}
static long n_of(const char *s) { long v = 0; if (!*s) return -1; for (; *s; s++) { if (*s < '0' || *s > '9') return -1; v = v * 10 + *s - '0'; } return v; }

int main(int argc, char **argv)
{
    int pin = argc > 2 ? (int)n_of(argv[2]) : -1, r;
    if (argc < 2) {
        char board[64];
        int real = 0, n = gpio_info(board, sizeof board, &real);
        if (n < 0) return fail("the server", n);
        gpio_pin_t st[GPIO_PINS];
        if ((r = (int)gpio_state(st)) < 0) return fail("the server", r);
        printf("%s: %d pins%s\n", board, n, real ? "" : " (simulated)");
        static const char *mode[] = { "in", "out", "busy" }, *pull[] = { "", " pull-down", " pull-up" };
        for (int p = 0; p < GPIO_PINS; p++)
            printf("  %2d  %-4s %d%s\n", p, mode[st[p].mode < 3 ? st[p].mode : 2], st[p].level, st[p].mode == GPIO_IN ? pull[st[p].pull % 3] : "");
        return 0;
    }
    const char *c = argv[1];
    if (!strcmp(c, "stop")) return (r = gpio_stop()) < 0 ? fail("stop", r) : 0;
    if (pin < 0) { printf("usage: gpio [in PIN [up|down] | out PIN [0|1] | get PIN | blink PIN MS | stop]\n"); return 2; }
    if (!strcmp(c, "in")) {
        int pl = argc > 3 && !strcmp(argv[3], "up") ? GPIO_PULL_UP : argc > 3 && !strcmp(argv[3], "down") ? GPIO_PULL_DOWN : GPIO_PULL_NONE;
        return (r = gpio_mode(pin, GPIO_IN, pl)) < 0 ? fail("in", r) : 0;
    }
    if (!strcmp(c, "out")) {
        if ((r = gpio_mode(pin, GPIO_OUT, GPIO_PULL_NONE)) < 0) return fail("out", r);
        if (argc > 3 && (r = gpio_write(pin, argv[3][0] == '1')) < 0) return fail("out", r);
        return 0;
    }
    if (!strcmp(c, "get")) {
        if ((r = gpio_read(pin)) < 0) return fail("get", r);
        printf("%d\n", r);
        return 0;
    }
    if (!strcmp(c, "blink")) {
        long ms = argc > 3 ? n_of(argv[3]) : 500;
        if (ms < 1) ms = 500;
        uint32_t s[2] = { 1u << pin, 0 };
        return (r = gpio_play(1u << pin, s, 2, (uint32_t)ms * 1000, 1)) < 0 ? fail("blink", r) : 0;
    }
    printf("gpio: %s: unknown\n", c);
    return 2;
}
