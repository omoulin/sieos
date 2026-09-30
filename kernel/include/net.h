/*
 * net.h - The SIEOS TCP/IP stack.
 *
 * IPv4 addresses and ports are kept in host byte order inside the kernel
 * and converted at the wire and at the sockets API.  The transports (TCP,
 * UDP) name endpoints with 16-byte addresses (naddr_t, network order): IPv6
 * ones, or IPv4 ones mapped as ::ffff:a.b.c.d (net6.c has the IPv6 layer).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_NET_H
#define SIEOS_NET_H

#include "kernel.h"
#include "abi.h"

struct sieos_sockinfo6;
struct sieos_netinfo6;

struct netif;

/* A network card driver.  Each card it finds is registered as an interface
 * (netif_register) with its own driver state in ifp->drv. */
struct nic_ops {
    const char *name;
    int  (*send)(struct netif *ifp, const void *frame, size_t len);
    void (*poll)(struct netif *ifp);             /* take received frames (net_rx) */
    bool (*link)(struct netif *ifp);             /* optional: the link is up */
};

/* An IPv6 address, or an IPv4 one as ::ffff:a.b.c.d (network byte order). */
typedef struct { uint8_t b[16]; } naddr_t;

static inline naddr_t na_v4(uint32_t ip)
{
    naddr_t a = { { 0 } };
    a.b[10] = a.b[11] = 0xFF;
    a.b[12] = ip >> 24;
    a.b[13] = ip >> 16;
    a.b[14] = ip >> 8;
    a.b[15] = ip;
    return a;
}
static inline bool na_is_v4(const naddr_t *a)
{
    for (int i = 0; i < 10; i++)
        if (a->b[i])
            return false;
    return a->b[10] == 0xFF && a->b[11] == 0xFF;
}
static inline uint32_t na_to_v4(const naddr_t *a)
{
    return (uint32_t)a->b[12] << 24 | a->b[13] << 16 | a->b[14] << 8 | a->b[15];
}
static inline bool na_eq(const naddr_t *a, const naddr_t *b) { return !memcmp(a, b, 16); }
static inline bool na_zero(const naddr_t *a)
{
    for (int i = 0; i < 16; i++)
        if (a->b[i])
            return false;
    return true;
}
/* The unspecified address: :: (any family) or 0.0.0.0 (IPv4 only). */
static inline bool na_any(const naddr_t *a) { return na_zero(a) || (na_is_v4(a) && !na_to_v4(a)); }
static inline bool na_linklocal(const naddr_t *a) { return a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0x80; }
static inline bool na_multicast6(const naddr_t *a) { return a->b[0] == 0xFF; }
static inline bool na_loop6(const naddr_t *a)
{
    naddr_t l = { { 0 } };
    l.b[15] = 1;
    return na_eq(a, &l);
}

#define ETH_MTU_MAX 1500
#define NETIF_MAX 4
#define NARP 32
#define NPENDING 16
#define NND 32
#define NPEND6 16

struct l2_entry {                   /* a neighbor: ARP or IPv6 neighbor discovery */
    naddr_t ip;
    uint8_t mac[6];
    bool valid;
    uint64_t stamp;
};

struct l2_pending {                 /* a packet waiting for its neighbor's address */
    bool used;
    naddr_t nexthop;
    uint64_t since;
    size_t len;
    uint8_t pkt[ETH_MTU_MAX];
};

/* IPv6 on an interface (net6.c): a link-local address from the MAC, and one
 * global address by stateless autoconfiguration from a router advertisement. */
#define NET6_ADDRS 2
struct net6_state {
    bool up;                        /* link-local address configured */
    naddr_t addr[NET6_ADDRS];       /* [0] link-local, [1] global (zero if none) */
    int plen[NET6_ADDRS];
    naddr_t router, dns;            /* from the router advertisement (zero if none) */
    uint32_t mtu;
    int hoplimit;
    uint64_t rx_packets, tx_packets;
    bool tentative[NET6_ADDRS];     /* duplicate address detection is running (RFC 4862) */
    uint64_t dad_until[NET6_ADDRS];
    uint64_t duplicates;            /* addresses given up: another node had them */
    uint64_t mld_reports;
    /* neighbor discovery, MLD and router solicitation */
    struct l2_entry nd[NND];
    struct l2_pending pend[NPEND6];
    uint64_t mld_next;              /* the next report due, 0 none */
    int mld_left;                   /* unsolicited reports still to send */
    int rs_sent;
    uint64_t rs_next;
    bool dad_pending;               /* the link-local address waits for the link */
};

/*
 * A network interface: eth0, eth1, ... in the order the cards were found.
 * IPv4 routes: the interface whose subnet holds the destination, else the
 * default route of the first interface with a gateway (or, if the sender
 * chose a source address, that address's interface).  IPv6 likewise.
 */
struct netif {
    int index;
    char name[8];
    bool present, up, dhcp;
    uint8_t mac[6];
    uint32_t ip, netmask, gateway, dns;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
    const struct nic_ops *nic;
    void *drv;                      /* the driver's state for this card */
    /* ARP */
    struct l2_entry arp[NARP];
    struct l2_pending pending[NPENDING];
    /* DHCP client */
    int dhcp_state, dhcp_tries;
    uint32_t dhcp_xid, dhcp_offer_ip, dhcp_server;
    uint64_t dhcp_next_send;
    struct net6_state v6;
};
extern struct netif netifs[NETIF_MAX];
extern int nnetif;

/* A driver found a card: a new interface (NULL if there are NETIF_MAX already). */
struct netif *netif_register(const struct nic_ops *ops, void *drv, const uint8_t mac[6]);
struct netif *netif_by_index(int i);
void net_count_drop(void);                    /* a datagram dropped above the link layer */

static inline uint16_t bswap16(uint16_t v) { return (v >> 8) | (v << 8); }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
#define htons(x) bswap16(x)
#define ntohs(x) bswap16(x)
#define htonl(x) bswap32(x)
#define ntohl(x) bswap32(x)

#define ETH_HLEN   14
#define ETH_MTU    1500
#define ETH_P_IP   0x0800
#define ETH_P_ARP  0x0806
#define ETH_P_IPV6 0x86DD
#define IP_HLEN    20
#define IP6_HLEN   40
#define IPPROTO_IPV6_K   41
#define IPPROTO_ICMPV6_K 58

struct ip_hdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t len;
    uint16_t id;
    uint16_t frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t csum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

/* drivers: each registers the cards it finds */
void e1000_probe(void);                       /* (drv/e1000, drv/virtio_net: from their _init) */
void virtio_net_probe(void);

/* net.c */
void net_init(void);
void net_attach(struct netif *ifp);           /* an interface registered after net_init starts (DHCP, IPv6) */
int  net_configure(int index, bool dhcp, uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);
extern bool net_started;
bool net_wait_config(int max_ticks);
void net_poll(void);                          /* called every timer tick on the BSP */
void net_rx(struct netif *ifp, const uint8_t *frame, size_t len);
int  ip_send(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len);
void ip_input(struct netif *in, const uint8_t *pkt, size_t len);     /* in: NULL for loopback */
uint32_t csum_add(uint32_t sum, const void *data, size_t len);
uint16_t csum_fold(uint32_t sum);
uint32_t pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);
bool ip_is_local(uint32_t ip);
const char *ip_str(uint32_t ip, char *buf);

/* transports over either IP (net.c) */
int      net_send(const naddr_t *src, const naddr_t *dst, uint8_t proto, const void *payload, size_t len);
uint32_t net_pseudo_sum(const naddr_t *src, const naddr_t *dst, uint8_t proto, uint32_t len);
naddr_t  net_source(const naddr_t *dst);      /* the source address for talking to dst */
bool     net_is_local(const naddr_t *a);       /* one of ours (loopback included) */
size_t   net_payload_max(const naddr_t *dst);  /* MTU less the IP header */
void     net_loop(const void *pkt, size_t len); /* a packet to ourselves */
void     net_loop_drain(void);
extern volatile bool net_loop_pending;
extern int net_test_drop, net_test_reorder;    /* uadmin A_NETTEST: per mille of loopback packets */
bool     net_reachable(const naddr_t *dst);    /* configured to send there at all */

/* ipfrag.c: reassembly of fragmented datagrams */
struct frag_key {
    naddr_t src, dst;
    uint32_t id;
    uint8_t proto;                  /* IPv4's protocol (0 for IPv6) */
    bool v6;
};
/* 1: complete, *out (kmalloc'd, to kfree) holds *outlen bytes; 0: waiting for more; -1: dropped */
int  frag_add(const struct frag_key *k, uint32_t off, const uint8_t *data, size_t len, bool more, uint8_t **out,
              size_t *outlen);
void frag_tick(void);
extern uint64_t frag_reassembled, frag_dropped;

/* net6.c */
void net6_attach(struct netif *ifp);
void net6_tick(void);
void ip6_input(struct netif *in, const uint8_t *pkt, size_t len);
int  ip6_send(const naddr_t *src, const naddr_t *dst, uint8_t proto, const void *payload, size_t len);
naddr_t ip6_source(const naddr_t *dst);
bool ip6_is_local(const naddr_t *a);
const char *ip6_str(const naddr_t *a, char *buf);    /* buf: 40 bytes */
int  icmp6_send(const naddr_t *dst, const void *msg, size_t len);   /* raw ICMPv6: checksummed here */
void eth_send(struct netif *ifp, const uint8_t *dst, uint16_t type, const void *payload, size_t len);

/* socket.c */
struct socket;
struct tcb;
void udp_input(const naddr_t *src, const naddr_t *dst, const uint8_t *seg, size_t len);
void icmp_deliver_raw(const naddr_t *src, int proto, const uint8_t *msg, size_t len);
struct socket *socket_alloc(int type, int proto);
void socket_close(struct socket *s);
long socket_read(struct socket *s, void *buf, size_t n);
long socket_write(struct socket *s, const void *buf, size_t n);
bool socket_readable(struct socket *s);
bool socket_writable(struct socket *s);
bool socket_failed(struct socket *s);
void socket_wake(struct socket *s);
long socket_kopt(int fd, int which, bool set, int *val);
long socket_netinfo6(struct sieos_netinfo6 *u, long idx);
long socket_netstat6(struct sieos_sockinfo6 *u, int max);
long net_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);

/* dhcp (net.c) */
void dhcp_input(const uint8_t *msg, size_t len);

/* tcp.c */
void tcp_input(const naddr_t *src, const naddr_t *dst, const uint8_t *seg, size_t len);
void tcp_tick(void);
struct tcb *tcp_alloc(void);
int  tcp_connect(struct tcb *t, const naddr_t *lip, uint16_t lport, const naddr_t *rip, uint16_t rport,
                 bool nonblock);
int  tcp_listen(struct tcb *t, const naddr_t *lip, uint16_t lport, bool v6only);
long tcp_send(struct tcb *t, const void *buf, size_t n, bool nonblock);
long tcp_recv(struct tcb *t, void *buf, size_t n, bool nonblock, int timeout_ms);
void tcp_shutdown_write(struct tcb *t);
void tcp_close(struct tcb *t);
void tcp_abort(struct tcb *t);
int  tcp_state(struct tcb *t);
int  tcp_error(struct tcb *t);
int  tcp_take_error(struct tcb *t);
bool tcp_connecting(struct tcb *t);
bool tcp_readable(struct tcb *t);
bool tcp_writable(struct tcb *t);
struct tcb *tcp_accept_ready(struct tcb *listener);
void tcp_set_owner(struct tcb *t, struct socket *s);
void tcp_endpoints(struct tcb *t, naddr_t *lip, uint16_t *lport, naddr_t *rip, uint16_t *rport);
bool tcp_port_in_use(uint16_t port);
int  tcp_info(struct sieos_sockinfo6 *out, int max);
void tcp_queues(struct tcb *t, uint32_t *rx, uint32_t *tx);

#endif
