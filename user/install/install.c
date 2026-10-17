/*
 * install - Put SIEOS on a PC's NVMe drive (docs/install.md), from the
 * system started from the USB key. The disk work only: the desktop's
 * Install window (user/atlas/inst.c) runs this program and shows what it
 * prints; it also works on a terminal, as root.
 *
 *   install list              the drives: model, size, partitions, free space
 *   install [-n] free D N     into free extent N of drive D: one partition
 *                             added to its GPT, nothing else moved or resized
 *   install [-n] whole D      erase drive D: a new GPT, an EFI partition, SIEOS
 *   -n                        print the plan only: nothing is written
 *
 * Before writing it prints the plan (exactly what changes) and asks: "yes"
 * for free space; for a whole drive, its model name typed exactly. Then:
 *   1. the new SieFS partition's file system, with a fresh system: the
 *      snapshot "factory" of the USB key's disk (the image as built, the
 *      firmware files included; no accounts: the first start makes them);
 *      checked (siefs_check, every block read back);
 *   2. the EFI System Partition: \EFI\SIEOS\ (the loader SIEOSX64.EFI, the
 *      kernel, the modules, BOOT.CFG with root=GUID: the new partition's
 *      own GUID) and the request ADDENTRY: the loader adds "SIEOS" to the
 *      firmware's boot menu at the next start from the USB key (it has the
 *      firmware's services; SIEOS has not); the other files are untouched;
 *   3. the GPT (free space: last, so a cut before leaves the drive's table
 *      as it was): backup entries and header, then primary ones, CRCs
 *      recomputed; read back and compared.
 *
 * Safety: the drive is refused if it holds SIEOS's own disk, if its GPT is
 * not valid in every way (both headers, both entry arrays identical, CRCs,
 * a purely protective MBR), or if its table has no free entry. Every write
 * goes through disk_wr, which checks it against the ranges the plan
 * declared (the new partition, the GPT's own sectors, the EFI partition)
 * and stops the program before anything outside them; fat.c checks the
 * EFI partition's writes finer still.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "mk.h"
#include "siefs.h"
#include "mk/gpt.h"
#include "install.h"

#define SRC     "/.snapshots/factory"            /* the system copied (mkusb.py's snapshot) */
#define MIN_GIB 4                                /* the smallest SieFS partition */
#define ESP_MIB 300                              /* a whole drive's new EFI partition */
static const uint8_t T_SIEFS[16] = GPT_SIEFS_TYPE;
static const uint8_t T_ESP[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };

static long dport;
static int drive, dry;
static disk_raw_t di;
static uint32_t bs, esect;                       /* sector size; sectors of a GPT entry array */
static uint64_t ns, mib;                         /* the drive's sectors; sectors per MiB */
static char buf[128 << 10];

void die(const char *why) { printf("error: %s\n", why); sys_exit(1); }
void *zalloc(uint64_t n) { void *p = malloc(n); if (!p) die("out of memory"); memset(p, 0, n); return p; }

/* ---- The drive's sectors (vblk's DISK_RAW_*), and the write gate. */
static long raw(int op, uint64_t lba, uint32_t n, void *b)
{
    msg_t m = { .w = { op, (uint64_t)drive, lba, n } };
    if (op == DISK_RAW_WRITE) { m.sbuf = b; m.slen = (uint64_t)n * bs; }
    else if (op == DISK_RAW_READ) { m.rbuf = b; m.rlen = (uint64_t)n * bs; }
    else if (op == DISK_RAW_INFO) { m.rbuf = b; m.rlen = sizeof(disk_raw_t); }
    long e = call_named(&dport, "disk", &m, 0);
    return e < 0 ? e : (long)m.w[0];
}
int disk_rd(uint64_t lba, uint64_t n, void *b)
{
    for (uint64_t c; n; n -= c, lba += c, b = (char *)b + c * bs)
        if (raw(DISK_RAW_READ, lba, (uint32_t)(c = n < DISK_MAX / bs ? n : DISK_MAX / bs), b)) return -1;
    return 0;
}
static struct { uint64_t lo, hi; } allow[6];
static int nallow;
static void may_write(uint64_t lo, uint64_t hi) { allow[nallow].lo = lo; allow[nallow++].hi = hi; }
void disk_wr(uint64_t lba, uint64_t n, const void *b)
{
    int ok = 0;
    for (int i = 0; i < nallow; i++) ok |= n && lba >= allow[i].lo && lba + n - 1 <= allow[i].hi && lba + n - 1 < ns;
    if (!ok || dry) {
        printf("error: BUG: a write to sectors %lu-%lu, outside what the plan allows: stopped before it\n",
               (unsigned long)lba, (unsigned long)(lba + n - 1));
        sys_exit(3);
    }
    for (uint64_t c; n; n -= c, lba += c, b = (const char *)b + c * bs)
        if (raw(DISK_RAW_WRITE, lba, (uint32_t)(c = n < DISK_MAX / bs ? n : DISK_MAX / bs), (void *)b)) die("a write to the drive failed");
}

/* ---- Text. */
static int unum(char *p, uint64_t v)             /* decimal -> its length */
{
    char t[24];
    int n = 0, k = 0;
    do t[n++] = '0' + v % 10; while (v /= 10);
    while (n) p[k++] = t[--n];
    p[k] = 0;
    return k;
}
static void size_str(char *s, uint64_t bytes)    /* "4.5 GiB", "300 MiB" */
{
    uint64_t m = bytes >> 20;
    if (m < 1024) { s += unum(s, m); strlcpy(s, " MiB", 5); return; }
    uint64_t t = (bytes * 10 + (1ULL << 29)) >> 30;
    s += unum(s, t / 10); *s++ = '.'; s += unum(s, t % 10); strlcpy(s, " GiB", 5);
}
static void guid_str(char *s, const uint8_t *g)  /* on-disk bytes -> text */
{
    static const uint8_t at[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) *s++ = '-';
        *s++ = "0123456789abcdef"[g[at[i]] >> 4]; *s++ = "0123456789abcdef"[g[at[i]] & 15];
    }
    *s = 0;
}
static const char *type_name(const uint8_t *t)
{
    static const struct { uint8_t g[4]; const char *name; } k[] = {   /* (the first 4 bytes tell them apart) */
        { { 0x28, 0x73, 0x2a, 0xc1 }, "EFI system" }, { { 0xa2, 0xa0, 0xd0, 0xeb }, "Microsoft basic data" },
        { { 0x16, 0xe3, 0xc9, 0xe3 }, "Microsoft reserved" }, { { 0xa4, 0xbb, 0x94, 0xde }, "Windows recovery" },
        { { 0xaf, 0x3d, 0xc6, 0x0f }, "Linux file system" }, { { 0x79, 0xd3, 0xd6, 0xe6 }, "Linux LVM" },
        { { 0xcb, 0x7c, 0x7d, 0xca }, "Linux LUKS" }, { { 0x6d, 0xfd, 0x57, 0x06 }, "Linux swap" },
        { { 0xff, 0xc2, 0x13, 0xbc }, "Linux /boot" }, { { 0x00, 0x05, 0x1e, 0x5e }, "SieFS (SIEOS)" } };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) if (!memcmp(t, k[i].g, 4)) return k[i].name;
    return "other";
}


/* ---- The GPT: read, checked in every way; the partitions in order; the gaps. */
static gpt_header_t h1, h2;
static uint8_t *ents, *ents2, *orig;             /* primary, backup, as first read */
static char why[100];                            /* why the drive's table cannot be used */
typedef struct { uint64_t first, last; int idx; } span_t;
static span_t parts[256], gaps[64];
static int nparts, ngaps, free_slot;

static int hdr_ok(gpt_header_t *h, uint64_t self, uint64_t alt)
{
    gpt_header_t c = *h;
    c.hcrc = 0;
    return !memcmp(h->sig, "EFI PART", 8) && h->hsize == 92 && gpt_crc(&c, 92) == h->hcrc && h->self == self &&
           h->alt == alt && h->entsize == 128 && h->nentries && h->nentries <= 1024 &&
           h->first_usable <= h->last_usable && h->last_usable < ns - 1;
}
static int read_gpt(void)
{
    static uint8_t s[4096];
    why[0] = 0; nparts = ngaps = 0; free_slot = -1;
    if (disk_rd(0, 1, s)) { strlcpy(why, "cannot read it", sizeof why); return -1; }
    int pmbr = 0, other = 0;
    for (int i = 0; i < 4; i++) { uint8_t t = s[446 + 16 * i + 4]; pmbr += t == 0xEE; other += t && t != 0xEE; }
    if (s[510] != 0x55 || s[511] != 0xAA || !pmbr) {
        strlcpy(why, s[510] == 0x55 && s[511] == 0xAA ? "an MBR partition table, not a GPT" : "no partition table", sizeof why);
        return -1;
    }
    if (other || pmbr > 1) { strlcpy(why, "a hybrid MBR beside the GPT", sizeof why); return -1; }
    if (disk_rd(1, 1, s)) { strlcpy(why, "cannot read it", sizeof why); return -1; }
    memcpy(&h1, s, sizeof h1);
    if (!hdr_ok(&h1, 1, ns - 1)) { strlcpy(why, "its GPT header is not valid (or its backup is not at the drive's end)", sizeof why); return -1; }
    esect = (h1.nentries * 128 + bs - 1) / bs;
    free(ents); free(ents2); free(orig);
    ents = zalloc((uint64_t)esect * bs); ents2 = zalloc((uint64_t)esect * bs); orig = zalloc((uint64_t)esect * bs);
    if (disk_rd(ns - 1, 1, s)) { strlcpy(why, "cannot read it", sizeof why); return -1; }
    memcpy(&h2, s, sizeof h2);
    if (h1.entries_lba < 2 || h1.entries_lba + esect > h1.first_usable || !hdr_ok(&h2, ns - 1, 1) ||
        h2.entries_lba <= h1.last_usable || h2.entries_lba + esect > ns - 1 ||
        disk_rd(h1.entries_lba, esect, ents) || disk_rd(h2.entries_lba, esect, ents2) ||
        gpt_crc(ents, h1.nentries * 128) != h1.entcrc || memcmp(ents, ents2, (uint64_t)esect * bs) ||
        memcmp(h1.guid, h2.guid, 16) || h1.nentries != h2.nentries || h1.entcrc != h2.entcrc ||
        h1.first_usable != h2.first_usable || h1.last_usable != h2.last_usable) {
        strlcpy(why, "its GPT is damaged, or its two copies differ", sizeof why);
        return -1;
    }
    memcpy(orig, ents, (uint64_t)esect * bs);
    static const uint8_t zero[16];
    for (uint32_t i = 0; i < h1.nentries; i++) {
        gpt_entry_t *e = (gpt_entry_t *)(ents + 128 * i);
        if (!memcmp(e->type, zero, 16)) { if (free_slot < 0) free_slot = (int)i; continue; }
        if (e->first < h1.first_usable || e->last > h1.last_usable || e->last < e->first) {
            strlcpy(why, "a partition lies outside the usable sectors", sizeof why);
            return -1;
        }
        int j = nparts++;                         /* (in order of their first sector) */
        while (j && parts[j - 1].first > e->first) { parts[j] = parts[j - 1]; j--; }
        parts[j].first = e->first; parts[j].last = e->last; parts[j].idx = (int)i;
    }
    uint64_t at = h1.first_usable;
    for (int i = 0; i <= nparts; i++) {
        uint64_t end = i < nparts ? parts[i].first : h1.last_usable + 1;
        if (i < nparts && i && parts[i].first <= parts[i - 1].last) { strlcpy(why, "two partitions overlap", sizeof why); return -1; }
        uint64_t a = (at + mib - 1) / mib * mib, z = end / mib * mib;     /* whole MiB */
        if (z > a && z - a >= mib && ngaps < 64) { gaps[ngaps].first = a; gaps[ngaps].last = z - 1; ngaps++; }
        if (i < nparts && parts[i].last + 1 > at) at = parts[i].last + 1;
    }
    return 0;
}

static int open_drive(int d)
{
    drive = d;
    if (raw(DISK_RAW_INFO, 0, 0, &di)) return -1;
    bs = di.sector; ns = di.sectors; mib = (1 << 20) / bs;
    return 0;
}
static int big(span_t *g) { return (g->last - g->first + 1) * bs >= ((uint64_t)MIN_GIB << 30); }

static void list(void)
{
    char a[24], b[24], g[40];
    int any = 0;
    for (int d = 0; !open_drive(d); d++) {
        any = 1;
        size_str(a, ns * bs);
        printf("drive %d: %s (serial %s), %s, %u-byte sectors\n", d, di.model, di.serial, a, bs);
        if (di.self || di.busy) { printf("  refused: %s\n", di.self ? "SIEOS runs from this drive" : "SIEOS runs from an NVMe drive: start it from the USB key to install"); continue; }
        if (read_gpt()) { printf("  table: %s: its free space cannot be used\n", why); printf("  whole ok\n"); continue; }
        guid_str(g, h1.guid);
        printf("  table: GPT %s, %d partitions, %u entries\n", g, nparts, h1.nentries);
        for (int i = 0; i < nparts; i++) {
            gpt_entry_t *e = (gpt_entry_t *)(ents + 128 * parts[i].idx);
            char nm[37];
            for (int k = 0; k < 36; k++) nm[k] = e->name[k] < 128 ? (char)e->name[k] : '?';
            nm[36] = 0;
            size_str(a, (e->last - e->first + 1) * bs); size_str(b, e->first * bs);
            printf("  part %d: %s, %s at %s, \"%s\"\n", parts[i].idx + 1, type_name(e->type), a, b, nm);
        }
        for (int i = 0; i < ngaps; i++) {
            size_str(a, (gaps[i].last - gaps[i].first + 1) * bs); size_str(b, gaps[i].first * bs);
            printf("  free %d %s: %s at %s%s\n", i + 1, !big(&gaps[i]) ? "small" : free_slot < 0 ? "full" : "ok", a, b,
                   !big(&gaps[i]) ? " (too small: SIEOS needs 4 GiB)" : free_slot < 0 ? " (no free entry in the table)" : "");
        }
        if (!ngaps) printf("  no free space\n");
        printf("  whole ok\n");
    }
    if (!any) printf("no NVMe drive\n");
}

/* ---- What is copied: the factory snapshot, counted first. */
static uint64_t nfiles, nbytes, done_bytes;
static int pct = -1;
static void progress(void)
{
    int p = nbytes ? (int)(10 + done_bytes * 80 / nbytes) : 50;
    if (p / 5 != pct / 5) printf("progress %d\n", p);
    pct = p;
}
static void join(char *out, const char *dir, const char *name)
{
    strlcpy(out, dir, 1024);
    strlcpy(out + strlen(out), "/", 1024 - strlen(out));
    strlcpy(out + strlen(out), name, 1024 - strlen(out));
}
/* Walk dir: each entry to fn (count, or copy). */
typedef int (*visit_t)(const char *path, const char *name, siefs_stat_t *st, void *ctx);
static int walk(const char *dir, visit_t fn, void *ctx)
{
    long h = fs_open(dir, FS_RDONLY | FS_DIRECTORY, 0);
    if (h < 0) return (int)h;
    static char d[4096];
    char name[256], path[1024];
    uint64_t cur = 0, ino;
    long n;
    int type, e = 0;
    while (!e && (n = fs_readdir(h, &cur, d, sizeof d)) > 0) {
        char *copy = malloc(n);                   /* (fn recurses: d is reused) */
        if (!copy) { e = -ENOMEM; break; }
        memcpy(copy, d, n);
        for (const char *p = copy; !e && fs_dirent(&p, copy + n, &ino, &type, name); ) {
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            siefs_stat_t st;
            join(path, dir, name);
            e = fs_stat(path, &st, 1) ? -EIO : fn(path, name, &st, ctx);
        }
        free(copy);
    }
    fs_close(h);
    return e;
}
static int count(const char *path, const char *name, siefs_stat_t *st, void *ctx)
{
    (void)name;
    nfiles++;
    if ((st->mode & SIEFS_IFMT) == SIEFS_IFREG) nbytes += st->size;
    return (st->mode & SIEFS_IFMT) == SIEFS_IFDIR ? walk(path, count, ctx) : 0;
}

/* ---- The new SieFS, on [sp0, sp0 + sectors) of the drive. */
static uint64_t sp0;
static int s_rd(void *c, uint64_t blk, uint32_t n, void *b) { (void)c; return disk_rd(sp0 + blk * (4096 / bs), (uint64_t)n * (4096 / bs), b) ? -SIEFS_EIO : 0; }
static int s_wr(void *c, uint64_t blk, uint32_t n, const void *b) { (void)c; disk_wr(sp0 + blk * (4096 / bs), (uint64_t)n * (4096 / bs), b); return 0; }
static int s_flush(void *c) { (void)c; return raw(DISK_RAW_FLUSH, 0, 0, 0) ? -SIEFS_EIO : 0; }
static int64_t s_now(void) { return sys_time(0); }
static int s_rand(void *b, size_t n) { for (size_t i = 0; i < n; i += 4096) sys_random((char *)b + i, n - i < 4096 ? n - i : 4096); return 0; }
static void s_free(void *p) { free(p); }

static int copy(const char *path, const char *name, siefs_stat_t *st, void *ctx)
{
    siefs_t *fs = ((void **)ctx)[0];
    uint64_t dir = *(uint64_t *)((void **)ctx)[1], ino;
    uint32_t t = st->mode & SIEFS_IFMT;
    long e;
    if (t == SIEFS_IFLNK) {
        long n = fs_readlink(path, buf, sizeof buf - 1);
        if (n < 0) return (int)n;
        buf[n] = 0;
        return siefs_symlink(fs, dir, name, buf, st->uid, st->gid, &ino);
    }
    if (t != SIEFS_IFDIR && t != SIEFS_IFREG) return 0;
    if ((e = siefs_create(fs, dir, name, t | (st->mode & 07777), st->uid, st->gid, &ino))) return (int)e;
    long n = fs_listxattr(path, buf, sizeof buf);    /* its attributes ("a\0b\0") */
    for (char *a = buf; n > 0 && a < buf + n; a += strlen(a) + 1) {
        static char v[SIEFS_XVAL_MAX];
        long l = fs_getxattr(path, a, v, sizeof v);
        if (l >= 0 && (e = siefs_setxattr(fs, ino, a, v, l))) return (int)e;
    }
    if (t == SIEFS_IFDIR) {
        void *sub[2] = { fs, &ino };
        return walk(path, copy, sub);
    }
    long h = fs_open(path, FS_RDONLY, 0);
    if (h < 0) return (int)h;
    for (uint64_t off = 0; ; off += n) {
        if ((n = fs_read(h, off, buf, sizeof buf)) <= 0) break;
        if ((e = siefs_write(fs, ino, off, buf, n)) < 0) { n = e; break; }
        done_bytes += n;
        progress();
    }
    fs_close(h);
    return n < 0 ? (int)n : 0;
}

static void problem(void *c, const char *m) { (void)c; printf("  siefs check: %s\n", m); }

static void make_siefs(uint64_t first, uint64_t last)
{
    sp0 = first;
    siefs_env_t env = { .nblocks = (last - first + 1) * bs / 4096, .read = s_rd, .write = s_wr, .flush = s_flush,
                        .alloc = malloc, .free = s_free, .now = s_now, .random = s_rand };
    siefs_t *fs;
    int e;
    printf("step: formatting the SieFS partition\n");
    if ((e = siefs_format(&env, "SIEOS")) || (e = siefs_mount(&env, &fs))) die("cannot make the SieFS file system");
    printf("step: copying the system (%lu files, %lu MiB)\n", (unsigned long)nfiles, (unsigned long)(nbytes >> 20));
    uint64_t root = SIEFS_ROOT;
    void *ctx[2] = { fs, &root };
    if ((e = walk(SRC, copy, ctx))) { printf("error: copying the system: error %d\n", -e); sys_exit(1); }
    if ((e = siefs_unmount(fs))) die("cannot write the SieFS file system");
    printf("step: checking the new file system (every block read back)\n");
    if (siefs_mount(&env, &fs)) die("the new SieFS file system does not mount");
    long bad = siefs_check(fs, 1, problem, 0);
    siefs_statfs_t sf;
    siefs_statfs(fs, &sf);
    siefs_unmount(fs);
    if (bad) die("the new file system has problems (above)");
    printf("check: SieFS clean, %lu objects, %lu MiB free\n", (unsigned long)sf.inodes, (unsigned long)(sf.free >> 8));
    printf("progress 90\n");
}

/* ---- The EFI partition's files: \EFI\SIEOS\. Read back and compared. */
static const char *bootfiles(char names[][16], char *cfg, uint64_t *size)
{
    char *c = file_get(SRC "/boot/boot.cfg", 0);
    if (!c) return "no /boot/boot.cfg in the factory snapshot";
    int n = 0;
    strlcpy(names[n++], "loader.efi", 16); strlcpy(names[n++], "kernel.bin", 16);
    cfg[0] = 0;
    for (char *l = c, *e; *l; l = e + (*e != 0)) {
        for (e = l; *e && *e != '\n'; e++) ;
        char sv = *e; *e = 0;
        if (!memcmp(l, "module=", 7) && n < 18) strlcpy(names[n++], l + 7, 16);
        if (memcmp(l, "root=", 5)) { strlcpy(cfg + strlen(cfg), l, 1024 - strlen(cfg)); strlcpy(cfg + strlen(cfg), "\n", 1024 - strlen(cfg)); }
        *e = sv;
    }
    free(c);
    names[n][0] = 0;
    *size = 0;
    for (int i = 0; names[i][0]; i++) {
        char p[64], n83[11];
        siefs_stat_t st;
        join(p, SRC "/boot", names[i]);
        if (fs_stat(p, &st, 0) || fat_to83(i ? names[i] : "SIEOSX64.EFI", n83)) return "a boot file is missing in /boot";
        *size += st.size;
    }
    return 0;
}
static void put_esp(char names[][16], const char *cfg, const uint8_t *guid)
{
    char n83[11], g[40], full[1100];
    printf("step: writing \\EFI\\SIEOS\\ in the EFI partition\n");
    uint32_t efi = fat_dir(0, "EFI        ", 0);
    if (!efi) die("\\EFI is a file in the EFI partition");
    uint32_t dir = fat_dir(efi, "SIEOS      ", 1);
    if (!dir) die("\\EFI\\SIEOS is a file in the EFI partition");
    guid_str(g, guid);
    strlcpy(full, cfg, sizeof full);
    strlcpy(full + strlen(full), "root=", sizeof full - strlen(full));
    strlcpy(full + strlen(full), g, sizeof full - strlen(full));
    strlcpy(full + strlen(full), "\n", sizeof full - strlen(full));
    static char check[1 << 20];
    for (int i = 0; names[i][0]; i++) {
        char p[64];
        size_t len;
        join(p, SRC "/boot", names[i]);
        char *d = file_get(p, &len);
        if (!d || len > sizeof check) die("cannot read a boot file");
        fat_to83(i ? names[i] : "SIEOSX64.EFI", n83);
        fat_put(dir, n83, d, (uint32_t)len);
        free(d);
    }
    fat_to83("BOOT.CFG", n83);
    fat_put(dir, n83, full, strlen(full));
    fat_to83("ADDENTRY", n83);
    fat_put(dir, n83, "", 0);
    fat_flush();
    raw(DISK_RAW_FLUSH, 0, 0, 0);
    for (int i = 0; names[i][0]; i++) {          /* read back */
        char p[64];
        size_t len;
        join(p, SRC "/boot", names[i]);
        char *d = file_get(p, &len);
        fat_to83(i ? names[i] : "SIEOSX64.EFI", n83);
        if (!d || fat_get(dir, n83, check, sizeof check) != (long)len || memcmp(d, check, len)) die("a boot file reads back wrong");
        free(d);
    }
    fat_to83("BOOT.CFG", n83);
    if (fat_get(dir, n83, check, sizeof check) != (long)strlen(full) || memcmp(check, full, strlen(full))) die("BOOT.CFG reads back wrong");
    printf("check: \\EFI\\SIEOS\\ read back: the same\n");
}

/* ---- The GPT written: backup first, primary last; read back. */
static void entry_of(gpt_entry_t *e, const uint8_t *type, uint64_t first, uint64_t last, const char *name)
{
    memset(e, 0, sizeof *e);
    memcpy(e->type, type, 16);
    sys_random(e->guid, 16);
    e->guid[7] = 0x40 | (e->guid[7] & 15);       /* (version 4, variant 1) */
    e->guid[8] = 0x80 | (e->guid[8] & 0x3F);
    e->first = first; e->last = last;
    for (int i = 0; name[i]; i++) e->name[i] = (uint8_t)name[i];
}
static void write_gpt(void)
{
    static uint8_t s[4096];
    h1.entcrc = h2.entcrc = gpt_crc(ents, h1.nentries * 128);
    h1.hcrc = h2.hcrc = 0;
    h1.hcrc = gpt_crc(&h1, 92);
    h2.hcrc = gpt_crc(&h2, 92);
    printf("step: writing the partition table (backup, then primary)\n");
    disk_wr(h2.entries_lba, esect, ents);
    memset(s, 0, bs); memcpy(s, &h2, 92);
    disk_wr(ns - 1, 1, s);
    disk_wr(h1.entries_lba, esect, ents);
    memset(s, 0, bs); memcpy(s, &h1, 92);
    disk_wr(1, 1, s);
    if (raw(DISK_RAW_FLUSH, 0, 0, 0)) die("the drive did not flush");
}
static void check_gpt(const uint8_t *want)
{
    gpt_header_t a = h1, b = h2;
    uint8_t *w = zalloc((uint64_t)esect * bs);
    memcpy(w, want, (uint64_t)esect * bs);
    if (read_gpt() || memcmp(&a, &h1, 92) || memcmp(&b, &h2, 92) || memcmp(w, ents, (uint64_t)esect * bs)) die("the partition table reads back wrong");
    free(w);
    printf("check: the partition table reads back as written (both copies, CRCs valid)\n");
}

/* ---- The two ways. */
static int confirm(const char *prompt, const char *want)
{
    char b[128];
    printf("%s", prompt);
    long n = con_read(b, sizeof b - 1);
    if (n < 0) n = 0;
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) n--;
    b[n] = 0;
    return !strcmp(b, want);
}

static int install(int whole, int extent)
{
    char a[24], b[24], g[40], names[20][16], cfg[1024];
    uint64_t bootsize;
    if (di.self || di.busy) die(di.self ? "SIEOS runs from this drive" : "SIEOS runs from an NVMe drive: start it from the USB key to install");
    if (bs != 512 && bs != 4096) die("sectors of an unusual size");
    siefs_stat_t st;
    if (fs_stat(SRC, &st, 0)) die("this system has no snapshot \"factory\" to copy (start from a USB key made by make usb)");
    const char *w = bootfiles(names, cfg, &bootsize);
    if (w) die(w);
    nfiles = nbytes = 0;
    walk(SRC, count, 0);

    uint64_t sfirst, slast, efirst = 0, elast = 0;
    int eidx = 0;
    gpt_entry_t ne;
    if (!whole) {
        if (read_gpt()) die(why);
        if (extent < 1 || extent > ngaps) die("no such free space on that drive");
        span_t *gp = &gaps[extent - 1];
        if (!big(gp)) die("that free space is too small: SIEOS needs 4 GiB");
        if (free_slot < 0) die("the partition table has no free entry");
        for (int i = 0; i < nparts; i++) {
            gpt_entry_t *e = (gpt_entry_t *)(ents + 128 * parts[i].idx);
            if (!memcmp(e->type, T_ESP, 16) && !efirst) { efirst = e->first; elast = e->last; eidx = parts[i].idx + 1; }
            if (!(gp->last < parts[i].first || gp->first > parts[i].last)) die("BUG: the free space overlaps a partition");
        }
        if (!efirst) die("this drive has no EFI System Partition (the firmware's boot menu needs one)");
        if ((w = fat_open(efirst, elast - efirst + 1, bs))) {
            printf("error: its EFI partition: %s\n", w);
            sys_exit(1);
        }
        sfirst = gp->first; slast = gp->last;
        entry_of(&ne, T_SIEFS, sfirst, slast, "SIEOS");
    } else {
        if (ns * bs < ((uint64_t)MIN_GIB << 30) + ((ESP_MIB + 2ULL) << 20)) die("the drive is too small: SIEOS needs 4.3 GiB");
        efirst = mib; elast = mib * (1 + ESP_MIB) - 1; eidx = 1;
        sfirst = elast + 1;
        slast = (ns - 2 - 16384 / bs) / mib * mib - 1;
        entry_of(&ne, T_SIEFS, sfirst, slast, "SIEOS");
    }

    /* The plan. */
    size_str(a, ns * bs);
    printf("plan: drive %d: %s (serial %s), %s\n", drive, di.model, di.serial, a);
    if (whole) {
        if (!read_gpt()) {
            printf("plan: ERASE THE WHOLE DRIVE: its %d partitions are lost:\n", nparts);
            for (int i = 0; i < nparts; i++) {
                gpt_entry_t *e = (gpt_entry_t *)(ents + 128 * parts[i].idx);
                size_str(b, (e->last - e->first + 1) * bs);
                printf("plan:   partition %d, %s, %s\n", parts[i].idx + 1, type_name(e->type), b);
            }
        } else printf("plan: ERASE THE WHOLE DRIVE (%s): everything on it is lost\n", why);
        printf("plan: a new GPT: partition 1, EFI system (FAT32, %d MiB); partition 2, SIEOS (SieFS)\n", ESP_MIB);
    }
    guid_str(g, ne.guid);
    size_str(a, (slast - sfirst + 1) * bs); size_str(b, sfirst * bs);
    printf("plan: %s partition %d \"SIEOS\" (SieFS): %s at %s, sectors %lu-%lu, GUID %s\n", whole ? "new" : "add",
           whole ? 2 : free_slot + 1, a, b, (unsigned long)sfirst, (unsigned long)slast, g);
    if (!whole)
        printf("plan: the GPT gets that one entry (both copies: sectors 1-%lu and %lu-%lu); no other partition changes\n",
               (unsigned long)(h1.entries_lba + esect - 1), (unsigned long)h2.entries_lba, (unsigned long)(ns - 1));
    printf("plan: EFI partition %d: \\EFI\\SIEOS\\ with the loader, kernel and modules (%lu KiB)%s\n", eidx,
           (unsigned long)(bootsize >> 10), whole ? "" : "; its other files are untouched");
    printf("plan: the system copied: %lu files, %lu MiB (no accounts: the first start makes them)\n",
           (unsigned long)nfiles, (unsigned long)(nbytes >> 20));
    printf("plan: firmware boot menu: \"SIEOS\" is added (after the others, not the default) at the next start from the USB key\n");
    if (dry) { printf("plan: dry run: nothing was written\n"); return 0; }
    if (!(whole ? confirm("Type the drive's model to erase it: ", di.model) : confirm("Type yes to install: ", "yes"))) {
        printf("error: not confirmed: nothing was written\n");
        return 1;
    }

    /* The writes, each range declared first. */
    printf("progress 5\n");
    if (whole) {
        may_write(0, ns - 1);
        esect = 16384 / bs;                      /* (128 entries) */
        static uint8_t s[4096];
        memset(s, 0, bs);                        /* the protective MBR */
        s[446 + 2] = 2; s[446 + 4] = 0xEE; s[446 + 5] = s[446 + 6] = s[446 + 7] = 0xFF;
        uint32_t n = ns - 1 > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)(ns - 1);
        s[446 + 8] = 1; for (int i = 0; i < 4; i++) s[446 + 12 + i] = n >> 8 * i;
        s[510] = 0x55; s[511] = 0xAA;
        disk_wr(0, 1, s);
        memset(&h1, 0, sizeof h1);
        memcpy(h1.sig, "EFI PART", 8);
        h1.rev = 0x10000; h1.hsize = 92; h1.self = 1; h1.alt = ns - 1;
        h1.first_usable = 2 + esect; h1.last_usable = ns - 2 - esect;
        sys_random(h1.guid, 16);
        h1.entries_lba = 2; h1.nentries = 128; h1.entsize = 128;
        h2 = h1; h2.self = ns - 1; h2.alt = 1; h2.entries_lba = ns - 1 - esect;
        free(ents); ents = zalloc(16384);
        entry_of((gpt_entry_t *)ents, T_ESP, efirst, elast, "EFI system partition");
        memcpy(ents + 128, &ne, 128);
        write_gpt();
        static uint8_t z[DISK_MAX];
        disk_wr(sfirst, DISK_MAX / bs, z);       /* (the old contents' first bytes: no stale superblock) */
        fat_format(efirst, elast - efirst + 1, bs, "SIEOS-BOOT");
        if ((w = fat_open(efirst, elast - efirst + 1, bs))) die(w);
    } else {
        may_write(1, h1.entries_lba + esect - 1);
        may_write(h2.entries_lba, ns - 1);
        may_write(sfirst, slast);
        may_write(efirst, elast);
    }
    make_siefs(sfirst, slast);
    put_esp(names, cfg, ne.guid);
    printf("progress 95\n");
    if (!whole) {
        memcpy(ents + 128 * free_slot, &ne, 128);
        write_gpt();
        for (uint32_t i = 0; i < h1.nentries; i++)   /* every other entry as it was */
            if ((int)i != free_slot && memcmp(ents + 128 * i, orig + 128 * i, 128)) die("BUG: another entry changed");
    }
    int pnum = whole ? 2 : free_slot + 1;        /* (check_gpt reads the table again) */
    uint8_t *want = zalloc((uint64_t)esect * bs);
    memcpy(want, ents, (uint64_t)esect * bs);
    check_gpt(want);
    printf("progress 100\n");
    printf("done: SIEOS is installed on drive %d, partition %d.\n", drive, pnum);
    printf("next: start the PC from the USB key once more: its loader adds \"SIEOS\" to the boot menu (F12).\n");
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-n")) { dry = 1; i++; }
    const char *cmd = i < argc ? argv[i] : "";
    mk_ident_t me;
    sys_ident(0, &me);
    if (me.uid) die("only root may install (the drives are root's)");
    if (!strcmp(cmd, "list")) { list(); return 0; }
    int whole = !strcmp(cmd, "whole");
    if ((!whole && strcmp(cmd, "free")) || i + 1 + !whole >= argc) {
        printf("usage: install list | install [-n] free DRIVE N | install [-n] whole DRIVE\n");
        return 2;
    }
    if (open_drive((int)strnum(argv[i + 1]))) die("no such drive");
    return install(whole, whole ? 0 : (int)strnum(argv[i + 2]));
}
