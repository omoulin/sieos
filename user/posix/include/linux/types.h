/*
 * linux/types.h - The kernel's fixed-size types, as Linux's uapi headers
 * name them, for ported code that includes them (GLib's futex calls).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
typedef signed char __s8;
typedef unsigned char __u8;
typedef short __s16;
typedef unsigned short __u16;
typedef int __s32;
typedef unsigned int __u32;
typedef long long __s64;
typedef unsigned long long __u64;
typedef long __kernel_long_t;
typedef unsigned long __kernel_ulong_t;
typedef __kernel_long_t __kernel_time_t;
typedef __kernel_long_t __kernel_off_t;
typedef int __kernel_pid_t;
typedef unsigned int __kernel_uid_t, __kernel_gid_t;
typedef __u16 __le16, __be16;
typedef __u32 __le32, __be32;
typedef __u64 __le64, __be64;
