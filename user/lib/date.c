/*
 * date.c - A time in nanoseconds since 1970 (UTC) as "YYYY-MM-DD HH:MM".
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"

void date_str(int64_t ns, char *s)
{
    int64_t t = ns / 1000000000, d = t / 86400 + 719468, sec = t % 86400;   /* days -> civil date */
    int64_t era = d / 146097, doe = d - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t day = doy - (153 * mp + 2) / 5 + 1, mon = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (mon <= 2);
    int v[5] = { (int)y, (int)mon, (int)day, (int)(sec / 3600), (int)(sec / 60 % 60) };
    for (int i = 0; i < 5; i++) {
        int w = i ? 2 : 4, x = v[i];
        for (int k = w - 1; k >= 0; k--) { s[k] = (char)('0' + x % 10); x /= 10; }
        s += w;
        if (i < 4) *s++ = "-- :"[i];
    }
    *s = 0;
}
