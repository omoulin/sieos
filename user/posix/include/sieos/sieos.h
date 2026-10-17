/*
 * sieos/sieos.h - SIEOS's own services for ported programs (libposix): a
 * call to a named server (messages as include/mk/abi.h's msg_t), and
 * shared memory objects. For what has no Linux equivalent: the desktop's
 * windows (mk/atlas.h, as the web browser's platform uses them).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t w[4];                       /* w[0]: the operation, then the result */
    const void *sbuf; uint64_t slen;     /* sent */
    void *rbuf; uint64_t rlen;           /* the reply's data (rlen: its size then) */
    int32_t pid, uid, gid, _pad;
} sieos_msg_t;

long  sieos_call(const char *port, sieos_msg_t *m);   /* -> the reply's w[0], or -error */
long  sieos_shm_create(uint64_t id, size_t size);     /* id: a random 63-bit name -> 0 */
void *sieos_shm_map(uint64_t id);                     /* read and write, anywhere -> address, or 0 */
long  sieos_shm_close(uint64_t id);                   /* (the mapping stays until munmap) */
