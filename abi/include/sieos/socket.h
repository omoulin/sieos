/*
 * sieos/socket.h - sockets (ABI v2).  Constants follow Solaris
 * (note SOCK_DGRAM == 1 and SOCK_STREAM == 2, SOL_SOCKET == 0xffff).
 */
#ifndef SIEOS_ABI_SOCKET_H
#define SIEOS_ABI_SOCKET_H

#include "types.h"

#define SIEOS_AF_UNSPEC 0
#define SIEOS_AF_UNIX   1
#define SIEOS_AF_INET   2
#define SIEOS_AF_INET6  26

#define SIEOS_SOCK_DGRAM     1
#define SIEOS_SOCK_STREAM    2
#define SIEOS_SOCK_RAW       4
#define SIEOS_SOCK_RDM       5
#define SIEOS_SOCK_SEQPACKET 6
#define SIEOS_SOCK_TYPE_MASK 0xffff
#define SIEOS_SOCK_CLOEXEC   0x080000
#define SIEOS_SOCK_NONBLOCK  0x100000
#define SIEOS_SOCK_NDELAY    0x200000

#define SIEOS_IPPROTO_IP   0
#define SIEOS_IPPROTO_ICMP 1
#define SIEOS_IPPROTO_TCP  6
#define SIEOS_IPPROTO_UDP  17
#define SIEOS_IPPROTO_IPV6   41
#define SIEOS_IPPROTO_ICMPV6 58

/* IPPROTO_IPV6 options (Solaris values) */
#define SIEOS_IPV6_UNICAST_HOPS   0x05   /* int: hop limit of unicast packets, -1 default */
#define SIEOS_IPV6_MULTICAST_IF   0x06
#define SIEOS_IPV6_MULTICAST_HOPS 0x07
#define SIEOS_IPV6_MULTICAST_LOOP 0x08
#define SIEOS_IPV6_JOIN_GROUP     0x09
#define SIEOS_IPV6_LEAVE_GROUP    0x0a
#define SIEOS_IPV6_V6ONLY         0x27   /* int: an AF_INET6 socket takes no IPv4 traffic */

#define SIEOS_SOL_SOCKET    0xffff
#define SIEOS_SO_DEBUG      0x0001
#define SIEOS_SO_ACCEPTCONN 0x0002
#define SIEOS_SO_REUSEADDR  0x0004
#define SIEOS_SO_KEEPALIVE  0x0008
#define SIEOS_SO_DONTROUTE  0x0010
#define SIEOS_SO_BROADCAST  0x0020
#define SIEOS_SO_USELOOPBACK 0x0040
#define SIEOS_SO_LINGER     0x0080
#define SIEOS_SO_OOBINLINE  0x0100
#define SIEOS_SO_SNDBUF     0x1001
#define SIEOS_SO_RCVBUF     0x1002
#define SIEOS_SO_SNDLOWAT   0x1003
#define SIEOS_SO_RCVLOWAT   0x1004
#define SIEOS_SO_SNDTIMEO   0x1005
#define SIEOS_SO_RCVTIMEO   0x1006
#define SIEOS_SO_ERROR      0x1007
#define SIEOS_SO_TYPE       0x1008

#define SIEOS_TCP_NODELAY   0x01

#define SIEOS_MSG_OOB       0x0001
#define SIEOS_MSG_PEEK      0x0002
#define SIEOS_MSG_DONTROUTE 0x0004
#define SIEOS_MSG_EOR       0x0008
#define SIEOS_MSG_CTRUNC    0x0010
#define SIEOS_MSG_TRUNC     0x0020
#define SIEOS_MSG_WAITALL   0x0040
#define SIEOS_MSG_DONTWAIT  0x0080
#define SIEOS_MSG_NOSIGNAL  0x0200

#define SIEOS_SHUT_RD   0
#define SIEOS_SHUT_WR   1
#define SIEOS_SHUT_RDWR 2

#define SIEOS_INADDR_ANY       0x00000000U
#define SIEOS_INADDR_LOOPBACK  0x7F000001U     /* host order */
#define SIEOS_INADDR_BROADCAST 0xFFFFFFFFU

struct sieos_sockaddr {
    sieos_sa_family_t sa_family;
    char sa_data[14];
};

struct sieos_in_addr {
    sieos_in_addr_t s_addr;            /* network byte order */
};

struct sieos_sockaddr_in {
    sieos_sa_family_t sin_family;
    sieos_in_port_t   sin_port;        /* network byte order */
    struct sieos_in_addr sin_addr;
    char sin_zero[8];
};

/* AF_INET6.  An AF_INET6 socket also talks IPv4, with the peer's address
 * mapped as ::ffff:a.b.c.d, unless IPV6_V6ONLY is set.  Addresses passed in
 * need 28 bytes (without __sin6_src_id). */
struct sieos_in6_addr {              /* as Solaris: s6_addr is _S6_un._S6_u8 */
    union {
        sieos_uint8_t _S6_u8[16];
        sieos_uint32_t _S6_u32[4];
    } _S6_un;
};

struct sieos_sockaddr_in6 {
    sieos_sa_family_t sin6_family;
    sieos_in_port_t   sin6_port;       /* network byte order */
    sieos_uint32_t sin6_flowinfo;
    struct sieos_in6_addr sin6_addr;
    sieos_uint32_t sin6_scope_id;            /* interface of a link-local address (eth0 is 2) */
    sieos_uint32_t __sin6_src_id;
};

struct sieos_linger {
    int l_onoff;
    int l_linger;
};

struct sieos_msghdr {
    void *msg_name;
    sieos_socklen_t msg_namelen;
    int __pad1;
    struct sieos_iovec *msg_iov;
    int msg_iovlen;
    int __pad2;
    void *msg_control;
    sieos_socklen_t msg_controllen;
    int msg_flags;
};

/* AF_UNIX: a path in the file system (no abstract names) */
#define SIEOS_UNIX_PATH_MAX 108
struct sieos_sockaddr_un {
    sieos_sa_family_t sun_family;
    char sun_path[SIEOS_UNIX_PATH_MAX];
};

/* Ancillary data: 8-byte aligned headers; SCM_RIGHTS passes descriptors
 * over AF_UNIX sockets. */
#define SIEOS_SCM_RIGHTS 0x1010
struct sieos_cmsghdr {
    sieos_socklen_t cmsg_len;          /* header and data */
    int __pad;
    int cmsg_level;                    /* SIEOS_SOL_SOCKET */
    int cmsg_type;
};

SIEOS_STATIC_ASSERT(sizeof(struct sieos_sockaddr_un) == 110, "sockaddr_un size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_cmsghdr) == 16, "cmsghdr size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_sockaddr_in) == 16, "sockaddr_in size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_sockaddr_in6) == 32, "sockaddr_in6 size");
SIEOS_STATIC_ASSERT(sizeof(struct sieos_msghdr) == 48, "msghdr size");

#endif
