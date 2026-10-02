/*
 * iwlwifi.c - Intel Wi-Fi 6 AX201 (the "22000" family: the Qu MAC with the
 * Hr radio, integrated in Ice Lake as CNVi: 8086:34F0, and in Comet/Tiger
 * Lake) and Wi-Fi 6E AX210 (the AX210 family: the Ty MAC with the Gf radio,
 * a PCIe card: 8086:2725): the firmware, its configuration, scanning.
 *
 * The device runs Intel's firmware (/lib/firmware/iwlwifi-Qu-<step>-hr-b0-77.ucode,
 * from linux-firmware, chosen by the MAC's stepping), which the driver
 * hands over in a "context info" structure: the firmware's sections in
 * DMA memory (LMAC, UMAC, paging, as the file's separators divide them),
 * the receive ring (free and used buffer descriptors, the status the
 * firmware writes), and the command queue.  Then UREG_CPU_INIT_RUN starts
 * the device's CPUs, which copy the firmware in themselves; the first thing
 * it sends back is the ALIVE notification (status 0xCAFE), in the first
 * receive buffer.
 *
 * The AX210 takes the "gen3" context info instead: a peripheral scratch area
 * (the firmware's sections, the free receive buffers, later the PNVM), the
 * image loader (the file's IML section) at CSR_IML_DATA_ADDR, 16-byte free
 * and 32-byte used receive descriptors, one packet a buffer, the UMAC's
 * peripheral registers 0x300000 higher.  After its ALIVE, the platform NVM
 * (iwlwifi-ty-a0-gf-a0.pnvm: the section for the SKU the ALIVE names) is
 * handed over (UREG_DOORBELL_TO_ISR6) before the NVM is read.  Its transmit
 * command has a 28-byte header, its byte count table counts bytes.
 *
 * Steps: the card is made ready (CSR_HW_IF_CONFIG NIC_READY), reset, its
 * clocks started (APM: INIT_DONE, MAC_CLOCK_READY), configured (the MAC's
 * and the radio's steps), the context info set (CSR_CTXT_INFO_BA), the
 * CPUs started; the ALIVE awaited (polled: no interrupt).  Each step is
 * logged, and a failure says which.
 *
 * After ALIVE: the commands (one at a time on queue 0), the NVM (the MAC
 * address, the channels), the runtime configuration, then scans on demand
 * (the wifi() system call); the notifications and frames are read by
 * wifi_poll, from the network timer; joining a network (open or WPA2-
 * Personal, the handshake in wpa.c's cryptography, the frames' CCMP in the
 * device) and the iwx0 interface.  "nowifi" on the command line leaves the device
 * alone.  The registers, the context info and the firmware file's format
 * are Intel's (the device's interface, as the open drivers document it:
 * OpenBSD's iwx, FreeBSD's iwlwifi); the driver is written for SIEOS.  Not
 * tested on the hardware (QEMU has no such device).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include "pci.h"
#include "mm.h"
#include "fs.h"
#include "ddi.h"
#include "net.h"
#include "sieos/sysinfo.h"
#include "sieos/errno.h"
#include "random.h"
#include "wpa.h"

/* CSR registers (BAR0) */
#define CSR_HW_IF_CONFIG   0x000
#define   HW_IF_NIC_READY    (1U << 22)
#define   HW_IF_PREPARE      (1U << 27)
#define CSR_INT            0x008
#define CSR_INT_MASK       0x00C
#define CSR_FH_INT_STATUS  0x010
#define CSR_RESET          0x020
#define   RESET_SW           (1U << 7)
#define CSR_GP_CNTRL       0x024
#define   GP_MAC_CLOCK_READY (1U << 0)
#define   GP_INIT_DONE       (1U << 2)
#define   GP_MAC_ACCESS_REQ  (1U << 3)
#define   GP_GOING_TO_SLEEP  (1U << 4)
#define   GP_RFKILL_HW       (1U << 27)           /* set: the radio switch is on */
#define CSR_HW_REV         0x028
#define CSR_CTXT_INFO_BA   0x040
#define CSR_UCODE_DRV_GP1_CLR 0x05C
#define CSR_HW_RF_ID       0x09C
#define CSR_INT_COALESCING 0x004
#define CSR_GIO_REG        0x03C
#define   GIO_L0S_DISABLED   0x2
#define CSR_MBOX_SET       0x088
#define   MBOX_OS_ALIVE      0x20
#define CSR_MAC_SHADOW_CTRL 0x0A8
#define CSR_GIO_CHICKEN    0x100
#define CSR_DBG_HPET_MEM   0x240
#define CSR_DBG_LINK_PWR_MGMT 0x250
#define HW_IF_HAP_WAKE_L1A (1U << 19)
#define INT_ALIVE          (1U << 0)
#define INT_FH_RX          (1U << 31)
/* the power manager (HPM), in the peripheral space */
#define HPM_DEBUG          0xA03440
#define   PERSISTENCE_BIT    (1U << 12)
#define PREG_PRPH_WPROT    0xA04D00
#define   PREG_WFPM_ACCESS   (1U << 12)
#define HPM_HIPM_GEN_CFG   0xA03458
#define   HIPM_PG_EN         (1U << 0)
#define   HIPM_SLP_EN        (1U << 1)
#define   HIPM_FORCE_ACTIVE  (1U << 10)
#define HPM_UMAC_LTR       0xA03480
#define HPM_MAC_LTR_CSR    0xA0348C
#define   HPM_LTR_ENABLE_ALL 0xF
/* the LTR the firmware boots with (about 250 us, snoop and no snoop; it sets its own later) */
#define BOOT_LTR           0x82FA82FA
#define HBUS_PRPH_WADDR    0x444
#define HBUS_PRPH_RADDR    0x448
#define HBUS_PRPH_WDAT     0x44C
#define HBUS_PRPH_RDAT     0x450
#define HBUS_TARG_WRPTR    0x460
#define RFH_Q0_FRBDCB_WIDX_TRG 0x1C80             /* the receive queue's free-buffer write index (22000, AX210) */
#define UREG_CPU_INIT_RUN  0xA05C44
#define UREG_DOORBELL_TO_ISR6 0xA05C04
#define   ISR6_PNVM          (1U << 20)
#define UMAC_PRPH_GEN3     0x300000               /* (the AX210's UMAC registers: above the 22000's) */
/* the AX210's context info (gen3) */
#define CSR_CTXT_INFO_BOOT_CTRL 0x000
#define   AUTO_FUNC_BOOT_ENA (1U << 1)
#define CSR_CTXT_INFO_ADDR 0x118
#define CSR_IML_DATA_ADDR  0x120
#define CSR_IML_SIZE_ADDR  0x128
#define CSR_LTR_LONG_VAL_AD 0x0D4

/* firmware file */
#define TLV_SEC_RT         19
#define TLV_PHY_SKU        22
#define TLV_API_CHANGES    29                     /* (index, 32 bits): what the firmware's commands look like */
#define TLV_CAPABILITIES   30                     /* (index, 32 bits): what it can do */
#define TLV_IML            52                     /* the image loader (AX210) */
#define TLV_HW_TYPE        58                     /* PNVM: the MAC and radio a section is for */
#define TLV_PNVM_SKU       64                     /* PNVM: a SKU's sections start */
#define SEP_CPU1_CPU2      0xFFFFCCCC
#define SEP_PAGING         0xAAAABBBB
#define MAX_SEC            64

#define NRBD      512                             /* the receive ring (4 KiB buffers): the device's table size */
#define RB_CB_LOG 9                               /* log2(NRBD) */
#define NRB_FIRST 8                               /* the buffers given at ALIVE (in eights) */
#define CMDQ_SIZE 256                             /* command queue TFDs (256 bytes each): the device's queue size */
#define CMDQ_CB   5                               /* log2(256) - 3 */
#define TFD_SIZE  256
#define FIRST_TB  20                              /* the first transfer buffer holds at most 20 bytes */
#define RB_SIZE   4096
#define INT_SW_ERR (1U << 25)

/* commands: (group, opcode) */
#define GRP_LEGACY 0x0
#define GRP_LONG   0x1                            /* (group 0's commands are sent as group 1) */
#define GRP_SYSTEM 0x2
#define GRP_NVM    0xC
#define CMD_ALIVE           0x01
#define CMD_INIT_COMPLETE   0x04
#define CMD_INIT_EXT_CFG    0x03                  /* system group: init_flags */
#define   INIT_NVM            (1U << 1)
#define CMD_NVM_ACCESS_DONE 0x00                  /* NVM group */
#define CMD_NVM_GET_INFO    0x02
#define CMD_PNVM_INIT_COMPLETE 0xFE               /* NVM group notification: the PNVM taken */
#define CSR_MAC_ADDR_BASE   0x380                 /* OTP 0x380, 0x384; OEM strap 0x388, 0x38C */
#define GRP_PHY_OPS 0x4
#define CMD_SCAN_CFG        0x0C                  /* (long group) the scan's antennas */
#define CMD_SCAN_REQ_UMAC   0x0D
#define CMD_SCAN_COMPLETE   0x0F                  /* notification: a scan ended */
#define CMD_POWER_TABLE     0x77
#define CMD_TX_ANT_CFG      0x98
#define CMD_BT_CONFIG       0x9B
#define CMD_SCAN_ITER_DONE  0xB5                  /* notification: one pass over the channels */
#define CMD_RX_MPDU         0xC1                  /* a received frame */
#define CMD_MCC_UPDATE      0xC8                  /* the regulatory domain: the channels allowed */
#define CMD_BEACON_FILTER   0xD2
#define CMD_PHY_CONTEXT     0x08
#define CMD_TX              0x1C                  /* (a data queue's frame; its answer: the transmission's status) */
#define CMD_ADD_STA         0x18
#define CMD_REMOVE_STA      0x19
#define CMD_MAC_CONTEXT     0x28
#define CMD_BINDING         0x2B
#define CMD_MISSED_BEACONS  0xA2                  /* notification */
#define CMD_MAC_PM_POWER    0xA9                  /* the connection's power management */
#define CMD_MCAST_FILTER    0xD0
#define CMD_SF_CONFIG       0xD1                  /* the smart FIFO */
#define GRP_MAC_CONF 0x3
#define GRP_DATA_PATH 0x5
#define CMD_SESSION_PROT    0x05                  /* MAC configuration group: stay on the channel while joining */
#define CMD_RLC_CONFIG      0x08                  /* data path group (version 2): the receive chains */
#define CMD_TLC_CONFIG      0x0F                  /* data path group: the firmware's rate selection */
#define CMD_SCD_QUEUE_CFG   0x17                  /* data path group: a transmit queue */
#define CMD_SEC_KEY         0x18                  /* data path group: a key */
#define CMD_SOC_CONFIG      0x01                  /* system group */
#define CMD_TEMP_THRESHOLDS 0x04                  /* phy-ops group */
#define PKT_CMD_FAILED      0x40                  /* (in an answer's group byte) */
/* the firmware's API and capability bits (TLV 29, 30) */
#define API_MCC_UPDATE      9
#define API_NVM_INFO_V4     48                    /* NVM_GET_INFO's channel profile in 32 bits */
#define API_REDUCED_SCAN_CFG 56
#define API_SCAN_EXT_CHAN   58                    /* the scan's channel entries carry the band */
#define CAPA_LAR            1
#define CAPA_DS_PARAM_IE    9
#define CAPA_DQA            12
#define CAPA_LAR_MULTI_MCC  29
#define CAPA_CT_KILL_BY_FW  74
/* the channels, in the order the NVM and the MCC answer list them (14 at 2.4 GHz, 37 at 5 GHz) */
static const uint8_t nvm_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132,
    136, 140, 144, 149, 153, 157, 161, 165, 169, 173, 177, 181,
};
#define NCHAN   (int)sizeof(nvm_channels)
#define NCHAN_2G 14
#define CH_VALID  (1U << 0)
#define CH_ACTIVE (1U << 3)                       /* probing allowed (else listen only) */
#define NBSS      48

/* The context info (22000 family), as the firmware reads it. */
struct ctxt_info {
    uint16_t mac_id, version, size, reserved;
    uint32_t control_flags, reserved_c;
    uint64_t reserved0;
    uint64_t free_rbd_addr, used_rbd_addr, status_wr_ptr;
    uint64_t cmd_queue_addr;
    uint8_t cmd_queue_size, reserved_h[7];
    uint32_t reserved1[4];
    uint64_t core_dump_addr;
    uint32_t core_dump_size, reserved_d;
    uint64_t early_debug_addr;
    uint32_t early_debug_size, reserved_e;
    uint64_t platform_nvm_addr;
    uint32_t platform_nvm_size, reserved_p;
    uint32_t reserved2[16];
    uint64_t umac_img[MAX_SEC], lmac_img[MAX_SEC], virtual_img[MAX_SEC];
    uint32_t reserved3[16];
};
_Static_assert(__builtin_offsetof(struct ctxt_info, free_rbd_addr) == 24, "context info layout");
_Static_assert(__builtin_offsetof(struct ctxt_info, cmd_queue_addr) == 48, "context info layout");
_Static_assert(__builtin_offsetof(struct ctxt_info, umac_img) == 192, "context info layout");

/* The AX210's (gen3) context info, peripheral scratch, receive descriptors. */
struct __attribute__((packed)) ctxt_info_gen3 {
    uint16_t version, size;
    uint32_t config;
    uint64_t prph_info_addr, cr_head_idx_addr, tr_tail_idx_addr, cr_tail_idx_addr, tr_head_idx_addr;
    uint16_t cr_idx_size, tr_idx_size;
    uint64_t mtr_addr, mcr_addr;
    uint16_t mtr_size, mcr_size, mtr_doorbell, mcr_doorbell, mtr_msi, mcr_msi;
    uint8_t mtr_hdr, mtr_ftr, mcr_hdr, mcr_ftr;
    uint16_t rings_flags, prph_info_msi;
    uint64_t prph_scratch_addr;
    uint32_t prph_scratch_size, reserved;
};
_Static_assert(sizeof(struct ctxt_info_gen3) == 104, "gen3 context info layout");
struct fw_dram { uint64_t umac_img[MAX_SEC], lmac_img[MAX_SEC], virtual_img[MAX_SEC]; };
struct __attribute__((packed)) prph_scratch {
    uint16_t mac_id, version, size, reserved;    /* version */
    uint32_t control_flags, reserved_c;          /* control */
    uint64_t pnvm_addr; uint32_t pnvm_size, reserved_p;
    uint64_t hwm_addr; uint32_t hwm_size, debug_token;
    uint64_t free_rbd_addr; uint32_t reserved_r;
    uint64_t reduce_power_addr; uint32_t reduce_power_size, reserved_u;
    uint32_t reserved2[12];
    struct fw_dram dram;
};
_Static_assert(__builtin_offsetof(struct prph_scratch, dram) == 124, "peripheral scratch layout");
#define PRPH_RB_SIZE_4K    (1U << 16)
#define PRPH_MTR_MODE      (1U << 17)
#define PRPH_MTR_FORMAT_256B 0xC0000
struct __attribute__((packed)) rx_transfer_desc { uint16_t rbid, reserved[3]; uint64_t addr; };
#define USED_DESC_GEN3     32                     /* (a completion descriptor: the buffer's id at byte 4) */

#define CTXT_TFD_FORMAT_LONG 0x0100
#define CTXT_RB_CB_SIZE_POS  4
#define CTXT_RB_SIZE_POS     9
#define CTXT_RB_SIZE_4K      0x4

static struct {
    struct pci_dev pci;
    volatile uint8_t *csr;
    uint32_t hw_rev, rf_id;
    struct ctxt_info *ci;
    uint64_t ci_pa;
    uint64_t *free_rbd;
    uint32_t *used_rbd;
    bool gen3;                                   /* the AX210 family */
    bool integrated;                             /* CNVi (the AX201), not a PCIe card */
    struct fw_dram *dram;                        /* where the firmware's sections go (context info, scratch) */
    struct prph_scratch *scratch;
    struct rx_transfer_desc *free_rbd3;
    uint8_t *used_rbd3;
    uint8_t *iml;
    uint32_t iml_len;
    uint32_t sku_id[3];                          /* the ALIVE's: which PNVM section */
    bool pnvm_done;
    volatile uint16_t *rb_status;
    uint64_t rb_pa[NRBD];
    const char *state;
    char fw_name[64];
    /* after ALIVE: the command queue and the receive processing */
    uint8_t *cmdq;
    uint64_t cmdq_pa;
    uint8_t *cmdbuf;                             /* the command being sent (one at a time) */
    uint64_t cmdbuf_pa;
    int cmd_cur;                                 /* the next TFD */
    int rx_cur;                                  /* the next receive buffer to look at */
    bool alive, init_complete;
    int wait_idx;                                /* the command whose answer is awaited, -1 none */
    bool got_resp;
    uint8_t resp[1024];
    uint32_t resp_len;
    uint32_t fw_major, fw_minor;
    uint32_t lmac_err, umac_err;                 /* the firmware's error tables (SRAM), from the ALIVE */
    bool err_dumped;
    uint8_t mac[6];
    uint32_t phy_cfg, api[4], capa[4];           /* the firmware file's */
    uint32_t sku, tx_ant, rx_ant;                /* the NVM's */
    bool lar;
    uint32_t chan_flags[NCHAN];                  /* the channels allowed here (CH_*) */
    int wait_code;
    int busy;                                    /* the device in use (a command, or the poll) */
    bool ready, scanning;
    unsigned scans;
    struct sieos_wifi_bss bss[NBSS];
    uint64_t bss_seen[NBSS];                     /* (ticks) */
    struct bss_info {                            /* what joining a BSS needs from its beacons */
        uint16_t bi, capinfo;                    /* the beacon interval (TU), the capabilities */
        uint8_t dtim;
        uint8_t rates[16], nrates;               /* the supported rates (500 kb/s units, 0x80: basic) */
        uint8_t group[4];                        /* the RSN group cipher suite */
        bool ccmp, psk, mfpr;                    /* pairwise CCMP, PSK key management, management protection required */
        /* 802.11n/ac (HT, VHT) and QoS (WMM), from its elements */
        bool ht, vht, wmm;
        uint16_t ht_cap;                         /* HT capabilities info (bit 1: 40 MHz, 5: SGI 20, 6: SGI 40) */
        uint8_t ampdu, ht_mcs[2];                /* A-MPDU parameters; the MCS it receives: 0-7, 8-15 */
        uint8_t sec_off, ht_prot;                /* HT operation: the secondary channel (1 above, 3 below), protection */
        uint32_t vht_cap;                        /* VHT capabilities info (bit 5: SGI 80) */
        uint16_t vht_mcs;                        /* the VHT MCS map it receives (2 bits a stream) */
        uint8_t vht_width, vht_center;           /* VHT operation: 1 = 80 MHz (and more), the centre channel */
    } bssx[NBSS];
    int nbss;
    volatile uint32_t missed_beacons;
} wf = { .state = "not found", .wait_idx = -1 };

static inline uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(wf.csr + r); }
static inline void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(wf.csr + r) = v; }
static void set_bits(uint32_t r, uint32_t m) { wr(r, rd(r) | m); }

static void udelay(unsigned us)
{
    uint64_t end = hrtime() + (uint64_t)us * 1000;
    while (hrtime() < end)
        __asm__ volatile("pause");
}

static bool poll(uint32_t reg, uint32_t mask, uint32_t want, unsigned us)
{
    for (unsigned t = 0; t < us; t += 10) {
        if ((rd(reg) & mask) == want)
            return true;
        udelay(10);
    }
    return (rd(reg) & mask) == want;
}

static bool grab_nic(void)
{
    set_bits(CSR_GP_CNTRL, GP_MAC_ACCESS_REQ);
    return poll(CSR_GP_CNTRL, GP_MAC_CLOCK_READY | GP_GOING_TO_SLEEP, GP_MAC_CLOCK_READY, 15000);
}

static void release_nic(void)
{
    wr(CSR_GP_CNTRL, rd(CSR_GP_CNTRL) & ~GP_MAC_ACCESS_REQ);
}

static uint32_t prph_mask(void) { return wf.gen3 ? 0x00FFFFFF : 0x000FFFFF; }

static uint32_t read_prph(uint32_t addr)
{
    wr(HBUS_PRPH_RADDR, (addr & prph_mask()) | (3U << 24));
    return rd(HBUS_PRPH_RDAT);
}

static void write_prph(uint32_t addr, uint32_t v)
{
    wr(HBUS_PRPH_WADDR, (addr & prph_mask()) | (3U << 24));  /* (the 22000 family's mask: 0xA05C44 is 0x05C44) */
    wr(HBUS_PRPH_WDAT, v);
}

/* A UMAC peripheral register's address (the AX210's are 0x300000 higher). */
static uint32_t umac(uint32_t addr) { return wf.gen3 ? addr + UMAC_PRPH_GEN3 : addr; }

static bool prph_bits(uint32_t addr, uint32_t set, uint32_t clear)
{
    if (!grab_nic())
        return false;
    write_prph(addr, (read_prph(addr) & ~clear) | set);
    release_nic();
    return true;
}

static bool fail(const char *step)
{
    kprintf("wifi: %s: %s (hw rev %08x, rf %08x, GP_CNTRL %08x, HW_IF %08x)\n", wf.fw_name[0] ? wf.fw_name : "Wi-Fi",
            step, wf.hw_rev, wf.rf_id, rd(CSR_GP_CNTRL), rd(CSR_HW_IF_CONFIG));
    wf.state = step;
    return false;
}

static bool dma_copy(const uint8_t *data, uint32_t len, uint64_t *pa)
{
    uint32_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    *pa = pmm_alloc_contig(pages);
    if (!*pa)
        return false;
    memcpy(P2V(*pa), data, len);
    return true;
}

/* The firmware file into the context info's image tables; the PHY SKU (radio config) in *phy. */
static bool load_firmware(struct inode *ip, uint32_t *phy)
{
    uint64_t size = inode_size(ip);
    if (size < 128 || size > 8 * 1024 * 1024)
        return fail("the firmware file has a bad size");
    uint8_t *fw = kmalloc(size);
    if (!fw)
        return fail("no memory for the firmware");
    bool ok = false;
    if (readi(ip, fw, 0, size) != (long)size || *(uint32_t *)fw != 0 || *(uint32_t *)(fw + 4) != 0x0A4C5749) {
        fail("not an iwlwifi firmware file");
        goto out;
    }
    int part = 0, n[3] = { 0 };                  /* 0 LMAC, 1 UMAC, 2 paging */
    for (uint64_t off = 88; off + 8 <= size;) {
        uint32_t type = *(uint32_t *)(fw + off), len = *(uint32_t *)(fw + off + 4);
        const uint8_t *data = fw + off + 8;
        if (off + 8 + len > size)
            break;
        if (type == TLV_PHY_SKU && len >= 4)
            *phy = *(uint32_t *)data;
        if (type == TLV_IML && len && !wf.iml) {
            uint64_t pa;
            if (!dma_copy(data, len, &pa)) {
                fail("no memory for the image loader");
                goto out;
            }
            wf.iml = P2V(pa);
            wf.iml_len = len;
        }
        if ((type == TLV_API_CHANGES || type == TLV_CAPABILITIES) && len >= 8) {
            uint32_t word = *(const uint32_t *)data;
            if (word < 4)
                (type == TLV_API_CHANGES ? wf.api : wf.capa)[word] = *(const uint32_t *)(data + 4);
        }
        if (type == TLV_SEC_RT && len >= 4) {
            uint32_t dev_off = *(uint32_t *)data;
            if (dev_off == SEP_CPU1_CPU2)
                part = 1;
            else if (dev_off == SEP_PAGING)
                part = 2;
            else if (n[part] < MAX_SEC) {
                uint64_t pa;
                if (!dma_copy(data + 4, len - 4, &pa)) {
                    fail("no memory for the firmware sections");
                    goto out;
                }
                uint64_t *table = part == 0 ? wf.dram->lmac_img : part == 1 ? wf.dram->umac_img : wf.dram->virtual_img;
                table[n[part]++] = pa;
            }
        }
        off += 8 + ((len + 3) & ~3U);
    }
    kprintf("wifi: %s: %d LMAC, %d UMAC, %d paging sections\n", wf.fw_name, n[0], n[1], n[2]);
    ok = n[0] && n[1];
    if (!ok)
        fail("the firmware has no runtime image");
    else if (wf.gen3 && !wf.iml)
        ok = fail("the firmware has no image loader (IML)");
out:
    kfree(fw);
    return ok;
}

static bool card_ready(void)
{
    set_bits(CSR_HW_IF_CONFIG, HW_IF_NIC_READY);
    if (poll(CSR_HW_IF_CONFIG, HW_IF_NIC_READY, HW_IF_NIC_READY, 50)) {
        set_bits(CSR_MBOX_SET, MBOX_OS_ALIVE);   /* (the host driver is there) */
        return true;
    }
    return false;
}

static bool apm_init(void)
{
    set_bits(CSR_GIO_CHICKEN, 1U << 23);         /* L1A_NO_L0S_RX: L0s off, L1 kept */
    set_bits(CSR_DBG_HPET_MEM, 0xFFFF0000);      /* the FH wait threshold at its maximum */
    set_bits(CSR_HW_IF_CONFIG, HW_IF_HAP_WAKE_L1A);
    set_bits(CSR_GIO_REG, GIO_L0S_DISABLED);
    set_bits(CSR_GP_CNTRL, GP_INIT_DONE);        /* D0U -> D0A (powered up, active) */
    return poll(CSR_GP_CNTRL, GP_MAC_CLOCK_READY, GP_MAC_CLOCK_READY, 25000);
}

static void sw_reset(void)
{
    set_bits(CSR_RESET, RESET_SW);
    udelay(5000);
}

/*
 * The device up, as its interface wants it for the integrated 22000
 * family (CNVi): ready, the persistence bit cleared, reset, the power
 * gating forced once and reset again, the power manager (APM).
 */
static bool start(uint32_t phy)
{
    if (!card_ready()) {
        set_bits(CSR_DBG_LINK_PWR_MGMT, 1U << 31);
        udelay(1000);
        bool ok = false;
        for (int n = 0; n < 10 && !ok; n++) {
            set_bits(CSR_HW_IF_CONFIG, HW_IF_PREPARE);
            for (int t = 0; t < 750 && !(ok = card_ready()); t++)
                udelay(200);
            if (!ok)
                udelay(25000);
        }
        if (!ok)
            return fail("the card is not ready (NIC_READY)");
    }
    uint32_t hpm = wf.gen3 ? 0 : read_prph(HPM_DEBUG);   /* (22000 only; read without the NIC lock) */
    if (hpm != 0xA5A5A5A0 && (hpm & PERSISTENCE_BIT)) {
        if (read_prph(PREG_PRPH_WPROT) & PREG_WFPM_ACCESS)
            return fail("the persistence bit cannot be cleared");
        write_prph(HPM_DEBUG, hpm & ~PERSISTENCE_BIT);
        kprintf("wifi: the persistence bit cleared\n");
    }
    sw_reset();
    if (!wf.integrated || wf.gen3)
        goto apm;
    /* integrated: the clocks, the power gating forced once, and a reset again */
    set_bits(CSR_GP_CNTRL, GP_INIT_DONE);
    udelay(20);
    if (!poll(CSR_GP_CNTRL, GP_MAC_CLOCK_READY, GP_MAC_CLOCK_READY, 25000))
        return fail("the MAC clock did not start (before the power gating)");
    if (!prph_bits(HPM_HIPM_GEN_CFG, HIPM_FORCE_ACTIVE, 0))
        return fail("no access for the power gating");
    udelay(20);
    prph_bits(HPM_HIPM_GEN_CFG, HIPM_PG_EN | HIPM_SLP_EN, 0);
    udelay(20);
    prph_bits(HPM_HIPM_GEN_CFG, 0, HIPM_FORCE_ACTIVE);
    sw_reset();
apm:
    if (!apm_init())
        return fail("the MAC clock did not start (APM)");
    if (!(rd(CSR_GP_CNTRL) & GP_RFKILL_HW))
        kprintf("wifi: the radio is switched off (RF kill)\n");

    /* the firmware's start: interrupts off and acknowledged, the RF kill handshake bits cleared */
    wr(CSR_INT, 0xFFFFFFFF);
    wr(CSR_INT_MASK, 0);
    wr(CSR_UCODE_DRV_GP1_CLR, 1U << 1);
    wr(CSR_UCODE_DRV_GP1_CLR, 1U << 2);
    wr(CSR_INT, 0xFFFFFFFF);
    /* the NIC: the power manager again, the steps, interrupt coalescing, the shadow registers */
    if (!apm_init())
        return fail("the MAC clock did not start (NIC init)");
    uint32_t mac_step = (wf.hw_rev >> 2) & 3, mac_dash = wf.hw_rev & 3;
    uint32_t v = mac_step << 2 | mac_dash | (phy & 3) << 10 | ((phy >> 2) & 3) << 14 | ((phy >> 4) & 3) << 12;
    uint32_t mask = 0x3 | 0xC | 0xC00 | 0x3000 | 0xC000 | 0x200 | 0x100;
    wr(CSR_HW_IF_CONFIG, (rd(CSR_HW_IF_CONFIG) & ~mask) | v);
    *(volatile uint8_t *)(wf.csr + CSR_INT_COALESCING) = 0x40;
    set_bits(CSR_MAC_SHADOW_CTRL, 0x800FFFFF);
    wr(CSR_INT_MASK, INT_ALIVE | INT_FH_RX);     /* (polled: the device's causes, not an interrupt line) */
    return true;
}

/* ---------------------------------------------------------------- after ALIVE: receive, commands */

static void rx_frame(const uint8_t *desc, uint32_t len);
static void tx_done(const uint8_t *pkt, uint32_t len);

/* One packet of a receive buffer: the ALIVE, a command's answer, a notification, a frame. */
static void rx_packet(const uint8_t *pkt, uint32_t len)
{
    uint8_t code = pkt[4], group = pkt[5], idx = pkt[6], qid = pkt[7];
    if (!(qid & 0x80) && (qid & 0x7F) == 0 && idx == wf.wait_idx && code == wf.wait_code) {   /* our command's answer */
        uint32_t n = len < sizeof(wf.resp) ? len : sizeof(wf.resp);
        memcpy(wf.resp, pkt, n);
        wf.resp_len = n;
        wf.got_resp = true;
        return;
    }
    if ((group & ~PKT_CMD_FAILED) == GRP_NVM && code == CMD_PNVM_INIT_COMPLETE) {
        wf.pnvm_done = true;
        return;
    }
    if ((group & ~PKT_CMD_FAILED) > GRP_LONG)
        return;
    if (code == CMD_ALIVE && len >= 20) {
        uint16_t status = *(const uint16_t *)(pkt + 8);
        wf.fw_major = *(const uint32_t *)(pkt + 12);
        wf.fw_minor = *(const uint32_t *)(pkt + 16);
        if (len >= 8 + 112) {                    /* (lmac_data[0].dbg_ptrs, umac_data.dbg_ptrs) */
            wf.lmac_err = *(const uint32_t *)(pkt + 8 + 20);
            wf.umac_err = *(const uint32_t *)(pkt + 8 + 108);
        }
        if (len >= 8 + 128)                      /* (version 5 and later: the SKU, for the PNVM) */
            memcpy(wf.sku_id, pkt + 8 + 116, 12);
        wf.alive = status == 0xCAFE;
        kprintf("wifi: the firmware is alive (status %#x, version %u.%x)%s\n", status, wf.fw_major, wf.fw_minor,
                wf.alive ? "" : ": not OK");
    } else if (code == CMD_INIT_COMPLETE) {
        wf.init_complete = true;
    } else if (code == CMD_RX_MPDU) {
        rx_frame(pkt + 8, len - 8);
    } else if (code == CMD_TX && !(qid & 0x80)) {
        tx_done(pkt, len);
    } else if (code == CMD_MISSED_BEACONS && len >= 8 + 12) {
        wf.missed_beacons = *(const uint32_t *)(pkt + 8 + 8);
    } else if (code == CMD_SCAN_COMPLETE && wf.scanning) {
        uint8_t status = len >= 8 + 7 ? pkt[8 + 6] : 0;
        wf.scanning = false;
        wf.scans++;
        kprintf("wifi: scan %u done (status %u): %d network%s\n", wf.scans, status, wf.nbss, wf.nbss == 1 ? "" : "s");
    }
}

/* The receive buffers the firmware closed since the last time; then they are given back. */
static void rx_process(void)
{
    int hw = (wf.rb_status[0] & 0xFFF) & (NRBD - 1);
    if (hw == wf.rx_cur)
        return;
    while (wf.rx_cur != hw) {
        int id = wf.gen3 ? *(const uint16_t *)(wf.used_rbd3 + wf.rx_cur * USED_DESC_GEN3 + 4) & 0xFFF
                         : (int)(wf.used_rbd[wf.rx_cur] & 0xFFF);   /* (the buffer the firmware closed there) */
        const uint8_t *rb = P2V(wf.rb_pa[id < NRBD ? id : wf.rx_cur]);
        for (uint32_t off = 0; off + 8 < RB_SIZE;) {
            const uint8_t *pkt = rb + off;
            uint32_t lnf = *(const uint32_t *)pkt;
            if (lnf == 0x55550000 || (lnf == 0 && pkt[4] == 0 && pkt[6] == 0 && (pkt[7] & 0x7F) == 0))
                break;                           /* (no more packets in this buffer) */
            uint32_t len = 4 + (lnf & 0x3FFF);
            if (len < 8 || len > RB_SIZE - off)
                break;
            rx_packet(pkt, len);
            if (wf.gen3)
                break;                           /* (the AX210: one packet a buffer) */
            off += (len + 63) & ~63U;            /* packets at 64-byte boundaries */
        }
        memset((void *)rb, 0, 16);               /* (a stale header must not look new next time) */
        wf.rx_cur = (wf.rx_cur + 1) % NRBD;
    }
    int give = hw == 0 ? NRBD - 1 : hw - 1;      /* the buffers back to the firmware (by eights) */
    wr(RFH_Q0_FRBDCB_WIDX_TRG, give & ~7);
}

/* The firmware's error tables (after SW_ERR): what it asserted on, and the last command it took. */
#define HBUS_TARG_MEM_RADDR 0x40C
#define HBUS_TARG_MEM_RDAT  0x41C
static void read_mem(uint32_t addr, uint32_t *v, int n)
{
    wr(HBUS_TARG_MEM_RADDR, addr);
    for (int i = 0; i < n; i++)
        v[i] = rd(HBUS_TARG_MEM_RDAT);
}

static void dump_errors(void)
{
    if (wf.err_dumped || !grab_nic())
        return;
    wf.err_dumped = true;
    uint32_t u[15] = { 0 }, l[25] = { 0 };
    if (wf.umac_err >= 0x400000)
        read_mem(wf.umac_err, u, 15);
    if (wf.lmac_err >= 0x400000)
        read_mem(wf.lmac_err, l, 25);
    release_nic();
    /* UMAC: valid, error_id, blink1, blink2, ilink1, ilink2, data1-3, major, minor, fp, sp, last command, isr */
    kprintf("wifi: UMAC error %08x (valid %x) blink %08x %08x ilink %08x %08x data %08x %08x %08x last cmd %08x\n",
            u[1], u[0], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[13]);
    /* LMAC: valid, error_id, trm status 0-1, blink2, ilink1-2, data1-3, ..., hcmd (23) */
    if (u[1] == 0x201002FD)                      /* (a command of the wrong size: data1 its id, data2 wanted, data3 got) */
        kprintf("wifi: the firmware refused command %02x.%02x: %u bytes expected, %u given\n", (u[6] >> 8) & 0xFF,
                u[6] & 0xFF, u[7], u[8]);
    kprintf("wifi: LMAC error %08x (valid %x) trm %08x %08x blink2 %08x ilink %08x %08x data %08x %08x %08x hcmd %08x\n",
            l[1], l[0], l[2], l[3], l[4], l[5], l[6], l[7], l[8], l[9], l[23]);
}

/* Wait (polling) for a condition, up to ms; the firmware's error ends it. */
static bool wait_for(volatile bool *flag, unsigned ms)
{
    for (unsigned t = 0; t < ms * 10; t++) {
        rx_process();
        if (*flag)
            return true;
        if (rd(CSR_INT) & INT_SW_ERR) {
            kprintf("wifi: the firmware reported an error (CSR_INT %08x)\n", rd(CSR_INT));
            dump_errors();
            return false;
        }
        udelay(100);
    }
    return false;
}

/* A command (wide header) with its payload; its answer in wf.resp.  0, or < 0. */
static int send_cmd_v(uint8_t group, uint8_t opcode, uint8_t ver, const void *data, uint16_t len)
{
    if (len > PAGE_SIZE - 8)
        return -1;
    if (group == GRP_LEGACY)
        group = GRP_LONG;
    int idx = wf.cmd_cur;
    uint8_t *c = wf.cmdbuf;
    c[0] = opcode, c[1] = group, c[2] = idx, c[3] = 0;           /* the command queue: 0 */
    *(uint16_t *)(c + 4) = len;
    c[6] = 0, c[7] = ver;                                       /* reserved, the command's version */
    memcpy(c + 8, data, len);
    uint32_t total = 8 + len;
    uint8_t *tfd = wf.cmdq + idx * TFD_SIZE;
    memset(tfd, 0, TFD_SIZE);
    uint16_t l0 = total < FIRST_TB ? total : FIRST_TB;
    uint64_t a0 = wf.cmdbuf_pa, a1 = wf.cmdbuf_pa + FIRST_TB;
    memcpy(tfd + 2, &l0, 2);
    memcpy(tfd + 4, &a0, 8);
    uint16_t ntb = 1;
    if (total > FIRST_TB) {
        uint16_t l1 = total - FIRST_TB;
        memcpy(tfd + 12, &l1, 2);
        memcpy(tfd + 14, &a1, 8);
        ntb = 2;
    }
    memcpy(tfd, &ntb, 2);
    __asm__ volatile("sfence" ::: "memory");
    wf.cmd_cur = (wf.cmd_cur + 1) % CMDQ_SIZE;
    wf.wait_idx = idx;
    wf.wait_code = opcode;
    wf.got_resp = false;
    wr(HBUS_TARG_WRPTR, 0U << 16 | wf.cmd_cur);                 /* queue 0, the next index */
    bool ok = wait_for(&wf.got_resp, 1000);
    wf.wait_idx = -1;
    if (!ok) {
        kprintf("wifi: command %02x.%02x: no answer\n", group, opcode);
        return -1;
    }
    if (wf.resp[5] & PKT_CMD_FAILED) {
        kprintf("wifi: command %02x.%02x: the firmware refused it\n", group, opcode);
        return -1;
    }
    return 0;
}

static int send_cmd(uint8_t group, uint8_t opcode, const void *data, uint16_t len)
{
    return send_cmd_v(group, opcode, 0, data, len);
}

/* The device, for a command sequence or the poll (which only tries). */
static bool dev_lock(bool wait)
{
    while (__atomic_exchange_n(&wf.busy, 1, __ATOMIC_ACQUIRE)) {
        if (!wait)
            return false;
        __builtin_ia32_pause();
    }
    return true;
}

static void dev_unlock(void)
{
    __atomic_store_n(&wf.busy, 0, __ATOMIC_RELEASE);
}

static bool has_api(int bit) { return wf.api[bit / 32] >> (bit % 32) & 1; }
static bool has_capa(int bit) { return wf.capa[bit / 32] >> (bit % 32) & 1; }

/* The antennas the firmware file and the NVM both allow. */
static uint32_t valid_tx(void)
{
    uint32_t a = (wf.phy_cfg >> 16) & 0xF;
    if (wf.tx_ant & 0xF)
        a &= wf.tx_ant;
    return a ? a : 1;
}

static uint32_t valid_rx(void)
{
    uint32_t a = (wf.phy_cfg >> 20) & 0xF;
    if (wf.rx_ant & 0xF)
        a &= wf.rx_ant;
    return a ? a : 1;
}

static bool mac_valid(const uint8_t *m)
{
    static const uint8_t reserved[6] = { 0x02, 0xCC, 0xAA, 0xFF, 0xEE, 0x00 }, zero[6] = { 0 };
    return memcmp(m, reserved, 6) && memcmp(m, zero, 6) && !(m[0] & 1) &&
           !(m[0] == 0xFF && m[1] == 0xFF && m[2] == 0xFF && m[3] == 0xFF && m[4] == 0xFF && m[5] == 0xFF);
}

static void mac_from(uint32_t lo, uint32_t hi, uint8_t *m)
{
    m[0] = lo >> 24, m[1] = lo >> 16, m[2] = lo >> 8, m[3] = lo;
    m[4] = hi >> 8, m[5] = hi;
}

/* A command whose failure stops the initialisation: logged and in the state. */
static bool init_cmd(uint8_t group, uint8_t opcode, const void *data, uint16_t len, const char *what)
{
    if (send_cmd(group, opcode, data, len) == 0)
        return true;
    static char why[64];
    snprintf(why, sizeof(why), "firmware: %s failed", what);
    wf.state = why;
    return false;
}

/* The regulatory domain: "ZZ" (the world), which the firmware refines from the country it hears (LAR).
 * Its answer lists every channel's flags, as the NVM does. */
static void update_mcc(void)
{
    struct __attribute__((packed)) { uint16_t mcc; uint8_t source, reserved; uint32_t key, reserved2[5]; } c = { 0 };
    c.mcc = 'Z' << 8 | 'Z';
    c.source = has_api(API_MCC_UPDATE) || has_capa(CAPA_LAR_MULTI_MCC) ? 0x10 : 0;   /* get current / old firmware */
    if (send_cmd(GRP_LONG, CMD_MCC_UPDATE, &c, sizeof(c)) < 0)
        return;
    const uint8_t *r = wf.resp + 8;
    uint32_t plen = (*(const uint32_t *)wf.resp & 0x3FFF) - 4;
    if (plen < 20)
        return;
    uint16_t mcc = *(const uint16_t *)(r + 4);
    uint32_t n = *(const uint32_t *)(r + 16);
    if (n > NCHAN)
        n = NCHAN;
    if (20 + 4 * n > plen)
        return;
    for (uint32_t i = 0; i < n; i++)
        wf.chan_flags[i] = *(const uint32_t *)(r + 20 + 4 * i);
    for (uint32_t i = n; i < NCHAN; i++)
        wf.chan_flags[i] = 0;
    kprintf("wifi: regulatory domain %c%c, %u channels listed\n", mcc >> 8 ?: '?', mcc & 0xFF ?: '?', n);
}

/* After the NVM: the runtime configuration (the order the firmware expects: antennas, Bluetooth coexistence, the
 * SoC's latency, the queues, the thermal throttling, power, the regulatory domain, the scan, the beacon filter). */
static bool runtime_init(void)
{
    uint32_t ant = valid_tx();
    if (!init_cmd(GRP_LONG, CMD_TX_ANT_CFG, &ant, 4, "TX_ANT_CONFIGURATION"))
        return false;
    uint32_t bt[2] = { 3, 0 };                   /* Wi-Fi only (no Bluetooth coexistence modules) */
    if (!init_cmd(GRP_LONG, CMD_BT_CONFIG, bt, sizeof(bt), "BT_CONFIG"))
        return false;
    uint32_t soc[2] = { 0, 500 };                /* integrated (no LTR delay: the driver sets no LTR); the crystal's 500 us */
    if (!wf.integrated)
        soc[0] = 1, soc[1] = 0;                  /* a discrete card (the AX210) */
    if (!init_cmd(GRP_SYSTEM, CMD_SOC_CONFIG, soc, sizeof(soc), "SOC_CONFIGURATION"))
        return false;
    if (has_capa(CAPA_DQA)) {
        uint32_t q = 0;                          /* the command queue: 0 */
        if (!init_cmd(0x5, 0x00, &q, 4, "DQA_ENABLE"))
            return false;
    }
    if (has_capa(CAPA_CT_KILL_BY_FW)) {
        uint8_t t[4 + 16] = { 0 };               /* no thresholds: the firmware throttles and stops by itself */
        if (!init_cmd(GRP_PHY_OPS, CMD_TEMP_THRESHOLDS, t, sizeof(t), "TEMP_REPORTING_THRESHOLDS"))
            return false;
    }
    uint16_t pw[2] = { 0, 0 };                   /* the device awake (no power save yet) */
    if (!init_cmd(GRP_LONG, CMD_POWER_TABLE, pw, sizeof(pw), "POWER_TABLE"))
        return false;
    if (wf.lar)
        update_mcc();
    if (has_api(API_REDUCED_SCAN_CFG)) {
        struct __attribute__((packed)) { uint8_t cam, promisc, bcast_sta, reserved; uint32_t tx, rx; } sc = { 0 };
        sc.tx = valid_tx(), sc.rx = valid_rx();
        if (!init_cmd(GRP_LONG, CMD_SCAN_CFG, &sc, sizeof(sc), "SCAN_CFG"))
            return false;
    }
    uint32_t bf[15] = { 0 };                     /* no beacon filtering (this firmware wants 60 bytes: its error said so) */
    if (!init_cmd(GRP_LONG, CMD_BEACON_FILTER, bf, sizeof(bf), "BEACON_FILTER"))
        return false;
    int n2 = 0, n5 = 0;
    for (int i = 0; i < NCHAN; i++)
        if (wf.chan_flags[i] & CH_VALID)
            *(i < NCHAN_2G ? &n2 : &n5) += 1;
    kprintf("wifi: ready: %d channels at 2.4 GHz, %d at 5 GHz; tx antennas %x, rx %x\n", n2, n5, valid_tx(),
            valid_rx());
    return true;
}

/*
 * The AX210's platform NVM: the .pnvm file's section for the SKU the ALIVE named
 * and this MAC and radio, in DMA memory the peripheral scratch points at; then
 * the doorbell, and the firmware's PNVM_INIT_COMPLETE.  Without a section, the
 * doorbell alone (the firmware goes on without it).
 */
#define PNVM_FILE "/lib/firmware/iwlwifi-ty-a0-gf-a0.pnvm"
static void pnvm_section(const uint8_t *d, uint32_t len)
{
    uint32_t mac_type = (wf.hw_rev & 0xFFF0) >> 4, rf_type = (wf.rf_id & 0xFFF000) >> 12;
    bool match = false;
    uint32_t total = 0;
    const uint8_t *parts[MAX_SEC];
    uint32_t sizes[MAX_SEC];
    int n = 0;
    for (uint32_t off = 0; off + 8 <= len;) {
        uint32_t type = *(const uint32_t *)(d + off), tl = *(const uint32_t *)(d + off + 4);
        const uint8_t *v = d + off + 8;
        if (off + 8 + tl > len || type == TLV_PNVM_SKU)
            break;
        if (type == TLV_HW_TYPE && tl >= 4 && !match)
            match = *(const uint16_t *)v == mac_type && *(const uint16_t *)(v + 2) == rf_type;
        if (type == TLV_SEC_RT && tl > 4 && *(const uint32_t *)v != 0xDDDDEEEE && n < MAX_SEC) {
            parts[n] = v + 4, sizes[n] = tl - 4;
            total += sizes[n++];
        }
        off += 8 + ((tl + 3) & ~3U);
    }
    if (!match || !total) {
        kprintf("wifi: PNVM: no section for MAC %03x, radio %03x\n", mac_type, rf_type);
        return;
    }
    uint64_t pa = pmm_alloc_contig((total + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!pa)
        return;
    uint8_t *dst = P2V(pa);
    for (int i = 0; i < n; i++)
        memcpy(dst, parts[i], sizes[i]), dst += sizes[i];
    wf.scratch->pnvm_addr = pa;
    wf.scratch->pnvm_size = total;
    __asm__ volatile("wbinvd" ::: "memory");
    kprintf("wifi: PNVM: %u bytes in %d part%s for MAC %03x, radio %03x\n", total, n, n == 1 ? "" : "s", mac_type,
            rf_type);
}

static void load_pnvm(void)
{
    if (!(wf.sku_id[0] | wf.sku_id[1] | wf.sku_id[2])) {
        kprintf("wifi: PNVM: the ALIVE named no SKU\n");
        return;
    }
    int err;
    struct inode *ip = namei(PNVM_FILE, &err);
    if (!ip) {
        kprintf("wifi: PNVM: %s is missing\n", PNVM_FILE);
    } else {
        uint64_t size = inode_size(ip);
        uint8_t *f = size && size < 4 * 1024 * 1024 ? kmalloc(size) : NULL;
        if (f && readi(ip, f, 0, size) == (long)size) {
            bool found = false;
            for (uint64_t off = 0; off + 8 <= size && !found;) {
                uint32_t type = *(uint32_t *)(f + off), tl = *(uint32_t *)(f + off + 4);
                if (off + 8 + tl > size)
                    break;
                uint64_t next = off + 8 + ((tl + 3) & ~3U);
                if (type == TLV_PNVM_SKU && tl >= 12 && !memcmp(f + off + 8, wf.sku_id, 12)) {
                    pnvm_section(f + next, (uint32_t)(size - next));
                    found = true;
                }
                off = next;
            }
            if (!found)
                kprintf("wifi: PNVM: no section for SKU %08x %08x %08x\n", wf.sku_id[0], wf.sku_id[1], wf.sku_id[2]);
        }
        kfree(f);
        iput(ip);
    }
    if (!grab_nic())
        return;
    write_prph(umac(UREG_DOORBELL_TO_ISR6), ISR6_PNVM);
    release_nic();
    if (wait_for(&wf.pnvm_done, 2000))
        kprintf("wifi: PNVM taken by the firmware\n");
    else
        kprintf("wifi: PNVM: no PNVM_INIT_COMPLETE from the firmware\n");
}

/* Stage 2: the firmware's initialisation (NVM access), the NVM's information, the MAC address. */
static void firmware_init(void)
{
    if (wf.gen3)
        load_pnvm();
    uint32_t v = INIT_NVM;
    if (send_cmd(GRP_SYSTEM, CMD_INIT_EXT_CFG, &v, 4) < 0)
        return (void)(wf.state = "firmware: INIT_EXTENDED_CFG failed");
    v = 0;
    if (send_cmd(GRP_NVM, CMD_NVM_ACCESS_DONE, &v, 4) < 0)
        return (void)(wf.state = "firmware: NVM_ACCESS_COMPLETE failed");
    if (!wait_for(&wf.init_complete, 2000)) {
        kprintf("wifi: no INIT_COMPLETE from the firmware\n");
        return (void)(wf.state = "firmware: no INIT_COMPLETE");
    }
    if (send_cmd(GRP_NVM, CMD_NVM_GET_INFO, &v, 4) < 0)
        return (void)(wf.state = "firmware: NVM_GET_INFO failed");
    const uint8_t *r = wf.resp + 8;              /* (after the packet's header) */
    uint16_t nvm_ver = *(const uint16_t *)(r + 4);
    uint8_t n_addrs = r[7];
    uint32_t sku = *(const uint32_t *)(r + 8), tx = *(const uint32_t *)(r + 12), rxc = *(const uint32_t *)(r + 16);
    uint32_t lar = *(const uint32_t *)(r + 20);
    wf.sku = sku, wf.tx_ant = tx, wf.rx_ant = rxc;
    wf.lar = lar && has_capa(CAPA_LAR);
    /* the channels the NVM allows (LAR replaces them with the country's, below) */
    for (int i = 0; i < NCHAN; i++)
        wf.chan_flags[i] = has_api(API_NVM_INFO_V4) ? *(const uint32_t *)(r + 28 + 4 * i)
                                                    : *(const uint16_t *)(r + 24 + 2 * i);
    /* the MAC address: the OEM's (strap) if valid, else the chip's (OTP) */
    if (grab_nic()) {
        mac_from(rd(CSR_MAC_ADDR_BASE + 8), rd(CSR_MAC_ADDR_BASE + 12), wf.mac);
        if (!mac_valid(wf.mac))
            mac_from(rd(CSR_MAC_ADDR_BASE), rd(CSR_MAC_ADDR_BASE + 4), wf.mac);
        release_nic();
    }
    kprintf("wifi: NVM version %x, %u address%s, bands:%s%s, %s%s%s, antennas tx %x rx %x%s\n", nvm_ver, n_addrs,
            n_addrs == 1 ? "" : "es", sku & 1 ? " 2.4 GHz" : "", sku & 2 ? " 5 GHz" : "", sku & 4 ? "11n" : "",
            sku & 8 ? " 11ac" : "", sku & 16 ? " 11ax" : "", tx & 0xF, rxc & 0xF, lar ? ", LAR" : "");
    if (!mac_valid(wf.mac))
        return (void)(wf.state = "no valid MAC address");
    kprintf("wifi: MAC address %02x:%02x:%02x:%02x:%02x:%02x\n", wf.mac[0], wf.mac[1], wf.mac[2], wf.mac[3],
            wf.mac[4], wf.mac[5]);
    if (runtime_init()) {
        wf.state = "ready";
        wf.ready = true;
    }
}

/* ---------------------------------------------------------------- stage 3: scanning */

/* The UMAC scan request (the firmware's version 14/15 layout). */
struct __attribute__((packed)) scan_req {
    uint32_t uid, ooc_priority;
    struct __attribute__((packed)) {             /* general */
        uint16_t flags;
        uint8_t reserved, start_mac_id, active_dwell[2], adwell_2g, adwell_5g, adwell_social, reserved1;
        uint16_t adwell_max_budget;
        uint32_t max_out_of_time[2], suspend_time[2], priority;
        uint8_t passive_dwell[2], fragments[2];
    } gen;
    struct __attribute__((packed)) {             /* channels */
        uint8_t flags, count, n_aps_override[2];
        struct __attribute__((packed)) { uint32_t flags; uint8_t num, band, iter_count, iter_interval; } ch[67];
    } chan;
    struct __attribute__((packed)) {             /* periodic */
        struct __attribute__((packed)) { uint16_t interval; uint8_t iter_count, reserved; } sched[2];
        uint16_t delay, reserved;
    } per;
    struct __attribute__((packed)) {             /* probe: the probe request's template, the SSIDs asked for */
        struct __attribute__((packed)) { uint16_t off, len; } mac_header, band_data[3], common;
        uint8_t buf[512];
        uint8_t short_ssid_num, bssid_num;
        uint16_t reserved;
        struct __attribute__((packed)) { uint8_t id, len, ssid[32]; } direct[20];
        uint32_t short_ssid[8];
        uint8_t bssid[16][6];
    } probe;
};
_Static_assert(sizeof(struct scan_req) == 1940, "scan request layout");

#define SCAN_FORCE_PASSIVE  (1U << 11)
#define SCAN_PASS_ALL       (1U << 1)             /* every beacon and probe answer up to the driver */
#define SCAN_NTFY_ITER      (1U << 2)
#define SCAN_ADAPTIVE_DWELL (1U << 7)

/* The probe request the firmware sends on the channels where probing is allowed; the SSID goes in by itself. */
static void fill_probe(struct scan_req *c)
{
    uint8_t *f = c->probe.buf, *p = f;
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    *p++ = 0x40, *p++ = 0x00;                    /* management, probe request */
    *p++ = 0, *p++ = 0;                          /* duration (the device's) */
    memcpy(p, bcast, 6), p += 6;
    memcpy(p, wf.mac, 6), p += 6;
    memcpy(p, bcast, 6), p += 6;
    *p++ = 0, *p++ = 0;                          /* sequence (the device's) */
    *p++ = 0, *p++ = 0;                          /* SSID element, empty: the wildcard */
    c->probe.mac_header.off = 0, c->probe.mac_header.len = p - f;
    /* 2.4 GHz: the 11b/g rates (the basic ones marked), the channel */
    static const uint8_t rates_2g[] = { 1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, 50, 4, 0x30, 0x48, 0x60, 0x6C };
    uint8_t *b = p;
    memcpy(p, rates_2g, sizeof(rates_2g)), p += sizeof(rates_2g);
    if (has_capa(CAPA_DS_PARAM_IE))
        *p++ = 3, *p++ = 1, *p++ = 0;            /* (the firmware writes the channel) */
    c->probe.band_data[0].off = b - f, c->probe.band_data[0].len = p - b;
    /* 5 GHz: the 11a rates */
    if (wf.sku & 2) {
        static const uint8_t rates_5g[] = { 1, 8, 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
        b = p;
        memcpy(p, rates_5g, sizeof(rates_5g)), p += sizeof(rates_5g);
        c->probe.band_data[1].off = b - f, c->probe.band_data[1].len = p - b;
    }
    c->probe.common.off = p - f, c->probe.common.len = 0;
}

/* Start a scan of every allowed channel (listening; the firmware probes where it may). */
static int scan_start(void)
{
    if (!wf.ready)
        return -ENXIO;
    if (wf.scanning)
        return -EBUSY;
    struct scan_req *c = kmalloc(sizeof(*c));
    if (!c)
        return -ENOMEM;
    memset(c, 0, sizeof(*c));
    c->uid = 0;
    c->ooc_priority = 6;
    c->gen.flags = SCAN_FORCE_PASSIVE | SCAN_PASS_ALL | SCAN_NTFY_ITER | SCAN_ADAPTIVE_DWELL;
    c->gen.active_dwell[0] = c->gen.active_dwell[1] = 10;        /* ms */
    c->gen.passive_dwell[0] = c->gen.passive_dwell[1] = 110;
    c->gen.adwell_2g = 2, c->gen.adwell_5g = 8, c->gen.adwell_social = 10;
    c->gen.adwell_max_budget = 300;
    c->gen.priority = 6;
    c->chan.flags = 1U << 5;                     /* the channels in our order */
    c->chan.n_aps_override[0] = 10, c->chan.n_aps_override[1] = 2;
    int n = 0;
    for (int i = 0; i < NCHAN && n < 67; i++) {
        if (!(wf.chan_flags[i] & CH_VALID) || (i >= NCHAN_2G && !(wf.sku & 2)) || (i < NCHAN_2G && !(wf.sku & 1)))
            continue;
        c->chan.ch[n].num = nvm_channels[i];
        if (has_api(API_SCAN_EXT_CHAN)) {
            c->chan.ch[n].band = i < NCHAN_2G ? 1 : 0;
            c->chan.ch[n].iter_count = 1;
        } else {
            c->chan.ch[n].band = 1;              /* (the older layout: iter_count, then a 16-bit interval) */
        }
        n++;
    }
    c->chan.count = n;
    c->per.sched[0].interval = 0, c->per.sched[0].iter_count = 1;
    fill_probe(c);
    if (!dev_lock(true)) {
        kfree(c);
        return -EBUSY;
    }
    wf.scanning = true;
    int r = send_cmd(GRP_LONG, CMD_SCAN_REQ_UMAC, c, sizeof(*c));
    if (r < 0)
        wf.scanning = false;
    dev_unlock();
    kfree(c);
    if (r < 0)
        return -EIO;
    kprintf("wifi: scanning %d channels\n", n);
    return 0;
}

/* A network heard (a beacon or a probe answer): the table of networks. */
static void bss_update(const uint8_t *d, const uint8_t *f, uint32_t flen, uint32_t hdr)
{
    const uint8_t *body = f + hdr, *end = f + flen;
    uint16_t capinfo = *(const uint16_t *)(body + 10);
    uint8_t ea = d[32], eb = d[33], channel = d[34];
    int rssi = -(int)(ea && (ea < eb || !eb) ? ea : eb);
    struct sieos_wifi_bss nb = { 0 };
    struct bss_info bx = { 0 };
    memcpy(nb.wb_bssid, f + 16, 6);
    nb.wb_channel = channel;
    nb.wb_rssi = rssi;
    bx.bi = *(const uint16_t *)(body + 8);
    bx.capinfo = capinfo;
    bx.dtim = 1;
    bool rsn = false, wpa = false;
    for (const uint8_t *e = body + 12; e + 2 <= end && e + 2 + e[1] <= end; e += 2 + e[1]) {
        uint8_t id = e[0], el = e[1];
        const uint8_t *v = e + 2;
        if (id == 0 && el <= 32) {
            bool zero = true;
            for (int k = 0; k < el; k++)
                zero &= v[k] == 0;
            if (!zero)
                memcpy(nb.wb_ssid, v, el);
        } else if ((id == 1 || id == 50) && el) {            /* supported, extended supported rates */
            for (int k = 0; k < el && bx.nrates < (int)sizeof(bx.rates); k++)
                bx.rates[bx.nrates++] = v[k];
        } else if (id == 3 && el == 1) {
            nb.wb_channel = v[0];
        } else if (id == 5 && el >= 2) {                     /* TIM: the DTIM period */
            bx.dtim = v[1] ? v[1] : 1;
        } else if (id == 45 && el >= 26) {                   /* HT capabilities */
            bx.ht = true;
            bx.ht_cap = v[0] | v[1] << 8;
            bx.ampdu = v[2];
            bx.ht_mcs[0] = v[3], bx.ht_mcs[1] = v[4];
        } else if (id == 61 && el >= 22) {                   /* HT operation */
            bx.sec_off = v[1] & 3;
            bx.ht_prot = v[2] & 3;
        } else if (id == 191 && el >= 12) {                  /* VHT capabilities */
            bx.vht = true;
            bx.vht_cap = v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24;
            bx.vht_mcs = v[4] | v[5] << 8;
        } else if (id == 192 && el >= 3) {                   /* VHT operation */
            bx.vht_width = v[0];
            bx.vht_center = v[1];
        } else if (id == 221 && el >= 7 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 2) {
            bx.wmm = true;                                   /* (WMM information or parameters) */
        } else if ((id == 48 && el >= 8) ||
                   (id == 221 && el >= 12 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 1)) {
            /* RSN (or WPA): version, group cipher, pairwise ciphers, key managements, capabilities */
            const uint8_t *q = v + (id == 221 ? 4 : 0), *qe = v + el;
            *(id == 48 ? &rsn : &wpa) = true;
            q += 2;
            if (id == 48)
                memcpy(bx.group, q, 4);
            q += 4;
            if (q + 2 > qe)
                continue;
            unsigned np = q[0] | q[1] << 8;
            q += 2;
            for (unsigned k = 0; k < np && q + 4 <= qe; k++, q += 4)
                if (id == 48 && q[0] == 0x00 && q[1] == 0x0F && q[2] == 0xAC && q[3] == 4)
                    bx.ccmp = true;
            if (q + 2 > qe)
                continue;
            unsigned na = q[0] | q[1] << 8;
            q += 2;
            for (unsigned k = 0; k < na && q + 4 <= qe; k++, q += 4) {
                uint8_t t = q[3];
                if (t == 2 || t == 6) {
                    nb.wb_sec |= SIEOS_WIFI_SEC_PSK;
                    if (id == 48 && t == 2)
                        bx.psk = true;
                } else if (t == 1 || t == 5) {
                    nb.wb_sec |= SIEOS_WIFI_SEC_8021X;
                } else if (t == 8) {
                    nb.wb_sec |= SIEOS_WIFI_SEC_SAE;
                }
            }
            if (id == 48 && q + 2 <= qe)
                bx.mfpr = (q[0] & 0x40) != 0;
        }
    }
    if (rsn)
        nb.wb_sec |= SIEOS_WIFI_SEC_RSN;
    if (wpa)
        nb.wb_sec |= SIEOS_WIFI_SEC_WPA;
    if (!rsn && !wpa && (capinfo & 0x10))
        nb.wb_sec |= SIEOS_WIFI_SEC_WEP;
    /* the table: the same BSS updated, else a free (or the oldest) entry */
    int slot = -1;
    for (int k = 0; k < wf.nbss; k++)
        if (!memcmp(wf.bss[k].wb_bssid, nb.wb_bssid, 6))
            slot = k;
    if (slot < 0) {
        if (wf.nbss < NBSS) {
            slot = wf.nbss++;
        } else {
            slot = 0;
            for (int k = 1; k < NBSS; k++)
                if (wf.bss_seen[k] < wf.bss_seen[slot])
                    slot = k;
        }
    } else if (!nb.wb_ssid[0] && wf.bss[slot].wb_ssid[0]) {
        memcpy(nb.wb_ssid, wf.bss[slot].wb_ssid, sizeof(nb.wb_ssid));   /* (a hidden network's probe answer named it) */
    }
    wf.bss[slot] = nb;
    wf.bssx[slot] = bx;
    wf.bss_seen[slot] = ticks;
}

/* ---------------------------------------------------------------- stage 4: joining a network */

enum { LINK_IDLE, LINK_AUTH, LINK_ASSOC, LINK_4WAY, LINK_UP };

#define NRXQ     64                              /* frames kept for the state machine and the network stack */
#define RXQ_LEN  2400
#define TXQ_SIZE 256
#define TX_BODY  512                             /* (in a slot's page: the frame's body from here) */

static struct {
    int link;                                    /* LINK_* */
    bool pending;                                /* a join asked for (wifi(CONNECT)) */
    bool ctx;                                    /* the firmware's PHY, MAC, binding, station, queue set up */
    struct sieos_wifi_bss bss;                   /* the network */
    struct bss_info bx;
    uint8_t pmk[32];
    bool secure;
    uint64_t deadline, retry_at;
    int tries, rejoins;
    uint16_t aid, seq;
    char info[64];
    /* the transmit queue (the station's, TID 15: management and data) */
    uint8_t *tfd;
    uint64_t tfd_pa;
    uint16_t *bc;
    uint64_t bc_pa;
    uint8_t *slot[TXQ_SIZE];
    uint64_t slot_pa[TXQ_SIZE];
    int fwq, cur, tail;
    uint64_t tx_errors;
    /* frames received for the state machine and the stack */
    struct rxf { uint16_t len; uint8_t kind; uint8_t buf[RXQ_LEN]; } *rxq;
    int nrx;
    /* the 4-way handshake (IEEE 802.11-2016 12.7.6) */
    uint8_t anonce[32], snonce[32], ptk[48], replay[8];
    bool have_replay, ptk_set, eapol_v2;
    uint8_t rsnie[24];
    int rsnie_len;
    struct netif *ifp;
    /* what the association uses: QoS data frames, HT (802.11n), VHT (802.11ac), the width (0 20, 1 40, 2 80 MHz) */
    bool qos, ht, vht, mimo;
    int width;
    int powermode;                               /* 0 off, 1 fast, 2 max (dladm's powermode) */
} jn = { .fwq = -1, .powermode = 1 };

enum { RXF_MGMT = 1, RXF_EAPOL, RXF_DATA };

static bool is_bssid(const uint8_t *a) { return !memcmp(a, jn.bss.wb_bssid, 6); }

/* A frame the device received; its descriptor first (48 bytes in the 22000 family). */
static void rx_frame(const uint8_t *d, uint32_t len)
{
    const uint32_t desc = 48;
    if (len < desc + 24)
        return;
    uint16_t flen = *(const uint16_t *)d;
    uint32_t status = *(const uint32_t *)(d + 12);
    if (!(status & 1) || !(status & 2) || flen > len - desc || flen < 24)
        return;                                  /* (CRC, overrun) */
    const uint8_t *f = d + desc;
    uint8_t fc0 = f[0], fc1 = f[1], type = fc0 & 0x0C, sub = fc0 >> 4;
    bool pad = d[3] & 0x20;                      /* the device aligned the body (2 bytes after the header) */
    if (type == 0x00) {                          /* management */
        uint32_t hdr = 24 + (pad ? 2 : 0);
        if ((sub == 8 || sub == 5) && flen >= hdr + 12)
            bss_update(d, f, flen, hdr);
        if (jn.link == LINK_IDLE || !is_bssid(f + 16) ||
            (sub != 11 && sub != 1 && sub != 3 && sub != 10 && sub != 12 && sub != 13))
            return;
        if (jn.nrx == NRXQ || flen - (pad ? 2 : 0) > RXQ_LEN)
            return;
        struct rxf *q = &jn.rxq[jn.nrx++];
        memcpy(q->buf, f, 24);
        memcpy(q->buf + 24, f + hdr, flen - hdr);
        q->len = flen - (hdr - 24);
        q->kind = RXF_MGMT;
        return;
    }
    if (type != 0x08 || jn.link < LINK_4WAY || !(fc1 & 0x02) || !is_bssid(f + 10))
        return;                                  /* data from our access point (from the DS) only */
    bool qos = sub & 0x8;
    if (sub & 0x4)
        return;                                  /* (no data: null frames) */
    uint32_t hdr = 24 + (qos ? 2 : 0), iv = 0;
    if (fc1 & 0x40) {                            /* protected: decrypted by the device? */
        if ((status & 0x700) != 0x200 || (status & ((1U << 11) | (1U << 6))) != ((1U << 11) | (1U << 6)))
            return;
        iv = 8;
    }
    uint32_t body = hdr + iv + (pad ? 2 : 0);
    const uint8_t *da = f + 4, *sa = f + 16;
    if (qos && (f[24] & 0x80)) {                 /* an A-MSDU subframe (the device split them): DA, SA, length, then LLC */
        if (flen < body + 14 + 8)
            return;
        da = f + body, sa = f + body + 6;
        uint32_t sl = f[body + 12] << 8 | f[body + 13];
        body += 14;
        if (sl < flen - body)
            flen = body + sl;                    /* (the padding after it cut) */
    }
    if (flen < body + 8)
        return;
    const uint8_t *llc = f + body;
    uint32_t plen = flen - body;
    if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03 || llc[3] || llc[4] || llc[5])
        return;                                  /* (only LLC/SNAP-encapsulated frames) */
    uint16_t etype = llc[6] << 8 | llc[7];
    if (jn.nrx == NRXQ || plen - 8 + 14 > RXQ_LEN)
        return;
    struct rxf *q = &jn.rxq[jn.nrx++];           /* as an Ethernet frame: destination, source (address 3), type */
    memcpy(q->buf, da, 6);
    memcpy(q->buf + 6, sa, 6);
    memcpy(q->buf + 12, llc + 6, 2);
    memcpy(q->buf + 14, llc + 8, plen - 8);
    q->len = 14 + plen - 8;
    q->kind = etype == 0x888E ? RXF_EAPOL : RXF_DATA;
}

/* The status of a transmitted frame: the ring's slots up to the one after it are free. */
static void tx_done(const uint8_t *pkt, uint32_t len)
{
    if (len < 8 + 48 || pkt[7] != jn.fwq)
        return;
    const uint8_t *r = pkt + 8;
    uint16_t st = *(const uint16_t *)(r + 40) & 0xFF;
    uint32_t ssn = *(const uint32_t *)(r + 44);
    if (st != 1 && st != 2)
        jn.tx_errors++;
    if (ssn < TXQ_SIZE)
        jn.tail = ssn;
}

/* A frame to the transmit queue: its 802.11 header in the command, the body in the slot's page. */
#define TXF_CMD_RATE   (1U << 0)
#define TXF_ENCRYPT_DIS (1U << 1)
#define TXF_HIGH_PRI   (1U << 2)
static int tx_frame(const uint8_t *hdr, uint32_t hlen, const uint8_t *body, uint32_t blen, uint32_t flags,
                    uint32_t rate)
{
    if (jn.fwq < 0 || blen > PAGE_SIZE - TX_BODY || (jn.cur + 1) % TXQ_SIZE == jn.tail)
        return -1;
    int i = jn.cur;
    uint8_t *c = jn.slot[i];
    uint32_t padn = (hlen & 3) ? 4 - (hlen & 3) : 0, total = hlen + blen;
    uint32_t th = wf.gen3 ? 28 : 20;             /* the command before the 802.11 header (AX210: version 8) */
    memset(c, 0, 4 + th + hlen + padn);
    c[0] = CMD_TX, c[1] = 0, c[2] = i, c[3] = jn.fwq;          /* (the narrow header) */
    uint8_t *t = c + 4;
    uint32_t offload = ((hlen / 2) & 0x1F) << 8 | (padn ? 1U << 13 : 0);   /* the header's size, padded */
    *(uint16_t *)t = total;
    if (wf.gen3) {
        *(uint16_t *)(t + 2) = (uint16_t)flags;
        *(uint32_t *)(t + 4) = offload;
    } else {
        *(uint16_t *)(t + 2) = (uint16_t)offload;
        *(uint32_t *)(t + 4) = flags;
    }
    *(uint32_t *)(t + 16) = rate;
    memcpy(t + th, hdr, hlen);
    memcpy(c + TX_BODY, body, blen);
    uint8_t *tfd = jn.tfd + i * TFD_SIZE;
    memset(tfd, 0, 2 + 3 * 10);
    uint16_t ntb = 3, l0 = FIRST_TB, l1 = 4 + th + hlen + padn - FIRST_TB, l2 = blen;
    uint64_t a0 = jn.slot_pa[i], a1 = a0 + FIRST_TB, a2 = a0 + TX_BODY;
    memcpy(tfd, &ntb, 2);
    memcpy(tfd + 2, &l0, 2), memcpy(tfd + 4, &a0, 8);
    memcpy(tfd + 12, &l1, 2), memcpy(tfd + 14, &a1, 8);
    memcpy(tfd + 22, &l2, 2), memcpy(tfd + 24, &a2, 8);
    jn.bc[i] = wf.gen3 ? total : (total + 3) / 4;   /* (AX210: bytes, else dwords; one 64-byte chunk of TFD) */
    __asm__ volatile("sfence" ::: "memory");
    jn.cur = (i + 1) % TXQ_SIZE;
    wr(HBUS_TARG_WRPTR, (uint32_t)jn.fwq << 16 | jn.cur);
    return 0;
}

/* The lowest basic rate, forced for management frames (the firmware's rate format, version 2: antenna A). */
static uint32_t mgmt_rate(void)
{
    return jn.bss.wb_channel <= 14 ? 0x4000 : 0x4000 | 1U << 8;    /* 1 Mb/s CCK, 6 Mb/s OFDM */
}

static void mgmt_header(uint8_t *h, uint8_t subtype)
{
    memset(h, 0, 24);
    h[0] = subtype << 4;
    memcpy(h + 4, jn.bss.wb_bssid, 6);
    memcpy(h + 10, wf.mac, 6);
    memcpy(h + 16, jn.bss.wb_bssid, 6);
    *(uint16_t *)(h + 22) = (uint16_t)(jn.seq++ << 4);
}

static void send_mgmt(uint8_t subtype, const uint8_t *body, uint32_t blen)
{
    uint8_t h[24];
    mgmt_header(h, subtype);
    tx_frame(h, 24, body, blen, TXF_CMD_RATE | TXF_ENCRYPT_DIS, mgmt_rate());
}

/* A data frame (from an Ethernet one): to the access point, LLC/SNAP-encapsulated; protected once keys are set. */
static int send_data(const uint8_t *eth, uint32_t len, bool eapol)
{
    if (len < 14 || len - 14 + 8 > PAGE_SIZE - TX_BODY)
        return -1;
    static uint8_t b[PAGE_SIZE - TX_BODY];      /* (the kernel runs one thing at a time: the big lock) */
    static uint16_t qseq;                        /* (TID 0's sequence numbers) */
    uint8_t h[26];
    memset(h, 0, 26);
    h[0] = jn.qos ? 0x88 : 0x08;                 /* QoS data (TID 0, normal acknowledgement), or data */
    h[1] = 0x01 | (jn.ptk_set ? 0x40 : 0);      /* to the DS, protected */
    memcpy(h + 4, jn.bss.wb_bssid, 6);
    memcpy(h + 10, wf.mac, 6);
    memcpy(h + 16, eth, 6);
    *(uint16_t *)(h + 22) = (uint16_t)((jn.qos ? qseq++ : jn.seq++) << 4);
    static const uint8_t snap[6] = { 0xAA, 0xAA, 0x03, 0, 0, 0 };
    memcpy(b, snap, 6);
    memcpy(b + 6, eth + 12, 2);
    memcpy(b + 8, eth + 14, len - 14);
    uint32_t flags = jn.ptk_set ? 0 : TXF_ENCRYPT_DIS;
    if (eapol && !jn.ptk_set)
        flags |= TXF_HIGH_PRI;
    return tx_frame(h, jn.qos ? 26 : 24, b, len - 14 + 8, flags, 0);
}

/* The network stack's interface. */
static int nic_send(struct netif *ifp, const void *frame, size_t len)
{
    (void)ifp;
    if (jn.link != LINK_UP)
        return -1;
    return send_data(frame, len, false);
}

static void nic_poll(struct netif *ifp) { (void)ifp; }   /* (frames come from wifi_poll) */
static bool nic_link(struct netif *ifp) { (void)ifp; return jn.link == LINK_UP; }
static const struct nic_ops wifi_nic_ops = { "iwlwifi", nic_send, nic_poll, nic_link };

/* The transmit queue: its TFDs, byte counts and a page per slot; given to the firmware for the station. */
static bool txq_alloc(void)
{
    if (jn.tfd)
        return true;
    jn.tfd_pa = pmm_alloc_contig(TXQ_SIZE * TFD_SIZE / PAGE_SIZE);
    jn.bc_pa = pmm_alloc_contig(1);
    if (!jn.tfd_pa || !jn.bc_pa)
        return false;
    for (int i = 0; i < TXQ_SIZE; i++) {
        if (!(jn.slot_pa[i] = pmm_alloc_contig(1)))
            return false;
        jn.slot[i] = P2V(jn.slot_pa[i]);
    }
    jn.tfd = P2V(jn.tfd_pa);
    jn.bc = P2V(jn.bc_pa);
    return true;
}

static uint32_t cmd_status(void) { return *(const uint32_t *)(wf.resp + 8); }

/* The firmware's contexts for the network: the PHY (the channel), the MAC (us, the BSSID), their binding, the
 * access point as station 0, its transmit queue, the session protection (stay on the channel while joining). */
static const char *ctx_setup(void)
{
    const struct sieos_wifi_bss *b = &jn.bss;
    bool g24 = b->wb_channel <= 14;
    /* PHY context (the 8-byte channel information: the firmware has the ultra-high-band channels) */
    struct __attribute__((packed)) {
        uint32_t id_color, action, channel;
        uint8_t band, width, ctrl_pos, reserved;
        uint32_t lmac, rxchain, dsp, reserved2;
    } phy = { 0, 1, b->wb_channel, g24 ? 1 : 0, 0, 0, 0, 0, 0, 0, 0 };
    if (send_cmd(GRP_LONG, CMD_PHY_CONTEXT, &phy, sizeof(phy)) < 0)
        return "PHY context";
    struct __attribute__((packed)) { uint32_t phy_id, rx_chain, reserved, sad[4]; uint8_t flags, r[3]; } rlc = { 0 };
    rlc.rx_chain = valid_rx() << 1 | 1U << 10 | 1U << 12;          /* valid chains; one idle, one active */
    if (send_cmd_v(GRP_DATA_PATH, CMD_RLC_CONFIG, 2, &rlc, sizeof(rlc)) < 0)
        return "RLC configuration";
    /* MAC context: a station of the BSS */
    struct __attribute__((packed)) {
        uint32_t id_color, action, mac_type, tsf_id;
        uint8_t node[6]; uint16_t r0;
        uint8_t bssid[6]; uint16_t r1;
        uint32_t cck, ofdm, protection, short_preamble, short_slot, filter, qos;
        struct __attribute__((packed)) { uint16_t cw_min, cw_max; uint8_t aifsn, fifos; uint16_t txop; } ac[5];
        uint32_t is_assoc, dtim_time; uint64_t dtim_tsf;
        uint32_t bi, r2, dtim_interval, data_policy, listen_interval, assoc_id, beacon_arrive, pad[1];
    } mac;
    _Static_assert(sizeof(mac) == 148, "MAC context layout");
    memset(&mac, 0, sizeof(mac));
    mac.action = 1;
    mac.mac_type = 5;                            /* BSS station */
    memcpy(mac.node, wf.mac, 6);
    memcpy(mac.bssid, b->wb_bssid, 6);
    uint32_t basic_ofdm = 0;
    static const uint8_t ofdm_rates[8] = { 12, 18, 24, 36, 48, 72, 96, 108 };
    for (int i = 0; i < jn.bx.nrates; i++)
        for (int k = 0; k < 8; k++)
            if ((jn.bx.rates[i] & 0x80) && (jn.bx.rates[i] & 0x7F) == ofdm_rates[k])
                basic_ofdm |= 1U << k;
    mac.cck = g24 ? 0xF : 0;                     /* the rates answers (ACKs) may use */
    mac.ofdm = basic_ofdm | 0x15;                /* (6, 12, 24 Mb/s are mandatory) */
    mac.short_preamble = g24 && (jn.bx.capinfo & 0x20) ? 1U << 5 : 0;
    mac.short_slot = (jn.bx.capinfo & 0x400) || !g24 ? 1U << 4 : 0;
    mac.filter = 1U << 2 | 1U << 6;              /* group frames; beacons */
    /* the default EDCA parameters (802.11-2016 table 9-137), by FIFO: BK 1, BE 2, VI 3, VO 4 */
    static const struct { uint8_t fifo, ecwmin, ecwmax, aifsn; uint16_t txop; } edca[4] = {
        { 2, 4, 10, 3, 0 }, { 1, 4, 10, 7, 0 }, { 3, 3, 4, 2, 94 }, { 4, 2, 3, 2, 47 } };
    for (int i = 0; i < 4; i++) {
        int f = edca[i].fifo;
        mac.ac[f].cw_min = (1U << edca[i].ecwmin) - 1;
        mac.ac[f].cw_max = (1U << edca[i].ecwmax) - 1;
        mac.ac[f].aifsn = edca[i].aifsn;
        mac.ac[f].fifos = 1U << f;
        mac.ac[f].txop = edca[i].txop * 32;
    }
    mac.bi = jn.bx.bi ? jn.bx.bi : 100;
    mac.dtim_interval = mac.bi * jn.bx.dtim;
    mac.listen_interval = 10;
    if (send_cmd(GRP_LONG, CMD_MAC_CONTEXT, &mac, sizeof(mac)) < 0)
        return "MAC context";
    struct __attribute__((packed)) { uint32_t id_color, action, macs[3], phy, lmac; } bind = {
        0, 1, { 0, 0xFFFFFFFF, 0xFFFFFFFF }, 0, 0 };
    if (send_cmd(GRP_LONG, CMD_BINDING, &bind, sizeof(bind)) < 0 || cmd_status())
        return "binding";
    /* the access point: station 0 */
    uint8_t sta[48] = { 0 };
    memcpy(sta + 8, b->wb_bssid, 6);             /* addr; sta_id 0, a link station, MAC context 0 */
    *(uint32_t *)(sta + 24) = 3U << 26 | 3U << 28;              /* station_flags_msk: width, MIMO */
    if (send_cmd(GRP_LONG, CMD_ADD_STA, sta, sizeof(sta)) < 0 || (cmd_status() & 0xFF) != 1)
        return "ADD_STA";
    /* its transmit queue (TID 15: management and non-QoS data) */
    if (!txq_alloc())
        return "memory for the transmit queue";
    memset(jn.tfd, 0, TXQ_SIZE * TFD_SIZE);
    memset(jn.bc, 0, PAGE_SIZE);
    jn.cur = jn.tail = 0;
    struct __attribute__((packed)) {
        uint32_t op, sta_mask; uint8_t tid, r[3]; uint32_t flags, cb_size; uint64_t bc, tfdq;
    } q = { 0, 1, 15, { 0 }, 0, CMDQ_CB, jn.bc_pa, jn.tfd_pa };
    if (send_cmd(GRP_DATA_PATH, CMD_SCD_QUEUE_CFG, &q, sizeof(q)) < 0)
        return "transmit queue";
    jn.fwq = *(const uint16_t *)(wf.resp + 8);
    uint16_t wp = *(const uint16_t *)(wf.resp + 12);
    jn.cur = jn.tail = wp % TXQ_SIZE;
    jn.ctx = true;
    struct __attribute__((packed)) { uint32_t id_color, action, conf, duration, rep, interval; } sp = {
        0, 1, 0, (uint32_t)mac.bi * 9, 0, 0 };
    if (send_cmd(GRP_MAC_CONF, CMD_SESSION_PROT, &sp, sizeof(sp)) < 0)
        return "session protection";
    kprintf("wifi: joining \"%s\" (%02x:%02x:%02x:%02x:%02x:%02x, channel %d): transmit queue %d\n", b->wb_ssid,
            b->wb_bssid[0], b->wb_bssid[1], b->wb_bssid[2], b->wb_bssid[3], b->wb_bssid[4], b->wb_bssid[5],
            b->wb_channel, jn.fwq);
    return NULL;
}

/* Undo ctx_setup (the firmware keeps nothing of the network). */
static void ctx_teardown(void)
{
    if (!jn.ctx)
        return;
    struct __attribute__((packed)) { uint32_t id_color, action, conf, duration, rep, interval; } sp = { 0, 3, 0, 0, 0, 0 };
    send_cmd(GRP_MAC_CONF, CMD_SESSION_PROT, &sp, sizeof(sp));
    struct __attribute__((packed)) { uint32_t op, sta_mask, tid, pad[6]; } q = { 1, 1, 15, { 0 } };
    send_cmd(GRP_DATA_PATH, CMD_SCD_QUEUE_CFG, &q, 36);
    uint32_t rm = 0;                             /* station 0 */
    send_cmd(GRP_LONG, CMD_REMOVE_STA, &rm, 4);
    struct __attribute__((packed)) { uint32_t id_color, action, macs[3], phy, lmac; } bind = {
        0, 3, { 0, 0xFFFFFFFF, 0xFFFFFFFF }, 0, 0 };
    send_cmd(GRP_LONG, CMD_BINDING, &bind, sizeof(bind));
    uint8_t mac[148] = { 0 };
    *(uint32_t *)(mac + 4) = 3;
    send_cmd(GRP_LONG, CMD_MAC_CONTEXT, mac, sizeof(mac));
    uint8_t phy[32] = { 0 };
    *(uint32_t *)(phy + 4) = 3;
    send_cmd(GRP_LONG, CMD_PHY_CONTEXT, phy, sizeof(phy));
    jn.ctx = false;
    jn.fwq = -1;
    jn.ptk_set = false;
}

/* After the association: the station and the MAC again (associated), the smart FIFO, multicast, the rates. */
static const char *ctx_assoc(void)
{
    /* the PHY: the width (and where the control channel is in it), both receive chains for MIMO */
    if (jn.width || jn.mimo) {
        uint8_t phy[32] = { 0 };
        *(uint32_t *)(phy + 4) = 2;              /* modify */
        *(uint32_t *)(phy + 8) = jn.bss.wb_channel;
        phy[12] = jn.bss.wb_channel <= 14 ? 1 : 0;
        if (jn.width == 2) {
            phy[13] = 2;                         /* 80 MHz */
            int d = jn.bss.wb_channel - jn.bx.vht_center;
            phy[14] = d == -6 ? 1 : d == -2 ? 0 : d == 2 ? 4 : d == 6 ? 5 : 0;
        } else if (jn.width == 1) {
            phy[13] = 1;                         /* 40 MHz: the secondary above, the control below (or the reverse) */
            phy[14] = jn.bx.sec_off == 1 ? 0 : 4;
        }
        if (send_cmd(GRP_LONG, CMD_PHY_CONTEXT, phy, sizeof(phy)) < 0)
            return "PHY context (width)";
        uint32_t chains = jn.mimo ? 2 : 1;
        struct __attribute__((packed)) { uint32_t phy_id, rx_chain, reserved, sad[4]; uint8_t flags, r[3]; } rlc = { 0 };
        rlc.rx_chain = valid_rx() << 1 | chains << 10 | chains << 12;
        if (send_cmd_v(GRP_DATA_PATH, CMD_RLC_CONFIG, 2, &rlc, sizeof(rlc)) < 0)
            return "RLC configuration (chains)";
    }
    uint8_t sta[48] = { 0 };
    sta[0] = 1;                                  /* modify */
    memcpy(sta + 8, jn.bss.wb_bssid, 6);
    uint32_t flags = 0;
    if (jn.width == 1)
        flags |= 1U << 26;                       /* 40 MHz */
    else if (jn.width == 2)
        flags |= 2U << 26;                       /* 80 MHz */
    if (jn.mimo)
        flags |= 1U << 28;                       /* MIMO2 */
    if (jn.ht) {
        uint32_t agg = jn.bx.ampdu & 3, dens = (jn.bx.ampdu >> 2) & 7;
        flags |= agg << 19 | (dens >= 4 ? dens << 23 : 0);   /* (spacing codes 4-7: 2 to 16 us) */
    }
    *(uint32_t *)(sta + 20) = flags;
    *(uint32_t *)(sta + 24) = 3U << 26 | 3U << 28 | (jn.ht ? 0xFU << 19 | 7U << 23 : 0);
    if (send_cmd(GRP_LONG, CMD_ADD_STA, sta, sizeof(sta)) < 0 || (cmd_status() & 0xFF) != 1)
        return "ADD_STA (update)";
    /* (the MAC context, modified: associated, with the AID) */
    uint8_t mac[148];
    memset(mac, 0, sizeof(mac));
    *(uint32_t *)(mac + 4) = 2;
    *(uint32_t *)(mac + 8) = 5;
    memcpy(mac + 16, wf.mac, 6);
    memcpy(mac + 24, jn.bss.wb_bssid, 6);
    bool g24 = jn.bss.wb_channel <= 14;
    *(uint32_t *)(mac + 32) = g24 ? 0xF : 0;
    *(uint32_t *)(mac + 36) = 0x15;
    *(uint32_t *)(mac + 44) = g24 && (jn.bx.capinfo & 0x20) ? 1U << 5 : 0;
    *(uint32_t *)(mac + 48) = (jn.bx.capinfo & 0x400) || !g24 ? 1U << 4 : 0;
    *(uint32_t *)(mac + 52) = 1U << 2 | 1U << 6;
    *(uint32_t *)(mac + 56) = (jn.qos ? 1U << 0 : 0) | (jn.ht ? 1U << 1 : 0);   /* QoS: EDCA, 802.11n */
    if (jn.ht && (jn.bx.ht_prot == 1 || jn.bx.ht_prot == 3 || (jn.bx.ht_prot == 2 && jn.width)))
        *(uint32_t *)(mac + 40) = 1U << 23 | 1U << 24;                          /* HT protection */
    static const uint8_t edca[4][5] = { { 2, 4, 10, 3, 0 }, { 1, 4, 10, 7, 0 }, { 3, 3, 4, 2, 94 }, { 4, 2, 3, 2, 47 } };
    for (int i = 0; i < 4; i++) {
        uint8_t *a = mac + 60 + 8 * edca[i][0];
        *(uint16_t *)a = (1U << edca[i][1]) - 1;
        *(uint16_t *)(a + 2) = (1U << edca[i][2]) - 1;
        a[4] = edca[i][3];
        a[5] = 1U << edca[i][0];
        *(uint16_t *)(a + 6) = edca[i][4] * 32;
    }
    uint32_t bi = jn.bx.bi ? jn.bx.bi : 100;
    uint8_t *st = mac + 100;
    *(uint32_t *)st = 1;                         /* is_assoc */
    *(uint32_t *)(st + 16) = bi;
    *(uint32_t *)(st + 24) = bi * jn.bx.dtim;
    *(uint32_t *)(st + 32) = 10;
    *(uint32_t *)(st + 36) = jn.aid;
    if (send_cmd(GRP_LONG, CMD_MAC_CONTEXT, mac, sizeof(mac)) < 0)
        return "MAC context (associated)";
    if (!has_api(68)) {                          /* the smart FIFO: full on (unless the firmware does it) */
        uint32_t sf[1 + 2 + 10 + 10] = { 1, 4096, 4096 };
        for (int i = 0; i < 10; i++)
            sf[3 + i] = 1000000;
        static const uint32_t full[10] = { 2016, 320, 2016, 320, 10016, 2016, 2016, 320, 2016, 320 };
        memcpy(sf + 13, full, sizeof(full));
        if (send_cmd(GRP_LONG, CMD_SF_CONFIG, sf, sizeof(sf)) < 0)
            return "smart FIFO";
    }
    uint8_t mc[12] = { 1, 0, 0, 1 };             /* our own filtered, every multicast passed */
    memcpy(mc + 4, jn.bss.wb_bssid, 6);
    if (send_cmd(GRP_LONG, CMD_MCAST_FILTER, mc, sizeof(mc)) < 0)
        return "multicast filter";
    /* the rates: legacy (no HT yet), the access point's; the firmware picks among them */
    static const uint8_t rv[12] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
    uint16_t non_ht = 0;
    for (int i = 0; i < jn.bx.nrates; i++)
        for (int k = 0; k < 12; k++)
            if ((jn.bx.rates[i] & 0x7F) == rv[k] && (g24 || k >= 4))
                non_ht |= 1U << k;
    if (!non_ht)
        non_ht = g24 ? 0xFFF : 0xFF0;
    uint8_t tlc[28] = { 0 };                     /* station 0 */
    tlc[4] = jn.width;                           /* 20, 40, 80 MHz */
    tlc[5] = jn.vht ? 2 : jn.ht ? 1 : 0;         /* the mode: VHT, HT, legacy */
    tlc[6] = jn.mimo ? 3 : 1;                    /* chains A (and B) */
    *(uint16_t *)(tlc + 10) = non_ht;
    if (jn.vht) {                                /* per stream: the MCS it receives (0-7, 0-8, 0-9) */
        for (int ss = 0; ss < (jn.mimo ? 2 : 1); ss++) {
            int m = (jn.bx.vht_mcs >> (2 * ss)) & 3;
            uint16_t mask = m == 0 ? 0xFF : m == 1 ? 0x1FF : m == 2 ? (jn.width ? 0x3FF : 0x1FF) : 0;
            *(uint16_t *)(tlc + 12 + 6 * ss) = mask;
        }
        tlc[7] = (jn.bx.ht_cap & 0x20 ? 1 : 0) | (jn.width >= 1 && (jn.bx.ht_cap & 0x40) ? 2 : 0) |
                 (jn.width == 2 && (jn.bx.vht_cap & 0x20) ? 4 : 0);   /* short guard interval, by width */
        *(uint16_t *)(tlc + 24) = 3895;
    } else if (jn.ht) {
        *(uint16_t *)(tlc + 12) = jn.bx.ht_mcs[0];
        if (jn.mimo)
            *(uint16_t *)(tlc + 18) = jn.bx.ht_mcs[1];
        tlc[7] = (jn.bx.ht_cap & 0x20 ? 1 : 0) | (jn.width == 1 && (jn.bx.ht_cap & 0x40) ? 2 : 0);
        *(uint16_t *)(tlc + 24) = 3839;
    } else {
        *(uint16_t *)(tlc + 24) = 2312;          /* max MPDU */
    }
    if (send_cmd(GRP_DATA_PATH, CMD_TLC_CONFIG, tlc, sizeof(tlc)) < 0)
        return "rate selection";
    return NULL;
}

static void set_info(const char *fmt, const char *a, unsigned n)
{
    snprintf(jn.info, sizeof(jn.info), fmt, a, n);
    wf.state = jn.info;
}

/* Joining failed or the link went down: everything undone; logged and in the state. */
static void join_fail(const char *why, unsigned n)
{
    char msg[64];
    snprintf(msg, sizeof(msg), why, n);
    kprintf("wifi: \"%s\": %s\n", jn.bss.wb_ssid, msg);
    ctx_teardown();
    bool was_up = jn.link == LINK_UP;
    jn.link = LINK_IDLE;
    if (jn.ifp)
        jn.ifp->up = false;
    set_info("%s", msg, 0);
    if (was_up && jn.rejoins < 5) {              /* (a link lost: joined again in 2 s) */
        jn.rejoins++;
        jn.retry_at = ticks + 2 * TIMER_HZ;
    }
}

static void send_auth(void)
{
    uint8_t b[6] = { 0, 0, 1, 0, 0, 0 };         /* open system, sequence 1 */
    send_mgmt(11, b, sizeof(b));
    jn.deadline = ticks + TIMER_HZ / 2;
}

/* Our RSN element: CCMP pairwise, the access point's group cipher, PSK. */
static void build_rsnie(void)
{
    static const uint8_t ccmp[4] = { 0x00, 0x0F, 0xAC, 4 }, psk[4] = { 0x00, 0x0F, 0xAC, 2 };
    uint8_t *p = jn.rsnie;
    *p++ = 48, *p++ = 20;
    *p++ = 1, *p++ = 0;                          /* version 1 */
    memcpy(p, jn.bx.group[0] || jn.bx.group[1] || jn.bx.group[2] ? jn.bx.group : ccmp, 4), p += 4;
    *p++ = 1, *p++ = 0, memcpy(p, ccmp, 4), p += 4;
    *p++ = 1, *p++ = 0, memcpy(p, psk, 4), p += 4;
    *p++ = 0, *p++ = 0;                          /* capabilities */
    jn.rsnie_len = p - jn.rsnie;
}

/* What the association will use: the access point's HT/VHT/WMM and ours (the NVM: 11n, 11ac, two antennas). */
static void choose_mode(void)
{
    bool g24 = jn.bss.wb_channel <= 14;
    jn.qos = jn.bx.wmm && !boot_option(ddi_cmdline(), "wifilegacy");   /* ("wifilegacy": 802.11a/g only) */
    jn.ht = jn.bx.ht && jn.qos && (wf.sku & 4);
    jn.vht = jn.ht && !g24 && jn.bx.vht && (wf.sku & 8);
    jn.mimo = jn.ht && (valid_tx() & 3) == 3 && (valid_rx() & 3) == 3 &&
              (jn.vht ? ((jn.bx.vht_mcs >> 2) & 3) != 3 : jn.bx.ht_mcs[1] != 0);
    jn.width = 0;
    if (jn.vht && jn.bx.vht_width >= 1 && jn.bx.vht_center)
        jn.width = 2;
    else if (jn.ht && !g24 && (jn.bx.sec_off == 1 || jn.bx.sec_off == 3) && (jn.bx.ht_cap & 2))
        jn.width = 1;
}

/* Our HT capabilities: 2 streams, SM power save off, SGI 20 (and 40 MHz, SGI 40 at 5 GHz), 1 RX STBC stream. */
static uint8_t *add_ht(uint8_t *p)
{
    bool w40 = jn.bss.wb_channel > 14;
    uint16_t cap = 0x000C | 1U << 5 | 1U << 8 | (w40 ? 1U << 1 | 1U << 6 : 0);
    *p++ = 45, *p++ = 26;
    *p++ = cap, *p++ = cap >> 8;
    *p++ = 0x17;                                 /* A-MPDU: 64 KiB, 4 us spacing */
    memset(p, 0, 16);
    p[0] = 0xFF;                                 /* MCS 0-7 */
    p[1] = (valid_rx() & 3) == 3 ? 0xFF : 0;     /* MCS 8-15: the second stream */
    p += 16;
    memset(p, 0, 7), p += 7;                     /* extended capabilities, beamforming, antenna selection */
    return p;
}

/* Our VHT capabilities: 3895-byte MPDUs, 80 MHz, SGI 80, 1 RX STBC stream, MCS 0-9 on 2 streams. */
static uint8_t *add_vht(uint8_t *p)
{
    uint32_t cap = 1U << 5 | 1U << 8;
    uint16_t map = (valid_rx() & 3) == 3 ? 0xFFFA : 0xFFFE;
    *p++ = 191, *p++ = 12;
    for (int i = 0; i < 4; i++)
        *p++ = cap >> (8 * i);
    *p++ = map, *p++ = map >> 8, *p++ = 0, *p++ = 0;     /* RX: the map, no highest rate */
    *p++ = map, *p++ = map >> 8, *p++ = 0, *p++ = 0;     /* TX */
    return p;
}

static void send_assoc(void)
{
    choose_mode();
    uint8_t b[256], *p = b;
    uint16_t cap = 0x0001 | (jn.bx.capinfo & (0x0020 | 0x0400)) | (jn.secure ? 0x0010 : 0);
    *p++ = cap, *p++ = cap >> 8;
    *p++ = 10, *p++ = 0;                         /* listen interval */
    size_t sl = strlen(jn.bss.wb_ssid);
    *p++ = 0, *p++ = sl;
    memcpy(p, jn.bss.wb_ssid, sl), p += sl;
    int n = jn.bx.nrates, n1 = n > 8 ? 8 : n;    /* the access point's rates: supported, then extended */
    if (!n) {
        static const uint8_t def24[] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6C };
        static const uint8_t def5[] = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
        const uint8_t *d = jn.bss.wb_channel <= 14 ? def24 : def5;
        n = jn.bss.wb_channel <= 14 ? 12 : 8;
        memcpy(jn.bx.rates, d, n);
        jn.bx.nrates = n;
        n1 = n > 8 ? 8 : n;
    }
    *p++ = 1, *p++ = n1;
    memcpy(p, jn.bx.rates, n1), p += n1;
    if (n > 8) {
        *p++ = 50, *p++ = n - 8;
        memcpy(p, jn.bx.rates + 8, n - 8), p += n - 8;
    }
    if (jn.secure) {
        build_rsnie();
        memcpy(p, jn.rsnie, jn.rsnie_len), p += jn.rsnie_len;
    }
    if (jn.ht)
        p = add_ht(p);
    if (jn.vht)
        p = add_vht(p);
    if (jn.qos) {                                /* WMM information: QoS, no U-APSD */
        static const uint8_t wmm[] = { 221, 7, 0x00, 0x50, 0xF2, 2, 0, 1, 0 };
        memcpy(p, wmm, sizeof(wmm)), p += sizeof(wmm);
    }
    send_mgmt(0, b, p - b);
    jn.deadline = ticks + TIMER_HZ / 2;
}

/* Power save (dladm's powermode): the device may sleep (the power table), and the connection's power management
 * tells the access point we doze between beacons, awake again for traffic (timeouts: 50/100 ms "fast", 25/25 ms
 * and every third DTIM "max"). */
static void power_apply(void)
{
    int m = jn.powermode;
    uint16_t dev[2] = { m ? 1 : 0, 0 };
    if (send_cmd(GRP_LONG, CMD_POWER_TABLE, dev, sizeof(dev)) < 0)
        return;
    if (jn.link != LINK_UP)
        return;
    uint8_t c[40] = { 0 };                       /* MAC context 0 */
    uint16_t flags = m ? 1U << 0 | 1U << 1 : 0;  /* power save, power management */
    uint32_t bi = jn.bx.bi ? jn.bx.bi : 100, dtim_ms = bi * jn.bx.dtim * 1024 / 1000;
    uint32_t ka = 3 * dtim_ms > 25000 ? 3 * dtim_ms : 25000;
    *(uint16_t *)(c + 6) = (ka + 999) / 1000;    /* keep-alive, seconds */
    if (m) {
        *(uint32_t *)(c + 8) = (m == 2 ? 25 : 50) * 1024;    /* RX data timeout (us) */
        *(uint32_t *)(c + 12) = (m == 2 ? 25 : 100) * 1024;  /* TX */
        if (m == 2 && jn.bx.dtim <= 2) {
            flags |= 1U << 2;                    /* skip over DTIMs */
            c[25] = 3;
        }
    }
    *(uint16_t *)(c + 4) = flags;
    if (send_cmd(GRP_LONG, CMD_MAC_PM_POWER, c, sizeof(c)) == 0)
        kprintf("wifi: power save %s\n", m == 2 ? "max" : m ? "fast" : "off");
}

/* Joined (the keys set, or an open network): the interface up; DHCP and IPv6 start on it. */
static void link_up(void)
{
    jn.link = LINK_UP;
    jn.rejoins = 0;
    set_info("connected to %s", jn.bss.wb_ssid, 0);
    kprintf("wifi: connected to \"%s\"%s, %s%s, %d MHz\n", jn.bss.wb_ssid, jn.secure ? " (WPA2)" : "",
            jn.vht ? "802.11ac" : jn.ht ? "802.11n" : "802.11a/g", jn.mimo ? " 2x2" : "", 20 << jn.width);
    power_apply();
    if (!jn.ifp) {
        jn.ifp = netif_register(&wifi_nic_ops, NULL, wf.mac);
        if (jn.ifp)
            strlcpy(jn.ifp->name, "iwx0", sizeof(jn.ifp->name));
    }
    if (jn.ifp)
        net_attach(jn.ifp);
}

/* ---- the 4-way and group key handshakes (EAPOL-Key frames, descriptor version 2: HMAC-SHA1, AES key wrap) */

#define KI_PAIRWISE (1U << 3)
#define KI_INSTALL  (1U << 6)
#define KI_ACK      (1U << 7)
#define KI_MIC      (1U << 8)
#define KI_SECURE   (1U << 9)
#define KI_ENCDATA  (1U << 12)
#define EK_HDR      99                           /* EAPOL header (4), the key descriptor up to its data (95) */

static uint16_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

static void send_eapol_key(uint16_t info, const uint8_t *replay, const uint8_t *nonce, const uint8_t *data,
                           uint16_t dlen)
{
    uint8_t f[14 + EK_HDR + 64];
    memcpy(f, jn.bss.wb_bssid, 6);
    memcpy(f + 6, wf.mac, 6);
    f[12] = 0x88, f[13] = 0x8E;
    uint8_t *e = f + 14, *k = e + 4;
    memset(e, 0, EK_HDR + dlen);
    uint16_t blen = 95 + dlen;
    e[0] = jn.eapol_v2 ? 2 : 1, e[1] = 3, e[2] = blen >> 8, e[3] = blen;
    k[0] = 2;                                    /* the RSN key descriptor */
    k[1] = info >> 8, k[2] = info;
    memcpy(k + 5, replay, 8);
    if (nonce)
        memcpy(k + 13, nonce, 32);
    k[93] = dlen >> 8, k[94] = dlen;
    if (dlen)
        memcpy(k + 95, data, dlen);
    uint8_t mic[20];
    hmac_sha1(jn.ptk, 16, e, 4 + blen, mic);     /* with the KCK, the MIC field zero */
    memcpy(k + 77, mic, 16);
    send_data(f, 14 + 4 + blen, true);
}

static bool eapol_mic_ok(const uint8_t *e, uint32_t len)
{
    static uint8_t copy[1600];
    uint8_t mic[20];
    if (len > sizeof(copy))
        return false;
    memcpy(copy, e, len);
    memset(copy + 4 + 77, 0, 16);
    hmac_sha1(jn.ptk, 16, copy, len, mic);
    return !memcmp(mic, e + 4 + 77, 16);
}

/* A key (the PTK's temporal key, or a GTK) into the device: CCMP, for station 0. */
static bool install_key(const uint8_t *key, int id, bool group, const uint8_t *rsc)
{
    uint8_t c[80] = { 0 };
    *(uint32_t *)c = 1;                          /* add */
    *(uint32_t *)(c + 4) = 1;                    /* station 0 */
    *(uint32_t *)(c + 8) = id;
    *(uint32_t *)(c + 12) = 0x02 | (group ? 0x40 : 0);
    memcpy(c + 16, key, 16);
    if (rsc)
        memcpy(c + 64, rsc, 6);                  /* rx_seq: the packet number */
    return send_cmd(GRP_DATA_PATH, CMD_SEC_KEY, c, sizeof(c)) == 0;
}

/* The GTK from the key data's KDEs (00-0F-AC:1): its index and key. */
static bool find_gtk(const uint8_t *kd, uint32_t len, int *id, const uint8_t **gtk, int *glen)
{
    for (const uint8_t *p = kd; p + 2 <= kd + len && p + 2 + p[1] <= kd + len; p += 2 + p[1]) {
        if (p[0] == 0xDD && p[1] >= 6 + 16 && p[2] == 0x00 && p[3] == 0x0F && p[4] == 0xAC && p[5] == 1) {
            *id = p[6] & 3;
            *gtk = p + 8;
            *glen = p[1] - 6;
            return true;
        }
        if (p[0] == 0xDD && p[1] == 0)
            break;                               /* (padding) */
    }
    return false;
}

static void eapol_input(const uint8_t *e, uint32_t len)
{
    if (len < EK_HDR || e[1] != 3)
        return;
    uint32_t blen = be16(e + 2);
    if (4 + blen > len || blen < 95)
        return;
    len = 4 + blen;
    const uint8_t *k = e + 4;
    if (k[0] != 2)
        return;                                  /* (not the RSN descriptor) */
    uint16_t info = be16(k + 1), dlen = be16(k + 93);
    if ((info & 7) != 2) {
        join_fail("key descriptor version %u is not supported", info & 7);
        return;
    }
    if (95U + dlen > blen)
        return;
    if (jn.have_replay && (info & KI_MIC) && memcmp(k + 5, jn.replay, 8) < 0)
        return;                                  /* (an old frame: its replay counter below the last) */
    jn.eapol_v2 = e[0] >= 2;
    if ((info & KI_PAIRWISE) && (info & KI_ACK) && !(info & KI_MIC)) {
        /* message 1: the ANonce; our SNonce; the PTK = PRF-384(PMK, "Pairwise key expansion", addresses, nonces) */
        memcpy(jn.anonce, k + 13, 32);
        memcpy(jn.replay, k + 5, 8);
        jn.have_replay = true;
        uint8_t data[76];
        bool apfirst = memcmp(jn.bss.wb_bssid, wf.mac, 6) < 0;
        memcpy(data, apfirst ? jn.bss.wb_bssid : wf.mac, 6);
        memcpy(data + 6, apfirst ? wf.mac : jn.bss.wb_bssid, 6);
        bool afirst = memcmp(jn.anonce, jn.snonce, 32) < 0;
        memcpy(data + 12, afirst ? jn.anonce : jn.snonce, 32);
        memcpy(data + 44, afirst ? jn.snonce : jn.anonce, 32);
        wpa_prf(jn.pmk, 32, "Pairwise key expansion", data, sizeof(data), jn.ptk, 48);
        send_eapol_key(2 | KI_PAIRWISE | KI_MIC, jn.replay, jn.snonce, jn.rsnie, jn.rsnie_len);
        kprintf("wifi: key handshake: message 1, answered\n");
        jn.deadline = ticks + 3 * TIMER_HZ;
        return;
    }
    if (!(info & KI_MIC) || !eapol_mic_ok(e, len)) {
        kprintf("wifi: key handshake: a message with a bad MIC (the password?)\n");
        return;
    }
    memcpy(jn.replay, k + 5, 8);
    static uint8_t plain[512];
    int plen = 0;
    if (info & KI_ENCDATA) {
        if (dlen < 24 || dlen > sizeof(plain) + 8 || !wpa_aes_unwrap(jn.ptk + 16, k + 95, dlen, plain)) {
            join_fail("the key data does not decrypt", 0);
            return;
        }
        plen = dlen - 8;
    }
    int gid = 0, glen = 0;
    const uint8_t *gtk = NULL;
    bool have_gtk = plen && find_gtk(plain, plen, &gid, &gtk, &glen);
    if ((info & KI_PAIRWISE) && (info & KI_INSTALL)) {
        /* message 3: the GTK; message 4 (sent in the clear), then the keys into the device */
        if (memcmp(k + 13, jn.anonce, 32)) {
            kprintf("wifi: key handshake: message 3 with another ANonce: ignored\n");
            return;
        }
        send_eapol_key(2 | KI_PAIRWISE | KI_MIC | KI_SECURE, jn.replay, NULL, NULL, 0);
        if (!install_key(jn.ptk + 32, 0, false, NULL)) {
            join_fail("the pairwise key was refused", 0);
            return;
        }
        jn.ptk_set = true;
        if (have_gtk && !install_key(gtk, gid, true, k + 61)) {
            join_fail("the group key was refused", 0);
            return;
        }
        kprintf("wifi: key handshake: message 3, keys set (group key %d)\n", gid);
        if (jn.link == LINK_4WAY)
            link_up();
        return;
    }
    if (!(info & KI_PAIRWISE) && have_gtk) {
        /* the group key handshake: a new GTK */
        send_eapol_key(2 | KI_MIC | KI_SECURE, jn.replay, NULL, NULL, 0);
        install_key(gtk, gid, true, k + 61);
        kprintf("wifi: new group key %d\n", gid);
    }
}

/* A management frame from our access point, in the state it answers. */
static void mgmt_input(const uint8_t *f, uint32_t len)
{
    uint8_t sub = f[0] >> 4;
    const uint8_t *b = f + 24;
    uint32_t bl = len - 24;
    if (sub == 13) {                             /* action: a block ack request is declined (no receive reordering yet) */
        if (bl >= 9 && b[0] == 3 && b[1] == 0 && jn.link >= LINK_4WAY) {
            uint8_t resp[9] = { 3, 1, b[2], 37, 0, b[3], b[4], b[5], b[6] };   /* status 37: request declined */
            send_mgmt(13, resp, sizeof(resp));
        }
        return;
    }
    if ((sub == 12 || sub == 10) && bl >= 2) {   /* deauthentication, disassociation */
        uint16_t reason = b[0] | b[1] << 8;
        if (jn.link == LINK_4WAY && (reason == 15 || reason == 2 || reason == 23))
            join_fail("the access point refused the key (wrong password?), reason %u", reason);
        else
            join_fail(sub == 12 ? "deauthenticated by the access point (reason %u)"
                                : "disassociated by the access point (reason %u)", reason);
        return;
    }
    if (sub == 11 && jn.link == LINK_AUTH && bl >= 6) {
        uint16_t seq = b[2] | b[3] << 8, status = b[4] | b[5] << 8;
        if (seq != 2)
            return;
        if (status) {
            join_fail("authentication refused (status %u)", status);
            return;
        }
        jn.link = LINK_ASSOC;
        jn.tries = 1;
        send_assoc();
        return;
    }
    if ((sub == 1 || sub == 3) && jn.link == LINK_ASSOC && bl >= 6) {
        uint16_t status = b[2] | b[3] << 8;
        if (status) {
            join_fail("association refused (status %u)", status);
            return;
        }
        jn.aid = (b[4] | b[5] << 8) & 0x3FFF;
        const char *err = ctx_assoc();
        if (err) {
            char m[64];
            snprintf(m, sizeof(m), "firmware: %s failed", err);
            join_fail(m, 0);
            return;
        }
        kprintf("wifi: associated (AID %u)\n", jn.aid);
        if (jn.secure) {
            jn.link = LINK_4WAY;
            jn.deadline = ticks + 3 * TIMER_HZ;
            set_info("%s: key handshake", jn.bss.wb_ssid, 0);
        } else {
            link_up();
        }
    }
}

/* The state machine, from wifi_poll (the device locked): a join asked for, the frames received, timeouts. */
static void mlme_step(void)
{
    if (!jn.rxq && !(jn.rxq = kmalloc(sizeof(*jn.rxq) * NRXQ)))
        return;
    if (!jn.pending && jn.retry_at && ticks >= jn.retry_at && jn.link == LINK_IDLE) {
        jn.retry_at = 0;
        jn.pending = true;                       /* (rejoin the network lost) */
    }
    if (jn.pending && !wf.scanning) {
        jn.pending = false;
        if (jn.link == LINK_UP) {
            uint8_t reason[2] = { 3, 0 };        /* leaving */
            send_mgmt(12, reason, 2);
        }
        ctx_teardown();
        jn.link = LINK_IDLE;
        jn.have_replay = false;
        jn.ptk_set = false;
        jn.nrx = 0;
        random_bytes(jn.snonce, 32);
        const char *err = ctx_setup();
        if (err) {
            set_info("firmware: %s failed", err, 0);
            kprintf("wifi: %s failed\n", err);
            ctx_teardown();
            return;
        }
        set_info("%s: authenticating", jn.bss.wb_ssid, 0);
        jn.link = LINK_AUTH;
        jn.tries = 1;
        send_auth();
    }
    for (int i = 0; i < jn.nrx; i++) {
        struct rxf *q = &jn.rxq[i];
        if (q->kind == RXF_MGMT)
            mgmt_input(q->buf, q->len);
        else if (q->kind == RXF_EAPOL && jn.link >= LINK_4WAY)
            eapol_input(q->buf + 14, q->len - 14);
        if (q->kind != RXF_DATA)
            q->kind = 0;
    }
    if ((jn.link == LINK_AUTH || jn.link == LINK_ASSOC) && ticks > jn.deadline) {
        if (++jn.tries > 3)
            join_fail(jn.link == LINK_AUTH ? "no answer to authentication" : "no answer to association", 0);
        else if (jn.link == LINK_AUTH)
            send_auth();
        else
            send_assoc();
    } else if (jn.link == LINK_4WAY && ticks > jn.deadline) {
        join_fail("no key handshake from the access point (wrong password?)", 0);
    }
    if (jn.link == LINK_UP && wf.missed_beacons > 30) {
        wf.missed_beacons = 0;
        join_fail("the access point is gone (%u beacons missed)", 30);
    }
}

/* From the network timer: the firmware's notifications and frames, the state machine; then the frames for the
 * network stack (outside the device lock: the stack may answer at once). */
void wifi_poll(void)
{
    if (!wf.ready || !dev_lock(false))
        return;
    rx_process();
    mlme_step();
    dev_unlock();
    for (int i = 0; jn.rxq && i < jn.nrx; i++)
        if (jn.rxq[i].kind == RXF_DATA && jn.ifp && jn.link == LINK_UP)
            net_rx(jn.ifp, jn.rxq[i].buf, jn.rxq[i].len);
    jn.nrx = 0;
}

static bool hexkey(const char *s, uint8_t out[32])
{
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0)
            return false;
        if (i % 2)
            out[i / 2] |= v;
        else
            out[i / 2] = v << 4;
    }
    return s[64] == 0;
}

/* Join a network of the last scan: the strongest one with the SSID (or the BSSID given). */
static int wifi_join(const struct sieos_wifi_connect *c)
{
    if (!wf.ready)
        return -ENXIO;
    int best = -1;
    static const uint8_t zero[6];
    for (int i = 0; i < wf.nbss; i++) {
        if (memcmp(c->wc_bssid, zero, 6) ? memcmp(wf.bss[i].wb_bssid, c->wc_bssid, 6)
                                         : strncmp(wf.bss[i].wb_ssid, c->wc_ssid, sizeof(c->wc_ssid)))
            continue;
        if (best < 0 || wf.bss[i].wb_rssi > wf.bss[best].wb_rssi)
            best = i;
    }
    if (best < 0)
        return -ENOENT;
    const struct sieos_wifi_bss *b = &wf.bss[best];
    const struct bss_info *x = &wf.bssx[best];
    bool secure = b->wb_sec != 0;
    if (secure) {
        static const uint8_t ccmp[4] = { 0x00, 0x0F, 0xAC, 4 };
        if (!(b->wb_sec & SIEOS_WIFI_SEC_RSN) || !x->ccmp || !x->psk || x->mfpr ||
            (memcmp(x->group, ccmp, 4) && (x->group[0] || x->group[1] || x->group[2])))
            return -EOPNOTSUPP;                  /* (WPA2-Personal with CCMP only, for now) */
        size_t kl = strnlen(c->wc_key, sizeof(c->wc_key));
        uint8_t pmk[32];
        if (kl == 64 && hexkey(c->wc_key, pmk))
            ;
        else if (kl >= 8 && kl <= 63)
            wpa_passphrase_pmk(c->wc_key, (const uint8_t *)b->wb_ssid, strlen(b->wb_ssid), pmk);
        else
            return -EINVAL;
        memcpy(jn.pmk, pmk, 32);
    }
    jn.bss = *b;
    jn.bx = *x;
    jn.secure = secure;
    jn.rejoins = 0;
    jn.retry_at = 0;
    jn.pending = true;
    set_info("%s: joining", b->wb_ssid, 0);
    return 0;
}

static void wifi_leave(void)
{
    jn.pending = false;
    jn.retry_at = 0;
    if (!dev_lock(true))
        return;
    if (jn.link == LINK_UP || jn.link == LINK_4WAY) {
        uint8_t reason[2] = { 3, 0 };            /* leaving */
        send_mgmt(12, reason, 2);
        udelay(20000);
    }
    ctx_teardown();
    jn.link = LINK_IDLE;
    if (jn.ifp)
        jn.ifp->up = false;
    wf.state = "ready";
    dev_unlock();
}

/* The wifi() system call: the status, a scan, its results, joining and leaving a network. */
long wifi_op(int op, void *buf, long n)
{
    switch (op) {
    case SIEOS_WIFI_OP_STATUS: {
        if (n < (long)sizeof(struct sieos_wifi_status))
            return -EINVAL;
        struct sieos_wifi_status *st = buf;
        memset(st, 0, sizeof(*st));
        st->ws_state = !wf.pci.vendor ? SIEOS_WIFI_NONE : !wf.ready ? SIEOS_WIFI_DOWN
                     : jn.link == LINK_UP ? SIEOS_WIFI_JOINED
                     : jn.pending || jn.link != LINK_IDLE ? SIEOS_WIFI_JOINING
                     : wf.scanning ? SIEOS_WIFI_SCANNING : SIEOS_WIFI_READY;
        strlcpy(st->ws_info, wf.state, sizeof(st->ws_info));
        memcpy(st->ws_mac, wf.mac, 6);
        if (jn.link != LINK_IDLE || jn.pending)
            memcpy(st->ws_ssid, jn.bss.wb_ssid, sizeof(st->ws_ssid));
        st->ws_nbss = wf.nbss;
        st->ws_scans = wf.scans;
        st->ws_link = jn.link == LINK_UP && jn.ifp ? jn.ifp->index : -1;
        st->ws_power = jn.powermode;
        st->ws_mode = jn.link != LINK_UP ? 0 : jn.vht ? 3 : jn.ht ? 2 : 1;
        st->ws_width = jn.link == LINK_UP ? 20 << jn.width : 0;
        st->ws_streams = jn.link == LINK_UP ? (jn.mimo ? 2 : 1) : 0;
        return 0;
    }
    case SIEOS_WIFI_OP_SCAN:
        if (jn.link != LINK_IDLE && jn.link != LINK_UP)
            return -EBUSY;
        return scan_start();
    case SIEOS_WIFI_OP_RESULTS: {
        struct sieos_wifi_bss *out = buf;
        long k = 0;
        for (int i = 0; i < wf.nbss && k < n; i++, k++) {
            out[k] = wf.bss[i];
            out[k].wb_age = (ticks - wf.bss_seen[i]) / TIMER_HZ;
        }
        return k;
    }
    case SIEOS_WIFI_OP_CONNECT:
        if (n < (long)sizeof(struct sieos_wifi_connect))
            return -EINVAL;
        return wifi_join(buf);
    case SIEOS_WIFI_OP_DISCONNECT:
        wifi_leave();
        return 0;
    case SIEOS_WIFI_OP_POWER:
        if (n < 0 || n > 2)
            return -EINVAL;
        jn.powermode = (int)n;
        if (wf.ready && dev_lock(true)) {
            power_apply();
            dev_unlock();
        }
        return 0;
    }
    return -EINVAL;
}

void wifi_init(const char *cmdline)
{
    static const uint16_t ids[] = { 0x34F0, 0x02F0, 0x06F0, 0xA0F0, 0x43F0, 0x4DF0, 0x3DF0,   /* Qu, QuZ (Hr) */
                                    0x2725 };                                                 /* Ty (Gf) */
    if (!pci_find(0x8086, ids, ARRAY_SIZE(ids), &wf.pci))
        return;
    if (boot_option(cmdline, "nowifi")) {
        wf.state = "off (nowifi)";
        return;
    }
    const struct pci_dev *pd = &wf.pci;
    pci_enable_path(pd);
    uint32_t cmd = pci_read32(pd->bus, pd->dev, pd->func, 4);
    pci_write32(pd->bus, pd->dev, pd->func, 4, cmd | 0x6 | 0x400);   /* memory, bus master, INTx off */
    bool io;
    uint64_t bar = pci_bar_addr(pd, 0, &io);
    if (!bar || io || !(wf.csr = mmio_map(bar, 0x4000))) {
        kprintf("wifi: 8086:%04x: no register BAR\n", pd->device);
        return;
    }
    wf.hw_rev = rd(CSR_HW_REV);
    /* From the 8000 family on, the step is in bits 1-0 (where the dash was): moved to 3-2, the dash 0
     * (0x332: step 2, C) - the MAC's step chooses the firmware and goes in the context info. */
    if (wf.hw_rev != 0xFFFFFFFF)
        wf.hw_rev = (wf.hw_rev & 0xFFFFFFF0) | (wf.hw_rev & 3) << 2;
    wf.rf_id = rd(CSR_HW_RF_ID);
    if (wf.hw_rev == 0xFFFFFFFF) {
        kprintf("wifi: 8086:%04x does not answer (powered off?)\n", pd->device);
        wf.state = "does not answer";
        return;
    }
    /* the firmware file: Qu (Ice Lake) or QuZ (Comet Lake), the MAC's step; Ty (the AX210) */
    int step = (wf.hw_rev >> 2) & 3;
    wf.gen3 = pd->device == 0x2725;
    wf.integrated = !wf.gen3;
    bool quz = pd->device == 0x02F0 || pd->device == 0x06F0;
    if (wf.gen3)
        snprintf(wf.fw_name, sizeof(wf.fw_name), "iwlwifi-ty-a0-gf-a0-77.ucode");
    else
        snprintf(wf.fw_name, sizeof(wf.fw_name), "iwlwifi-%s-%c0-hr-b0-77.ucode", quz ? "QuZ" : "Qu",
                 quz ? 'a' : step >= 2 ? 'c' : 'b');
    kprintf("wifi: Intel %s (8086:%04x), hw rev %08x (step %c), rf %08x: %s\n",
            wf.gen3 ? "Wi-Fi 6E AX210" : "Wi-Fi 6 AX201", pd->device, wf.hw_rev, 'A' + step, wf.rf_id, wf.fw_name);
    char path[96];
    snprintf(path, sizeof(path), "/lib/firmware/%s", wf.fw_name);
    int err;
    struct inode *ip = namei(path, &err);
    if (!ip) {
        fail("the firmware file is missing");
        return;
    }
    /* the context info (gen3: with the peripheral scratch and information), the receive ring, the command queue */
    size_t used_size = wf.gen3 ? NRBD * USED_DESC_GEN3 : NRBD * 4, free_size = wf.gen3 ? NRBD * 16 : NRBD * 8;
    wf.ci_pa = pmm_alloc_contig((sizeof(struct ctxt_info) + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t free_pa = pmm_alloc_contig((free_size + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t used_pa = pmm_alloc_contig((used_size + PAGE_SIZE - 1) / PAGE_SIZE), st_pa = pmm_alloc_contig(1);
    uint64_t cmdq_pa = pmm_alloc_contig(CMDQ_SIZE * 256 / PAGE_SIZE);
    uint64_t scratch_pa = wf.gen3 ? pmm_alloc_contig(1) : 0, info_pa = wf.gen3 ? pmm_alloc_contig(1) : 0;
    if (!wf.ci_pa || !free_pa || !used_pa || !st_pa || !cmdq_pa || (wf.gen3 && (!scratch_pa || !info_pa))) {
        iput(ip);
        fail("no memory");
        return;
    }
    wf.ci = P2V(wf.ci_pa);
    memset(wf.ci, 0, sizeof(struct ctxt_info));
    if (wf.gen3) {
        wf.scratch = P2V(scratch_pa);
        memset(wf.scratch, 0, PAGE_SIZE);
        memset(P2V(info_pa), 0, PAGE_SIZE);
        wf.dram = (struct fw_dram *)(void *)((uint8_t *)wf.scratch + __builtin_offsetof(struct prph_scratch, dram));
    } else {
        wf.dram = (struct fw_dram *)wf.ci->umac_img;
    }
    uint32_t phy = 0;
    bool ok = load_firmware(ip, &phy);
    iput(ip);
    wf.phy_cfg = phy;
    if (!ok || !start(phy))
        return;

    /* the receive ring: free descriptors (address | id; gen3: id, address), used ones (the ids), the status */
    wf.free_rbd = P2V(free_pa);
    wf.free_rbd3 = P2V(free_pa);
    wf.used_rbd = P2V(used_pa);
    wf.used_rbd3 = P2V(used_pa);
    memset(wf.used_rbd3, 0, used_size);
    wf.rb_status = P2V(st_pa);
    memset((void *)wf.rb_status, 0, PAGE_SIZE);
    for (int i = 0; i < NRBD; i++) {
        if (!(wf.rb_pa[i] = pmm_alloc_contig(1)))
            return (void)fail("no memory for the receive buffers");
        if (wf.gen3) {
            wf.free_rbd3[i].rbid = i;
            wf.free_rbd3[i].addr = wf.rb_pa[i];
        } else {
            wf.free_rbd[i] = wf.rb_pa[i] | (uint64_t)i;             /* the buffer's address | its index */
        }
    }
    wf.cmdq = P2V(cmdq_pa);
    wf.cmdq_pa = cmdq_pa;
    wf.cmdbuf_pa = pmm_alloc_contig(1);
    if (!wf.cmdbuf_pa)
        return (void)fail("no memory for the command buffer");
    wf.cmdbuf = P2V(wf.cmdbuf_pa);
    if (wf.gen3) {
        struct prph_scratch *sc = wf.scratch;
        sc->mac_id = rd(CSR_HW_REV) & 0xFFFF;
        sc->version = 0;
        sc->size = sizeof(*sc) / 4;
        sc->control_flags = PRPH_RB_SIZE_4K | PRPH_MTR_MODE | PRPH_MTR_FORMAT_256B;
        sc->free_rbd_addr = free_pa;
        struct ctxt_info_gen3 *c3 = (struct ctxt_info_gen3 *)wf.ci;
        c3->prph_info_addr = info_pa;
        c3->prph_scratch_addr = scratch_pa;
        c3->prph_scratch_size = sizeof(*sc);
        c3->cr_head_idx_addr = st_pa;
        c3->tr_tail_idx_addr = info_pa + PAGE_SIZE / 2;     /* (unused tail indexes the device still writes) */
        c3->cr_tail_idx_addr = info_pa + 3 * PAGE_SIZE / 4;
        c3->mtr_addr = cmdq_pa;
        c3->mcr_addr = used_pa;
        c3->mtr_size = CMDQ_CB;
        c3->mcr_size = RB_CB_LOG;
    } else {
        struct ctxt_info *ci = wf.ci;
        ci->mac_id = rd(CSR_HW_REV) & 0xFFFF;    /* (the register as it reads) */
        ci->version = 0;
        ci->size = sizeof(*ci) / 4;
        ci->control_flags = CTXT_TFD_FORMAT_LONG | (uint32_t)RB_CB_LOG << CTXT_RB_CB_SIZE_POS |
                            CTXT_RB_SIZE_4K << CTXT_RB_SIZE_POS;
        ci->free_rbd_addr = free_pa;
        ci->used_rbd_addr = used_pa;
        ci->status_wr_ptr = st_pa;
        ci->cmd_queue_addr = cmdq_pa;
        ci->cmd_queue_size = CMDQ_CB;
    }
    __asm__ volatile("wbinvd" ::: "memory");     /* (the device may not snoop: all of it in memory) */

    if (wf.gen3) {
        uint64_t iml_pa = V2P(wf.iml);
        wr(CSR_CTXT_INFO_ADDR, (uint32_t)wf.ci_pa);
        wr(CSR_CTXT_INFO_ADDR + 4, (uint32_t)(wf.ci_pa >> 32));
        wr(CSR_IML_DATA_ADDR, (uint32_t)iml_pa);
        wr(CSR_IML_DATA_ADDR + 4, (uint32_t)(iml_pa >> 32));
        wr(CSR_IML_SIZE_ADDR, wf.iml_len);
        set_bits(CSR_CTXT_INFO_BOOT_CTRL, AUTO_FUNC_BOOT_ENA);
    } else {
        wr(CSR_CTXT_INFO_BA, (uint32_t)wf.ci_pa);    /* (two 32-bit writes: a 64-bit one does not take on the AX201) */
        wr(CSR_CTXT_INFO_BA + 4, (uint32_t)(wf.ci_pa >> 32));
    }
    if (!grab_nic())
        return (void)fail("the NIC does not wake (MAC access)");
    if (wf.integrated) {
        write_prph(HPM_MAC_LTR_CSR, HPM_LTR_ENABLE_ALL);   /* the boot LTR (integrated 22000) */
        write_prph(HPM_UMAC_LTR, BOOT_LTR);
    } else {
        wr(CSR_LTR_LONG_VAL_AD, BOOT_LTR);       /* (a discrete card: the CSR) */
    }
    kprintf("wifi: context info at %#lx%s, starting the CPUs\n", wf.ci_pa, wf.gen3 ? " (gen3)" : "");
    write_prph(umac(UREG_CPU_INIT_RUN), 1);      /* the CPUs load the firmware from the context info */
    release_nic();
    kprintf("wifi: starting the firmware\n");

    /* the ALIVE interrupt cause first (the firmware has set up the receive side), then buffers, then the packet */
    bool alive_int = false;
    for (int t = 0; t < 3000 && !wf.alive; t++) {
        uint32_t cause = rd(CSR_INT);
        if (!alive_int && (cause & INT_ALIVE)) {
            alive_int = true;
            wr(CSR_INT, INT_ALIVE);
            wr(RFH_Q0_FRBDCB_WIDX_TRG, NRB_FIRST);
        }
        if (alive_int)
            rx_process();
        udelay(1000);
    }
    if (wf.alive) {
        wf.state = "firmware alive";
        pci_claim(pd, "iwlwifi");
        firmware_init();
        return;
    }
    fail(alive_int ? "the ALIVE interrupt came, but no ALIVE packet in 3 s" : "no ALIVE from the firmware in 3 s");
    /* what the device's CPUs did: their load (secure boot) status and where they run */
    if (grab_nic()) {
        kprintf("wifi: CPU1 status %08x, CPU2 status %08x, UMAC PC %08x, LMAC PC %08x, RX closed %u, INT %08x, FH %08x\n",
                read_prph(0xA038C0), read_prph(0xA038C4), read_prph(umac(0xA05C18)), read_prph(umac(0xA05C1C)),
                wf.rb_status[0] & 0xFFF, rd(CSR_INT), rd(CSR_FH_INT_STATUS));
        release_nic();
    }
    sw_reset();                                  /* (stop it: no DMA into memory we give back) */
}

const char *wifi_state(void)
{
    return wf.state;
}

DDI_DRIVER("iwlwifi", DDI_PHASE_ROOT, "Intel Wi-Fi 6 AX201 (Qu, QuZ with the Hr radio), Wi-Fi 6E AX210 (Ty, Gf)");
DDI_ALIAS("pci8086,34f0");
DDI_ALIAS("pci8086,2f0");
DDI_ALIAS("pci8086,6f0");
DDI_ALIAS("pci8086,a0f0");
DDI_ALIAS("pci8086,43f0");
DDI_ALIAS("pci8086,4df0");
DDI_ALIAS("pci8086,3df0");
DDI_ALIAS("pci8086,2725");

int _init(void)
{
    wifi_init(ddi_cmdline());
    ddi_poll_register(wifi_poll);
    ddi_wifi_op = wifi_op;
    return 0;
}
