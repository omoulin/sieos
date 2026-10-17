/*
 * fat.c - Just enough FAT32 for the installer (install.c): put SIEOS's boot
 * files in \EFI\SIEOS\ of a drive's EFI System Partition, next to the other
 * systems' files, without touching any of them; or format a new one.
 *
 * The whole FAT is read into memory (both copies must agree, else the
 * partition is refused: it needs a check first), changed there, and only
 * its changed sectors are written back, to every copy. Names are short
 * (8.3, upper case: no long-name entries are written; those of other
 * files are skipped when reading). The FSInfo sector's free count is set
 * to the exact count.
 *
 * What may be written is checked here, besides the installer's sector
 * ranges (any other write ends the program, before it reaches the drive):
 *   - a cluster's contents: only clusters this run owns, "mine": the ones
 *     it found free and took, and those of \EFI\SIEOS\ and its files (SIEOS's
 *     own, from an earlier install);
 *   - a FAT entry: only of a cluster it owns, or a directory's last cluster
 *     (end of chain) when that directory grows by one cluster of its own;
 *   - another directory's cluster (\EFI\, or the root): one 32-byte entry
 *     that was free, and nothing else in that cluster (compared).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "install.h"

#define EOC 0x0FFFFFFFu
static uint64_t p0, psize;                   /* the partition: first drive sector, drive sectors */
static uint32_t bps, spc, rsvd, nfats, fatsz, rootc, nclus, csize, k;   /* k: drive sectors per FAT sector */
static uint64_t data0;                       /* the first data sector (FAT sectors, from the partition's start) */
static uint32_t *fat;                        /* the FAT, in memory */
static uint8_t *dirty, *mine;                /* changed FAT sectors; clusters this run owns (bitmaps) */
static uint8_t *cbuf;                        /* one cluster */
static uint16_t ftime, fdate;                /* now, as FAT stores it */

#define BIT(m, i) (m[(i) >> 3] >> ((i) & 7) & 1)
#define SET(m, i) (m[(i) >> 3] |= 1 << ((i) & 7))

static int fsec_rd(uint64_t s, uint32_t n, void *b) { return disk_rd(p0 + s * k, (uint64_t)n * k, b); }
static void fsec_wr(uint64_t s, uint32_t n, const void *b)
{
    if ((s + n) * k > psize) die("BUG: a FAT write past the EFI partition's end");
    disk_wr(p0 + s * k, (uint64_t)n * k, b);
}
static int clus_rd(uint32_t c, void *b) { return fsec_rd(data0 + (uint64_t)(c - 2) * spc, spc, b); }
static void clus_wr(uint32_t c, const void *b)
{
    if (c < 2 || c >= nclus + 2 || !BIT(mine, c)) die("BUG: a write to a cluster of the EFI partition this install does not own");
    fsec_wr(data0 + (uint64_t)(c - 2) * spc, spc, b);
}
static void fat_set(uint32_t c, uint32_t v, int grow)
{
    if (c < 2 || c >= nclus + 2 || (!BIT(mine, c) && !(grow && (fat[c] & EOC) >= 0x0FFFFFF8)))
        die("BUG: a change to a FAT entry this install does not own");
    fat[c] = (fat[c] & ~EOC) | (v & EOC);
    SET(dirty, c * 4 / bps);
}
static uint32_t next(uint32_t c) { return fat[c] & EOC; }
static int last(uint32_t c) { return next(c) >= 0x0FFFFFF8; }
static int bad(uint32_t c) { return c < 2 || c >= nclus + 2; }

static uint32_t take(void)                   /* a free cluster, now ours, zeroed and ending a chain */
{
    static uint32_t from = 2;
    for (uint32_t c = from; c < nclus + 2; c++)
        if (!(fat[c] & EOC)) {
            from = c + 1;
            SET(mine, c);
            fat_set(c, EOC, 0);
            return c;
        }
    return 0;
}

/* The date and time of now, as FAT stores them (from date_str's "YYYY-MM-DD HH:MM"). */
static void stamp(void)
{
    char s[20];
    date_str(sys_time(0), s);
    int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + s[3] - '0';
    int v[4];
    for (int i = 0; i < 4; i++) v[i] = (s[5 + 3 * i] - '0') * 10 + s[6 + 3 * i] - '0';   /* month, day, hour, minute */
    fdate = y >= 1980 ? (uint16_t)((y - 1980) << 9 | v[0] << 5 | v[1]) : 0x21;
    ftime = (uint16_t)(v[2] << 11 | v[3] << 5);
}

int fat_to83(const char *s, char *n)        /* "KERNEL.BIN" -> "KERNEL  BIN" -> 0, or -1 */
{
    memset(n, ' ', 11);
    int i = 0, j = 0;
    for (; s[i] && s[i] != '.'; i++) { if (i == 8) return -1; n[i] = s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]; }
    if (!i) return -1;
    if (s[i] == '.') for (i++; s[i]; i++, j++) { if (j == 3) return -1; n[8 + j] = s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]; }
    return 0;
}

static uint32_t first_of(const uint8_t *e) { return (uint32_t)(e[20] | e[21] << 8) << 16 | (e[26] | e[27] << 8); }

/* Find name (8.3) in the directory at cluster dir: -> 1 (*e: its entry, *at: cluster, *ix: index), 0 not there. */
static int find(uint32_t dir, const char *name, uint8_t *e, uint32_t *at, int *ix)
{
    int per = csize / 32, n = 0;
    for (uint32_t c = dir; !bad(c) && n++ < 65536; c = next(c)) {
        if (clus_rd(c, cbuf)) die("cannot read the EFI partition");
        for (int i = 0; i < per; i++) {
            uint8_t *d = cbuf + 32 * i;
            if (!d[0]) return 0;
            if (d[0] == 0xE5 || d[11] == 0x0F || (d[11] & 8)) continue;     /* deleted, long name, label */
            if (!memcmp(d, name, 11)) { memcpy(e, d, 32); *at = c; *ix = i; return 1; }
        }
        if (last(c)) break;
    }
    return 0;
}

/* Write entry e into slot ix of cluster c of a directory: a cluster we own,
 * or another's, where only that slot may change and must have been free (or
 * held the same name: an entry of ours being replaced). */
static void put_entry(uint32_t c, int ix, const uint8_t *e)
{
    static uint8_t before[65536];
    if (clus_rd(c, cbuf)) die("cannot read the EFI partition");
    memcpy(before, cbuf, csize);
    uint8_t *d = cbuf + 32 * ix;
    if (!BIT(mine, c) && d[0] && d[0] != 0xE5 && memcmp(d, e, 11)) die("BUG: an entry of the EFI partition that is not free");
    memcpy(d, e, 32);
    if (!BIT(mine, c)) {                     /* another's cluster: exactly that slot changed */
        for (uint32_t i = 0; i < csize; i++)
            if (cbuf[i] != before[i] && (i < 32u * ix || i >= 32u * ix + 32)) die("BUG: more than one entry changed");
        SET(mine, c);                        /* (allowed for this one write) */
        clus_wr(c, cbuf);
        mine[c >> 3] &= ~(1 << (c & 7));
    } else clus_wr(c, cbuf);
}

/* A free slot in directory dir (growing it by a cluster of ours if full). */
static void add_entry(uint32_t dir, const uint8_t *e)
{
    int per = csize / 32, n = 0;
    uint32_t c = dir;
    for (; !bad(c) && n++ < 65536; c = next(c)) {
        if (clus_rd(c, cbuf)) die("cannot read the EFI partition");
        for (int i = 0; i < per; i++)
            if (!cbuf[32 * i] || cbuf[32 * i] == 0xE5) {
                put_entry(c, i, e);
                return;
            }
        if (last(c)) break;
    }
    if (bad(c)) die("a directory of the EFI partition is damaged");
    uint32_t nc = take();
    if (!nc) die("the EFI partition is full");
    memset(cbuf, 0, csize);
    clus_wr(nc, cbuf);
    fat_set(c, nc, 1);                       /* (the directory's end: now our cluster) */
    memcpy(cbuf, e, 32);
    clus_wr(nc, cbuf);
}

static void entry(uint8_t *e, const char *name, uint8_t attr, uint32_t c, uint32_t size)
{
    memset(e, 0, 32);
    memcpy(e, name, 11);
    e[11] = attr;
    e[14] = ftime; e[15] = ftime >> 8; e[16] = fdate; e[17] = fdate >> 8;   /* created */
    e[18] = fdate; e[19] = fdate >> 8;                                     /* accessed */
    e[20] = c >> 16; e[21] = c >> 24; e[22] = ftime; e[23] = ftime >> 8;   /* written */
    e[24] = fdate; e[25] = fdate >> 8; e[26] = c; e[27] = c >> 8;
    e[28] = size; e[29] = size >> 8; e[30] = size >> 16; e[31] = size >> 24;
}

static void own_chain(uint32_t c)            /* SIEOS's own (an earlier install's): ours to change */
{
    for (int n = 0; !bad(c) && n < (int)nclus; n++) { SET(mine, c); if (last(c)) break; c = next(c); }
}
static void free_chain(uint32_t c)
{
    while (!bad(c)) { uint32_t nx = next(c), end = last(c); fat_set(c, 0, 0); if (end) break; c = nx; }
}

/* ---- What the installer calls. */

/* The FAT32 file system on sectors [first, first + n): -> 0, or what is wrong. */
const char *fat_open(uint64_t first, uint64_t n, uint32_t sector)
{
    static uint8_t b[4096];
    p0 = first; psize = n; k = 1;
    if (disk_rd(p0, 1, b)) return "cannot read it";
    bps = b[11] | b[12] << 8; spc = b[13]; rsvd = b[14] | b[15] << 8; nfats = b[16];
    uint32_t tot = b[32] | b[33] << 8 | b[34] << 16 | (uint32_t)b[35] << 24;
    fatsz = b[36] | b[37] << 8 | b[38] << 16 | (uint32_t)b[39] << 24;
    rootc = b[44] | b[45] << 8 | b[46] << 16 | (uint32_t)b[47] << 24;
    if (b[510] != 0x55 || b[511] != 0xAA || (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) || bps < sector ||
        !spc || (spc & (spc - 1)) || !rsvd || !nfats || nfats > 2)
        return "not a FAT file system";
    if ((b[17] | b[18]) || (b[22] | b[23]) || !fatsz) return "FAT12 or FAT16, not FAT32 (not supported)";
    k = bps / sector;
    csize = bps * spc;
    data0 = rsvd + (uint64_t)nfats * fatsz;
    if ((uint64_t)tot * k > psize || tot <= data0) return "its size does not fit the partition";
    nclus = (uint32_t)((tot - data0) / spc);
    if (nclus < 65525 || (uint64_t)(nclus + 2) * 4 > (uint64_t)fatsz * bps || bad(rootc)) return "not a valid FAT32 file system";
    free(fat); free(dirty); free(mine); free(cbuf);
    uint32_t *fat2 = malloc((size_t)fatsz * bps);
    fat = malloc((size_t)fatsz * bps);
    dirty = zalloc(fatsz / 8 + 1);
    mine = zalloc((nclus + 2) / 8 + 1);
    cbuf = malloc(csize);
    if (!fat || !fat2 || !dirty || !mine || !cbuf || csize > 65536) return "not enough memory (or clusters above 64 KiB)";
    int e = fsec_rd(rsvd, fatsz, fat);
    for (uint32_t i = 1; !e && i < nfats; i++)
        if ((e = fsec_rd(rsvd + (uint64_t)i * fatsz, fatsz, fat2)) || memcmp(fat, fat2, (size_t)fatsz * bps)) e = 1;
    free(fat2);
    if (e) return "its two FAT copies differ (check it with fsck.vfat from Linux first)";
    stamp();
    return 0;
}

/* The directory name (8.3) in dir, made if missing -> its cluster (0: a file has that name). */
uint32_t fat_dir(uint32_t dir, const char *name83, int ours)
{
    uint8_t e[32];
    uint32_t at;
    int ix;
    if (!dir) dir = rootc;
    if (find(dir, name83, e, &at, &ix)) {
        if (!(e[11] & 0x10)) return 0;
        if (ours) own_chain(first_of(e));
        return first_of(e);
    }
    uint32_t c = take();
    if (!c) die("the EFI partition is full");
    memset(cbuf, 0, csize);                  /* ".", ".." (0: the root) */
    entry(cbuf, ".          ", 0x10, c, 0);
    entry(cbuf + 32, "..         ", 0x10, dir == rootc ? 0 : dir, 0);
    clus_wr(c, cbuf);
    entry(e, name83, 0x10, c, 0);
    add_entry(dir, e);
    return c;
}

/* The file name83 in our directory dir: data, len (replaces an older one of that name). */
void fat_put(uint32_t dir, const char *name83, const void *data, uint32_t len)
{
    uint8_t e[32];
    uint32_t at, first = 0, prev = 0;
    int ix, had = find(dir, name83, e, &at, &ix);
    if (had) {                               /* (in our directory: an earlier install's file) */
        if (e[11] & 0x10) die("a folder of that name is in the way in \\EFI\\SIEOS");
        own_chain(first_of(e));
        free_chain(first_of(e));
    }
    for (uint32_t done = 0; done < len; done += csize) {
        uint32_t c = take();
        if (!c) die("the EFI partition is full");
        uint32_t n = len - done < csize ? len - done : csize;
        memset(cbuf, 0, csize);
        memcpy(cbuf, (const uint8_t *)data + done, n);
        clus_wr(c, cbuf);
        if (prev) fat_set(prev, c, 0); else first = c;
        prev = c;
    }
    entry(e, name83, 0x20, first, len);
    if (had) put_entry(at, ix, e); else add_entry(dir, e);
}

/* Read the file name83 of dir back (to compare): -> its length, or -1. */
long fat_get(uint32_t dir, const char *name83, void *buf, uint32_t cap)
{
    uint8_t e[32];
    uint32_t at, done = 0;
    int ix;
    if (!find(dir, name83, e, &at, &ix)) return -1;
    uint32_t len = e[28] | e[29] << 8 | e[30] << 16 | (uint32_t)e[31] << 24;
    if (len > cap) return -1;
    for (uint32_t c = first_of(e); done < len; c = next(c)) {
        if (bad(c) || clus_rd(c, cbuf)) return -1;
        uint32_t n = len - done < csize ? len - done : csize;
        memcpy((uint8_t *)buf + done, cbuf, n);
        done += n;
        if (last(c) && done < len) return -1;
    }
    return len;
}

/* The changed FAT sectors, to every copy; the FSInfo sector's free count. */
void fat_flush(void)
{
    for (uint32_t s = 0; s < fatsz; s++)
        if (BIT(dirty, s))
            for (uint32_t i = 0; i < nfats; i++) fsec_wr(rsvd + (uint64_t)i * fatsz + s, 1, (uint8_t *)fat + (uint64_t)s * bps);
    memset(dirty, 0, fatsz / 8 + 1);
    static uint8_t b[4096];
    uint32_t fsi = 0, nfree = 0;
    if (fsec_rd(0, 1, b)) return;
    fsi = b[48] | b[49] << 8;
    if (!fsi || fsi >= rsvd || fsec_rd(fsi, 1, b) || memcmp(b, "RRaA", 4) || memcmp(b + 484, "rrAa", 4)) return;
    for (uint32_t c = 2; c < nclus + 2; c++) nfree += !(fat[c] & EOC);
    b[488] = nfree; b[489] = nfree >> 8; b[490] = nfree >> 16; b[491] = nfree >> 24;
    fsec_wr(fsi, 1, b);
}

/* A new FAT32 file system on [first, first + n) (a new partition: all ours). */
void fat_format(uint64_t first, uint64_t n, uint32_t sector, const char *label)
{
    static uint8_t b[4096];
    uint32_t s = sector, sc = 4096 / s, rs = 32, tot = (uint32_t)n;   /* 4 KiB clusters */
    uint32_t cl = (tot - rs) / sc, fs = (cl * 4 + 8 + s - 1) / s;        /* (a few clusters too many: harmless) */
    memset(b, 0, s);
    memcpy(b, "\xEB\x58\x90SIEOS   ", 11);
    b[11] = s; b[12] = s >> 8; b[13] = sc; b[14] = rs; b[16] = 2; b[21] = 0xF8;
    b[24] = 63; b[26] = 255;                                   /* (geometry: unused) */
    for (int i = 0; i < 4; i++) { b[32 + i] = tot >> 8 * i; b[36 + i] = fs >> 8 * i; }
    b[44] = 2; b[48] = 1; b[50] = 6; b[64] = 0x80; b[66] = 0x29;
    sys_random(b + 67, 4);
    memset(b + 71, ' ', 11); memcpy(b + 71, label, strlen(label) < 11 ? strlen(label) : 11);
    memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    disk_wr(first, 1, b);
    disk_wr(first + 6, 1, b);                                  /* the backup boot sector */
    memset(b, 0, s);
    memcpy(b, "RRaA", 4); memcpy(b + 484, "rrAa", 4);
    memset(b + 488, 0xFF, 8);
    b[510] = 0x55; b[511] = 0xAA;
    disk_wr(first + 1, 1, b);
    disk_wr(first + 7, 1, b);
    static uint8_t z[DISK_MAX];                                /* the FATs and the root cluster: zero */
    memset(z, 0, sizeof z);
    for (uint64_t at = rs, end = rs + 2ULL * fs + sc; at < end; ) {
        uint64_t m = end - at < DISK_MAX / s ? end - at : DISK_MAX / s;
        disk_wr(first + at, m, z);
        at += m;
    }
    uint32_t head[3] = { 0x0FFFFFF8, EOC, EOC };               /* media, reserved, the root's one cluster */
    memcpy(z, head, sizeof head);
    disk_wr(first + rs, 1, z);
    disk_wr(first + rs + fs, 1, z);
}
