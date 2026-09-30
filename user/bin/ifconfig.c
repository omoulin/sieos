/*
 * ifconfig - show and set network interface configuration.
 *   ifconfig        the interfaces (lo, eth0, eth1, ...), IPv4 and IPv6
 *   ifconfig -r     resolv.conf lines for the DNS servers learnt (DHCP, router advertisements)
 *   ifconfig [-P] IF dhcp
 *   ifconfig [-P] IF inet ADDRESS [netmask MASK] [gateway GW] [dns DNS]
 *                   set IF's IPv4 settings, kept in /etc/network.conf
 *   ifconfig -a     apply /etc/network.conf (at boot, from /etc/rc)
 * Installed set-user-ID root: setting needs root, or root's password (asked
 * on the terminal; with -P the first line of standard input: Settings).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "sieos.h"
#include "sieos/sysinfo.h"
#include <arpa/inet.h>

#define CONF "/etc/network.conf"

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

static int if_index(const char *name)
{
    struct netinfo ni;
    for (int i = 0; netinfo_if(&ni, i) == 0; i++)
        if (!strcmp(ni.name, name))
            return i;
    return -1;
}

static bool ipv4(const char *s, unsigned *out)
{
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1)
        return false;
    *out = ntohl(a.s_addr);
    return true;
}

/* "IF dhcp" or "IF inet A [netmask M] [gateway G] [dns D]" (as words) into nc; the interface's name in ifname. */
static int parse(int argc, char **argv, struct sieos_netconfig *nc)
{
    memset(nc, 0, sizeof(*nc));
    if (argc < 2 || (nc->nc_index = if_index(argv[0])) < 0) {
        fprintf(stderr, "ifconfig: %s: no such interface\n", argc ? argv[0] : "");
        return -1;
    }
    if (!strcmp(argv[1], "dhcp") && argc == 2) {
        nc->nc_dhcp = 1;
        return 0;
    }
    if (strcmp(argv[1], "inet") || argc < 3 || !ipv4(argv[2], &nc->nc_ip))
        goto bad;
    for (int i = 3; i + 1 < argc; i += 2) {
        unsigned *f = !strcmp(argv[i], "netmask") ? &nc->nc_netmask : !strcmp(argv[i], "gateway") ? &nc->nc_gateway
                    : !strcmp(argv[i], "dns") ? &nc->nc_dns : NULL;
        if (!f || !ipv4(argv[i + 1], f))
            goto bad;
    }
    if (argc % 2 == 0)
        goto bad;
    return 0;
bad:
    fprintf(stderr, "usage: ifconfig IF dhcp | ifconfig IF inet ADDRESS [netmask M] [gateway G] [dns D]\n");
    return -1;
}

/* Keep "IF ..." as IF's line in /etc/network.conf. */
static void save(const char *ifname, const char *line)
{
    char buf[4096] = "", l[256];
    size_t n = strlen(ifname);
    FILE *f = fopen(CONF, "r");
    if (f) {
        while (fgets(l, sizeof(l), f))
            if (!(strncmp(l, ifname, n) == 0 && l[n] == ' ') && strlen(buf) + strlen(l) < sizeof(buf))
                strcat(buf, l);
        fclose(f);
    }
    if (!(f = fopen(CONF ".new", "w")))
        return;
    if (!strstr(buf, "# network.conf"))
        fprintf(f, "# network.conf - the interfaces' IPv4 settings, kept by ifconfig(1M)\n");
    fprintf(f, "%s%s\n", buf, line);
    fclose(f);
    chmod(CONF ".new", 0644);
    rename(CONF ".new", CONF);
}

static int apply_conf(void)
{
    FILE *f = fopen(CONF, "r");
    if (!f)
        return 0;
    char l[256];
    int r = 0;
    while (fgets(l, sizeof(l), f)) {
        l[strcspn(l, "\r\n")] = 0;
        if (!l[0] || l[0] == '#')
            continue;
        char *w[12], *save_p;
        int n = 0;
        for (char *t = strtok_r(l, " \t", &save_p); t && n < 12; t = strtok_r(NULL, " \t", &save_p))
            w[n++] = t;
        struct sieos_netconfig nc;
        if (parse(n, w, &nc) < 0 || netconfig(&nc) < 0) {
            fprintf(stderr, "ifconfig: %s: not applied: %s\n", CONF, n ? w[0] : "");
            r = 1;
        }
    }
    fclose(f);
    return r;
}

static bool root_password(bool from_stdin)
{
    if (getuid() == 0)
        return true;
    char line[256], *pass = NULL;
    if (from_stdin) {
        if (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line, "\n")] = 0;
            pass = line;
        }
    } else {
        pass = getpass("Root password: ");
    }
    struct spwd *sp = getspnam("root");
    const char *hash = sp ? sp->sp_pwdp : "!";
    bool ok = pass && hash[0] != '!' && hash[0] != '*' && check_password(pass, hash);
    memset(line, 0, sizeof(line));
    if (!ok) {
        sleep(1);
        fprintf(stderr, "ifconfig: wrong root password\n");
    }
    return ok;
}

int main(int argc, char **argv)
{
    setenv("PATH", "/sbin:/bin", 1);                 /* (set-user-ID) */
    if (argc > 1 && !strcmp(argv[1], "-a"))
        return geteuid() == 0 && getuid() == 0 ? apply_conf() : 1;
    int first = 1;
    bool pass_stdin = false;
    if (argc > 1 && !strcmp(argv[1], "-P"))
        pass_stdin = true, first = 2;
    if (argc > first && argv[first][0] != '-') {     /* set */
        struct sieos_netconfig nc;
        if (parse(argc - first, argv + first, &nc) < 0)
            return 2;
        if (!root_password(pass_stdin))
            return 1;
        if (netconfig(&nc) < 0) {
            fprintf(stderr, "ifconfig: %s: %s\n", argv[first], strerror(errno));
            return 1;
        }
        char line[256] = "";
        for (int i = first; i < argc; i++)
            snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s", i > first ? " " : "", argv[i]);
        save(argv[first], line);
        return 0;
    }
    setuid(getuid());                                /* (showing: no privileges needed) */
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
