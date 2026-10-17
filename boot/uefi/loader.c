/*
 * loader.c - SIEOS's UEFI boot loader (EFI/BOOT/BOOTX64.EFI on a USB key's
 * EFI System Partition). The PC's firmware starts it; it:
 *   1. reads \SIEOS\BOOT.CFG from its own partition (on a drive SIEOS was
 *      installed on, \EFI\SIEOS\BOOT.CFG): the boot modules
 *      ("module=init.elf", init first), a preferred screen size
 *      ("screen=1920x1080") and SIEOS's partition ("root=GUID", passed to
 *      the kernel as a boot option);
 *      then adds "SIEOS" to the firmware's boot menu if the installer asked
 *      (boot_entry below);
 *   2. loads \SIEOS\KERNEL.BIN where its multiboot header says (1 MiB),
 *      and the modules, with its information and room for the kernel's page
 *      bitmap, into one block below 4 GiB (mem.c puts the bitmap after the
 *      last of them);
 *   3. sets the screen mode (the preferred size if the firmware offers it,
 *      else the one it uses) and notes the frame buffer;
 *   4. finds the ACPI tables' root pointer;
 *   5. takes the final memory map, leaves the firmware (ExitBootServices)
 *      and jumps to the kernel's 64-bit entry with a sieos_boot_t (mk/boot.h).
 * Any failure is printed and returned to the firmware (its boot menu).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include "efi.h"
#include "mk/boot.h"
#include "mk/lib.h"

#define PAGE 4096UL
#define PAGES(n) (((n) + PAGE - 1) / PAGE)

static efi_system_t *st;
static efi_boot_t *bs;
#define BS(i) (bs->fn[i])

/* ---- Text on the firmware's console (UCS-2). */
static void print(const char *s)
{
    static uint16_t w[256];
    int n = 0;
    for (; *s && n < 254; s++) {
        if (*s == '\n') w[n++] = '\r';
        w[n++] = (uint8_t)*s;
    }
    w[n] = 0;
    CALL(st->con_out->output_string, (uint64_t)st->con_out, (uint64_t)w, 0, 0, 0, 0);
}
static efi_status_t fail(const char *what, efi_status_t s)
{
    print("SIEOS loader: "); print(what); print("\n");
    return s ? s : EFI_LOAD_ERROR;
}

static int guid_eq(const efi_guid_t *a, const efi_guid_t *b) { return !memcmp(a, b, sizeof *a); }
static const efi_guid_t LOADED_IMAGE = { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const efi_guid_t SIMPLE_FS    = { 0x964E5B22, 0x6459, 0x11d2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const efi_guid_t GOP          = { 0x9042A9DE, 0x23DC, 0x4A38, { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } };
static const efi_guid_t ACPI20       = { 0x8868E871, 0xE4F1, 0x11d3, { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } };
static const efi_guid_t ACPI10       = { 0xEB9D2D30, 0x2D88, 0x11d3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

/* ---- Memory: pages from the firmware (type 0: any, 1: below max, 2: at addr). */
static uint64_t pages(int type, uint64_t addr, uint64_t n)
{
    uint64_t a = addr;
    return EFI_ERR(CALL(BS(BS_ALLOCATE_PAGES), type, LOADER_DATA, n, (uint64_t)&a, 0, 0)) ? 0 : a;
}

/* ---- Files on our own partition. */
static efi_file_t *root;
static const char *dir = "\\SIEOS\\";                       /* (or "\\EFI\\SIEOS\\": installed) */

static efi_file_t *open_file(const char *name)
{
    static uint16_t path[80];
    int n = 0;
    for (const char *s = dir; *s; s++) path[n++] = *s;
    for (; *name && n < 78; name++) path[n++] = *name == '/' ? '\\' : (uint8_t)*name;
    path[n] = 0;
    efi_file_t *f = 0;
    if (EFI_ERR(CALL(root->open, (uint64_t)root, (uint64_t)&f, (uint64_t)path, 1, 0, 0))) return 0;   /* 1: read */
    return f;
}
static uint64_t file_size(efi_file_t *f)
{
    uint64_t size = 0;
    CALL(f->set_pos, (uint64_t)f, ~0UL, 0, 0, 0, 0);                 /* the end ... */
    CALL(f->get_pos, (uint64_t)f, (uint64_t)&size, 0, 0, 0, 0);       /* ... is the size */
    CALL(f->set_pos, (uint64_t)f, 0, 0, 0, 0, 0);
    return size;
}
static int read_all(efi_file_t *f, void *buf, uint64_t size)
{
    for (uint64_t done = 0; done < size; ) {
        uint64_t n = size - done;
        if (EFI_ERR(CALL(f->read, (uint64_t)f, (uint64_t)&n, (uint64_t)buf + done, 0, 0, 0)) || !n) return -1;
        done += n;
    }
    return 0;
}

/* ---- The configuration: "key=value" lines. */
static char cfg[2048];
static const char *mods[SIEOS_BOOT_MODS], *rootid;
static int nmods, want_w = 1920, want_h = 1080;

static int number(const char **s)
{
    int v = 0;
    while (**s >= '0' && **s <= '9') v = v * 10 + *(*s)++ - '0';
    return v;
}
static void read_config(void)
{
    efi_file_t *f = open_file("BOOT.CFG");
    uint64_t n = f ? file_size(f) : 0;
    if (!f || n >= sizeof cfg || read_all(f, cfg, n)) n = 0;
    if (f) CALL(f->close, (uint64_t)f, 0, 0, 0, 0, 0);
    cfg[n] = 0;
    for (char *l = cfg; *l; ) {
        char *e = l;
        while (*e && *e != '\n' && *e != '\r') e++;
        char next = *e;
        *e = 0;
        if (!memcmp(l, "module=", 7) && nmods < SIEOS_BOOT_MODS) mods[nmods++] = l + 7;
        else if (!memcmp(l, "root=", 5) && strlen(l) < 45) rootid = l;
        else if (!memcmp(l, "screen=", 7)) {
            const char *s = l + 7;
            int w = number(&s), h = 0;
            if (*s == 'x') { s++; h = number(&s); }
            if (w && h) { want_w = w; want_h = h; }
        }
        l = next ? e + 1 : e;
    }
}

/* ---- The screen: the preferred size if offered (32-bit, frame buffer), else the current mode. */
static void screen(sieos_boot_t *b)
{
    efi_gop_t *g = 0;
    if (EFI_ERR(CALL(BS(BS_LOCATE_PROTOCOL), (uint64_t)&GOP, 0, (uint64_t)&g, 0, 0, 0)) || !g) return;
    uint32_t best = g->mode->mode;
    for (uint32_t i = 0; i < g->mode->max_mode; i++) {
        efi_mode_info_t *in = 0;
        uint64_t size = 0;
        if (EFI_ERR(CALL(g->query_mode, (uint64_t)g, i, (uint64_t)&size, (uint64_t)&in, 0, 0)) || !in) continue;
        if (in->format <= 1 && (int)in->width == want_w && (int)in->height == want_h) { best = i; break; }
    }
    if (best != g->mode->mode) CALL(g->set_mode, (uint64_t)g, best, 0, 0, 0, 0);
    efi_mode_info_t *in = g->mode->info;
    if (in->format > 1) return;                                        /* not a plain 32-bit frame buffer */
    b->fb.pa = g->mode->fb_base;
    b->fb.size = g->mode->fb_size;
    b->fb.width = in->width;
    b->fb.height = in->height;
    b->fb.pitch = in->pitch;
    b->fb.format = in->format;
}

/* ---- The firmware's boot menu. SIEOS's kernel runs after ExitBootServices,
 * without the firmware's runtime services, so the installer (user/install)
 * cannot write UEFI variables itself: it leaves a request, the file
 * \EFI\SIEOS\ADDENTRY, on the drive's EFI partition. Started from the USB
 * key (or from that partition), this loader still has the firmware's
 * services: it adds the boot entry "SIEOS" (Boot####: that partition,
 * \EFI\SIEOS\SIEOSX64.EFI) at the END of BootOrder, so the system that
 * starts by default stays first and the boot menu (F12) shows SIEOS, then
 * removes the request. An identical entry already there is not added again. */
static const efi_guid_t DEVICE_PATH  = { 0x09576E91, 0x6D3F, 0x11d2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const efi_guid_t GLOBAL_VAR   = { 0x8BE4DF61, 0x93CA, 0x11d2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };
#define RT(i) (st->runtime->fn[i])
#define EFI_NOT_FOUND (0x800000000000000EUL)

static uint8_t *wstr(uint8_t *w, const char *s)   /* UCS-2, with its 0 -> after it */
{
    do { w[0] = (uint8_t)*s; w[1] = 0; w += 2; } while (*s++);
    return w;
}
static uint16_t vname[16];
static uint16_t *boot_var(unsigned n)             /* "Boot0003" */
{
    wstr((uint8_t *)vname, "Boot0000");
    for (int i = 0; i < 4; i++) vname[7 - i] = "0123456789ABCDEF"[n >> 4 * i & 15];
    return vname;
}
static efi_status_t get_var(uint16_t *name, void *buf, uint64_t *size)
{
    uint32_t attr;
    return CALL(RT(RT_GET_VARIABLE), (uint64_t)name, (uint64_t)&GLOBAL_VAR, (uint64_t)&attr, (uint64_t)size, (uint64_t)buf, 0);
}
static efi_status_t set_var(uint16_t *name, const void *buf, uint64_t size)   /* non-volatile, boot + runtime */
{
    return CALL(RT(RT_SET_VARIABLE), (uint64_t)name, (uint64_t)&GLOBAL_VAR, 7, size, (uint64_t)buf, 0);
}

static void boot_entry(void *h, efi_file_t *req)
{
    efi_dp_t *dp = 0, *hd = 0;
    if (EFI_ERR(CALL(BS(BS_HANDLE_PROTOCOL), (uint64_t)h, (uint64_t)&DEVICE_PATH, (uint64_t)&dp, 0, 0, 0)) || !dp) return;
    for (efi_dp_t *d = dp; d->type != 0x7F; d = (efi_dp_t *)((uint8_t *)d + (d->len[0] | d->len[1] << 8)))
        if (d->type == 4 && d->sub == 1) hd = d;              /* the partition ("hard drive" node) */
    if (!hd || hd->len[0] != 42) { print("SIEOS loader: boot entry: no partition in the device path\n"); CALL(req->close, (uint64_t)req, 0, 0, 0, 0, 0); return; }
    /* The load option: attributes (active), the path's length, "SIEOS",
     * then the path in its short form: the partition, the file, the end. */
    uint8_t opt[200], cur[400];                             /* (on the stack: not in the file) */
    uint16_t order[256], ord_name[10];
    uint8_t *p = wstr(opt + 6, "SIEOS"), *path = p;
    const char *file = "\\EFI\\SIEOS\\SIEOSX64.EFI";
    unsigned fl = 4 + 2 * (strlen(file) + 1);
    opt[0] = 1;                                              /* LOAD_OPTION_ACTIVE */
    memcpy(p, hd, 42); p += 42;
    p[0] = 4; p[1] = 4; p[2] = fl; p[3] = 0; wstr(p + 4, file); p += fl;
    p[0] = 0x7F; p[1] = 0xFF; p[2] = 4; p[3] = 0; p += 4;
    opt[4] = (p - path) & 0xFF; opt[5] = (p - path) >> 8;
    uint64_t len = p - opt, osz = sizeof order - 2, sz;
    wstr((uint8_t *)ord_name, "BootOrder");
    efi_status_t e = get_var(ord_name, order, &osz);
    if (e == EFI_NOT_FOUND) osz = 0;
    else if (EFI_ERR(e) || osz % 2) {                        /* (never a boot order cut short) */
        print("SIEOS loader: cannot read BootOrder: no boot entry added\n");
        CALL(req->close, (uint64_t)req, 0, 0, 0, 0, 0);
        return;
    }
    for (uint64_t i = 0; i < osz / 2; i++)                  /* the same entry already in the menu? */
        if (sz = sizeof cur, !EFI_ERR(get_var(boot_var(order[i]), cur, &sz)) && sz == len && !memcmp(cur, opt, len)) {
            CALL(req->del, (uint64_t)req, 0, 0, 0, 0, 0);
            return;
        }
    unsigned n = 0;
    for (; n < 0x1000; n++)                                  /* the first free Boot#### */
        if (sz = sizeof cur, get_var(boot_var(n), cur, &sz) == EFI_NOT_FOUND) break;
    order[osz / 2] = n;
    if (n == 0x1000 || EFI_ERR(set_var(boot_var(n), opt, len)) || EFI_ERR(set_var(ord_name, order, osz + 2))) {
        print("SIEOS loader: could not add SIEOS to the firmware's boot menu\n");
        CALL(req->close, (uint64_t)req, 0, 0, 0, 0, 0);
        return;
    }
    CALL(req->del, (uint64_t)req, 0, 0, 0, 0, 0);            /* (done: the request goes) */
    char m[] = "SIEOS loader: SIEOS added to the firmware's boot menu (Boot0000)\n";
    for (int i = 0; i < 4; i++) m[62 - i] = "0123456789ABCDEF"[n >> 4 * i & 15];
    print(m);
}

/* Every FAT volume of every drive: a request? If ours is the only one,
 * every device is connected first (a firmware starting from the USB key may
 * have connected only that; connecting all costs about 0.5 s in QEMU). */
static void boot_entries(void)
{
    void **hs = 0;
    uint16_t req[24];
    uint64_t n = 0;
    wstr((uint8_t *)req, "\\EFI\\SIEOS\\ADDENTRY");
    for (int pass = 0; pass < 2; pass++) {
        if (EFI_ERR(CALL(BS(BS_LOCATE_HANDLE_BUFFER), 2, (uint64_t)&SIMPLE_FS, 0, (uint64_t)&n, (uint64_t)&hs, 0))) return;
        if (n > 1 || pass) break;
        CALL(BS(BS_FREE_POOL), (uint64_t)hs, 0, 0, 0, 0, 0);
        if (EFI_ERR(CALL(BS(BS_LOCATE_HANDLE_BUFFER), 0, 0, 0, (uint64_t)&n, (uint64_t)&hs, 0))) return;   /* 0: all handles */
        for (uint64_t i = 0; i < n; i++) CALL(BS(BS_CONNECT_CONTROLLER), (uint64_t)hs[i], 0, 0, 1, 0, 0);
        CALL(BS(BS_FREE_POOL), (uint64_t)hs, 0, 0, 0, 0, 0);
    }
    for (uint64_t i = 0; i < n; i++) {
        efi_fs_t *fs = 0;
        efi_file_t *r = 0, *f = 0;
        if (EFI_ERR(CALL(BS(BS_HANDLE_PROTOCOL), (uint64_t)hs[i], (uint64_t)&SIMPLE_FS, (uint64_t)&fs, 0, 0, 0)) ||
            EFI_ERR(CALL(fs->open_volume, (uint64_t)fs, (uint64_t)&r, 0, 0, 0, 0))) continue;
        if (!EFI_ERR(CALL(r->open, (uint64_t)r, (uint64_t)&f, (uint64_t)req, 3, 0, 0)) && f) boot_entry(hs[i], f);   /* 3: read, write */
        CALL(r->close, (uint64_t)r, 0, 0, 0, 0, 0);
    }
    CALL(BS(BS_FREE_POOL), (uint64_t)hs, 0, 0, 0, 0, 0);
}

efi_status_t efi_main(void *image, efi_system_t *system)
{
    st = system;
    bs = st->boot;
    CALL(BS(BS_SET_WATCHDOG), 0, 0, 0, 0, 0, 0);                      /* (the firmware's 5-minute reset: off) */
    print("SIEOS loader\n");

    /* Our partition: the device we were loaded from. */
    efi_loaded_image_t *li = 0;
    efi_fs_t *fs = 0;
    if (EFI_ERR(CALL(BS(BS_HANDLE_PROTOCOL), (uint64_t)image, (uint64_t)&LOADED_IMAGE, (uint64_t)&li, 0, 0, 0)) ||
        EFI_ERR(CALL(BS(BS_HANDLE_PROTOCOL), (uint64_t)li->device, (uint64_t)&SIMPLE_FS, (uint64_t)&fs, 0, 0, 0)) ||
        EFI_ERR(CALL(fs->open_volume, (uint64_t)fs, (uint64_t)&root, 0, 0, 0, 0)))
        return fail("cannot read my own partition", 0);
    efi_file_t *cf = open_file("BOOT.CFG");
    if (cf) CALL(cf->close, (uint64_t)cf, 0, 0, 0, 0, 0);
    else dir = "\\EFI\\SIEOS\\";                                /* installed on a drive's EFI partition */
    read_config();
    boot_entries();
    if (!nmods) return fail("no module= line in \\SIEOS\\BOOT.CFG", 0);

    /* The kernel: a flat binary with a multiboot header at its start, then
     * "SIE6" and the 64-bit entry point (boot.S). */
    efi_file_t *f = open_file("KERNEL.BIN");
    uint64_t ksize = f ? file_size(f) : 0;
    uint32_t hdr[10];
    if (!f || ksize < sizeof hdr || read_all(f, hdr, sizeof hdr)) return fail("cannot read \\SIEOS\\KERNEL.BIN", 0);
    if (hdr[0] != 0x1BADB002 || hdr[8] != SIEOS_ENTRY64 || hdr[4] != hdr[3]) return fail("KERNEL.BIN is not a SIEOS kernel", 0);
    uint64_t load = hdr[4], load_end = hdr[5], bss_end = hdr[6], entry = hdr[9];
    if (load_end - load > ksize || !pages(ALLOC_ADDRESS, load, PAGES(bss_end - load)))
        return fail("the memory at 1 MiB is not free for the kernel", 0);
    CALL(f->set_pos, (uint64_t)f, 0, 0, 0, 0, 0);
    if (read_all(f, (void *)load, load_end - load)) return fail("cannot read the kernel", 0);
    memset((void *)load_end, 0, bss_end - load_end);
    CALL(f->close, (uint64_t)f, 0, 0, 0, 0, 0);

    /* The memory map's size now, and the RAM's top (for the bitmap's room). */
    uint64_t msize = 0, key = 0, dsize = 0;
    uint32_t dver = 0;
    CALL(BS(BS_GET_MEMORY_MAP), (uint64_t)&msize, 0, (uint64_t)&key, (uint64_t)&dsize, (uint64_t)&dver, 0);
    if (!dsize) return fail("no memory map", 0);
    uint64_t mcap = msize + 32 * dsize;                                /* (allocations below add a few entries) */
    uint8_t *tmp = (uint8_t *)pages(ALLOC_ANY, 0, PAGES(mcap));
    uint64_t top = 0;
    msize = mcap;
    if (!tmp || EFI_ERR(CALL(BS(BS_GET_MEMORY_MAP), (uint64_t)&msize, (uint64_t)tmp, (uint64_t)&key, (uint64_t)&dsize, (uint64_t)&dver, 0)))
        return fail("cannot read the memory map", 0);
    for (uint64_t o = 0; o + dsize <= msize; o += dsize) {
        uint32_t t = *(uint32_t *)(tmp + o);
        uint64_t end = *(uint64_t *)(tmp + o + 8) + *(uint64_t *)(tmp + o + 24) * PAGE;
        if (((t >= 1 && t <= 4) || t == 7) && end > top) top = end;
    }
    CALL(BS(BS_FREE_PAGES), (uint64_t)tmp, PAGES(mcap), 0, 0, 0, 0);

    /* The modules' sizes, then one block below 4 GiB: the modules, this
     * information, the final memory map, room for the bitmap. */
    efi_file_t *mf[SIEOS_BOOT_MODS];
    uint64_t msz[SIEOS_BOOT_MODS], total = 0;
    for (int i = 0; i < nmods; i++) {
        char name[64];
        int n = 0;
        while (mods[i][n] && mods[i][n] != ' ' && n < 63) { name[n] = mods[i][n]; n++; }
        name[n] = 0;
        if (!(mf[i] = open_file(name))) { print(name); return fail(": module not found", 0); }
        msz[i] = file_size(mf[i]);
        total += PAGES(msz[i]) * PAGE;
    }
    uint64_t info_len = PAGES(sizeof(sieos_boot_t)) * PAGE + PAGES(mcap) * PAGE;
    uint64_t room = PAGES(top / PAGE / 8) * PAGE + PAGE;
    uint64_t blk = pages(ALLOC_MAX_ADDRESS, 0xFFFFFFFF, PAGES(total + info_len + room));
    if (!blk) return fail("not enough memory below 4 GiB for the modules", 0);
    sieos_boot_t *b = (sieos_boot_t *)(blk + total);
    memset(b, 0, sizeof *b);
    b->magic = SIEOS_BOOT_MAGIC;
    b->version = 2;
    if (rootid) strlcpy(b->cmdline, rootid, sizeof b->cmdline);
    b->self = (uint64_t)b;
    b->self_len = info_len;
    b->mmap = (uint64_t)b + PAGES(sizeof(sieos_boot_t)) * PAGE;
    for (uint64_t at = blk, i = 0; i < (uint64_t)nmods; i++) {
        if (read_all(mf[i], (void *)at, msz[i])) return fail("cannot read a module", 0);
        CALL(mf[i]->close, (uint64_t)mf[i], 0, 0, 0, 0, 0);
        b->mod[i].pa = at;
        b->mod[i].len = msz[i];
        strlcpy(b->mod[i].cmdline, mods[i], sizeof b->mod[i].cmdline);
        at += PAGES(msz[i]) * PAGE;
    }
    b->nmod = nmods;

    screen(b);
    for (uint64_t i = 0; i < st->ntables; i++)                         /* ACPI 2.0's pointer, else 1.0's */
        if (guid_eq(&st->tables[i].guid, &ACPI20) || (!b->rsdp && guid_eq(&st->tables[i].guid, &ACPI10)))
            b->rsdp = (uint64_t)st->tables[i].table;
    print("SIEOS loader: starting the kernel\n");

    /* The final map, then leave the firmware. If the map changed between
     * the two calls, ExitBootServices refuses: read it again (no more
     * allocations or printing from here). */
    for (int tries = 0; tries < 8; tries++) {
        msize = mcap;
        if (EFI_ERR(CALL(BS(BS_GET_MEMORY_MAP), (uint64_t)&msize, b->mmap, (uint64_t)&key, (uint64_t)&dsize, (uint64_t)&dver, 0))) continue;
        b->mmap_size = msize;
        b->desc_size = dsize;
        if (!EFI_ERR(CALL(BS(BS_EXIT_BOOT_SERVICES), (uint64_t)image, key, 0, 0, 0, 0)))
            efi_jump(entry, SIEOS_BOOT_MAGIC, (uint64_t)b);
    }
    return fail("the firmware would not let go (ExitBootServices)", 0);
}
