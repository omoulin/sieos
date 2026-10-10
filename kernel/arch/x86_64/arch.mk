# arch.mk (x86-64) - What the Makefile needs to build the kernel for this
# processor: its objects (from kernel/arch/x86_64/), compiler flags and
# linker script.
#
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

# boot.S first (the multiboot header), then the entry code
KARCH_OBJS   := boot entry ap cpu fpu smp mmu platform
# Linked in the top 2 GiB (-mcmodel=kernel). No "red zone": interrupts
# arriving in kernel mode use the same stack.
KARCH_CFLAGS := -mcmodel=kernel -mno-red-zone
KARCH_LDS    := kernel/arch/x86_64/linker.ld
# The tools that build SIEOS for this processor (the host's, or sicc's
# under cc-sieos), what QEMU loads, and what this processor does not have.
TCC       ?= gcc
TLD       ?= ld
TAR       ?= ar
TOBJCOPY  ?= objcopy
TOOLDEP   :=
KARCH_EXTRA :=
KARCH_NOBINS :=
KTEST_CC = $(B)/host/sicc      # ktest-sicc: ktest built by sicc (ktest itself: by gcc)
