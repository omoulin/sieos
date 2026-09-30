/*
 * net6.c - IPv6 (RFC 8200), neighbor discovery (RFC 4861), stateless
 * address autoconfiguration (RFC 4862), ICMPv6 (RFC 4443) and loopback.
 *
 * Every interface gets a link-local address from its MAC (modified EUI-64)
 * and solicits a router; a router advertisement with an autonomous /64
 * prefix gives it a global address the same way, a default router, the
 * link MTU and (RFC 8106) a DNS server.  Each address is tentative until
 * duplicate address detection (RFC 4862: one probe, one second) finds no
 * other node using it.  MLDv2 reports (RFC 3810) announce our solicited-
 * node groups when they are joined and in answer to queries.  Fragmented
 * datagrams are reassembled (ipfrag.c) and large ones sent in fragments;
 * hop-by-hop, routing and destination-option headers are skipped.
 *
 * Routes: a destination on an interface's prefix leaves by it; a link-local
 * or multicast one by the interface of the chosen source address, else the
 * first interface; anything else by the first interface with a router.
 * Not implemented: path MTU discovery, privacy addresses, scope ids.
 * Everything runs under the big kernel lock, driven by net_poll() like IPv4.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "net.h"
#include "proc.h"
#include "mm.h"

struct ip6_hdr {
    uint32_t vtcfl;                  /* version 6, traffic class, flow label */
    uint16_t plen;
    uint8_t nxt, hlim;
    naddr_t src, dst;
} __attribute__((packed));

#define ICMP6_ECHO_REQUEST 128
#define ICMP6_ECHO_REPLY   129
#define ND_ROUTER_SOLICIT  133
#define ND_ROUTER_ADVERT   134
#define ND_NEIGHBOR_SOLICIT 135
#define ND_NEIGHBOR_ADVERT 136
#define MLD_QUERY          130
#define MLD2_REPORT        143

static const naddr_t all_nodes = { { 0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } };
static const naddr_t all_routers = { { 0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 } };
static const naddr_t mld_routers = { { 0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x16 } };

/* ---------------- addresses ---------------- */

const char *ip6_str(const naddr_t *a, char *buf)
{
    if (na_is_v4(a)) {
        char v4[16];
        snprintf(buf, 40, "::ffff:%s", ip_str(na_to_v4(a), v4));
        return buf;
    }
    uint16_t g[8];
    for (int i = 0; i < 8; i++)
        g[i] = a->b[2 * i] << 8 | a->b[2 * i + 1];
    int best = -1, bestlen = 1;                  /* the longest run of zero groups, if > 1 (RFC 5952) */
    for (int i = 0; i < 8;) {
        int j = i;
        while (j < 8 && !g[j])
            j++;
        if (j - i > bestlen) {
            best = i;
            bestlen = j - i;
        }
        i = j > i ? j : i + 1;
    }
    char *p = buf;
    for (int i = 0; i < 8; i++) {
        if (i == best) {
            *p++ = ':';
            if (i == 0)
                *p++ = ':';
            i += bestlen - 1;
            continue;
        }
        p += snprintf(p, 6, "%x", g[i]);
        if (i < 7)
            *p++ = ':';
    }
    *p = 0;
    return buf;
}

/* The interface identifier from the MAC: modified EUI-64. */
static void set_iid(const struct netif *ifp, naddr_t *a)
{
    const uint8_t *m = ifp->mac;
    a->b[8] = m[0] ^ 2;
    a->b[9] = m[1];
    a->b[10] = m[2];
    a->b[11] = 0xFF;
    a->b[12] = 0xFE;
    a->b[13] = m[3];
    a->b[14] = m[4];
    a->b[15] = m[5];
}

static naddr_t solicited_node(const naddr_t *a)
{
    naddr_t s = { { 0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xFF } };
    s.b[13] = a->b[13];
    s.b[14] = a->b[14];
    s.b[15] = a->b[15];
    return s;
}

/* An address is configured (known), and usable once its DAD is over (have). */
static bool known(const struct netif *ifp, int i) { return ifp->v6.up && !na_zero(&ifp->v6.addr[i]); }
static bool have(const struct netif *ifp, int i) { return known(ifp, i) && !ifp->v6.tentative[i]; }

/* The interface that has address a (usable), or NULL. */
static struct netif *ifp_of6(const naddr_t *a)
{
    for (int k = 0; k < nnetif; k++)
        for (int i = 0; i < NET6_ADDRS; i++)
            if (have(&netifs[k], i) && na_eq(a, &netifs[k].v6.addr[i]))
                return &netifs[k];
    return NULL;
}

bool ip6_is_local(const naddr_t *a)
{
    return na_loop6(a) || ifp_of6(a);
}

/* Multicast groups ifp listens to: all-nodes and its solicited-node groups. */
static bool our_group(const struct netif *ifp, const naddr_t *a)
{
    if (na_eq(a, &all_nodes))
        return true;
    for (int i = 0; i < NET6_ADDRS; i++)
        if (known(ifp, i)) {                     /* (during DAD too: other nodes' probes come here) */
            naddr_t s = solicited_node(&ifp->v6.addr[i]);
            if (na_eq(a, &s))
                return true;
        }
    return false;
}

static bool on_link(const struct netif *ifp, const naddr_t *dst)
{
    if (na_linklocal(dst) || na_multicast6(dst))
        return true;
    if (!have(ifp, 1))
        return false;
    int plen = ifp->v6.plen[1];
    for (int i = 0; i < plen / 8; i++)
        if (dst->b[i] != ifp->v6.addr[1].b[i])
            return false;
    int r = plen % 8;
    return !r || ((dst->b[plen / 8] ^ ifp->v6.addr[1].b[plen / 8]) & (0xFF << (8 - r))) == 0;
}

/* The interface to reach dst by; src is the source the sender chose (NULL or :: for any). */
static struct netif *route6(const naddr_t *dst, const naddr_t *src)
{
    struct netif *pref = src && !na_zero(src) ? ifp_of6(src) : NULL;
    if (na_linklocal(dst) || na_multicast6(dst)) {
        if (pref)
            return pref;
        for (int k = 0; k < nnetif; k++)
            if (have(&netifs[k], 0))
                return &netifs[k];
        return NULL;
    }
    for (int k = 0; k < nnetif; k++)
        if ((!pref || pref == &netifs[k]) && on_link(&netifs[k], dst))
            return &netifs[k];
    if (pref && !na_zero(&pref->v6.router))
        return pref;
    for (int k = 0; k < nnetif; k++)
        if (netifs[k].v6.up && !na_zero(&netifs[k].v6.router))
            return &netifs[k];
    return NULL;
}

/* The source address on ifp for dst. */
static naddr_t source_on(const struct netif *ifp, const naddr_t *dst)
{
    naddr_t none = { { 0 } };
    if (!ifp || !ifp->v6.up)
        return none;
    bool link_scope = na_linklocal(dst) || (na_multicast6(dst) && (dst->b[1] & 15) <= 2);
    if (!link_scope && have(ifp, 1))
        return ifp->v6.addr[1];
    return have(ifp, 0) ? ifp->v6.addr[0] : none;
}

naddr_t ip6_source(const naddr_t *dst)
{
    if (ip6_is_local(dst))
        return *dst;
    return source_on(route6(dst, NULL), dst);
}

/* ---------------- neighbors ---------------- */

static void nd_update(struct netif *ifp, const naddr_t *ip, const uint8_t *mac)
{
    struct net6_state *v = &ifp->v6;
    int slot = -1, oldest = 0;
    for (int i = 0; i < NND; i++) {
        if (v->nd[i].valid && na_eq(&v->nd[i].ip, ip)) {
            slot = i;
            break;
        }
        if (!v->nd[i].valid && slot < 0)
            slot = i;
        if (v->nd[i].stamp < v->nd[oldest].stamp)
            oldest = i;
    }
    if (slot < 0)
        slot = oldest;
    v->nd[slot].ip = *ip;
    memcpy(v->nd[slot].mac, mac, 6);
    v->nd[slot].valid = true;
    v->nd[slot].stamp = ticks;
    for (int i = 0; i < NPEND6; i++)
        if (v->pend[i].used && na_eq(&v->pend[i].nexthop, ip)) {
            eth_send(ifp, mac, ETH_P_IPV6, v->pend[i].pkt, v->pend[i].len);
            v->pend[i].used = false;
        }
}

static bool nd_lookup(struct netif *ifp, const naddr_t *ip, uint8_t *mac)
{
    for (int i = 0; i < NND; i++)
        if (ifp->v6.nd[i].valid && na_eq(&ifp->v6.nd[i].ip, ip)) {
            memcpy(mac, ifp->v6.nd[i].mac, 6);
            return true;
        }
    return false;
}

static uint64_t now_ticks(void);

/* ---------------- output ---------------- */

static int output(struct netif *ifp, const naddr_t *src, const naddr_t *dst, uint8_t proto, const void *payload,
                  size_t len, int hlim);

/* ICMPv6 out of ifp; src: NULL to choose one, else exactly that (:: for DAD probes) */
static void icmp6_out(struct netif *ifp, const naddr_t *src, const naddr_t *dst, uint8_t *msg, size_t len, int hlim)
{
    naddr_t s = src ? *src : source_on(ifp, dst);
    msg[2] = msg[3] = 0;
    uint16_t c = csum_fold(csum_add(net_pseudo_sum(&s, dst, IPPROTO_ICMPV6_K, len), msg, len));
    memcpy(msg + 2, &c, 2);
    output(ifp, &s, dst, IPPROTO_ICMPV6_K, msg, len, hlim);
}

/* ICMPv6 from a raw socket: the kernel computes the checksum (RFC 3542). */
int icmp6_send(const naddr_t *dst, const void *msg, size_t len)
{
    if (len < 4 || len > 65535)
        return -EMSGSIZE;
    naddr_t s = ip6_source(dst);
    if (na_zero(&s))
        return -ENETUNREACH;
    uint8_t *m = kmalloc(len);
    if (!m)
        return -ENOBUFS;
    memcpy(m, msg, len);
    m[2] = m[3] = 0;
    uint16_t c = csum_fold(csum_add(net_pseudo_sum(&s, dst, IPPROTO_ICMPV6_K, len), m, len));
    memcpy(m + 2, &c, 2);
    int r = output(ip6_is_local(dst) ? NULL : route6(dst, &s), &s, dst, IPPROTO_ICMPV6_K, m, len, 0);
    kfree(m);
    return r;
}

static void send_ns(struct netif *ifp, const naddr_t *target, const naddr_t *dst)
{
    uint8_t m[32];
    memset(m, 0, sizeof(m));
    m[0] = ND_NEIGHBOR_SOLICIT;
    memcpy(m + 8, target, 16);
    m[24] = 1;                               /* source link-layer address */
    m[25] = 1;
    memcpy(m + 26, ifp->mac, 6);
    icmp6_out(ifp, NULL, dst, m, sizeof(m), 255);
}

static void send_rs(struct netif *ifp)
{
    uint8_t m[16];
    memset(m, 0, sizeof(m));
    m[0] = ND_ROUTER_SOLICIT;
    m[8] = 1;
    m[9] = 1;
    memcpy(m + 10, ifp->mac, 6);
    icmp6_out(ifp, &ifp->v6.addr[0], &all_routers, m, sizeof(m), 255);
}

/* A finished packet: loopback (ifp NULL or dst ours), multicast, or the neighbor (queued for resolution). */
static int emit(struct netif *ifp, const naddr_t *dst, const uint8_t *pkt, size_t total)
{
    if (ip6_is_local(dst)) {
        net_loop(pkt, total);
        return 0;
    }
    if (!ifp)
        return nnetif ? -ENETUNREACH : -ENETDOWN;
    if (!ifp->nic)
        return -ENETDOWN;
    if (!ifp->v6.up)
        return -ENETUNREACH;
    uint8_t mac[6];
    if (na_multicast6(dst)) {
        mac[0] = mac[1] = 0x33;
        memcpy(mac + 2, dst->b + 12, 4);
        eth_send(ifp, mac, ETH_P_IPV6, pkt, total);
        return 0;
    }
    naddr_t nexthop = on_link(ifp, dst) ? *dst : ifp->v6.router;
    if (na_zero(&nexthop))
        return -ENETUNREACH;
    if (nd_lookup(ifp, &nexthop, mac)) {
        eth_send(ifp, mac, ETH_P_IPV6, pkt, total);
        return 0;
    }
    for (int i = 0; i < NPEND6; i++) {           /* until the neighbor answers */
        struct l2_pending *p = &ifp->v6.pend[i];
        if (!p->used) {
            p->used = true;
            p->nexthop = nexthop;
            p->since = ticks;
            p->len = total;
            memcpy(p->pkt, pkt, total);
            break;
        }
    }
    naddr_t sn = solicited_node(&nexthop);
    send_ns(ifp, &nexthop, &sn);
    return 0;
}

static void header6(struct netif *ifp, uint8_t *pkt, const naddr_t *src, const naddr_t *dst, uint8_t nxt,
                    size_t plen, int hlim)
{
    struct ip6_hdr *h = (struct ip6_hdr *)pkt;
    h->vtcfl = htonl(6U << 28);
    h->plen = htons(plen);
    h->nxt = nxt;
    h->hlim = hlim ? hlim : na_multicast6(dst) ? 1 : ifp && ifp->v6.hoplimit ? ifp->v6.hoplimit : 64;
    h->src = src ? *src : ip6_source(dst);
    h->dst = *dst;
}

static uint32_t frag_id = 0x5E105000;

/*
 * An IPv6 packet out of ifp; one larger than the MTU (UDP, ICMPv6, up to
 * 65,535 bytes) goes in fragments (a Fragment header, 8-byte multiples).
 */
static int output(struct netif *ifp, const naddr_t *src, const naddr_t *dst, uint8_t proto, const void *payload,
                  size_t len, int hlim)
{
    size_t mtu = ifp && ifp->v6.mtu ? ifp->v6.mtu : ETH_MTU;
    if (len > 65535)
        return -EMSGSIZE;
    uint8_t pkt[ETH_MTU];
    if (len + IP6_HLEN <= mtu) {
        header6(ifp, pkt, src, dst, proto, len, hlim);
        memcpy(pkt + IP6_HLEN, payload, len);
        return emit(ifp, dst, pkt, IP6_HLEN + len);
    }
    if (proto == IPPROTO_TCP)
        return -EMSGSIZE;                        /* (TCP segments fit: the MSS) */
    uint32_t per = (mtu - IP6_HLEN - 8) & ~7U, id = frag_id++;
    for (uint32_t off = 0; off < len; off += per) {
        uint32_t n = MIN(per, (uint32_t)(len - off));
        bool more = off + n < len;
        header6(ifp, pkt, src, dst, 44, 8 + n, hlim);
        uint8_t *f = pkt + IP6_HLEN;
        f[0] = proto;
        f[1] = 0;
        uint16_t offlg = htons((uint16_t)(off | (more ? 1 : 0)));    /* offset in units of 8, M */
        memcpy(f + 2, &offlg, 2);
        uint32_t nid = htonl(id);
        memcpy(f + 4, &nid, 4);
        memcpy(f + 8, (const uint8_t *)payload + off, n);
        int r = emit(ifp, dst, pkt, IP6_HLEN + 8 + n);
        if (r < 0)
            return r;
    }
    return 0;
}

/* ICMPv6 msg with a hop-by-hop Router Alert option (MLD), hop limit 1. */
static void output_router_alert(struct netif *ifp, const naddr_t *src, const naddr_t *dst, uint8_t *msg, size_t len)
{
    uint8_t pkt[ETH_MTU];
    if (IP6_HLEN + 8 + len > sizeof(pkt))
        return;
    naddr_t s = src ? *src : (naddr_t){ { 0 } };
    header6(ifp, pkt, &s, dst, 0, 8 + len, 1);
    uint8_t *hbh = pkt + IP6_HLEN;
    hbh[0] = IPPROTO_ICMPV6_K;
    hbh[1] = 0;                                  /* 8 bytes */
    hbh[2] = 5;                                  /* Router Alert: MLD */
    hbh[3] = 2;
    hbh[4] = 0;
    hbh[5] = 0;
    hbh[6] = 1;                                  /* PadN */
    hbh[7] = 0;
    msg[2] = msg[3] = 0;
    uint16_t c = csum_fold(csum_add(net_pseudo_sum(&s, dst, IPPROTO_ICMPV6_K, len), msg, len));
    memcpy(msg + 2, &c, 2);
    memcpy(pkt + IP6_HLEN + 8, msg, len);
    emit(ifp, dst, pkt, IP6_HLEN + 8 + len);
}

int ip6_send(const naddr_t *src, const naddr_t *dst, uint8_t proto, const void *payload, size_t len)
{
    const naddr_t *s = src && !na_zero(src) ? src : NULL;
    struct netif *ifp = ip6_is_local(dst) ? NULL : route6(dst, s);
    if (!ifp && !ip6_is_local(dst))
        return nnetif ? -ENETUNREACH : -ENETDOWN;
    naddr_t chosen;
    if (!s) {
        chosen = ip6_is_local(dst) ? *dst : source_on(ifp, dst);
        s = &chosen;
    }
    return output(ifp, s, dst, proto, payload, len, 0);
}

/* ---------------- duplicate address detection and MLD ---------------- */

static uint32_t rand6(void)
{
    static uint32_t x = 0x9E3779B9;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x ^ (uint32_t)ticks;
}

/* An MLDv2 report of ifp's solicited-node groups (record type: 4 joining, 2 answering a query). */
static void mld_report(struct netif *ifp, int rtype)
{
    uint8_t m[8 + 20 * NET6_ADDRS];
    memset(m, 0, sizeof(m));
    m[0] = MLD2_REPORT;
    int n = 0;
    naddr_t groups[NET6_ADDRS];
    for (int i = 0; i < NET6_ADDRS; i++) {
        if (!known(ifp, i))
            continue;
        naddr_t g = solicited_node(&ifp->v6.addr[i]);
        bool dup = false;
        for (int k = 0; k < n; k++)
            dup |= na_eq(&groups[k], &g);
        if (dup)
            continue;
        groups[n] = g;
        uint8_t *r = m + 8 + 20 * n++;
        r[0] = rtype;                            /* aux length 0, no sources */
        memcpy(r + 4, &g, 16);
    }
    if (!n)
        return;
    m[6] = n >> 8;
    m[7] = n;
    naddr_t zero = { { 0 } };
    output_router_alert(ifp, have(ifp, 0) ? &ifp->v6.addr[0] : &zero, &mld_routers, m, 8 + 20 * n);
    ifp->v6.mld_reports++;
}

/* The time in ticks now (ticks itself lags at boot, until the timer interrupt catches up). */
static uint64_t now_ticks(void)
{
    uint64_t t = hrtime() / (1000000000UL / TIMER_HZ);
    return t > ticks ? t : ticks;
}

/* Probe for addr[i] (from ::, to its solicited-node group) and join the group. */
static void start_dad(struct netif *ifp, int i)
{
    struct net6_state *v = &ifp->v6;
    v->tentative[i] = true;
    v->dad_until[i] = now_ticks() + TIMER_HZ;    /* RetransTimer, one transmission */
    v->mld_left = 2;                             /* robustness: report twice */
    mld_report(ifp, 4);
    v->mld_next = now_ticks() + TIMER_HZ / 2;
    uint8_t m[24];
    memset(m, 0, sizeof(m));
    m[0] = ND_NEIGHBOR_SOLICIT;
    memcpy(m + 8, &v->addr[i], 16);
    naddr_t zero = { { 0 } }, sn = solicited_node(&v->addr[i]);
    icmp6_out(ifp, &zero, &sn, m, sizeof(m), 255);
}

static void duplicate(struct netif *ifp, int i)
{
    char b[40];
    kprintf("ipv6: %s is in use by another node (%s): not configured\n", ip6_str(&ifp->v6.addr[i], b), ifp->name);
    memset(&ifp->v6.addr[i], 0, sizeof(ifp->v6.addr[i]));
    ifp->v6.tentative[i] = false;
    ifp->v6.duplicates++;
}

/* The tentative address addr[i] of ifp is target, or -1. */
static int tentative_for(const struct netif *ifp, const naddr_t *target)
{
    for (int i = 0; i < NET6_ADDRS; i++)
        if (known(ifp, i) && ifp->v6.tentative[i] && na_eq(target, &ifp->v6.addr[i]))
            return i;
    return -1;
}

/* ---------------- input ---------------- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void router_advert(struct netif *ifp, const naddr_t *src, const uint8_t *m, size_t len)
{
    struct net6_state *v = &ifp->v6;
    if (len < 16 || !na_linklocal(src))
        return;
    if (m[4])
        v->hoplimit = m[4];
    uint16_t lifetime = m[6] << 8 | m[7];
    for (size_t o = 16; o + 8 <= len;) {
        size_t olen = m[o + 1] * 8;
        if (!olen || o + olen > len)
            break;
        const uint8_t *op = m + o;
        switch (op[0]) {
        case 1:                                  /* source link-layer address */
            nd_update(ifp, src, op + 2);
            break;
        case 3:                                  /* prefix information */
            if (olen == 32 && (op[3] & 0x40) && op[2] == 64 && be32(op + 4) &&
                !(op[16] == 0xFE && (op[17] & 0xC0) == 0x80)) {
                naddr_t a = { { 0 } };
                memcpy(a.b, op + 16, 8);
                set_iid(ifp, &a);
                if (!na_eq(&a, &v->addr[1])) {
                    v->addr[1] = a;
                    v->plen[1] = 64;
                    start_dad(ifp, 1);           /* usable once no one else answers */
                }
            }
            break;
        case 5:                                  /* MTU */
            if (be32(op + 4) >= 1280 && be32(op + 4) <= ETH_MTU)
                v->mtu = be32(op + 4);
            break;
        case 25:                                 /* RDNSS */
            if (olen >= 24 && be32(op + 4))
                memcpy(&v->dns, op + 8, 16);
            break;
        }
        o += olen;
    }
    if (lifetime)
        v->router = *src;
    else if (na_eq(&v->router, src))
        memset(&v->router, 0, sizeof(v->router));
}

/* in: the receiving interface (NULL: loopback, where only echo is answered). */
static void icmp6_input(struct netif *in, const naddr_t *src, const naddr_t *dst, const uint8_t *m, size_t len,
                        int hlim)
{
    if (len < 4 || csum_fold(csum_add(net_pseudo_sum(src, dst, IPPROTO_ICMPV6_K, len), m, len)) != 0)
        return;
    switch (m[0]) {
    case ICMP6_ECHO_REQUEST:
        if (len >= 8 && m[1] == 0) {
            uint8_t *r = kmalloc(len);
            if (r) {
                memcpy(r, m, len);
                r[0] = ICMP6_ECHO_REPLY;
                const naddr_t *from = na_multicast6(dst) ? NULL : dst;
                struct netif *out;                  /* back the way it came, for link-local senders */
                if (ip6_is_local(src))
                    out = NULL;
                else if (in && na_linklocal(src))
                    out = in;
                else if (!(out = route6(src, from)))
                    out = in;
                icmp6_out(out, from, src, r, len, 0);
                kfree(r);
            }
        }
        break;
    case ND_ROUTER_ADVERT:
        if (in && hlim == 255)
            router_advert(in, src, m, len);
        return;
    case ND_NEIGHBOR_SOLICIT: {
        if (!in || hlim != 255 || len < 24)
            return;
        naddr_t target;
        memcpy(&target, m + 8, 16);
        bool from_any = na_zero(src);
        for (size_t o = 24; o + 8 <= len && !from_any; o += m[o + 1] * 8) {
            if (!m[o + 1])
                break;
            if (m[o] == 1)
                nd_update(in, src, m + o + 2);
        }
        int t = tentative_for(in, &target);
        if (t >= 0) {
            if (from_any)
                duplicate(in, t);                /* another node probes for it too */
            return;                              /* (a tentative address is not answered for) */
        }
        if (ifp_of6(&target) != in)
            return;                              /* (only for this interface's addresses) */
        uint8_t a[32];
        memset(a, 0, sizeof(a));
        a[0] = ND_NEIGHBOR_ADVERT;
        a[4] = from_any ? 0x20 : 0x60;           /* (solicited) override */
        memcpy(a + 8, &target, 16);
        a[24] = 2;                               /* target link-layer address */
        a[25] = 1;
        memcpy(a + 26, in->mac, 6);
        icmp6_out(in, &target, from_any ? &all_nodes : src, a, sizeof(a), 255);
        return;
    }
    case ND_NEIGHBOR_ADVERT: {
        if (!in || hlim != 255 || len < 24)
            return;
        naddr_t target;
        memcpy(&target, m + 8, 16);
        int t = tentative_for(in, &target);
        if (t >= 0) {
            duplicate(in, t);                    /* someone has it already */
            return;
        }
        for (size_t o = 24; o + 8 <= len; o += m[o + 1] * 8) {
            if (!m[o + 1])
                break;
            if (m[o] == 2)
                nd_update(in, &target, m + o + 2);
        }
        return;
    }
    case ND_ROUTER_SOLICIT:
        return;
    case MLD_QUERY:                              /* report our groups within a second */
        if (in && hlim == 1 && (!in->v6.mld_next || in->v6.mld_left == 0))
            in->v6.mld_next = ticks + 1 + rand6() % TIMER_HZ;
        return;
    case MLD2_REPORT:
        return;
    }
    icmp_deliver_raw(src, IPPROTO_ICMPV6_K, m, len);
}

/* The payload after the fixed header: extension headers, fragments, then the protocol. */
static void deliver6(struct netif *in, const naddr_t *src, const naddr_t *dst, uint8_t nxt, const uint8_t *p,
                     size_t plen, int hlim, int depth)
{
    while (nxt == 0 || nxt == 43 || nxt == 60) {  /* hop-by-hop, routing, destination options */
        if (plen < 8 || plen < (size_t)(p[1] + 1) * 8)
            return;
        if (nxt == 43 && p[3])
            return;                              /* segments left: we are no router */
        size_t hl = (p[1] + 1) * 8;
        nxt = p[0];
        p += hl;
        plen -= hl;
    }
    if (nxt == 44) {                             /* a fragment: reassemble, then deliver the whole */
        if (plen < 8 || depth)
            return;
        uint16_t offlg = (uint16_t)(p[2] << 8 | p[3]);
        struct frag_key k = { *src, *dst, be32(p + 4), 0, true };
        uint8_t *whole;
        size_t n;
        if (frag_add(&k, offlg & ~7U, p + 8, plen - 8, offlg & 1, &whole, &n) == 1) {
            deliver6(in, src, dst, p[0], whole, n, hlim, depth + 1);
            kfree(whole);
        }
        return;
    }
    switch (nxt) {
    case IPPROTO_ICMPV6_K: icmp6_input(in, src, dst, p, plen, hlim); break;
    case IPPROTO_UDP:      udp_input(src, dst, p, plen); break;
    case IPPROTO_TCP:      tcp_input(src, dst, p, plen); break;
    }
}

void ip6_input(struct netif *in, const uint8_t *pkt, size_t len)
{
    if (len < IP6_HLEN)
        return;
    const struct ip6_hdr *h = (const struct ip6_hdr *)pkt;
    size_t plen = ntohs(h->plen);
    if ((pkt[0] >> 4) != 6 || plen > len - IP6_HLEN)
        return;
    if (in)
        in->v6.rx_packets++;
    naddr_t src = h->src, dst = h->dst;
    if (na_multicast6(&src))
        return;
    if (!ip6_is_local(&dst) && !(in && our_group(in, &dst)))
        return;
    deliver6(in, &src, &dst, h->nxt, pkt + IP6_HLEN, plen, h->hlim, 0);
}

/* ---------------- start-up and timers ---------------- */

void net6_attach(struct netif *ifp)
{
    struct net6_state *v = &ifp->v6;
    naddr_t ll = { { 0xFE, 0x80 } };
    set_iid(ifp, &ll);
    v->addr[0] = ll;
    v->plen[0] = 64;
    v->mtu = ETH_MTU;
    v->hoplimit = 64;
    v->up = true;
    v->tentative[0] = true;                      /* DAD starts once the link is up (net6_tick) */
    v->dad_pending = true;
}

static void tick_if(struct netif *ifp)
{
    struct net6_state *v = &ifp->v6;
    if (!v->up)
        return;
    if (v->dad_pending && (!ifp->nic || !ifp->nic->link || ifp->nic->link(ifp))) {
        v->dad_pending = false;                  /* (frames before link-up would go unseen) */
        start_dad(ifp, 0);
        v->rs_next = v->dad_until[0] + 1;        /* a router is asked once the address is ours */
    }
    for (int i = 0; i < NET6_ADDRS; i++)
        if (known(ifp, i) && v->tentative[i] && v->dad_until[i] && ticks >= v->dad_until[i])
            v->tentative[i] = false;             /* nobody answered: the address is ours */
    if (v->mld_next && ticks >= v->mld_next) {
        bool unsolicited = v->mld_left > 0;
        mld_report(ifp, unsolicited ? 4 : 2);
        v->mld_next = unsolicited && --v->mld_left > 0 ? ticks + TIMER_HZ : 0;
    }
    /* solicit a router three times, 4 s apart (RFC 4861 6.3.7), until one answers */
    if (na_zero(&v->router) && v->rs_sent < 3 && ticks >= v->rs_next && have(ifp, 0)) {
        send_rs(ifp);
        v->rs_sent++;
        v->rs_next = ticks + 4 * TIMER_HZ;
    }
    for (int i = 0; i < NPEND6; i++) {
        struct l2_pending *p = &v->pend[i];
        if (!p->used)
            continue;
        uint64_t age = ticks - p->since;
        if (age > 3 * TIMER_HZ) {
            p->used = false;
        } else if (age && age % TIMER_HZ == 0) {
            naddr_t sn = solicited_node(&p->nexthop);
            send_ns(ifp, &p->nexthop, &sn);
        }
    }
}

void net6_tick(void)
{
    for (int i = 0; i < nnetif; i++)
        tick_if(&netifs[i]);
}
