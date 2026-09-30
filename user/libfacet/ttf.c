/*
 * ttf.c - TrueType fonts for libfacet (facet/font.h).
 *
 * The tables used: cmap (formats 4 and 12), head, hhea, hmtx, maxp, loca,
 * glyf and, when there is one, a format 0 kern table.  Glyph outlines,
 * simple or composite, are quadratic curves; they are flattened to lines
 * in pixel space and rasterized with exact area coverage (the accumulation
 * method of font-rs): each line adds its signed area to an accumulation
 * buffer, and a running sum along each row gives the coverage.  There is
 * no hinting.  Each face caches the glyphs it has drawn.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "facet/font.h"

struct fct_font {
    const uint8_t *d;
    size_t size;
    int mapped;
    uint32_t glyf, loca, hmtx, cmap;
    int cmap_fmt;
    int upem, loca_long, nglyphs, nhmetrics;
    int ascent, descent;
    uint32_t kpairs;                 /* the kern pairs (format 0), 0 if none */
    int nkpairs;
};

struct glyph {
    unsigned cp;
    struct glyph *next;
    int gid;
    int w, h, bx, by;                /* bitmap size; its top left relative to the pen on the baseline */
    float adv;
    uint8_t *a;                      /* coverage, 0..255 */
};

struct fct_face {
    struct fct_font *f;
    int px;
    float scale;
    int ascent, descent;
    struct glyph *tab[256];
};

/* ---------------- reading the file ---------------- */

static uint16_t u16(const struct fct_font *f, uint32_t o)
{
    return (size_t)o + 2 <= f->size ? (uint16_t)(f->d[o] << 8 | f->d[o + 1]) : 0;
}

static int16_t s16(const struct fct_font *f, uint32_t o)
{
    return (int16_t)u16(f, o);
}

static uint32_t u32(const struct fct_font *f, uint32_t o)
{
    return (uint32_t)u16(f, o) << 16 | u16(f, o + 2);
}

static uint32_t table(const struct fct_font *f, const char *tag)
{
    int n = u16(f, 4);
    for (int i = 0; i < n; i++) {
        uint32_t r = 12 + 16 * i;
        if ((size_t)r + 16 > f->size)
            return 0;
        if (!memcmp(f->d + r, tag, 4)) {
            uint32_t off = u32(f, r + 8), len = u32(f, r + 12);
            return off < f->size && len <= f->size - off ? off : 0;
        }
    }
    return 0;
}

static void choose_cmap(struct fct_font *f, uint32_t cm)
{
    int n = u16(f, cm + 2), best = 0;
    for (int i = 0; i < n; i++) {
        uint32_t r = cm + 4 + 8 * i;
        int pid = u16(f, r), eid = u16(f, r + 2);
        uint32_t off = cm + u32(f, r + 4);
        int fmt = u16(f, off), rank = 0;
        if (fmt == 12 && (pid == 0 || (pid == 3 && eid == 10)))
            rank = 3;
        else if (fmt == 4 && (pid == 0 || (pid == 3 && eid == 1)))
            rank = 2;
        if (rank > best) {
            best = rank;
            f->cmap = off;
            f->cmap_fmt = fmt;
        }
    }
}

static void find_kern(struct fct_font *f)
{
    uint32_t k = table(f, "kern");
    if (!k || u16(f, k) != 0)
        return;
    int n = u16(f, k + 2);
    uint32_t t = k + 4;
    for (int i = 0; i < n; i++) {
        int len = u16(f, t + 2), cov = u16(f, t + 4);
        if ((cov >> 8) == 0 && (cov & 1)) {           /* format 0, horizontal */
            f->nkpairs = u16(f, t + 6);
            f->kpairs = t + 14;
            return;
        }
        t += len;
    }
}

struct fct_font *fct_font_load(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    struct fct_font *f = calloc(1, sizeof(*f));
    if (!f || fstat(fd, &st) < 0 || st.st_size < 12) {
        close(fd);
        free(f);
        return NULL;
    }
    f->size = st.st_size;
    void *m = mmap(NULL, f->size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m != MAP_FAILED) {
        f->d = m;
        f->mapped = 1;
    } else {                                           /* no file mappings: read it */
        uint8_t *b = malloc(f->size);
        size_t got = 0;
        while (b && got < f->size) {
            ssize_t r = read(fd, b + got, f->size - got);
            if (r <= 0)
                break;
            got += r;
        }
        if (!b || got < f->size) {
            free(b);
            close(fd);
            free(f);
            return NULL;
        }
        f->d = b;
    }
    close(fd);
    uint32_t head = table(f, "head"), hhea = table(f, "hhea"), maxp = table(f, "maxp"), cm = table(f, "cmap");
    f->glyf = table(f, "glyf");
    f->loca = table(f, "loca");
    f->hmtx = table(f, "hmtx");
    if (head && hhea && maxp && cm && f->glyf && f->loca && f->hmtx) {
        f->upem = u16(f, head + 18);
        f->loca_long = s16(f, head + 50);
        f->nglyphs = u16(f, maxp + 4);
        f->ascent = s16(f, hhea + 4);
        f->descent = s16(f, hhea + 6);
        f->nhmetrics = u16(f, hhea + 34);
        choose_cmap(f, cm);
        find_kern(f);
    }
    if (!f->cmap || f->upem < 16 || !f->nhmetrics) {
        fct_font_free(f);
        return NULL;
    }
    return f;
}

void fct_font_free(struct fct_font *f)
{
    if (!f)
        return;
    if (f->mapped)
        munmap((void *)f->d, f->size);
    else
        free((void *)f->d);
    free(f);
}

static int glyph_index(const struct fct_font *f, unsigned cp)
{
    uint32_t t = f->cmap;
    if (f->cmap_fmt == 4) {
        if (cp > 0xFFFF)
            return 0;
        int nseg = u16(f, t + 6) / 2;
        uint32_t ends = t + 14, starts = ends + 2 * nseg + 2, deltas = starts + 2 * nseg, ranges = deltas + 2 * nseg;
        int lo = 0, hi = nseg - 1;
        while (lo <= hi) {                             /* the first segment ending at or after cp */
            int mid = (lo + hi) / 2;
            if (cp > u16(f, ends + 2 * mid))
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        if (lo >= nseg || cp < u16(f, starts + 2 * lo))
            return 0;
        uint16_t delta = u16(f, deltas + 2 * lo), ro = u16(f, ranges + 2 * lo);
        if (!ro)
            return (cp + delta) & 0xFFFF;
        uint16_t g = u16(f, ranges + 2 * lo + ro + 2 * (cp - u16(f, starts + 2 * lo)));
        return g ? (g + delta) & 0xFFFF : 0;
    }
    uint32_t n = u32(f, t + 12), lo = 0, hi = n;       /* format 12 */
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, g = t + 16 + 12 * mid;
        if (cp < u32(f, g))
            hi = mid;
        else if (cp > u32(f, g + 4))
            lo = mid + 1;
        else
            return u32(f, g + 8) + (cp - u32(f, g));
    }
    return 0;
}

static int advance_units(const struct fct_font *f, int gid)
{
    return u16(f, f->hmtx + 4 * (gid < f->nhmetrics ? gid : f->nhmetrics - 1));
}

static int kern_units(const struct fct_font *f, int left, int right)
{
    if (!f->kpairs)
        return 0;
    uint32_t key = (uint32_t)left << 16 | right;
    int lo = 0, hi = f->nkpairs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t p = f->kpairs + 6 * mid, k = u32(f, p);
        if (k == key)
            return s16(f, p + 4);
        if (k < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

/* ---------------- outlines ---------------- */

struct seg {
    float x0, y0, x1, y1;
};

struct outline {
    struct seg *s;
    int n, cap;
    float minx, miny, maxx, maxy;
};

static void line(struct outline *o, float x0, float y0, float x1, float y1)
{
    if (o->n == o->cap) {
        int nc = o->cap ? 2 * o->cap : 64;
        struct seg *s = realloc(o->s, nc * sizeof(*s));
        if (!s)
            return;
        o->s = s;
        o->cap = nc;
    }
    o->s[o->n++] = (struct seg){ x0, y0, x1, y1 };
    o->minx = fminf(o->minx, fminf(x0, x1));
    o->maxx = fmaxf(o->maxx, fmaxf(x0, x1));
    o->miny = fminf(o->miny, fminf(y0, y1));
    o->maxy = fmaxf(o->maxy, fmaxf(y0, y1));
}

static void quad(struct outline *o, float x0, float y0, float cx, float cy, float x1, float y1)
{
    float dx = x0 - 2 * cx + x1, dy = y0 - 2 * cy + y1;
    int n = 1 + (int)sqrtf(sqrtf(dx * dx + dy * dy) * 3);   /* error about 1/(8 n^2) of the deviation */
    if (n > 24)
        n = 24;
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n, u = 1 - t;
        float x = u * u * x0 + 2 * u * t * cx + t * t * x1, y = u * u * y0 + 2 * u * t * cy + t * t * y1;
        line(o, px, py, x, y);
        px = x;
        py = y;
    }
}

/* One contour of transformed points; on[] marks the on-curve ones. */
static void contour(struct outline *o, const float *x, const float *y, const uint8_t *on, int n)
{
    if (n < 2)
        return;
    int s = 0;
    while (s < n && !on[s])
        s++;
    float sx, sy;
    int first, count;
    if (s < n) {
        sx = x[s];
        sy = y[s];
        first = s + 1;
        count = n - 1;
    } else {                                           /* all off-curve: start between the first two */
        sx = (x[0] + x[1]) / 2;
        sy = (y[0] + y[1]) / 2;
        first = 1;
        count = n;
    }
    float lx = sx, ly = sy, cx = 0, cy = 0;
    int ctrl = 0;
    for (int k = 0; k < count; k++) {
        int i = (first + k) % n;
        if (on[i]) {
            if (ctrl)
                quad(o, lx, ly, cx, cy, x[i], y[i]);
            else
                line(o, lx, ly, x[i], y[i]);
            lx = x[i];
            ly = y[i];
            ctrl = 0;
        } else {
            if (ctrl) {
                float mx = (cx + x[i]) / 2, my = (cy + y[i]) / 2;
                quad(o, lx, ly, cx, cy, mx, my);
                lx = mx;
                ly = my;
            }
            cx = x[i];
            cy = y[i];
            ctrl = 1;
        }
    }
    if (ctrl)
        quad(o, lx, ly, cx, cy, sx, sy);
    else if (lx != sx || ly != sy)
        line(o, lx, ly, sx, sy);
}

/* The outline of glyph gid, through the matrix m (x' = m0 x + m2 y + m4, y' = m1 x + m3 y + m5). */
static void glyph_outline(const struct fct_font *f, int gid, const float m[6], struct outline *o, int depth)
{
    if (gid >= f->nglyphs || depth > 8)
        return;
    uint32_t a, b;
    if (f->loca_long) {
        a = u32(f, f->loca + 4 * gid);
        b = u32(f, f->loca + 4 * gid + 4);
    } else {
        a = 2 * (uint32_t)u16(f, f->loca + 2 * gid);
        b = 2 * (uint32_t)u16(f, f->loca + 2 * gid + 2);
    }
    if (b <= a || (size_t)f->glyf + b > f->size)
        return;                                        /* no outline (a space) */
    uint32_t g = f->glyf + a;
    int nc = s16(f, g);
    if (nc < 0) {                                      /* composite */
        uint32_t p = g + 10;
        for (;;) {
            int flags = u16(f, p), cg = u16(f, p + 2);
            float dx, dy;
            p += 4;
            if (flags & 1) {
                dx = s16(f, p);
                dy = s16(f, p + 2);
                p += 4;
            } else {
                dx = (int8_t)(u16(f, p) >> 8);
                dy = (int8_t)(u16(f, p) & 255);
                p += 2;
            }
            if (!(flags & 2))
                dx = dy = 0;                           /* point matching: not supported */
            float xa = 1, xb = 0, xc = 0, xd = 1;
            if (flags & 8) {
                xa = xd = s16(f, p) / 16384.0f;
                p += 2;
            } else if (flags & 0x40) {
                xa = s16(f, p) / 16384.0f;
                xd = s16(f, p + 2) / 16384.0f;
                p += 4;
            } else if (flags & 0x80) {
                xa = s16(f, p) / 16384.0f;
                xb = s16(f, p + 2) / 16384.0f;
                xc = s16(f, p + 4) / 16384.0f;
                xd = s16(f, p + 6) / 16384.0f;
                p += 8;
            }
            float cm[6] = { m[0] * xa + m[2] * xb, m[1] * xa + m[3] * xb, m[0] * xc + m[2] * xd,
                            m[1] * xc + m[3] * xd, m[0] * dx + m[2] * dy + m[4], m[1] * dx + m[3] * dy + m[5] };
            glyph_outline(f, cg, cm, o, depth + 1);
            if (!(flags & 0x20) || p >= g + (b - a))
                break;
        }
        return;
    }
    if (!nc)
        return;
    int npts = u16(f, g + 10 + 2 * (nc - 1)) + 1;
    uint32_t p = g + 10 + 2 * nc;
    p += 2 + u16(f, p);                                /* instructions */
    uint8_t *fl = malloc(npts);
    float *x = malloc(npts * sizeof(float)), *y = malloc(npts * sizeof(float));
    if (!fl || !x || !y)
        goto out;
    for (int i = 0; i < npts; ) {
        if (p >= f->size)
            goto out;
        uint8_t c = f->d[p++];
        int rep = 1;
        if (c & 8) {
            if (p >= f->size)
                goto out;
            rep += f->d[p++];
        }
        while (rep-- && i < npts)
            fl[i++] = c;
    }
    int v = 0;
    for (int i = 0; i < npts; i++) {                   /* x coordinates */
        if (fl[i] & 2) {
            int d = p < f->size ? f->d[p] : 0;
            p++;
            v += fl[i] & 16 ? d : -d;
        } else if (!(fl[i] & 16)) {
            v += s16(f, p);
            p += 2;
        }
        x[i] = v;
    }
    v = 0;
    for (int i = 0; i < npts; i++) {                   /* y coordinates */
        if (fl[i] & 4) {
            int d = p < f->size ? f->d[p] : 0;
            p++;
            v += fl[i] & 32 ? d : -d;
        } else if (!(fl[i] & 32)) {
            v += s16(f, p);
            p += 2;
        }
        y[i] = v;
    }
    for (int i = 0; i < npts; i++) {
        float tx = m[0] * x[i] + m[2] * y[i] + m[4], ty = m[1] * x[i] + m[3] * y[i] + m[5];
        x[i] = tx;
        y[i] = ty;
        fl[i] &= 1;
    }
    int start = 0;
    for (int c = 0; c < nc; c++) {
        int end = u16(f, g + 10 + 2 * c);
        if (end < start || end >= npts)
            break;
        contour(o, x + start, y + start, fl + start, end - start + 1);
        start = end + 1;
    }
out:
    free(fl);
    free(x);
    free(y);
}

/* ---------------- rasterizing ---------------- */

static void acc_line(float *a, int w, int h, float x0, float y0, float x1, float y1)
{
    if (y0 == y1)
        return;
    float dir = 1;
    if (y0 > y1) {
        float t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        dir = -1;
    }
    float dxdy = (x1 - x0) / (y1 - y0), x = x0;
    int ys = y0 < 0 ? 0 : (int)y0;
    if (y0 < 0)
        x -= y0 * dxdy;
    int ye = (int)ceilf(y1);
    if (ye > h)
        ye = h;
    for (int y = ys; y < ye; y++) {
        float *row = a + y * w;
        float dy = fminf(y + 1, y1) - fmaxf(y, y0), xn = x + dxdy * dy, d = dy * dir;
        float xa = fminf(x, xn), xb = fmaxf(x, xn);
        /* The bitmap starts at the polygon's leftmost point and has a margin on
         * the right, so an edge only leaves it by rounding (x = -1e-7): clamped,
         * not dropped.  (A dropped part of an edge left its row's coverage
         * uncancelled, and the running sum carried it into every row below: a
         * pale band down the rest of the shape, depending on where it was drawn.) */
        xa = fminf(fmaxf(xa, 0), (float)w - 1);
        xb = fminf(fmaxf(xb, xa), (float)w - 1);
        float xaf = floorf(xa);
        int xai = (int)xaf, xbi = (int)ceilf(xb);
        if (xbi <= xai + 1) {
            float xm = 0.5f * (xa + xb) - xaf;
            row[xai] += d - d * xm;
            row[xai + 1] += d * xm;
        } else {
            float s = 1 / (xb - xa), x0f = xa - xaf;
            float a0 = 0.5f * s * (1 - x0f) * (1 - x0f);
            float x1f = xb - xbi + 1, am = 0.5f * s * x1f * x1f;
            row[xai] += d * a0;
            if (xbi == xai + 2) {
                row[xai + 1] += d * (1 - a0 - am);
            } else {
                float a1 = s * (1.5f - x0f);
                row[xai + 1] += d * (a1 - a0);
                for (int xi = xai + 2; xi < xbi - 1; xi++)
                    row[xi] += d * s;
                float a2 = a1 + (xbi - xai - 3) * s;
                row[xbi - 1] += d * (1 - a2 - am);
            }
            row[xbi] += d * am;
        }
        x = xn;
    }
}

static struct glyph *render(struct fct_face *fc, unsigned cp)
{
    struct fct_font *f = fc->f;
    struct glyph *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->cp = cp;
    g->gid = glyph_index(f, cp);
    g->adv = advance_units(f, g->gid) * fc->scale;
    struct outline o = { NULL, 0, 0, 1e9f, 1e9f, -1e9f, -1e9f };
    float m[6] = { fc->scale, 0, 0, -fc->scale, 0, 0 };    /* y down, origin on the baseline */
    glyph_outline(f, g->gid, m, &o, 0);
    if (o.n) {
        g->bx = (int)floorf(o.minx);
        g->by = (int)floorf(o.miny);
        g->w = (int)ceilf(o.maxx) - g->bx + 2;
        g->h = (int)ceilf(o.maxy) - g->by;
        if (g->w > 0 && g->h > 0 && g->w * g->h < 1 << 22) {
            float *acc = calloc((size_t)g->w * g->h + 4, sizeof(float));
            g->a = acc ? malloc((size_t)g->w * g->h) : NULL;
            if (g->a) {
                for (int i = 0; i < o.n; i++)
                    acc_line(acc, g->w, g->h, o.s[i].x0 - g->bx, o.s[i].y0 - g->by, o.s[i].x1 - g->bx,
                             o.s[i].y1 - g->by);
                float sum = 0;
                for (int i = 0; i < g->w * g->h; i++) {
                    sum += acc[i];
                    float c = fabsf(sum);
                    g->a[i] = c >= 1 ? 255 : (uint8_t)(c * 255 + 0.5f);
                }
            }
            free(acc);
        }
    }
    free(o.s);
    return g;
}

/* ---------------- faces ---------------- */

struct fct_face *fct_face_new(struct fct_font *f, int px)
{
    if (!f || px < 4 || px > 512)
        return NULL;
    struct fct_face *fc = calloc(1, sizeof(*fc));
    if (!fc)
        return NULL;
    fc->f = f;
    fc->px = px;
    fc->scale = (float)px / f->upem;
    fc->ascent = (int)lroundf(f->ascent * fc->scale);
    fc->descent = (int)lroundf(-f->descent * fc->scale);
    return fc;
}

void fct_face_free(struct fct_face *fc)
{
    if (!fc)
        return;
    for (int i = 0; i < 256; i++)
        for (struct glyph *g = fc->tab[i], *n; g; g = n) {
            n = g->next;
            free(g->a);
            free(g);
        }
    free(fc);
}

int fct_face_ascent(const struct fct_face *fc) { return fc->ascent; }
int fct_face_descent(const struct fct_face *fc) { return fc->descent; }
int fct_face_height(const struct fct_face *fc) { return fc->ascent + fc->descent; }

static struct glyph *glyph(struct fct_face *fc, unsigned cp)
{
    struct glyph **b = &fc->tab[cp & 255];
    for (struct glyph *g = *b; g; g = g->next)
        if (g->cp == cp)
            return g;
    struct glyph *g = render(fc, cp);
    if (g) {
        g->next = *b;
        *b = g;
    }
    return g;
}

static unsigned next_cp(const char **sp)
{
    const unsigned char *p = (const unsigned char *)*sp;
    unsigned c = *p++;
    if (c >= 0xC2 && c < 0xF5) {
        int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1, i;
        unsigned v = c & (0x3F >> n);
        for (i = 0; i < n && (p[i] & 0xC0) == 0x80; i++)
            v = v << 6 | (p[i] & 0x3F);
        if (i == n) {
            *sp = (const char *)p + n;
            return v;
        }
    }
    *sp = (const char *)p;
    return c;                                          /* not UTF-8: the byte, as Latin-1 */
}

static void blit(struct surface *s, int x, int y, const struct glyph *g, color_t fg)
{
    if (!g->a)
        return;
    struct rect r = rect_intersect(rect_make(x, y, g->w, g->h), s->clip);
    int fr = (fg >> 16) & 255, fgg = (fg >> 8) & 255, fb = fg & 255;
    for (int j = r.y; j < r.y + r.h; j++) {
        const uint8_t *src = g->a + (j - y) * g->w + (r.x - x);
        uint32_t *d = s->px + j * s->stride + r.x;
        for (int i = 0; i < r.w; i++) {
            int a = src[i];
            if (!a)
                continue;
            if (a == 255) {
                d[i] = fg;
                continue;
            }
            a += a >> 7;                               /* 0..256 */
            uint32_t p = d[i];
            int pr = (p >> 16) & 255, pg = (p >> 8) & 255, pb = p & 255;
            d[i] = RGB(pr + (((fr - pr) * a) >> 8), pg + (((fgg - pg) * a) >> 8), pb + (((fb - pb) * a) >> 8));
        }
    }
}

int fct_face_draw(struct surface *s, struct fct_face *fc, int x, int y, const char *utf8, color_t fg)
{
    float pen = x;
    int prev = -1;
    while (*utf8) {
        unsigned cp = next_cp(&utf8);
        struct glyph *g = glyph(fc, cp);
        if (!g)
            continue;
        if (prev >= 0)
            pen += kern_units(fc->f, prev, g->gid) * fc->scale;
        blit(s, (int)floorf(pen + 0.5f) + g->bx, y + g->by, g, fg);
        pen += g->adv;
        prev = g->gid;
    }
    return (int)floorf(pen - x + 0.5f);
}

int fct_face_draw_cp(struct surface *s, struct fct_face *fc, int x, int y, unsigned cp, color_t fg)
{
    struct glyph *g = glyph(fc, cp);
    if (!g)
        return 0;
    blit(s, x + g->bx, y + g->by, g, fg);
    return (int)floorf(g->adv + 0.5f);
}

int fct_face_width(struct fct_face *fc, const char *utf8)
{
    float pen = 0;
    int prev = -1;
    while (*utf8) {
        unsigned cp = next_cp(&utf8);
        struct glyph *g = glyph(fc, cp);
        if (!g)
            continue;
        if (prev >= 0)
            pen += kern_units(fc->f, prev, g->gid) * fc->scale;
        pen += g->adv;
        prev = g->gid;
    }
    return (int)floorf(pen + 0.5f);
}

size_t fct_face_fit(struct fct_face *fc, const char *utf8, int maxw)
{
    const char *start = utf8;
    float pen = 0;
    int prev = -1;
    while (*utf8) {
        const char *here = utf8;
        unsigned cp = next_cp(&utf8);
        struct glyph *g = glyph(fc, cp);
        if (!g)
            continue;
        float adv = g->adv + (prev >= 0 ? kern_units(fc->f, prev, g->gid) * fc->scale : 0);
        if (pen + adv > maxw + 0.5f)
            return here - start;
        pen += adv;
        prev = g->gid;
    }
    return utf8 - start;
}

float fct_face_advance(struct fct_face *fc, unsigned cp)
{
    struct glyph *g = glyph(fc, cp);
    return g ? g->adv : 0;
}

bool fct_face_has(struct fct_face *fc, unsigned cp)
{
    return glyph_index(fc->f, cp) != 0;
}

/* ---------------- the desktop's faces ---------------- */

#define FONT_DIR "/usr/share/fonts/dejavu/"

static const char *const ui_files[FCT_FONT_NFACES] = {
    FONT_DIR "DejaVuSans.ttf", FONT_DIR "DejaVuSans-Bold.ttf", FONT_DIR "DejaVuSansMono.ttf",
    FONT_DIR "DejaVuSansMono-Bold.ttf",
};

static struct fct_font *ui_fonts[FCT_FONT_NFACES];
static struct fct_face *ui_faces[FCT_FONT_NFACES];
static int ui_state;                                   /* 0 not tried, 1 loaded, -1 none */
static int ui_px = 13;

static struct {
    int which, px;
    struct fct_face *fc;
} sized[8];

static void ui_load(void)
{
    ui_state = -1;
    const char *e = getenv("FACET_FONT");
    if (e && !strcmp(e, "bitmap"))
        return;
    e = getenv("FACET_FONT_SIZE");
    if (e && atoi(e) >= 8 && atoi(e) <= 48)
        ui_px = atoi(e);
    for (int i = 0; i < FCT_FONT_NFACES; i++) {
        ui_fonts[i] = fct_font_load(ui_files[i]);
        if (!ui_fonts[i] && i != FCT_FONT_MONO_BOLD)
            return;                                    /* (Mono Bold may be missing: Mono stands in) */
    }
    for (int i = 0; i < FCT_FONT_NFACES; i++)
        ui_faces[i] = fct_face_new(ui_fonts[i] ? ui_fonts[i] : ui_fonts[FCT_FONT_MONO], ui_px);
    ui_state = 1;
}

struct fct_face *fct_ui_face(int which)
{
    if (!ui_state)
        ui_load();
    return ui_state > 0 && which >= 0 && which < FCT_FONT_NFACES ? ui_faces[which] : NULL;
}

struct fct_face *fct_ui_face_px(int which, int px)
{
    struct fct_face *base = fct_ui_face(which);
    if (!base || px == ui_px)
        return base;
    for (int i = 0; i < 8; i++)
        if (sized[i].fc && sized[i].which == which && sized[i].px == px)
            return sized[i].fc;
    for (int i = 0; i < 8; i++)
        if (!sized[i].fc) {
            sized[i].fc = fct_face_new(base->f, px);
            sized[i].which = which;
            sized[i].px = px;
            return sized[i].fc ? sized[i].fc : base;
        }
    return base;                                       /* (eight sizes in use: the usual one) */
}

/* ---------------- antialiased polygons (the same rasterizer) ---------------- */

/* Coverage of the polygon (xy: n points, closed) over its bounding box. */
static uint8_t *poly_coverage(const float *xy, int n, int *bx, int *by, int *bw, int *bh)
{
    if (n < 3)
        return NULL;
    float minx = xy[0], maxx = xy[0], miny = xy[1], maxy = xy[1];
    for (int i = 1; i < n; i++) {
        minx = fminf(minx, xy[2 * i]);
        maxx = fmaxf(maxx, xy[2 * i]);
        miny = fminf(miny, xy[2 * i + 1]);
        maxy = fmaxf(maxy, xy[2 * i + 1]);
    }
    *bx = (int)floorf(minx);
    *by = (int)floorf(miny);
    *bw = (int)ceilf(maxx) - *bx + 2;
    *bh = (int)ceilf(maxy) - *by;
    if (*bw <= 0 || *bh <= 0 || *bw * *bh > 1 << 22)
        return NULL;
    float *acc = calloc((size_t)*bw * *bh + 4, sizeof(float));
    uint8_t *a = acc ? malloc((size_t)*bw * *bh) : NULL;
    if (!a) {
        free(acc);
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        acc_line(acc, *bw, *bh, xy[2 * i] - *bx, xy[2 * i + 1] - *by, xy[2 * j] - *bx, xy[2 * j + 1] - *by);
    }
    float sum = 0;
    for (int i = 0; i < *bw * *bh; i++) {
        sum += acc[i];
        float c = fabsf(sum);
        a[i] = c >= 1 ? 255 : (uint8_t)(c * 255 + 0.5f);
    }
    free(acc);
    return a;
}

static void blend_cov(struct surface *s, int bx, int by, int bw, int bh, const uint8_t *a, color_t top, color_t bottom)
{
    struct rect r = rect_intersect(rect_make(bx, by, bw, bh), s->clip);
    for (int j = r.y; j < r.y + r.h; j++) {
        color_t c = bh > 1 ? color_mix(top, bottom, (j - by) * 256 / (bh - 1)) : top;
        int fr = (c >> 16) & 255, fg = (c >> 8) & 255, fb = c & 255;
        const uint8_t *src = a + (j - by) * bw + (r.x - bx);
        uint32_t *d = s->px + j * s->stride + r.x;
        for (int i = 0; i < r.w; i++) {
            int k = src[i];
            if (!k)
                continue;
            k += k >> 7;
            uint32_t p = d[i];
            int pr = (p >> 16) & 255, pg = (p >> 8) & 255, pb = p & 255;
            d[i] = RGB(pr + (((fr - pr) * k) >> 8), pg + (((fg - pg) * k) >> 8), pb + (((fb - pb) * k) >> 8));
        }
    }
}

void gfx_poly_vgradient(struct surface *s, const float *xy, int n, color_t top, color_t bottom)
{
    int bx, by, bw, bh;
    uint8_t *a = poly_coverage(xy, n, &bx, &by, &bw, &bh);
    if (a)
        blend_cov(s, bx, by, bw, bh, a, top, bottom);
    free(a);
}

void gfx_poly(struct surface *s, const float *xy, int n, color_t c)
{
    gfx_poly_vgradient(s, xy, n, c, c);
}

void gfx_stroke(struct surface *s, const float *xy, int n, bool closed, float width, color_t c)
{
    float h = width / 2;
    for (int i = 0; i + 1 < n + (closed ? 1 : 0); i++) {
        int j = (i + 1) % n;
        float x0 = xy[2 * i], y0 = xy[2 * i + 1], x1 = xy[2 * j], y1 = xy[2 * j + 1];
        float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
        if (len < 0.01f)
            continue;
        float nx = -dy / len * h, ny = dx / len * h, ex = dx / len * h, ey = dy / len * h;
        float q[8] = { x0 + nx - ex, y0 + ny - ey, x1 + nx + ex, y1 + ny + ey,
                       x1 - nx + ex, y1 - ny + ey, x0 - nx - ex, y0 - ny - ey };
        gfx_poly(s, q, 4, c);
    }
}

void gfx_ellipse_aa(struct surface *s, float cx, float cy, float rx, float ry, color_t top, color_t bottom)
{
    float xy[2 * 48];
    for (int i = 0; i < 48; i++) {
        float t = (float)i / 48 * 6.2831853f;
        xy[2 * i] = cx + rx * cosf(t);
        xy[2 * i + 1] = cy + ry * sinf(t);
    }
    gfx_poly_vgradient(s, xy, 48, top, bottom);
}
