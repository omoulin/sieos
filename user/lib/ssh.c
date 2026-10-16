/*
 * ssh.c - SSH-2's transport layer (RFC 4253), for /bin/ssh and sshd.
 *
 *   client                                     server
 *   "SSH-2.0-..."  ◄──────── version lines ────────►  "SSH-2.0-..."
 *   KEXINIT        ◄──── algorithm lists ──────────►  KEXINIT
 *   ECDH_INIT Q_C  ─────────────────────────────────►
 *                  ◄──── ECDH_REPLY host key, Q_S, signature of H
 *   NEWKEYS        ◄───────────────────────────────►  NEWKEYS
 *   then every packet encrypted and authenticated, both ways
 *
 * One set of algorithms, the one OpenSSH prefers among those without
 * post-quantum parts: key exchange curve25519-sha256 (X25519, RFC 8731),
 * host keys ssh-ed25519 (RFC 8709), and chacha20-poly1305@openssh.com:
 * ChaCha20 with two keys, one for the 4-byte packet length and one for the
 * rest, the packet's sequence number as nonce, and a Poly1305 tag over the
 * encrypted packet (OpenSSH's PROTOCOL.chacha20poly1305). Its MAC is the
 * tag, so no separate MAC is used, and no compression.
 *
 * Strict key exchange (kex-strict-*-v00@openssh.com, the fix for the
 * "Terrapin" attack): when both sides offer it, sequence numbers start
 * again at 0 with the new keys, and any message out of place during the
 * exchange ends the connection (this side never accepts one anyway).
 *
 * The exchange hash H covers both version lines, both KEXINITs, the host
 * key, both ephemeral keys and the shared secret K; the server signs it,
 * and keys are derived from K and H (RFC 4253, 7.2). The first H is the
 * session id, which user authentication signs too (ssh.c, sshd.c).
 *
 * Also here: OpenSSH's key files ("openssh-key-v1", unencrypted), public
 * key lines (authorized_keys, known_hosts) and base64.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "ssh.h"
#include "mk/crypto.h"

#define VERSION "SSH-2.0-SIEOS_1.0"
static const char CIPHER[] = "chacha20-poly1305@openssh.com";

/* ---- Reading and writing the fields of a message (RFC 4251, 5). */
uint8_t ssh_u8(ssh_rd_t *r)
{
    if (r->p >= r->end) { r->bad = 1; return 0; }
    return *r->p++;
}

uint32_t ssh_u32(ssh_rd_t *r)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = v << 8 | ssh_u8(r);
    return v;
}

const uint8_t *ssh_str(ssh_rd_t *r, uint32_t *len)
{
    uint32_t n = ssh_u32(r);
    if (r->bad || n > (size_t)(r->end - r->p)) { r->bad = 1; r->p = r->end; *len = 0; return (const uint8_t *)""; }
    const uint8_t *s = r->p;
    r->p += n;
    *len = n;
    return s;
}

int ssh_is(ssh_rd_t *r, const char *s)
{
    uint32_t n;
    const uint8_t *p = ssh_str(r, &n);
    return n == strlen(s) && !memcmp(p, s, n);
}

void ssh_put8(ssh_wr_t *w, uint8_t v) { if (w->n < w->cap) w->p[w->n] = v; w->n++; }   /* past cap: refused at the end */
void ssh_put32(ssh_wr_t *w, uint32_t v) { for (int i = 24; i >= 0; i -= 8) ssh_put8(w, (uint8_t)(v >> i)); }
void ssh_putstr(ssh_wr_t *w, const void *s, size_t n)
{
    ssh_put32(w, (uint32_t)n);
    if (w->n + n <= w->cap) memcpy(w->p + w->n, s, n);
    w->n += n;
}
void ssh_puts(ssh_wr_t *w, const char *s) { ssh_putstr(w, s, strlen(s)); }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }

static void random_bytes(void *b, size_t n)
{
    while (sys_random(b, n) != (long)n) sys_sleep(50000000);   /* the kernel may still be gathering */
}

/* The nonce of packet `seq`: a 64-bit block counter's high half (0), then
 * the sequence number, big-endian. */
static void nonce(uint8_t n[12], uint32_t seq)
{
    memset(n, 0, 12);
    put_be32(n + 8, seq);
}

static void poly_key(uint8_t pk[32], const uint8_t key[32], const uint8_t n[12])
{
    memset(pk, 0, 32);
    chacha20_xor(key, n, 0, pk, pk, 32);          /* block 0's first 32 bytes */
}

/* ---- Packets: length, padding length, payload, padding (to a multiple of
 * 8; with the cipher, the length is left out of that count), tag. */
static void lock(ssh_t *s)   { while (__atomic_test_and_set(&s->lock, __ATOMIC_ACQUIRE)) sys_yield(); }
static void unlock(ssh_t *s) { __atomic_clear(&s->lock, __ATOMIC_RELEASE); }

ssh_wr_t *ssh_begin(ssh_t *s, int type)
{
    lock(s);
    s->w = (ssh_wr_t){ s->obuf + 5, 0, SSH_MAXPKT - 32 };
    ssh_put8(&s->w, (uint8_t)type);
    return &s->w;
}

long ssh_end(ssh_t *s)
{
    long r = -EINVAL;
    size_t n = s->w.n;
    if (n <= s->w.cap) {
        uint8_t *p = s->obuf, nc[12], pk[32];
        size_t pad = 8 - ((s->keyed_out ? 0 : 4) + 1 + n) % 8;
        if (pad < 4) pad += 8;
        uint32_t len = (uint32_t)(1 + n + pad);
        put_be32(p, len);
        p[4] = (uint8_t)pad;
        memset(p + 5 + n, 0, pad);
        size_t total = 4 + len;
        if (s->keyed_out) {
            nonce(nc, s->seq_out);
            chacha20_xor(s->key_out + 32, nc, 0, p, p, 4);
            chacha20_xor(s->key_out, nc, 1, p + 4, p + 4, len);
            poly_key(pk, s->key_out, nc);
            poly1305(p + total, pk, p, total);
            total += 16;
        }
        s->seq_out++;
        r = net_send(s->sock, p, total);
    }
    unlock(s);
    return r < 0 ? r : 0;
}

/* Until ibuf holds `need` bytes from ipos (need fits the buffer). */
static int fill(ssh_t *s, size_t need, unsigned timeout_ms)
{
    if (s->ipos + need > sizeof s->ibuf) {
        memmove(s->ibuf, s->ibuf + s->ipos, s->ilen - s->ipos);
        s->ilen -= s->ipos;
        s->ipos = 0;
    }
    while (s->ilen - s->ipos < need) {
        long r = net_recv(s->sock, s->ibuf + s->ilen, sizeof s->ibuf - s->ilen, timeout_ms);
        if (r <= 0) return -1;
        s->ilen += (size_t)r;
    }
    return 0;
}

/* One packet, as it comes (the key exchange wants every message). */
static int recv_any(ssh_t *s, ssh_rd_t *r, unsigned timeout_ms)
{
    uint8_t nc[12], lb[4], pk[32], tag[16];
    if (s->ipos == s->ilen) s->ipos = s->ilen = 0;
    if (fill(s, 4, timeout_ms)) return -1;
    nonce(nc, s->seq_in);
    memcpy(lb, s->ibuf + s->ipos, 4);
    if (s->keyed_in) chacha20_xor(s->key_in + 32, nc, 0, lb, lb, 4);
    uint32_t len = be32(lb), mac = s->keyed_in ? 16 : 0;
    if (len < 5 || len > SSH_MAXPKT || fill(s, 4 + len + mac, timeout_ms)) return -1;
    uint8_t *p = s->ibuf + s->ipos;
    if (s->keyed_in) {
        poly_key(pk, s->key_in, nc);
        poly1305(tag, pk, p, 4 + len);
        if (!ct_equal(tag, p + 4 + len, 16)) return -1;   /* forged or damaged */
        chacha20_xor(s->key_in, nc, 1, p + 4, p + 4, len);
    }
    s->ipos += 4 + len + mac;
    s->seq_in++;
    if (p[4] >= len) return -1;
    *r = (ssh_rd_t){ p + 5, p + 4 + len - p[4], 0 };
    return ssh_u8(r);
}

int ssh_recv(ssh_t *s, ssh_rd_t *r, unsigned timeout_ms)
{
    for (;;) {
        int t = recv_any(s, r, timeout_ms);
        if (t == SSH_DISCONNECT || t < 0) return -1;
        if (t != SSH_IGNORE && t != SSH_DEBUG && t != SSH_UNIMPLEMENTED && t != SSH_EXT_INFO) return t;
    }
}

void ssh_disconnect(ssh_t *s, const char *why)
{
    ssh_wr_t *w = ssh_begin(s, SSH_DISCONNECT);
    ssh_put32(w, 2);                              /* SSH_DISCONNECT_PROTOCOL_ERROR */
    ssh_puts(w, why);
    ssh_puts(w, "");
    ssh_end(s);
}

/* ---- The key exchange. */
static int has(const uint8_t *list, uint32_t n, const char *name)   /* in a name-list */
{
    size_t l = strlen(name);
    for (uint32_t i = 0; i + l <= n; ) {
        uint32_t e = i;
        while (e < n && list[e] != ',') e++;
        if (e - i == l && !memcmp(list + i, name, l)) return 1;
        i = e + 1;
    }
    return 0;
}

static void hstr(sha_t *h, const void *p, size_t n)    /* a string, into the hash */
{
    uint8_t l[4];
    put_be32(l, (uint32_t)n);
    sha_update(h, l, 4);
    sha_update(h, p, n);
}

/* Keys (RFC 4253, 7.2): HASH(K || H || letter || session id), extended to 64 bytes. */
static void derive(uint8_t out[64], const uint8_t *k, size_t kn, const uint8_t h[32], char x, const uint8_t sid[32])
{
    sha_t c;
    sha_init(&c, SHA256);
    sha_update(&c, k, kn); sha_update(&c, h, 32); sha_update(&c, &x, 1); sha_update(&c, sid, 32);
    sha_final(&c, out);
    sha_init(&c, SHA256);
    sha_update(&c, k, kn); sha_update(&c, h, 32); sha_update(&c, out, 32);
    sha_final(&c, out + 32);
}

void ssh_blob(uint8_t b[SSH_BLOB], const uint8_t pub[32])
{
    ssh_wr_t w = { b, 0, SSH_BLOB };
    ssh_puts(&w, "ssh-ed25519");
    ssh_putstr(&w, pub, 32);
}

int ssh_kex(ssh_t *s, const uint8_t *seed, const uint8_t *pub, const char **why)
{
    char peer[256];
    uint8_t *mine = 0, *theirs = 0, eph[32], q[32], qp[32], k[37], h[32], blob[SSH_BLOB], sig[64];
    uint32_t nmine = 0, ntheirs = 0, n;
    ssh_rd_t r;
    int ok = -1, t;
    const uint8_t *f;
    *why = "connection lost";
    /* version lines: ours, then the peer's (a server may send other lines first) */
    if (net_send(s->sock, VERSION "\r\n", sizeof VERSION + 1) < 0) return -1;
    for (int lines = 0;; lines++) {
        size_t len = 0;
        for (;;) {
            if (fill(s, 1, 30000)) return -1;
            char c = (char)s->ibuf[s->ipos++];
            if (c == '\n') break;
            if (len < sizeof peer - 1) peer[len++] = c;
        }
        if (len && peer[len - 1] == '\r') len--;
        peer[len] = 0;
        if (!memcmp(peer, "SSH-", 4)) break;
        if (lines == 20) { *why = "no SSH version line"; return -1; }
    }
    if (memcmp(peer, "SSH-2.0-", 8) && memcmp(peer, "SSH-1.99-", 9)) { *why = "not SSH version 2"; return -1; }

    /* KEXINIT both ways */
    ssh_wr_t *w = ssh_begin(s, SSH_KEXINIT);
    random_bytes(w->p + w->n, 16);
    w->n += 16;
    ssh_puts(w, s->server ? "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-s-v00@openssh.com"
                          : "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-c-v00@openssh.com");
    ssh_puts(w, "ssh-ed25519");
    ssh_puts(w, CIPHER); ssh_puts(w, CIPHER);
    ssh_puts(w, "hmac-sha2-256"); ssh_puts(w, "hmac-sha2-256");   /* (unused: the cipher has its own) */
    ssh_puts(w, "none"); ssh_puts(w, "none");
    ssh_puts(w, ""); ssh_puts(w, "");
    ssh_put8(w, 0); ssh_put32(w, 0);
    nmine = (uint32_t)w->n;
    if ((mine = malloc(nmine))) memcpy(mine, w->p, nmine);
    if (ssh_end(s) < 0 || !mine) goto out;
    if ((t = recv_any(s, &r, 30000)) != SSH_KEXINIT) { *why = "no key exchange"; goto out; }
    ntheirs = (uint32_t)(r.end - r.p + 1);
    if (!(theirs = malloc(ntheirs))) goto out;
    memcpy(theirs, r.p - 1, ntheirs);
    r.p += 16;                                    /* their cookie */
    f = ssh_str(&r, &n);
    int guess = n >= 17 && !memcmp(f, "curve25519-sha256", 17) && (n == 17 || f[17] == ',' || f[17] == '@');
    s->strict = has(f, n, s->server ? "kex-strict-c-v00@openssh.com" : "kex-strict-s-v00@openssh.com");
    *why = "no common algorithm (they need curve25519-sha256, ssh-ed25519, chacha20-poly1305@openssh.com)";
    if (!has(f, n, "curve25519-sha256") && !has(f, n, "curve25519-sha256@libssh.org")) goto out;
    f = ssh_str(&r, &n);
    guess = guess && n >= 11 && !memcmp(f, "ssh-ed25519", 11);
    if (!has(f, n, "ssh-ed25519")) goto out;
    for (int i = 0; i < 2; i++) { f = ssh_str(&r, &n); if (!has(f, n, CIPHER)) goto out; }
    ssh_str(&r, &n); ssh_str(&r, &n);             /* MACs */
    for (int i = 0; i < 2; i++) { f = ssh_str(&r, &n); if (!has(f, n, "none")) goto out; }
    ssh_str(&r, &n); ssh_str(&r, &n);             /* languages */
    if (ssh_u8(&r) && !guess && recv_any(s, &r, 30000) < 0) goto out;   /* their guessed packet, wrong: dropped */
    if (r.bad) { *why = "bad key exchange message"; goto out; }

    /* ephemeral X25519 keys, the shared secret K, the hash H */
    *why = "key exchange failed";
    random_bytes(eph, 32);
    x25519_base(q, eph);
    if (!s->server) {
        w = ssh_begin(s, SSH_KEX_ECDH_INIT);
        ssh_putstr(w, q, 32);
        if (ssh_end(s) < 0 || recv_any(s, &r, 30000) != SSH_KEX_ECDH_REPLY) goto out;
        ssh_rd_t hk;
        hk.p = ssh_str(&r, &n); hk.end = hk.p + n; hk.bad = 0;
        if (!ssh_is(&hk, "ssh-ed25519") || (f = ssh_str(&hk, &n), n != 32) || hk.bad) { *why = "not an Ed25519 host key"; goto out; }
        memcpy(s->hostkey, f, 32);
        f = ssh_str(&r, &n);
        if (n != 32) goto out;
        memcpy(qp, f, 32);
        ssh_rd_t sg;
        sg.p = ssh_str(&r, &n); sg.end = sg.p + n; sg.bad = 0;
        if (!ssh_is(&sg, "ssh-ed25519") || (f = ssh_str(&sg, &n), n != 64) || r.bad || sg.bad) goto out;
        memcpy(sig, f, 64);
    } else {
        if (recv_any(s, &r, 30000) != SSH_KEX_ECDH_INIT || (f = ssh_str(&r, &n), n != 32)) goto out;
        memcpy(qp, f, 32);
        memcpy(s->hostkey, pub, 32);
    }
    uint8_t shared[32];
    if (x25519(shared, eph, qp)) goto out;
    wipe(eph, sizeof eph);
    /* K as an mpint: big-endian, no leading zeros, a 0 first if the top bit is set */
    int z = 0;
    while (z < 31 && !shared[z]) z++;
    int lead = shared[z] >> 7;
    put_be32(k, (uint32_t)(32 - z + lead));
    k[4] = 0;
    memcpy(k + 4 + lead, shared + z, 32 - z);
    size_t kn = 4 + lead + 32 - z;
    wipe(shared, sizeof shared);
    ssh_blob(blob, s->hostkey);
    sha_t hh;
    sha_init(&hh, SHA256);
    if (s->server) { hstr(&hh, peer, strlen(peer)); hstr(&hh, VERSION, strlen(VERSION)); hstr(&hh, theirs, ntheirs); hstr(&hh, mine, nmine); }
    else { hstr(&hh, VERSION, strlen(VERSION)); hstr(&hh, peer, strlen(peer)); hstr(&hh, mine, nmine); hstr(&hh, theirs, ntheirs); }
    hstr(&hh, blob, SSH_BLOB);
    hstr(&hh, s->server ? qp : q, 32);
    hstr(&hh, s->server ? q : qp, 32);
    sha_update(&hh, k, kn);
    sha_final(&hh, h);
    if (s->server) {
        ed25519_sign(sig, h, 32, seed, pub);
        w = ssh_begin(s, SSH_KEX_ECDH_REPLY);
        ssh_putstr(w, blob, SSH_BLOB);
        ssh_putstr(w, q, 32);
        ssh_put32(w, 4 + 11 + 4 + 64);
        ssh_puts(w, "ssh-ed25519");
        ssh_putstr(w, sig, 64);
        if (ssh_end(s) < 0) goto out;
    } else if (ed25519_verify(sig, h, 32, s->hostkey)) { *why = "the host key's signature is wrong"; goto out; }
    memcpy(s->sid, h, 32);

    /* NEWKEYS: from now on, the new keys (and with strict exchange, sequence numbers from 0) */
    w = ssh_begin(s, SSH_NEWKEYS);
    if (ssh_end(s) < 0) goto out;
    derive(s->key_out, k, kn, h, s->server ? 'D' : 'C', s->sid);
    s->keyed_out = 1;
    if (s->strict) s->seq_out = 0;
    if (recv_any(s, &r, 30000) != SSH_NEWKEYS) goto out;
    derive(s->key_in, k, kn, h, s->server ? 'C' : 'D', s->sid);
    s->keyed_in = 1;
    if (s->strict) s->seq_in = 0;
    ok = 0;
out:
    wipe(k, sizeof k);
    wipe(eph, sizeof eph);
    free(mine); free(theirs);
    return ok;
}

/* "SHA256:..." as OpenSSH shows a key: base64 of its blob's SHA-256, no padding. */
void ssh_fingerprint(char out[52], const uint8_t pub[32])
{
    uint8_t blob[SSH_BLOB], h[32];
    ssh_blob(blob, pub);
    sha(SHA256, h, blob, SSH_BLOB);
    memcpy(out, "SHA256:", 7);
    size_t n = b64_enc(out + 7, h, 32);
    out[7 + n - 1] = 0;                           /* (one '=' of padding) */
}

/* printf into buf (cut to size). */
typedef struct { char *b; size_t n, size; } sbuf_t;
static void sput(void *c, char ch) { sbuf_t *s = c; if (s->n + 1 < s->size) s->b[s->n++] = ch; }
void ssh_fmt(char *buf, size_t size, const char *f, ...)
{
    sbuf_t s = { buf, 0, size };
    va_list ap;
    va_start(ap, f);
    vformat(sput, &s, f, ap);
    va_end(ap);
    buf[s.n] = 0;
}

/* ---- Base64 (RFC 4648). */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_enc(char *o, const uint8_t *in, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        o[k++] = B64[v >> 18]; o[k++] = B64[v >> 12 & 63];
        o[k++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        o[k++] = i + 2 < n ? B64[v & 63] : '=';
    }
    o[k] = 0;
    return k;
}

/* Up to the first character that is not base64 ("=" padding included);
 * multiline: line breaks are skipped. */
long b64_dec(uint8_t *out, size_t max, const char *in, int multiline)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (;; in++) {
        if (multiline && (*in == '\n' || *in == '\r')) continue;
        const char *c = *in ? strchr(B64, *in) : 0;
        if (!c) break;
        acc = acc << 6 | (uint32_t)(c - B64);
        if ((bits += 6) >= 8) {
            bits -= 8;
            if (n == max) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (long)n;
}

/* ---- Key files. */
static const char BEGIN[] = "-----BEGIN OPENSSH PRIVATE KEY-----\n", END[] = "-----END OPENSSH PRIVATE KEY-----\n";

int ssh_key_load(const char *path, uint8_t seed[32], uint8_t pub[32])
{
    uint8_t b[1024];
    size_t len;
    char *t = file_get(path, &len);
    if (!t) return -ENOENT;
    char *p = t;
    while (*p && memcmp(p, BEGIN, sizeof BEGIN - 2)) p++;
    long n = *p ? b64_dec(b, sizeof b, p + sizeof BEGIN - 1, 1) : -1;
    wipe(t, len);
    free(t);
    int e = -EINVAL;
    if (n < 15 || memcmp(b, "openssh-key-v1", 15)) goto out;
    ssh_rd_t r = { b + 15, b + n, 0 }, k;
    if (!ssh_is(&r, "none")) { e = -EACCES; goto out; }   /* encrypted with a passphrase */
    if (!ssh_is(&r, "none")) goto out;
    uint32_t l;
    ssh_str(&r, &l);
    if (ssh_u32(&r) != 1) goto out;
    ssh_str(&r, &l);                              /* the public key, again inside */
    k.p = ssh_str(&r, &l); k.end = k.p + l; k.bad = 0;
    if (ssh_u32(&k) != ssh_u32(&k) || !ssh_is(&k, "ssh-ed25519")) goto out;
    ssh_str(&k, &l);
    const uint8_t *sk = ssh_str(&k, &l);
    if (l != 64 || k.bad || r.bad) goto out;
    memcpy(seed, sk, 32);
    memcpy(pub, sk + 32, 32);
    e = 0;
out:
    wipe(b, sizeof b);
    return e;
}

static int put_file(const char *path, const char *text, int mode)
{
    long h = fs_open(path, FS_WRONLY | FS_CREAT | FS_EXCL, mode), r;
    if (h < 0) return (int)h;
    r = fs_write(h, 0, text, strlen(text));
    fs_close(h);
    return r < 0 ? (int)r : 0;
}

int ssh_key_new(const char *path, const char *comment)
{
    uint8_t seed[32], pub[32], blob[SSH_BLOB], b[512], pv[256];
    char e[700], text[800], pubpath[FS_PATH];
    uint32_t check;
    random_bytes(seed, 32);
    random_bytes(&check, 4);
    ed25519_public(pub, seed);
    ssh_blob(blob, pub);
    ssh_wr_t w = { pv, 0, sizeof pv };            /* the private part */
    ssh_put32(&w, check); ssh_put32(&w, check);
    ssh_puts(&w, "ssh-ed25519");
    ssh_putstr(&w, pub, 32);
    ssh_put32(&w, 64);
    for (int i = 0; i < 32; i++) ssh_put8(&w, seed[i]);
    for (int i = 0; i < 32; i++) ssh_put8(&w, pub[i]);
    ssh_puts(&w, comment);
    for (uint8_t i = 1; w.n % 8; i++) ssh_put8(&w, i);
    ssh_wr_t f = { b, 15, sizeof b };
    memcpy(b, "openssh-key-v1", 15);
    ssh_puts(&f, "none"); ssh_puts(&f, "none"); ssh_puts(&f, "");
    ssh_put32(&f, 1);
    ssh_putstr(&f, blob, SSH_BLOB);
    ssh_putstr(&f, pv, w.n);
    size_t n = b64_enc(e, b, f.n), k = strlcpy(text, BEGIN, sizeof text);
    for (size_t i = 0; i < n; i += 70) {          /* lines of 70, as OpenSSH writes them */
        size_t c = n - i < 70 ? n - i : 70;
        memcpy(text + k, e + i, c);
        k += c;
        text[k++] = '\n';
    }
    strlcpy(text + k, END, sizeof text - k);
    int r = put_file(path, text, 0600);
    wipe(seed, sizeof seed); wipe(pv, sizeof pv); wipe(b, sizeof b); wipe(e, sizeof e); wipe(text, sizeof text);
    if (r) return r;
    k = strlcpy(text, "ssh-ed25519 ", sizeof text);
    k += b64_enc(text + k, blob, SSH_BLOB);
    text[k++] = ' ';
    k += strlcpy(text + k, comment, sizeof text - k - 1);
    strlcpy(text + k, "\n", sizeof text - k);
    strlcpy(pubpath, path, sizeof pubpath - 4);
    strlcpy(pubpath + strlen(pubpath), ".pub", 5);
    fs_unlink(pubpath);
    return put_file(pubpath, text, 0644);
}

int ssh_key_listed(const char *t, const char *name, const uint8_t *pub)
{
    uint8_t want[SSH_BLOB], got[64];
    int other = 0;
    size_t nl = name ? strlen(name) : 0;
    ssh_blob(want, pub);
    for (const char *line = t; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : 0) {
        const char *p = line, *e = line;
        while (*e && *e != '\n') e++;
        if (name) {                               /* "name[,name...] ssh-ed25519 KEY" */
            int match = 0;
            while (p < e && *p != ' ') {
                const char *c = p;
                while (c < e && *c != ',' && *c != ' ') c++;
                if ((size_t)(c - p) == nl && !memcmp(p, name, nl)) match = 1;
                p = c + (*c == ',');
            }
            if (!match) continue;
        }
        for (; p + 12 < e; p++)                   /* the key, after any options */
            if ((p == line || p[-1] == ' ') && !memcmp(p, "ssh-ed25519 ", 12)) break;
        if (p + 12 >= e) continue;
        if (b64_dec(got, sizeof got, p + 12, 0) == SSH_BLOB && !memcmp(got, want, SSH_BLOB)) return 1;
        other = 1;
    }
    return name && other ? -1 : 0;
}
