# SIEOS - Synthetic Intelligence Enhanced Operating System
#
#   make          build kernel, user programs, bootable ISO and ext4 disk image
#   make run      boot in QEMU with the persistent disk (VGA window + serial)
#   make run-nox  same, serial console only (no window)
#   make run-uefi same as 'make run' but boots through UEFI firmware (OVMF)
#   make run-iso  boot the ISO alone (root fs is a RAM disk from the ISO)
#   make newdisk  reset build/disk.img to the pristine root file system
#   make toolchain  the x86_64-pc-sieos cross compiler (build/cross)
#   make native   GCC and binutils for SIEOS itself, put on the disk by newdisk
#   make fsck     check the ext4 disk image with e2fsck
#   make clean    remove all build output

BUILD    := build
ISO      := $(BUILD)/sieos.iso
DISK     := $(BUILD)/disk.img
# sizes: the ISO's root file system (a RAM disk), the hard disk; guest memory
ROOT_MB  := 64
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

KSRCS    := $(wildcard kernel/*.c) $(wildcard kernel/*.S)
KOBJS    := $(patsubst kernel/%,$(BUILD)/kernel/%.o,$(KSRCS))

# the cross toolchain (x86_64-pc-sieos, built by 'make toolchain') and its sysroot
SYSROOT   := $(abspath $(BUILD))/sysroot
TARGET    := x86_64-pc-sieos
CROSS     := $(abspath $(BUILD))/cross
TC        := $(abspath $(BUILD))/toolchain
SIEOS_CC  := $(CROSS)/bin/$(TARGET)-gcc
SIEOS_CXX := $(CROSS)/bin/$(TARGET)-g++
TC_DONE   := $(CROSS)/.gcc-final
TCDEP     := $(TC_DONE) $(SYSROOT)/usr/lib/libc.so

# ports (third-party programs, see the ports section)
PORTS    := $(abspath $(BUILD))/ports
PORTS_DL := $(PORTS)/dl
DASH     := dash-0.5.12
DASH_BIN := $(PORTS)/$(DASH)/src/dash
GNU_PORTS := coreutils-9.5.tar.xz sed-4.9.tar.xz grep-3.11.tar.xz diffutils-3.10.tar.xz \
             findutils-4.10.0.tar.xz gawk-5.3.1.tar.xz make-4.4.1.tar.gz
GNU_NAMES := coreutils sed grep diffutils findutils gawk make
GNU_DONE  := $(GNU_NAMES:%=$(PORTS)/.done-%)
PORT_URLS := http://gondor.apana.org.au/~herbert/dash/files/$(DASH).tar.gz \
             $(foreach t,$(GNU_PORTS),https://ftp.gnu.org/gnu/$(firstword $(subst -, ,$(t)))/$(t))

# user programs: the cross compiler and libsieos
UCC      = $(SIEOS_CC)
UAR      = $(CROSS)/bin/$(TARGET)-ar
UCFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Wno-format-truncation -Iuser/include
ULDFLAGS := -s -Wl,-u,__sieos_stdio
LIBSIEOS_SRCS := $(wildcard user/libsieos/*.c)
LIBSIEOS := $(BUILD)/user/libsieos.a
UPROGS   := $(patsubst user/bin/%.c,%,$(wildcard user/bin/*.c)) facet
UBINS    := $(addprefix $(BUILD)/user/bin/,$(UPROGS))

# The Facet desktop is built from several files plus the kernel's font.
FACET_SRCS := $(wildcard user/facet/*.c)
FACET_OBJS := $(patsubst user/facet/%.c,$(BUILD)/user/facet/%.o,$(FACET_SRCS)) $(BUILD)/user/facet/font8x16.o
# Static libraries: libtls (TLS 1.3 client) and libsia (the assistant: model
# client, conversation engine, command and desktop tools).  sia (terminal
# harness), sia-agent (headless, for the Facet strip) and Facet link them.
TLS_SRCS    := $(wildcard user/tls/*.c)
LIBTLS      := $(BUILD)/user/libtls.a
LIBSIA_SRCS := $(wildcard user/libsia/*.c)
LIBSIA      := $(BUILD)/user/libsia.a
SIA_PROGS   := $(BUILD)/user/bin/sia $(BUILD)/user/bin/sia-agent

# sdm (graphical login) shares Facet's drawing code and widgets.
SDM_OBJS := $(BUILD)/user/sdm/sdm.o $(addprefix $(BUILD)/user/facet/,gfx.o ui.o font8x16.o)
SDM      := $(BUILD)/user/sbin/sdm

ROOTFS   := $(BUILD)/rootfs
# Model connection pre-registered for sia (endpoint, model, API key); optional, never committed.
AI_CONFIG ?= ai.config

QEMU     := qemu-system-x86_64
SMP      ?= 4
# e1000 NIC on QEMU user networking; host port 8080 is forwarded to the guest's port 80
NET      ?= user,model=e1000,hostfwd=tcp:127.0.0.1:8080-:80
QEMUFLAGS := -smp $(SMP) -m $(MEM) -cdrom $(ISO) -boot d -nic $(NET) \
             -drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
             -no-reboot

.PHONY: all iso disk run run-nox run-uefi run-iso fsck clean abi-check tls-test native

all: $(ISO) $(DISK)

iso: $(ISO)
disk: $(DISK)

# ---------------------------------------------------------------- kernel

$(BUILD)/kernel/%.c.o: kernel/%.c $(wildcard kernel/include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/kernel/%.S.o: kernel/%.S
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(KERNEL): $(KOBJS) kernel/linker.ld
	$(LD) -n -nostdlib -z max-page-size=0x1000 --no-warn-rwx-segments -T kernel/linker.ld -o $@ $(KOBJS)

# ---------------------------------------------------------------- user space
#
# The programs are built by the cross compiler against the C library
# (musl, ABI v2) and linked dynamically (/lib/ld-musl-sieos64.so.1).
# libsieos holds the SIEOS extensions and helpers (user/include/sieos.h).

$(BUILD)/user/libsieos/%.o: user/libsieos/%.c user/include/sieos.h $(wildcard abi/include/sieos/*.h) | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -Iabi/include -c $< -o $@

$(LIBSIEOS): $(patsubst user/%.c,$(BUILD)/user/%.o,$(LIBSIEOS_SRCS))
	rm -f $@ && $(UAR) rcs $@ $^

$(BUILD)/user/bin/%.o: user/bin/%.c user/include/sieos.h | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/bin/%: $(BUILD)/user/bin/%.o $(LIBSIEOS)
	$(UCC) $(ULDFLAGS) -o $@ $< $(LIBSIEOS)

$(BUILD)/user/facet/%.o: user/facet/%.c $(wildcard user/facet/*.h) user/include/sieos.h | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -Iuser/facet -Iuser/libsia -c $< -o $@

$(BUILD)/user/facet/font8x16.o: kernel/font8x16.c | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/bin/facet: $(FACET_OBJS) $(LIBSIA) $(LIBTLS) $(LIBSIEOS)
	@mkdir -p $(dir $@)
	$(UCC) $(ULDFLAGS) -o $@ $(FACET_OBJS) $(LIBSIA) $(LIBTLS) $(LIBSIEOS)

$(BUILD)/user/tls/%.o: user/tls/%.c $(wildcard user/tls/*.h) user/include/sieos.h | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -c $< -o $@

$(LIBTLS): $(patsubst user/%.c,$(BUILD)/user/%.o,$(TLS_SRCS))
	rm -f $@ && $(UAR) rcs $@ $^

$(BUILD)/user/libsia/%.o: user/libsia/%.c $(wildcard user/libsia/*.h) $(wildcard user/tls/*.h) user/include/sieos.h | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -c $< -o $@

$(LIBSIA): $(patsubst user/%.c,$(BUILD)/user/%.o,$(LIBSIA_SRCS))
	rm -f $@ && $(UAR) rcs $@ $^

$(BUILD)/user/sia/%.o: user/sia/%.c $(wildcard user/libsia/*.h) user/include/sieos.h | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -Iuser/libsia -c $< -o $@

$(BUILD)/user/bin/sia $(BUILD)/user/bin/sia-agent: $(BUILD)/user/bin/%: $(BUILD)/user/sia/%.o $(LIBSIA) $(LIBTLS) $(LIBSIEOS)
	@mkdir -p $(dir $@)
	$(UCC) $(ULDFLAGS) -o $@ $< $(LIBSIA) $(LIBTLS) $(LIBSIEOS)

$(BUILD)/user/sdm/%.o: user/sdm/%.c $(wildcard user/facet/*.h) user/include/sieos.h | $(TC_DONE)
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -Iuser/facet -c $< -o $@

$(SDM): $(SDM_OBJS) $(LIBSIEOS)
	@mkdir -p $(dir $@)
	$(UCC) $(ULDFLAGS) -o $@ $(SDM_OBJS) $(LIBSIEOS)

# ABI v2 self-test: a freestanding static-PIE that uses only the syscall
# instruction (no libc; SSE allowed).
ABI2TEST := $(BUILD)/user/bin/abi2test
$(BUILD)/user/test/abi2test.o: user/test/abi2test.c $(wildcard abi/include/sieos/*.h)
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fPIE -msse2 \
		-Iabi/include -c $< -o $@

$(ABI2TEST): $(BUILD)/user/test/abi2test.o
	$(LD) -pie --no-dynamic-linker -z text -z max-page-size=0x1000 -e _start -o $@ $<

# ---------------------------------------------------------------- images

# rootfs.img: pristine ext4 image built from rootfs/ + the user programs.
# It is embedded in the ISO (loaded by GRUB as a RAM disk) and is the
# template for disk.img.
$(ROOTIMG): $(UBINS) $(SDM) $(SIA_PROGS) $(ABI2TEST) $(TCDEP) $(DASH_BIN) $(shell find rootfs -type f 2>/dev/null) tools/rootfs.perms tools/mkperms.sh tools/mkshadow.py \
             tools/mksiaconfig.py $(wildcard $(AI_CONFIG))
	rm -rf $(ROOTFS) && mkdir -p $(ROOTFS)/bin $(ROOTFS)/sbin $(ROOTFS)/tmp $(ROOTFS)/proc $(ROOTFS)/dev/pts $(ROOTFS)/dev/shm $(ROOTFS)/mnt
	cp -r rootfs/. $(ROOTFS)/
	for p in $(UPROGS); do cp $(BUILD)/user/bin/$$p $(ROOTFS)/bin/$$p; done
	mv $(ROOTFS)/bin/init $(ROOTFS)/sbin/init
	cp $(SDM) $(ROOTFS)/sbin/sdm
	cp $(SIA_PROGS) $(ROOTFS)/bin/
	cp $(ABI2TEST) $(ROOTFS)/bin/abi2test
	cp $(DASH_BIN) $(ROOTFS)/bin/sh
	@# the runtime: dynamic linker and C library, libstdc++ and libgcc_s
	mkdir -p $(ROOTFS)/usr/lib $(ROOTFS)/lib
	cp $(SYSROOT)/usr/lib/libc.so $(ROOTFS)/usr/lib/
	ln -sf /usr/lib/libc.so $(ROOTFS)/lib/ld-musl-sieos64.so.1
	cp -P $(CROSS)/$(TARGET)/lib/libstdc++.so* $(CROSS)/$(TARGET)/lib/libgcc_s.so* $(ROOTFS)/usr/lib/
	rm -f $(ROOTFS)/usr/lib/*.py
	for f in $(ROOTFS)/usr/lib/*.so*; do [ -L $$f ] || $(CROSS)/bin/$(TARGET)-strip --strip-unneeded $$f; done
	python3 tools/mkshadow.py root:root user:user daemon:'*' bin:'*' sys:'*' nobody:'*' > $(ROOTFS)/etc/shadow
	find $(ROOTFS) -type d -exec chmod 755 {} +
	find $(ROOTFS) -type f -exec chmod 644 {} +
	chmod 755 $(ROOTFS)/bin/* $(ROOTFS)/sbin/*
	find $(ROOTFS)/usr/lib -name '*.so*' -type f -exec chmod 755 {} +
	python3 tools/mksiaconfig.py $(AI_CONFIG) $(ROOTFS) $(BUILD)/sia.perms
	cat tools/rootfs.perms $(BUILD)/sia.perms > $(BUILD)/all.perms
	rm -f $@
	mkfs.ext4 -q -F -b 4096 -O ^dir_index -L sieos-root -E root_owner=0:0 -d $(ROOTFS) $@ $(ROOT_MB)M
	tools/mkperms.sh $(ROOTFS) $(BUILD)/all.perms > $(BUILD)/perms.debugfs
	debugfs -w -f $(BUILD)/perms.debugfs $@ >/dev/null 2>&1

$(ISO): $(KERNEL) $(ROOTIMG) iso/boot/grub/grub.cfg tools/mkiso.sh tools/mkfat.py
	@mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/kernel.elf
	cp $(ROOTIMG) $(BUILD)/isodir/boot/rootfs.img
	cp iso/boot/grub/grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	tools/mkiso.sh $@ $(BUILD)/isodir $(BUILD)

# diskroot.img: the root file system for the hard disk, DISK_MB large: the
# ISO's root plus, once 'make native' has run, the native toolchain (binutils,
# GCC, the C and C++ headers and libraries) under /usr, with the ABI headers
# (/usr/include/sieos/), sieos.h and libsieos.a for SIEOS programs.
DISKROOT := $(BUILD)/diskroot
DISKIMG  := $(BUILD)/diskroot.img
$(DISKIMG): $(ROOTIMG) $(LIBSIEOS) $(GNU_DONE) $(wildcard $(NATIVE_DONE)) $(wildcard abi/include/sieos/*.h) user/include/sieos.h
	rm -rf $(DISKROOT) && cp -a $(ROOTFS) $(DISKROOT)
	mkdir -p $(DISKROOT)/usr/bin && cp -a $(PORTS)/root/usr/gnu $(DISKROOT)/usr/ && rm -rf $(DISKROOT)/usr/gnu/share
	for f in $(DISKROOT)/usr/gnu/bin/* $$(find $(DISKROOT)/usr/gnu/libexec -type f 2>/dev/null); do \
		[ -L $$f ] || $(CROSS)/bin/$(TARGET)-strip $$f 2>/dev/null; done; true
	ln -sf ../gnu/bin/make $(DISKROOT)/usr/bin/gmake && ln -sf gmake $(DISKROOT)/usr/bin/make
	if [ -f $(NATIVE_DONE) ]; then \
		cp -a $(NATIVE)/usr/. $(DISKROOT)/usr/ && rm -rf $(DISKROOT)/usr/share && \
		cp -a $(SYSROOT)/usr/include $(DISKROOT)/usr/ && \
		cp $(SYSROOT)/usr/lib/*.o $(SYSROOT)/usr/lib/*.a $(DISKROOT)/usr/lib/ && \
		cp -r abi/include/sieos $(DISKROOT)/usr/include/ && cp user/include/sieos.h $(DISKROOT)/usr/include/ && \
		cp $(LIBSIEOS) $(DISKROOT)/usr/lib/ && \
		ln -sf gcc $(DISKROOT)/usr/bin/cc && \
		for f in $(DISKROOT)/usr/bin/* $$(find $(DISKROOT)/usr/libexec -type f -perm -u+x); do \
			[ -L $$f ] || $(CROSS)/bin/$(TARGET)-strip $$f 2>/dev/null; done; \
		for f in $(DISKROOT)/usr/lib/*.so*; do [ -L $$f ] || $(CROSS)/bin/$(TARGET)-strip --strip-unneeded $$f; done; \
		find $(DISKROOT)/usr -name '*.la' -delete; rm -f $(DISKROOT)/usr/lib/*.py; \
		chmod -R go-w,a+rX $(DISKROOT)/usr; fi
	rm -f $@
	mkfs.ext4 -q -F -b 4096 -O ^dir_index -L sieos-root -E root_owner=0:0 -d $(DISKROOT) $@ $(DISK_MB)M
	tools/mkperms.sh $(DISKROOT) $(BUILD)/all.perms > $(BUILD)/diskperms.debugfs
	debugfs -w -f $(BUILD)/diskperms.debugfs $@ >/dev/null 2>&1

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

run-uefi: all
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	$(QEMU) -machine pc -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd $(QEMUFLAGS) -serial stdio

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
LIBC_DEPS := libc/musl-1.2.5.tar.gz libc/sieos-port.py $(shell find libc/port -type f) $(wildcard abi/include/sieos/*.h)

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

LIBC_TEST_DIRS := functional regression math musl
libc-test: $(TCDEP) $(BUILD)/libc-test/runall
	rm -rf $(BUILD)/libc-test/src && tar xzf libc/libc-test-7b95dfa5.tar.gz -C $(BUILD)
	cp libc/tests/config.mak $(BUILD)/libc-test/config.mak
	echo 'CC = $(MUSL_CC)' >> $(BUILD)/libc-test/config.mak
	cd $(BUILD)/libc-test && $(MAKE) -k src/common/runtest.exe \
		$$(ls $(LIBC_TEST_DIRS:%=src/%/*.c) | sed 's/\.c$$/-static.exe/') >/dev/null 2>&1 || true

# SIEOS tests: Solaris interfaces; named FIFOs and AF_UNIX sockets
SIEOS_LTESTS := $(BUILD)/libc-test/sieos/solaris.exe $(BUILD)/libc-test/sieos/unixsock.exe $(BUILD)/libc-test/sieos/m12.exe
$(BUILD)/libc-test/sieos/%.exe: libc/tests/%.c $(TCDEP)
	@mkdir -p $(dir $@)
	$(MUSL_CC) -static -O2 -Wall -Wextra -o $@ $<

# the same tests linked dynamically (libc.so), with the DSO and dlopen tests
LTD := $(BUILD)/libc-test-dyn
libc-test-dyn: $(TCDEP)
	rm -rf $(LTD) && mkdir -p $(LTD) && tar xzf libc/libc-test-7b95dfa5.tar.gz -C $(LTD) --strip-components=1
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
	@# the dynamic tests name their DSOs relative to the libc-test root: run them from /opt/libc-test-dyn
	for d in $(LIBC_TEST_DIRS); do mkdir -p $(BUILD)/testroot/opt/libc-test-dyn/src/$$d; \
		cp $(LTD)/src/$$d/*.exe $(LTD)/src/$$d/*.so $(BUILD)/testroot/opt/libc-test-dyn/src/$$d/ 2>/dev/null || true; done
	install -m 755 $(BUILD)/libc-test/runall $(BUILD)/testroot/bin/runall
	mkdir -p $(BUILD)/testroot/opt/toolchain-test/shlib
	cp $(BUILD)/toolchain-test/*.exe $(BUILD)/testroot/opt/toolchain-test/
	cp $(TCS)/*.exe $(TCS)/*.so $(BUILD)/testroot/opt/toolchain-test/shlib/
	rm -f $(BUILD)/libc-test.img
	mkfs.ext4 -q -F -b 4096 -O ^dir_index -L sieos-test -E root_owner=0:0 -d $(BUILD)/testroot $(BUILD)/libc-test.img 256M
	tools/mkperms.sh $(BUILD)/testroot $(BUILD)/all.perms > $(BUILD)/testperms.debugfs
	debugfs -w -f $(BUILD)/testperms.debugfs $(BUILD)/libc-test.img >/dev/null 2>&1

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
	cd $(TC)/dl && for u in $(TC_URLS); do [ -f $$(basename $$u) ] || curl -sSfLO $$u; done

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

$(PORTS_DL)/%: ports/SHA256SUMS
	@mkdir -p $(PORTS_DL)
	cd $(PORTS_DL) && [ -f $* ] || curl -sSfLO $(filter %/$*,$(PORT_URLS))
	cd $(PORTS_DL) && grep " $*$$" $(abspath ports/SHA256SUMS) | sha256sum -c --quiet
	touch $@

# dash: its build-time signal table is regenerated from the target's <signal.h>
$(DASH_BIN): $(PORTS_DL)/$(DASH).tar.gz ports/signames.py | $(TC_DONE) $(SYSROOT)/usr/lib/libc.so
	rm -rf $(PORTS)/$(DASH) && tar xzf $< -C $(PORTS)
	cd $(PORTS)/$(DASH) && PATH=$(CROSS)/bin:$$PATH ./configure --host=$(TARGET) --prefix=/usr \
		CFLAGS=-O2 >configure.log
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(PORTS)/$(DASH) >build.log
	python3 ports/signames.py $(SIEOS_CC) $(PORTS)/$(DASH)/src/signames.c
	rm -f $(PORTS)/$(DASH)/src/signames.o $@
	PATH=$(CROSS)/bin:$$PATH $(MAKE) -C $(PORTS)/$(DASH) >>build.log
	$(CROSS)/bin/$(TARGET)-strip $@

# GNU utilities in /usr/gnu (as on Solaris 11), cross-built by ports/build.py
$(PORTS)/.done-%: ports/build.py | $(TC_DONE) $(SYSROOT)/usr/lib/libc.so
	$(MAKE) $(PORTS_DL)/$(filter $*-%,$(GNU_PORTS))
	PATH=$(CROSS)/bin:$$PATH python3 ports/build.py $* $(PORTS_DL) $(PORTS) $(PORTS)/root
	touch $@

.PHONY: ports
ports: $(DASH_BIN) $(GNU_DONE)

# ---------------------------------------------------------------- native toolchain
#
# binutils and GCC (C, C++) hosted on SIEOS: a Canadian cross (build = Linux,
# host = target = x86_64-pc-sieos) built by the cross toolchain, installed
# under build/native/usr with prefix /usr.  'make native' builds it; the
# disk image (make newdisk) carries it with the C library headers and
# static libraries, so programs can be compiled on SIEOS.
NATIVE     := $(abspath $(BUILD))/native
NATIVE_DONE := $(TC)/.native-gcc
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
