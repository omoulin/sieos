/*
 * net.c - Ethernet, ARP, IPv4, ICMP, loopback and the DHCP client.
 *
 * Everything runs under the big kernel lock.  Received frames are pulled
 * from the NIC by net_poll(), called from the PIT tick on the BSP; this
 * also drives the ARP, DHCP and TCP timers.
 */
#include "net.h"
#include "mm.h"
#include "proc.h"

struct netif_state netif;

static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

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

bool ip_is_local(uint32_t ip)
{
    return (ip >> 24) == 127 || (netif.ip && ip == netif.ip);
}

/* ------------------------------------------------------------------ */
/* Link layer                                                          */
/* ------------------------------------------------------------------ */

static void eth_send(const uint8_t *dst, uint16_t type, const void *payload, size_t len)
{
    if (!netif.nic || len > ETH_MTU)
        return;
    uint8_t frame[ETH_HLEN + ETH_MTU];
    memcpy(frame, dst, 6);
    memcpy(frame + 6, netif.mac, 6);
    frame[12] = type >> 8;
    frame[13] = type & 0xFF;
    memcpy(frame + ETH_HLEN, payload, len);
    if (netif.nic->send(frame, ETH_HLEN + len) == 0) {
        netif.tx_packets++;
        netif.tx_bytes += ETH_HLEN + len;
    }
}

/* ---------------- ARP ---------------- */

#define NARP 32
#define NPENDING 16

static struct {
    uint32_t ip;
    uint8_t mac[6];
    bool valid;
    uint64_t stamp;
} arp_cache[NARP];

static struct {
    bool used;
    uint32_t nexthop;
    uint64_t since;
    size_t len;
    uint8_t pkt[ETH_MTU];
} pending[NPENDING];

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed));

static void arp_update(uint32_t ip, const uint8_t *mac)
{
    int slot = -1, oldest = 0;
    for (int i = 0; i < NARP; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            slot = i;
            break;
        }
        if (!arp_cache[i].valid && slot < 0)
            slot = i;
        if (arp_cache[i].stamp < arp_cache[oldest].stamp)
            oldest = i;
    }
    if (slot < 0)
        slot = oldest;
    arp_cache[slot].ip = ip;
    memcpy(arp_cache[slot].mac, mac, 6);
    arp_cache[slot].valid = true;
    arp_cache[slot].stamp = ticks;
    /* Flush packets waiting for this neighbour. */
    for (int i = 0; i < NPENDING; i++) {
        if (pending[i].used && pending[i].nexthop == ip) {
            eth_send(mac, ETH_P_IP, pending[i].pkt, pending[i].len);
            pending[i].used = false;
        }
    }
}

static bool arp_lookup(uint32_t ip, uint8_t *mac)
{
    for (int i = 0; i < NARP; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac, arp_cache[i].mac, 6);
            return true;
        }
    return false;
}

static void arp_send(uint16_t op, const uint8_t *tha, uint32_t tpa)
{
    struct arp_pkt a;
    a.htype = htons(1);
    a.ptype = htons(ETH_P_IP);
    a.hlen = 6;
    a.plen = 4;
    a.op = htons(op);
    memcpy(a.sha, netif.mac, 6);
    a.spa = htonl(netif.ip);
    memcpy(a.tha, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : tha, 6);
    a.tpa = htonl(tpa);
    eth_send(op == 1 ? bcast_mac : tha, ETH_P_ARP, &a, sizeof(a));
}

static void arp_input(const uint8_t *p, size_t len)
{
    if (len < sizeof(struct arp_pkt))
        return;
    const struct arp_pkt *a = (const struct arp_pkt *)p;
    if (ntohs(a->htype) != 1 || ntohs(a->ptype) != ETH_P_IP)
        return;
    uint32_t spa = ntohl(a->spa), tpa = ntohl(a->tpa);
    if (spa)
        arp_update(spa, a->sha);
    if (ntohs(a->op) == 1 && netif.ip && tpa == netif.ip)
        arp_send(2, a->sha, spa);
}

/* ------------------------------------------------------------------ */
/* IPv4                                                                */
/* ------------------------------------------------------------------ */

#define NLOOP 32
static struct {
    size_t len;
    uint8_t pkt[ETH_MTU];
} loopq[NLOOP];
static uint32_t loop_head, loop_tail;
static uint16_t ip_ident = 1;

int ip_send(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len)
{
    if (len + IP_HLEN > ETH_MTU)
        return -EMSGSIZE;
    uint8_t pkt[ETH_MTU];
    struct ip_hdr *h = (struct ip_hdr *)pkt;
    if (!src)
        src = ip_is_local(dst) && (dst >> 24) == 127 ? dst : netif.ip;
    h->ver_ihl = 0x45;
    h->tos = 0;
    h->len = htons(IP_HLEN + len);
    h->id = htons(ip_ident++);
    h->frag = htons(0x4000);                     /* don't fragment */
    h->ttl = 64;
    h->proto = proto;
    h->csum = 0;
    h->src = htonl(src);
    h->dst = htonl(dst);
    h->csum = csum_fold(csum_add(0, h, IP_HLEN));
    memcpy(pkt + IP_HLEN, payload, len);
    size_t total = IP_HLEN + len;

    if (ip_is_local(dst)) {                      /* loopback */
        if (loop_tail - loop_head == NLOOP)
            return -ENOBUFS;
        loopq[loop_tail % NLOOP].len = total;
        memcpy(loopq[loop_tail % NLOOP].pkt, pkt, total);
        loop_tail++;
        return 0;
    }
    if (!netif.nic)
        return -ENETDOWN;
    if (dst == INADDR_BROADCAST || (netif.netmask && (dst | netif.netmask) == 0xFFFFFFFF)) {
        eth_send(bcast_mac, ETH_P_IP, pkt, total);
        return 0;
    }
    if (!netif.ip)
        return -ENETUNREACH;
    uint32_t nexthop = ((dst ^ netif.ip) & netif.netmask) == 0 ? dst : netif.gateway;
    if (!nexthop)
        return -ENETUNREACH;
    uint8_t mac[6];
    if (arp_lookup(nexthop, mac)) {
        eth_send(mac, ETH_P_IP, pkt, total);
        return 0;
    }
    /* Queue until the neighbour answers our ARP request. */
    for (int i = 0; i < NPENDING; i++) {
        if (!pending[i].used) {
            pending[i].used = true;
            pending[i].nexthop = nexthop;
            pending[i].since = ticks;
            pending[i].len = total;
            memcpy(pending[i].pkt, pkt, total);
            break;
        }
    }
    arp_send(1, NULL, nexthop);
    return 0;
}

static void icmp_input(uint32_t src, uint32_t dst, const uint8_t *msg, size_t len)
{
    if (len < 8 || csum_fold(csum_add(0, msg, len)) != 0)
        return;
    if (msg[0] == 8 && msg[1] == 0) {            /* echo request -> echo reply */
        uint8_t reply[ETH_MTU];
        memcpy(reply, msg, len);
        reply[0] = 0;
        reply[2] = reply[3] = 0;
        uint16_t c = csum_fold(csum_add(0, reply, len));
        memcpy(reply + 2, &c, 2);
        ip_send(dst == INADDR_BROADCAST ? 0 : dst, src, IPPROTO_ICMP, reply, len);
    }
    icmp_deliver_raw(src, msg, len);
}

void ip_input(const uint8_t *pkt, size_t len)
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
    if (ntohs(h->frag) & 0x3FFF)
        return;                                  /* fragments are not reassembled */
    uint32_t src = ntohl(h->src), dst = ntohl(h->dst);
    bool for_us = ip_is_local(dst) || dst == INADDR_BROADCAST || !netif.ip ||
                  (netif.netmask && (dst | netif.netmask) == 0xFFFFFFFF);
    if (!for_us)
        return;
    const uint8_t *payload = pkt + ihl;
    size_t plen = total - ihl;
    switch (h->proto) {
    case IPPROTO_ICMP: icmp_input(src, dst, payload, plen); break;
    case IPPROTO_UDP:  udp_input(src, dst, payload, plen); break;
    case IPPROTO_TCP:  tcp_input(src, dst, payload, plen); break;
    }
}

void net_rx(const uint8_t *frame, size_t len)
{
    if (len < ETH_HLEN)
        return;
    netif.rx_packets++;
    netif.rx_bytes += len;
    uint16_t type = (frame[12] << 8) | frame[13];
    if (type == ETH_P_ARP)
        arp_input(frame + ETH_HLEN, len - ETH_HLEN);
    else if (type == ETH_P_IP)
        ip_input(frame + ETH_HLEN, len - ETH_HLEN);
}

/* ------------------------------------------------------------------ */
/* DHCP client                                                         */
/* ------------------------------------------------------------------ */

enum { DHCP_OFF, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND };

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

static int dhcp_state;
static uint32_t dhcp_xid, dhcp_offer_ip, dhcp_server;
static uint64_t dhcp_next_send;
static int dhcp_tries;

static void dhcp_send(int type)
{
    struct bootp b;
    memset(&b, 0, sizeof(b));
    b.op = 1;
    b.htype = 1;
    b.hlen = 6;
    b.xid = htonl(dhcp_xid);
    b.flags = htons(0x8000);                     /* replies by broadcast */
    memcpy(b.chaddr, netif.mac, 6);
    b.magic = htonl(0x63825363);
    uint8_t *o = b.options;
    *o++ = 53; *o++ = 1; *o++ = type;            /* message type */
    *o++ = 61; *o++ = 7; *o++ = 1;               /* client identifier */
    memcpy(o, netif.mac, 6);
    o += 6;
    if (type == 3) {                             /* REQUEST */
        uint32_t ip = htonl(dhcp_offer_ip), srv = htonl(dhcp_server);
        *o++ = 50; *o++ = 4; memcpy(o, &ip, 4); o += 4;
        *o++ = 54; *o++ = 4; memcpy(o, &srv, 4); o += 4;
    }
    *o++ = 12; *o++ = 5; memcpy(o, "sieos", 5); o += 5;   /* host name */
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;    /* want mask, router, dns */
    *o++ = 255;

    /* UDP 68 -> 67 by hand (no socket needed) */
    uint8_t seg[8 + sizeof(b)];
    size_t len = 8 + sizeof(b);
    seg[0] = 0; seg[1] = 68; seg[2] = 0; seg[3] = 67;
    seg[4] = len >> 8; seg[5] = len & 0xFF; seg[6] = seg[7] = 0;
    memcpy(seg + 8, &b, sizeof(b));
    uint16_t c = csum_fold(csum_add(pseudo_sum(0, INADDR_BROADCAST, IPPROTO_UDP, len), seg, len));
    memcpy(seg + 6, &c, 2);
    uint32_t saved = netif.ip;
    netif.ip = 0;                                /* source 0.0.0.0 while unconfigured */
    ip_send(0, INADDR_BROADCAST, IPPROTO_UDP, seg, len);
    netif.ip = saved;
}

void dhcp_input(const uint8_t *msg, size_t len)
{
    if (len < 240 || dhcp_state == DHCP_OFF || dhcp_state == DHCP_BOUND)
        return;
    const struct bootp *b = (const struct bootp *)msg;
    if (b->op != 2 || ntohl(b->xid) != dhcp_xid || ntohl(b->magic) != 0x63825363)
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
    if (type == 2 && dhcp_state == DHCP_SELECTING) {          /* OFFER */
        dhcp_offer_ip = ntohl(b->yiaddr);
        dhcp_server = server;
        dhcp_state = DHCP_REQUESTING;
        dhcp_tries = 0;
        dhcp_send(3);
        dhcp_next_send = ticks + TIMER_HZ;
    } else if (type == 5 && dhcp_state == DHCP_REQUESTING) {  /* ACK */
        netif.ip = ntohl(b->yiaddr);
        netif.netmask = mask ? mask : 0xFFFFFF00;
        netif.gateway = router;
        netif.dns = dns ? dns : router;
        netif.dhcp = true;
        netif.up = true;
        dhcp_state = DHCP_BOUND;
    } else if (type == 6) {                                   /* NAK */
        dhcp_state = DHCP_SELECTING;
    }
}

static void dhcp_tick(void)
{
    if (dhcp_state != DHCP_SELECTING && dhcp_state != DHCP_REQUESTING)
        return;
    if (ticks < dhcp_next_send)
        return;
    if (++dhcp_tries > 6 && dhcp_state == DHCP_REQUESTING)
        dhcp_state = DHCP_SELECTING;
    dhcp_send(dhcp_state == DHCP_SELECTING ? 1 : 3);
    dhcp_next_send = ticks + TIMER_HZ * (dhcp_tries < 4 ? 1 : 4);
}

/* ------------------------------------------------------------------ */
/* Setup and polling                                                   */
/* ------------------------------------------------------------------ */

void net_init(void)
{
    netif.nic = e1000_probe();
    if (!netif.nic)
        return;
    netif.present = true;
    dhcp_xid = (uint32_t)(ticks * 2654435761U) ^ (netif.mac[5] << 8) ^ 0xA1E05;
    dhcp_state = DHCP_SELECTING;
    dhcp_next_send = 0;
}

/* Wait (at boot) for DHCP; fall back to the QEMU user-network defaults. */
bool net_wait_config(int max_ticks)
{
    if (!netif.present)
        return false;
    uint64_t end = ticks + max_ticks;
    while (!netif.up && ticks < end) {
        sti();
        hlt();
        cli();
    }
    if (!netif.up) {
        dhcp_state = DHCP_OFF;
        netif.ip = 0x0A00020F;          /* 10.0.2.15 */
        netif.netmask = 0xFFFFFF00;
        netif.gateway = 0x0A000202;
        netif.dns = 0x0A000203;
        netif.up = true;
    }
    return netif.dhcp;
}

void net_poll(void)
{
    if (netif.nic)
        netif.nic->poll();
    for (int budget = 0; loop_head != loop_tail && budget < NLOOP; budget++) {
        uint32_t i = loop_head++ % NLOOP;
        ip_input(loopq[i].pkt, loopq[i].len);
    }
    /* expire ARP-pending packets after 3 s (and re-ask once a second) */
    for (int i = 0; i < NPENDING; i++) {
        if (!pending[i].used)
            continue;
        uint64_t age = ticks - pending[i].since;
        if (age > 3 * TIMER_HZ)
            pending[i].used = false;
        else if (age && age % TIMER_HZ == 0)
            arp_send(1, NULL, pending[i].nexthop);
    }
    dhcp_tick();
    tcp_tick();
}
