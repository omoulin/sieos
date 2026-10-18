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
    /* a precision touchpad: its report, fingers, button; its "input mode" feature, its "surface and button switches" feature */
    int trid, nf, fsz, tbpos, mrid, msz, srid;
    int ftip[HID_FINGERS], fok[HID_FINGERS], fx[HID_FINGERS], fy[HID_FINGERS];
    int32_t txmin, txmax, tymin, tymax;
    int tw, th;                  /* its size in µm (the descriptor's physical units; 0: not given) */
    /* the touchpad's state: fingers down, most of them, moved, last place, remainders; the touch's start, the last report */
    int n, maxn, moved, click, lx, ly, rx, ry, sacc, btn;
    int64_t t0, tlast;
    /* consumer keys (volume, as a Surface Type Cover's top row sends them): their report, the usage array; the one held */
    int crid, cpos, csz, cprev;
} hidrep_t;

void hid_parse(hidrep_t *h, const uint8_t *desc, int n);   /* the report descriptor */
void hid_input(hidrep_t *h, const uint8_t *r, int len);    /* an input report -> the console */
void hid_send(const con_inject_t *v, int n);               /* CON_INJECT (n 0: "a device is here") */
void hid_keys(uint8_t prev[8], const uint8_t r[8]);        /* a boot-layout keyboard report (prev: the last one) */
int hid_pad_size(const hidrep_t *h, int *w, int *hh);      /* a touchpad's size in µm -> 1 its descriptor's, 0 assumed */
/* For the log: its ranges and size (w, hh, ok: from hid_pad_size). */
#define HID_PAD_FMT "X %d-%d, Y %d-%d, %d x %d mm%s"
#define HID_PAD_ARGS(r, w, hh, ok) (r)->txmin, (r)->txmax, (r)->tymin, (r)->tymax, (w) / 1000, (hh) / 1000, (ok) ? "" : " (its size not given: assumed)"
