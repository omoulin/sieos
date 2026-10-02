/*
 * nvgpu.c - NVIDIA GeForce RTX 50 GPUs (Blackwell: GB202 to GB207), for
 * compute: GSP-RM, NVIDIA's resource manager running on the GPU's own
 * RISC-V processor (GSP), started and spoken to as Linux's nouveau does it.
 *
 * Stages (milestone 76), each logged ("nvgpu: ..."), so that a machine's
 * hardware report shows how far the GPU got:
 *   1. the GPU (PCI class 3, vendor 10de), its registers (BAR0), the chip
 *      (PMC_BOOT_0), its memory (0x1183a4), the security processor (FSP)
 *      done with its own boot (the THERM scratch register);
 *   2. GSP-RM 570.144 (/lib/firmware/nvidia/gb202/gsp: fmc, bootloader,
 *      gsp): its image in DMA memory under a three-level "radix3" table,
 *      the bootloader, LIBOS's logs, the command and message queues (shared
 *      memory, 4 KiB elements with a checksum), the RM arguments, the WPR
 *      metadata; the system information and the registry queued as RPCs;
 *      then the FSP given the FMC image and its boot parameters (a "chain of
 *      trust" message, MCTP and NVDM headers, in its EMEM), the GSP's
 *      lockdown released, GSP-RM's INIT_DONE;
 *   3. GSP-RM's static information (its internal client, the memory
 *      regions).
 * Channels, memory and /dev/nvgpu0 come next.
 *
 * The registers, structures and sequences are NVIDIA's (the GPU's
 * interface, as nouveau and NVIDIA's open kernel modules document them;
 * the RM structures in nvrm/ are NVIDIA's MIT-licensed excerpts, as nouveau
 * carries them); the driver is written for SIEOS.  The GPU's own firmware
 * does what is not in here.  Not tested on the hardware yet.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "pci.h"
#include "mm.h"
#include "fs.h"
#include "ddi.h"
#include "arch.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#include "nvrm/r535_extras.h"
#include "nvrm/r570_gsp.h"
#include "nvrm/r570_rpcfn.h"
#include "nvrm/r570_msgfn.h"
#include "nvgpu.h"

#define GSP_PAGE_SIZE  0x1000
#define GSP_PAGE_SHIFT 12

/* registers (BAR0) */
#define PMC_BOOT_0         0x000000
#define FB_VIDMEM_SIZE_MB  0x1183a4               /* GA102 and later: the VRAM in MiB */
#define THERM_I2CS_SCRATCH 0x00ad00bc             /* GB202: the FSP's boot status, 0xff when done */
#define GSP_FALCON         0x110000
#define   FLCN_MAILBOX0      0x040
#define   FLCN_MAILBOX1      0x044
#define   FLCN_APP_VERSION   0x080               /* (the bootloader's appVersion, written before INIT_DONE) */
#define   FLCN_HWCFG2        0x0f4
#define     HWCFG2_RISCV_BR_PRIV_LOCKDOWN (1U << 13)
#define   FLCN_DOORBELL      0xc00               /* the command queue's doorbell */
#define   RISCV_CPUCTL       (0x1000 + 0x388)
#define     CPUCTL_ACTIVE      0x80
#define FSP_FALCON         0x8f2000
#define   FSP_EMEMC0         0xac0
#define   FSP_EMEMD0         0xac4
#define FSP_QUEUE_HEAD0    0x8f2c00
#define FSP_QUEUE_TAIL0    0x8f2c04
#define FSP_MSGQ_HEAD0     0x8f2c80
#define FSP_MSGQ_TAIL0     0x8f2c84
#define PCI_CFG_MIRROR     0x092000              /* GH100 and later: PCI configuration space in BAR0 */

/* GSP-RM 570's WPR layout for GB20x (nouveau's r570_wpr_libos3_gb20x) */
#define WPR_OS_CARVEOUT    (22U << 20)           /* GSP_FW_HEAP_PARAM_OS_SIZE_LIBOS3_BAREMETAL */
#define WPR_BASE_SIZE      (14U << 20)           /* GSP_FW_HEAP_PARAM_BASE_RM_SIZE_GH100 */
#define WPR_HEAP_NON_WPR   0x220000U
#define WPR_RSVD_PMU       ((0x0800000U + 0x1000000U + 0x0001000U + 0x1FFFFU) & ~0x1FFFFU)

#define ALIGN_UP(x, a)     (((x) + (a) - 1) & ~((uint64_t)(a) - 1))

/* DMA memory: physically contiguous, zeroed */
struct dmem {
    uint64_t pa;
    void *va;
    size_t size;
};

/* a message queue element (nouveau's r535_gsp_msg) and an RPC's header (nvfw_gsp_rpc) */
struct gsp_msg {
    u8 auth_tag[16], aad[16];
    u32 checksum, sequence, elem_count, pad;
    u8 data[];
};
struct gsp_rpc {
    u32 header_version, signature, length, function, rpc_result, rpc_result_private, sequence, spare;
    u8 data[];
};
#define MSG_HDR     (sizeof(struct gsp_msg))
#define MSG_MAX     (GSP_PAGE_SIZE * 16)

static struct {
    struct pci_dev pci;
    volatile uint8_t *regs;
    uint64_t bar_pa[3];                          /* BAR0 (registers), BAR1 (VRAM), the instance BAR */
    u32 boot0, chipset;
    uint64_t vram;
    const char *state;
    /* the firmware */
    struct dmem fmc_img, boot_fw, sig, radix0, radix1, radix2;
    uint64_t *fw_pages;                          /* the image's pages (physical addresses) */
    uint64_t fw_len, fw_npages;
    u8 fmc_hash[48], fmc_pkey[97], fmc_sig[96];
    u32 boot_code_off, boot_data_off, boot_manifest_off, app_version;
    /* LIBOS, the queues, the arguments */
    struct dmem libos, loginit, logintr, logrm, rmargs, shm, wpr_meta, fmc_args;
    msgqTxHeader *cmdq_tx;
    volatile u32 *cmdq_wptr, *cmdq_rptr, *msgq_wptr, *msgq_rptr;
    u8 *cmdq, *msgq;
    u32 qcnt, cmd_seq, rpc_seq;
    bool running;
    u8 rx[MSG_MAX];                              /* a received RPC (header and payload) */
    /* GSP-RM's static information */
    u32 h_client, h_device, h_subdevice;
    struct sieos_nvgpu_info info;                /* /dev/nvgpu0's (dev.c) */
    bool info_ok;
} nv = { .state = "not found" };

static inline u32 rd32(u32 r) { return *(volatile u32 *)(nv.regs + r); }
static inline void wr32(u32 r, u32 v) { *(volatile u32 *)(nv.regs + r) = v; }

static void udelay(unsigned us)
{
    uint64_t end = hrtime() + (uint64_t)us * 1000;
    while (hrtime() < end)
        __asm__ volatile("pause");
}

static bool fail(const char *step)
{
    kprintf("nvgpu: %s\n", step);
    nv.state = step;
    return false;
}

static bool dmem_alloc(struct dmem *m, size_t size)
{
    size = ALIGN_UP(size, PAGE_SIZE);
    m->pa = pmm_alloc_contig(size / PAGE_SIZE);
    if (!m->pa)
        return false;
    m->va = P2V(m->pa);
    m->size = size;
    memset(m->va, 0, size);
    return true;
}

/* ---------------------------------------------------------------- the firmware files */

static u8 *read_file(const char *path, uint64_t *size)
{
    int err;
    struct inode *ip = namei(path, &err);
    if (!ip) {
        kprintf("nvgpu: %s: missing\n", path);
        return NULL;
    }
    *size = inode_size(ip);
    u8 *buf = *size && *size < 16 * 1024 * 1024 ? kmalloc(*size) : NULL;
    if (buf && readi(ip, buf, 0, *size) != (long)*size) {
        kfree(buf);
        buf = NULL;
    }
    iput(ip);
    if (!buf)
        kprintf("nvgpu: %s: cannot be read (%lu bytes)\n", path, (unsigned long)*size);
    return buf;
}

struct elf32_shdr_ { u32 name, type, flags, addr, offset, size, link, info, align, entsize; };
struct elf64_shdr_ { u32 name, type; u64 flags, addr, offset, size; u32 link, info; u64 align, entsize; };

/* A section of the FMC (ELF32: hash, signature, publickey, image). */
static const u8 *elf32_section(const u8 *elf, uint64_t len, const char *name, u32 *size)
{
    u32 shoff = *(const u32 *)(elf + 32);
    u16 shnum = *(const u16 *)(elf + 48), shstrndx = *(const u16 *)(elf + 50);
    if (memcmp(elf, "\x7f" "ELF\x01\x01\x01", 7) || shoff + (uint64_t)shnum * 40 > len || shstrndx >= shnum)
        return NULL;
    const struct elf32_shdr_ *sh = (const void *)(elf + shoff);
    const char *names = (const char *)elf + sh[shstrndx].offset;
    for (u16 i = 1; i < shnum; i++)
        if (!strcmp(names + sh[i].name, name) && (uint64_t)sh[i].offset + sh[i].size <= len) {
            *size = sh[i].size;
            return elf + sh[i].offset;
        }
    return NULL;
}

/* A section of GSP-RM's image (ELF64): its offset and size in the file. */
static bool elf64_section(struct inode *ip, uint64_t flen, const char *name, uint64_t *off, uint64_t *size)
{
    u8 eh[64];
    if (readi(ip, eh, 0, 64) != 64 || memcmp(eh, "\x7f" "ELF\x02\x01", 6))
        return false;
    u64 shoff = *(u64 *)(eh + 40);
    u16 shnum = *(u16 *)(eh + 60), shstrndx = *(u16 *)(eh + 62);
    if (shnum > 64 || shoff + (uint64_t)shnum * 64 > flen || shstrndx >= shnum)
        return false;
    struct elf64_shdr_ sh[64];
    if (readi(ip, sh, shoff, shnum * 64) != shnum * 64)
        return false;
    char names[512];
    size_t nl = sh[shstrndx].size < sizeof(names) - 1 ? sh[shstrndx].size : sizeof(names) - 1;
    if (readi(ip, names, sh[shstrndx].offset, nl) != (long)nl)
        return false;
    names[nl] = 0;
    for (u16 i = 1; i < shnum; i++)
        if (sh[i].name < nl && !strcmp(names + sh[i].name, name) && sh[i].offset + sh[i].size <= flen) {
            *off = sh[i].offset;
            *size = sh[i].size;
            return true;
        }
    return false;
}

#define FW_DIR "/lib/firmware/nvidia/gb202/gsp/"

/* The FMC (signed by NVIDIA, checked by the FSP): its image in DMA memory, its hash, key and signature. */
static bool load_fmc(void)
{
    uint64_t len;
    u8 *f = read_file(FW_DIR "fmc-570.144.bin", &len);
    if (!f)
        return fail("the FMC firmware is missing");
    u32 hl = 0, sl = 0, kl = 0, il = 0;
    const u8 *hash = elf32_section(f, len, "hash", &hl), *sig = elf32_section(f, len, "signature", &sl);
    const u8 *pkey = elf32_section(f, len, "publickey", &kl), *img = elf32_section(f, len, "image", &il);
    bool ok = hash && sig && pkey && img && hl == 48 && sl == 96 && kl == 97 && dmem_alloc(&nv.fmc_img, il);
    if (ok) {
        memcpy(nv.fmc_img.va, img, il);
        memcpy(nv.fmc_hash, hash, 48);
        memcpy(nv.fmc_pkey, pkey, 97);
        memcpy(nv.fmc_sig, sig, 96);
        kprintf("nvgpu: FMC: image %u bytes at %#lx\n", il, (unsigned long)nv.fmc_img.pa);
    }
    kfree(f);
    return ok || fail("the FMC firmware is not as expected (hash 48, signature 96, key 97 bytes)");
}

/* The bootloader (nvfw_bin_hdr, then RM_RISCV_UCODE_DESC): its data in DMA memory. */
static bool load_bootloader(void)
{
    uint64_t len;
    u8 *f = read_file(FW_DIR "bootloader-570.144.bin", &len);
    if (!f)
        return fail("the GSP bootloader is missing");
    const u32 *hdr = (const u32 *)f;             /* magic, version, size, header offset, data offset, data size */
    bool ok = len >= 24 && (hdr[0] & 0xffff) == 0x10de && hdr[3] + sizeof(RM_RISCV_UCODE_DESC) <= len &&
              (uint64_t)hdr[4] + hdr[5] <= len && dmem_alloc(&nv.boot_fw, hdr[5]);
    if (ok) {
        const RM_RISCV_UCODE_DESC *d = (const void *)(f + hdr[3]);
        memcpy(nv.boot_fw.va, f + hdr[4], hdr[5]);
        nv.boot_fw.size = hdr[5];
        nv.boot_code_off = d->monitorCodeOffset;
        nv.boot_data_off = d->monitorDataOffset;
        nv.boot_manifest_off = d->manifestOffset;
        nv.app_version = d->appVersion;
        kprintf("nvgpu: bootloader: %u bytes, app version %u\n", hdr[5], d->appVersion);
    }
    kfree(f);
    return ok || fail("the GSP bootloader is not as expected");
}

/*
 * GSP-RM's image (.fwimage, 64 MB) a page at a time into DMA memory, its signature for this
 * chip (.fwsignature_gb20x), and the radix3 table: level 0 points at level 1, whose entries
 * point at level 2's pages, whose entries point at the image's pages.
 */
static bool load_gsp_image(void)
{
    int err;
    struct inode *ip = namei(FW_DIR "gsp-570.144.bin", &err);
    if (!ip)
        return fail("the GSP-RM firmware is missing");
    uint64_t flen = inode_size(ip), off, size, soff, ssize;
    bool ok = elf64_section(ip, flen, ".fwimage", &off, &size) &&
              elf64_section(ip, flen, ".fwsignature_gb20x", &soff, &ssize);
    if (!ok) {
        iput(ip);
        return fail("the GSP-RM firmware has no image or no GB20x signature");
    }
    nv.fw_len = size;
    nv.fw_npages = (size + GSP_PAGE_SIZE - 1) / GSP_PAGE_SIZE;
    uint64_t l2_bytes = ALIGN_UP(nv.fw_npages * 8, GSP_PAGE_SIZE);
    ok = l2_bytes / GSP_PAGE_SIZE <= 512 && dmem_alloc(&nv.radix0, GSP_PAGE_SIZE) &&
         dmem_alloc(&nv.radix1, GSP_PAGE_SIZE) && dmem_alloc(&nv.radix2, l2_bytes) &&
         dmem_alloc(&nv.sig, ALIGN_UP(ssize, 256)) && readi(ip, nv.sig.va, soff, ssize) == (long)ssize;
    if (ok)
        nv.sig.size = ssize;
    uint64_t *l2 = ok ? nv.radix2.va : NULL;
    for (uint64_t i = 0; ok && i < nv.fw_npages; i++) {
        uint64_t pa = pmm_alloc();
        uint64_t n = size - i * GSP_PAGE_SIZE < GSP_PAGE_SIZE ? size - i * GSP_PAGE_SIZE : GSP_PAGE_SIZE;
        if (!pa || readi(ip, P2V(pa), off + i * GSP_PAGE_SIZE, n) != (long)n)
            ok = false;
        else
            l2[i] = pa;
    }
    iput(ip);
    if (!ok)
        return fail("no memory for GSP-RM's image (or it cannot be read)");
    *(uint64_t *)nv.radix0.va = nv.radix1.pa;
    uint64_t *l1 = nv.radix1.va;
    for (uint64_t p = 0; p < l2_bytes / GSP_PAGE_SIZE; p++)
        l1[p] = nv.radix2.pa + p * GSP_PAGE_SIZE;
    kprintf("nvgpu: GSP-RM: image %lu bytes (%lu pages), signature %lu bytes\n", (unsigned long)size,
            (unsigned long)nv.fw_npages, (unsigned long)ssize);
    return true;
}

/* ---------------------------------------------------------------- the queues and the RPCs */

static uint64_t libos_id8(const char *name)
{
    uint64_t id = 0;
    for (int i = 0; i < 8 && *name; i++, name++)
        id = (id << 8) | (u8)*name;
    return id;
}

static bool log_region(struct dmem *m, LibosMemoryRegionInitArgument *a, const char *name)
{
    if (!dmem_alloc(m, 0x10000))
        return false;
    a->id8 = libos_id8(name);
    a->pa = m->pa;
    a->size = m->size;
    a->kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
    a->loc = LIBOS_MEMORY_REGION_LOC_SYSMEM;
    uint64_t *pte = (uint64_t *)m->va + 1;       /* (word 0: the put pointer; then the pages) */
    for (size_t i = 0; i < m->size / GSP_PAGE_SIZE; i++)
        pte[i] = m->pa + i * GSP_PAGE_SIZE;
    return true;
}

/* LIBOS's arguments: the three log buffers and the RM arguments; the shared queues. */
static bool libos_init(void)
{
    if (!dmem_alloc(&nv.libos, GSP_PAGE_SIZE))
        return false;
    LibosMemoryRegionInitArgument *args = nv.libos.va;
    if (!log_region(&nv.loginit, &args[0], "LOGINIT") || !log_region(&nv.logintr, &args[1], "LOGINTR") ||
        !log_region(&nv.logrm, &args[2], "LOGRM"))
        return false;
    /* the queues: their page table, the command queue, the message (status) queue, 256 KiB each */
    u32 qsize = 0x40000, nptes = (2 * qsize) >> GSP_PAGE_SHIFT;
    nptes += (nptes * 8 + GSP_PAGE_SIZE - 1) / GSP_PAGE_SIZE;
    u32 ptes_size = ALIGN_UP(nptes * 8, GSP_PAGE_SIZE);
    if (!dmem_alloc(&nv.shm, ptes_size + 2 * qsize))
        return false;
    uint64_t *ptes = nv.shm.va;
    for (u32 i = 0; i < nptes; i++)
        ptes[i] = nv.shm.pa + (uint64_t)i * GSP_PAGE_SIZE;
    nv.cmdq = (u8 *)nv.shm.va + ptes_size;
    nv.msgq = nv.cmdq + qsize;
    struct { msgqTxHeader tx; msgqRxHeader rx; } *cq = (void *)nv.cmdq, *mq = (void *)nv.msgq;
    cq->tx.version = 0;
    cq->tx.size = qsize;
    cq->tx.entryOff = GSP_PAGE_SIZE;
    cq->tx.msgSize = GSP_PAGE_SIZE;
    cq->tx.msgCount = (qsize - GSP_PAGE_SIZE) / GSP_PAGE_SIZE;
    cq->tx.writePtr = 0;
    cq->tx.flags = 1;
    cq->tx.rxHdrOff = __builtin_offsetof(__typeof__(*cq), rx.readPtr);
    nv.qcnt = cq->tx.msgCount;
    nv.cmdq_wptr = &cq->tx.writePtr;
    nv.cmdq_rptr = &mq->rx.readPtr;              /* (GSP-RM's progress through our commands) */
    nv.msgq_wptr = &mq->tx.writePtr;
    nv.msgq_rptr = &cq->rx.readPtr;              /* (ours through its messages) */
    /* the RM arguments (r570) */
    if (!dmem_alloc(&nv.rmargs, GSP_PAGE_SIZE))
        return false;
    GSP_ARGUMENTS_CACHED *ra = nv.rmargs.va;
    ra->messageQueueInitArguments.sharedMemPhysAddr = nv.shm.pa;
    ra->messageQueueInitArguments.pageTableEntryCount = nptes;
    ra->messageQueueInitArguments.cmdQueueOffset = nv.cmdq - (u8 *)nv.shm.va;
    ra->messageQueueInitArguments.statQueueOffset = nv.msgq - (u8 *)nv.shm.va;
    ra->bDmemStack = 1;
    args[3].id8 = libos_id8("RMARGS");
    args[3].pa = nv.rmargs.pa;
    args[3].size = nv.rmargs.size;
    args[3].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
    args[3].loc = LIBOS_MEMORY_REGION_LOC_SYSMEM;
    return true;
}

/* An RPC into the command queue: the element's checksum (the 64-bit words' XOR, folded), its pages. */
static bool cmdq_push(u32 fn, const void *payload, u32 plen, bool noseq)
{
    u32 rlen = sizeof(struct gsp_rpc) + plen, len = ALIGN_UP(MSG_HDR + rlen, GSP_PAGE_SIZE);
    if (len > MSG_MAX)
        return fail("an RPC too large for one element");
    static u8 buf[MSG_MAX];
    memset(buf, 0, len);
    struct gsp_msg *m = (void *)buf;
    struct gsp_rpc *r = (void *)m->data;
    r->header_version = 0x03000000;
    r->signature = ('C' << 24) | ('P' << 16) | ('R' << 8) | 'V';
    r->function = fn;
    r->rpc_result = 0xffffffff;
    r->rpc_result_private = 0xffffffff;
    r->length = rlen;
    r->sequence = noseq ? 0 : nv.rpc_seq++;
    memcpy(r->data, payload, plen);
    m->sequence = nv.cmd_seq++;
    m->elem_count = len / GSP_PAGE_SIZE;
    uint64_t csum = 0;
    for (u32 i = 0; i < len / 8; i++)
        csum ^= ((uint64_t *)buf)[i];
    m->checksum = (u32)(csum >> 32) ^ (u32)csum;
    u32 wptr = *nv.cmdq_wptr, off = 0;
    while (off < len) {
        int free = 0;
        for (int t = 0; t < 1000000; t++) {
            free = (int)(*nv.cmdq_rptr + nv.qcnt - wptr - 1);
            if (free >= (int)nv.qcnt)
                free -= nv.qcnt;
            if (free >= 1)
                break;
            udelay(1);
        }
        if (free < 1)
            return fail("the command queue stays full");
        u32 step = free < (int)(nv.qcnt - wptr) ? (u32)free : nv.qcnt - wptr;
        u32 n = len - off < step * GSP_PAGE_SIZE ? len - off : step * GSP_PAGE_SIZE;
        memcpy(nv.cmdq + GSP_PAGE_SIZE + wptr * GSP_PAGE_SIZE, buf + off, n);
        wptr += n / GSP_PAGE_SIZE;
        if (wptr == nv.qcnt)
            wptr = 0;
        off += n;
    }
    __sync_synchronize();
    *nv.cmdq_wptr = wptr;
    __sync_synchronize();
    if (nv.running)
        wr32(GSP_FALCON + FLCN_DOORBELL, 0);
    return true;
}

/* The next message from GSP-RM into nv.rx (its RPC header and payload); false after ms. */
static bool msgq_recv(unsigned ms)
{
    uint64_t end = hrtime() + (uint64_t)ms * 1000000;
    u32 rptr;
    for (;;) {
        rptr = *nv.msgq_rptr;
        if (*nv.msgq_wptr != rptr)
            break;
        if (hrtime() > end)
            return false;
        udelay(10);
    }
    struct gsp_msg *m = (void *)(nv.msgq + GSP_PAGE_SIZE + rptr * GSP_PAGE_SIZE);
    struct gsp_rpc *r = (void *)m->data;
    u32 len = r->length;
    if (len < sizeof(*r) || len > sizeof(nv.rx))
        len = sizeof(*r);
    u32 first = (nv.qcnt - rptr) * GSP_PAGE_SIZE - MSG_HDR;
    if (first >= len) {
        memcpy(nv.rx, m->data, len);
    } else {                                     /* (it wraps to the queue's first element) */
        memcpy(nv.rx, m->data, first);
        memcpy(nv.rx + first, nv.msgq + GSP_PAGE_SIZE, len - first);
    }
    rptr = (rptr + (ALIGN_UP(len + MSG_HDR, GSP_PAGE_SIZE) / GSP_PAGE_SIZE)) % nv.qcnt;
    __sync_synchronize();
    *nv.msgq_rptr = rptr;
    return true;
}

static struct gsp_rpc *rx_rpc(void) { return (struct gsp_rpc *)nv.rx; }

/* GSP-RM's CPU sequencer: the register commands it asks for during its boot. */
static void run_cpu_sequencer(const u32 *p, u32 n)
{
    if (n < 2)
        return;
    u32 count = p[1];                            /* (bufferSizeDWord, cmdIndex, regSaveArea[8], commands) */
    const u32 *c = p + 2 + 8, *e = c + count;
    while (c < e && c < p + n) {
        u32 op = c[0];
        switch (op) {
        case 0: wr32(c[1], c[2]); c += 3; break;                               /* REG_WRITE */
        case 1: wr32(c[1], (rd32(c[1]) & ~c[2]) | c[3]); c += 4; break;         /* REG_MODIFY (addr, mask, val) */
        case 2: {                                                              /* REG_POLL (addr, mask, val, timeout, error) */
            u32 us = c[4] ? c[4] : 4000000;
            for (u32 t = 0; t < us && (rd32(c[1]) & c[2]) != c[3]; t += 10)
                udelay(10);
            c += 6;
            break;
        }
        case 3: udelay(c[1]); c += 2; break;                                   /* DELAY_US */
        case 4: c += 3; break;                                                 /* REG_STORE */
        default:
            kprintf("nvgpu: the CPU sequencer asked for operation %u (not done)\n", op);
            return;
        }
    }
}

/* The messages that are not answers: logged (and the sequencer run). */
static void event(struct gsp_rpc *r)
{
    u32 n = r->length - sizeof(*r);
    switch (r->function) {
    case NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER:
        kprintf("nvgpu: GSP-RM runs the CPU sequencer\n");
        run_cpu_sequencer((const u32 *)r->data, n / 4);
        break;
    case NV_VGPU_MSG_EVENT_OS_ERROR_LOG:
        kprintf("nvgpu: GSP-RM error: Xid %u\n", n >= 4 ? *(const u32 *)r->data : 0);
        break;
    case NV_VGPU_MSG_EVENT_GSP_LOCKDOWN_NOTICE:
    case NV_VGPU_MSG_EVENT_GSP_POST_NOCAT_RECORD:
    case NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT:
    case NV_VGPU_MSG_EVENT_POST_EVENT:
        break;
    default:
        kprintf("nvgpu: GSP-RM message %#x (%u bytes)\n", r->function, n);
    }
}

/* The answer to function fn (or the event fn), the events before it handled; ms at most. */
static struct gsp_rpc *wait_for(u32 fn, unsigned ms)
{
    uint64_t end = hrtime() + (uint64_t)ms * 1000000;
    while (hrtime() < end) {
        if (!msgq_recv(10))
            continue;
        struct gsp_rpc *r = rx_rpc();
        if (r->function == fn) {
            if (r->rpc_result)
                kprintf("nvgpu: RPC %u: result %#x\n", fn, r->rpc_result);
            return r;
        }
        event(r);
    }
    return NULL;
}

/* ---------------------------------------------------------------- the RPCs queued before the boot */

static bool set_system_info(void)
{
    static GspSystemInfo info;
    memset(&info, 0, sizeof(info));
    const struct pci_dev *pd = &nv.pci;
    info.gpuPhysAddr = nv.bar_pa[0];
    info.gpuPhysFbAddr = nv.bar_pa[1];
    info.gpuPhysInstAddr = nv.bar_pa[2];
    info.nvDomainBusDeviceFunc = (pd->bus << 8) | (pd->dev << 3) | pd->func;
    info.maxUserVa = 0x00007ffffffff000ULL;
    info.pciConfigMirrorBase = PCI_CFG_MIRROR;
    info.pciConfigMirrorSize = 0x1000;
    info.PCIDeviceID = (u32)pd->device << 16 | pd->vendor;
    u32 sub = pci_read32(pd->bus, pd->dev, pd->func, 0x2c);
    info.PCISubDeviceID = (sub >> 16) << 16 | (sub & 0xffff);
    info.PCIRevisionID = pd->revision;
    info.bIsPrimary = 0;                         /* (the display is on the Intel GPU) */
    info.bPreserveVideoMemoryAllocations = 0;
    return cmdq_push(NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO, &info, sizeof(info), true);
}

/* The registry: the entries GSP-RM needs (nouveau's r535_registry_entries). */
static bool set_registry(void)
{
    static const struct { const char *key; u32 val; } keys[] = {
        { "RMSecBusResetEnable", 1 }, { "RMForcePcieConfigSave", 1 }, { "RMDevidCheckIgnore", 1 },
    };
    static u8 buf[512];
    memset(buf, 0, sizeof(buf));
    PACKED_REGISTRY_TABLE *t = (void *)buf;
    u32 n = sizeof(keys) / sizeof(keys[0]), off = sizeof(*t) + n * sizeof(PACKED_REGISTRY_ENTRY);
    t->numEntries = n;
    for (u32 i = 0; i < n; i++) {
        size_t kl = strlen(keys[i].key) + 1;
        t->entries[i].nameOffset = off;
        t->entries[i].type = REGISTRY_TABLE_ENTRY_TYPE_DWORD;
        t->entries[i].data = keys[i].val;
        t->entries[i].length = 4;
        memcpy(buf + off, keys[i].key, kl);
        off += kl;
    }
    t->size = off;
    return cmdq_push(NV_VGPU_MSG_FUNCTION_SET_REGISTRY, buf, off, true);
}

/* The WPR metadata (what GSP-RM's boot puts where in VRAM) and the FMC's boot parameters. */
static bool boot_params(uint32_t *rsvd_size)
{
    if (!dmem_alloc(&nv.wpr_meta, sizeof(GspFwWprMeta)) || !dmem_alloc(&nv.fmc_args, sizeof(GSP_FMC_BOOT_PARAMS)))
        return false;
    GspFwWprMeta *m = nv.wpr_meta.va;
    uint64_t fb_gb = (nv.vram + (1ULL << 30) - 1) >> 30;
    uint64_t heap = WPR_OS_CARVEOUT + WPR_BASE_SIZE + ALIGN_UP(GSP_FW_HEAP_PARAM_SIZE_PER_GB_FB * fb_gb, 1 << 20) +
                    ALIGN_UP(GSP_FW_HEAP_PARAM_CLIENT_ALLOC_SIZE, 1 << 20);
    m->magic = GSP_FW_WPR_META_MAGIC;
    m->revision = GSP_FW_WPR_META_REVISION;
    m->sizeOfRadix3Elf = nv.fw_len;
    m->sysmemAddrOfRadix3Elf = nv.radix0.pa;
    m->sizeOfBootloader = nv.boot_fw.size;
    m->sysmemAddrOfBootloader = nv.boot_fw.pa;
    m->bootloaderCodeOffset = nv.boot_code_off;
    m->bootloaderDataOffset = nv.boot_data_off;
    m->bootloaderManifestOffset = nv.boot_manifest_off;
    m->sysmemAddrOfSignature = nv.sig.pa;
    m->sizeOfSignature = nv.sig.size;
    m->nonWprHeapSize = WPR_HEAP_NON_WPR;
    m->gspFwHeapSize = heap;
    m->frtsSize = 0x100000;
    m->vgaWorkspaceSize = 128 * 1024;
    m->pmuReservedSize = WPR_RSVD_PMU;
    GSP_FMC_BOOT_PARAMS *a = nv.fmc_args.va;
    a->bootGspRmParams.gspRmDescOffset = nv.wpr_meta.pa;
    a->bootGspRmParams.gspRmDescSize = sizeof(GspFwWprMeta);
    a->bootGspRmParams.target = GSP_DMA_TARGET_COHERENT_SYSTEM;
    a->bootGspRmParams.bIsGspRmBoot = 1;
    a->gspRmParams.target = GSP_DMA_TARGET_NONCOHERENT_SYSTEM;
    a->gspRmParams.bootArgsOffset = nv.libos.pa;
    *rsvd_size = ALIGN_UP(WPR_HEAP_NON_WPR + WPR_RSVD_PMU, 0x200000);
    kprintf("nvgpu: WPR: GSP-RM heap %lu MiB, reserved %u MiB\n", (unsigned long)(heap >> 20), *rsvd_size >> 20);
    return true;
}

/* ---------------------------------------------------------------- the FSP: the chain of trust */

static void fsp_emem_write(const u8 *p, u32 len)
{
    wr32(FSP_FALCON + FSP_EMEMC0, (1U << 24) | 0);   /* (write, auto-increment, from offset 0) */
    for (u32 i = 0; i < len; i += 4)
        wr32(FSP_FALCON + FSP_EMEMD0, *(const u32 *)(p + i));
}

static void fsp_emem_read(u8 *p, u32 len)
{
    wr32(FSP_FALCON + FSP_EMEMC0, (1U << 25) | 0);   /* (read, auto-increment) */
    for (u32 i = 0; i < len; i += 4)
        *(u32 *)(p + i) = rd32(FSP_FALCON + FSP_EMEMD0);
}

#define MCTP_SOM (1U << 31)
#define MCTP_EOM (1U << 30)
#define NVDM_HEADER(type) (0x7eU | (0x10deU << 8) | ((u32)(type) << 24))
#define NVDM_COT 0x14
#define NVDM_FSP_RESPONSE 0x15

struct __attribute__((packed)) cot_msg {
    u32 mctp, nvdm;
    u16 version, size;
    u64 gspFmcSysmemOffset, frtsSysmemOffset;
    u32 frtsSysmemSize;
    u64 frtsVidmemOffset;
    u32 frtsVidmemSize;
    u32 hash384[12], publicKey[96], signature[96];
    u64 gspBootArgsSysmemOffset;
};

static bool fsp_boot_gsp(uint32_t rsvd_size)
{
    static struct cot_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.mctp = MCTP_SOM | MCTP_EOM;
    msg.nvdm = NVDM_HEADER(NVDM_COT);
    msg.version = 2;                             /* (GB202's FSP) */
    msg.size = sizeof(msg) - 8;
    msg.gspFmcSysmemOffset = nv.fmc_img.pa;
    msg.frtsVidmemOffset = ALIGN_UP(rsvd_size, 0x200000);
    msg.frtsVidmemSize = 0x100000;
    memcpy(msg.hash384, nv.fmc_hash, 48);
    memcpy(msg.publicKey, nv.fmc_pkey, 97);
    memcpy(msg.signature, nv.fmc_sig, 96);
    msg.gspBootArgsSysmemOffset = nv.fmc_args.pa;
    u32 len = (sizeof(msg) + 3) & ~3U;
    /* the queue free (a message before ours consumed), the message, the pointers */
    int t;
    for (t = 0; t < 1000 && rd32(FSP_QUEUE_HEAD0) != rd32(FSP_QUEUE_TAIL0); t++)
        udelay(1000);
    if (t == 1000)
        return fail("the FSP's queue stays busy");
    fsp_emem_write((const u8 *)&msg, len);
    wr32(FSP_QUEUE_TAIL0, len - 4);              /* (the last word written) */
    wr32(FSP_QUEUE_HEAD0, 0);
    kprintf("nvgpu: FSP: chain of trust sent (%u bytes)\n", len);
    for (t = 0; t < 2000 && rd32(FSP_MSGQ_HEAD0) == rd32(FSP_MSGQ_TAIL0); t++)
        udelay(1000);
    if (t == 2000)
        return fail("the FSP did not answer the chain of trust");
    u32 rlen = rd32(FSP_MSGQ_TAIL0) - rd32(FSP_MSGQ_HEAD0) + 4;
    u32 reply[5] = { 0 };
    fsp_emem_read((u8 *)reply, rlen < sizeof(reply) ? rlen : sizeof(reply));
    wr32(FSP_MSGQ_TAIL0, 0);
    wr32(FSP_MSGQ_HEAD0, 0);
    kprintf("nvgpu: FSP: answer %08x %08x task %u type %#x error %#x\n", reply[0], reply[1], reply[2], reply[3],
            reply[4]);
    if (reply[1] != NVDM_HEADER(NVDM_FSP_RESPONSE) || reply[3] != NVDM_COT || reply[4])
        return fail("the FSP refused GSP-RM's chain of trust");
    return true;
}

/* ---------------------------------------------------------------- the boot */

static bool gsp_boot(void)
{
    if (!load_fmc() || !load_bootloader() || !load_gsp_image())
        return false;
    if (!libos_init())
        return fail("no memory for LIBOS and the queues");
    if (!set_system_info() || !set_registry())
        return false;
    uint32_t rsvd;
    if (!boot_params(&rsvd))
        return fail("no memory for the boot parameters");
    __asm__ volatile("wbinvd" ::: "memory");
    if (!fsp_boot_gsp(rsvd))
        return false;
    /* the FMC: GSP's lockdown released (or its error in the mailboxes) */
    u32 mbox0 = 0, mbox1 = 0;
    int t;
    for (t = 0; t < 4000; t++) {
        mbox0 = rd32(GSP_FALCON + FLCN_MAILBOX0);
        if (mbox0 && (mbox0 & 0xffffff00) == 0xbadf4100) {
            udelay(1000);
            continue;
        }
        if (mbox0) {
            mbox1 = rd32(GSP_FALCON + FLCN_MAILBOX1);
            if (((uint64_t)mbox1 << 32 | mbox0) != nv.fmc_args.pa)
                break;
        }
        if (!(rd32(GSP_FALCON + FLCN_HWCFG2) & HWCFG2_RISCV_BR_PRIV_LOCKDOWN))
            break;
        udelay(1000);
    }
    if (t == 4000)
        return fail("GSP-FMC: the boot timed out (lockdown not released)");
    if (mbox0 && ((uint64_t)mbox1 << 32 | mbox0) != nv.fmc_args.pa) {
        kprintf("nvgpu: GSP-FMC: boot failed, mailbox %08x %08x\n", mbox1, mbox0);
        return fail("GSP-FMC: the boot failed");
    }
    kprintf("nvgpu: GSP-FMC: lockdown released after %d ms\n", t);
    wr32(GSP_FALCON + FLCN_APP_VERSION, nv.app_version);
    if (!(rd32(GSP_FALCON + RISCV_CPUCTL) & CPUCTL_ACTIVE))
        return fail("GSP: its RISC-V is not running");
    nv.running = true;
    if (!wait_for(NV_VGPU_MSG_EVENT_GSP_INIT_DONE, 10000)) {
        kprintf("nvgpu: GSP-RM: log put pointers: init %lu, rm %lu\n", (unsigned long)*(uint64_t *)nv.loginit.va,
                (unsigned long)*(uint64_t *)nv.logrm.va);
        return fail("GSP-RM: no INIT_DONE in 10 s");
    }
    kprintf("nvgpu: GSP-RM is running\n");
    return true;
}

/* Stage 3: GSP-RM's static information. */
static void static_info(void)
{
    static GspStaticConfigInfo req;              /* (sent zeroed, at its size: it comes back filled) */
    memset(&req, 0, sizeof(req));
    if (!cmdq_push(NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO, &req, sizeof(req), false))
        return;
    struct gsp_rpc *r = wait_for(NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO, 2000);
    if (!r || r->length < sizeof(*r) + sizeof(GspStaticConfigInfo)) {
        fail("GSP-RM: no static information");
        return;
    }
    const GspStaticConfigInfo *si = (const void *)r->data;
    nv.h_client = si->hInternalClient;
    nv.h_device = si->hInternalDevice;
    nv.h_subdevice = si->hInternalSubdevice;
    kprintf("nvgpu: GSP-RM: internal client %#x, device %#x, subdevice %#x\n", nv.h_client, nv.h_device,
            nv.h_subdevice);
    const NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO_PARAMS *fb = &si->fbRegionInfoParams;
    for (u32 i = 0; i < fb->numFBRegions && i < 16; i++)
        kprintf("nvgpu: VRAM region %u: %#lx-%#lx%s%s\n", i, (unsigned long)fb->fbRegion[i].base,
                (unsigned long)fb->fbRegion[i].limit, fb->fbRegion[i].reserved ? " reserved" : "",
                fb->fbRegion[i].bProtected ? " protected" : "");
    nv.state = "GSP-RM running";
}

/* ---------------------------------------------------------------- for /dev/nvgpu0 */

const struct sieos_nvgpu_info *nvgpu_info(void)
{
    return nv.info_ok ? &nv.info : NULL;
}

uint64_t nvgpu_timestamp(void)
{
    u32 hi, lo;
    do {                                         /* (PTIMER: TIME_1 then TIME_0, until TIME_1 holds) */
        hi = rd32(0x009410);
        lo = rd32(0x009400);
    } while (hi != rd32(0x009410));
    return (uint64_t)hi << 32 | lo;
}

/* What /dev/nvgpu0 tells: the chip, its classes (GB20x's), its memory, the VA space processes have. */
static void info_init(void)
{
    static const char *const chips[] = { "GB202", "GB203", "GB204", "GB205", "GB206", "GB207" };
    struct sieos_nvgpu_info *in = &nv.info;
    const struct pci_dev *pd = &nv.pci;
    memset(in, 0, sizeof(*in));
    in->version = 1;
    in->chipset = nv.chipset;
    in->device_id = pd->device;
    u32 sub = pci_read32(pd->bus, pd->dev, pd->func, 0x2c);
    in->subvendor_id = sub & 0xffff;
    in->subdevice_id = sub >> 16;
    in->revision = pd->revision;
    in->pci_bus = pd->bus, in->pci_dev = pd->dev, in->pci_func = pd->func;
    in->cls_copy = 0xcab5;                       /* BLACKWELL_DMA_COPY_B */
    in->cls_eng2d = 0x902d;                      /* FERMI_TWOD_A */
    in->cls_eng3d = 0xce97;                      /* BLACKWELL_B */
    in->cls_m2mf = 0xcd40;                       /* BLACKWELL_INLINE_TO_MEMORY_A */
    in->cls_compute = 0xcec0;                    /* BLACKWELL_COMPUTE_B */
    in->cls_gpfifo = 0xca6f;                     /* BLACKWELL_CHANNEL_GPFIFO_B */
    in->vram_size = nv.vram;
    in->bar1_size = pci_bar_size(pd, 1);
    in->va_start = 1ULL << 32;
    in->va_end = 1ULL << 39;                     /* (above: the kernel's, as nouveau keeps it) */
    in->max_push = 512;
    in->bind_align = 0x10000;
    snprintf(in->chip, sizeof(in->chip), "%s", nv.chipset >= 0x1b2 && nv.chipset <= 0x1b7 ? chips[nv.chipset - 0x1b2] : "GB20x");
    snprintf(in->name, sizeof(in->name), "NVIDIA %s (GeForce RTX 50)", in->chip);
    nv.info_ok = true;
}

/* ---------------------------------------------------------------- the device */

static void nvgpu_probe(void)
{
    struct pci_dev *pd = &nv.pci;
    bool found = false;
    for (int i = 0; i < pci_count() && !found; i++) {
        const struct pci_dev *d = pci_at(i);
        if (d->vendor == 0x10de && d->class_code == 0x03)
            *pd = *d, found = true;
    }
    if (!found)
        return;
    kprintf("nvgpu: %02x:%02x.%u: NVIDIA 10de:%04x (class %02x%02x, rev %02x)\n", pd->bus, pd->dev, pd->func,
            pd->device, pd->class_code, pd->subclass, pd->revision);
    /* the BARs: 0 the registers, then VRAM, then the instance memory (64-bit ones take two) */
    int idx = 0;
    for (int b = 0; b < 3 && idx < 6; b++) {
        bool io;
        nv.bar_pa[b] = pci_bar_addr(pd, idx, &io);
        kprintf("nvgpu: BAR%d: %#lx, %lu MiB\n", idx, (unsigned long)nv.bar_pa[b],
                (unsigned long)(pci_bar_size(pd, idx) >> 20));
        idx += (pci_read32(pd->bus, pd->dev, pd->func, 0x10 + idx * 4) & 0x6) == 0x4 ? 2 : 1;
    }
    u32 cmd = pci_read32(pd->bus, pd->dev, pd->func, 4);
    if (cmd == 0xffffffff) {
        fail("the GPU does not answer (powered off?)");
        return;
    }
    pci_enable_path(pd);
    pci_write32(pd->bus, pd->dev, pd->func, 4, cmd | 0x6);       /* memory, bus master */
    if (!nv.bar_pa[0] || !(nv.regs = mmio_map(nv.bar_pa[0], 16 << 20))) {
        fail("no register BAR");
        return;
    }
    nv.boot0 = rd32(PMC_BOOT_0);
    if (nv.boot0 == 0xffffffff) {
        fail("the GPU's registers read all ones (powered off?)");
        return;
    }
    nv.chipset = (nv.boot0 >> 20) & 0x1ff;
    nv.vram = (uint64_t)rd32(FB_VIDMEM_SIZE_MB) << 20;
    kprintf("nvgpu: PMC_BOOT_0 %08x: chipset %03x rev %02x, VRAM %lu MiB\n", nv.boot0, nv.chipset, nv.boot0 & 0xff,
            (unsigned long)(nv.vram >> 20));
    if (nv.chipset < 0x1b2 || nv.chipset > 0x1b7) {
        fail("not a GB20x (GeForce RTX 50): not supported");
        return;
    }
    pci_claim(pd, "nvgpu");
    /* the FSP: done with its own boot (FWSEC and the FRTS region are its work on Blackwell) */
    int t;
    for (t = 0; t < 4000 && rd32(THERM_I2CS_SCRATCH) != 0xff; t++)
        udelay(1000);
    kprintf("nvgpu: FSP: boot status %#x after %d ms\n", rd32(THERM_I2CS_SCRATCH), t);
    if (t == 4000) {
        fail("the FSP did not finish its boot");
        return;
    }
    if (boot_option(ddi_cmdline(), "nonvgpu")) {
        nv.state = "off (nonvgpu)";
        return;
    }
    if (gsp_boot())
        static_info();
    info_init();                                 /* (/dev/nvgpu0 tells what it knows, GSP-RM up or not) */
    nvgpu_dev_init();
}

const char *nvgpu_state(void)
{
    return nv.state;
}

DDI_DRIVER("nvgpu", DDI_PHASE_ROOT, "NVIDIA GeForce RTX 50 (Blackwell GB20x): GSP-RM, for compute");
DDI_ALIAS("pci10de,class03");                  /* (NVIDIA's display controllers: VGA or 3D) */

int _init(void)
{
    nvgpu_probe();
    return 0;
}
