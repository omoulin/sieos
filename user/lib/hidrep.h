/*
 * hidrep.h - HID input reports (hidrep.c): where a mouse's, a tablet's, a
 * touch screen's or a precision touchpad's buttons, X, Y, wheel and fingers are (from the
 * device's report descriptor), and each report turned into CON_INJECT
 * events for the console; keyboards' reports (boot layout: modifier bits,
 * a reserved byte, 6 keys) turned into scancodes, held keys repeated.
 * Shared by USB (user/usb/hid.c), I2C (user/i2chid) and Bluetooth (user/btd).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "mk.h"

#define HID_FINGERS 5
typedef struct {
    /* a mouse or tablet (positions are bit offsets in the report, after its id); screen: a touch screen's first finger */
    int rid, bpos, bn, xpos, xsz, ypos, ysz, wpos, wsz, absolute, screen;
    int32_t xmax, ymax;
    /* a keyboard: its report, modifier bits, key array (6 keys used); the keys held */
    int krid, kpos, kkeys, kn;
    uint8_t kprev[8];
    /* a precision touchpad: its report, fingers, button; its "input mode" feature */
    int trid, nf, fsz, tbpos, mrid, msz;
    int ftip[HID_FINGERS], fok[HID_FINGERS], fx[HID_FINGERS], fy[HID_FINGERS];
    int32_t txmax;
    /* the touchpad's state: fingers down, most of them, moved, last place, remainders */
    int n, maxn, moved, click, lx, ly, rx, ry, sacc, btn;
    int64_t t0;
} hidrep_t;

void hid_parse(hidrep_t *h, const uint8_t *desc, int n);   /* the report descriptor */
void hid_input(hidrep_t *h, const uint8_t *r, int len);    /* an input report -> the console */
void hid_send(const con_inject_t *v, int n);               /* CON_INJECT (n 0: "a device is here") */
void hid_keys(uint8_t prev[8], const uint8_t r[8]);        /* a boot-layout keyboard report (prev: the last one) */
