/*
 * net.c - Network interfaces; Ethernet, ARP, IPv4, ICMP, loopback and the
 * DHCP client, and the transports' view of both IP versions (net_send and
 * friends).
 *
 * Drivers register each card they find as an interface (eth0, eth1, ...).
 * Every interface has its own addresses, ARP cache and DHCP client.  An
 * IPv4 datagram leaves by the interface whose subnet holds its destination,
 * else by the default route: the interface of the source address the
 * sender chose, if it has a gateway, otherwise the first interface that
 * has one.  An address of any interface is local (the weak host model).
 *
 * Everything runs under the big kernel lock.  Received frames are pulled
 * from the cards by net_poll(), called from the PIT tick on the BSP (and at
 * their interrupts); this also drives the ARP, DHCP and TCP timers.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "net.h"
#include "mm.h"
#include "proc.h"

struct netif netifs[NETIF_MAX];
int nnetif;

static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

enum { DHCP_OFF, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND };

struct netif *netif_register(const struct nic_ops *ops, void *drv, const uint8_t mac[6])
{
    if (nnetif == NETIF_MAX)
        return NULL;
    struct netif *ifp = &netifs[nnetif];
    memset(ifp, 0, sizeof(*ifp));
    ifp->index = nnetif++;
    snprintf(ifp->name, sizeof(ifp->name), "eth%d", ifp->index);
    ifp->nic = ops;
    ifp->drv = drv;
    memcpy(ifp->mac, mac, 6);
    ifp->present = true;
    return ifp;
}

struct netif *netif_by_index(int i)
{
    return i >= 0 && i < nnetif ? &netifs[i] : NULL;
}

void net_count_drop(void)
{
    if (nnetif)
        netifs[0].rx_dropped++;
}

/* ------------------------------------------------------------------ */
/* Checksums and helpers                                               */
/* ------------------------------------------------------------------ */

uint32_t csum_add(uint32_t sum, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 1) {
        sum += (p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += p[0] << 8;
    return sum;
}

uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

uint32_t pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len)
{
    uint32_t s = 0;
    s += src >> 16;
    s += src & 0xFFFF;
    s += dst >> 16;
    s += dst & 0xFFFF;
    s += proto;
    s += len;
    return s;
}

const char *ip_str(uint32_t ip, char *buf)
{
    snprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return buf;
}

/* The interface that has this IPv4 address, or NULL. */
static struct netif *ifp_of_ip(uint32_t ip)
{
    for (int i = 0; ip && i < nnetif; i++)
        if (netifs[i].ip == ip)
            return &netifs[i];
    return NULL;
}

bool ip_is_local(uint32_t ip)
{
    return (ip >> 24) == 127 || ifp_of_ip(ip);
}

static bool subnet_bcast(const struct netif *ifp, uint32_t dst)
{
    return ifp->netmask && ifp->ip && (dst | ifp->netmask) == 0xFFFFFFFF &&
           ((dst ^ ifp->ip) & ifp->netmask) == 0;
}

/*
 * The interface and next hop for dst; src is the source the sender chose
 * (0: any).  NULL: no route.
 */
static struct netif *route4(uint32_t dst, uint32_t src, uint32_t *nexthop)
{
    struct netif *pref = ifp_of_ip(src);
    for (int i = 0; i < nnetif; i++) {             /* on a link: the subnet holds it */
        struct netif *ifp = &netifs[i];
        if (ifp->up && ifp->ip && ((dst ^ ifp->ip) & ifp->netmask) == 0 && (!pref || pref == ifp)) {
            *nexthop = dst;
            return ifp;
        }
    }
    if (pref && pref->up && pref->gateway) {
        *nexthop = pref->gateway;
        return pref;
    }
    for (int i = 0; i < nnetif; i++)               /* the default route: the first gateway */
        if (netifs[i].up && netifs[i].gateway) {
            *nexthop = netifs[i].gateway;
            return &netifs[i];
        }
    return NULL;
}

/* ---------------- either IP version, for the transports ---------------- */

int net_send(const naddr_t *src, const naddr_t *dst, uint8_t proto, const void *payload, size_t len)
{
    return net_send_opts(src, dst, proto, payload, len, 0);
}

/* hltc: for IPv6, the hop limit (0: the default) | the traffic class << 8 */
int net_send_opts(const naddr_t *src, const naddr_t *dst, uint8_t proto, const void *payload, size_t len, int hltc)
{
    if (na_is_v4(dst))
        return ip_send(na_is_v4(src) ? na_to_v4(src) : 0, na_to_v4(dst), proto, payload, len);
    return ip6_send_opts(src, dst, proto, payload, len, hltc);
}

uint32_t net_pseudo_sum(const naddr_t *src, const naddr_t *dst, uint8_t proto, uint32_t len)
{
    if (na_is_v4(dst))
        return pseudo_sum(na_to_v4(src), na_to_v4(dst), proto, len);
    uint32_t s = csum_add(csum_add(0, src, 16), dst, 16);   /* RFC 8200 8.1 */
    return s + (len >> 16) + (len & 0xFFFF) + proto;
}

/* The source address for an IPv4 destination (0 if there is no route). */
static uint32_t source4(uint32_t d)
{
    if ((d >> 24) == 127)
        return d;
    if (ifp_of_ip(d))
        return d;                                   /* ourselves, by another interface's address */
    uint32_t nh;
    struct netif *ifp = route4(d, 0, &nh);
    if (!ifp && nnetif && d == INADDR_BROADCAST)
        ifp = &netifs[0];
    return ifp ? ifp->ip : 0;
}

naddr_t net_source(const naddr_t *dst)
{
    if (!na_is_v4(dst))
        return ip6_source(dst);
    return na_v4(source4(na_to_v4(dst)));
}

bool net_is_local(const naddr_t *a)
{
    return na_is_v4(a) ? ip_is_local(na_to_v4(a)) : ip6_is_local(a);
}

size_t net_payload_max(const naddr_t *dst)
{
    if (na_is_v4(dst))
        return ETH_MTU - IP_HLEN;
    uint32_t mtu = ETH_MTU;
    for (int i = 0; i < nnetif; i++)
        if (netifs[i].v6.up && netifs[i].v6.mtu)
            mtu = MIN(mtu, netifs[i].v6.mtu);
    return mtu - IP6_HLEN;
}

bool net_reachable(const naddr_t *dst)
{
    if (net_is_local(dst))
        return true;
    if (na_is_v4(dst)) {
        uint32_t nh;
        return route4(na_to_v4(dst), 0, &nh) || na_to_v4(dst) == INADDR_BROADCAST;
    }
    for (int i = 0; i < nnetif; i++)
        if (netifs[i].v6.up)
            return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Link layer                                                          */
/* ------------------------------------------------------------------ */

void eth_send(struct netif *ifp, const uint8_t *dst, uint16_t type, const void *payload, size_t len)
{
    if (!ifp || !ifp->nic || len > ETH_MTU)
        return;
    uint8_t frame[ETH_HLEN + ETH_MTU];
    if (type == ETH_P_IPV6)
        ifp->v6.tx_packets++;
    memcpy(frame, dst, 6);
    memcpy(frame + 6, ifp->mac, 6);
    frame[12] = type >> 8;
    frame[13] = type & 0xFF;
    memcpy(frame + ETH_HLEN, payload, len);
    if (ifp->nic->send(ifp, frame, ETH_HLEN + len) == 0) {
        ifp->tx_packets++;
        ifp->tx_bytes += ETH_HLEN + len;
    }
}

/* ---------------- ARP ---------------- */

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed));

static void arp_update(struct netif *ifp, uint32_t ip, const uint8_t *mac)
{
    naddr_t a = na_v4(ip);
    int slot = -1, oldest = 0;
    for (int i = 0; i < NARP; i++) {
        if (ifp->arp[i].valid && na_eq(&ifp->arp[i].ip, &a)) {
            slot = i;
            break;
        }
        if (!ifp->arp[i].valid && slot < 0)
            slot = i;
        if (ifp->arp[i].stamp < ifp->arp[oldest].stamp)
            oldest = i;
    }
    if (slot < 0)
        slot = oldest;
    ifp->arp[slot].ip = a;
    memcpy(ifp->arp[slot].mac, mac, 6);
    ifp->arp[slot].valid = true;
    ifp->arp[slot].stamp = ticks;
    /* Flush packets waiting for this neighbour. */
    for (int i = 0; i < NPENDING; i++) {
        struct l2_pending *p = &ifp->pending[i];
        if (p->used && na_eq(&p->nexthop, &a)) {
            eth_send(ifp, mac, ETH_P_IP, p->pkt, p->len);
            p->used = false;
        }
    }
}

static bool arp_lookup(struct netif *ifp, uint32_t ip, uint8_t *mac)
{
    naddr_t a = na_v4(ip);
    for (int i = 0; i < NARP; i++)
        if (ifp->arp[i].valid && na_eq(&ifp->arp[i].ip, &a)) {
            memcpy(mac, ifp->arp[i].mac, 6);
            return true;
        }
    return false;
}

static void arp_send(struct netif *ifp, uint16_t op, const uint8_t *tha, uint32_t tpa)
{
    struct arp_pkt a;
    a.htype = htons(1);
    a.ptype = htons(ETH_P_IP);
    a.hlen = 6;
    a.plen = 4;
    a.op = htons(op);
    memcpy(a.sha, ifp->mac, 6);
    a.spa = htonl(ifp->ip);
    memcpy(a.tha, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : tha, 6);
    a.tpa = htonl(tpa);
    eth_send(ifp, op == 1 ? bcast_mac : tha, ETH_P_ARP, &a, sizeof(a));
}

static void arp_input(struct netif *ifp, const uint8_t *p, size_t len)
{
    if (len < sizeof(struct arp_pkt))
        return;
    const struct arp_pkt *a = (const struct arp_pkt *)p;
    if (ntohs(a->htype) != 1 || ntohs(a->ptype) != ETH_P_IP)
        return;
    uint32_t spa = ntohl(a->spa), tpa = ntohl(a->tpa);
    if (spa)
        arp_update(ifp, spa, a->sha);
    if (ntohs(a->op) == 1 && ifp->ip && tpa == ifp->ip)
        arp_send(ifp, 2, a->sha, spa);
}

/* ------------------------------------------------------------------ */
/* IPv4                                                                */
/* ------------------------------------------------------------------ */

#define NLOOP 256                       /* a full 128 KB window of segments and their ACKs */
static struct {
    size_t len;
    uint8_t pkt[ETH_MTU];
} loopq[NLOOP];
static uint32_t loop_head, loop_tail;
static uint16_t ip_ident = 1;

/* A finished packet out of ifp: broadcast, or to the next hop (queued for ARP). */
static int ip_emit(struct netif *ifp, uint32_t dst, uint32_t nexthop, const uint8_t *pkt, size_t total)
{
    if (!ifp->nic)
        return -ENETDOWN;
    if (dst == INADDR_BROADCAST || subnet_bcast(ifp, dst)) {
        eth_send(ifp, bcast_mac, ETH_P_IP, pkt, total);
        return 0;
    }
    if (!ifp->ip || !nexthop)
        return -ENETUNREACH;
    uint8_t mac[6];
    if (arp_lookup(ifp, nexthop, mac)) {
        eth_send(ifp, mac, ETH_P_IP, pkt, total);
        return 0;
    }
    /* Queue until the neighbour answers our ARP request. */
    for (int i = 0; i < NPENDING; i++) {
        struct l2_pending *p = &ifp->pending[i];
        if (!p->used) {
            p->used = true;
            p->nexthop = na_v4(nexthop);
            p->since = ticks;
            p->len = total;
            memcpy(p->pkt, pkt, total);
            break;
        }
    }
    arp_send(ifp, 1, NULL, nexthop);
    return 0;
}

volatile bool net_loop_pending;

/* Deliver the packets sent to ourselves (from net_poll, and on the way out of a system call). */
void net_loop_drain(void)
{
    net_loop_pending = false;
    for (int budget = 0; loop_head != loop_tail && budget < NLOOP; budget++) {
        uint32_t i = loop_head++ % NLOOP;
        if (loopq[i].len && (loopq[i].pkt[0] >> 4) == 6)
            ip6_input(NULL, loopq[i].pkt, loopq[i].len);
        else
            ip_input(NULL, loopq[i].pkt, loopq[i].len);
    }
    if (loop_head != loop_tail)
        net_loop_pending = true;
}

int net_test_drop, net_test_reorder;
static uint8_t held[ETH_MTU];             /* A_NETTEST: a packet delayed behind the next one */
static size_t held_len;

static uint32_t test_rand(void)
{
    static uint32_t x = 2463534242U;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static void loop_put(const void *pkt, size_t len);

static void ip_header(uint8_t *pkt, uint32_t src, uint32_t dst, uint8_t proto, size_t len, uint16_t id, uint16_t frag)
{
    struct ip_hdr *h = (struct ip_hdr *)pkt;
    h->ver_ihl = 0x45;
    h->tos = 0;
    h->len = htons(IP_HLEN + len);
    h->id = htons(id);
    h->frag = htons(frag);
    h->ttl = 64;
    h->proto = proto;
    h->csum = 0;
    h->src = htonl(src);
    h->dst = htonl(dst);
    h->csum = csum_fold(csum_add(0, h, IP_HLEN));
}

/*
 * An IPv4 datagram by ifp (NULL: to ourselves, by loopback).  One that fits
 * the MTU goes whole, with "don't fragment"; a larger one (UDP, ICMP, up to
 * 65,515 bytes) in fragments of 1,480 bytes, the loopback included.
 */
static int ip_send_via(struct netif *ifp, uint32_t nexthop, uint32_t src, uint32_t dst, uint8_t proto,
                       const void *payload, size_t len)
{
    if (len > 65535 - IP_HLEN)
        return -EMSGSIZE;
    uint8_t pkt[ETH_MTU];
    uint16_t id = ip_ident++;
    if (len + IP_HLEN <= ETH_MTU) {
        ip_header(pkt, src, dst, proto, len, id, 0x4000);        /* don't fragment */
        memcpy(pkt + IP_HLEN, payload, len);
        if (!ifp) {
            if (loop_tail - loop_head == NLOOP)
                return -ENOBUFS;
            net_loop(pkt, IP_HLEN + len);
            return 0;
        }
        return ip_emit(ifp, dst, nexthop, pkt, IP_HLEN + len);
    }
    if (proto == IPPROTO_TCP)
        return -EMSGSIZE;                        /* (TCP segments fit: the MSS) */
    const uint32_t per = (ETH_MTU - IP_HLEN) & ~7U;
    for (uint32_t off = 0; off < len; off += per) {
        uint32_t n = MIN(per, (uint32_t)(len - off));
        bool more = off + n < len;
        ip_header(pkt, src, dst, proto, n, id, (off / 8) | (more ? 0x2000 : 0));
        memcpy(pkt + IP_HLEN, (const uint8_t *)payload + off, n);
        int r;
        if (!ifp) {
            if (loop_tail - loop_head == NLOOP)
                return -ENOBUFS;
            net_loop(pkt, IP_HLEN + n);
            r = 0;
        } else {
            r = ip_emit(ifp, dst, nexthop, pkt, IP_HLEN + n);
        }
        if (r < 0)
            return r;
    }
    return 0;
}

int ip_send(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len)
{
    if (ip_is_local(dst)) {                      /* loopback */
        if (!src)
            src = dst;                           /* 127.x, or one of our addresses */
        return ip_send_via(NULL, 0, src, dst, proto, payload, len);
    }
    uint32_t nexthop = 0;
    struct netif *ifp;
    if (dst == INADDR_BROADCAST) {
        ifp = ifp_of_ip(src);
        if (!ifp && nnetif)
            ifp = &netifs[0];
    } else {
        ifp = route4(dst, src, &nexthop);
        for (int i = 0; !ifp && i < nnetif; i++)   /* a subnet broadcast */
            if (subnet_bcast(&netifs[i], dst))
                ifp = &netifs[i];
    }
    if (!ifp)
        return nnetif ? -ENETUNREACH : -ENETDOWN;
    if (!src)
        src = ifp->ip;
    return ip_send_via(ifp, nexthop, src, dst, proto, payload, len);
}

/* Queue a packet (either IP version) for input: at the end of the system call, or the next poll. */
void net_loop(const void *pkt, size_t len)
{
    if (net_test_drop && test_rand() % 1000 < (uint32_t)net_test_drop)
        return;                               /* testing: lost */
    if (net_test_reorder && !held_len && len <= sizeof(held) && test_rand() % 1000 < (uint32_t)net_test_reorder) {
        memcpy(held, pkt, len);               /* testing: it goes after the next one */
        held_len = len;
        return;
    }
    loop_put(pkt, len);
    if (held_len) {
        size_t n = held_len;
        held_len = 0;
        loop_put(held, n);
    }
}

static void loop_put(const void *pkt, size_t len)
{
    if (loop_tail - loop_head == NLOOP || len > ETH_MTU) {
        net_count_drop();
        return;
    }
    loopq[loop_tail % NLOOP].len = len;
    memcpy(loopq[loop_tail % NLOOP].pkt, pkt, len);
    loop_tail++;
    net_loop_pending = true;
}

static void icmp_input(uint32_t src, uint32_t dst, const uint8_t *msg, size_t len)
{
    if (len < 8 || csum_fold(csum_add(0, msg, len)) != 0)
        return;
    if (msg[0] == 8 && msg[1] == 0) {            /* echo request -> echo reply */
        uint8_t *reply = kmalloc(len);
        if (reply) {
            memcpy(reply, msg, len);
            reply[0] = 0;
            reply[2] = reply[3] = 0;
            uint16_t c = csum_fold(csum_add(0, reply, len));
            memcpy(reply + 2, &c, 2);
            ip_send(ip_is_local(dst) ? dst : 0, src, IPPROTO_ICMP, reply, len);
            kfree(reply);
        }
    }
    naddr_t s = na_v4(src);
    icmp_deliver_raw(&s, IPPROTO_ICMP, msg, len);
}

void ip_input(struct netif *in, const uint8_t *pkt, size_t len)
{
    if (len < IP_HLEN)
        return;
    const struct ip_hdr *h = (const struct ip_hdr *)pkt;
    size_t ihl = (h->ver_ihl & 15) * 4;
    size_t total = ntohs(h->len);
    if ((h->ver_ihl >> 4) != 4 || ihl < IP_HLEN || total > len || total < ihl)
        return;
    if (csum_fold(csum_add(0, h, ihl)) != 0)
        return;
    uint16_t frag = ntohs(h->frag);
    uint32_t src = ntohl(h->src), dst = ntohl(h->dst);
    bool for_us = ip_is_local(dst) || dst == INADDR_BROADCAST ||
                  (in && (!in->ip || subnet_bcast(in, dst)));
    if (!for_us)
        return;
    const uint8_t *payload = pkt + ihl;
    size_t plen = total - ihl;
    naddr_t s = na_v4(src), d = na_v4(dst);
    uint8_t *whole = NULL;
    if (frag & 0x3FFF) {                         /* a fragment: reassemble */
        struct frag_key k = { s, d, ntohs(h->id), h->proto, false };
        size_t n;
        if (frag_add(&k, (frag & 0x1FFF) * 8, payload, plen, frag & 0x2000, &whole, &n) != 1)
            return;
        payload = whole;
        plen = n;
    }
    switch (h->proto) {
    case IPPROTO_ICMP: icmp_input(src, dst, payload, plen); break;
    case IPPROTO_UDP:  udp_input(&s, &d, payload, plen, h->ttl, h->tos); break;
    case IPPROTO_TCP:  tcp_input(&s, &d, payload, plen); break;
    }
    kfree(whole);
}

void net_rx(struct netif *ifp, const uint8_t *frame, size_t len)
{
    if (len < ETH_HLEN)
        return;
    ifp->rx_packets++;
    ifp->rx_bytes += len;
    uint16_t type = (frame[12] << 8) | frame[13];
    if (type == ETH_P_ARP)
        arp_input(ifp, frame + ETH_HLEN, len - ETH_HLEN);
    else if (type == ETH_P_IP)
        ip_input(ifp, frame + ETH_HLEN, len - ETH_HLEN);
    else if (type == ETH_P_IPV6)
        ip6_input(ifp, frame + ETH_HLEN, len - ETH_HLEN);
}

/* ------------------------------------------------------------------ */
/* DHCP client, one per interface                                      */
/* ------------------------------------------------------------------ */

struct bootp {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64], file[128];
    uint32_t magic;
    uint8_t options[312];
} __attribute__((packed));

static void dhcp_send(struct netif *ifp, int type)
{
    struct bootp b;
    memset(&b, 0, sizeof(b));
    b.op = 1;
    b.htype = 1;
    b.hlen = 6;
    b.xid = htonl(ifp->dhcp_xid);
    b.flags = htons(0x8000);                     /* replies by broadcast */
    memcpy(b.chaddr, ifp->mac, 6);
    b.magic = htonl(0x63825363);
    uint8_t *o = b.options;
    *o++ = 53; *o++ = 1; *o++ = type;            /* message type */
    *o++ = 61; *o++ = 7; *o++ = 1;               /* client identifier */
    memcpy(o, ifp->mac, 6);
    o += 6;
    if (type == 3) {                             /* REQUEST */
        uint32_t ip = htonl(ifp->dhcp_offer_ip), srv = htonl(ifp->dhcp_server);
        *o++ = 50; *o++ = 4; memcpy(o, &ip, 4); o += 4;
        *o++ = 54; *o++ = 4; memcpy(o, &srv, 4); o += 4;
    }
    *o++ = 12; *o++ = 5; memcpy(o, "sieos", 5); o += 5;   /* host name */
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;    /* want mask, router, dns */
    *o++ = 255;

    /* UDP 68 -> 67 by hand (no socket needed), from 0.0.0.0 on this interface */
    uint8_t seg[8 + sizeof(b)];
    size_t len = 8 + sizeof(b);
    seg[0] = 0; seg[1] = 68; seg[2] = 0; seg[3] = 67;
    seg[4] = len >> 8; seg[5] = len & 0xFF; seg[6] = seg[7] = 0;
    memcpy(seg + 8, &b, sizeof(b));
    uint16_t c = csum_fold(csum_add(pseudo_sum(0, INADDR_BROADCAST, IPPROTO_UDP, len), seg, len));
    memcpy(seg + 6, &c, 2);
    ip_send_via(ifp, 0, 0, INADDR_BROADCAST, IPPROTO_UDP, seg, len);
}

void dhcp_input(const uint8_t *msg, size_t len)
{
    if (len < 240)
        return;
    const struct bootp *b = (const struct bootp *)msg;
    struct netif *ifp = NULL;
    for (int i = 0; i < nnetif; i++)             /* the interface asking: by transaction id */
        if (netifs[i].dhcp_xid == ntohl(b->xid))
            ifp = &netifs[i];
    if (!ifp || ifp->dhcp_state == DHCP_OFF || ifp->dhcp_state == DHCP_BOUND)
        return;
    if (b->op != 2 || ntohl(b->magic) != 0x63825363 || memcmp(b->chaddr, ifp->mac, 6))
        return;
    int type = 0;
    uint32_t mask = 0, router = 0, dns = 0, server = 0;
    const uint8_t *o = b->options, *end = msg + len;
    while (o < end && *o != 255) {
        if (*o == 0) {
            o++;
            continue;
        }
        if (o + 2 > end || o + 2 + o[1] > end)
            break;
        uint8_t code = o[0], l = o[1];
        const uint8_t *v = o + 2;
        uint32_t word = l >= 4 ? ((uint32_t)v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]) : 0;
        if (code == 53 && l >= 1) type = v[0];
        if (code == 1) mask = word;
        if (code == 3) router = word;
        if (code == 6) dns = word;
        if (code == 54) server = word;
        o += 2 + l;
    }
    if (type == 2 && ifp->dhcp_state == DHCP_SELECTING) {          /* OFFER */
        ifp->dhcp_offer_ip = ntohl(b->yiaddr);
        ifp->dhcp_server = server;
        ifp->dhcp_state = DHCP_REQUESTING;
        ifp->dhcp_tries = 0;
        dhcp_send(ifp, 3);
        ifp->dhcp_next_send = ticks + TIMER_HZ;
    } else if (type == 5 && ifp->dhcp_state == DHCP_REQUESTING) {  /* ACK */
        ifp->ip = ntohl(b->yiaddr);
        ifp->netmask = mask ? mask : 0xFFFFFF00;
        ifp->gateway = router;
        ifp->dns = dns ? dns : router;
        ifp->dhcp = true;
        ifp->up = true;
        ifp->dhcp_state = DHCP_BOUND;
    } else if (type == 6) {                                        /* NAK */
        ifp->dhcp_state = DHCP_SELECTING;
    }
}

static void dhcp_tick(struct netif *ifp)
{
    if (ifp->dhcp_state != DHCP_SELECTING && ifp->dhcp_state != DHCP_REQUESTING)
        return;
    if (ticks < ifp->dhcp_next_send)
        return;
    if (++ifp->dhcp_tries > 6 && ifp->dhcp_state == DHCP_REQUESTING)
        ifp->dhcp_state = DHCP_SELECTING;
    dhcp_send(ifp, ifp->dhcp_state == DHCP_SELECTING ? 1 : 3);
    ifp->dhcp_next_send = ticks + TIMER_HZ * (ifp->dhcp_tries < 4 ? 1 : 4);
}

/* ------------------------------------------------------------------ */
/* Setup and polling                                                   */
/* ------------------------------------------------------------------ */

bool net_started;

/* An interface's IPv4 settings: DHCP again, or static ones (netconfig). */
int net_configure(int index, bool dhcp, uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns)
{
    struct netif *ifp = netif_by_index(index);
    if (!ifp)
        return -ENODEV;
    if (dhcp) {
        ifp->up = false;
        ifp->dhcp = false;
        ifp->ip = ifp->gateway = 0;
        ifp->dhcp_xid = (uint32_t)(ticks * 2654435761U) ^ ifp->mac[5] ^ 0x5EE05;
        ifp->dhcp_state = DHCP_SELECTING;
        ifp->dhcp_next_send = 0;
        return 0;
    }
    if (!ip || (mask && (~mask & (~mask + 1))))    /* (a netmask is ones then zeros) */
        return -EINVAL;
    ifp->dhcp_state = DHCP_OFF;
    ifp->dhcp = false;
    ifp->ip = ip;
    ifp->netmask = mask ? mask : 0xFFFFFF00;
    ifp->gateway = gw;
    ifp->dns = dns;
    ifp->up = true;
    return 0;
}

/* An interface starts: DHCP, IPv6 (at boot, and for a card plugged in later: a USB adapter). */
void net_attach(struct netif *ifp)
{
    ifp->dhcp_xid = (uint32_t)(ticks * 2654435761U) ^ (ifp->mac[5] << 8) ^ (ifp->mac[4] << 16) ^ 0xA1E05 ^ ifp->index;
    ifp->dhcp_state = DHCP_SELECTING;
    ifp->dhcp_next_send = 0;
    net6_attach(ifp);
}

void net_init(void)
{
    for (int i = 0; i < nnetif; i++)             /* (the cards' drivers registered them: DDI_PHASE_ROOT) */
        net_attach(&netifs[i]);
    net_started = true;
}

/* Wait (at boot) for DHCP on every interface; eth0 falls back to the QEMU user-network defaults. */
bool net_wait_config(int max_ticks)
{
    if (!nnetif)
        return false;
    uint64_t end = ticks + max_ticks;
    for (;;) {
        bool all = true;
        for (int i = 0; i < nnetif; i++)
            all &= netifs[i].up;
        if (all || ticks >= end)
            break;
        sti();
        hlt();
        cli();
    }
    struct netif *e0 = &netifs[0];
    if (!e0->up) {
        e0->dhcp_state = DHCP_OFF;
        e0->ip = 0x0A00020F;            /* 10.0.2.15 */
        e0->netmask = 0xFFFFFF00;
        e0->gateway = 0x0A000202;
        e0->dns = 0x0A000203;
        e0->up = true;
    }
    return e0->dhcp;
}

void net_poll(void)
{
    for (int i = 0; i < nnetif; i++)
        if (netifs[i].nic)
            netifs[i].nic->poll(&netifs[i]);
    net_loop_drain();
    for (int k = 0; k < nnetif; k++) {
        struct netif *ifp = &netifs[k];
        /* expire ARP-pending packets after 3 s (and re-ask once a second) */
        for (int i = 0; i < NPENDING; i++) {
            struct l2_pending *p = &ifp->pending[i];
            if (!p->used)
                continue;
            uint64_t age = ticks - p->since;
            if (age > 3 * TIMER_HZ)
                p->used = false;
            else if (age && age % TIMER_HZ == 0)
                arp_send(ifp, 1, NULL, na_to_v4(&p->nexthop));
        }
        dhcp_tick(ifp);
    }
    net6_tick();
    frag_tick();
    tcp_tick();
}
