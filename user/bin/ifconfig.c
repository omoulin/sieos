/*
 * ifconfig - show network interface configuration and counters.
 *   ifconfig        the interfaces (lo, eth0, eth1, ...), IPv4 and IPv6
 *   ifconfig -r     resolv.conf lines for the DNS servers learnt (DHCP, router advertisements)
 */
#include "sieos.h"
#include "sieos/sysinfo.h"

static void show(int index, const struct netinfo *ni, const struct sieos_netinfo6 *n6, bool have6)
{
    char ip[16], mask[16], gw[16], dns[16], a6[INET6_ADDRSTRLEN], name[12];
    snprintf(name, sizeof(name), "%s:", ni->name);
    printf("%-6s inet %s  netmask %s  %s\n", name, ip_to_str(ni->ip, ip), ip_to_str(ni->netmask, mask),
           ni->up ? (ni->dhcp ? "(DHCP)" : "(static)") : "(no address)");
    for (int i = 0; have6 && i < n6->naddr; i++) {
        if (!(n6->addr[i].flags & (SIEOS_NET6_LINKLOCAL | SIEOS_NET6_AUTOCONF)))
            continue;                                /* (::1 is lo's) */
        printf("       inet6 %s/%d  %s\n", inet_ntop(AF_INET6, n6->addr[i].addr, a6, sizeof(a6)),
               n6->addr[i].prefixlen, n6->addr[i].flags & SIEOS_NET6_LINKLOCAL ? "(link-local)" : "(autoconf)");
    }
    printf("       ether %02x:%02x:%02x:%02x:%02x:%02x  driver %s\n", ni->mac[0], ni->mac[1], ni->mac[2],
           ni->mac[3], ni->mac[4], ni->mac[5], ni->driver);
    printf("       gateway %s  dns %s\n", ip_to_str(ni->gateway, gw), ip_to_str(ni->dns, dns));
    if (have6 && n6->router[0]) {
        printf("       router6 %s", inet_ntop(AF_INET6, n6->router, a6, sizeof(a6)));
        if (n6->dns[0])
            printf("  dns6 %s", inet_ntop(AF_INET6, n6->dns, a6, sizeof(a6)));
        printf("  mtu %u\n", n6->mtu);
    }
    printf("       RX packets %lu  bytes %lu  dropped %lu\n", ni->rx_packets, ni->rx_bytes, ni->rx_dropped);
    printf("       TX packets %lu  bytes %lu\n", ni->tx_packets, ni->tx_bytes);
    if (have6)
        printf("       IPv6 RX packets %lu  TX packets %lu\n", n6->rx_packets, n6->tx_packets);
    (void)index;
}

int main(int argc, char **argv)
{
    struct netinfo ni;
    struct sieos_netinfo6 n6;
    char dns[16], a6[INET6_ADDRSTRLEN];
    bool resolv = argc > 1 && !strcmp(argv[1], "-r");
    if (!resolv) {
        printf("lo:    inet 127.0.0.1  netmask 255.0.0.0  (loopback)\n");
        printf("       inet6 ::1/128\n");
    }
    int n = 0;
    for (int i = 0; netinfo_if(&ni, i) == 0; i++, n++) {
        bool have6 = netinfo6_if(&n6, i) == 0;
        if (resolv) {
            if (ni.dns)
                printf("nameserver %s\n", ip_to_str(ni.dns, dns));
            if (have6 && n6.dns[0])
                printf("nameserver %s\n", inet_ntop(AF_INET6, n6.dns, a6, sizeof(a6)));
        } else {
            show(i, &ni, &n6, have6);
        }
    }
    if (!n && !resolv)
        printf("eth0:  no network interface\n");
    return n ? 0 : 1;
}
