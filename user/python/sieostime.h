/*
 * sieostime.h - The time module's date functions, included by MicroPython's
 * extmod/modtime.c: time.time() and the current date for localtime() (UTC:
 * SIEOS has no time zones), from mp_hal_time_ns (main.c).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "shared/timeutils/timeutils.h"

static mp_obj_t mp_time_time_get(void) { return mp_obj_new_float((mp_float_t)mp_hal_time_ns() / 1e9); }
static void mp_time_localtime_get(timeutils_struct_time_t *tm)
{
    timeutils_seconds_since_1970_to_struct_time((timeutils_timestamp_t)(mp_hal_time_ns() / 1000000000), tm);
}
