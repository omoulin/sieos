/*
 * gpio.c - The GPIO server's calls (gpio.h).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "gpio.h"

static long port;

static long call(msg_t *m) { long e = call_named(&port, "gpio", m, 1); return e < 0 ? e : (long)m->w[0]; }

int gpio_info(char *board, int cap, int *real)
{
    char b[64] = { 0 };
    msg_t m = { .w = { GPIO_INFO }, .rbuf = b, .rlen = sizeof b - 1 };
    long r = call(&m);
    if (r < 0) return (int)r;
    if (board && cap > 0) strlcpy(board, b, (size_t)cap);
    if (real) *real = (int)m.w[1];
    return (int)r;
}
int gpio_mode(int pin, int mode, int pull) { msg_t m = { .w = { GPIO_MODE, (uint64_t)pin, (uint64_t)mode, (uint64_t)pull } }; return (int)call(&m); }
int gpio_write(int pin, int level) { msg_t m = { .w = { GPIO_WRITE, (uint64_t)pin, (uint64_t)!!level } }; return (int)call(&m); }
int gpio_read(int pin) { msg_t m = { .w = { GPIO_READ, (uint64_t)pin } }; return (int)call(&m); }
long gpio_state(gpio_pin_t pins[GPIO_PINS])
{
    msg_t m = { .w = { GPIO_STATE }, .rbuf = pins, .rlen = GPIO_PINS * sizeof *pins };
    return call(&m);
}
int gpio_play(uint32_t mask, const uint32_t *steps, int n, uint32_t step_us, int loop)
{
    if (n <= 0 || n > GPIO_MAXSTEP) return -EINVAL;
    size_t len = sizeof(gpio_pattern_t) + (size_t)n * 4;
    char *b = malloc(len);
    if (!b) return -ENOMEM;
    gpio_pattern_t p = { mask, (uint32_t)n, step_us, (uint32_t)!!loop };
    memcpy(b, &p, sizeof p);
    memcpy(b + sizeof p, steps, (size_t)n * 4);
    msg_t m = { .w = { GPIO_PLAY }, .sbuf = b, .slen = len };
    long r = call(&m);
    free(b);
    return (int)r;
}
int gpio_stop(void) { msg_t m = { .w = { GPIO_STOP } }; return (int)call(&m); }
int gpio_playing(uint32_t *rounds)
{
    msg_t m = { .w = { GPIO_PLAYING } };
    long r = call(&m);
    if (rounds) *rounds = (uint32_t)m.w[1];
    return (int)r;
}
int gpio_record(uint32_t *levels, int max)
{
    msg_t m = { .w = { GPIO_RECORD }, .rbuf = levels, .rlen = (uint64_t)(max > 0 ? max : 0) * 4 };
    long r = call(&m);
    return (int)(r > max ? max : r);
}
int gpio_selftest(uint8_t res[GPIO_PINS])
{
    msg_t m = { .w = { GPIO_SELFTEST }, .rbuf = res, .rlen = GPIO_PINS };
    return (int)call(&m);
}
void sleep_ms(long ms) { sys_sleep((uint64_t)ms * 1000000); }
