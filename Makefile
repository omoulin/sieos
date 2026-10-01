# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
# SIEOS - Synthetic Intelligence Enhanced Operating System
#
#   make          build kernel, user programs, bootable ISO and ext4 disk image
#   make run      boot in QEMU with the persistent disk (VGA window + serial)
#   make run-nox  same, serial console only (no window)
#   make run-uefi same as 'make run' but boots through UEFI firmware (OVMF);
#                 GPU=intel passes the host's Intel GPU to it (tools/vfio-gpu.sh)
#   make run-iso  boot the ISO alone (root fs is a RAM disk from the ISO)
#   make usb      build/sieos-usb.img, to write to a USB drive and boot a real PC
#   make run-usb  boot that image in QEMU (UEFI) as a USB drive
#   make newdisk  reset build/disk.img to the pristine root file system
#   make toolchain  the x86_64-pc-sieos cross compiler (build/cross)
#   make native   GCC and binutils for SIEOS itself, put on the disk by newdisk
#   make fsck     check the ext4 disk image with e2fsck
#   make clean    remove all build output

# mkfs.ext4, debugfs and e2fsck are in /usr/sbin (/sbin), which a Debian user's
# PATH does not have (Ubuntu's does)
export PATH := $(PATH):/usr/sbin:/sbin

BUILD    := build
ISO      := $(BUILD)/sieos.iso
DISK     := $(BUILD)/disk.img
# sizes: the ISO's root file system (a RAM disk), the hard disk; guest memory
ROOT_MB  := 96
DISK_MB  := 768
MEM      ?= 1G
ROOTIMG  := $(BUILD)/rootfs.img
KERNEL   := $(BUILD)/kernel.elf

CC       := gcc
LD       := ld

COMMON_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -ffreestanding -fno-pic -fno-pie \
                 -fno-stack-protector -fno-builtin -mgeneral-regs-only -mno-red-zone \
                 -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
                 -fno-tree-loop-distribute-patterns

KCFLAGS  := $(COMMON_CFLAGS) -mcmodel=kernel -Ikernel/include -Iabi/include

# the drivers: drv/NAME/*.c -> build/drv/NAME.drv (ELF relocatable, loaded by the kernel: kernel/modload.c),
# and the boot archive (a ustar of drv/, which GRUB loads with the kernel)
DRVS      := $(notdir $(wildcard drv/*))

# kernel/NAME.c left over from before NAME moved to drv/NAME/ is not linked into the kernel
KSRCS    := $(filter-out $(DRVS:%=kernel/%.c),$(wildcard kernel/*.c)) $(wildcard kernel/*.S)
KOBJS    := $(patsubst kernel/%,$(BUILD)/kernel/%.o,$(KSRCS))
DRV_FILES := $(DRVS:%=$(BUILD)/drv/%.drv)
BOOTARCH  := $(BUILD)/boot_archive

# the cross toolchain (x86_64-pc-sieos, built by 'make toolchain') and its sysroot
SYSROOT   := $(abspath $(BUILD))/sysroot
TARGET    := x86_64-pc-sieos
CROSS     := $(abspath $(BUILD))/cross
TC        := $(abspath $(BUILD))/toolchain
SIEOS_CC  := $(CROSS)/bin/$(TARGET)-gcc
SIEOS_CXX := $(CROSS)/bin/$(TARGET)-g++
TC_DONE   := $(CROSS)/.gcc-final
TCDEP     := $(TC_DONE) $(SYSROOT)/usr/lib/libc.so
# the native toolchain (binutils and GCC hosted on SIEOS, see 'native' below)
NATIVE     := $(abspath $(BUILD))/native
NATIVE_DONE := $(TC)/.native-gcc

# ports (third-party programs, see the ports section)
PORTS    := $(abspath $(BUILD))/ports
PORTS_DL := $(PORTS)/dl
DASH     := dash-0.5.12
DASH_BIN := $(PORTS)/$(DASH)/src/dash
KSH      := ksh-1.0.10
KSH_BIN  := $(PORTS)/ksh/ksh93
GNU_PORTS := coreutils-9.5.tar.xz sed-4.9.tar.xz grep-3.11.tar.xz diffutils-3.10.tar.xz \
             findutils-4.10.0.tar.xz gawk-5.3.1.tar.xz make-4.4.1.tar.gz tar-1.35.tar.xz gzip-1.13.tar.xz
GNU_NAMES := coreutils sed grep diffutils findutils gawk make tar gzip
GNU_DONE  := $(GNU_NAMES:%=$(PORTS)/.done-%)
E2FS     := e2fsprogs-1.47.2
E2FS_BINS := $(PORTS)/$(E2FS)/misc/mke2fs $(PORTS)/$(E2FS)/e2fsck/e2fsck
NETLIB_TARS := zlib-1.3.2.tar.xz libpng-1.6.58.tar.xz jpegsrc.v9f.tar.gz expat-2.8.5.tar.xz \
               freetype-2.14.3.tar.xz mbedtls-3.6.7.tar.bz2 curl-8.22.0.tar.xz netsurf-all-3.11.tar.gz
PORT_URLS := http://gondor.apana.org.au/~herbert/dash/files/$(DASH).tar.gz \
             https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.xz \
             https://download.sourceforge.net/libpng/libpng-1.6.58.tar.xz \
             https://ijg.org/files/jpegsrc.v9f.tar.gz \
             https://github.com/libexpat/libexpat/releases/download/R_2_8_5/expat-2.8.5.tar.xz \
             https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz \
             https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2 \
             https://curl.se/download/curl-8.22.0.tar.xz \
             https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz \
             https://www.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.2/$(E2FS).tar.xz \
             $(foreach t,$(GNU_PORTS),https://ftp.gnu.org/gnu/$(firstword $(subst -, ,$(t)))/$(t))

# user programs: the cross compiler and libsieos
UCC      = $(SIEOS_CC)
UAR      = $(CROSS)/bin/$(TARGET)-ar
UCFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Wno-format-truncation -Iuser/include -Iabi/include -Iuser/libfacet/include -Iuser/libsia/include
ULDFLAGS := -s -Wl,-u,__sieos_stdio
LIBSIEOS_SRCS := $(wildcard user/libsieos/*.c)
LIBSIEOS := $(BUILD)/user/libsieos.a
LIBSIEOS_PIC := $(LIBSIEOS)
UPROGS   := $(patsubst user/bin/%.c,%,$(wildcard user/bin/*.c)) facet
UBINS    := $(addprefix $(BUILD)/user/bin/,$(UPROGS))

# libfacet: drawing, widgets and the client side of the window protocol,
# for Facet applications (headers user/libfacet/include/facet/); static and
# shared (libfacet.so.1).  Facet itself and sdm use its drawing code.
LIBFACET_SRCS := $(wildcard user/libfacet/*.c)
LIBFACET_OBJS := $(patsubst user/%.c,$(BUILD)/user/%.o,$(LIBFACET_SRCS)) $(BUILD)/user/libfacet/font8x16.o
LIBFACET      := $(BUILD)/user/libfacet.a
LIBFACET_SO   := $(BUILD)/user/libfacet.so.1
FACET_HDRS    := $(wildcard user/libfacet/include/facet/*.h)
# The Facet desktop (window manager and window server) and its applications,
# separate programs on libfacet (/bin/facet-*).
FACET_SRCS := $(wildcard user/facet/*.c)
FACET_OBJS := $(patsubst user/facet/%.c,$(BUILD)/user/facet/%.o,$(FACET_SRCS))
FAPPS      := $(patsubst user/facet-apps/%.c,%,$(filter-out user/facet-apps/common.c,$(wildcard user/facet-apps/*.c)))
FAPP_BINS  := $(addprefix $(BUILD)/user/bin/facet-,$(FAPPS))
# Static libraries: libtls (TLS 1.3 client) and libsia (the assistant: model
# client, conversation engine, command and desktop tools).  sia (terminal
# harness), sia-agent (headless, for the Facet strip) and Facet link them.
TLS_SRCS    := $(wildcard user/tls/*.c)
LIBTLS      := $(BUILD)/user/libtls.a
LIBSIA_SRCS := $(wildcard user/libsia/*.c)
LIBSIA      := $(BUILD)/user/libsia.a
LIBSIA_SO   := $(BUILD)/user/libsia.so.1
SIA_HDRS    := $(wildcard user/libsia/include/sia/*.h)
SIA_PROGS   := $(BUILD)/user/bin/sia $(BUILD)/user/bin/sia-agent
# pkg, the package manager (user/pkg): libsia's HTTP(S) client, libtls's SHA-256
# and ECDSA, and zlib (the port, static); and the public half of the key that
# signs this build's packages (see Packages)
PKG_BIN     := $(BUILD)/user/bin/pkg
PKG_KEY     ?= $(or $(HOME),/root)/.config/sieos/pkg-signing-key.pem
PKG_PUB     := $(BUILD)/pkg-key.pub

# sdm (graphical login) shares Facet's drawing code and widgets.
SDM_OBJS := $(BUILD)/user/sdm/sdm.o
SDM      := $(BUILD)/user/sbin/sdm

ROOTFS   := $(BUILD)/rootfs
PCI_IDS   ?= $(firstword $(wildcard /usr/share/misc/pci.ids /usr/share/hwdata/pci.ids))

QEMU     := qemu-system-x86_64
SMP      ?= 4
# e1000 NIC on QEMU user networking; host port 8080 is forwarded to the guest's port 80
NET      ?= user,model=e1000,hostfwd=tcp:127.0.0.1:8080-:80
QEMUFLAGS := -smp $(SMP) -m $(MEM) -cdrom $(ISO) -boot d -nic $(NET) \
             -drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
             -no-reboot

.PHONY: all iso disk run run-nox run-uefi run-iso usb run-usb fsck clean abi-check tls-test intel-test hid-test native

all: $(ISO) $(DISK)

iso: $(ISO)
disk: $(DISK)

# ---------------------------------------------------------------- kernel

$(BUILD)/kernel/%.c.o: kernel/%.c $(wildcard kernel/include/*.h) $(wildcard kernel/*.inc) $(wildcard abi/include/sieos/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/kernel/%.S.o: kernel/%.S
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

# Two links: the first with an empty symbol table, then the table of its symbols (tools/mkksyms.py)
# linked last (.ksyms at the end: no other address moves); checked.
KLD = $(LD) -n -nostdlib -z max-page-size=0x1000 --no-warn-rwx-segments -T kernel/linker.ld
$(KERNEL): $(KOBJS) kernel/linker.ld tools/mkksyms.py
	python3 tools/mkksyms.py - $(BUILD)/ksyms0.S && $(CC) -c $(BUILD)/ksyms0.S -o $(BUILD)/ksyms0.o
	$(KLD) -o $(BUILD)/kernel.pass1 $(KOBJS) $(BUILD)/ksyms0.o
	nm $(BUILD)/kernel.pass1 > $(BUILD)/kernel.pass1.nm
	python3 tools/mkksyms.py $(BUILD)/kernel.pass1.nm $(BUILD)/ksyms.S && $(CC) -c $(BUILD)/ksyms.S -o $(BUILD)/ksyms.o
	$(KLD) -o $@ $(KOBJS) $(BUILD)/ksyms.o
	nm $@ | grep -v ' ksyms_\| _kernel_' > $(BUILD)/kernel.nm
	grep -v ' ksyms_\| _kernel_' $(BUILD)/kernel.pass1.nm | cmp -s - $(BUILD)/kernel.nm || \
		{ echo "kernel: symbols moved between the two links"; rm -f $@; exit 1; }

$(BUILD)/drvobj/%.c.o: drv/%.c $(wildcard kernel/include/*.h) $(wildcard drv/*/*.h) $(wildcard abi/include/sieos/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -Idrv/$(firstword $(subst /, ,$*)) -c $< -o $@

define DRV_RULE
$(BUILD)/drv/$(1).drv: $(patsubst drv/%.c,$(BUILD)/drvobj/%.c.o,$(wildcard drv/$(1)/*.c))
	@mkdir -p $$(dir $$@)
	$(LD) -r --strip-debug -o $$@ $$^
endef
$(foreach d,$(DRVS),$(eval $(call DRV_RULE,$(d))))

$(BOOTARCH): $(DRV_FILES)
	tar --format=ustar --owner=0 --group=0 --numeric-owner --mtime=@0 --sort=name -C $(BUILD) -cf $@ $(DRVS:%=drv/%.drv)

.PHONY: drivers
drivers: $(DRV_FILES) $(BOOTARCH)

# ---------------------------------------------------------------- user space
#
# The programs are built by the cross compiler against the C library
# (musl, ABI v2) and linked dynamically (/lib/ld-musl-sieos64.so.1).
# libsieos holds the SIEOS extensions and helpers (user/include/sieos.h).

$(BUILD)/user/libsieos/%.o: user/libsieos/%.c user/include/sieos.h $(wildcard abi/include/sieos/*.h) | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -fPIC -Iabi/include -c $< -o $@

$(LIBSIEOS): $(patsubst user/%.c,$(BUILD)/user/%.o,$(LIBSIEOS_SRCS))
	rm -f $@ && $(UAR) rcs $@ $^

$(BUILD)/user/bin/%.o: user/bin/%.c user/include/sieos.h | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/bin/%: $(BUILD)/user/bin/%.o $(LIBSIEOS)
	$(UCC) $(ULDFLAGS) -o $@ $< $(LIBSIEOS)

$(BUILD)/user/libfacet/%.o: user/libfacet/%.c $(FACET_HDRS) | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -fPIC -c $< -o $@

$(BUILD)/user/libfacet/font8x16.o: kernel/font8x16.c | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -fPIC -c $< -o $@

$(LIBFACET): $(LIBFACET_OBJS)
	rm -f $@ && $(UAR) rcs $@ $^

$(LIBFACET_SO): $(LIBFACET_OBJS)
	$(UCC) -shared -Wl,-soname,libfacet.so.1 -o $@ $^
	ln -sf libfacet.so.1 $(BUILD)/user/libfacet.so

$(BUILD)/user/facet/%.o: user/facet/%.c $(wildcard user/facet/*.h) $(FACET_HDRS) user/include/sieos.h | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -Iuser/facet -Iuser/libsia -c $< -o $@

$(BUILD)/user/bin/facet: $(FACET_OBJS) $(LIBFACET) $(LIBSIA) $(LIBTLS) $(LIBSIEOS)
	@mkdir -p $(dir $@)
	$(UCC) $(ULDFLAGS) -o $@ $(FACET_OBJS) $(LIBFACET) $(LIBSIA) $(LIBTLS) $(LIBSIEOS)

# the applications link libfacet.so.1 (installed in /usr/lib)
$(BUILD)/user/facet-apps/%.o: user/facet-apps/%.c user/facet-apps/common.h $(FACET_HDRS) user/include/sieos.h | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -c $< -o $@

# (Settings also uses libsia: the models and their test)
FAPPS_SIA := settings
$(patsubst %,$(BUILD)/user/facet-apps/%.o,$(FAPPS_SIA)): UCFLAGS += -Iuser/libsia
$(patsubst %,$(BUILD)/user/facet-apps/%.o,$(FAPPS_SIA)): $(wildcard user/libsia/*.h)
$(patsubst %,$(BUILD)/user/bin/facet-%,$(FAPPS_SIA)): $(LIBSIA) $(LIBTLS)

$(BUILD)/user/bin/facet-%: $(BUILD)/user/facet-apps/%.o $(BUILD)/user/facet-apps/common.o $(LIBFACET_SO) $(LIBSIEOS)
	@mkdir -p $(dir $@)
	$(UCC) $(ULDFLAGS) -o $@ $< $(BUILD)/user/facet-apps/common.o -L$(BUILD)/user -lfacet \
		$(if $(filter $*,$(FAPPS_SIA)),$(LIBSIA) $(LIBTLS)) $(LIBSIEOS) -lutil

$(BUILD)/user/tls/%.o: user/tls/%.c $(wildcard user/tls/*.h) user/include/sieos.h | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -fPIC -c $< -o $@

$(LIBTLS): $(patsubst user/%.c,$(BUILD)/user/%.o,$(TLS_SRCS))
	rm -f $@ && $(UAR) rcs $@ $^

$(BUILD)/user/libsia/%.o: user/libsia/%.c $(wildcard user/libsia/*.h) $(SIA_HDRS) $(wildcard user/tls/*.h) user/include/sieos.h | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -fPIC -c $< -o $@

$(LIBSIA): $(patsubst user/%.c,$(BUILD)/user/%.o,$(LIBSIA_SRCS))
	rm -f $@ && $(UAR) rcs $@ $^

# libsia.so.1 carries libtls too; it needs libsieos's few helpers, which are linked in
$(LIBSIA_SO): $(patsubst user/%.c,$(BUILD)/user/%.o,$(LIBSIA_SRCS) $(TLS_SRCS)) $(LIBSIEOS_PIC)
	$(UCC) -shared -Wl,-soname,libsia.so.1 -o $@ $(patsubst user/%.c,$(BUILD)/user/%.o,$(LIBSIA_SRCS) $(TLS_SRCS)) $(LIBSIEOS_PIC)
	ln -sf libsia.so.1 $(BUILD)/user/libsia.so

$(BUILD)/user/sia/%.o: user/sia/%.c $(wildcard user/libsia/*.h) user/include/sieos.h | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -Iuser/libsia -c $< -o $@

$(PKG_BIN): user/pkg/pkg.c $(LIBSIA) $(LIBTLS) $(LIBSIEOS) $(PORTS)/.lib-zlib | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -Iuser/libsia -Iuser/tls -I$(NETLIBS)/usr/include $(ULDFLAGS) -o $@ $< \
		$(LIBSIA) $(LIBTLS) $(LIBSIEOS) $(NETLIBS)/usr/lib/libz.a

$(PKG_PUB): tools/pkgrepo.py
	python3 tools/pkgrepo.py key $(PKG_KEY) $@

$(BUILD)/user/bin/sia $(BUILD)/user/bin/sia-agent: $(BUILD)/user/bin/%: $(BUILD)/user/sia/%.o $(LIBSIA) $(LIBTLS) $(LIBSIEOS)
	@mkdir -p $(dir $@)
	$(UCC) $(ULDFLAGS) -o $@ $< $(LIBSIA) $(LIBTLS) $(LIBSIEOS)

$(BUILD)/user/sdm/%.o: user/sdm/%.c $(wildcard user/facet/*.h) $(FACET_HDRS) user/include/sieos.h | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -Iuser/facet -c $< -o $@

$(SDM): $(SDM_OBJS) $(LIBFACET) $(LIBSIEOS)
	@mkdir -p $(dir $@)
	$(UCC) $(ULDFLAGS) -o $@ $(SDM_OBJS) $(LIBFACET) $(LIBSIEOS)

# The SDK for SIEOS applications, installed in the cross sysroot (and so on
# the native disk): <facet/*.h> and libfacet (Facet windows, drawing,
# widgets), <sia/sia.h> and libsia (the assistant; libsia.so carries libtls),
# <sieos.h> and libsieos, and the ABI headers <sieos/*.h>.
#   x86_64-pc-sieos-gcc app.c -lfacet -lsia       (make sdk-test builds the examples)
SDK_STAMP := $(BUILD)/.sdk
SDK_LIBS  := $(LIBFACET) $(LIBFACET_SO) $(LIBSIA) $(LIBSIA_SO) $(LIBTLS) $(LIBSIEOS)
.PHONY: sdk sdk-test
sdk: $(SDK_STAMP)
$(SDK_STAMP): $(SDK_LIBS) $(FACET_HDRS) $(SIA_HDRS) user/include/sieos.h $(wildcard abi/include/sieos/*.h) | $(TCDEP)
	mkdir -p $(SYSROOT)/usr/include/facet $(SYSROOT)/usr/include/sia
	cp $(FACET_HDRS) $(SYSROOT)/usr/include/facet/
	cp $(SIA_HDRS) $(SYSROOT)/usr/include/sia/
	cp user/include/sieos.h $(SYSROOT)/usr/include/ && cp -r abi/include/sieos $(SYSROOT)/usr/include/
	cp $(LIBFACET) $(LIBSIA) $(LIBTLS) $(LIBSIEOS) $(LIBFACET_SO) $(LIBSIA_SO) $(SYSROOT)/usr/lib/
	ln -sf libfacet.so.1 $(SYSROOT)/usr/lib/libfacet.so && ln -sf libsia.so.1 $(SYSROOT)/usr/lib/libsia.so
	touch $@

# Build the examples the way an application developer would: only the SDK.
sdk-test: $(SDK_STAMP)
	@mkdir -p $(BUILD)/sdk-test
	for d in user/examples/*/; do n=$$(basename $$d); \
		$(SIEOS_CC) -O2 -Wall -Wextra -o $(BUILD)/sdk-test/$$n $$d*.c -lfacet -lsia || exit 1; \
		$(SIEOS_CC) -O2 -static -o $(BUILD)/sdk-test/$$n-static $$d*.c -lfacet -lsia -ltls -lsieos || exit 1; done
	@ls -l $(BUILD)/sdk-test

# ABI v2 self-test: a freestanding static-PIE that uses only the syscall
# instruction (no libc; SSE allowed).
ABI2TEST := $(BUILD)/user/bin/abi2test
$(BUILD)/user/test/abi2test.o: user/test/abi2test.c $(wildcard abi/include/sieos/*.h)
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fPIE -msse2 \
		-Iabi/include -c $< -o $@

$(ABI2TEST): $(BUILD)/user/test/abi2test.o
	@mkdir -p $(dir $@)
	$(LD) -pie --no-dynamic-linker -z text -z max-page-size=0x1000 -e _start -o $@ $<

# ---------------------------------------------------------------- images

# esp.img: the EFI system partition sieinstall writes on the disk it installs
# on: GRUB (the removable-media path EFI/BOOT/BOOTX64.EFI; its prefix names no
# device, so it reads grub.cfg from the partition it was loaded from), the
# kernel, and grub.cfg whose root= sieinstall sets.
ESPIMG := $(BUILD)/esp.img
$(ESPIMG): $(KERNEL) $(BOOTARCH) iso/boot/grub/installed.cfg tools/mkfat.py
	grub-mkimage -O x86_64-efi -d /usr/lib/grub/x86_64-efi -p /boot/grub -o $(BUILD)/BOOTX64-disk.EFI \
		normal configfile search search_fs_file test echo multiboot2 part_gpt part_msdos fat \
		efi_gop all_video video gfxterm
	python3 tools/mkfat.py $@ 16384 EFI/BOOT/BOOTX64.EFI=$(BUILD)/BOOTX64-disk.EFI \
		boot/kernel.elf=$(KERNEL) boot/bootarch.tar=$(BOOTARCH) boot/grub/grub.cfg=iso/boot/grub/installed.cfg

# rootfs.img: pristine ext4 image built from rootfs/ + the user programs.
# It is embedded in the ISO (loaded by GRUB as a RAM disk) and is the
# template for disk.img.
$(ROOTIMG): $(DRV_FILES) $(wildcard kernel/include/*.h) $(UBINS) $(FAPP_BINS) $(LIBSIA_SO) $(SDM) $(SIA_PROGS) $(PKG_BIN) $(PKG_PUB) $(ABI2TEST) $(TCDEP) $(DASH_BIN) $(E2FS_BINS) $(ESPIMG) $(shell find rootfs -type f 2>/dev/null) tools/rootfs.perms tools/mkperms.sh tools/mkshadow.py
	rm -rf $(ROOTFS) && mkdir -p $(ROOTFS)/bin $(ROOTFS)/sbin $(ROOTFS)/tmp $(ROOTFS)/proc $(ROOTFS)/dev/pts $(ROOTFS)/dev/shm $(ROOTFS)/mnt
	cp -r rootfs/. $(ROOTFS)/
	for p in $(UPROGS); do cp $(BUILD)/user/bin/$$p $(ROOTFS)/bin/$$p; done
	cp $(FAPP_BINS) $(ROOTFS)/bin/
	mv $(ROOTFS)/bin/init $(ROOTFS)/sbin/init
	@# the drivers (/drv, also in the boot archive) and what building one needs (/usr/include/ddk)
	mkdir -p $(ROOTFS)/drv $(ROOTFS)/usr/include/ddk
	cp $(DRV_FILES) $(ROOTFS)/drv/
	cp kernel/include/*.h $(ROOTFS)/usr/include/ddk/
	cp -r abi/include/sieos $(ROOTFS)/usr/include/ddk/
	ln -sf ping $(ROOTFS)/bin/ping6
	@# the PCI ID database (device names for lidev), from the build host when it has one
	if [ -f $(PCI_IDS) ]; then mkdir -p $(ROOTFS)/usr/share/misc && cp $(PCI_IDS) $(ROOTFS)/usr/share/misc/pci.ids; fi
	cp $(SDM) $(ROOTFS)/sbin/sdm
	cp $(SIA_PROGS) $(ROOTFS)/bin/
	@# pkg: its program, directories and the key its indexes must be signed with
	cp $(PKG_BIN) $(ROOTFS)/bin/
	mkdir -p $(ROOTFS)/usr/pkg/bin $(ROOTFS)/usr/pkg/lib $(ROOTFS)/var/lib/pkg $(ROOTFS)/var/cache/pkg $(ROOTFS)/etc/pkg/keys
	cp $(PKG_PUB) $(ROOTFS)/etc/pkg/keys/build.pub
	cp $(ABI2TEST) $(ROOTFS)/bin/abi2test
	cp $(DASH_BIN) $(ROOTFS)/bin/sh
	@# Intel's Wi-Fi firmware (AX201: Qu/QuZ with the Hr radio), from the build host's linux-firmware
	mkdir -p $(ROOTFS)/lib/firmware
	for f in iwlwifi-Qu-b0-hr-b0-77 iwlwifi-Qu-c0-hr-b0-77 iwlwifi-QuZ-a0-hr-b0-77; do \
		if [ -f /lib/firmware/$$f.ucode.zst ]; then zstd -dqf /lib/firmware/$$f.ucode.zst -o $(ROOTFS)/lib/firmware/$$f.ucode; \
		elif [ -f /lib/firmware/$$f.ucode ]; then cp /lib/firmware/$$f.ucode $(ROOTFS)/lib/firmware/; fi; done
	cp ports/firmware/LICENCE.iwlwifi_firmware $(ROOTFS)/lib/firmware/
	@# the installer's tools: mke2fs, e2fsck, the EFI system partition
	cp $(E2FS_BINS) $(ROOTFS)/sbin/
	mkdir -p $(ROOTFS)/usr/share/sieos && cp $(ESPIMG) $(ROOTFS)/usr/share/sieos/esp.img
	@# the runtime: dynamic linker and C library, libstdc++ and libgcc_s
	mkdir -p $(ROOTFS)/usr/lib $(ROOTFS)/lib
	cp $(SYSROOT)/usr/lib/libc.so $(ROOTFS)/usr/lib/
	ln -sf /usr/lib/libc.so $(ROOTFS)/lib/ld-musl-sieos64.so.1
	cp -P $(CROSS)/$(TARGET)/lib/libstdc++.so* $(CROSS)/$(TARGET)/lib/libgcc_s.so* $(ROOTFS)/usr/lib/
	cp $(LIBFACET_SO) $(LIBSIA_SO) $(ROOTFS)/usr/lib/
	rm -f $(ROOTFS)/usr/lib/*.py
	for f in $(ROOTFS)/usr/lib/*.so*; do [ -L $$f ] || $(CROSS)/bin/$(TARGET)-strip --strip-unneeded $$f; done
	python3 tools/mkshadow.py root:root user:user daemon:'*' bin:'*' sys:'*' nobody:'*' > $(ROOTFS)/etc/shadow
	find $(ROOTFS) -type d -exec chmod 755 {} +
	find $(ROOTFS) -type f -exec chmod 644 {} +
	chmod 755 $(ROOTFS)/bin/* $(ROOTFS)/sbin/*
	find $(ROOTFS)/usr/lib -name '*.so*' -type f -exec chmod 755 {} +
	@# (no model connection in any image: sia asks for one on first use, or Settings > Assistant)
	cp tools/rootfs.perms $(BUILD)/all.perms
	rm -f $@
	mkfs.ext4 -q -F -b 4096 -L sieos-root -E root_owner=0:0 -d $(ROOTFS) $@ $(ROOT_MB)M
	tools/mkperms.sh $(ROOTFS) $(BUILD)/all.perms > $(BUILD)/perms.debugfs
	debugfs -w -f $(BUILD)/perms.debugfs $@ >/dev/null 2>&1
	@# index the larger directories (htree), as a long-used ext4 file system has them
	e2fsck -fyD $@ >/dev/null 2>&1; [ $$? -le 1 ]

$(ISO): $(KERNEL) $(BOOTARCH) $(ROOTIMG) iso/boot/grub/grub.cfg tools/mkiso.sh tools/mkfat.py
	@mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/kernel.elf
	cp $(BOOTARCH) $(BUILD)/isodir/boot/bootarch.tar
	cp $(ROOTIMG) $(BUILD)/isodir/boot/rootfs.img
	cp iso/boot/grub/grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	tools/mkiso.sh $@ $(BUILD)/isodir $(BUILD)

# diskroot.img: the root file system for the hard disk, DISK_MB large: the
# ISO's root plus, once 'make native' has run, the native toolchain (binutils,
# GCC, the C and C++ headers and libraries) under /usr, with the ABI headers
# (/usr/include/sieos/), sieos.h and libsieos.a for SIEOS programs.
DISKROOT := $(BUILD)/diskroot
DEVROOT  := $(BUILD)/.devroot
DISKIMG  := $(BUILD)/diskroot.img
$(DEVROOT): $(ROOTIMG) $(LIBSIEOS) $(SDK_STAMP) $(GNU_DONE) $(NATIVE_DONE) $(PORTS)/.netsurf $(wildcard abi/include/sieos/*.h) \
             user/include/sieos.h
	rm -rf $(DISKROOT) && cp -a $(ROOTFS) $(DISKROOT)
	mkdir -p $(DISKROOT)/usr/bin && cp -a $(PORTS)/root/usr/gnu $(DISKROOT)/usr/ && rm -rf $(DISKROOT)/usr/gnu/share
	for f in $(DISKROOT)/usr/gnu/bin/* $$(find $(DISKROOT)/usr/gnu/libexec -type f 2>/dev/null); do \
		[ -L $$f ] || $(CROSS)/bin/$(TARGET)-strip $$f 2>/dev/null; done; true
	ln -sf ../gnu/bin/make $(DISKROOT)/usr/bin/gmake && ln -sf gmake $(DISKROOT)/usr/bin/make
	if [ -f $(NATIVE_DONE) ]; then \
		mv $(DISKROOT)/usr/share $(DISKROOT)/.share && \
		cp -a $(NATIVE)/usr/. $(DISKROOT)/usr/ && rm -rf $(DISKROOT)/usr/share && \
		mv $(DISKROOT)/.share $(DISKROOT)/usr/share && \
		cp -a $(SYSROOT)/usr/include $(DISKROOT)/usr/ && \
		cp $(SYSROOT)/usr/lib/*.o $(SYSROOT)/usr/lib/*.a $(DISKROOT)/usr/lib/ && \
		cp -r abi/include/sieos $(DISKROOT)/usr/include/ && cp user/include/sieos.h $(DISKROOT)/usr/include/ && \
		cp $(LIBSIEOS) $(DISKROOT)/usr/lib/ && \
		ln -sf libfacet.so.1 $(DISKROOT)/usr/lib/libfacet.so && ln -sf libsia.so.1 $(DISKROOT)/usr/lib/libsia.so && \
		mkdir -p $(DISKROOT)/usr/src && cp -r user/examples $(DISKROOT)/usr/src/ && \
		ln -sf gcc $(DISKROOT)/usr/bin/cc && \
		for f in $(DISKROOT)/usr/bin/* $$(find $(DISKROOT)/usr/libexec -type f -perm -u+x); do \
			[ -L $$f ] || $(CROSS)/bin/$(TARGET)-strip $$f 2>/dev/null; done; \
		rm -f $(DISKROOT)/usr/lib/*.py; \
		for f in $(DISKROOT)/usr/lib/*.so*; do [ -L $$f ] || $(CROSS)/bin/$(TARGET)-strip --strip-unneeded $$f; done; \
		find $(DISKROOT)/usr -name '*.la' -delete; \
		chmod -R go-w,a+rX $(DISKROOT)/usr; fi
	@# the web browser: NetSurf, as /bin/netsurf (Facet's Web Browser)
	cp -a $(NS_ROOT)/usr/. $(DISKROOT)/usr/
	$(CROSS)/bin/$(TARGET)-strip $(DISKROOT)/usr/bin/netsurf-fb
	ln -sf ../usr/bin/netsurf-fb $(DISKROOT)/bin/netsurf
	chmod -R go-w,a+rX $(DISKROOT)/usr/share/netsurf
	touch $@

# ksh93 (the Solaris shell): built on SIEOS, from the dev root, by tools/nativebuild.py
$(KSH_BIN): $(PORTS_DL)/$(KSH).tar.gz ports/ksh.build tools/nativebuild.py | $(DEVROOT) $(ISO)
	@mkdir -p $(dir $@)
	python3 tools/nativebuild.py --iso $(ISO) --root $(DISKROOT) --src $< --script ports/ksh.build \
		--out /root/build/arch/sieos.i386/bin/ksh=$@

# the hard disk: the dev root with ksh93 as /bin/sh (dash stays as /bin/dash)
$(DISKIMG): $(DEVROOT) $(KSH_BIN)
	cp $(DASH_BIN) $(DISKROOT)/bin/dash && cp $(KSH_BIN) $(DISKROOT)/bin/ksh93
	ln -sf ksh93 $(DISKROOT)/bin/ksh && ln -sf ksh93 $(DISKROOT)/bin/sh
	mkdir -p $(DISKROOT)/usr/bin && ln -sf ../../bin/ksh93 $(DISKROOT)/usr/bin/ksh93 && ln -sf ksh93 $(DISKROOT)/usr/bin/ksh
	rm -f $@
	mkfs.ext4 -q -F -b 4096 -L sieos-root -E root_owner=0:0 -d $(DISKROOT) $@ $(DISK_MB)M
	tools/mkperms.sh $(DISKROOT) $(BUILD)/all.perms > $(BUILD)/diskperms.debugfs
	debugfs -w -f $(BUILD)/diskperms.debugfs $@ >/dev/null 2>&1
	@# index the larger directories (htree), as a long-used ext4 file system has them
	e2fsck -fyD $@ >/dev/null 2>&1; [ $$? -le 1 ]

# disk.img: persistent hard disk.  Only created if missing, so files you
# create inside SIEOS survive rebuilds.  'make newdisk' resets it.
$(DISK): | $(DISKIMG)
	cp $(DISKIMG) $@

.PHONY: newdisk
newdisk: $(DISKIMG)
	cp $(DISKIMG) $(DISK)

# ---------------------------------------------------------------- run

run: all
	$(QEMU) $(QEMUFLAGS) -serial stdio

run-nox: all
	$(QEMU) $(QEMUFLAGS) -display none -serial stdio

OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd

UEFI_FW = -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) -drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd
# The firmware reads the ISO as a USB drive (xHCI): its IDE CD-ROM reads are very
# slow under KVM (minutes for the root file system module).
UEFI_BOOT = -device qemu-xhci -drive if=none,id=boot,format=raw,readonly=on,file=$(ISO) \
            -device usb-storage,drive=boot,bootindex=0
UEFI_QEMUFLAGS := $(filter-out -cdrom $(ISO) -boot d,$(QEMUFLAGS))

# GPU=intel: the host's GPU at GPU_PCI (the Intel integrated GPU) is passed to the
# guest with VFIO instead of QEMU's VGA; the picture appears on the GPU's own
# outputs (the laptop panel, its monitors).  It must first be bound to vfio-pci
# (sudo tools/vfio-gpu.sh bind - read the warning there: the host loses that GPU
# until 'unbind').  QEMU needs KVM here and runs with a raised memory-lock limit
# (through sudo, as the calling user).
# GPU_ROM: the GPU's option ROM (UEFI GOP driver) for the firmware's picture
# before SIEOS's driver; without one only SIEOS's own display output shows.
GPU_PCI ?= 0000:00:02.0
GPU_ROM ?=
comma := ,
ifeq ($(GPU),intel)
GPU_FLAGS := -cpu host -vga none -display none \
             -device vfio-pci,host=$(GPU_PCI),addr=02.0,x-igd-opregion=on$(if $(GPU_ROM),$(comma)romfile=$(abspath $(GPU_ROM)))
GPU_RUN   := sudo tools/vfio-gpu.sh run $(GPU_PCI) --
else ifneq ($(GPU),)
$(error GPU=$(GPU): only GPU=intel is supported)
endif

run-uefi: all
	$(if $(GPU_FLAGS),tools/vfio-gpu.sh check $(GPU_PCI))
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	$(GPU_RUN) $(QEMU) -machine pc -accel kvm -accel tcg $(UEFI_FW) $(UEFI_BOOT) $(UEFI_QEMUFLAGS) $(GPU_FLAGS) -serial stdio

# ---------------------------------------------------------------- USB drive
#
# sieos-usb.img: a hybrid image (the ISO's layout: MBR + GPT, an EFI system
# partition, BIOS boot) to write to a USB drive for a real PC:
#     sudo dd if=build/sieos-usb.img of=/dev/sdX bs=4M conv=fsync status=progress
# The root file system is a RAM disk loaded from the drive (the kernel has no
# USB storage driver), so changes are lost at power-off.  It is the hard disk's
# root, native toolchain included (USB_ROOT_MB large), which the installer
# copies to the disk.
# It has no model connection: sia asks for one on first use (or Settings > Assistant).
USB_ROOT_MB ?= 384
USBIMG  := $(BUILD)/sieos-usb.img
USBROOT := $(BUILD)/usbroot.img
# the USB image's root: the hard disk's (the native toolchain, the DDK, ksh93), so that the
# installer, which copies the running root, puts all of it on the disk too
$(USBROOT): $(DISKIMG) tools/rootfs.perms
	rm -rf $(BUILD)/usbroot && cp -a $(DISKROOT) $(BUILD)/usbroot
	rm -f $@
	mkfs.ext4 -q -F -b 4096 -L sieos-root -E root_owner=0:0 -d $(BUILD)/usbroot $@ $(USB_ROOT_MB)M
	tools/mkperms.sh $(BUILD)/usbroot $(BUILD)/all.perms > $(BUILD)/usbperms.debugfs
	debugfs -w -f $(BUILD)/usbperms.debugfs $@ >/dev/null 2>&1
	e2fsck -fyD $@ >/dev/null 2>&1; [ $$? -le 1 ]

$(USBIMG): $(KERNEL) $(BOOTARCH) $(USBROOT) iso/boot/grub/grub.cfg tools/mkiso.sh tools/mkfat.py
	rm -rf $(BUILD)/usbdir $(BUILD)/usbwork && mkdir -p $(BUILD)/usbdir/boot/grub $(BUILD)/usbwork
	cp $(KERNEL) $(BUILD)/usbdir/boot/kernel.elf
	cp $(BOOTARCH) $(BUILD)/usbdir/boot/bootarch.tar
	cp $(USBROOT) $(BUILD)/usbdir/boot/rootfs.img
	cp iso/boot/grub/grub.cfg $(BUILD)/usbdir/boot/grub/grub.cfg
	tools/mkiso.sh $@ $(BUILD)/usbdir $(BUILD)/usbwork

.PHONY: usb run-usb
usb: $(USBIMG)
	@echo "Write $(USBIMG) to a USB drive (all its data is lost) with"
	@echo "    sudo dd if=$(USBIMG) of=/dev/sdX bs=4M conv=fsync status=progress"
	@echo "where /dev/sdX is the drive itself (see lsblk), not a partition."

run-usb: $(USBIMG)
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	$(QEMU) -machine pc -accel kvm -accel tcg $(UEFI_FW) -smp $(SMP) -m $(MEM) -nic $(NET) -device qemu-xhci \
		-drive if=none,id=usb,format=raw,file=$(USBIMG) -device usb-storage,drive=usb,bootindex=0 \
		-serial stdio -no-reboot

run-iso: $(ISO)
	$(QEMU) -smp $(SMP) -m $(MEM) -cdrom $(ISO) -boot d -nic $(NET) -serial stdio -no-reboot

fsck: $(DISK)
	e2fsck -fn $(DISK)

# Check the ABI v2 headers: hosted C, freestanding C (kernel flags) and C++.
abi-check:
	@mkdir -p $(BUILD)/abi
	$(CC) -std=c11 -Wall -Wextra -Werror -Iabi/include tools/abi-check.c -o $(BUILD)/abi/abi-check
	$(CC) $(KCFLAGS) -Werror -DABI_CHECK_FREESTANDING -Iabi/include -c tools/abi-check.c -o $(BUILD)/abi/abi-check-kernel.o
	g++ -std=c++17 -Wall -Wextra -Werror -x c++ -Iabi/include tools/abi-check.c -o $(BUILD)/abi/abi-check-cxx
	@$(BUILD)/abi/abi-check

# Host-side check of the Intel display driver's register scan (simulated registers).
intel-test:
	@mkdir -p $(BUILD)/test
	gcc -O2 -Wall -Wextra -Itools -o $(BUILD)/test/intel-scan-test tools/intel-scan-test.c
	$(BUILD)/test/intel-scan-test

# Host-side checks of the HID report parser and decoder (USB and I2C keyboards, mice, touchpads)
# and of the HID-over-I2C driver on a simulated DesignWare controller, touchpad and DSDT.
hid-test:
	@mkdir -p $(BUILD)/test
	gcc -O2 -Wall -Wextra -Ikernel/include -o $(BUILD)/test/hid-test tools/hid-test.c
	$(BUILD)/test/hid-test
	gcc -O2 -Wall -Wextra -Itools -Ikernel/include -o $(BUILD)/test/i2c-hid-test tools/i2c-hid-test.c
	$(BUILD)/test/i2c-hid-test

# Host-side check of user/tls against Python's hashlib/hmac/cryptography.
tls-test:
	@mkdir -p $(BUILD)/test
	gcc -O2 -Wall -Wextra -DTLS_HOSTED -o $(BUILD)/test/tlstest tools/tlstest.c $(TLS_SRCS)
	python3 tools/tlstest.py $(BUILD)/test/tlstest

# ---------------------------------------------------------------- libc (ABI v2)
#
# musl 1.2.5 adapted to SIEOS (libc/): 'make libc' installs headers, crt
# files and libc.a into build/sysroot/usr.  Programs are built with the
# cross compiler (make toolchain): build/cross/bin/x86_64-pc-sieos-gcc.
# 'make libc-test' builds libc-test against it and 'make libc-test-img' makes
# a 256 MiB disk (build/libc-test.img) with the tests in /opt/libc-test; on
# SIEOS run:  runall /root/report /opt/libc-test/functional ... (/opt/libc-test/sieos:
# the Solaris interfaces)

MUSL_SRC  := $(BUILD)/musl-src
MUSL_CC    = $(SIEOS_CC)
LIBC_DEPS := libc/musl-1.2.5.tar.gz libc/sieos-port.py $(shell find libc/port libc/backports -type f) $(wildcard abi/include/sieos/*.h)

# stage 0 (once): headers and a static libc built by the host compiler, for
# building the stage-1 compiler; stage 2 replaces them
$(TC)/.libc0:
	python3 libc/sieos-port.py libc/musl-1.2.5.tar.gz $(MUSL_SRC)
	cd $(MUSL_SRC) && CC=gcc ./configure --target=x86_64-pc-sieos --prefix=/usr --disable-shared >/dev/null
	rm -rf $(SYSROOT)
	$(MAKE) -C $(MUSL_SRC) AR=ar RANLIB=ranlib
	$(MAKE) -C $(MUSL_SRC) AR=ar RANLIB=ranlib DESTDIR=$(SYSROOT) install >/dev/null
	mkdir -p $(TC) && touch $@

.PHONY: libc libc-test libc-test-dyn libc-test-img
libc: $(SYSROOT)/usr/lib/libc.so

$(BUILD)/libc-test/runall: libc/tests/runall.c $(TCDEP)
	@mkdir -p $(dir $@)
	$(MUSL_CC) -static -O2 -Wall -o $@ $<

# The static set leaves out the DSO sources (*_dso.c) and the tests that use
# dlopen: musl has no dlopen in static programs.  The dynamic set runs them.
# libc/tests/libc-test-sieos.patch: sigprocmask-internal knows Solaris's signals.
LIBC_TEST_DIRS := functional regression math musl
libc-test: $(TCDEP) $(BUILD)/libc-test/runall libc/tests/libc-test-sieos.patch
	rm -rf $(BUILD)/libc-test/src && tar xzf libc/libc-test-7b95dfa5.tar.gz -C $(BUILD)
	patch -s -p1 -d $(BUILD)/libc-test < libc/tests/libc-test-sieos.patch
	cp libc/tests/config.mak $(BUILD)/libc-test/config.mak
	echo 'CC = $(MUSL_CC)' >> $(BUILD)/libc-test/config.mak
	cd $(BUILD)/libc-test && $(MAKE) -k src/common/runtest.exe \
		$$(ls $(LIBC_TEST_DIRS:%=src/%/*.c) | grep -v '_dso\.c$$' | xargs grep -L dlopen | \
		sed 's/\.c$$/-static.exe/') >/dev/null 2>&1 || true
	@# the header check: every POSIX interface declared (it builds, and runs as a no-op)
	cd $(BUILD)/libc-test && $(MAKE) -k src/api/main.exe >/dev/null 2>&1 || true

# SIEOS tests: Solaris interfaces; named FIFOs and AF_UNIX sockets
SIEOS_LTESTS := $(BUILD)/libc-test/sieos/solaris.exe $(BUILD)/libc-test/sieos/unixsock.exe $(BUILD)/libc-test/sieos/m12.exe $(BUILD)/libc-test/sieos/m18.exe $(BUILD)/libc-test/sieos/vmstress.exe $(BUILD)/libc-test/sieos/ipv6.exe $(BUILD)/libc-test/sieos/symlink.exe $(BUILD)/libc-test/sieos/tcpstress.exe $(BUILD)/libc-test/sieos/frag.exe
$(BUILD)/libc-test/sieos/%.exe: libc/tests/%.c $(TCDEP)
	@mkdir -p $(dir $@)
	$(MUSL_CC) -static -O2 -Wall -Wextra -Iabi/include -o $@ $<

# the same tests linked dynamically (libc.so), with the DSO and dlopen tests
LTD := $(BUILD)/libc-test-dyn
libc-test-dyn: $(TCDEP)
	rm -rf $(LTD) && mkdir -p $(LTD) && tar xzf libc/libc-test-7b95dfa5.tar.gz -C $(LTD) --strip-components=1
	patch -s -p1 -d $(LTD) < libc/tests/libc-test-sieos.patch
	grep -v -e '-static' -e 'BINS_TEMPL' libc/tests/config.mak > $(LTD)/config.mak
	echo 'CC = $(MUSL_CC)' >> $(LTD)/config.mak
	cd $(LTD) && $(MAKE) -k src/common/runtest.exe $$($(MAKE) -s debug 2>/dev/null | \
		sed -n 's/^\(BINS\|LIBS\) //p' | tr ' ' '\n' | grep -E '^src/(functional|regression|musl|math)/' | grep -v -- '-static\.exe$$') \
		>/dev/null 2>&1 || true

libc-test-img: libc-test libc-test-dyn $(SIEOS_LTESTS) toolchain-test $(ROOTIMG)
	rm -rf $(BUILD)/testroot && cp -a $(ROOTFS) $(BUILD)/testroot
	for d in $(LIBC_TEST_DIRS); do mkdir -p $(BUILD)/testroot/opt/libc-test/$$d; \
		cp $(BUILD)/libc-test/src/$$d/*-static.exe $(BUILD)/testroot/opt/libc-test/$$d/ 2>/dev/null || true; done
	mkdir -p $(BUILD)/testroot/opt/libc-test/sieos && cp $(BUILD)/libc-test/sieos/*.exe $(BUILD)/testroot/opt/libc-test/sieos/
	mkdir -p $(BUILD)/testroot/opt/libc-test/api && cp $(BUILD)/libc-test/src/api/main.exe $(BUILD)/testroot/opt/libc-test/api/
	@# the dynamic tests name their DSOs relative to the libc-test root: run them from /opt/libc-test-dyn
	for d in $(LIBC_TEST_DIRS); do mkdir -p $(BUILD)/testroot/opt/libc-test-dyn/src/$$d; \
		cp $(LTD)/src/$$d/*.exe $(LTD)/src/$$d/*.so $(BUILD)/testroot/opt/libc-test-dyn/src/$$d/ 2>/dev/null || true; done
	install -m 755 $(BUILD)/libc-test/runall $(BUILD)/testroot/bin/runall
	mkdir -p $(BUILD)/testroot/opt/toolchain-test/shlib
	cp $(BUILD)/toolchain-test/*.exe $(BUILD)/testroot/opt/toolchain-test/
	cp $(TCS)/*.exe $(TCS)/*.so $(BUILD)/testroot/opt/toolchain-test/shlib/
	rm -f $(BUILD)/libc-test.img
	mkfs.ext4 -q -F -b 4096 -L sieos-test -E root_owner=0:0 -d $(BUILD)/testroot $(BUILD)/libc-test.img 256M
	tools/mkperms.sh $(BUILD)/testroot $(BUILD)/all.perms > $(BUILD)/testperms.debugfs
	debugfs -w -f $(BUILD)/testperms.debugfs $(BUILD)/libc-test.img >/dev/null 2>&1
	@# index the larger directories (htree), as a long-used ext4 file system has them
	e2fsck -fyD $(BUILD)/libc-test.img >/dev/null 2>&1; [ $$? -le 1 ]

# ---------------------------------------------------------------- cross toolchain
#
# binutils 2.45 + GCC 15.2 (C, C++ with libstdc++) for x86_64-pc-sieos,
# in build/cross, with build/sysroot as the target root.  'make toolchain'
# downloads the pinned release tarballs (checked by toolchain/sieos-toolchain.py),
# adds the SIEOS target and builds.  Programs: build/cross/bin/x86_64-pc-sieos-gcc / -g++.

TC_URLS   := https://ftp.gnu.org/gnu/binutils/binutils-2.45.tar.xz \
             https://ftp.gnu.org/gnu/gcc/gcc-15.2.0/gcc-15.2.0.tar.xz \
             https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz \
             https://ftp.gnu.org/gnu/mpfr/mpfr-4.2.2.tar.xz \
             https://ftp.gnu.org/gnu/mpc/mpc-1.3.1.tar.gz
.PHONY: toolchain toolchain-fetch toolchain-test toolchain-test-img
toolchain-fetch:
	@mkdir -p $(TC)/dl
	cd $(TC)/dl && for u in $(TC_URLS); do $(abspath tools/fetch.sh) $$u $$(basename $$u) || exit 1; done

$(TC)/src/.stamp: toolchain/sieos-toolchain.py toolchain/sieos.h | toolchain-fetch
	python3 toolchain/sieos-toolchain.py $(TC)/dl $(TC)/src
	touch $@

$(CROSS)/bin/$(TARGET)-ld: $(TC)/src/.stamp
	rm -rf $(TC)/obj-binutils && mkdir -p $(TC)/obj-binutils
	cd $(TC)/obj-binutils && $(TC)/src/binutils-2.45/configure --target=$(TARGET) --prefix=$(CROSS) \
		--with-sysroot=$(SYSROOT) --disable-nls --disable-werror --disable-gdb --disable-gdbserver \
		--disable-sim --disable-gprofng --disable-libdecnumber --disable-readline MAKEINFO=true >configure.log
	$(MAKE) -C $(TC)/obj-binutils MAKEINFO=true
	$(MAKE) -C $(TC)/obj-binutils MAKEINFO=true install >/dev/null

GCC_CONF := --target=$(TARGET) --prefix=$(CROSS) --with-sysroot=$(SYSROOT) --disable-nls --disable-multilib \
	--enable-threads=posix --enable-tls --disable-libsanitizer --disable-libssp --disable-libgomp \
	--disable-libitm --disable-libvtv --disable-libquadmath --disable-bootstrap \
	CFLAGS_FOR_TARGET=-O2 CXXFLAGS_FOR_TARGET=-O2 MAKEINFO=true

# stage 1: the C compiler and libgcc, to build the C library with
$(TC)/.gcc1: $(CROSS)/bin/$(TARGET)-ld | $(TC)/.libc0
	rm -rf $(TC)/obj-gcc1 && mkdir -p $(TC)/obj-gcc1
	cd $(TC)/obj-gcc1 && PATH=$(CROSS)/bin:$$PATH $(TC)/src/gcc-15.2.0/configure $(GCC_CONF) \
		--enable-languages=c --disable-shared >configure.log
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(TC)/obj-gcc1 MAKEINFO=true all-gcc all-target-libgcc
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(TC)/obj-gcc1 MAKEINFO=true install-gcc install-target-libgcc >/dev/null
	touch $@

# stage 2: the C library, shared and static, built by the cross compiler
MUSL_SRC2 := $(BUILD)/musl-src2
# (a C library change rebuilds only the library, not the compiler)
$(SYSROOT)/usr/lib/libc.so: $(LIBC_DEPS) | $(TC)/.gcc1
	python3 libc/sieos-port.py libc/musl-1.2.5.tar.gz $(MUSL_SRC2)
	@# CFLAGS=-fPIE: libc.a also links into static PIEs (-static-pie)
	cd $(MUSL_SRC2) && PATH=$(CROSS)/bin:$$PATH CC=$(TARGET)-gcc CROSS_COMPILE=$(TARGET)- CFLAGS=-fPIE ./configure \
		--target=$(TARGET) --prefix=/usr --syslibdir=/lib >/dev/null
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(MUSL_SRC2)
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(MUSL_SRC2) DESTDIR=$(SYSROOT) install >/dev/null

# stage 3: the full compiler with shared libgcc_s and libstdc++
$(TC_DONE): $(TC)/.gcc1 | $(SYSROOT)/usr/lib/libc.so
	rm -rf $(TC)/obj-gcc && mkdir -p $(TC)/obj-gcc
	cd $(TC)/obj-gcc && PATH=$(CROSS)/bin:$$PATH $(TC)/src/gcc-15.2.0/configure $(GCC_CONF) \
		--enable-languages=c,c++ --enable-shared >configure.log
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(TC)/obj-gcc MAKEINFO=true
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(TC)/obj-gcc MAKEINFO=true install >/dev/null
	touch $@

toolchain: $(TC_DONE) $(SYSROOT)/usr/lib/libc.so

# ---------------------------------------------------------------- ports
#
# Third-party programs cross-built for SIEOS from pinned release tarballs
# (checked against ports/SHA256SUMS): dash is /bin/sh.

# (a tarball is checked when fetched; its time stays the download's, so editing
# SHA256SUMS does not rebuild every port)
$(PORTS_DL)/$(KSH).tar.gz:
	@mkdir -p $(PORTS_DL)
	cd $(PORTS_DL) && $(abspath tools/fetch.sh) https://github.com/ksh93/ksh/archive/refs/tags/v1.0.10.tar.gz $(KSH).tar.gz
	cd $(PORTS_DL) && grep " $(KSH).tar.gz$$" $(abspath ports/SHA256SUMS) | sha256sum -c --quiet

$(PORTS_DL)/%:
	@mkdir -p $(PORTS_DL)
	cd $(PORTS_DL) && $(abspath tools/fetch.sh) $(filter %/$*,$(PORT_URLS)) $*
	cd $(PORTS_DL) && grep " $*$$" $(abspath ports/SHA256SUMS) | sha256sum -c --quiet

# dash: its build-time signal table is regenerated from the target's <signal.h>
$(DASH_BIN): $(PORTS_DL)/$(DASH).tar.gz ports/signames.py | $(TC_DONE) $(SYSROOT)/usr/lib/libc.so
	rm -rf $(PORTS)/$(DASH) && tar xzf $< -C $(PORTS)
	cd $(PORTS)/$(DASH) && PATH=$(CROSS)/bin:$$PATH ./configure --host=$(TARGET) --prefix=/usr \
		CFLAGS=-O2 >configure.log
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(PORTS)/$(DASH) >$(PORTS)/$(DASH)/build.log
	python3 ports/signames.py $(SIEOS_CC) $(PORTS)/$(DASH)/src/signames.c
	rm -f $(PORTS)/$(DASH)/src/signames.o $@
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(PORTS)/$(DASH) >>$(PORTS)/$(DASH)/build.log
	$(CROSS)/bin/$(TARGET)-strip $@

# e2fsprogs: mke2fs and e2fsck for SIEOS (static), for the installer (sieinstall)
$(E2FS_BINS) &: $(PORTS_DL)/$(E2FS).tar.xz ports/build.py | $(TC_DONE) $(SYSROOT)/usr/lib/libc.so
	rm -rf $(PORTS)/$(E2FS) && tar xf $< -C $(PORTS)
	python3 -c "import sys; sys.path.insert(0, 'ports'); import build; build.teach_config_sub('$(PORTS)/$(E2FS)')"
	cd $(PORTS)/$(E2FS) && PATH=$(CROSS)/bin:$$PATH ./configure --host=$(TARGET) --prefix=/usr --disable-nls \
		--disable-fuse2fs --disable-uuidd --disable-defrag --disable-imager --disable-e2initrd-helper \
		--disable-tdb --disable-bmap-stats --without-libarchive CFLAGS="-O2 -std=gnu17" LDFLAGS=-static \
		>$(PORTS)/$(E2FS).log 2>&1
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(PORTS)/$(E2FS) libs >>$(PORTS)/$(E2FS).log 2>&1
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(PORTS)/$(E2FS)/misc mke2fs >>$(PORTS)/$(E2FS).log 2>&1
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(PORTS)/$(E2FS)/e2fsck e2fsck >>$(PORTS)/$(E2FS).log 2>&1
	$(CROSS)/bin/$(TARGET)-strip $(E2FS_BINS)

# GNU utilities in /usr/gnu (as on Solaris 11), cross-built by ports/build.py
$(PORTS)/.done-%: ports/build.py | $(TC_DONE) $(SYSROOT)/usr/lib/libc.so
	$(MAKE) $(PORTS_DL)/$(filter $*-%,$(GNU_PORTS))
	PATH=$(CROSS)/bin:$$PATH python3 ports/build.py $* $(PORTS_DL) $(PORTS) $(PORTS)/root
	touch $@

.PHONY: ports
ports: $(DASH_BIN) $(GNU_DONE) $(KSH_BIN)

# The libraries under the web browser (NetSurf): static, in the staging root
# $(NETLIBS)/usr (not the SDK sysroot, nor the disk), cross-built by ports/netlibs.py
NETLIBS      := $(PORTS)/netlibs
NETLIB_NAMES := zlib libpng jpeg expat freetype mbedtls curl
NETLIB_DONE  := $(NETLIB_NAMES:%=$(PORTS)/.lib-%)
$(PORTS)/.lib-libpng: $(PORTS)/.lib-zlib
$(PORTS)/.lib-curl: $(PORTS)/.lib-zlib $(PORTS)/.lib-mbedtls
$(PORTS)/.lib-%: ports/netlibs.py ports/build.py | $(TC_DONE) $(SYSROOT)/usr/lib/libc.so
	$(MAKE) $(PORTS_DL)/$(filter $*-% $(if $(filter jpeg,$*),jpegsrc.%),$(NETLIB_TARS))
	PATH=$(CROSS)/bin:$$PATH python3 ports/netlibs.py $* $(PORTS_DL) $(PORTS) $(NETLIBS)
	touch $@

.PHONY: netlibs
netlibs: $(NETLIB_DONE)

# NetSurf's own libraries (HTML and CSS parsing, the DOM, image decoders, libnsfb
# with the Facet surface: ports/netsurf/), from the release bundle, static, in the
# same staging root (their -I$(PREFIX)/include
# must not name the build host's /usr/include: PREFIX is the staging root itself);
# pkg-config sees only the staging root.  (nsgenbind, the JavaScript binding
# generator, is not built: JavaScript is off.)
NS_ALL     := netsurf-all-3.11
NS_SRC     := $(PORTS)/$(NS_ALL)
NSHOST     := $(PORTS)/nshost
NS_LIBS    := buildsystem libwapcaplet libparserutils libcss libhubbub libdom libnsutils \
              libnsbmp libnsgif libutf8proc libnspsl libnslog libsvgtiny libnsfb
NS_MAKE     = CFLAGS=-fPIC PATH=$(CROSS)/bin:$$PATH $(MAKE) HOST=$(TARGET) PREFIX=$(NETLIBS)/usr DESTDIR= Q= \
              PKGCONFIG="PKG_CONFIG_LIBDIR=$(NETLIBS)/usr/lib/pkgconfig pkg-config" \
              WARNFLAGS='-Wall -W -Wno-error' WITH_HUBBUB_BINDING=yes WITH_EXPAT_BINDING=yes
$(PORTS)/.lib-netsurf: $(PORTS_DL)/$(NS_ALL).tar.gz $(NETLIB_DONE) $(wildcard ports/netsurf/*) $(SDK_STAMP)
	rm -rf $(NS_SRC) && tar xzf $< -C $(PORTS)
	ports/netsurf/prepare.sh $(NS_SRC)
	for l in $(NS_LIBS); do \
		echo "netsurf: $$l"; $(NS_MAKE) -C $(NS_SRC)/$$l install >$(PORTS)/ns-$$l.log 2>&1 || \
			{ tail -20 $(PORTS)/ns-$$l.log; exit 1; }; done
	touch $@

.PHONY: netsurf-libs
netsurf-libs: $(PORTS)/.lib-netsurf

# NetSurf's build runs two tools of its own on the build host, which need the
# host's zlib and libpng: built here from the same tarballs, static, in $(NSHOST).
$(PORTS)/.host-png: $(PORTS_DL)/zlib-1.3.2.tar.xz $(PORTS_DL)/libpng-1.6.58.tar.xz
	rm -rf $(PORTS)/host && mkdir -p $(PORTS)/host
	tar xf $(PORTS_DL)/zlib-1.3.2.tar.xz -C $(PORTS)/host
	cd $(PORTS)/host/zlib-1.3.2 && ./configure --static --prefix=$(NSHOST) >../zlib.log && \
		$(MAKE) install >>../zlib.log
	tar xf $(PORTS_DL)/libpng-1.6.58.tar.xz -C $(PORTS)/host
	cd $(PORTS)/host/libpng-1.6.58 && ./configure --prefix=$(NSHOST) --disable-shared \
		CPPFLAGS=-I$(NSHOST)/include LDFLAGS=-L$(NSHOST)/lib >../libpng.log && $(MAKE) install >>../libpng.log
	touch $@

# NetSurf (the framebuffer front end, on libnsfb's Facet surface), installed with
# prefix /usr into $(NS_ROOT): /usr/bin/netsurf-fb and /usr/share/netsurf
NS_ROOT := $(PORTS)/netsurf-root
NS_FB    = $(MAKE) -C $(NS_SRC)/netsurf TARGET=framebuffer CC=$(SIEOS_CC) Q= VQ= \
           PKG_CONFIG="PKG_CONFIG_LIBDIR=$(NETLIBS)/usr/lib/pkgconfig pkg-config" \
           BUILD_CC=cc BUILD_CFLAGS="-O2 -I$(NSHOST)/include" BUILD_LDFLAGS=-L$(NSHOST)/lib \
           BUILD_LIBPNG_CFLAGS=-I$(NSHOST)/include BUILD_LIBPNG_LDFLAGS="-lpng16 -lz -lm" PREFIX=/usr
$(PORTS)/.netsurf: $(PORTS)/.lib-netsurf $(PORTS)/.host-png
	$(NS_FB) >$(PORTS)/netsurf.log 2>&1 || { tail -30 $(PORTS)/netsurf.log; exit 1; }
	rm -rf $(NS_ROOT) && $(NS_FB) install DESTDIR=$(NS_ROOT) >>$(PORTS)/netsurf.log 2>&1
	touch $@

.PHONY: netsurf
netsurf: $(PORTS)/.netsurf

# ---------------------------------------------------------------- packages
#
# Software added to SIEOS as packages (pkg, /usr/pkg): a recipe per package in
# ports/pkgs/NAME (tools/pkgbuild.py says what it holds), built with the cross
# toolchain into build/repo/NAME-VERSION.spkg, after the packages it depends
# on.  make repo writes build/repo/INDEX and signs it (INDEX.sig) with
# $(PKG_KEY), made on first use and kept outside the source tree; the images
# carry its public half (/etc/pkg/keys/build.pub).  The published repository
# is https://www.sieos.org/repo/ (build/repo's files, uploaded); make repo-serve
# serves build/repo on port 8000, for SIEOS in QEMU to test it
# (http://10.0.2.2:8000/ in /etc/pkg/repos).
REPO     := $(BUILD)/repo
PKGWORK  := $(BUILD)/pkgwork
PKG_NAMES := $(notdir $(wildcard ports/pkgs/*))
pkg_deps = $(shell sed -n 's/^depends *= *//p' ports/pkgs/$(1)/recipe)
# (SIEOS's own software, "source = tree:DIR": rebuilt when DIR changes, on the SDK and libsia's headers)
pkg_tree = $(shell sed -n 's/^source *= *tree://p' ports/pkgs/$(1)/recipe)
pkg_tree_deps = $(if $(call pkg_tree,$(1)),$(shell find $(call pkg_tree,$(1)) -type f) $(SDK_STAMP) \
		$(wildcard user/libsia/*.h) $(wildcard user/facet-apps/common.*))
define PKG_RULE
$(PKGWORK)/.built-$(1): ports/pkgs/$(1)/recipe $(wildcard ports/pkgs/$(1)/*.patch) tools/pkgbuild.py \
		$(call pkg_tree_deps,$(1)) \
		$(foreach d,$(call pkg_deps,$(1)),$(PKGWORK)/.built-$(d)) | $(TC_DONE) $(SYSROOT)/usr/lib/libc.so
	python3 tools/pkgbuild.py ports/pkgs/$(1) $(REPO) $(PKGWORK)
	@touch $$@
endef
$(foreach p,$(PKG_NAMES),$(eval $(call PKG_RULE,$(p))))

.PHONY: pkgs repo repo-serve
pkgs: $(PKG_NAMES:%=$(PKGWORK)/.built-%)
repo: pkgs $(PKG_PUB)
	python3 tools/pkgrepo.py index $(REPO) $(PKG_KEY)
repo-serve: repo
	@echo "serving $(REPO) at http://127.0.0.1:8000/ (SIEOS in QEMU: http://10.0.2.2:8000/)"
	python3 -m http.server 8000 --bind 127.0.0.1 --directory $(REPO)

# a self-hosting check: GNU make configured and built on SIEOS, then rebuilt by itself
.PHONY: native-make-test
native-make-test: $(PORTS_DL)/make-4.4.1.tar.gz ports/make.build | $(DEVROOT) $(ISO)
	python3 tools/nativebuild.py --iso $(ISO) --root $(DISKROOT) --src $< --script ports/make.build \
		--out /root/build/make=$(BUILD)/native-test/make

# ---------------------------------------------------------------- native toolchain
#
# binutils and GCC (C, C++) hosted on SIEOS: a Canadian cross (build = Linux,
# host = target = x86_64-pc-sieos) built by the cross toolchain, installed
# under build/native/usr with prefix /usr.  'make native' builds it; the
# disk image (make newdisk) carries it with the C library headers and
# static libraries, so programs can be compiled on SIEOS.
NATIVE_CONF := --build=x86_64-pc-linux-gnu --host=$(TARGET) --target=$(TARGET) --prefix=/usr \
	--disable-nls --disable-werror MAKEINFO=true

$(TC)/.native-src: toolchain/sieos-native.py $(TC)/src/.stamp
	python3 toolchain/sieos-native.py $(TC)/src
	touch $@

$(TC)/.native-binutils: $(TC)/.native-src | $(TC_DONE) $(SYSROOT)/usr/lib/libc.so
	rm -rf $(TC)/obj-nbinutils && mkdir -p $(TC)/obj-nbinutils
	cd $(TC)/obj-nbinutils && PATH=$(CROSS)/bin:$$PATH $(TC)/src/binutils-2.45/configure $(NATIVE_CONF) \
		--disable-gdb --disable-gdbserver --disable-sim --disable-gprofng --disable-libdecnumber \
		--disable-readline --disable-gprof >configure.log
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(TC)/obj-nbinutils MAKEINFO=true
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(TC)/obj-nbinutils MAKEINFO=true DESTDIR=$(NATIVE) install >/dev/null
	touch $@

$(NATIVE_DONE): $(TC)/.native-binutils
	rm -rf $(TC)/obj-ngcc && mkdir -p $(TC)/obj-ngcc
	cd $(TC)/obj-ngcc && PATH=$(CROSS)/bin:$$PATH $(TC)/src/gcc-15.2.0/configure $(NATIVE_CONF) \
		--enable-languages=c,c++ --disable-multilib --enable-threads=posix --enable-tls --enable-shared \
		--disable-lto --disable-libsanitizer --disable-libssp --disable-libgomp --disable-libitm \
		--disable-libvtv --disable-libquadmath --disable-bootstrap \
		CFLAGS=-O2 CXXFLAGS=-O2 CFLAGS_FOR_TARGET=-O2 CXXFLAGS_FOR_TARGET=-O2 >configure.log
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(TC)/obj-ngcc MAKEINFO=true
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(TC)/obj-ngcc MAKEINFO=true DESTDIR=$(NATIVE) install >/dev/null
	touch $@

native: $(NATIVE_DONE)

# C and C++ programs built with the cross compiler, run on SIEOS by runall
TC_TESTS := $(patsubst toolchain/tests/%.cc,%,$(wildcard toolchain/tests/*.cc)) \
            $(patsubst toolchain/tests/%.c,%,$(wildcard toolchain/tests/*.c))
$(BUILD)/toolchain-test/%.exe: toolchain/tests/%.cc toolchain/tests/check.h $(TCDEP)
	@mkdir -p $(dir $@)
	$(SIEOS_CXX) -std=c++23 -O2 -Wall -o $@ $<
$(BUILD)/toolchain-test/%.exe: toolchain/tests/%.c $(TCDEP)
	@mkdir -p $(dir $@)
	$(SIEOS_CC) -O2 -Wall -o $@ $< -lm
$(BUILD)/toolchain-test/%-pie.exe: toolchain/tests/%.c $(TCDEP)
	@mkdir -p $(dir $@)
	$(SIEOS_CC) -O2 -Wall -static-pie -fPIE -o $@ $< -lm
$(BUILD)/toolchain-test/%-static.exe: toolchain/tests/%.cc toolchain/tests/check.h $(TCDEP)
	@mkdir -p $(dir $@)
	$(SIEOS_CXX) -std=c++23 -O2 -Wall -static -o $@ $<
$(BUILD)/toolchain-test/%-static.exe: toolchain/tests/%.c $(TCDEP)
	@mkdir -p $(dir $@)
	$(SIEOS_CC) -O2 -Wall -static -o $@ $< -lm

# shared libraries: a linked library (rpath /opt/toolchain-test/shlib) and a dlopen() plugin
TCS := $(BUILD)/toolchain-test/shlib
$(TCS)/libshtest.so: toolchain/tests/shlib/libshtest.cc $(TCDEP)
	@mkdir -p $(dir $@)
	$(SIEOS_CXX) -O2 -Wall -fPIC -shared -Wl,-soname,libshtest.so -o $@ $<
$(TCS)/plugin.so: toolchain/tests/shlib/plugin.cc $(TCDEP)
	@mkdir -p $(dir $@)
	$(SIEOS_CXX) -O2 -Wall -fPIC -shared -o $@ $<
$(TCS)/shtest.exe: toolchain/tests/shlib/shtest.cc $(TCS)/libshtest.so $(TCS)/plugin.so
	$(SIEOS_CXX) -std=c++23 -O2 -Wall -rdynamic -o $@ $< -L$(TCS) -lshtest -Wl,-rpath,/opt/toolchain-test/shlib

toolchain-test: $(TC_TESTS:%=$(BUILD)/toolchain-test/%.exe) $(TC_TESTS:%=$(BUILD)/toolchain-test/%-static.exe) \
                $(BUILD)/toolchain-test/c_hello-pie.exe $(TCS)/shtest.exe

clean:
	rm -rf $(BUILD)
