/* netstat - list network sockets */
#include "sieos.h"

int main(void)
{
    static const char *states[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
                                    "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };
    struct sockinfo si[64];
    int n = netstat(si, 64);
    if (n < 0) {
        perror("netstat");
        return 1;
    }
    printf("Proto Recv-Q Send-Q Local Address          Foreign Address        State\n");
    for (int i = 0; i < n; i++) {
        char l[32], r[32], a[16], b[16];
        snprintf(l, sizeof(l), "%s:%u", si[i].lip ? ip_to_str(si[i].lip, a) : "*", si[i].lport);
        if (si[i].rip || si[i].rport)
            snprintf(r, sizeof(r), "%s:%u", ip_to_str(si[i].rip, b), si[i].rport);
        else
            strcpy(r, "*:*");
        const char *proto = si[i].proto == IPPROTO_TCP ? "tcp" : si[i].proto == IPPROTO_UDP ? "udp" : "raw";
        printf("%-5s %6u %6u %-22s %-22s %s\n", proto, si[i].rxq, si[i].txq, l, r,
               si[i].proto == IPPROTO_TCP && si[i].state <= 10 ? states[si[i].state] : "");
    }
    return 0;
}
