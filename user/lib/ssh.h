/*
 * ssh.h - SSH-2's transport (RFC 4253) for /bin/ssh and sshd (user/lib/ssh.c):
 * one modern set of algorithms, what OpenSSH negotiates by default, and
 * OpenSSH's key files (docs/ssh.md).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include "net.h"

/* Message numbers (RFC 4250). */
enum {
    SSH_DISCONNECT = 1, SSH_IGNORE, SSH_UNIMPLEMENTED, SSH_DEBUG, SSH_SERVICE_REQUEST, SSH_SERVICE_ACCEPT,
    SSH_EXT_INFO, SSH_KEXINIT = 20, SSH_NEWKEYS, SSH_KEX_ECDH_INIT = 30, SSH_KEX_ECDH_REPLY,
    SSH_USERAUTH_REQUEST = 50, SSH_USERAUTH_FAILURE, SSH_USERAUTH_SUCCESS, SSH_USERAUTH_BANNER,
    SSH_USERAUTH_PK_OK = 60, SSH_GLOBAL_REQUEST = 80, SSH_REQUEST_SUCCESS, SSH_REQUEST_FAILURE,
    SSH_CHANNEL_OPEN = 90, SSH_CHANNEL_OPEN_CONFIRMATION, SSH_CHANNEL_OPEN_FAILURE, SSH_CHANNEL_WINDOW_ADJUST,
    SSH_CHANNEL_DATA, SSH_CHANNEL_EXTENDED_DATA, SSH_CHANNEL_EOF, SSH_CHANNEL_CLOSE, SSH_CHANNEL_REQUEST,
    SSH_CHANNEL_SUCCESS, SSH_CHANNEL_FAILURE,
};
#define SSH_MAXPKT 35000                    /* the largest packet taken (RFC 4253: 32768 bytes of data) */
#define SSH_MAXDATA 32768                   /* the most channel data per packet, either way */

/* Reading a message: a cursor; any read past the end sets `bad`. */
typedef struct { const uint8_t *p, *end; int bad; } ssh_rd_t;
uint8_t        ssh_u8(ssh_rd_t *r);
uint32_t       ssh_u32(ssh_rd_t *r);
const uint8_t *ssh_str(ssh_rd_t *r, uint32_t *len);
int            ssh_is(ssh_rd_t *r, const char *s);     /* reads a string: 1 if it is s */

/* Writing one: into the connection's packet buffer, up to its end. */
typedef struct { uint8_t *p; size_t n, cap; } ssh_wr_t;
void ssh_put8(ssh_wr_t *w, uint8_t v);
void ssh_put32(ssh_wr_t *w, uint32_t v);
void ssh_putstr(ssh_wr_t *w, const void *s, size_t n);
void ssh_puts(ssh_wr_t *w, const char *s);

/* A connection. After ssh_kex, packets go encrypted both ways
 * (chacha20-poly1305@openssh.com). Sending is safe from several threads
 * (a lock); receiving is for one thread. */
typedef struct {
    long sock;
    int server, strict, keyed_in, keyed_out;
    uint32_t seq_in, seq_out;
    uint8_t key_in[64], key_out[64];        /* K_2 (data) then K_1 (lengths) */
    uint8_t sid[32];                        /* the session id: the first exchange hash */
    uint8_t hostkey[32];                    /* the server's Ed25519 key (client: to check) */
    char lock;
    ssh_wr_t w;                             /* the packet being written (ssh_begin) */
    size_t ipos, ilen;                      /* received bytes: ibuf[ipos..ilen) */
    uint8_t ibuf[SSH_MAXPKT + 64], obuf[SSH_MAXPKT + 64];
} ssh_t;

/* Version lines and key exchange (curve25519-sha256, ssh-ed25519); the
 * server signs with its key (seed, pub), the client gets the server's
 * key in s->hostkey. -> 0, or -1 with *why. */
int ssh_kex(ssh_t *s, const uint8_t *seed, const uint8_t *pub, const char **why);
/* A packet: ssh_begin (takes the send lock) -> the writer; ssh_end sends it. */
ssh_wr_t *ssh_begin(ssh_t *s, int type);
long ssh_end(ssh_t *s);
/* The next message (IGNORE, DEBUG and such are skipped): its type; r covers
 * the rest. -1: closed or broken (DISCONNECT, a bad packet, the network). */
int ssh_recv(ssh_t *s, ssh_rd_t *r, unsigned timeout_ms);
void ssh_disconnect(ssh_t *s, const char *why);

/* Keys: OpenSSH's files. blob = string "ssh-ed25519", string pub (51 bytes). */
#define SSH_BLOB 51
void   ssh_blob(uint8_t blob[SSH_BLOB], const uint8_t pub[32]);
size_t b64_enc(char *out, const uint8_t *in, size_t n);              /* -> length, 0-terminated */
long   b64_dec(uint8_t *out, size_t max, const char *in, int multiline);     /* -> bytes or -1 */
int    ssh_key_load(const char *path, uint8_t seed[32], uint8_t pub[32]);    /* "openssh-key-v1", unencrypted */
int    ssh_key_new(const char *path, const char *comment);                    /* path and path.pub */
/* Is this key (blob) on one of the "ssh-ed25519 BASE64 ..." lines of text?
 * name: the line must start with it (known_hosts) or 0 (authorized_keys).
 * -> 1 yes, 0 not there, -1 the name has another key (known_hosts). */
int    ssh_key_listed(const char *text, const char *name, const uint8_t *pub);
void   ssh_fingerprint(char out[52], const uint8_t pub[32]);           /* "SHA256:...", as OpenSSH shows it */
void   ssh_fmt(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
