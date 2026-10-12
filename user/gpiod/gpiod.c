/*
 * gpiod.c - The GPIO server (port "gpio"; the protocol: mk/proto.h, the
 * library: user/lib/gpio.h): the Raspberry Pi's pins for every program,
 * and patterns played on them with steady timing.
 *
 *   pins      a pin is an input (with a pull-up, a pull-down or none) or an
 *             output (0 or 1). A pin a device uses (the serial console's
 *             14 and 15, I2C, SPI: the firmware or a driver gave it an
 *             alternate function) is shown as busy and refused.
 *   patterns  a pattern is a list of steps, each the levels of the pins it
 *             drives, one step every step_us microseconds, once or looping.
 *             The server's own thread plays it (deadlines from the start,
 *             so the timing does not drift), whatever its client does,
 *             and records every pin's level at each step (the inputs'
 *             recording: GPIO_RECORD), at the end of the step, just before
 *             the next one is set.
 *
 * On a Raspberry Pi (2 to 4: the BCM283x / BCM2711 GPIO block, from the
 * device tree) the pins are real. Elsewhere (x86-64, QEMU's virt) they are
 * simulated, so programs and tests run the same everywhere: an output reads
 * back its level, an input reads its pull (1 with a pull-up).
 *
 * Started on demand by init (the first program that looks up "gpio"), and
 * kept: a pattern may be playing.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include <stdarg.h>
#ifdef __aarch64__
#include "mk/fdt.h"
#endif

static volatile uint32_t *gpio;          /* the real block, or 0: simulated */
static int bcm2711;                      /* (its pulls are set another way than the older chips') */
static gpio_pin_t sim[GPIO_PINS];        /* the simulated pins */
static char board[48] = "simulated (no GPIO on this machine)";

static struct { char b[160]; size_t n; } line;
static void put(void *c, char ch) { (void)c; if (line.n < sizeof line.b) line.b[line.n++] = ch; }
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    line.n = 0;
    vformat(put, 0, fmt, ap);
    va_end(ap);
    sys_debug(line.b, line.n);
}

/* ---- The pins: real or simulated. */
static int fsel(int pin) { return gpio[pin / 10] >> (pin % 10 * 3) & 7; }
static int mode_of(int pin)
{
    if (!gpio) return sim[pin].mode;
    int f = fsel(pin);
    return f == 0 ? GPIO_IN : f == 1 ? GPIO_OUT : GPIO_BUSY;
}
static int level_of(int pin)
{
    if (!gpio) return sim[pin].level;
    return gpio[0x34 / 4] >> pin & 1;                    /* GPLEV0 */
}
static void set_levels(uint32_t mask, uint32_t lv)       /* outputs in mask: 1 where lv has 1 */
{
    if (!gpio) { for (int p = 0; p < GPIO_PINS; p++) if (mask >> p & 1) sim[p].level = lv >> p & 1; return; }
    if (mask & lv) gpio[0x1c / 4] = mask & lv;           /* GPSET0 */
    if (mask & ~lv) gpio[0x28 / 4] = mask & ~lv;         /* GPCLR0 */
}
static uint8_t pulls[GPIO_PINS];
static int set_mode(int pin, int mode, int pull)
{
    if (pin < 0 || pin >= GPIO_PINS || (mode != GPIO_IN && mode != GPIO_OUT) || pull < 0 || pull > GPIO_PULL_UP) return -EINVAL;
    if (mode_of(pin) == GPIO_BUSY) return -EBUSY;
    pulls[pin] = (uint8_t)pull;
    if (!gpio) {
        sim[pin].mode = (uint8_t)mode; sim[pin].pull = (uint8_t)pull;
        if (mode == GPIO_IN) sim[pin].level = pull == GPIO_PULL_UP;
        return 0;
    }
    volatile uint32_t *r = gpio + pin / 10;
    *r = (*r & ~(7u << pin % 10 * 3)) | (uint32_t)(mode == GPIO_OUT) << pin % 10 * 3;
    if (bcm2711) {                                       /* GPIO_PUP_PDN_CNTRL: 0 none, 1 up, 2 down */
        volatile uint32_t *c = gpio + 0xe4 / 4 + pin / 16;
        uint32_t v = pull == GPIO_PULL_UP ? 1 : pull == GPIO_PULL_DOWN ? 2 : 0;
        *c = (*c & ~(3u << pin % 16 * 2)) | v << pin % 16 * 2;
    } else {                                             /* the older chips: GPPUD, then a clock pulse */
        gpio[0x94 / 4] = pull == GPIO_PULL_UP ? 2 : pull == GPIO_PULL_DOWN ? 1 : 0;
        sys_sleep(10000);
        gpio[0x98 / 4] = 1u << pin;
        sys_sleep(10000);
        gpio[0x94 / 4] = 0; gpio[0x98 / 4] = 0;
    }
    return 0;
}

static void hardware(void)
{
#ifdef __aarch64__
    long size = sys_fdt(0, 0);
    char *f = size > 0 ? malloc(size) : 0;
    if (!f || sys_fdt(f, size) != size) { free(f); return; }
    static const char *compat[] = { "brcm,bcm2711-gpio", "brcm,bcm2835-gpio", 0 };
    for (int k = 0; compat[k] && !gpio; k++) {
        uint64_t a, sz;
        for (int n = -1; (n = fdt_find(f, n, compat[k])) >= 0; )
            if (!fdt_reg(f, n, 0, &a, &sz)) {
                void *m = map_phys(a, 4096);
                if ((long)m < 0) continue;
                gpio = m; bcm2711 = k == 0;
                strlcpy(board, bcm2711 ? "Raspberry Pi 4 (BCM2711)" : "Raspberry Pi (BCM283x)", sizeof board);
                break;
            }
    }
    free(f);
#endif
    if (!gpio) {                                         /* the simulated pins: 14 and 15 are the "console's" */
        for (int p = 0; p < GPIO_PINS; p++) sim[p] = (gpio_pin_t){ GPIO_IN, GPIO_PULL_NONE, 0, 0 };
        sim[14].mode = sim[15].mode = GPIO_BUSY;
    }
}

/* ---- Patterns: one at a time, played by a thread of their own. */
static uint32_t steps[GPIO_MAXSTEP];
static gpio_pattern_t pat;
static volatile int go, stop, at = -1;                   /* at: the step being played (-1: none) */
static volatile uint32_t rounds;
static uint32_t rec[GPIO_MAXSTEP];                       /* the levels at each step, as played */
static volatile uint32_t nrec;                           /* steps sampled so far (up to steps) */
static uint32_t levels(void)
{
    uint32_t v = 0;
    for (int p = 0; p < GPIO_PINS; p++) v |= (uint32_t)level_of(p) << p;
    return v;
}
static long player_tid;

static void player(void *arg)
{
    (void)arg;
    for (;;) {
        while (!go) sys_sleep(1000000000);               /* (woken by PLAY) */
        uint64_t t0 = (uint64_t)sys_clock(), k = 0;
        rounds = 0; nrec = 0;
        while (!stop) {
            uint32_t i = (uint32_t)(k % pat.steps);
            if (i == 0 && k) { rounds++; if (!pat.loop) break; }
            at = (int)i;
            set_levels(pat.mask, steps[i]);
            k++;
            uint64_t next = t0 + k * (uint64_t)pat.step_us * 1000, now = (uint64_t)sys_clock();
            if (next > now + 50000) sys_sleep(next - now - 50000);   /* (to 50 us before its end: ...) */
            rec[i] = levels();                                       /* (... the step's levels, recorded) */
            if (nrec < pat.steps) nrec = i + 1 > nrec ? i + 1 : nrec;
            now = (uint64_t)sys_clock();
            if (next > now) sys_sleep(next - now);
        }
        at = -1;
        go = 0;
    }
}
static void halt(void)
{
    if (!go) return;
    stop = 1;
    sys_wake(player_tid);
    for (int i = 0; go && i < 2000; i++) sys_sleep(1000000);
    stop = 0;
}
static long play(const void *buf, uint64_t len)
{
    const gpio_pattern_t *p = buf;
    if (len < sizeof *p) return -EINVAL;
    if (!p->steps || p->steps > GPIO_MAXSTEP || p->step_us < 100 || len < sizeof *p + p->steps * 4ULL) return -EINVAL;
    if (p->mask >> GPIO_PINS) return -EINVAL;
    for (int i = 0; i < GPIO_PINS; i++)                  /* the pins it drives: outputs */
        if (p->mask >> i & 1) {
            if (mode_of(i) == GPIO_BUSY) return -EBUSY;
            if (mode_of(i) != GPIO_OUT) set_mode(i, GPIO_OUT, GPIO_PULL_NONE);
        }
    halt();
    pat = *p;
    memcpy(steps, (const char *)buf + sizeof *p, p->steps * 4);
    if (player_tid <= 0) player_tid = thread_start(player, 0, 16384);
    if (player_tid <= 0) return -ENOMEM;
    go = 1;
    sys_wake(player_tid);
    say("gpiod: playing %u steps of %u us on pins %x%s\n", p->steps, p->step_us, p->mask, p->loop ? ", looping" : "");
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    hardware();
    long port = port_create("gpio");
    if (port < 0) return 1;
    say("gpiod: %d pins, %s\n", GPIO_PINS, board);
    static char buf[sizeof(gpio_pattern_t) + GPIO_MAXSTEP * 4] __attribute__((aligned(8)));
    for (;;) {
        msg_t m = { .rbuf = buf, .rlen = sizeof buf };
        long from = ipc_recv(port, &m);
        if (from <= 0) continue;
        msg_t r = { .w = { 0 } };
        long pin = (long)m.w[1];
        int ok_pin = pin >= 0 && pin < GPIO_PINS;
        switch (m.w[0]) {
        case GPIO_INFO:
            r.w[0] = GPIO_PINS; r.w[1] = gpio != 0;
            r.sbuf = board; r.slen = strlen(board) + 1;
            break;
        case GPIO_MODE:
            r.w[0] = (uint64_t)set_mode((int)pin, (int)m.w[2], (int)m.w[3]);
            break;
        case GPIO_WRITE:
            if (!ok_pin) r.w[0] = (uint64_t)-EINVAL;
            else if (mode_of((int)pin) != GPIO_OUT) r.w[0] = (uint64_t)(mode_of((int)pin) == GPIO_BUSY ? -EBUSY : -EPERM);
            else set_levels(1u << pin, m.w[2] ? 1u << pin : 0);
            break;
        case GPIO_READ:
            r.w[0] = ok_pin ? (uint64_t)level_of((int)pin) : (uint64_t)-EINVAL;
            break;
        case GPIO_STATE: {
            static gpio_pin_t st[GPIO_PINS];
            uint32_t lv = 0;
            for (int p = 0; p < GPIO_PINS; p++) {
                st[p] = (gpio_pin_t){ (uint8_t)mode_of(p), pulls[p], (uint8_t)level_of(p), 0 };
                lv |= (uint32_t)st[p].level << p;
            }
            r.w[0] = lv; r.sbuf = st; r.slen = sizeof st;
            break;
        }
        case GPIO_PLAY:    r.w[0] = (uint64_t)play(buf, m.rlen); break;
        case GPIO_STOP:    halt(); break;
        case GPIO_PLAYING: r.w[0] = (uint64_t)(long)at; r.w[1] = rounds; break;
        case GPIO_RECORD:  r.w[0] = nrec; r.sbuf = rec; r.slen = (uint64_t)nrec * 4; break;
        default:           r.w[0] = (uint64_t)-ENOSYS;   /* (and init's ping) */
        }
        ipc_reply(from, &r);
    }
}
