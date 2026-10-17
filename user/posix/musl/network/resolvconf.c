/*
 * resolvconf.c - musl's DNS settings on SIEOS (wraps src/network/resolvconf.c):
 * /etc/resolv.conf if there is one; else the name server netd learnt by
 * DHCP (the Linux personality asks it), not musl's 127.0.0.1.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#define __get_resolv_conf __musl_get_resolv_conf
#include "../resolvconf.c"
#undef __get_resolv_conf

unsigned __sieos_dns(void);             /* (linux.c) the DNS server's IPv4 address, 0: none known */

hidden int __get_resolv_conf(struct resolvconf *conf, char *search, size_t search_sz)
{
    int r = __musl_get_resolv_conf(conf, search, search_sz);
    static const uint8_t lo[4] = { 127, 0, 0, 1 };
    if (r || conf->nns != 1 || conf->ns[0].family != AF_INET || memcmp(conf->ns[0].addr, lo, 4)) return r;
    unsigned ip = __sieos_dns();
    if (ip) {
        uint8_t a[4] = { ip >> 24, ip >> 16, ip >> 8, ip };
        memcpy(conf->ns[0].addr, a, 4);
    }
    return 0;
}
