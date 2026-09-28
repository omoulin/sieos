/*
 * core.c - Core files (see abi/include/sieos/procfs.h): an ELF64 ET_CORE
 * image of a process killed by a core-action signal, written to "core" in
 * its working directory when RLIMIT_CORE allows it.
 */
#include "proc.h"
#include "mm.h"
#include "fs.h"
#include "elf.h"
#include "abi2.h"
#include "sieos/procfs.h"
#include "sieos/time.h"

#define ET_CORE  4
#define PT_NOTE  4
#define MAXSEG   512

struct seg { uint64_t start, end; uint32_t flags; };

/* The present user pages, as runs with the same permissions. */
static int collect(uint64_t pml4, struct seg *segs)
{
    int n = 0;
    uint64_t *l4 = P2V(pml4);
    for (int i4 = 0; i4 < 256; i4++) {                   /* the lower half: user space */
        if (!(l4[i4] & PTE_P))
            continue;
        uint64_t *l3 = P2V(l4[i4] & PTE_ADDR);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(l3[i3] & PTE_P))
                continue;
            uint64_t *l2 = P2V(l3[i3] & PTE_ADDR);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(l2[i2] & PTE_P))
                    continue;
                uint64_t *l1 = P2V(l2[i2] & PTE_ADDR);
                for (int i1 = 0; i1 < 512; i1++) {
                    uint64_t e = l1[i1];
                    if (!(e & PTE_P) || !(e & PTE_U))
                        continue;                        /* absent, or PROT_NONE */
                    uint64_t va = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) | ((uint64_t)i2 << 21) |
                                  ((uint64_t)i1 << 12);
                    uint32_t fl = PF_R | ((e & (PTE_W | PTE_WANTW)) ? PF_W : 0) | ((e & pte_nx) ? 0 : PF_X);
                    if (n && segs[n - 1].end == va && segs[n - 1].flags == fl) {
                        segs[n - 1].end += PAGE_SIZE;
                    } else if (n < MAXSEG) {
                        segs[n].start = va;
                        segs[n].end = va + PAGE_SIZE;
                        segs[n].flags = fl;
                        n++;
                    }
                }
            }
        }
    }
    return n;
}

struct out {
    struct inode *ip;
    uint64_t off, limit;
    bool full;
};

static void put(struct out *o, const void *src, size_t n)
{
    if (o->full)
        return;
    if (o->off + n > o->limit) {
        n = o->limit - o->off;
        o->full = true;
    }
    if (n && writei(o->ip, src, o->off, n) != (long)n)
        o->full = true;
    o->off += n;
}

static void note(struct out *o, const char *name, uint32_t type, const void *desc, uint32_t size)
{
    uint32_t hdr[3] = { strlen(name) + 1, size, type };
    char nm[8] = { 0 };
    memcpy(nm, name, hdr[0]);
    put(o, hdr, sizeof(hdr));
    put(o, nm, (hdr[0] + 3) & ~3u);
    put(o, desc, size);
    static const char pad[4];
    put(o, pad, ((size + 3) & ~3u) - size);
}

static uint32_t note_size(const char *name, uint32_t size)
{
    return 12 + ((strlen(name) + 1 + 3) & ~3u) + ((size + 3) & ~3u);
}

/* "core" in the working directory, empty; NULL if it cannot be made. */
static struct inode *core_file(struct proc *p)
{
    int err;
    struct inode *ip = namei_at(p->cwd, "core", NAMEI_NOFOLLOW, &err);
    if (ip) {
        if (!S_ISREG(inode_mode(ip)) || inode_uid(ip) != p->euid || inode_permission(ip, W_OK) < 0 ||
            itruncate(ip, 0) < 0) {
            iput(ip);
            return NULL;
        }
        return ip;
    }
    if (err != -ENOENT || inode_permission(p->cwd, W_OK | X_OK) < 0)
        return NULL;
    if (vfs_create(p->cwd, "core", S_IFREG | (0600 & ~p->umask), 0, p->euid, p->egid, &ip) < 0)
        return NULL;
    return ip;
}

bool core_dump(int sig, const struct trapframe *tf)
{
    struct proc *p = current;
    uint64_t limit = p->rlim_cur[SIEOS_RLIMIT_CORE];
    if (limit == 0 || p->uid != p->euid || p->gid != p->egid || !p->cwd)
        return false;                                    /* no core for set-id processes */
    struct seg *segs = kmalloc(MAXSEG * sizeof(*segs));
    uint8_t *page = kmalloc(PAGE_SIZE);
    struct {
        sieos_pstatus_t st;
        sieos_psinfo_t ps;
        struct sieos_core_regs regs;
    } *n = kzalloc(sizeof(*n));
    struct inode *ip = segs && page && n ? core_file(p) : NULL;
    if (!ip) {
        kfree(segs);
        kfree(page);
        kfree(n);
        return false;
    }
    int nseg = collect(p->pml4, segs);
    procfs_pstatus(p, &n->st);
    procfs_psinfo(p, &n->ps);
    n->regs.cr_lwpid = curlwp->lwpid;
    n->regs.cr_sig = sig;
    sig_tf_to_gregs(tf, n->regs.cr_gregs, curlwp->fsbase);

    struct out o = { ip, 0, limit == SIEOS_RLIM_INFINITY ? ~0UL : limit, false };
    uint32_t nsz = note_size("CORE", sizeof(n->st)) + note_size("CORE", sizeof(n->ps)) +
                   note_size("SIEOS", sizeof(n->regs));
    uint64_t hdrs = sizeof(Elf64_Ehdr) + (1 + nseg) * sizeof(Elf64_Phdr);
    uint64_t data = PAGE_ALIGN_UP(hdrs + nsz);
    Elf64_Ehdr eh = { .e_magic = ELF_MAGIC, .e_class = ELFCLASS64, .e_data = 1, .e_version0 = 1,
                      .e_type = ET_CORE, .e_machine = EM_X86_64, .e_version = 1,
                      .e_phoff = sizeof(Elf64_Ehdr), .e_ehsize = sizeof(Elf64_Ehdr),
                      .e_phentsize = sizeof(Elf64_Phdr), .e_phnum = 1 + nseg };
    put(&o, &eh, sizeof(eh));
    Elf64_Phdr ph = { .p_type = PT_NOTE, .p_offset = hdrs, .p_filesz = nsz, .p_align = 4 };
    put(&o, &ph, sizeof(ph));
    uint64_t at = data;
    for (int i = 0; i < nseg; i++) {
        uint64_t len = segs[i].end - segs[i].start;
        Elf64_Phdr lp = { .p_type = PT_LOAD, .p_flags = segs[i].flags, .p_offset = at,
                          .p_vaddr = segs[i].start, .p_filesz = len, .p_memsz = len, .p_align = PAGE_SIZE };
        put(&o, &lp, sizeof(lp));
        at += len;
    }
    note(&o, "CORE", SIEOS_NT_PSTATUS, &n->st, sizeof(n->st));
    note(&o, "CORE", SIEOS_NT_PSINFO, &n->ps, sizeof(n->ps));
    note(&o, "SIEOS", SIEOS_NT_LWPREGS, &n->regs, sizeof(n->regs));
    memset(page, 0, PAGE_SIZE);
    put(&o, page, data - o.off);
    for (int i = 0; i < nseg && !o.full; i++)
        for (uint64_t va = segs[i].start; va < segs[i].end && !o.full; va += PAGE_SIZE) {
            memcpy(page, (void *)va, PAGE_SIZE);         /* our own address space, present pages */
            put(&o, page, PAGE_SIZE);
        }
    iput(ip);
    kfree(segs);
    kfree(page);
    kfree(n);
    return true;
}
