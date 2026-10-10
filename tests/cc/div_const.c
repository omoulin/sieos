/* div_const.c - division and remainder by constants (reciprocal multiplication), all signs and widths.
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
#include "t.h"
#define D(T, d) static T d_##T##_##d(T x) { return x / d; } static T m_##T##_##d(T x) { return x % d; }
#define N(T, d, n) static T d_##T##_n##n(T x) { return x / d; } static T m_##T##_n##n(T x) { return x % d; }
typedef unsigned int ui; typedef unsigned long ul;
D(int,3) D(int,7) D(int,10) D(int,25) D(int,641) D(int,1000000007) N(int,-7,7) N(int,-1000,1000)
D(ui,3) D(ui,7) D(ui,10) D(ui,26) D(ui,641) D(ui,4294967291u)
D(long,3) D(long,7) D(long,25) D(long,100) D(long,1000000007) N(long,-7,7)
D(ul,3) D(ul,7) D(ul,25) D(ul,1000) D(ul,7919) D(ul,9223372036854775783ul)
static unsigned long rnd(void) { static unsigned long s = 88172645463325252ul; s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
int main(void) {
    unsigned long h = 0;
    long sp[] = { 0, 1, -1, 2, -2, 3, -3, 6, -6, 7, -7, 2147483647, -2147483647 - 1, 9223372036854775807L, -9223372036854775807L - 1 };
    for (int k = 0; k < 200000 + 15; k++) {
        unsigned long r = k < 15 ? (unsigned long)sp[k] : rnd() >> (rnd() & 63);
        if (k & 1) r = -r;
        int i = (int)r; ui u = (ui)r; long l = (long)r; ul q = r;
#define H(f, x) h = h * 31 + (unsigned long)f(x)
        H(d_int_3,i); H(m_int_3,i); H(d_int_7,i); H(m_int_7,i); H(d_int_10,i); H(m_int_10,i); H(d_int_25,i); H(d_int_641,i); H(m_int_1000000007,i); H(d_int_1000000007,i); H(d_int_n7,i); H(m_int_n7,i); H(d_int_n1000,i); H(m_int_n1000,i);
        H(d_ui_3,u); H(m_ui_3,u); H(d_ui_7,u); H(d_ui_10,u); H(m_ui_26,u); H(d_ui_641,u); H(d_ui_4294967291u,u); H(m_ui_4294967291u,u);
        H(d_long_3,l); H(m_long_3,l); H(d_long_7,l); H(d_long_25,l); H(m_long_100,l); H(d_long_1000000007,l); H(d_long_n7,l); H(m_long_n7,l);
        H(d_ul_3,q); H(m_ul_3,q); H(d_ul_7,q); H(m_ul_7,q); H(d_ul_25,q); H(d_ul_1000,q); H(m_ul_7919,q); H(d_ul_9223372036854775783ul,q); H(m_ul_9223372036854775783ul,q);
    }
    printf("%lx\n", h);
    return 0;
}
