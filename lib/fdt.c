/*
 * fdt.c - Reading a flattened device tree (see mk/fdt.h).
 *
 * The blob: a header, then a "structure" block of 32-bit big-endian tokens
 * (BEGIN_NODE name, PROP length name-offset value, END_NODE, END), and a
 * "strings" block holding the property names. Every offset and length is
 * checked against the blob's size: a damaged tree gives "not found", never
 * a read out of bounds.
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdint.h>
#include "mk/lib.h"
#include "mk/fdt.h"

enum { T_BEGIN = 1, T_END_NODE = 2, T_PROP = 3, T_NOP = 4, T_END = 9 };
#define MAXDEPTH 16

uint32_t fdt_be32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}
uint64_t fdt_cells(const void *p, int n)
{
    return n == 2 ? (uint64_t)fdt_be32(p) << 32 | fdt_be32((const uint8_t *)p + 4) : fdt_be32(p);
}

static const uint8_t *st(const void *f)   { return (const uint8_t *)f + fdt_be32((const uint8_t *)f + 8); }
static uint32_t st_size(const void *f)    { return fdt_be32((const uint8_t *)f + 36); }
static const char *strs(const void *f)    { return (const char *)f + fdt_be32((const uint8_t *)f + 12); }
static uint32_t strs_size(const void *f)  { return fdt_be32((const uint8_t *)f + 32); }

uint32_t fdt_valid(const void *f, uint64_t max)
{
    if (max < 40 || fdt_be32(f) != 0xD00DFEED) return 0;
    const uint8_t *h = f;
    uint32_t size = fdt_be32(h + 4), so = fdt_be32(h + 8), ss = fdt_be32(h + 36), to = fdt_be32(h + 12), ts = fdt_be32(h + 32);
    if (size > max || fdt_be32(h + 20) < 17 || so % 4 || (uint64_t)so + ss > size || (uint64_t)to + ts > size) return 0;
    return size;
}

/* The token at offset o of the structure block (T_END past its end). */
static uint32_t tok(const void *f, uint32_t o) { return o + 4 <= st_size(f) ? fdt_be32(st(f) + o) : T_END; }

/* The offset just past a BEGIN_NODE's name / a PROP's value (4-byte aligned). */
static uint32_t skip_name(const void *f, uint32_t o)
{
    uint32_t n = st_size(f);
    for (o += 4; o < n && st(f)[o]; o++) ;
    return (o + 4) & ~3u;
}
static uint32_t skip_prop(const void *f, uint32_t o)
{
    uint32_t len = o + 8 <= st_size(f) ? fdt_be32(st(f) + o + 4) : 0;
    return (o + 12 + len + 3) & ~3u;
}

int fdt_next(const void *f, int node, int *depth)
{
    uint32_t o = 0;
    if (node >= 0) o = skip_name(f, node);          /* inside node: its properties, then children */
    else *depth = -1;
    for (int guard = 0; guard < 1 << 20; guard++) {
        switch (tok(f, o)) {
        case T_BEGIN:    ++*depth; return (int)o;
        case T_END_NODE: --*depth; o += 4; break;
        case T_PROP:     o = skip_prop(f, o); break;
        case T_NOP:      o += 4; break;
        default:         return -1;
        }
    }
    return -1;
}

const char *fdt_name(const void *f, int node) { return (const char *)st(f) + node + 4; }

const void *fdt_prop(const void *f, int node, const char *name, int *len)
{
    uint32_t o = skip_name(f, node);
    for (;;) {
        uint32_t t = tok(f, o);
        if (t == T_NOP) { o += 4; continue; }
        if (t != T_PROP || o + 12 > st_size(f)) return 0;
        uint32_t l = fdt_be32(st(f) + o + 4), no = fdt_be32(st(f) + o + 8);
        if (no < strs_size(f) && o + 12 + l <= st_size(f) && !strcmp(strs(f) + no, name)) {
            if (len) *len = (int)l;
            return st(f) + o + 12;
        }
        o = skip_prop(f, o);
    }
}

int fdt_compatible(const void *f, int node, const char *compat)
{
    int len;
    const char *p = fdt_prop(f, node, "compatible", &len);
    for (int i = 0; p && i < len; i += strlen(p + i) + 1)        /* a list of 0-terminated strings */
        if (!strcmp(p + i, compat)) return 1;
    return 0;
}

int fdt_find(const void *f, int after, const char *compat)
{
    int d = 0, n = after;
    while ((n = fdt_next(f, n, &d)) >= 0)
        if (fdt_compatible(f, n, compat)) return n;
    return -1;
}

int fdt_path(const void *f, const char *path)
{
    int d = 0, n = fdt_next(f, -1, &d), want = 1;          /* n: the root */
    for (const char *c = path + 1; *c; ) {                  /* one component at a time */
        const char *e = strchr(c, '/') ? strchr(c, '/') : c + strlen(c);
        int found = -1;
        for (int m = n, dm = d; (m = fdt_next(f, m, &dm)) >= 0 && dm >= want; )
            if (dm == want) {
                const char *nm = fdt_name(f, m);
                size_t k = e - c;
                if (!memcmp(nm, c, k) && (!nm[k] || nm[k] == '@')) { found = m; break; }
            }
        if (found < 0) return -1;
        n = found; d = want++;
        c = *e ? e + 1 : e;
    }
    return n;
}

/* Walk from the root to node: its ancestors (anc[0] the root, anc[depth]
 * the node) and the cells in force at each depth (cells[k]: what node k's
 * parent declared, i.e. the format of node k's "reg"). Returns its depth. */
static int ancestry(const void *f, int node, int anc[MAXDEPTH], int ac[MAXDEPTH + 1], int sc[MAXDEPTH + 1])
{
    int d = 0;
    ac[0] = 2; sc[0] = 1;                                   /* the root's own defaults */
    for (int n = -1; (n = fdt_next(f, n, &d)) >= 0; ) {
        if (d >= MAXDEPTH) continue;
        anc[d] = n;
        const void *p;
        ac[d + 1] = (p = fdt_prop(f, n, "#address-cells", 0)) ? (int)fdt_be32(p) : 2;
        sc[d + 1] = (p = fdt_prop(f, n, "#size-cells", 0)) ? (int)fdt_be32(p) : 1;
        if (n == node) return d;
    }
    return -1;
}

void fdt_reg_cells(const void *f, int node, int *ac, int *sc)
{
    int anc[MAXDEPTH], a[MAXDEPTH + 1], s[MAXDEPTH + 1], d = ancestry(f, node, anc, a, s);
    *ac = d >= 0 ? a[d] : 2;
    *sc = d >= 0 ? s[d] : 1;
}

int fdt_reg(const void *f, int node, int i, uint64_t *addr, uint64_t *size)
{
    int anc[MAXDEPTH], ac[MAXDEPTH + 1], sc[MAXDEPTH + 1], len, d = ancestry(f, node, anc, ac, sc);
    const uint8_t *r = fdt_prop(f, node, "reg", &len);
    int w = 4 * (ac[d] + sc[d]);
    if (d < 0 || !r || ac[d] < 1 || ac[d] > 2 || sc[d] > 2 || (i + 1) * w > len) return -1;
    *addr = fdt_cells(r + i * w, ac[d]);
    *size = sc[d] ? fdt_cells(r + i * w + 4 * ac[d], sc[d]) : 0;
    /* Up through the buses: each parent's "ranges" maps its children's
     * addresses to its own parent's (child, parent, size); none or empty:
     * the same addresses. */
    for (int k = d - 1; k >= 1; k--) {
        const uint8_t *rg = fdt_prop(f, anc[k], "ranges", &len);
        int ca = ac[k + 1], pa = ac[k], cs = sc[k + 1], e = 4 * (ca + pa + cs);
        if (!rg || !len || ca < 1 || ca > 2 || pa < 1 || pa > 2 || cs < 1 || cs > 2) continue;
        for (int j = 0; (j + 1) * e <= len; j++) {
            uint64_t c = fdt_cells(rg + j * e, ca), p = fdt_cells(rg + j * e + 4 * ca, pa),
                     s = fdt_cells(rg + j * e + 4 * (ca + pa), cs);
            if (*addr >= c && *addr - c < s) { *addr = p + (*addr - c); break; }
        }
    }
    return 0;
}

int fdt_spi(const void *f, int node, int i, int *spi, int *flags)
{
    int len;
    const uint8_t *p = fdt_prop(f, node, "interrupts", &len);   /* the GIC's 3 cells: type, number, flags */
    if (!p || (i + 1) * 12 > len || fdt_be32(p + i * 12) != 0) return -1;
    *spi = (int)fdt_be32(p + i * 12 + 4);
    *flags = (int)fdt_be32(p + i * 12 + 8);
    return 0;
}

/* The address a device under `node` uses for CPU physical address pa ("bus
 * address"): each ancestor's "dma-ranges" (child, parent, size) maps its
 * parent's addresses to its children's, from the root down. None or empty:
 * the same address. (The Raspberry Pi 4's firmware sees RAM at 0xC0000000.) */
int fdt_bus_addr(const void *f, int node, uint64_t pa, uint64_t *bus)
{
    int anc[MAXDEPTH], ac[MAXDEPTH + 1], sc[MAXDEPTH + 1], len, d = ancestry(f, node, anc, ac, sc);
    if (d < 0) return -1;
    for (int k = 1; k < d; k++) {
        const uint8_t *rg = fdt_prop(f, anc[k], "dma-ranges", &len);
        int ca = ac[k + 1], pc = ac[k], cs = sc[k + 1], e = 4 * (ca + pc + cs);
        if (!rg || !len || ca < 1 || ca > 2 || pc < 1 || pc > 2 || cs < 1 || cs > 2) continue;
        int j;
        for (j = 0; (j + 1) * e <= len; j++) {
            uint64_t c = fdt_cells(rg + j * e, ca), p = fdt_cells(rg + j * e + 4 * ca, pc),
                     s = fdt_cells(rg + j * e + 4 * (ca + pc), cs);
            if (pa >= p && pa - p < s) { pa = c + (pa - p); break; }
        }
        if ((j + 1) * e > len) return -1;          /* not reachable by this bus */
    }
    *bus = pa;
    return 0;
}

/* 1 if the device sees the CPU's caches ("dma-coherent" on it or a parent
 * bus): then no cache maintenance is needed around its transfers. */
int fdt_dma_coherent(const void *f, int node)
{
    int anc[MAXDEPTH], ac[MAXDEPTH + 1], sc[MAXDEPTH + 1], d = ancestry(f, node, anc, ac, sc);
    if (fdt_prop(f, node, "dma-coherent", 0)) return 1;
    for (int k = d - 1; k >= 1; k--) if (fdt_prop(f, anc[k], "dma-coherent", 0)) return 1;
    return 0;
}
