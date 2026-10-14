/*
 * gpio.c - /bin/gpio: the pins from the shell (through the GPIO server).
 *
 *   gpio                      every pin: its mode, pull and level
 *   gpio in PIN [up|down]     an input (with a pull)
 *   gpio out PIN [0|1]        an output (and its level)
 *   gpio get PIN              its level: 0 or 1
 *   gpio blink PIN MS         on and off every MS milliseconds (the server plays it)
 *   gpio stop                 the pattern stops
 *   gpio selftest             nothing wired: every free pin checked (out 1, out 0,
 *                             pull-up, pull-down), then put back as it was
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
/* The self-check's table: a row a pin, PASS or FAIL, then the verdict. */
static int selftest(void)
{
    char board[64];
    int n = gpio_info(board, sizeof board, 0);
    if (n < 0) return fail("the server", n);
    gpio_pin_t st[GPIO_PINS];
    uint8_t res[GPIO_PINS];
    if ((n = (int)gpio_state(st)) < 0) return fail("the server", n);
    printf("gpio selftest: %s; nothing may be wired to the pins\n", board);
    if ((n = gpio_selftest(res)) < 0) {
        if (n == -EBUSY) { printf("gpio: a pattern is playing: gpio stop first\n"); return 1; }
        return fail("selftest", n);
    }
    printf("  pin  out 1  out 0  pull-up  pull-down  kept\n");
    int done = 0, bad = 0;
    for (int p = 0; p < GPIO_PINS; p++) {
        if (!res[p]) {
            printf("  %3d  skipped: %s\n", p, st[p].mode == GPIO_BUSY ? "used by a device" : p == 14 || p == 15 ? "the serial console's" :
                   p == 2 || p == 3 ? "I2C, pulled up on the board" : "the HAT ID EEPROM's");
            continue;
        }
#define R(bit) (res[p] & (bit) ? "ok" : "FAIL")
        printf("  %3d  %-5s  %-5s  %-7s  %-9s  %-4s  %s\n", p, R(GPIO_CHK_HIGH), R(GPIO_CHK_LOW), R(GPIO_CHK_UP), R(GPIO_CHK_DOWN),
               R(GPIO_CHK_KEPT), res[p] == GPIO_CHK_ALL ? "PASS" : "FAIL");
#undef R
        done++; bad += res[p] != GPIO_CHK_ALL;
    }
    printf("selftest: %s: %d pins checked, %d failed, %d skipped\n", bad || !done ? "FAIL" : "PASS", done, bad, GPIO_PINS - done);
    return bad || !done;
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
    if (!strcmp(c, "selftest")) return selftest();
    if (pin < 0) { printf("usage: gpio [in PIN [up|down] | out PIN [0|1] | get PIN | blink PIN MS | stop | selftest]\n"); return 2; }
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
