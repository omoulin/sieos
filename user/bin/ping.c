/*
 * ping - send ICMP echo requests (installed set-user-ID root, since raw
 * sockets are privileged).
 *   ping [-c count] host
 */
#include "sieos.h"

static unsigned short icmp_csum(const unsigned char *p, int len)
{
    unsigned long sum = 0;
    for (int i = 0; i + 1 < len; i += 2)
        sum += (p[i] << 8) | p[i + 1];
    if (len & 1)
        sum += p[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return htons(~sum & 0xFFFF);
}

static volatile int stop;
static void on_int(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
    int count = 4;
    const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            count = atoi(argv[++i]);
        else
            host = argv[i];
    }
    if (!host) {
        dprintf(STDERR_FILENO, "usage: ping [-c count] host\n");
        return 2;
    }
    unsigned int addr;
    if (resolve_host(host, &addr) < 0) {
        dprintf(STDERR_FILENO, "ping: unknown host %s\n", host);
        return 2;
    }
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        perror("ping: socket");
        return 2;
    }
    setuid(getuid());                              /* drop privileges once the socket exists */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_int;
    sigaction(SIGINT, &sa, NULL);

    struct sockaddr_in to = make_addr(addr, 0);
    char ipbuf[16];
    inet_ntop(AF_INET, &addr, ipbuf, sizeof(ipbuf));
    printf("PING %s (%s): 56 data bytes\n", host, ipbuf);
    unsigned short ident = getpid() & 0xFFFF;
    int sent = 0, received = 0;
    long tmin = 1 << 30, tmax = 0, tsum = 0;
    for (int seq = 1; (count <= 0 || seq <= count) && !stop; seq++) {
        unsigned char pkt[64];
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 8;                                /* echo request */
        pkt[4] = ident >> 8; pkt[5] = ident;
        pkt[6] = seq >> 8;   pkt[7] = seq;
        long t0 = uptime_ms();
        memcpy(pkt + 8, &t0, sizeof(t0));
        for (int i = 16; i < 64; i++)
            pkt[i] = i;
        unsigned short c = icmp_csum(pkt, 64);
        memcpy(pkt + 2, &c, 2);
        if (sendto(fd, pkt, 64, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            perror("ping: sendto");
            break;
        }
        sent++;
        long deadline = t0 + 1000;
        while (!stop) {
            long now = uptime_ms();
            if (now >= deadline)
                break;
            struct pollfd p = { fd, POLLIN, 0 };
            if (poll(&p, 1, deadline - now) <= 0)
                continue;
            unsigned char r[1500];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            long n = recvfrom(fd, r, sizeof(r), 0, (struct sockaddr *)&from, &fl);
            if (n < 8 || r[0] != 0 || ((r[4] << 8) | r[5]) != ident || from.sin_addr.s_addr != addr)
                continue;
            int rseq = (r[6] << 8) | r[7];
            long rtt = uptime_ms() - t0;
            printf("%ld bytes from %s: icmp_seq=%d time=%ld ms\n", n, ipbuf, rseq, rtt);
            received++;
            tsum += rtt;
            if (rtt < tmin) tmin = rtt;
            if (rtt > tmax) tmax = rtt;
            break;
        }
        long rest = deadline - uptime_ms();
        if (rest > 0 && (count <= 0 || seq < count) && !stop)
            msleep(rest);
    }
    printf("--- %s ping statistics ---\n", host);
    printf("%d packets transmitted, %d received, %d%% packet loss\n", sent, received,
           sent ? (sent - received) * 100 / sent : 0);
    if (received)
        printf("round-trip min/avg/max = %ld/%ld/%ld ms\n", tmin, tsum / received, tmax);
    return received ? 0 : 1;
}
