/*
 * __sieos_dgram.c - SIEOS: sendmsg and recvmsg with several buffers on a
 * datagram socket (src/network/sendmsg.c, recvmsg.c, sieos-port.py).
 *
 * SIEOS's kernel takes a datagram in one buffer (it stays whole); with
 * several, the C library gathers them into one before sending, and receives
 * into one that it scatters back.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "syscall.h"

#define DGRAM_MAX 65536                          /* (UDP's largest datagram) */

/* Is fd a socket that is not a stream (one whose messages are datagrams)? */
hidden int __sieos_is_dgram(int fd)
{
	int type;
	socklen_t l = sizeof type;
	return !getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &l) && type != SOCK_STREAM;
}

/* The buffers' total length (at most max), and a buffer that long; 0 if out of memory. */
hidden char *__sieos_dgram_buf(const struct msghdr *msg, size_t max, size_t *len)
{
	size_t n = 0;
	for (int i = 0; i < (int)msg->msg_iovlen; i++) {
		n += msg->msg_iov[i].iov_len;
		if (n > max) {
			n = max;
			break;
		}
	}
	*len = n;
	char *b = malloc(n ? n : 1);
	if (!b) errno = ENOBUFS;
	return b;
}

hidden void __sieos_dgram_copy(const struct msghdr *msg, char *buf, size_t n, int gather)
{
	for (int i = 0; i < (int)msg->msg_iovlen && n; i++) {
		size_t c = msg->msg_iov[i].iov_len < n ? msg->msg_iov[i].iov_len : n;
		if (gather) memcpy(buf, msg->msg_iov[i].iov_base, c);
		else memcpy(msg->msg_iov[i].iov_base, buf, c);
		buf += c;
		n -= c;
	}
}
