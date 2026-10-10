/*
 * x509.c - Certificates (RFC 5280), enough to know who a TLS server is:
 * read a certificate (DER, the binary form of ASN.1), check its signature
 * with its issuer's key, check it names the host we wanted, and follow the
 * chain the server sent up to one of the trusted roots (/etc/ssl/roots, the
 * root certificates that browsers trust, one DER after another).
 *
 * DER is a tree of tag-length-value items. A certificate is
 *   SEQUENCE { tbsCertificate, signatureAlgorithm, signatureValue }
 * where tbsCertificate ("to be signed") holds the issuer's and subject's
 * names, the validity dates, the subject's public key and extensions.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include "net.h"
#include "mk/crypto.h"

typedef struct { const uint8_t *p, *end; } der_t;

/* The next item: its tag, value and (with hdr) where the whole item starts. */
static int der_next(der_t *d, int *tag, const uint8_t **val, size_t *len, const uint8_t **item)
{
    const uint8_t *p = d->p;
    if (d->end - p < 2) return -1;
    if (item) *item = p;
    *tag = *p++;
    size_t l = *p++;
    if (l & 0x80) {
        int n = l & 0x7f;
        if (n < 1 || n > 3 || d->end - p < n) return -1;
        for (l = 0; n--; ) l = l << 8 | *p++;
    }
    if ((size_t)(d->end - p) < l) return -1;
    *val = p; *len = l;
    d->p = p + l;
    return 0;
}

static int der_expect(der_t *d, int want, der_t *inner)
{
    int tag; const uint8_t *v; size_t l;
    if (der_next(d, &tag, &v, &l, 0) || tag != want) return -1;
    inner->p = v; inner->end = v + l;
    return 0;
}

static int oid_is(const uint8_t *v, size_t l, const char *hex)   /* compare an OID with hex bytes */
{
    size_t n = strlen(hex) / 2;
    if (l != n) return 0;
    for (size_t i = 0; i < n; i++) {
        int x = 0;
        for (int j = 0; j < 2; j++) { char c = hex[2 * i + j]; x = x * 16 + (c <= '9' ? c - '0' : c - 'a' + 10); }
        if (v[i] != x) return 0;
    }
    return 1;
}

/* "YYMMDDHHMMSSZ" (UTCTime) or "YYYYMMDDHHMMSSZ" (GeneralizedTime) -> seconds since 1970. */
static int64_t der_time(int tag, const uint8_t *v, size_t l)
{
    int f[6], k = 0, i = 0;
    int64_t y;
    if (tag == 0x17 && l >= 13) { y = (v[0] - '0') * 10 + v[1] - '0'; y += y < 50 ? 2000 : 1900; i = 2; }
    else if (tag == 0x18 && l >= 15) { y = 0; for (; i < 4; i++) y = y * 10 + v[i] - '0'; }
    else return -1;
    for (; k < 5; k++, i += 2) f[k] = (v[i] - '0') * 10 + v[i + 1] - '0';
    int64_t m = f[0], d = f[1];
    y -= m <= 2;
    int64_t era = y / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + f[2] * 3600 + f[3] * 60 + f[4];
}

static int sig_alg(der_t *d)
{
    der_t a;
    int tag; const uint8_t *v; size_t l;
    if (der_expect(d, 0x30, &a) || der_next(&a, &tag, &v, &l, 0) || tag != 6) return 0;
    if (oid_is(v, l, "2a864886f70d01010b")) return X509_RSA_SHA256;
    if (oid_is(v, l, "2a864886f70d01010c")) return X509_RSA_SHA384;
    if (oid_is(v, l, "2a864886f70d01010d")) return X509_RSA_SHA512;
    if (oid_is(v, l, "2a8648ce3d040302")) return X509_ECDSA_SHA256;
    if (oid_is(v, l, "2a8648ce3d040303")) return X509_ECDSA_SHA384;
    if (oid_is(v, l, "2a8648ce3d040304")) return X509_ECDSA_SHA512;
    return -1;                                   /* known structure, unknown algorithm */
}

int x509_parse(x509_t *c, const uint8_t *der, size_t len)
{
    memset(c, 0, sizeof *c);
    c->pathlen = -1;
    der_t d = { der, der + len }, cert, tbs;
    int tag; const uint8_t *v, *item; size_t l;
    if (der_expect(&d, 0x30, &cert)) return -1;
    if (der_next(&cert, &tag, &v, &l, &item) || tag != 0x30) return -1;
    c->tbs = item; c->tbslen = (size_t)(v + l - item);
    tbs.p = v; tbs.end = v + l;
    if ((c->sigalg = sig_alg(&cert)) <= 0) return -1;
    if (der_next(&cert, &tag, &v, &l, 0) || tag != 3 || l < 1 || v[0]) return -1;
    c->sig = v + 1; c->siglen = l - 1;
    /* tbsCertificate */
    if (*tbs.p == 0xa0 && der_next(&tbs, &tag, &v, &l, 0)) return -1;   /* version */
    if (der_next(&tbs, &tag, &v, &l, 0) || tag != 2) return -1;          /* serial number */
    if (sig_alg(&tbs) == 0) return -1;
    if (der_next(&tbs, &tag, &v, &l, &item) || tag != 0x30) return -1;
    c->issuer = item; c->ilen = (size_t)(v + l - item);
    der_t val;
    if (der_expect(&tbs, 0x30, &val)) return -1;
    if (der_next(&val, &tag, &v, &l, 0) || (c->not_before = der_time(tag, v, l)) < 0) return -1;
    if (der_next(&val, &tag, &v, &l, 0) || (c->not_after = der_time(tag, v, l)) < 0) return -1;
    if (der_next(&tbs, &tag, &v, &l, &item) || tag != 0x30) return -1;
    c->subject = item; c->slen = (size_t)(v + l - item);
    der_t spki, alg, rsa;
    if (der_expect(&tbs, 0x30, &spki) || der_expect(&spki, 0x30, &alg)) return -1;
    if (der_next(&alg, &tag, &v, &l, 0) || tag != 6) return -1;
    if (oid_is(v, l, "2a864886f70d010101")) c->keytype = X509_RSA;
    else if (oid_is(v, l, "2a8648ce3d0201")) {
        if (der_next(&alg, &tag, &v, &l, 0) || tag != 6) return -1;
        c->keytype = oid_is(v, l, "2a8648ce3d030107") ? X509_P256 : oid_is(v, l, "2b81040022") ? X509_P384 : 0;
    }
    if (der_next(&spki, &tag, &v, &l, 0) || tag != 3 || l < 1 || v[0]) return -1;
    c->key = v + 1; c->keylen = l - 1;
    if (c->keytype == X509_RSA) {
        der_t k = { c->key, c->key + c->keylen };
        if (der_expect(&k, 0x30, &rsa)) return -1;
        if (der_next(&rsa, &tag, &c->mod, &c->modlen, 0) || tag != 2) return -1;
        if (der_next(&rsa, &tag, &c->exp, &c->explen, 0) || tag != 2) return -1;
    }
    /* extensions: [3] { SEQUENCE OF { OID, [critical], OCTET STRING } } */
    while (tbs.p < tbs.end) {
        if (der_next(&tbs, &tag, &v, &l, 0)) return -1;
        if (tag != 0xa3) continue;
        der_t ex = { v, v + l }, list, e;
        if (der_expect(&ex, 0x30, &list)) return -1;
        while (list.p < list.end) {
            if (der_expect(&list, 0x30, &e)) return -1;
            const uint8_t *oid; size_t ol;
            if (der_next(&e, &tag, &oid, &ol, 0) || tag != 6) return -1;
            if (der_next(&e, &tag, &v, &l, 0)) return -1;
            if (tag == 1 && der_next(&e, &tag, &v, &l, 0)) return -1;   /* "critical" */
            if (tag != 4) return -1;
            der_t inner = { v, v + l }, seq;
            if (oid_is(oid, ol, "551d13")) {                            /* basicConstraints */
                if (der_expect(&inner, 0x30, &seq)) return -1;
                while (seq.p < seq.end) {
                    if (der_next(&seq, &tag, &v, &l, 0)) return -1;
                    if (tag == 1 && l == 1) c->ca = v[0] != 0;
                    if (tag == 2 && l == 1) c->pathlen = v[0];
                }
            } else if (oid_is(oid, ol, "551d11")) {                     /* subjectAltName */
                if (der_expect(&inner, 0x30, &seq)) return -1;
                c->san = seq.p; c->sanlen = (size_t)(seq.end - seq.p);
            }
        }
    }
    return 0;
}

int ecdsa_der_verify(int curve, const uint8_t *pub, size_t publen, int alg, const uint8_t *hash,
                     const uint8_t *der, size_t dlen)
{
    der_t d = { der, der + dlen }, s;
    int tag; const uint8_t *r, *ss; size_t rl, sl;
    if (der_expect(&d, 0x30, &s) || der_next(&s, &tag, &r, &rl, 0) || tag != 2 ||
        der_next(&s, &tag, &ss, &sl, 0) || tag != 2) return -1;
    return ecdsa_verify(curve, pub, publen, hash, sha_len(alg), r, rl, ss, sl);
}

int x509_check_sig(const x509_t *c, const x509_t *is)
{
    static const int hash[] = { 0, SHA256, SHA384, SHA512, SHA256, SHA384, SHA512 };
    if (c->sigalg <= 0 || c->sigalg > X509_ECDSA_SHA512) return -1;
    int alg = hash[c->sigalg];
    uint8_t h[64];
    sha(alg, h, c->tbs, c->tbslen);
    if (c->sigalg <= X509_RSA_SHA512)
        return is->keytype == X509_RSA ? rsa_verify_pkcs1(is->mod, is->modlen, is->exp, is->explen, alg, h, c->sig, c->siglen) : -1;
    if (is->keytype != X509_P256 && is->keytype != X509_P384) return -1;
    return ecdsa_der_verify(is->keytype == X509_P256 ? P256 : P384, is->key, is->keylen, alg, h, c->sig, c->siglen);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* One dNSName against the host: exact (any case), or "*." + the rest for
 * exactly one more label on the left ("*.example.com" matches
 * "www.example.com", not "example.com" nor "a.b.example.com"). */
static int name_match(const uint8_t *n, size_t l, const char *host)
{
    size_t hl = strlen(host);
    if (l >= 3 && n[0] == '*' && n[1] == '.') {
        const char *dot = strchr(host, '.');
        if (!dot || dot == host) return 0;
        host = dot; hl = strlen(host);
        n++; l--;
    }
    if (l != hl) return 0;
    for (size_t i = 0; i < l; i++) if (lower(n[i]) != lower(host[i])) return 0;
    return 1;
}

int x509_host(const x509_t *c, const char *host)
{
    uint8_t ip[4];
    int isip = 1;
    { uint32_t v = 0; const char *s = host; int k;
      for (k = 0; k < 4; k++) { int x = 0, dd = 0; while (*s >= '0' && *s <= '9') { x = x * 10 + *s++ - '0'; dd++; }
        if (!dd || x > 255 || (k < 3 && *s++ != '.')) { isip = 0; break; } v = v << 8 | (uint32_t)x; }
      if (*s) isip = 0;
      for (k = 0; k < 4; k++) ip[k] = (uint8_t)(v >> (24 - 8 * k)); }
    der_t d = { c->san, c->san + c->sanlen };
    int tag; const uint8_t *v; size_t l;
    while (d.p < d.end && !der_next(&d, &tag, &v, &l, 0)) {
        if (tag == 0x82 && !isip && name_match(v, l, host)) return 0;    /* dNSName */
        if (tag == 0x87 && isip && l == 4 && !memcmp(v, ip, 4)) return 0;   /* iPAddress */
    }
    return -1;
}

int x509_verify_chain(const uint8_t **certs, const size_t *lens, int n, const char *host,
                      int64_t now, const char *roots_path, const char **why)
{
    static x509_t chain[10];
    x509_t root;
    if (n < 1 || n > 10) { *why = "no certificate"; return -1; }
    for (int i = 0; i < n; i++)
        if (x509_parse(&chain[i], certs[i], lens[i])) { *why = "unreadable certificate"; return -1; }
    if (x509_host(&chain[0], host)) { *why = "certificate is for another name"; return -1; }
    size_t rlen = 0;
    char *roots = file_get(roots_path, &rlen);
    if (!roots) { *why = "no trusted roots (/etc/ssl/roots)"; return -1; }
    int ok = -1, used[10] = { 0 };
    x509_t *cur = &chain[0];
    *why = "unknown issuer";
    for (int depth = 0; depth < 10; depth++) {
        if (now && (now < cur->not_before || now > cur->not_after)) { *why = "certificate expired or not yet valid"; break; }
        if (depth && !cur->ca) { *why = "issuer is not a certificate authority"; break; }
        /* signed by a trusted root? */
        for (const uint8_t *p = (const uint8_t *)roots, *e = p + rlen; p + 4 < e;) {
            size_t l = p[1] == 0x82 ? (size_t)(p[2] << 8 | p[3]) + 4 : p[1] == 0x81 ? (size_t)p[2] + 3 : 0;
            if (!l || p + l > e) break;
            if (!x509_parse(&root, p, l) && root.slen == cur->ilen && !memcmp(root.subject, cur->issuer, cur->ilen) &&
                !x509_check_sig(cur, &root)) { ok = 0; break; }
            p += l;
        }
        if (!ok) break;
        /* else by the next certificate the server sent */
        int next = -1;
        for (int i = 1; i < n; i++)
            if (!used[i] && chain[i].slen == cur->ilen && !memcmp(chain[i].subject, cur->issuer, cur->ilen)) { next = i; break; }
        if (next < 0) break;
        if (x509_check_sig(cur, &chain[next])) { *why = "bad signature in the chain"; break; }
        used[next] = 1;
        cur = &chain[next];
    }
    free(roots);
    return ok;
}
