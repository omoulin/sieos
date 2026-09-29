/* netstat - list network sockets, IPv4 and IPv6 */
#include "sieos.h"
#include "sieos/socket.h"
#include "sieos/sysinfo.h"

static const char *endpoint(const unsigned char *a, unsigned port, char *buf, size_t n)
{
    static const unsigned char mapped[12] = { [10] = 0xff, [11] = 0xff }, zero[16];
    char ip[INET6_ADDRSTRLEN];
    if (!memcmp(a, mapped, 12)) {
        if (!a[12] && !a[13] && !a[14] && !a[15])
            snprintf(buf, n, "*:%s", port ? "" : "*");
        else
            inet_ntop(AF_INET, a + 12, ip, sizeof(ip)), snprintf(buf, n, "%s:", ip);
    } else if (!memcmp(a, zero, 16)) {
        snprintf(buf, n, "*:%s", port ? "" : "*");
    } else {
        inet_ntop(AF_INET6, a, ip, sizeof(ip));
        snprintf(buf, n, "[%s]:", ip);
    }
    if (port)
        snprintf(buf + strlen(buf), n - strlen(buf), "%u", port);
    return buf;
}

int main(void)
{
    static const char *states[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
                                    "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };
    static struct sieos_sockinfo6 si[128];
    int n = netstat6(si, 128);
    if (n < 0) {
        perror("netstat");
        return 1;
    }
    printf("Proto Recv-Q Send-Q Local Address                Foreign Address              State\n");
    for (int i = 0; i < n; i++) {
        char l[64], r[64];
        endpoint(si[i].laddr, si[i].lport, l, sizeof(l));
        endpoint(si[i].raddr, si[i].rport, r, sizeof(r));
        const char *proto = si[i].proto == IPPROTO_TCP ? "tcp" : si[i].proto == IPPROTO_UDP ? "udp" :
                            si[i].proto == IPPROTO_ICMPV6 ? "icmp6" : "icmp";
        printf("%-5s %6u %6u %-28s %-28s %s\n", si[i].family == SIEOS_AF_INET6 && si[i].proto != IPPROTO_ICMPV6 ?
               (si[i].proto == IPPROTO_TCP ? "tcp6" : "udp6") : proto, si[i].rxq, si[i].txq, l, r,
               si[i].proto == IPPROTO_TCP && si[i].state <= 10 ? states[si[i].state] : "");
    }
    return 0;
}
