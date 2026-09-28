/*
 * x509.c - Minimal X.509 v3 (RFC 5280) for TLS server authentication.
 *
 * Supports RSA keys with sha256/384/512WithRSAEncryption signatures,
 * validity periods, basicConstraints and subjectAltName dNSName matching
 * (with a single left-most wildcard label).
 */
#include "x509.h"

/* ---------------- DER ---------------- */

/* Read one TLV from d; returns false on malformed input. */
static bool der_next(struct der *d, int *tag, struct der *val, struct der *whole)
{
    if (d->len < 2)
        return false;
    const uint8_t *start = d->p;
    int t = d->p[0];
    size_t len = d->p[1], hdr = 2;
    if (len & 0x80) {
        int nb = len & 0x7F;
        if (nb < 1 || nb > 4 || d->len < 2 + (size_t)nb)
            return false;
        len = 0;
        for (int i = 0; i < nb; i++)
            len = len << 8 | d->p[2 + i];
        hdr += nb;
    }
    if (d->len - hdr < len)
        return false;
    *tag = t;
    val->p = d->p + hdr;
    val->len = len;
    if (whole) {
        whole->p = start;
        whole->len = hdr + len;
    }
    d->p += hdr + len;
    d->len -= hdr + len;
    return true;
}

static bool der_expect(struct der *d, int tag, struct der *val, struct der *whole)
{
    int t;
    struct der save = *d;
    if (!der_next(d, &t, val, whole) || t != tag) {
        *d = save;
        return false;
    }
    return true;
}

static bool oid_is(struct der o, const uint8_t *oid, size_t len)
{
    return o.len == len && memcmp(o.p, oid, len) == 0;
}

static const uint8_t OID_RSA[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01 };
static const uint8_t OID_SHA256RSA[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B };
static const uint8_t OID_SHA384RSA[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C };
static const uint8_t OID_SHA512RSA[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0D };
static const uint8_t OID_CN[] = { 0x55, 0x04, 0x03 };
static const uint8_t OID_SAN[] = { 0x55, 0x1D, 0x11 };
static const uint8_t OID_BASIC[] = { 0x55, 0x1D, 0x13 };

static int sig_hash_of(struct der alg)
{
    struct der oid;
    if (!der_expect(&alg, 0x06, &oid, NULL))
        return -1;
    if (oid_is(oid, OID_SHA256RSA, sizeof(OID_SHA256RSA)))
        return HASH_SHA256;
    if (oid_is(oid, OID_SHA384RSA, sizeof(OID_SHA384RSA)))
        return HASH_SHA384;
    if (oid_is(oid, OID_SHA512RSA, sizeof(OID_SHA512RSA)))
        return HASH_SHA512;
    return -1;
}

static long days_from_civil(long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static bool parse_time(struct der *d, long *out)
{
    int tag;
    struct der v;
    if (!der_next(d, &tag, &v, NULL))
        return false;
    const uint8_t *s = v.p;
    int digits[14], nd = tag == 0x17 ? 12 : tag == 0x18 ? 14 : 0;
    if (!nd || v.len < (size_t)nd + 1)
        return false;
    for (int i = 0; i < nd; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        digits[i] = s[i] - '0';
    }
    int k = 0;
    long year;
    if (nd == 12) {
        year = digits[0] * 10 + digits[1];
        year += year < 50 ? 2000 : 1900;
        k = 2;
    } else {
        year = digits[0] * 1000 + digits[1] * 100 + digits[2] * 10 + digits[3];
        k = 4;
    }
    int mon = digits[k] * 10 + digits[k + 1], day = digits[k + 2] * 10 + digits[k + 3];
    int hh = digits[k + 4] * 10 + digits[k + 5], mm = digits[k + 6] * 10 + digits[k + 7];
    int ss = digits[k + 8] * 10 + digits[k + 9];
    *out = days_from_civil(year, mon, day) * 86400 + hh * 3600 + mm * 60 + ss;
    return true;
}

/* Extract the subject CN (for messages only). */
static void name_cn(struct der name, char *out, size_t outlen)
{
    struct der rdn, set = name;
    out[0] = 0;
    if (!der_expect(&set, 0x30, &rdn, NULL))
        return;
    while (rdn.len) {
        struct der s, atv, oid, val;
        int tag;
        if (!der_expect(&rdn, 0x31, &s, NULL))
            return;
        while (s.len) {
            if (!der_expect(&s, 0x30, &atv, NULL) || !der_expect(&atv, 0x06, &oid, NULL) ||
                !der_next(&atv, &tag, &val, NULL))
                return;
            if (oid_is(oid, OID_CN, sizeof(OID_CN))) {
                size_t n = val.len < outlen - 1 ? val.len : outlen - 1;
                memcpy(out, val.p, n);
                out[n] = 0;
            }
        }
    }
}

bool x509_parse(struct x509 *c, const uint8_t *buf, size_t len)
{
    memset(c, 0, sizeof(*c));
    struct der d = { buf, len }, cert, tbs, alg, v;
    int tag;
    if (!der_expect(&d, 0x30, &cert, &c->raw))
        return false;
    if (!der_expect(&cert, 0x30, &tbs, &c->tbs) || !der_expect(&cert, 0x30, &alg, NULL) ||
        !der_expect(&cert, 0x03, &c->sig, NULL))
        return false;
    if (c->sig.len < 1 || c->sig.p[0] != 0)          /* unused-bits byte */
        return false;
    c->sig.p++;
    c->sig.len--;
    c->sig_hash = sig_hash_of(alg);

    der_expect(&tbs, 0xA0, &v, NULL);                 /* version (optional) */
    if (!der_expect(&tbs, 0x02, &v, NULL))           /* serial */
        return false;
    if (!der_expect(&tbs, 0x30, &v, NULL))           /* signature algorithm */
        return false;
    struct der name;
    if (!der_expect(&tbs, 0x30, &name, &c->issuer))
        return false;
    struct der validity;
    if (!der_expect(&tbs, 0x30, &validity, NULL) || !parse_time(&validity, &c->not_before) ||
        !parse_time(&validity, &c->not_after))
        return false;
    if (!der_expect(&tbs, 0x30, &name, &c->subject))
        return false;
    name_cn(c->subject, c->cn, sizeof(c->cn));
    struct der spki, kalg, koid, kbits;
    if (!der_expect(&tbs, 0x30, &spki, NULL) || !der_expect(&spki, 0x30, &kalg, NULL) ||
        !der_expect(&kalg, 0x06, &koid, NULL) || !der_expect(&spki, 0x03, &kbits, NULL))
        return false;
    if (oid_is(koid, OID_RSA, sizeof(OID_RSA)) && kbits.len > 1 && kbits.p[0] == 0) {
        struct der rk = { kbits.p + 1, kbits.len - 1 }, seq, n, e;
        if (der_expect(&rk, 0x30, &seq, NULL) && der_expect(&seq, 0x02, &n, NULL) &&
            der_expect(&seq, 0x02, &e, NULL)) {
            while (n.len > 1 && n.p[0] == 0) {
                n.p++;
                n.len--;
            }
            c->is_rsa = true;
            c->key.n = n.p;
            c->key.nlen = n.len;
            c->key.e = e.p;
            c->key.elen = e.len;
        }
    }
    /* optional issuerUniqueID [1], subjectUniqueID [2], extensions [3] */
    while (tbs.len) {
        if (!der_next(&tbs, &tag, &v, NULL))
            return false;
        if (tag != 0xA3)
            continue;
        struct der exts;
        if (!der_expect(&v, 0x30, &exts, NULL))
            return false;
        while (exts.len) {
            struct der ext, oid, val, b;
            if (!der_expect(&exts, 0x30, &ext, NULL) || !der_expect(&ext, 0x06, &oid, NULL))
                return false;
            der_expect(&ext, 0x01, &b, NULL);         /* critical (optional) */
            if (!der_expect(&ext, 0x04, &val, NULL))
                return false;
            if (oid_is(oid, OID_SAN, sizeof(OID_SAN))) {
                struct der names;
                if (der_expect(&val, 0x30, &names, NULL))
                    c->san = names;
            } else if (oid_is(oid, OID_BASIC, sizeof(OID_BASIC))) {
                struct der bc, ca;
                if (der_expect(&val, 0x30, &bc, NULL) && der_expect(&bc, 0x01, &ca, NULL))
                    c->is_ca = ca.len == 1 && ca.p[0] != 0;
            }
        }
    }
    return true;
}

static bool label_match(const char *pat, size_t plen, const char *host)
{
    size_t hlen = strlen(host);
    if (plen > 2 && pat[0] == '*' && pat[1] == '.') {
        const char *dot = strchr(host, '.');
        if (!dot || dot == host)
            return false;
        host = dot;
        hlen = strlen(host);
        pat++;
        plen--;
    }
    if (plen != hlen)
        return false;
    for (size_t i = 0; i < plen; i++) {
        char a = pat[i], b = host[i];
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (b >= 'A' && b <= 'Z')
            b += 32;
        if (a != b)
            return false;
    }
    return true;
}

bool x509_host_matches(const struct x509 *c, const char *host)
{
    struct der names = c->san, v;
    int tag;
    while (names.len && der_next(&names, &tag, &v, NULL))
        if (tag == 0x82 && label_match((const char *)v.p, v.len, host))
            return true;
    return false;
}

/* ---------------- trusted roots ---------------- */

#define MAXROOTS 32
static struct x509 roots[MAXROOTS];
static uint8_t *root_der[MAXROOTS];
static int nroots;

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

long pem_next_cert(const char **text, uint8_t *out, size_t outmax)
{
    const char *begin = strstr(*text, "-----BEGIN CERTIFICATE-----");
    if (!begin)
        return -1;
    const char *p = begin + 27;
    const char *end = strstr(p, "-----END CERTIFICATE-----");
    if (!end)
        return -1;
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; p < end; p++) {
        int v = b64val((unsigned char)*p);
        if (v < 0)
            continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= outmax)
                return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    *text = end + 25;
    return (long)n;
}

int x509_load_roots(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int before = nroots;
    size_t cap = 65536, len = 0;
    char *text = malloc(cap + 1);
    if (!text) {
        close(fd);
        return -1;
    }
    long got;
    while ((got = read(fd, text + len, cap - len)) > 0) {
        len += got;
        if (len == cap) {
            char *bigger = realloc(text, cap * 2 + 1);
            if (!bigger)
                break;
            text = bigger;
            cap *= 2;
        }
    }
    close(fd);
    text[len] = 0;
    const char *p = text;
    uint8_t tmp[4096];
    long n;
    while (nroots < MAXROOTS && (n = pem_next_cert(&p, tmp, sizeof(tmp))) > 0) {
        uint8_t *copy = malloc(n);
        if (!copy)
            break;
        memcpy(copy, tmp, n);
        if (x509_parse(&roots[nroots], copy, n) && roots[nroots].is_rsa)
            root_der[nroots++] = copy;
        else
            free(copy);
    }
    free(text);
    return nroots - before;
}

int x509_root_count(void)
{
    return nroots;
}

static bool same_der(struct der a, struct der b)
{
    return a.len == b.len && memcmp(a.p, b.p, a.len) == 0;
}

/* Is cert signed by issuer's key? */
static bool signed_by(const struct x509 *cert, const struct x509 *issuer)
{
    if (cert->sig_hash < 0 || !issuer->is_rsa)
        return false;
    uint8_t digest[HASH_MAX];
    hash_once((enum hash_alg)cert->sig_hash, cert->tbs.p, cert->tbs.len, digest);
    return rsa_verify_pkcs1(&issuer->key, (enum hash_alg)cert->sig_hash, digest, cert->sig.p, cert->sig.len);
}

static void set_err(char *err, size_t errlen, const char *msg, const char *cn)
{
    if (err)
        snprintf(err, errlen, "%s%s%s", msg, cn && cn[0] ? ": " : "", cn ? cn : "");
}

bool x509_verify_chain(struct x509 *certs, int n, const char *host, long now, char *err, size_t errlen)
{
    if (n < 1) {
        set_err(err, errlen, "no server certificate", NULL);
        return false;
    }
    if (!x509_host_matches(&certs[0], host)) {
        set_err(err, errlen, "certificate does not match the host name", certs[0].cn);
        return false;
    }
    for (int i = 0; i < n && i < 8; i++) {
        struct x509 *c = &certs[i];
        if (now < c->not_before || now > c->not_after) {
            set_err(err, errlen, "certificate expired or not yet valid (check the clock)", c->cn);
            return false;
        }
        if (i > 0 && !c->is_ca) {
            set_err(err, errlen, "intermediate certificate is not a CA", c->cn);
            return false;
        }
        /* issued by a trusted root? */
        for (int r = 0; r < nroots; r++) {
            if (same_der(roots[r].raw, c->raw))
                return true;                          /* the root itself was sent */
            if (same_der(roots[r].subject, c->issuer) && signed_by(c, &roots[r])) {
                if (now > roots[r].not_after) {
                    set_err(err, errlen, "root certificate expired", roots[r].cn);
                    return false;
                }
                return true;
            }
        }
        /* otherwise the next certificate must be its issuer */
        if (i + 1 >= n) {
            set_err(err, errlen, "certificate chain does not end at a trusted root", c->cn);
            return false;
        }
        if (!same_der(certs[i + 1].subject, c->issuer)) {
            set_err(err, errlen, "certificate chain is out of order", c->cn);
            return false;
        }
        if (!certs[i + 1].is_rsa || c->sig_hash < 0) {
            set_err(err, errlen, "unsupported certificate algorithm (only RSA is supported)", c->cn);
            return false;
        }
        if (!signed_by(c, &certs[i + 1])) {
            set_err(err, errlen, "bad certificate signature", c->cn);
            return false;
        }
    }
    set_err(err, errlen, "certificate chain too long", NULL);
    return false;
}
