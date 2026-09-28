/*
 * net.h - The SIEOS TCP/IP stack.
 *
 * Addresses and ports are kept in host byte order inside the kernel and
 * converted at the wire and at the sockets API.
 */
#ifndef SIEOS_NET_H
#define SIEOS_NET_H

#include "kernel.h"
#include "abi.h"

struct nic_ops {
    const char *name;
    int  (*send)(const void *frame, size_t len);
    void (*poll)(void);
    bool (*link)(void);
};

struct netif_state {
    bool present, up, dhcp;
    uint8_t mac[6];
    uint32_t ip, netmask, gateway, dns;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
    const struct nic_ops *nic;
};
extern struct netif_state netif;

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
#define IP_HLEN    20

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

/* e1000.c */
const struct nic_ops *e1000_probe(void);

/* net.c */
void net_init(void);
bool net_wait_config(int max_ticks);
void net_poll(void);                          /* called every timer tick on the BSP */
void net_rx(const uint8_t *frame, size_t len);
int  ip_send(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len);
void ip_input(const uint8_t *pkt, size_t len);
uint32_t csum_add(uint32_t sum, const void *data, size_t len);
uint16_t csum_fold(uint32_t sum);
uint32_t pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);
bool ip_is_local(uint32_t ip);
const char *ip_str(uint32_t ip, char *buf);

/* socket.c */
struct socket;
struct tcb;
void udp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);
void icmp_deliver_raw(uint32_t src, const uint8_t *msg, size_t len);
struct socket *socket_alloc(int type, int proto);
void socket_close(struct socket *s);
long socket_read(struct socket *s, void *buf, size_t n);
long socket_write(struct socket *s, const void *buf, size_t n);
bool socket_readable(struct socket *s);
bool socket_writable(struct socket *s);
void socket_wake(struct socket *s);
long socket_kopt(int fd, int which, bool set, int *val);
long net_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);

/* dhcp (net.c) */
void dhcp_input(const uint8_t *msg, size_t len);

/* tcp.c */
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);
void tcp_tick(void);
struct tcb *tcp_alloc(void);
int  tcp_connect(struct tcb *t, uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport);
int  tcp_listen(struct tcb *t, uint32_t lip, uint16_t lport);
long tcp_send(struct tcb *t, const void *buf, size_t n, bool nonblock);
long tcp_recv(struct tcb *t, void *buf, size_t n, bool nonblock, int timeout_ms);
void tcp_shutdown_write(struct tcb *t);
void tcp_close(struct tcb *t);
void tcp_abort(struct tcb *t);
int  tcp_state(struct tcb *t);
int  tcp_error(struct tcb *t);
bool tcp_readable(struct tcb *t);
bool tcp_writable(struct tcb *t);
struct tcb *tcp_accept_ready(struct tcb *listener);
void tcp_set_owner(struct tcb *t, struct socket *s);
void tcp_endpoints(struct tcb *t, uint32_t *lip, uint16_t *lport, uint32_t *rip, uint16_t *rport);
bool tcp_port_in_use(uint16_t port);
int  tcp_info(struct sockinfo *out, int max, int start);
void tcp_queues(struct tcb *t, uint32_t *rx, uint32_t *tx);

#endif
