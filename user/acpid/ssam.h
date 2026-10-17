/*
 * ssam.h - The Surface's embedded controller, the "Surface Aggregator
 * Module" (SAM), spoken to over a UART in the Surface Serial Hub protocol
 * (ssam.c; Linux: drivers/platform/surface/aggregator). On a Surface Pro 7
 * it owns the battery and the charger: acpid asks it for them (the tables
 * have no battery device).
 *
 * A message: SYN (AA 55), a frame (type, payload length, sequence number)
 * and its CRC, the payload and its CRC (CRC-16-CCITT, 0xFFFF first, little
 * endian). Data frames are sequenced (DATA_SEQ: the other side ACKs them,
 * NAKs a bad one) or not (DATA_NSQ). Their payload is a command: 0x80,
 * target category (2: battery), target id (1: SAM), source id (0: us),
 * instance, request id (16 bits; 1-38 are events'), command id, data. A
 * request's answer comes back as a command with the same request id.
 *
 * The protocol part takes its bytes from callbacks, so it also runs on the
 * host against a pretend SAM (tests/ssam, SSAM_HOST).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#ifdef SSAM_HOST
#include <stdint.h>
#include <string.h>
#else
#include "mk.h"
#endif

enum { SSAM_TC_SAM = 1, SSAM_TC_BAT = 2, SSAM_TID_SAM = 1 };

typedef struct ssam ssam_t;
struct ssam {
    int  (*getc)(ssam_t *s);                         /* a byte that came, or -1: none now */
    void (*put)(ssam_t *s, const uint8_t *p, int n); /* bytes to send */
    void (*wait)(ssam_t *s);                         /* nothing came: wait a little (about 1 ms) */
    int64_t (*now)(void);                            /* ns */
    void (*event)(ssam_t *s, int tc, int cid, int iid, const uint8_t *p, int n);   /* (may be 0) */
    uint8_t seq;                                     /* our next frame's */
    uint16_t rqid;                                   /* our last request's id */
    uint8_t seen[8], nseen;                          /* its last data frames' numbers (a frame sent again is ACKed, not used again) */
    uint8_t rb[320];                                 /* what came, not yet a whole message */
    int rn;
    /* the request in progress: its frame ACKed (or NAKed), its answer */
    int acked, nak, want, got;
    uint8_t ackseq;                                  /* the frame waiting for its ACK */
    uint8_t *resp;
    int cap, rlen;
    int bad, resent;                                 /* (counts, for the log: messages refused, frames sent again) */
};

uint16_t ssam_crc(const uint8_t *p, int n);
/* A request; its answer's data (after the 8 bytes of command) in resp:
 * -> its length, -1: never ACKed, -2: no answer (each after 3 tries). */
int  ssam_request(ssam_t *s, int tc, int tid, int iid, int cid, const void *data, int n, void *resp, int cap);
void ssam_poll(ssam_t *s);                           /* what came meanwhile: frames ACKed, events */

#ifndef SSAM_HOST
/* acpid's side (ssam.c): SAM through MSHW0084's UART -> 0 when it answers;
 * the battery and charger as ACPI's methods would give them ("_STA",
 * "_BIX", "_BST", "_PSR") -> how many numbers, -1; what came meanwhile
 * (events: sam_news 1 the battery or charger changed, 2 its capacities). */
int  sam_start(int dev);
int  sam_battery(const char *what, int64_t *v, int max);
void sam_poll(void);
extern volatile int sam_news;
extern int sam_on;
#endif
