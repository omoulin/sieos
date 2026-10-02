/*
 * sieos/nvgpu.h - /dev/nvgpuN: an NVIDIA GPU for Vulkan (Mesa's NVK, through
 * its kernel interface nvkmd: nvkmd_sieos), as drv/nvgpu drives it with
 * NVIDIA's GSP-RM.
 *
 * A process opens the device and has, for that open file: GPU memory objects
 * (handles; in VRAM or in system memory, mapped into the process by mmap at
 * the offset the kernel gives), a GPU virtual address space (ranges reserved,
 * memory bound into them), channels (an engine set each: compute, copy, 3D)
 * that run push buffers, and timeline semaphores (64-bit values the GPU's
 * work and the processor signal; waited on with a timeout).
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_ABI_NVGPU_H
#define SIEOS_ABI_NVGPU_H

#define SIEOS_DEV_NVGPU_MAJOR     195          /* 195,n = /dev/nvgpun */

#define SIEOS_NVGPU_IOC_BASE      (('N' << 16) | ('V' << 8))
#define SIEOS_NVGPU_INFO          (SIEOS_NVGPU_IOC_BASE | 0x01)   /* struct sieos_nvgpu_info (out) */
#define SIEOS_NVGPU_MEM_NEW       (SIEOS_NVGPU_IOC_BASE | 0x10)   /* struct sieos_nvgpu_mem */
#define SIEOS_NVGPU_MEM_FREE      (SIEOS_NVGPU_IOC_BASE | 0x11)   /* struct sieos_nvgpu_mem (handle) */
#define SIEOS_NVGPU_VA_ALLOC      (SIEOS_NVGPU_IOC_BASE | 0x20)   /* struct sieos_nvgpu_va */
#define SIEOS_NVGPU_VA_FREE       (SIEOS_NVGPU_IOC_BASE | 0x21)   /* struct sieos_nvgpu_va (addr, size) */
#define SIEOS_NVGPU_BIND          (SIEOS_NVGPU_IOC_BASE | 0x22)   /* struct sieos_nvgpu_bind */
#define SIEOS_NVGPU_CHAN_NEW      (SIEOS_NVGPU_IOC_BASE | 0x30)   /* struct sieos_nvgpu_chan */
#define SIEOS_NVGPU_CHAN_FREE     (SIEOS_NVGPU_IOC_BASE | 0x31)   /* struct sieos_nvgpu_chan (chan) */
#define SIEOS_NVGPU_EXEC          (SIEOS_NVGPU_IOC_BASE | 0x32)   /* struct sieos_nvgpu_exec */
#define SIEOS_NVGPU_SYNC_NEW      (SIEOS_NVGPU_IOC_BASE | 0x40)   /* struct sieos_nvgpu_sync */
#define SIEOS_NVGPU_SYNC_FREE     (SIEOS_NVGPU_IOC_BASE | 0x41)   /* struct sieos_nvgpu_sync (handle) */
#define SIEOS_NVGPU_SYNC_SIGNAL   (SIEOS_NVGPU_IOC_BASE | 0x42)   /* struct sieos_nvgpu_sync (handle, value): by the processor */
#define SIEOS_NVGPU_SYNC_QUERY    (SIEOS_NVGPU_IOC_BASE | 0x43)   /* struct sieos_nvgpu_sync (handle; value out) */
#define SIEOS_NVGPU_SYNC_WAIT     (SIEOS_NVGPU_IOC_BASE | 0x44)   /* struct sieos_nvgpu_wait */
#define SIEOS_NVGPU_TIMESTAMP     (SIEOS_NVGPU_IOC_BASE | 0x50)   /* unsigned long long (out): the GPU's clock, ns */

struct sieos_nvgpu_info {
    unsigned int version;                      /* 1 */
    unsigned short chipset, device_id, subvendor_id, subdevice_id;
    unsigned char revision, pci_bus, pci_dev, pci_func;
    unsigned short pci_domain, gpc_count;
    unsigned int tpc_count;
    unsigned short cls_copy, cls_eng2d, cls_eng3d, cls_m2mf, cls_compute, cls_gpfifo;
    unsigned long long vram_size, vram_used, bar1_size;
    unsigned long long va_start, va_end;      /* the VA space a process may reserve in */
    unsigned int max_push;                     /* push buffers in one EXEC */
    unsigned int bind_align;                   /* VA and binding granularity (bytes) */
    char name[64], chip[16];
};

/* memory: where (VRAM or system memory), mappable, coherent; its PTE kind for VRAM */
#define SIEOS_NVGPU_MEM_VRAM      0x01
#define SIEOS_NVGPU_MEM_GART      0x02         /* system memory the GPU reaches */
#define SIEOS_NVGPU_MEM_CAN_MAP   0x04
#define SIEOS_NVGPU_MEM_COHERENT  0x08
struct sieos_nvgpu_mem {
    unsigned long long size, align;
    unsigned int flags;
    unsigned char pte_kind, pad;
    unsigned short tile_mode;
    unsigned int handle;                       /* out (in for FREE) */
    unsigned int pad2;
    unsigned long long map_offset;             /* out: mmap's offset for it on the device's file */
};

#define SIEOS_NVGPU_VA_FIXED      0x01         /* addr is where it must be */
#define SIEOS_NVGPU_VA_SPARSE     0x02
struct sieos_nvgpu_va {
    unsigned long long addr, size, align;      /* addr: in with FIXED (and FREE), else out */
    unsigned int flags, pad;
};

/* bind (map, unmap) operations, after some semaphores' values, before others' */
#define SIEOS_NVGPU_BIND_MAP      0
#define SIEOS_NVGPU_BIND_UNMAP    1
struct sieos_nvgpu_bind_op {
    unsigned int op, handle;                   /* handle: MAP's memory */
    unsigned long long addr, range, mem_offset;
    unsigned char pte_kind, pad[7];
};
struct sieos_nvgpu_sync_point {
    unsigned int handle, pad;
    unsigned long long value;                  /* (a timeline's value) */
};
struct sieos_nvgpu_bind {
    unsigned int op_count, wait_count, sig_count, pad;
    unsigned long long op_ptr, wait_ptr, sig_ptr;   /* bind_op[], sync_point[], sync_point[] */
};

/* channels: an engine set each */
#define SIEOS_NVGPU_ENGINE_COPY    0x01
#define SIEOS_NVGPU_ENGINE_2D      0x02
#define SIEOS_NVGPU_ENGINE_3D      0x04
#define SIEOS_NVGPU_ENGINE_M2MF    0x08
#define SIEOS_NVGPU_ENGINE_COMPUTE 0x10
struct sieos_nvgpu_chan {
    unsigned int engines;
    unsigned int chan;                         /* out (in for FREE) */
};

/* push buffers into a channel, after some semaphores' values; others signaled when done */
#define SIEOS_NVGPU_PUSH_NO_PREFETCH 0x01
struct sieos_nvgpu_push {
    unsigned long long va;
    unsigned int len, flags;
};
struct sieos_nvgpu_exec {
    unsigned int chan, push_count, wait_count, sig_count;
    unsigned long long push_ptr, wait_ptr, sig_ptr;  /* push[], sync_point[], sync_point[] */
};

struct sieos_nvgpu_sync {
    unsigned int handle, pad;                  /* out for NEW */
    unsigned long long value;                  /* NEW: the initial value; SIGNAL: in; QUERY: out */
};

#define SIEOS_NVGPU_WAIT_ALL      0x01         /* every point reached, else one */
struct sieos_nvgpu_wait {
    unsigned long long points_ptr;             /* sync_point[] */
    unsigned int count, flags;
    long long timeout_ns;                      /* absolute, CLOCK_MONOTONIC; -1: none */
    unsigned int first;                        /* out: (one) the first point reached */
    unsigned int pad;
};

#endif
