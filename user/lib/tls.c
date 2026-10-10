/*
 * tls.c - A TLS 1.3 client (RFC 8446): what makes https:// private and
 * authentic.
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
 * Key exchange: X25519, or P-256 if the server asks for it (HelloRetryRequest).
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "net.h"
#include "mk/crypto.h"

#define REC_MAX 16384
enum { CT_CCS = 20, CT_ALERT = 21, CT_HS = 22, CT_APP = 23 };
enum { HS_CH = 1, HS_SH = 2, HS_TICKET = 4, HS_EE = 8, HS_CERT = 11, HS_CREQ = 13, HS_CV = 15, HS_FIN = 20, HS_KU = 24 };

struct tls {
    long sock;
    int flags, eof, suite, aead_alg;
    size_t keylen;
    sha_t th;                                  /* the transcript hash (SHA-256) */
    aead_t rd, wr;
    uint8_t rd_iv[12], wr_iv[12], rd_secret[32], wr_secret[32];
    uint64_t rd_seq, wr_seq;
    int rd_on, wr_on;                          /* records protected yet? */
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

static void th_now(struct tls *t, uint8_t h[32])   /* the transcript hash so far */
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

static void nonce(uint8_t n[12], const uint8_t iv[12], uint64_t seq)
{
    memcpy(n, iv, 12);
    for (int i = 0; i < 8; i++) n[11 - i] ^= (uint8_t)(seq >> (8 * i));
}

/* ---- Records out. */
static long rec_send(struct tls *t, int type, const uint8_t *data, size_t n)
{
    static uint8_t rec[REC_MAX + 256 + 5];
    for (size_t off = 0; off < n || (n == 0 && off == 0); ) {
        size_t c = n - off < REC_MAX ? n - off : REC_MAX;
        size_t len;
        if (t->wr_on && type != CT_CCS) {      /* inner plaintext: data, real type; then encrypt */
            static uint8_t inner[REC_MAX + 1];
            memcpy(inner, data + off, c);
            inner[c] = (uint8_t)type;
            len = c + 1 + 16;
            rec[0] = CT_APP; rec[1] = 3; rec[2] = 3; put16(rec + 3, (unsigned)len);
            uint8_t no[12];
            nonce(no, t->wr_iv, t->wr_seq++);
            aead_seal(&t->wr, no, rec, 5, inner, c + 1, rec + 5);
        } else {
            len = c;
            rec[0] = (uint8_t)type; rec[1] = 3; rec[2] = type == CT_HS && !t->wr_on ? 1 : 3; put16(rec + 3, (unsigned)len);
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
    if (type == CT_CCS) {                      /* compatibility noise: ignore */
        t->ptlen = 0; t->pttype = CT_CCS;
    } else if (t->rd_on && type == CT_APP) {
        uint8_t no[12];
        nonce(no, t->rd_iv, t->rd_seq++);
        if (len < 17 || aead_open(&t->rd, no, t->in, 5, body, len, t->pt)) return -EPROTO;
        size_t n = len - 16;
        while (n && !t->pt[n - 1]) n--;        /* padding, then the real type */
        if (!n) return -EPROTO;
        t->pttype = t->pt[--n];
        t->ptlen = n;
    } else if (!t->rd_on && (type == CT_HS || type == CT_ALERT)) {
        memcpy(t->pt, body, len);
        t->ptlen = len; t->pttype = type;
    } else return -EPROTO;
    t->ptpos = 0;
    memmove(t->in, t->in + 5 + len, t->inlen - 5 - len);
    t->inlen -= 5 + len;
    if (t->pttype == CT_ALERT) {
        if (t->ptlen >= 2 && t->pt[1] == 0) { t->eof = 1; return 0; }   /* close_notify */
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
    put16(p, 4); put16(p + 2, 0x1303); put16(p + 4, 0x1301); p += 6;   /* ChaCha20, then AES-128-GCM */
    *p++ = 1; *p++ = 0;                                                 /* no compression */
    uint8_t *ext = p; p += 2;
    int ip = 1;
    for (const char *s = host; *s; s++) if ((*s < '0' || *s > '9') && *s != '.') ip = 0;
    if (!ip) {                                                          /* server_name */
        size_t hl = strlen(host);
        put16(p, 0); put16(p + 2, (unsigned)hl + 5); put16(p + 4, (unsigned)hl + 3); p[6] = 0;
        put16(p + 7, (unsigned)hl); memcpy(p + 9, host, hl); p += 9 + hl;
    }
    put16(p, 0x000a); put16(p + 2, 6); put16(p + 4, 4); put16(p + 6, 0x001d); put16(p + 8, 0x0017); p += 10;
    static const uint16_t sigs[] = { 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601 };
    put16(p, 0x000d); put16(p + 2, 2 + 2 * 8); put16(p + 4, 2 * 8); p += 6;
    for (int i = 0; i < 8; i++) { put16(p, sigs[i]); p += 2; }
    put16(p, 0x0033); put16(p + 2, (unsigned)publen + 6); put16(p + 4, (unsigned)publen + 4);   /* key_share */
    put16(p + 6, (unsigned)group); put16(p + 8, (unsigned)publen); memcpy(p + 10, pub, publen); p += 10 + publen;
    put16(p, 0x002b); put16(p + 2, 3); p[4] = 2; put16(p + 5, 0x0304); p += 7;   /* TLS 1.3 only */
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

tls_t *tls_connect(long sock, const char *host, int flags, long *err)
{
    struct tls *t = malloc(sizeof *t);
    if (!t) { *err = -ENOMEM; net_close(sock); return 0; }
    memset(t, 0, sizeof *t);
    t->sock = sock; t->flags = flags; t->timeout = 20000;
    sha_init(&t->th, SHA256);
    uint8_t xpriv[32], xpub[32], ppriv[32], ppub[65], sid[32], shared[32];
    static uint8_t msg[2048];
    sys_random(xpriv, 32); sys_random(sid, 32);
    x25519_base(xpub, xpriv);
    size_t n = client_hello(msg, host, 0x001d, xpub, 32, 0, 0, sid);
    long e = hs_send(t, msg, n);
    int type, group = 0x001d, hrr = 0;
    uint8_t *b; size_t bl;
    for (;;) {                                  /* ServerHello (or HelloRetryRequest) */
        if (e < 0 || (e = hs_next(t, &type, &b, &bl))) goto out;
        if (type != HS_SH || bl < 38) { e = -EPROTO; goto out; }
        size_t sidl = b[34], off = 35 + sidl;
        if (off + 3 > bl) { e = -EPROTO; goto out; }
        t->suite = (int)get16(b + off);
        int is_hrr = !memcmp(b + 2, HRR_RANDOM, 32);
        size_t ext = off + 3, extend = ext + 2 + (ext + 2 <= bl ? get16(b + ext) : 0);
        if (extend > bl) { e = -EPROTO; goto out; }
        int version = 0, sgroup = 0;
        const uint8_t *skey = 0, *cookie = 0; size_t skeyl = 0, cookiel = 0;
        for (size_t i = ext + 2; i + 4 <= extend;) {
            unsigned et = get16(b + i), el = get16(b + i + 2);
            const uint8_t *v = b + i + 4;
            if (i + 4 + el > extend) { e = -EPROTO; goto out; }
            if (et == 0x002b && el == 2) version = (int)get16(v);
            if (et == 0x0033 && el >= 2) { sgroup = (int)get16(v); if (el >= 4) { skeyl = get16(v + 2); skey = v + 4; if (4 + skeyl > el) { e = -EPROTO; goto out; } } }
            if (et == 0x002c && el >= 2) { cookiel = el; cookie = v; }
            i += 4 + el;
        }
        if (version != 0x0304 || (t->suite != 0x1301 && t->suite != 0x1303)) { e = -EPROTO; goto out; }
        if (!is_hrr) {
            if (sgroup != group || !skey) { e = -EPROTO; goto out; }
            if (group == 0x001d) { if (skeyl != 32 || x25519(shared, xpriv, skey)) { e = -EPROTO; goto out; } }
            else if (p256_shared(shared, ppriv, skey, skeyl)) { e = -EPROTO; goto out; }
            break;
        }
        /* HelloRetryRequest: the transcript restarts from a hash of the first
         * ClientHello; then we try again with the group it asked for. */
        if (hrr++ || sgroup != 0x0017) { e = -EPROTO; goto out; }
        uint8_t ch1[32], mh[4 + 32];
        sha_t c1; sha_init(&c1, SHA256);
        sha_update(&c1, msg, n);
        sha_final(&c1, ch1);
        mh[0] = 254; put24(mh + 1, 32); memcpy(mh + 4, ch1, 32);
        sha_init(&t->th, SHA256);
        sha_update(&t->th, mh, sizeof mh);
        sha_update(&t->th, b - 4, bl + 4);      /* the HRR message itself */
        group = 0x0017;
        sys_random(ppriv, 32);
        if (p256_public(ppub, ppriv)) { e = -EPROTO; goto out; }
        uint8_t ck[256];
        if (cookiel > sizeof ck) { e = -EPROTO; goto out; }
        if (cookiel) memcpy(ck, cookie, cookiel);
        n = client_hello(msg, host, 0x0017, ppub, 65, ck, cookiel, sid);
        e = hs_send(t, msg, n);
    }
    t->aead_alg = t->suite == 0x1303 ? AEAD_CHACHA20_POLY1305 : AEAD_AES128_GCM;
    t->keylen = t->suite == 0x1303 ? 32 : 16;
    /* the key schedule, to the handshake secrets */
    uint8_t zero[32] = { 0 }, early[32], derived[32], hsec[32], cs[32], ss[32], h[32], empty[32];
    sha(SHA256, empty, "", 0);
    hkdf_extract(SHA256, early, 0, 0, zero, 32);
    derive(derived, early, "derived", empty);
    hkdf_extract(SHA256, hsec, derived, 32, shared, 32);
    th_now(t, h);
    derive(cs, hsec, "c hs traffic", h);
    derive(ss, hsec, "s hs traffic", h);
    set_keys(t, 0, ss);
    /* EncryptedExtensions, [CertificateRequest], Certificate, CertificateVerify, Finished */
    const uint8_t *certs[10]; size_t clens[10]; int ncerts = 0, creq = 0;
    uint8_t *chain = 0;
    for (int stage = 0; stage < 4;) {
        if ((e = hs_next(t, &type, &b, &bl))) goto out;
        if (stage == 0 && type == HS_EE) { stage = 1; continue; }
        if (stage == 1 && type == HS_CREQ) { creq = 1; continue; }
        if (stage == 1 && type == HS_CERT) {
            if (bl < 4 || b[0] + 4u > bl) { e = -EPROTO; goto out; }
            size_t i = 1 + b[0], end = i + 3 + get24(b + i);
            if (end > bl) { e = -EPROTO; goto out; }
            chain = malloc(bl);                 /* keep a copy: t->hs moves on */
            if (!chain) { e = -ENOMEM; goto out; }
            memcpy(chain, b, bl);
            for (i += 3; i + 3 <= end && ncerts < 10;) {
                size_t cl = get24(chain + i);
                if (i + 3 + cl + 2 > end) { e = -EPROTO; goto out2; }
                certs[ncerts] = chain + i + 3; clens[ncerts++] = cl;
                i += 3 + cl;
                i += 2 + get16(chain + i);      /* the entry's extensions */
            }
            if (!ncerts) { e = -EPROTO; goto out2; }
            if (!(t->flags & TLS_INSECURE)) {
                net_info_t ni;
                const char *why;
                if (net_info(&ni) < 0) ni.time = 0;
                if (x509_verify_chain(certs, clens, ncerts, host, ni.time, "/etc/ssl/roots", &why)) {
                    printf("tls: %s: %s\n", host, why);
                    e = -EIO; goto out2;
                }
            }
            th_now(t, h);                       /* for CertificateVerify: up to Certificate */
            stage = 2;
            continue;
        }
        if (stage == 2 && type == HS_CV) {
            if (bl < 4 || 4 + get16(b + 2) > bl) { e = -EPROTO; goto out2; }
            unsigned alg = get16(b), sl = get16(b + 2);
            uint8_t content[64 + 34 + 32], dh[64];
            memset(content, 0x20, 64);
            memcpy(content + 64, "TLS 1.3, server CertificateVerify", 34);   /* includes the 0 */
            memcpy(content + 98, h, 32);
            x509_t leaf;
            if (x509_parse(&leaf, certs[0], clens[0])) { e = -EPROTO; goto out2; }
            int bad = 1;
            if (alg == 0x0403 && leaf.keytype == X509_P256) {
                sha(SHA256, dh, content, sizeof content);
                bad = ecdsa_der_verify(P256, leaf.key, leaf.keylen, SHA256, dh, b + 4, sl);
            } else if (alg == 0x0503 && leaf.keytype == X509_P384) {
                sha(SHA384, dh, content, sizeof content);
                bad = ecdsa_der_verify(P384, leaf.key, leaf.keylen, SHA384, dh, b + 4, sl);
            } else if (alg >= 0x0804 && alg <= 0x0806 && leaf.keytype == X509_RSA) {
                int ha = alg == 0x0804 ? SHA256 : alg == 0x0805 ? SHA384 : SHA512;
                sha(ha, dh, content, sizeof content);
                bad = rsa_verify_pss(leaf.mod, leaf.modlen, leaf.exp, leaf.explen, ha, dh, b + 4, sl);
            }
            if (bad && !(t->flags & TLS_INSECURE)) { printf("tls: %s: bad CertificateVerify\n", host); e = -EIO; goto out2; }
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
    wipe(xpriv, 32); wipe(ppriv, 32); wipe(shared, 32); wipe(hsec, 32); wipe(master, 32);
    free(t->hs); t->hs = 0; t->hslen = t->hscap = 0;
    *err = 0;
    return t;
out2:
    free(chain);
out:
    *err = e < 0 ? e : -EPROTO;
    fail(t, 0);
    return 0;
}

long tls_write(tls_t *t, const void *buf, size_t n)
{
    return rec_send(t, CT_APP, buf, n);
}

/* Messages after the handshake: session tickets (ignored: no resumption)
 * and key updates (new keys, and ours too if asked). */
static long post_handshake(struct tls *t)
{
    const uint8_t *p = t->pt;
    size_t n = t->ptlen;
    while (n >= 4) {
        size_t l = get24(p + 1);
        if (4 + l > n) return -EPROTO;
        if (p[0] == HS_KU && l == 1) {
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

const char *tls_cipher(tls_t *t) { return t->suite == 0x1303 ? "TLS_CHACHA20_POLY1305_SHA256" : "TLS_AES_128_GCM_SHA256"; }
