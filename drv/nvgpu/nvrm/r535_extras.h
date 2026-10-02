/* SPDX-License-Identifier: MIT */

/* Copyright (c) 2023 - 2025, NVIDIA CORPORATION. All rights reserved. */

#ifndef __NVRM_R535_EXTRAS_H__
#define __NVRM_R535_EXTRAS_H__
#include "nvtypes.h"

/*
 * Excerpt of RM headers from https://github.com/NVIDIA/open-gpu-kernel-modules
 * (as Linux's nouveau has them in nvkm/subdev/gsp/rm/r535/nvrm/gsp.h): the
 * definitions GSP-RM 570 shares with 535, which the r570 excerpt does not
 * repeat.
 */

typedef struct
{
    NvU32 version;   // queue version
    NvU32 size;      // bytes, page aligned
    NvU32 msgSize;   // entry size, bytes, must be power-of-2, 16 is minimum
    NvU32 msgCount;  // number of entries in queue
    NvU32 writePtr;  // message id of next slot
    NvU32 flags;     // if set it means "i want to swap RX"
    NvU32 rxHdrOff;  // Offset of msgqRxHeader from start of backing store.
    NvU32 entryOff;  // Offset of entries from start of backing store.
} msgqTxHeader;

typedef struct
{
    NvU32 readPtr; // message id of last message read
} msgqRxHeader;

typedef NvU64 LibosAddress;

typedef enum
{
    LIBOS_MEMORY_REGION_NONE,
    LIBOS_MEMORY_REGION_CONTIGUOUS,
    LIBOS_MEMORY_REGION_RADIX3
} LibosMemoryRegionKind;

typedef enum
{
    LIBOS_MEMORY_REGION_LOC_NONE,
    LIBOS_MEMORY_REGION_LOC_SYSMEM,
    LIBOS_MEMORY_REGION_LOC_FB
} LibosMemoryRegionLoc;

typedef struct
{
    LibosAddress          id8;  // Id tag.
    LibosAddress          pa;   // Physical address.
    LibosAddress          size; // Size of memory area.
    NvU8                  kind; // See LibosMemoryRegionKind above.
    NvU8                  loc;  // See LibosMemoryRegionLoc above.
} LibosMemoryRegionInitArgument;

#define REGISTRY_TABLE_ENTRY_TYPE_DWORD   1U

typedef struct PACKED_REGISTRY_ENTRY
{
    NvU32                   nameOffset;
    NvU8                    type;
    NvU32                   data;
    NvU32                   length;
} PACKED_REGISTRY_ENTRY;

typedef struct PACKED_REGISTRY_TABLE
{
    NvU32                   size;
    NvU32                   numEntries;
    PACKED_REGISTRY_ENTRY   entries[];
} PACKED_REGISTRY_TABLE;

typedef struct {
    NvU32  version;                         // structure version
    NvU32  bootloaderOffset;
    NvU32  bootloaderSize;
    NvU32  bootloaderParamOffset;
    NvU32  bootloaderParamSize;
    NvU32  riscvElfOffset;
    NvU32  riscvElfSize;
    NvU32  appVersion;                      // Changelist number associated with the image
    NvU32  manifestOffset;
    NvU32  manifestSize;
    NvU32  monitorDataOffset;
    NvU32  monitorDataSize;
    NvU32  monitorCodeOffset;
    NvU32  monitorCodeSize;
    NvU32  bIsMonitorEnabled;
    NvU32  swbromCodeOffset;
    NvU32  swbromCodeSize;
    NvU32  swbromDataOffset;
    NvU32  swbromDataSize;
    NvU32  fbReservedSize;
    NvU32  bSignedAsCode;
} RM_RISCV_UCODE_DESC;

#define GSP_FW_WPR_META_MAGIC     0xdc3aae21371a60b3ULL
#define GSP_FW_WPR_META_REVISION  1

#define GSP_FW_HEAP_PARAM_SIZE_PER_GB_FB                  (96 << 10)   // All architectures
#define GSP_FW_HEAP_PARAM_CLIENT_ALLOC_SIZE      ((48 << 10) * 2048)   // Support 2048 channels

#endif
