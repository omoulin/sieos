/*
 * modload.c - Loadable drivers: /drv/NAME.drv, ELF relocatable objects
 * (ELF-64 object file format; the x86-64 psABI's relocations).
 *
 * The drivers come from the boot archive (/boot/bootarch.tar, a POSIX ustar
 * archive of /drv the boot loader brings with the kernel, as Solaris' boot
 * archive: the drivers the root device itself needs are there before it is
 * mounted) and, once the root is mounted, from /drv.  Each says in its
 * .drv_info section its name and phase, in .drv_aliases the devices it
 * drives (see ddi.h); both are read without loading it.
 *
 * At each phase of the boot the devices (the PCI functions; the platform's:
 * the i8042) are matched against the aliases, the most specific name first:
 * pciVVVV,DDDD, then pciVVVV,classCC (a vendor's devices of a class: its
 * GPUs, say), then pciclass,CCSSPP, then pciclass,CCSS.  A driver matched
 * is loaded, once: its allocated sections laid out in physically contiguous
 * memory below 2 GiB, used through the kernel's own mapping (so drivers are
 * compiled as the kernel is, -mcmodel=kernel); its symbols resolved against
 * the kernel's exported table (ksyms, every global symbol of the kernel,
 * generated at the kernel's link) and the drivers loaded before it; its
 * RELA relocations applied; then its _init called.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "kernel.h"
#include "mm.h"
#include "fs.h"
#include "pci.h"
#include "arch.h"
#include "ddi.h"
#include "elf.h"
#include "sieos/sysinfo.h"
#include "abi.h"

/* ELF-64 object files */
typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} Elf64_Shdr;

typedef struct {
    uint32_t st_name;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} Elf64_Sym;

typedef struct {
    uint64_t r_offset, r_info;
    int64_t r_addend;
} Elf64_Rela;

#define ET_REL       1
#define SHT_SYMTAB   2
#define SHT_RELA     4
#define SHT_NOBITS   8
#define SHF_ALLOC    0x2
#define SHN_UNDEF    0
#define SHN_ABS      0xFFF1
#define SHN_COMMON   0xFFF2
#define STB_LOCAL    0
#define STB_WEAK     2
#define R_X86_64_NONE  0
#define R_X86_64_64    1
#define R_X86_64_PC32  2
#define R_X86_64_PLT32 4
#define R_X86_64_32    10
#define R_X86_64_32S   11
#define R_X86_64_PC64  24

/* the kernel's exported symbols (ksyms.S, generated): sorted by name */
struct ksym { uint64_t addr; uint64_t name; };
extern const uint64_t ksyms_count;
extern const struct ksym ksyms_table[];
extern const char ksyms_names[];

#define NMOD 48

static struct module {
    char name[32], desc[96];
    int phase, state;                            /* SIEOS_MOD_* */
    const uint8_t *image;                        /* the file (in the boot archive, or read into kmalloc memory) */
    size_t size;
    char aliases[1024];                          /* NUL-separated */
    int alen;
    char source[16], alias[64];
    int ndev;
    uint8_t *base;                               /* loaded: the sections' block */
    size_t mem;
    const Elf64_Sym *syms;                       /* (kept for the drivers loaded after it) */
    const char *strtab;
    uint64_t *symval;
    int nsyms;
} mods[NMOD];
static int nmods;

static void (*polls[16])(void);
static int npolls;
long (*ddi_wifi_op)(int op, void *buf, long n);

void ddi_poll_register(void (*fn)(void))
{
    if (npolls < (int)ARRAY_SIZE(polls))
        polls[npolls++] = fn;
}

void ddi_poll(void)
{
    for (int i = 0; i < npolls; i++)
        polls[i]();
}

/* ---------------------------------------------------------------- the files */

static const Elf64_Shdr *section(const uint8_t *img, size_t size, const char *want)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img;
    if (size < sizeof(*eh) || eh->e_magic != ELF_MAGIC || eh->e_class != ELFCLASS64 || eh->e_type != ET_REL ||
        eh->e_machine != EM_X86_64 || eh->e_shentsize != sizeof(Elf64_Shdr) ||
        eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > size || eh->e_shstrndx >= eh->e_shnum)
        return NULL;
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(img + eh->e_shoff);
    const Elf64_Shdr *ss = &sh[eh->e_shstrndx];
    if (ss->sh_offset + ss->sh_size > size)
        return NULL;
    for (int i = 0; i < eh->e_shnum; i++)
        if (sh[i].sh_name < ss->sh_size && !strcmp((const char *)img + ss->sh_offset + sh[i].sh_name, want))
            return sh[i].sh_offset + sh[i].sh_size <= size || sh[i].sh_type == SHT_NOBITS ? &sh[i] : NULL;
    return NULL;
}

/* A driver file known: its name, phase and aliases (not loaded). */
static struct module *mod_add(const uint8_t *img, size_t size, const char *source)
{
    const Elf64_Shdr *info = section(img, size, ".drv_info"), *al = section(img, size, ".drv_aliases");
    if (!info || info->sh_size < sizeof(struct ddi_modinfo))
        return NULL;
    const struct ddi_modinfo *mi = (const struct ddi_modinfo *)(img + info->sh_offset);
    if (mi->magic != DDI_MAGIC || mi->abi != DDI_ABI)
        return NULL;
    for (int i = 0; i < nmods; i++)
        if (!strncmp(mods[i].name, mi->name, sizeof(mods[i].name)))
            return &mods[i];                     /* (the boot archive's copy first) */
    if (nmods == NMOD)
        return NULL;
    struct module *m = &mods[nmods++];
    memset(m, 0, sizeof(*m));
    strlcpy(m->name, mi->name, sizeof(m->name));
    strlcpy(m->desc, mi->desc, sizeof(m->desc));
    m->phase = mi->phase;
    m->image = img;
    m->size = size;
    strlcpy(m->source, source, sizeof(m->source));
    if (al) {
        m->alen = al->sh_size < sizeof(m->aliases) - 1 ? (int)al->sh_size : (int)sizeof(m->aliases) - 1;
        memcpy(m->aliases, img + al->sh_offset, m->alen);
    }
    return m;
}

static bool has_alias(const struct module *m, const char *name)
{
    for (int i = 0; i < m->alen;) {
        const char *a = m->aliases + i;
        size_t l = strnlen(a, m->alen - i);
        if (l && !strcmp(a, name))
            return true;
        i += l + 1;
    }
    return false;
}

static uint64_t octal(const char *s, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++)
        v = v * 8 + (s[i] - '0');
    return v;
}

/* The boot archive (ustar): its drv/NAME.drv files, used in place. */
void modules_init(uint64_t pa, uint64_t size)
{
    if (!pa || !size) {
        kprintf("drv: no boot archive from the boot loader (module2 ... boot_archive): no driver before the root\n");
        return;
    }
    const uint8_t *a = P2V(pa);
    int n = 0;
    for (uint64_t off = 0; off + 512 <= size;) {
        const char *h = (const char *)a + off;
        if (!h[0])
            break;
        uint64_t fsz = octal(h + 124, 12);
        char typ = h[156];
        char name[256];
        const char *prefix = h + 345;
        if (!memcmp(h + 257, "ustar", 5) && prefix[0])
            snprintf(name, sizeof(name), "%.155s/%.100s", prefix, h);
        else
            snprintf(name, sizeof(name), "%.100s", h);
        size_t nl = strlen(name);
        if ((typ == '0' || typ == 0) && nl > 4 && !strcmp(name + nl - 4, ".drv") && off + 512 + fsz <= size &&
            mod_add(a + off + 512, fsz, "boot archive"))
            n++;
        off += 512 + ((fsz + 511) & ~511UL);
    }
    kprintf("drv: boot archive: %d driver%s, %lu KiB\n", n, n == 1 ? "" : "s", size / 1024);
}

/* ---------------------------------------------------------------- loading */

static const struct ksym *ksym_find(const char *name)
{
    uint64_t lo = 0, hi = ksyms_count;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        int c = strcmp(name, ksyms_names + ksyms_table[mid].name);
        if (!c)
            return &ksyms_table[mid];
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

/* A symbol a driver needs: the kernel's, or a global of a driver loaded before. */
static bool resolve(const char *name, uint64_t *out)
{
    const struct ksym *k = ksym_find(name);
    if (k) {
        *out = k->addr;
        return true;
    }
    for (int i = 0; i < nmods; i++) {
        const struct module *m = &mods[i];
        if (m->state != SIEOS_MOD_LOADED || !m->syms)
            continue;
        for (int s = 1; s < m->nsyms; s++)
            if ((m->syms[s].st_info >> 4) != STB_LOCAL && m->syms[s].st_shndx != SHN_UNDEF &&
                !strcmp(m->strtab + m->syms[s].st_name, name)) {
                *out = m->symval[s];
                return true;
            }
    }
    return false;
}

static int load_fail(struct module *m, const char *why, const char *what)
{
    kprintf("drv: %s: %s%s%s\n", m->name, why, what ? " " : "", what ? what : "");
    m->state = SIEOS_MOD_FAILED;
    return -ENOEXEC;
}

static int mod_load(struct module *m)
{
    if (m->state == SIEOS_MOD_LOADED)
        return 0;
    if (m->state == SIEOS_MOD_FAILED)
        return -ENOEXEC;
    const uint8_t *img = m->image;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img;
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(img + eh->e_shoff);
    int nsh = eh->e_shnum;
    uint64_t *off = kzalloc(nsh * sizeof(uint64_t));
    if (!off)
        return load_fail(m, "no memory", NULL);
    /* the allocated sections, in one block */
    uint64_t total = 0;
    for (int i = 0; i < nsh; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC) || !sh[i].sh_size)
            continue;
        uint64_t al = sh[i].sh_addralign > 1 ? sh[i].sh_addralign : 1;
        total = (total + al - 1) & ~(al - 1);
        off[i] = total;
        total += sh[i].sh_size;
        if (sh[i].sh_type != SHT_NOBITS && sh[i].sh_offset + sh[i].sh_size > m->size) {
            kfree(off);
            return load_fail(m, "a section past the end of the file", NULL);
        }
    }
    /* (common symbols after them) */
    const Elf64_Shdr *symsh = NULL;
    for (int i = 0; i < nsh; i++)
        if (sh[i].sh_type == SHT_SYMTAB)
            symsh = &sh[i];
    if (!symsh || symsh->sh_link >= (uint32_t)nsh) {
        kfree(off);
        return load_fail(m, "no symbol table", NULL);
    }
    const Elf64_Sym *syms = (const Elf64_Sym *)(img + symsh->sh_offset);
    int nsyms = symsh->sh_size / sizeof(Elf64_Sym);
    const char *strtab = (const char *)img + sh[symsh->sh_link].sh_offset;
    uint64_t *commons = kzalloc(nsyms * sizeof(uint64_t));
    for (int s = 1; s < nsyms && commons; s++)
        if (syms[s].st_shndx == SHN_COMMON) {
            uint64_t al = syms[s].st_value > 1 ? syms[s].st_value : 1;
            total = (total + al - 1) & ~(al - 1);
            commons[s] = total;
            total += syms[s].st_size;
        }
    size_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t pa = pages ? pmm_alloc_contig(pages) : 0;
    if (!pa || !commons || pa + pages * PAGE_SIZE > (2UL << 30)) {
        if (pa)
            pmm_free_contig(pa, pages);
        kfree(off);
        kfree(commons);
        return load_fail(m, "no memory below 2 GiB", NULL);
    }
    uint8_t *base = (uint8_t *)(KERNEL_VBASE + pa);  /* (the kernel's mapping: -mcmodel=kernel reaches it) */
    for (int i = 0; i < nsh; i++)
        if ((sh[i].sh_flags & SHF_ALLOC) && sh[i].sh_size && sh[i].sh_type != SHT_NOBITS)
            memcpy(base + off[i], img + sh[i].sh_offset, sh[i].sh_size);
    /* the symbols' values */
    uint64_t *val = kzalloc(nsyms * sizeof(uint64_t));
    int err = 0;
    for (int s = 1; s < nsyms && val; s++) {
        const Elf64_Sym *y = &syms[s];
        if (y->st_shndx == SHN_UNDEF) {
            const char *name = strtab + y->st_name;
            if (!name[0])
                continue;
            if (!resolve(name, &val[s])) {
                if ((y->st_info >> 4) == STB_WEAK)
                    continue;
                load_fail(m, "undefined symbol", name);
                err = -ENOEXEC;
            }
        } else if (y->st_shndx == SHN_ABS) {
            val[s] = y->st_value;
        } else if (y->st_shndx == SHN_COMMON) {
            val[s] = (uint64_t)base + commons[s];
        } else if (y->st_shndx < nsh) {
            val[s] = (uint64_t)base + off[y->st_shndx] + y->st_value;
        }
    }
    /* the relocations of the allocated sections */
    for (int i = 0; i < nsh && !err && val; i++) {
        if (sh[i].sh_type != SHT_RELA || sh[i].sh_info >= (uint32_t)nsh || !(sh[sh[i].sh_info].sh_flags & SHF_ALLOC))
            continue;
        const Elf64_Rela *r = (const Elf64_Rela *)(img + sh[i].sh_offset);
        int nr = sh[i].sh_size / sizeof(Elf64_Rela);
        uint8_t *sec = base + off[sh[i].sh_info];
        for (int k = 0; k < nr && !err; k++) {
            uint32_t type = r[k].r_info & 0xFFFFFFFF, si = r[k].r_info >> 32;
            if (si >= (uint32_t)nsyms) {
                err = load_fail(m, "a bad relocation", NULL);
                break;
            }
            uint8_t *p = sec + r[k].r_offset;
            uint64_t S = val[si], P = (uint64_t)p;
            int64_t A = r[k].r_addend, v;
            switch (type) {
            case R_X86_64_NONE:
                break;
            case R_X86_64_64:
                *(uint64_t *)p = S + A;
                break;
            case R_X86_64_PC64:
                *(uint64_t *)p = S + A - P;
                break;
            case R_X86_64_PC32:
            case R_X86_64_PLT32:
                v = (int64_t)(S + A - P);
                if (v != (int32_t)v)
                    err = load_fail(m, "a relocation out of range", strtab + syms[si].st_name);
                *(int32_t *)p = (int32_t)v;
                break;
            case R_X86_64_32S:
                v = (int64_t)(S + A);
                if (v != (int32_t)v)
                    err = load_fail(m, "a relocation out of range", strtab + syms[si].st_name);
                *(int32_t *)p = (int32_t)v;
                break;
            case R_X86_64_32:
                if (S + A > 0xFFFFFFFFUL)
                    err = load_fail(m, "a 32-bit relocation (compile with -mcmodel=kernel)",
                                    strtab + syms[si].st_name);
                *(uint32_t *)p = (uint32_t)(S + A);
                break;
            default: {
                char t[16];
                snprintf(t, sizeof(t), "%u", type);
                err = load_fail(m, "an unsupported relocation type", t);
            }
            }
        }
    }
    kfree(off);
    kfree(commons);
    /* the entry point */
    int (*init)(void) = NULL;
    for (int s = 1; s < nsyms && !err && val; s++)
        if (syms[s].st_shndx != SHN_UNDEF && (syms[s].st_info >> 4) != STB_LOCAL &&
            !strcmp(strtab + syms[s].st_name, "_init"))
            init = (int (*)(void))val[s];
    if (!err && !init)
        err = load_fail(m, "no _init", NULL);
    if (err || !val) {
        pmm_free_contig(pa, pages);
        kfree(val);
        m->state = SIEOS_MOD_FAILED;
        return err ? err : -ENOMEM;
    }
    m->base = base;
    m->mem = total;
    m->syms = syms;                              /* (the image stays: in the archive, or kept) */
    m->strtab = strtab;
    m->symval = val;
    m->nsyms = nsyms;
    m->state = SIEOS_MOD_LOADED;
    __asm__ volatile("wbinvd" ::: "memory");     /* (the code written: fetched from memory) */
    kprintf("drv: %s loaded at %p (%lu KiB; %s)\n", m->name, base, (total + 1023) / 1024, m->source);
    int r = init();
    if (r < 0)
        kprintf("drv: %s: _init: %d\n", m->name, r);
    return 0;
}

/* ---------------------------------------------------------------- devices */

/* The driver for a device: its names, the most specific first. */
static struct module *match(const char *const *names, int n, int phase, int *which)
{
    for (int k = 0; k < n; k++)
        for (int i = 0; i < nmods; i++)
            if (mods[i].phase <= phase && has_alias(&mods[i], names[k])) {
                *which = k;
                return &mods[i];
            }
    return NULL;
}

/* A device's driver: matched (the alias its first device matched kept), loaded once. */
static bool attach(const char *const *names, int n, int phase)
{
    int which;
    struct module *m = match(names, n, phase, &which);
    if (!m)
        return false;
    if (!m->ndev++)
        strlcpy(m->alias, names[which], sizeof(m->alias));
    if (m->state == SIEOS_MOD_KNOWN)
        mod_load(m);
    return true;
}

void modules_attach(int phase)
{
    static uint8_t done[PCI_MAX];                /* (a device's driver attached: not again) */
    static bool i8042_done;
    for (int i = 0; i < pci_count() && i < PCI_MAX; i++) {
        if (done[i])
            continue;
        const struct pci_dev *d = pci_at(i);
        char a[4][32];
        snprintf(a[0], 32, "pci%x,%x", d->vendor, d->device);
        snprintf(a[1], 32, "pci%x,class%02x", d->vendor, d->class_code);   /* (a vendor's devices of a class) */
        snprintf(a[2], 32, "pciclass,%02x%02x%02x", d->class_code, d->subclass, d->prog_if);
        snprintf(a[3], 32, "pciclass,%02x%02x", d->class_code, d->subclass);
        const char *names[4] = { a[0], a[1], a[2], a[3] };
        done[i] = attach(names, 4, phase);
    }
    /* the platform's devices: the i8042 (its ports do not read all ones) */
    if (!i8042_done && inb(0x64) != 0xFF) {
        const char *names[1] = { "platform,i8042" };
        i8042_done = attach(names, 1, phase);
    }
}

/* One file of /drv on the root. */
static int root_file(void *arg, const char *name, size_t len, uint64_t ino, int dtype, uint64_t next)
{
    (void)ino, (void)dtype, (void)next;
    int *count = arg;
    char path[80];
    if (len < 5 || len > 60 || memcmp(name + len - 4, ".drv", 4))
        return 0;
    snprintf(path, sizeof(path), "/drv/%.*s", (int)len, name);
    int err;
    struct inode *ip = namei(path, &err);
    if (!ip)
        return 0;
    uint64_t size = inode_size(ip);
    uint8_t *img = size && size < (16UL << 20) ? kmalloc(size) : NULL;
    if (img && readi(ip, img, 0, size) == (long)size) {
        int before = nmods;
        mod_add(img, size, "/drv");
        if (nmods > before)
            (*count)++;
        else
            kfree(img);                          /* (known already, or not a driver) */
    } else {
        kfree(img);
    }
    iput(ip);
    return 0;
}

void modules_root(void)
{
    int err, n = 0;
    struct inode *dir = namei("/drv", &err);
    if (dir) {
        uint64_t off = 0;
        vfs_readdir(dir, &off, root_file, &n);
        iput(dir);
    }
    if (n)
        kprintf("drv: /drv: %d more driver%s\n", n, n == 1 ? "" : "s");
    modules_attach(DDI_PHASE_ROOT);
}

/* modload: a driver file, then its devices; loaded even if none matches (a pseudo-driver). */
int modload_path(const char *path)
{
    int err;
    struct inode *ip = namei(path, &err);
    if (!ip)
        return err;
    uint64_t size = inode_size(ip);
    uint8_t *img = size && size < (16UL << 20) ? kmalloc(size) : NULL;
    if (!img || readi(ip, img, 0, size) != (long)size) {
        iput(ip);
        kfree(img);
        return -EIO;
    }
    iput(ip);
    int before = nmods;
    struct module *m = mod_add(img, size, "modload");
    if (!m) {
        kfree(img);
        return -ENOEXEC;
    }
    if (nmods == before)
        kfree(img);                              /* (known already: that one) */
    if (m->state == SIEOS_MOD_LOADED)
        return -EEXIST;
    modules_attach(DDI_PHASE_ROOT);
    if (m->state == SIEOS_MOD_KNOWN)
        mod_load(m);
    return m->state == SIEOS_MOD_LOADED ? 0 : -ENOEXEC;
}

int modinfo_get(int i, struct sieos_modinfo *mi)
{
    if (i < 0 || i >= nmods)
        return -ENOENT;
    const struct module *m = &mods[i];
    memset(mi, 0, sizeof(*mi));
    strlcpy(mi->mi_name, m->name, sizeof(mi->mi_name));
    strlcpy(mi->mi_desc, m->desc, sizeof(mi->mi_desc));
    mi->mi_state = m->state;
    mi->mi_phase = m->phase;
    mi->mi_base = (unsigned long)m->base;
    mi->mi_size = m->mem;
    strlcpy(mi->mi_source, m->source, sizeof(mi->mi_source));
    strlcpy(mi->mi_alias, m->alias, sizeof(mi->mi_alias));
    mi->mi_ndev = m->ndev;
    return 0;
}
