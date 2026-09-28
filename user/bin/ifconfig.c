/* ifconfig - show network interface configuration and counters */
#include "sieos.h"

int main(void)
{
    struct netinfo ni;
    char ip[16], mask[16], gw[16], dns[16];
    printf("lo:    inet 127.0.0.1  netmask 255.0.0.0  (loopback)\n");
    if (netinfo(&ni) < 0) {
        printf("eth0:  no network interface\n");
        return 1;
    }
    printf("eth0%-2s inet %s  netmask %s  %s\n", ":", ip_to_str(ni.ip, ip), ip_to_str(ni.netmask, mask),
           ni.up ? (ni.dhcp ? "(DHCP)" : "(static)") : "(down)");
    printf("       ether %02x:%02x:%02x:%02x:%02x:%02x  driver %s\n", ni.mac[0], ni.mac[1], ni.mac[2],
           ni.mac[3], ni.mac[4], ni.mac[5], ni.driver);
    printf("       gateway %s  dns %s\n", ip_to_str(ni.gateway, gw), ip_to_str(ni.dns, dns));
    printf("       RX packets %lu  bytes %lu  dropped %lu\n", ni.rx_packets, ni.rx_bytes, ni.rx_dropped);
    printf("       TX packets %lu  bytes %lu\n", ni.tx_packets, ni.tx_bytes);
    return 0;
}
