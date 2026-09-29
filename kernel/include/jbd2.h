#ifndef SIEOS_JBD2_H
#define SIEOS_JBD2_H

#include "kernel.h"
#include "bcache.h"

/* A running transaction commits once it is this old (at the end of an operation, or a system call). */
#define JBD_COMMIT_TICKS (5 * TIMER_HZ)

struct jbd {
    int dev;
    uint32_t bs;
    uint64_t *map;                  /* journal block -> file system block (kmalloc'd, owned) */
    uint32_t maxlen, first;
    uint32_t seq;                   /* the next transaction */
    bool b64, csum, csum3;
    uint32_t tag_bytes;
    uint8_t uuid[16];
    uint32_t cseed;
    uint32_t (*crc)(uint32_t seed, const void *p, size_t n);   /* ext4's CRC32C */
    uint8_t *sbuf, *io;             /* the journal superblock, a scratch block */
    /* the running transaction */
    struct buf **blocks;
    uint32_t n, cap, limit;
    uint64_t *freed;
    uint32_t nfreed, capfreed;
    uint64_t started;
    uint64_t commits;
};

int  jbd_load(struct jbd *j, int dev, uint32_t bs, uint64_t *map, uint32_t nmap,
              uint32_t (*crc)(uint32_t, const void *, size_t));
int  jbd_recover(struct jbd *j);            /* blocks replayed, or -errno */
int  jbd_add(struct jbd *j, struct buf *b); /* a metadata block changed */
bool jbd_has(struct jbd *j, struct buf *b);
void jbd_freed(struct jbd *j, uint64_t blk);
bool jbd_is_freed(struct jbd *j, uint64_t blk);
bool jbd_due(struct jbd *j);                /* large or old enough to commit */
int  jbd_commit(struct jbd *j);
void jbd_close(struct jbd *j);

/* ext4.c: commit every mounted volume's due (or, with force, any) transaction */
void ext4_journal_tick(bool force);
extern volatile uint64_t fs_commit_deadline;    /* ticks; 0 when nothing waits */
extern bool jbd_crash_test;                     /* uadmin A_JTEST */

#endif
