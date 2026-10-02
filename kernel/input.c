/*
 * input.c - Input event queue (/dev/events).
 *
 * While /dev/events is open, keyboard input is delivered as key events
 * instead of going to the console terminal (a "grab"), and the mice's
 * motion (drv/i8042, the USB and I2C HID drivers) as motion events.
 * The queue is under input_lock (readers wait on input_cv).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "proc.h"
#include "arch.h"
#include "poll.h"

#define QSIZE 512

static struct input_event queue[QSIZE];
static uint32_t q_head, q_tail;
static int open_count;
static kmutex_t input_lock;
static kcondvar_t input_cv;

bool input_grabbed(void)
{
    return open_count > 0;
}

static void push(struct input_event *e)
{
    mutex_enter(&input_lock);
    if (!open_count) {
        mutex_exit(&input_lock);
        return;
    }
    if (q_tail - q_head == QSIZE)
        q_head++;                            /* drop the oldest */
    queue[q_tail++ % QSIZE] = *e;
    cv_broadcast(&input_cv);
    mutex_exit(&input_lock);
    poll_wakeup();
}

void input_key(uint16_t code, bool pressed, char ascii, unsigned mods)
{
    struct input_event e = { 0 };
    e.type = EV_KEY;
    e.code = code;
    e.value = pressed;
    e.ascii = (unsigned char)ascii;
    e.mods = mods;
    push(&e);
}

void input_mouse(int dx, int dy, unsigned buttons)
{
    struct input_event e = { 0 };
    e.type = EV_MOUSE;
    e.dx = dx;
    e.dy = dy;
    e.buttons = buttons;
    push(&e);
}

void input_mouse_abs(int x, int y, unsigned buttons)
{
    struct input_event e = { 0 };
    e.type = EV_MOUSE_ABS;
    e.dx = x;
    e.dy = y;
    e.buttons = buttons;
    push(&e);
}

void input_wheel(int notches)
{
    if (!notches)
        return;
    struct input_event e = { 0 };
    e.type = EV_WHEEL;
    e.value = notches;
    push(&e);
}

bool input_readable(void)
{
    return q_head != q_tail;
}

long input_read(char *buf, size_t n)
{
    size_t sz = sizeof(struct input_event);
    if (n < sz)
        return -EINVAL;
    mutex_enter(&input_lock);
    while (q_head == q_tail) {
        if (!cv_wait_sig(&input_cv, &input_lock)) {
            mutex_exit(&input_lock);
            return -ERESTART;
        }
    }
    size_t got = 0;
    while (got + sz <= n && q_head != q_tail) {
        memcpy(buf + got, &queue[q_head++ % QSIZE], sz);
        got += sz;
    }
    mutex_exit(&input_lock);
    return got;
}

int input_open(void)
{
    mutex_enter(&input_lock);
    if (open_count == 0)
        q_head = q_tail = 0;
    open_count++;
    mutex_exit(&input_lock);
    return 0;
}

void input_close(void)
{
    mutex_enter(&input_lock);
    if (open_count > 0)
        open_count--;
    mutex_exit(&input_lock);
}

/* The pointer is absolute (a hypervisor's): set by the i8042 driver (vmmouse). */
static bool absolute;

bool input_absolute(void)
{
    return absolute;
}

void input_set_absolute(bool on)
{
    absolute = on;
}
