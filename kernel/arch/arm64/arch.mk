# arch.mk (arm64) - What the Makefile needs to build SIEOS for AArch64
# processors (QEMU's "virt" machine today; the Raspberry Pi 4 and 5 next).
# Everything is built by sicc, SIEOS's own compiler (--target=aarch64):
# no other compiler is involved.
#
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

# boot.S first (the Image header the loaders look for), then the rest
KARCH_OBJS   := boot entry cpu fpu smp mmu platform fdt diag
KARCH_CFLAGS :=
KARCH_LDS    := kernel/arch/arm64/linker.ld
# sicc (built by the host compiler first) is compiler, assembler, linker,
# archiver and objcopy in one program
TCC       = $(SICC) --target=aarch64
TLD       = $(SICC) --target=aarch64 --ld
TAR       = $(SICC) --ar
TOBJCOPY  = $(SICC) --objcopy
SICC     ?= $(B)/host/sicc
TOOLDEP   = $(SICC)      # (objects depend on it: a compiler change rebuilds them)
# the boot modules go to QEMU (or a Pi's firmware) as one "initrd" archive
KARCH_EXTRA = $(B)/modules.img
# programs this port does not have yet (none: the desktop finds its screen
# through screen.c: QEMU's ramfb, the Pi 4's firmware, the Pi 5's frame buffer)
KARCH_NOBINS :=
# the tests' timeouts (tools/*.py): QEMU emulates these processors on a PC
export SIEOS_SLOW ?= 4
KTEST_CC = $(TCC)                # ktest-sicc (the same as ktest here: both by sicc)
