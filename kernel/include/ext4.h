/*
 * ext4.h - ext4 on-disk structures.
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#ifndef SIEOS_EXT4_H
#define SIEOS_EXT4_H

#include "kernel.h"

#define EXT4_SUPER_MAGIC 0xEF53
#define EXT4_ROOT_INO    2

/* s_feature_compat */
#define COMPAT_HAS_JOURNAL     0x0004
#define COMPAT_DIR_INDEX       0x0020
/* s_feature_incompat */
#define INCOMPAT_FILETYPE      0x0002
#define INCOMPAT_RECOVER       0x0004
#define INCOMPAT_JOURNAL_DEV   0x0008
#define INCOMPAT_META_BG       0x0010
#define INCOMPAT_EXTENTS       0x0040
#define INCOMPAT_64BIT         0x0080
#define INCOMPAT_MMP           0x0100
#define INCOMPAT_FLEX_BG       0x0200
#define INCOMPAT_EA_INODE      0x0400
#define INCOMPAT_DIRDATA       0x1000
#define INCOMPAT_CSUM_SEED     0x2000
#define INCOMPAT_LARGEDIR      0x4000
#define INCOMPAT_INLINE_DATA   0x8000
#define INCOMPAT_ENCRYPT       0x10000
#define INCOMPAT_CASEFOLD      0x20000
/* s_feature_ro_compat */
#define RO_COMPAT_SPARSE_SUPER  0x0001
#define RO_COMPAT_LARGE_FILE    0x0002
#define RO_COMPAT_BTREE_DIR     0x0004
#define RO_COMPAT_HUGE_FILE     0x0008
#define RO_COMPAT_GDT_CSUM      0x0010
#define RO_COMPAT_DIR_NLINK     0x0020
#define RO_COMPAT_EXTRA_ISIZE   0x0040
#define RO_COMPAT_QUOTA         0x0100
#define RO_COMPAT_BIGALLOC      0x0200
#define RO_COMPAT_METADATA_CSUM 0x0400
#define RO_COMPAT_READONLY      0x1000
#define RO_COMPAT_PROJECT       0x2000
#define RO_COMPAT_ORPHAN_PRESENT 0x10000

/* bg_flags */
#define BG_INODE_UNINIT 0x0001
#define BG_BLOCK_UNINIT 0x0002
#define BG_INODE_ZEROED 0x0004

/* i_flags */
#define EXT4_INDEX_FL       0x00001000
#define EXT4_HUGE_FILE_FL   0x00040000
#define EXT4_EXTENTS_FL     0x00080000
#define EXT4_INLINE_DATA_FL 0x10000000

/* directory entry file types */
#define EXT4_FT_UNKNOWN 0
#define EXT4_FT_REG     1
#define EXT4_FT_DIR     2
#define EXT4_FT_CHRDEV  3
#define EXT4_FT_BLKDEV  4
#define EXT4_FT_FIFO    5
#define EXT4_FT_SOCK    6
#define EXT4_FT_SYMLINK 7
#define EXT4_FT_DIR_CSUM 0xDE

struct ext4_super {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_checksum_type;
    uint16_t s_reserved_pad;
    uint64_t s_kbytes_written;
    uint8_t  s_pad1[0x270 - 0x180];
    uint32_t s_checksum_seed;
    uint8_t  s_pad2[0x3FC - 0x274];
    uint32_t s_checksum;
} __attribute__((packed));

struct ext4_gd {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} __attribute__((packed));

struct ext4_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_version_lo;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;
    uint32_t i_obso_faddr;
    uint16_t i_blocks_high;
    uint16_t i_file_acl_high;
    uint16_t i_uid_high;
    uint16_t i_gid_high;
    uint16_t i_checksum_lo;
    uint16_t i_reserved;
    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
} __attribute__((packed));

#define EXT4_EXT_MAGIC 0xF30A

struct ext4_extent_header {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} __attribute__((packed));

struct ext4_extent {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} __attribute__((packed));

struct ext4_extent_idx {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} __attribute__((packed));

struct ext4_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed));

struct ext4_dirent_tail {
    uint32_t det_reserved_zero1;
    uint16_t det_rec_len;
    uint8_t  det_reserved_zero2;
    uint8_t  det_reserved_ft;
    uint32_t det_checksum;
} __attribute__((packed));

_Static_assert(sizeof(struct ext4_super) == 1024, "ext4_super size");
_Static_assert(__builtin_offsetof(struct ext4_super, s_desc_size) == 0xFE, "s_desc_size");
_Static_assert(__builtin_offsetof(struct ext4_super, s_blocks_count_hi) == 0x150, "s_blocks_count_hi");
_Static_assert(__builtin_offsetof(struct ext4_super, s_kbytes_written) == 0x178, "s_kbytes_written");
_Static_assert(sizeof(struct ext4_gd) == 64, "ext4_gd size");
_Static_assert(__builtin_offsetof(struct ext4_gd, bg_checksum) == 0x1E, "bg_checksum");
_Static_assert(sizeof(struct ext4_inode) == 160, "ext4_inode size");
_Static_assert(__builtin_offsetof(struct ext4_inode, i_checksum_lo) == 0x7C, "i_checksum_lo");
_Static_assert(__builtin_offsetof(struct ext4_inode, i_checksum_hi) == 0x82, "i_checksum_hi");

#endif
