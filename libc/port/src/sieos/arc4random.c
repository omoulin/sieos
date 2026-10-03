/*
 * arc4random.c - arc4random(3C), arc4random_buf, arc4random_uniform: the
 * kernel's random numbers (getrandom), taken a buffer at a time; the
 * buffer is used once and cleared, and a child of fork starts afresh.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/random.h>
#include "lock.h"

static volatile int lock[1];
static unsigned char pool[512];
static size_t avail;
static pid_t owner;

static void fill(void *out, size_t n)
{
	unsigned char *p = out;
	while (n) {
		ssize_t r = getrandom(p, n, 0);
		if (r > 0) {
			p += r;
			n -= r;
		}
	}
}

void arc4random_buf(void *buf, size_t n)
{
	unsigned char *p = buf;
	if (n >= sizeof pool) {
		fill(p, n);
		return;
	}
	LOCK(lock);
	pid_t me = getpid();
	if (owner != me) {
		owner = me;
		avail = 0;
	}
	while (n) {
		if (!avail) {
			fill(pool, sizeof pool);
			avail = sizeof pool;
		}
		size_t k = n < avail ? n : avail;
		unsigned char *src = pool + sizeof pool - avail;
		memcpy(p, src, k);
		memset(src, 0, k);
		avail -= k;
		p += k;
		n -= k;
	}
	UNLOCK(lock);
}

uint32_t arc4random(void)
{
	uint32_t v;
	arc4random_buf(&v, sizeof v);
	return v;
}

uint32_t arc4random_uniform(uint32_t bound)
{
	if (bound < 2)
		return 0;
	uint32_t min = -bound % bound;          /* 2^32 mod bound: the values that would bias */
	for (;;) {
		uint32_t r = arc4random();
		if (r >= min)
			return r % bound;
	}
}
