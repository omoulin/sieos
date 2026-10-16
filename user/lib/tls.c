/*
 * tls.c - A TLS 1.3 client (RFC 8446), and TLS 1.2 (RFC 5246) for servers
 * that know no better: what makes https:// private and authentic.
 *
 *   ClientHello  ──────────────►   (our X25519 public key, the ciphers we know)
 *                ◄──────────────   ServerHello (its key share): both now compute
 *                                  the same secret; everything after is encrypted
 *                ◄──────────────   EncryptedExtensions, Certificate (its chain),
 *                                  CertificateVerify (a signature over everything
 *                                  so far, with the certificate's key), Finished
 *  Finished      ──────────────►   then application data both ways
 *
 * Keys come from the "key schedule": HKDF over the shared secret and a hash
 * of every handshake message (the transcript), so tampering with any of
 * them breaks the Finished check. Ciphers: ChaCha20-Poly1305 (preferred: no
 * tables, fast in plain integer code) and AES-128-GCM, both with SHA-256.
 * Key exchange: X25519, or P-256 or P-384 if the server asks for it
 * (HelloRetryRequest).
 *
 * TLS 1.2, when a server answers with it: the same idea in the older shape
 * (tls12 below). Only what keeps forward secrecy and authenticated
 * encryption: ECDHE (X25519, P-256 or P-384) signed by the certificate's key, then
 * ChaCha20-Poly1305, AES-128-GCM or AES-256-GCM; keys from the PRF (HMAC
 * with SHA-256 or SHA-384), the extended master secret (RFC 7627) when the
 * server offers it. No static-RSA key exchange, no CBC, no SHA-1 MACs, no
 * resumption, no renegotiation. A server that speaks 1.3 but was made to
 * answer 1.2 says so in its random (RFC 8446 4.1.3): we refuse.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "net.h"
#include "mk/crypto.h"

#define REC_MAX 16384
enum { CT_CCS = 20, CT_ALERT = 21, CT_HS = 22, CT_APP = 23 };
enum { HS_HELLO_REQ = 0, HS_CH = 1, HS_SH = 2, HS_TICKET = 4, HS_EE = 8, HS_CERT = 11, HS_SKE = 12,
       HS_CREQ = 13, HS_SHD = 14, HS_CV = 15, HS_CKE = 16, HS_FIN = 20, HS_KU = 24 };

/* The cipher suites we offer, in our order of preference. */
static const struct suite { uint16_t id; uint8_t aead, hash; const char *name; } SUITES[] = {
    { 0x1303, AEAD_CHACHA20_POLY1305, SHA256, "TLS 1.3 TLS_CHACHA20_POLY1305_SHA256" },
    { 0x1301, AEAD_AES128_GCM, SHA256, "TLS 1.3 TLS_AES_128_GCM_SHA256" },
    { 0xcca9, AEAD_CHACHA20_POLY1305, SHA256, "TLS 1.2 ECDHE-ECDSA-CHACHA20-POLY1305" },
    { 0xcca8, AEAD_CHACHA20_POLY1305, SHA256, "TLS 1.2 ECDHE-RSA-CHACHA20-POLY1305" },
    { 0xc02b, AEAD_AES128_GCM, SHA256, "TLS 1.2 ECDHE-ECDSA-AES128-GCM-SHA256" },
    { 0xc02f, AEAD_AES128_GCM, SHA256, "TLS 1.2 ECDHE-RSA-AES128-GCM-SHA256" },
    { 0xc02c, AEAD_AES256_GCM, SHA384, "TLS 1.2 ECDHE-ECDSA-AES256-GCM-SHA384" },
    { 0xc030, AEAD_AES256_GCM, SHA384, "TLS 1.2 ECDHE-RSA-AES256-GCM-SHA384" },
};
#define NSUITES (sizeof SUITES / sizeof SUITES[0])

struct tls {
    long sock;
    int flags, eof, aead_alg, v12, both;       /* v12: TLS 1.2; both: th384 kept too */
    size_t ex;                                 /* 1.2 GCM: 8 bytes of the nonce in each record */
    const struct suite *cs;
    size_t keylen;
    sha_t th, th384;                           /* the transcript hash (the suite's; SHA-384 too
                                                  until the server has chosen) */
    aead_t rd, wr;
    uint8_t rd_iv[12], wr_iv[12], rd_secret[32], wr_secret[32];
    uint64_t rd_seq, wr_seq;
    int rd_on, wr_on, rd_ready;                /* records protected yet? (1.2: rd_on at the
                                                  server's ChangeCipherSpec once rd_ready) */
    uint8_t in[REC_MAX + 256 + 5];             /* record read buffer */
    size_t inlen;
    uint8_t pt[REC_MAX + 256];                 /* the current record's plaintext */
    size_t ptpos, ptlen;
    int pttype;
    uint8_t *hs;                               /* handshake messages, reassembled */
    size_t hslen, hscap, consumed;          /* consumed: the message hs_next last returned */
    unsigned timeout;
};

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put24(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned get24(const uint8_t *p) { return (unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2]; }
static void put64(uint8_t *p, uint64_t v) { for (int i = 7; i >= 0; i--, v >>= 8) p[i] = (uint8_t)v; }

/* ---- HKDF-Expand-Label and the traffic keys. */
static void expand_label(uint8_t *out, size_t olen, const uint8_t *secret, const char *label,
                         const uint8_t *ctx, size_t clen)
{
    uint8_t info[300];
    size_t ll = strlen(label);
    put16(info, (unsigned)olen);
    info[2] = (uint8_t)(6 + ll);
    memcpy(info + 3, "tls13 ", 6);
    memcpy(info + 9, label, ll);
    info[9 + ll] = (uint8_t)clen;
    memcpy(info + 10 + ll, ctx, clen);
    hkdf_expand(SHA256, out, olen, secret, 32, info, 10 + ll + clen);
}

static void derive(uint8_t *out, const uint8_t *secret, const char *label, const uint8_t *hash)
{
    expand_label(out, 32, secret, label, hash, 32);
}

static void th_now(struct tls *t, uint8_t *h)       /* the transcript hash so far */
{
    sha_t c = t->th;
    sha_final(&c, h);
}

static void set_keys(struct tls *t, int write, const uint8_t secret[32])
{
    uint8_t key[32];
    expand_label(key, t->keylen, secret, "key", 0, 0);
    if (write) {
        expand_label(t->wr_iv, 12, secret, "iv", 0, 0);
        aead_init(&t->wr, t->aead_alg, key);
        memcpy(t->wr_secret, secret, 32);
        t->wr_seq = 0; t->wr_on = 1;
    } else {
        expand_label(t->rd_iv, 12, secret, "iv", 0, 0);
        aead_init(&t->rd, t->aead_alg, key);
        memcpy(t->rd_secret, secret, 32);
        t->rd_seq = 0; t->rd_on = 1;
    }
    wipe(key, sizeof key);
}

/* The record nonce: iv xor the sequence number. TLS 1.2's GCM has a 4-byte
 * iv (the rest zero) and sends the other 8 bytes, the sequence number here. */
static void nonce(uint8_t n[12], const uint8_t iv[12], uint64_t seq)
{
    memcpy(n, iv, 12);
    for (int i = 0; i < 8; i++) n[11 - i] ^= (uint8_t)(seq >> (8 * i));
}

/* TLS 1.2's additional data: sequence number, type, version, length. */
static void ad12(uint8_t ad[13], uint64_t seq, int type, size_t len)
{
    put64(ad, seq); ad[8] = (uint8_t)type; ad[9] = 3; ad[10] = 3; put16(ad + 11, (unsigned)len);
}

/* TLS 1.2's PRF (RFC 5246 5): P_hash(secret, label + seed), the seed in two parts. */
static void prf(int alg, uint8_t *out, size_t olen, const uint8_t *secret, size_t slen, const char *label,
                const uint8_t *s1, size_t l1, const uint8_t *s2, size_t l2)
{
    size_t hl = sha_len(alg), ll = strlen(label);
    uint8_t a[64], blk[64];
    hmac_t h;
    for (size_t off = 0; off < olen; off += hl) {
        hmac_init(&h, alg, secret, slen);       /* A(i) = HMAC(A(i-1)), A(0) = label + seed */
        if (off) hmac_update(&h, a, hl);
        else { hmac_update(&h, label, ll); hmac_update(&h, s1, l1); hmac_update(&h, s2, l2); }
        hmac_final(&h, a);
        hmac_init(&h, alg, secret, slen);       /* the block: HMAC(A(i) + label + seed) */
        hmac_update(&h, a, hl); hmac_update(&h, label, ll); hmac_update(&h, s1, l1); hmac_update(&h, s2, l2);
        hmac_final(&h, blk);
        memcpy(out + off, blk, olen - off < hl ? olen - off : hl);
    }
    wipe(blk, sizeof blk);
}

/* ---- Records out. */
static long rec_send(struct tls *t, int type, const uint8_t *data, size_t n)
{
    static uint8_t rec[REC_MAX + 256 + 5];
    for (size_t off = 0; off < n || (n == 0 && off == 0); ) {
        size_t c = n - off < REC_MAX ? n - off : REC_MAX;
        size_t len;
        if (t->wr_on && (t->v12 || type != CT_CCS)) {
            /* 1.3: the data then its real type, as "application data"; 1.2: the
             * data as it is, after the explicit part of the nonce (GCM) */
            static uint8_t inner[REC_MAX + 1];
            uint8_t no[12], ad[13];
            size_t m = c, ex = t->ex;
            memcpy(inner, data + off, c);
            if (!t->v12) inner[m++] = (uint8_t)type;
            len = ex + m + 16;
            rec[0] = t->v12 ? (uint8_t)type : CT_APP; rec[1] = 3; rec[2] = 3; put16(rec + 3, (unsigned)len);
            nonce(no, t->wr_iv, t->wr_seq);
            memcpy(rec + 5, no + 4, ex);
            if (t->v12) ad12(ad, t->wr_seq, type, c); else memcpy(ad, rec, 5);
            t->wr_seq++;
            aead_seal(&t->wr, no, ad, t->v12 ? 13 : 5, inner, m, rec + 5 + ex);
        } else {
            len = c;
            rec[0] = (uint8_t)type; rec[1] = 3; rec[2] = type == CT_HS && !t->wr_on && !t->v12 ? 1 : 3; put16(rec + 3, (unsigned)len);
            memcpy(rec + 5, data + off, c);
        }
        long e = net_send(t->sock, rec, 5 + len);
        if (e < 0) return e;
        off += c;
        if (n == 0) break;
    }
    return (long)n;
}

static long hs_send(struct tls *t, const uint8_t *msg, size_t n)
{
    sha_update(&t->th, msg, n);
    if (t->both) sha_update(&t->th384, msg, n);
    return rec_send(t, CT_HS, msg, n);
}

/* ---- Records in: one whole record into t->in, decrypted into t->pt. */
static long fill(struct tls *t, size_t want)
{
    while (t->inlen < want) {
        long r = net_recv(t->sock, t->in + t->inlen, sizeof t->in - t->inlen, t->timeout);
        if (r < 0) return r;
        if (r == 0) return -ECONNRESET;
        t->inlen += (size_t)r;
    }
    return 0;
}

static long rec_read(struct tls *t)
{
    long e;
    if ((e = fill(t, 5))) return e;
    size_t len = get16(t->in + 3);
    if (len > REC_MAX + 256) return -EPROTO;
    if ((e = fill(t, 5 + len))) return e;
    int type = t->in[0];
    uint8_t *body = t->in + 5;
    if (type == CT_CCS) {                      /* 1.3: compatibility noise; 1.2: keys on */
        t->ptlen = 0; t->pttype = CT_CCS;
        if (t->rd_ready) { t->rd_on = 1; t->rd_ready = 0; }
    } else if (t->rd_on && (t->v12 || type == CT_APP)) {
        size_t ex = t->ex, m = len - ex - 16;
        uint8_t no[12], ad[13];
        if (len < ex + 17 - (size_t)t->v12) return -EPROTO;
        nonce(no, t->rd_iv, t->rd_seq);
        memcpy(no + 4, body, ex);
        if (t->v12) ad12(ad, t->rd_seq, type, m); else memcpy(ad, t->in, 5);
        t->rd_seq++;
        if (aead_open(&t->rd, no, ad, t->v12 ? 13 : 5, body + ex, len - ex, t->pt)) return -EPROTO;
        t->pttype = type;
        if (!t->v12) {
            while (m && !t->pt[m - 1]) m--;    /* padding, then the real type */
            if (!m) return -EPROTO;
            t->pttype = t->pt[--m];
        }
        t->ptlen = m;
    } else if (!t->rd_on && (type == CT_HS || type == CT_ALERT)) {
        memcpy(t->pt, body, len);
        t->ptlen = len; t->pttype = type;
    } else return -EPROTO;
    t->ptpos = 0;
    memmove(t->in, t->in + 5 + len, t->inlen - 5 - len);
    t->inlen -= 5 + len;
    if (t->pttype == CT_ALERT) {
        if (t->ptlen >= 2 && t->pt[1] == 0) { t->eof = 1; return 0; }   /* close_notify */
        if (t->v12 && t->ptlen >= 2 && t->pt[0] == 1) { t->ptlen = 0; t->pttype = CT_CCS; return 0; }  /* a warning: noise */
        return -EPROTO;
    }
    return 0;
}

/* The next handshake message (type, body); it stays in t->hs until the next call. */
static long hs_next(struct tls *t, int *type, uint8_t **body, size_t *blen)
{
    if (t->consumed) { memmove(t->hs, t->hs + t->consumed, t->hslen - t->consumed); t->hslen -= t->consumed; t->consumed = 0; }
    for (;;) {
        if (t->hslen >= 4 && t->hslen >= 4 + get24(t->hs + 1)) {
            *type = t->hs[0]; *body = t->hs + 4; *blen = get24(t->hs + 1);
            sha_update(&t->th, t->hs, 4 + *blen);
            if (t->both) sha_update(&t->th384, t->hs, 4 + *blen);
            t->consumed = 4 + *blen;
            return 0;
        }
        long e = rec_read(t);
        if (e) return e;
        if (t->eof) return -ECONNRESET;
        if (t->pttype == CT_CCS) continue;
        if (t->pttype != CT_HS) return -EPROTO;
        if (t->hslen + t->ptlen > t->hscap) {
            size_t cap = t->hscap ? t->hscap * 2 : 32768;
            while (cap < t->hslen + t->ptlen) cap *= 2;
            if (cap > (1 << 20)) return -EPROTO;
            uint8_t *n = realloc(t->hs, cap);
            if (!n) return -ENOMEM;
            t->hs = n; t->hscap = cap;
        }
        memcpy(t->hs + t->hslen, t->pt, t->ptlen);
        t->hslen += t->ptlen;
    }
}

/* ---- The handshake. */
static const uint8_t HRR_RANDOM[32] = {
    0xcf,0x21,0xad,0x74,0xe5,0x9a,0x61,0x11,0xbe,0x1d,0x8c,0x02,0x1e,0x65,0xb8,0x91,
    0xc2,0xa2,0x11,0x16,0x7a,0xbb,0x8c,0x5e,0x07,0x9e,0x09,0xe2,0xc8,0xa8,0x33,0x9c };

static size_t client_hello(uint8_t *m, const char *host, int group, const uint8_t *pub, size_t publen,
                           const uint8_t *cookie, size_t cookielen, const uint8_t sid[32])
{
    uint8_t *p = m + 4;
    put16(p, 0x0303); p += 2;
    sys_random(p, 32); p += 32;
    *p++ = 32; memcpy(p, sid, 32); p += 32;
    put16(p, 2 * NSUITES); p += 2;
    for (size_t i = 0; i < NSUITES; i++, p += 2) put16(p, SUITES[i].id);
    *p++ = 1; *p++ = 0;                                                 /* no compression */
    uint8_t *ext = p; p += 2;
    int ip = 1;
    for (const char *s = host; *s; s++) if ((*s < '0' || *s > '9') && *s != '.') ip = 0;
    if (!ip) {                                                          /* server_name */
        size_t hl = strlen(host);
        put16(p, 0); put16(p + 2, (unsigned)hl + 5); put16(p + 4, (unsigned)hl + 3); p[6] = 0;
        put16(p + 7, (unsigned)hl); memcpy(p + 9, host, hl); p += 9 + hl;
    }
    put16(p, 0x000a); put16(p + 2, 8); put16(p + 4, 6);                 /* X25519, P-256, P-384 */
    put16(p + 6, 0x001d); put16(p + 8, 0x0017); put16(p + 10, 0x0018); p += 12;
    static const uint16_t sigs[] = { 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601 };
    put16(p, 0x000d); put16(p + 2, 2 + 2 * 8); put16(p + 4, 2 * 8); p += 6;
    for (int i = 0; i < 8; i++) { put16(p, sigs[i]); p += 2; }
    put16(p, 0x0033); put16(p + 2, (unsigned)publen + 6); put16(p + 4, (unsigned)publen + 4);   /* key_share */
    put16(p + 6, (unsigned)group); put16(p + 8, (unsigned)publen); memcpy(p + 10, pub, publen); p += 10 + publen;
    put16(p, 0x002b); put16(p + 2, 5); p[4] = 4; put16(p + 5, 0x0304); put16(p + 7, 0x0303); p += 9;  /* 1.3, 1.2 */
    /* for 1.2: uncompressed points, the extended master secret, an empty renegotiation_info */
    static const uint8_t v12[] = { 0, 0x0b, 0, 2, 1, 0, 0, 0x17, 0, 0, 0xff, 1, 0, 1, 0 };
    memcpy(p, v12, sizeof v12); p += sizeof v12;
    put16(p, 0x0010); put16(p + 2, 11); put16(p + 4, 9); p[6] = 8; memcpy(p + 7, "http/1.1", 8); p += 15;
    if (cookielen) { put16(p, 0x002c); put16(p + 2, (unsigned)cookielen); memcpy(p + 4, cookie, cookielen); p += 4 + cookielen; }
    put16(ext, (unsigned)(p - ext - 2));
    m[0] = HS_CH;
    put24(m + 1, (unsigned)(p - m - 4));
    return (size_t)(p - m);
}

static long fail(tls_t *t, long e)
{
    if (t) { free(t->hs); net_close(t->sock); wipe(t, sizeof *t); free(t); }
    return e;
}

/* The server's Certificate message: a copy of the chain in *chain (t->hs
 * moves on; the caller frees it), checked against our roots, and its first
 * certificate in *leaf. 1.3 adds a request context and extensions per entry. */
static long certificates(struct tls *t, const char *host, const uint8_t *b, size_t bl, uint8_t **chain, x509_t *leaf)
{
    const uint8_t *certs[10]; size_t clens[10]; int n = 0, v13 = !t->v12;
    size_t i = v13 ? 1 + (bl ? b[0] : 0) : 0, end;
    if (i + 3 > bl || (end = i + 3 + get24(b + i)) > bl) return -EPROTO;
    if (!(*chain = malloc(bl))) return -ENOMEM;
    uint8_t *c = *chain;
    memcpy(c, b, bl);
    for (i += 3; i + 3 <= end && n < 10;) {
        size_t cl = get24(c + i);
        if (i + 3 + cl + 2 * v13 > end) return -EPROTO;
        certs[n] = c + i + 3; clens[n++] = cl;
        i += 3 + cl;
        if (v13) i += 2 + get16(c + i);         /* the entry's extensions */
    }
    if (!n) return -EPROTO;
    if (!(t->flags & TLS_INSECURE)) {
        net_info_t ni;
        const char *why;
        if (net_info(&ni) < 0) ni.time = 0;
        if (x509_verify_chain(certs, clens, n, host, ni.time, "/etc/ssl/roots", &why)) {
            printf("tls: %s: %s\n", host, why);
            return -EIO;
        }
    }
    return x509_parse(leaf, certs[0], clens[0]) ? -EPROTO : 0;
}

/* A signature with the certificate's key over content: CertificateVerify
 * (1.3) or ServerKeyExchange (1.2). 1.3 ties ECDSA's hash to its curve and
 * has no PKCS #1 v1.5. 0 if valid. */
static int sig_check(const x509_t *k, unsigned alg, const uint8_t *content, size_t n,
                     const uint8_t *sig, size_t sl, int v13)
{
    uint8_t dh[64];
    int ha = (int)(alg >> 8) - 3, lo = alg & 0xff;   /* 04xx, 05xx, 06xx: SHA-256, -384, -512 */
    if (lo == 3 && ha >= SHA256 && ha <= SHA512 && (k->keytype == X509_P256 || k->keytype == X509_P384)) {
        int curve = k->keytype == X509_P256 ? P256 : P384;
        if (v13 && ha != curve) return -1;      /* (P256 == SHA256, P384 == SHA384) */
        sha(ha, dh, content, n);
        return ecdsa_der_verify(curve, k->key, k->keylen, ha, dh, sig, sl);
    }
    if (k->keytype != X509_RSA) return -1;
    if (alg >= 0x0804 && alg <= 0x0806) {       /* RSA-PSS */
        ha = (int)alg - 0x0803;
        sha(ha, dh, content, n);
        return rsa_verify_pss(k->mod, k->modlen, k->exp, k->explen, ha, dh, sig, sl);
    }
    if (v13 || lo != 1 || ha < SHA256 || ha > SHA512) return -1;
    sha(ha, dh, content, n);
    return rsa_verify_pkcs1(k->mod, k->modlen, k->exp, k->explen, ha, dh, sig, sl);
}

/* TLS 1.2 after the ServerHello (RFC 5246 7.3):
 *   server: Certificate, ServerKeyExchange (its ECDHE key, signed with the
 *           certificate's key over both randoms), [CertificateRequest],
 *           ServerHelloDone
 *   us:     [empty Certificate], ClientKeyExchange (our ECDHE key),
 *           ChangeCipherSpec, Finished
 *   server: ChangeCipherSpec, Finished
 * master secret = PRF(shared secret, a hash of the transcript, or both
 * randoms); the record keys and the Finished messages come from it. */
static long tls12(struct tls *t, const char *host, const uint8_t rnd[64], int ems,
                  const uint8_t xpriv[32], const uint8_t xpub[32])
{
    int type, creq = 0, stage = 0, ha = t->cs->hash;
    uint8_t *b, *chain = 0, pms[48], ppriv[48], cke[5 + 97], content[64 + 4 + 255], h[48], master[48], kb[88];
    size_t bl, hl = sha_len(ha), publen = 0, zl = 32;
    x509_t leaf;
    long e;
    for (;;) {
        if ((e = hs_next(t, &type, &b, &bl))) goto out;
        if (stage == 0 && type == HS_CERT) {
            if ((e = certificates(t, host, b, bl, &chain, &leaf))) goto out;
            stage = 1;
        } else if (stage == 1 && type == HS_SKE) {
            /* named_curve (3), the group, its key; then the signature: algorithm, length, bytes */
            size_t pl = bl >= 4 ? 4 + (size_t)b[3] : 0;
            if (!pl || b[0] != 3 || pl + 4 > bl || pl + 4 + get16(b + pl + 2) != bl) { e = -EPROTO; goto out; }
            memcpy(content, rnd, 64);
            memcpy(content + 64, b, pl);
            if (sig_check(&leaf, get16(b + pl), content, 64 + pl, b + pl + 4, bl - pl - 4, 0) &&
                !(t->flags & TLS_INSECURE)) { printf("tls: %s: bad ServerKeyExchange\n", host); e = -EIO; goto out; }
            unsigned group = get16(b + 1);
            if (group == 0x001d && b[3] == 32 && !x25519(pms, xpriv, b + 4)) {
                memcpy(cke + 5, xpub, publen = 32);
            } else if (group == 0x0017 || group == 0x0018) {   /* P-256, P-384 */
                zl = group == 0x0017 ? 32 : 48;
                sys_random(ppriv, zl);
                if (ec_public((int)group - 0x16, cke + 5, ppriv) || ec_shared((int)group - 0x16, pms, ppriv, b + 4, b[3])) { e = -EPROTO; goto out; }
                publen = 2 * zl + 1;
            } else { e = -EPROTO; goto out; }
            stage = 2;
        } else if (stage == 2 && type == HS_CREQ) creq = 1;
        else if (stage == 2 && type == HS_SHD && !bl) break;
        else { e = -EPROTO; goto out; }
    }
    if (creq) {                                 /* we have no certificate to show */
        static const uint8_t ec[7] = { HS_CERT, 0, 0, 3, 0, 0, 0 };
        hs_send(t, ec, 7);
    }
    cke[0] = HS_CKE; put24(cke + 1, (unsigned)publen + 1); cke[4] = (uint8_t)publen;
    if ((e = hs_send(t, cke, 5 + publen)) < 0) goto out;
    if (ems) { th_now(t, h); prf(ha, master, 48, pms, zl, "extended master secret", h, hl, 0, 0); }
    else prf(ha, master, 48, pms, zl, "master secret", rnd, 64, 0, 0);
    /* the key block (randoms swapped): client key, server key, client iv, server iv */
    size_t kl = t->keylen, ivl = t->aead_alg == AEAD_CHACHA20_POLY1305 ? 12 : 4;
    t->ex = 12 - ivl;
    prf(ha, kb, 2 * kl + 2 * ivl, master, 48, "key expansion", rnd + 32, 32, rnd, 32);
    aead_init(&t->wr, t->aead_alg, kb);
    aead_init(&t->rd, t->aead_alg, kb + kl);
    memcpy(t->wr_iv, kb + 2 * kl, ivl);
    memcpy(t->rd_iv, kb + 2 * kl + ivl, ivl);
    uint8_t ccs = 1, fin[16];
    rec_send(t, CT_CCS, &ccs, 1);
    t->wr_on = t->rd_ready = 1;
    th_now(t, h);
    fin[0] = HS_FIN; put24(fin + 1, 12);
    prf(ha, fin + 4, 12, master, 48, "client finished", h, hl, 0, 0);
    if ((e = hs_send(t, fin, 16)) < 0) goto out;
    th_now(t, h);                               /* the server's Finished covers ours */
    prf(ha, fin + 4, 12, master, 48, "server finished", h, hl, 0, 0);
    if ((e = hs_next(t, &type, &b, &bl))) goto out;
    if (type != HS_FIN || !t->rd_on || bl != 12 || !ct_equal(fin + 4, b, 12)) e = -EPROTO;
out:
    free(chain);
    wipe(pms, 48); wipe(ppriv, 48); wipe(master, 48); wipe(kb, sizeof kb);
    return e < 0 ? e : 0;
}

tls_t *tls_connect(long sock, const char *host, int flags, long *err)
{
    struct tls *t = malloc(sizeof *t);
    if (!t) { *err = -ENOMEM; net_close(sock); return 0; }
    memset(t, 0, sizeof *t);
    t->sock = sock; t->flags = flags; t->timeout = 20000;
    sha_init(&t->th, SHA256);
    sha_init(&t->th384, SHA384);
    t->both = 1;
    uint8_t xpriv[32], xpub[32], ppriv[48], ppub[97], sid[32], shared[48], rnd[64];
    size_t zl = 32;                             /* the shared secret's length */
    static uint8_t msg[2048];
    sys_random(xpriv, 32); sys_random(sid, 32);
    x25519_base(xpub, xpriv);
    size_t n = client_hello(msg, host, 0x001d, xpub, 32, 0, 0, sid);
    memcpy(rnd, msg + 6, 32);                   /* our random (for 1.2) */
    long e = hs_send(t, msg, n);
    int type, group = 0x001d, hrr = 0;
    uint8_t *b; size_t bl;
    for (;;) {                                  /* ServerHello (or HelloRetryRequest) */
        if (e < 0 || (e = hs_next(t, &type, &b, &bl))) goto out;
        t->both = 0;
        if (type != HS_SH || bl < 38) { e = -EPROTO; goto out; }
        size_t sidl = b[34], off = 35 + sidl;
        if (off + 3 > bl || b[off + 2]) { e = -EPROTO; goto out; }
        unsigned id = get16(b + off);
        t->cs = 0;
        for (size_t i = 0; i < NSUITES; i++) if (SUITES[i].id == id) t->cs = &SUITES[i];
        int is_hrr = !memcmp(b + 2, HRR_RANDOM, 32);
        size_t ext = off + 3, extend = ext + 2 <= bl ? ext + 2 + get16(b + ext) : ext;
        if (extend > bl) { e = -EPROTO; goto out; }
        int version = 0, sgroup = 0, ems = 0;
        const uint8_t *skey = 0, *cookie = 0; size_t skeyl = 0, cookiel = 0;
        for (size_t i = ext + 2; i + 4 <= extend;) {
            unsigned et = get16(b + i), el = get16(b + i + 2);
            const uint8_t *v = b + i + 4;
            if (i + 4 + el > extend) { e = -EPROTO; goto out; }
            if (et == 0x002b && el == 2) version = (int)get16(v);
            if (et == 0x0033 && el >= 2) { sgroup = (int)get16(v); if (el >= 4) { skeyl = get16(v + 2); skey = v + 4; if (4 + skeyl > el) { e = -EPROTO; goto out; } } }
            if (et == 0x002c && el >= 2) { cookiel = el; cookie = v; }
            if (et == 0x0017) ems = 1;
            if (et == 0xff01 && (el != 1 || v[0])) { e = -EPROTO; goto out; }   /* renegotiation_info */
            i += 4 + el;
        }
        if (!version && get16(b) == 0x0303 && !hrr && !is_hrr) {   /* TLS 1.2 */
            /* a 1.3 server pushed down to 1.2 (by someone in the middle) says so */
            if (!memcmp(b + 26, "DOWNGRD", 7)) { printf("tls: %s: downgrade to TLS 1.2 refused\n", host); e = -EIO; goto out; }
            if (!t->cs || t->cs->id >> 8 == 0x13) { e = -EPROTO; goto out; }
            if (t->cs->hash == SHA384) t->th = t->th384;
            memcpy(rnd + 32, b + 2, 32);
            t->v12 = 1;
            t->aead_alg = t->cs->aead;
            t->keylen = t->aead_alg == AEAD_AES128_GCM ? 16 : 32;
            if ((e = tls12(t, host, rnd, ems, xpriv, xpub))) goto out;
            goto done;
        }
        if (version != 0x0304 || !t->cs || t->cs->id >> 8 != 0x13) { e = -EPROTO; goto out; }
        if (!is_hrr) {
            if (sgroup != group || !skey) { e = -EPROTO; goto out; }
            if (group == 0x001d) { if (skeyl != 32 || x25519(shared, xpriv, skey)) { e = -EPROTO; goto out; } }
            else if (ec_shared(group - 0x16, shared, ppriv, skey, skeyl)) { e = -EPROTO; goto out; }
            break;
        }
        /* HelloRetryRequest: the transcript restarts from a hash of the first
         * ClientHello; then we try again with the group it asked for. */
        if (hrr++ || (sgroup != 0x0017 && sgroup != 0x0018)) { e = -EPROTO; goto out; }
        uint8_t ch1[32], mh[4 + 32];
        sha_t c1; sha_init(&c1, SHA256);
        sha_update(&c1, msg, n);
        sha_final(&c1, ch1);
        mh[0] = 254; put24(mh + 1, 32); memcpy(mh + 4, ch1, 32);
        sha_init(&t->th, SHA256);
        sha_update(&t->th, mh, sizeof mh);
        sha_update(&t->th, b - 4, bl + 4);      /* the HRR message itself */
        group = sgroup;                         /* P-256 or P-384 */
        zl = group == 0x0017 ? 32 : 48;
        sys_random(ppriv, zl);
        if (ec_public(group - 0x16, ppub, ppriv)) { e = -EPROTO; goto out; }
        uint8_t ck[256];
        if (cookiel > sizeof ck) { e = -EPROTO; goto out; }
        if (cookiel) memcpy(ck, cookie, cookiel);
        n = client_hello(msg, host, group, ppub, 2 * zl + 1, ck, cookiel, sid);
        e = hs_send(t, msg, n);
    }
    t->aead_alg = t->cs->aead;
    t->keylen = t->aead_alg == AEAD_AES128_GCM ? 16 : 32;
    /* the key schedule, to the handshake secrets */
    uint8_t zero[32] = { 0 }, early[32], derived[32], hsec[32], cs[32], ss[32], h[32], empty[32];
    sha(SHA256, empty, "", 0);
    hkdf_extract(SHA256, early, 0, 0, zero, 32);
    derive(derived, early, "derived", empty);
    hkdf_extract(SHA256, hsec, derived, 32, shared, zl);
    th_now(t, h);
    derive(cs, hsec, "c hs traffic", h);
    derive(ss, hsec, "s hs traffic", h);
    set_keys(t, 0, ss);
    /* EncryptedExtensions, [CertificateRequest], Certificate, CertificateVerify, Finished */
    int creq = 0;
    uint8_t *chain = 0;
    x509_t leaf;
    for (int stage = 0; stage < 4;) {
        if ((e = hs_next(t, &type, &b, &bl))) goto out2;
        if (stage == 0 && type == HS_EE) { stage = 1; continue; }
        if (stage == 1 && type == HS_CREQ) { creq = 1; continue; }
        if (stage == 1 && type == HS_CERT) {
            if ((e = certificates(t, host, b, bl, &chain, &leaf))) goto out2;
            th_now(t, h);                       /* for CertificateVerify: up to Certificate */
            stage = 2;
            continue;
        }
        if (stage == 2 && type == HS_CV) {
            if (bl < 4 || 4 + get16(b + 2) > bl) { e = -EPROTO; goto out2; }
            uint8_t content[64 + 34 + 32];
            memset(content, 0x20, 64);
            memcpy(content + 64, "TLS 1.3, server CertificateVerify", 34);   /* includes the 0 */
            memcpy(content + 98, h, 32);
            if (sig_check(&leaf, get16(b), content, sizeof content, b + 4, get16(b + 2), 1) &&
                !(t->flags & TLS_INSECURE)) { printf("tls: %s: bad CertificateVerify\n", host); e = -EIO; goto out2; }
            th_now(t, h);                       /* for Finished: up to CertificateVerify */
            stage = 3;
            continue;
        }
        if (stage == 3 && type == HS_FIN) {
            uint8_t fk[32], want[32];
            expand_label(fk, 32, ss, "finished", 0, 0);
            hmac(SHA256, want, fk, 32, h, 32);   /* h: the transcript before Finished */
            if (bl != 32 || !ct_equal(want, b, 32)) { e = -EPROTO; goto out2; }
            stage = 4;
            continue;
        }
        e = -EPROTO; goto out2;
    }
    free(chain); chain = 0;
    uint8_t sfin_h[32], master[32], cap[32], sap[32];
    th_now(t, sfin_h);                          /* up to the server's Finished */
    derive(derived, hsec, "derived", empty);
    hkdf_extract(SHA256, master, derived, 32, zero, 32);
    derive(cap, master, "c ap traffic", sfin_h);
    derive(sap, master, "s ap traffic", sfin_h);
    /* our side: [CCS], [empty Certificate], Finished */
    uint8_t ccs = 1;
    rec_send(t, CT_CCS, &ccs, 1);
    set_keys(t, 1, cs);
    if (creq) {
        uint8_t ec[8] = { HS_CERT, 0, 0, 4, 0, 0, 0, 0 };
        hs_send(t, ec, 8);
    }
    uint8_t fk[32], fin[36];
    th_now(t, h);
    expand_label(fk, 32, cs, "finished", 0, 0);
    fin[0] = HS_FIN; put24(fin + 1, 32);
    hmac(SHA256, fin + 4, fk, 32, h, 32);
    if ((e = hs_send(t, fin, 36)) < 0) goto out;
    set_keys(t, 0, sap);
    set_keys(t, 1, cap);
    wipe(shared, 48); wipe(hsec, 32); wipe(master, 32);
done:
    wipe(xpriv, 32); wipe(ppriv, 48);
    free(t->hs); t->hs = 0; t->hslen = t->hscap = 0;
    *err = 0;
    return t;
out2:
    free(chain);
out:
    wipe(xpriv, 32); wipe(ppriv, 48);
    *err = e < 0 ? e : -EPROTO;
    fail(t, 0);
    return 0;
}

long tls_write(tls_t *t, const void *buf, size_t n)
{
    return rec_send(t, CT_APP, buf, n);
}

/* Messages after the handshake: session tickets (ignored: no resumption),
 * key updates (new keys, and ours too if asked); 1.2's HelloRequest, a
 * renegotiation we decline (a no_renegotiation warning). */
static long post_handshake(struct tls *t)
{
    const uint8_t *p = t->pt;
    size_t n = t->ptlen;
    while (n >= 4) {
        size_t l = get24(p + 1);
        if (4 + l > n) return -EPROTO;
        if (t->v12) {
            if (p[0] == HS_HELLO_REQ) rec_send(t, CT_ALERT, (const uint8_t[2]){ 1, 100 }, 2);
        } else if (p[0] == HS_KU && l == 1) {
            uint8_t ns[32];
            expand_label(ns, 32, t->rd_secret, "traffic upd", 0, 0);
            set_keys(t, 0, ns);
            if (p[4] == 1) {                    /* update_requested */
                uint8_t ku[5] = { HS_KU, 0, 0, 1, 0 };
                rec_send(t, CT_HS, ku, 5);
                expand_label(ns, 32, t->wr_secret, "traffic upd", 0, 0);
                set_keys(t, 1, ns);
            }
        }
        p += 4 + l; n -= 4 + l;
    }
    return 0;
}

long tls_read(tls_t *t, void *buf, size_t n, unsigned timeout_ms)
{
    t->timeout = timeout_ms;
    while (t->ptpos >= t->ptlen || t->pttype != CT_APP) {
        if (t->eof) return 0;
        long e = rec_read(t);
        if (e) return e;
        if (t->pttype == CT_HS) { if ((e = post_handshake(t))) return e; t->ptlen = 0; }
    }
    size_t c = t->ptlen - t->ptpos < n ? t->ptlen - t->ptpos : n;
    memcpy(buf, t->pt + t->ptpos, c);
    t->ptpos += c;
    return (long)c;
}

void tls_close(tls_t *t)
{
    if (!t) return;
    uint8_t alert[2] = { 1, 0 };                /* close_notify */
    rec_send(t, CT_ALERT, alert, 2);
    net_close(t->sock);
    free(t->hs);
    wipe(t, sizeof *t);
    free(t);
}

const char *tls_cipher(tls_t *t) { return t->cs->name; }
