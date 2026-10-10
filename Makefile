# SIEOS - build and run.
#   make            build the kernel (build/kernel.bin) and the programs (build/*.elf)
#   make run        run in QEMU: a window (VGA screen + keyboard) and this terminal (serial)
#                   (SMP=8 MEM=2G: the number of CPUs and the memory; default 4 and 256M)
#   make run-nox    run in this terminal only
#   make test       boot, type a few commands, check the answers (and that files persist),
#                   then kill servers while they work and check that everything carries on
#   make crash-test only that second part
#   make newdisk    recreate build/disk.img from rootfs/ (DISK=64M: its size); your files on it are lost
#   make STACKCHECK=1  a kernel that measures its stacks' deepest use (shown at power off)
#   make ARCH=arm64 DIAG_TEST=n   a kernel that faults on purpose after boot step n (tests the Pi's boot-step screen)
#   make clean
#
# Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only

# the build directory: build/ (x86-64), build-arm64/ (another one: B=...)
B       := build$(if $(filter-out x86_64,$(ARCH)),-$(ARCH))
# our C compiler (defined early: rules below list it as a prerequisite)
SICC      := $(B)/host/sicc
.SECONDEXPANSION:                 # lets user/$$*/$$*.c name a program's source
CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic \
           -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
           -mgeneral-regs-only -Iinclude
# Kernel: the portable core (kernel/) and one processor's part
# (kernel/arch/$(ARCH)/, described by its arch.mk: objects, flags, linker script).
ARCH    ?= x86_64
include kernel/arch/$(ARCH)/arch.mk
KCFLAGS := $(CFLAGS) $(KARCH_CFLAGS) -Ikernel -Ikernel/arch/$(ARCH) $(if $(STACKCHECK),-DSTACKCHECK) $(if $(DIAG_TEST),-DDIAG_TEST=$(DIAG_TEST))
KHDRS   := kernel/*.h kernel/arch/$(ARCH)/*.h include/mk/*.h
UCFLAGS := $(CFLAGS) -Iuser/lib -Isiefs

KCORE   := main mem task ipc syscall random kprintf   # kernel/: portable
KOBJS   := $(KARCH_OBJS) $(KCORE) fmt string blake2b chacha20
UCORE   := crt0 lib                     # in every program
ULIBOBJ := fs acct tty fmt string fdt virtio mbox blake2b argon2 chacha20 sha2 aead x25519 pubkey net http tls x509   # the library: a program gets only what it uses
ULIB    := $(B)/user/libsieos.a
PROGS   := con usb vblk fs init         # boot modules: loaded by QEMU with the kernel
BINS    := sh auth login acct svc atlas hello bench fsloop crash siastub ktest vnet netd fetch sia siad racey   # programs on the disk, in /bin
LINKS   := passwd su useradd userdel    # other names of /bin/acct (symbolic links)
SIEFS_CORE := crc tree space fs         # the SieFS library, linked into the file server

BINS    := $(filter-out $(KARCH_NOBINS),$(BINS))

all: $(B)/kernel.bin $(PROGS:%=$(B)/%.elf) $(KARCH_EXTRA) $(B)/disk.img

# ---- the kernel: an ELF file, then flattened into the binary QEMU loads
$(B)/kernel.elf: $(KOBJS:%=$(B)/kernel/%.o) $(KARCH_LDS) | $(TOOLDEP)
	$(TLD) -T $(KARCH_LDS) -z max-page-size=4096 --no-warn-rwx-segments -o $@ $(KOBJS:%=$(B)/kernel/%.o)
$(B)/kernel.bin: $(B)/kernel.elf | $(TOOLDEP)
	$(TOBJCOPY) -O binary $< $@
$(B)/kernel/%.o: kernel/%.c $(KHDRS) $(B)/kernel/flags $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(KCFLAGS) -c $< -o $@
$(B)/kernel/%.o: kernel/arch/$(ARCH)/%.c $(KHDRS) $(B)/kernel/flags $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(KCFLAGS) -c $< -o $@
$(B)/kernel/%.o: kernel/arch/$(ARCH)/%.S $(B)/kernel/flags $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(KCFLAGS) -c $< -o $@
# the kernel's flags: a change (e.g. STACKCHECK=1) rebuilds it
$(B)/kernel/flags: FORCE
	@mkdir -p $(@D)
	@echo '$(KCFLAGS)' | cmp -s - $@ || echo '$(KCFLAGS)' > $@
$(B)/kernel/%.o: lib/%.c include/mk/*.h $(B)/kernel/flags $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(KCFLAGS) -c $< -o $@

# ---- user programs: the program's files + the library
$(B)/%.elf: $(B)/user/%.o $(UCORE:%=$(B)/user/lib/%.o) $(ULIB) user/user.ld | $(TOOLDEP)
	$(TLD) -T user/user.ld -z max-page-size=4096 -o $@ $(UCORE:%=$(B)/user/lib/%.o) $< $(ULIB)
$(ULIB): $(ULIBOBJ:%=$(B)/user/lib/%.o) | $(TOOLDEP)
	rm -f $@ && $(TAR) rcs $@ $^
$(B)/user/%.o: user/$$*/$$*.c user/lib/mk.h include/mk/*.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) -c $< -o $@
$(B)/user/lib/%.o: user/lib/%.c user/lib/mk.h include/mk/*.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) -c $< -o $@
$(B)/user/lib/%.o: user/lib/%.S $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) -c $< -o $@
$(B)/user/lib/%.o: lib/%.c include/mk/*.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) -c $< -o $@

# The file server also links the SieFS library, and knows the build date
# (no clock chip driver yet: file times are this date + the time since boot).
$(B)/fs.elf: $(B)/user/fs.o $(SIEFS_CORE:%=$(B)/user/siefs/%.o) $(UCORE:%=$(B)/user/lib/%.o) $(ULIB) user/user.ld | $(TOOLDEP)
	$(TLD) -T user/user.ld -z max-page-size=4096 -o $@ $(UCORE:%=$(B)/user/lib/%.o) $< $(SIEFS_CORE:%=$(B)/user/siefs/%.o) $(ULIB)
$(B)/user/fs.o: UCFLAGS += -DEPOCH=$(shell date +%s)
$(B)/user/siefs/%.o: siefs/%.c siefs/*.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) -c $< -o $@

# The disk server: virtio, and on arm64 also SD cards (the Raspberry Pis: sd.c).
VBLK_X  := $(if $(filter arm64,$(ARCH)),$(B)/user/vblk/sd.o $(B)/user/usb/pci.o)
$(B)/vblk.elf: $(B)/user/vblk.o $(VBLK_X) $(UCORE:%=$(B)/user/lib/%.o) $(ULIB) user/user.ld | $(TOOLDEP)
	$(TLD) -T user/user.ld -z max-page-size=4096 -o $@ $(UCORE:%=$(B)/user/lib/%.o) $< $(VBLK_X) $(ULIB)
$(B)/user/vblk/%.o: user/vblk/%.c user/vblk/*.h user/lib/mk.h include/mk/*.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) -c $< -o $@

# The network card server: virtio, and on arm64 also the Raspberry Pis' own
# Ethernet (genet.c: Pi 4; gem.c: Pi 5's RP1).
VNET_X  := $(if $(filter arm64,$(ARCH)),$(B)/user/vnet/genet.o $(B)/user/vnet/gem.o)
$(B)/vnet.elf: $(B)/user/vnet.o $(VNET_X) $(UCORE:%=$(B)/user/lib/%.o) $(ULIB) user/user.ld | $(TOOLDEP)
	$(TLD) -T user/user.ld -z max-page-size=4096 -o $@ $(UCORE:%=$(B)/user/lib/%.o) $< $(VNET_X) $(ULIB)
$(B)/user/vnet.o: user/vnet/nic.h
$(B)/user/vnet/%.o: user/vnet/%.c user/vnet/nic.h user/lib/mk.h include/mk/*.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) -c $< -o $@

# The USB server (user/usb): the xHCI controller, hubs, disks, keyboards and mice.
USBSRV  := usb xhci pci msc hid
$(B)/usb.elf: $(USBSRV:%=$(B)/user/usb/%.o) $(UCORE:%=$(B)/user/lib/%.o) $(ULIB) user/user.ld | $(TOOLDEP)
	$(TLD) -T user/user.ld -z max-page-size=4096 -o $@ $(UCORE:%=$(B)/user/lib/%.o) $(USBSRV:%=$(B)/user/usb/%.o) $(ULIB)
$(B)/user/usb/%.o: user/usb/%.c user/usb/usb.h user/lib/mk.h include/mk/*.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) -c $< -o $@

# The desktop has several files; its font is drawn in tools/mkfont.py.
ATLAS   := atlas panel edit setup draw font screen
$(B)/atlas.elf: $(ATLAS:%=$(B)/user/atlas/%.o) $(UCORE:%=$(B)/user/lib/%.o) $(ULIB) user/user.ld | $(TOOLDEP)
	$(TLD) -T user/user.ld -z max-page-size=4096 -o $@ $(UCORE:%=$(B)/user/lib/%.o) $(ATLAS:%=$(B)/user/atlas/%.o) $(ULIB)
$(B)/user/atlas/%.o: user/atlas/%.c user/atlas/draw.h user/atlas/atlas.h user/lib/mk.h include/mk/*.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) -c $< -o $@
user/atlas/font.c: tools/mkfont.py
	python3 tools/mkfont.py $@

# ---- sia, the assistant (user/sia, docs/ai-plan.md): siad, the server,
# is siad.c + its backends + our language-model engine (llm/); sia, the
# command, is an ordinary program. The engine computes in floating point
# and AVX2 (chosen at run time from what the CPU has), so these files are
# built without -mgeneral-regs-only. remote.c, the backend for a remote
# model (it needs the network), is linked when present, a stub otherwise.
SIA_REMOTE := $(if $(wildcard user/sia/remote.c),remote,remote_stub)
SIAD    := siad memory local $(SIA_REMOTE)
LLM_SIEOS := gguf quant model tok chat math $(if $(filter arm64,$(ARCH)),neon neon_dot)
# (AArch64: the NEON kernels, twice: plain, and with sdot for the CPUs that have it)
$(B)/user/llm/neon_dot.o: SIA_CF += -march=armv8.2-a+dotprod
SIA_CF  := $(filter-out -mgeneral-regs-only,$(UCFLAGS)) -fno-math-errno
$(B)/siad.elf: $(SIAD:%=$(B)/user/sia/%.o) $(LLM_SIEOS:%=$(B)/user/llm/%.o) $(UCORE:%=$(B)/user/lib/%.o) $(ULIB) user/user.ld | $(TOOLDEP)
	$(TLD) -T user/user.ld -z max-page-size=4096 -o $@ $(UCORE:%=$(B)/user/lib/%.o) $(SIAD:%=$(B)/user/sia/%.o) $(LLM_SIEOS:%=$(B)/user/llm/%.o) $(ULIB)
$(B)/user/sia/%.o: user/sia/%.c user/sia/backend.h llm/llm.h user/lib/mk.h include/mk/*.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(SIA_CF) -c $< -o $@
$(B)/user/llm/%.o: llm/%.c llm/llm.h llm/internal.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(SIA_CF) -c $< -o $@

# Programs that compute in floating point or vector registers: built
# without -mgeneral-regs-only (the kernel saves those registers per
# thread, kernel/arch/x86_64/fpu.c; the servers keep the flag and never pay for it).
FPU_PROGS := ktest
$(FPU_PROGS:%=$(B)/user/%.o): UCFLAGS := $(filter-out -mgeneral-regs-only,$(UCFLAGS))

# ---- the disk: a SieFS image of rootfs/ plus the programs in /bin.
# build/rootfs is the staging copy. The image is made once (mkfs.siefs);
# after that, when rootfs/ or a program changes, only those files are
# copied into it (host tool "siefs put"), so what you wrote on the disk
# while SIEOS ran stays. "make newdisk" starts again from scratch.
# DISK: the size of a new disk (sparse: only what is written takes space);
# TESTDISK: the tests' fresh disks.
DISK    ?= 3G
TESTDISK ?= 64M
ROOTFS  := $(shell find rootfs -type f)
$(B)/rootfs.stamp: $(ROOTFS) $(BINS:%=$(B)/%.elf) $(if $(filter atlas,$(KARCH_NOBINS)),$(B)/noscreen.elf)
	rm -rf $(B)/rootfs && mkdir -p $(B)/rootfs && cp -R rootfs/. $(B)/rootfs/
	mkdir -p $(B)/rootfs/bin $(B)/rootfs/home $(B)/rootfs/tmp
	for p in $(BINS); do cp $(B)/$$p.elf $(B)/rootfs/bin/$$p; done
	$(if $(filter atlas,$(KARCH_NOBINS)),cp $(B)/noscreen.elf $(B)/rootfs/bin/atlas)
	for l in $(LINKS); do ln -s acct $(B)/rootfs/bin/$$l; done
	chmod -R u=rwX,go=rX $(B)/rootfs && chmod 755 $(B)/rootfs/bin/* && chmod 1777 $(B)/rootfs/tmp
	touch $@
$(B)/disk.img: $(B)/rootfs.stamp $(B)/host/mkfs.siefs $(B)/host/siefs
	@if [ ! -f $@ ]; then \
	    echo "mkfs.siefs: new disk $@ ($(DISK))"; \
	    $(B)/host/mkfs.siefs -s $(DISK) -L SIEOS -d $(B)/rootfs $@; \
	else \
	    echo "updating $@ with the changed files of $(B)/rootfs"; \
	    cd $(B)/rootfs && for d in $$(find . -mindepth 1 -type d); do ../host/siefs ../disk.img mkdir /$${d#./} 2>/dev/null; done; \
	    for f in $$(find . -type f -newer ../disk.img); do \
	        ../host/siefs ../disk.img put $$f /$${f#./} && \
	        case $$f in ./bin/*) ../host/siefs ../disk.img chmod 755 /$${f#./};; esac; done; \
	    for l in $(LINKS); do ../host/siefs ../disk.img symlink acct /bin/$$l 2>/dev/null; done; \
	    cd ../.. && touch $@; \
	fi
# The default model for sia (models/, docs/llm.md) goes in /models once
# (1 GB: copied only if missing). MODEL= picks another file of models/.
MODEL   ?= smollm2-1.7b-instruct-q4_k_m.gguf
model: $(B)/disk.img
	@if [ ! -f models/$(MODEL) ]; then echo "model: models/$(MODEL) is missing (docs/llm.md says where to get it)"; \
	elif ! $(B)/host/siefs $(B)/disk.img stat /models/$(MODEL) >/dev/null 2>&1; then \
	    free=$$($(B)/host/siefs $(B)/disk.img df 2>&1 | sed -n 's/.*KiB, \([0-9]*\) free,.*/\1/p'); \
	    if [ $$(( $${free:-0} * 4096 )) -lt $$(( $$(wc -c < models/$(MODEL)) + 16777216 )) ]; then \
	        echo "model: $(B)/disk.img is too small for models/$(MODEL) (it was made with an older, smaller DISK size)."; \
	        echo "       'make newdisk' makes a $(DISK) one (what you wrote on the old one is lost); sia says 'no model' until then."; \
	    else echo "copying models/$(MODEL) into /models on $(B)/disk.img"; \
	        $(B)/host/siefs $(B)/disk.img mkdir /models 2>/dev/null; \
	        $(B)/host/siefs $(B)/disk.img put models/$(MODEL) /models/$(MODEL); fi; fi
newdisk:
	rm -f $(B)/disk.img
	$(MAKE) $(B)/disk.img

# ---- running. QEMU loads the kernel and the programs itself (multiboot):
# each -initrd entry is a program file; the first is init, the supervisor, which
# starts the others (and starts them again if they fail). KVM when available.
SMP     ?= 4
# MEM: the assistant's model needs ~1.5 GiB (QEMU only takes what is used).
MEM     ?= 4G
ACCEL   := $(shell [ -w /dev/kvm ] && echo "-accel kvm -cpu host")
QEMU_x86_64 := qemu-system-x86_64 -smp $(SMP) -m $(MEM) $(ACCEL) -no-reboot -kernel $(B)/kernel.bin \
           -initrd "$(B)/init.elf,$(B)/con.elf,$(B)/usb.elf,$(B)/vblk.elf,$(B)/fs.elf" \
           -drive file=$(B)/disk.img,if=none,id=d0,format=raw \
           -device virtio-blk-pci,drive=d0,disable-modern=on \
           -netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-modern=on,addr=5
# arm64 (QEMU's "virt" machine, with the Pis' interrupt controller, GICv2, and the
# Pi 4's processor, Cortex-A72): no KVM here, so QEMU emulates the processors
# (one host thread each). The kernel is an "Image", the boot modules one
# archive (-initrd), the disk and network card virtio over MMIO.
QEMU_A64SYS ?= $(shell command -v qemu-system-aarch64 || echo $(CURDIR)/.hosttools/usr/bin/qemu-system-aarch64)
# CPU64: the emulated ARM processor: cortex-a72 = the Pi 4's (no dot product);
# max (or cortex-a76) = like the Pi 5's, with dot product
CPU64 ?= cortex-a72
QEMU_arm64 := $(QEMU_A64SYS) -M virt,gic-version=2 -cpu $(CPU64) -smp $(SMP) -m $(MEM) \
           -accel tcg,thread=multi -no-reboot -kernel $(B)/kernel.bin -initrd $(B)/modules.img \
           -drive file=$(B)/disk.img,if=none,id=d0,format=raw -device virtio-blk-device,drive=d0 \
           -netdev user,id=n0 -device virtio-net-device,netdev=n0
QEMU    := $(QEMU_$(ARCH))
# The screen and its keyboard and pointer, for run and gui-test (the other
# tests run without a screen): x86 has its VGA, PS/2 keyboard and mouse;
# arm64 (virt) gets QEMU's ramfb and virtio keyboard and tablet.
SCREEN_arm64 := -device ramfb -device virtio-keyboard-device -device virtio-tablet-device
SCREEN  := $(SCREEN_$(ARCH))
# arm64: the boot modules in one archive of our own (tools/mkmods.py), init first
$(B)/modules.img: $(PROGS:%=$(B)/%.elf) tools/mkmods.py
	python3 tools/mkmods.py $@ $(B)/init.elf $(B)/con.elf $(B)/usb.elf $(B)/vblk.elf $(B)/fs.elf
SERIAL  := -chardev stdio,id=s0,signal=off -serial chardev:s0
# The QEMU window: resizable; the picture is scaled to the window size.
WINDOW  ?= gtk,zoom-to-fit=on

run: all model
	$(QEMU) $(SCREEN) -display $(WINDOW) $(SERIAL)
# run-nox: no screen at all (no display card): the first start happens on this terminal
run-nox: all model
	$(QEMU) -display none -vga none $(SERIAL)
# The test starts from a fresh disk (no accounts: it runs the first-start
# setup), never from yours.
test: all $(B)/host/fsck.siefs
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s $(TESTDISK) -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	python3 tools/boottest.py $(SMP) $(B)/host $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio
	python3 tools/crashtest.py $(SMP) $(B)/host $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio
# gui-test: the desktop, through QEMU's control socket (screenshots in build/screens)
gui-test: all $(B)/host/fsck.siefs
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s $(TESTDISK) -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	python3 tools/guitest.py $(SMP) $(B)/screens $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) $(SCREEN) -display none -serial stdio
# gui-test-real: the same, but the assistant panel talks to the real sia with the
# default model copied onto the test disk (slower; needs models/$(MODEL))
gui-test-real: all $(B)/host/fsck.siefs
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 3G -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	$(B)/host/siefs $(B)/test.img mkdir /models && $(B)/host/siefs $(B)/test.img put models/$(MODEL) /models/$(MODEL)
	SIA_REAL=1 python3 tools/guitest.py $(SMP) $(B)/screens $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) $(SCREEN) -display none -serial stdio
# crash-test: only the crash-recovery part (kills servers while they work)
crash-test: all $(B)/host/fsck.siefs
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s $(TESTDISK) -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	python3 tools/crashtest.py $(SMP) $(B)/host $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio
# kernel-test: floating-point and AVX registers per thread, thread-local
# variables (in programs built by gcc and by sicc), memory quotas (tools/ktest.py)
$(B)/ktest-sicc.elf: user/ktest/ktest.c $(B)/host/sicc $(TOOLDEP) $(UCORE:%=$(B)/user/lib/%.o) $(ULIB) user/user.ld
	$(KTEST_CC) $(filter-out -mgeneral-regs-only,$(UCFLAGS)) -c $< -o $(B)/user/ktest-sicc.o
	$(TLD) -T user/user.ld -z max-page-size=4096 -o $@ $(UCORE:%=$(B)/user/lib/%.o) $(B)/user/ktest-sicc.o $(ULIB)
kernel-test: all $(B)/ktest-sicc.elf
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s $(TESTDISK) -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	$(B)/host/siefs $(B)/test.img put $(B)/ktest-sicc.elf /bin/ktest-sicc && $(B)/host/siefs $(B)/test.img chmod 755 /bin/ktest-sicc
	python3 tools/ktest.py kernel $(SMP) $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio
# sia-test: the assistant on a fresh disk with a small model (fast):
# answers, two conversations at once, siad killed during an answer
# (init restarts it), speed and memory. SIATEST_MODEL= another one of models/.
SIATEST_MODEL ?= SmolLM2-135M-Instruct-Q8_0.gguf
sia-test: all
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 2G -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	$(B)/host/siefs $(B)/test.img mkdir /models && $(B)/host/siefs $(B)/test.img put models/$(SIATEST_MODEL) /models/$(SIATEST_MODEL)
	printf 'backend=local\nmodel=/models/%s\n' $(SIATEST_MODEL) > $(B)/sia-test.conf && $(B)/host/siefs $(B)/test.img put $(B)/sia-test.conf /etc/sia.conf
	python3 tools/ktest.py sia $(SMP) $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio
# sia-mem-test: sia's memory (docs/sia.md): conversations saved and continued
# after a restart, an idle stop and a reboot; compaction; facts; privacy
sia-mem-test: all
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 2G -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	$(B)/host/siefs $(B)/test.img mkdir /models && $(B)/host/siefs $(B)/test.img put models/$(SIATEST_MODEL) /models/$(SIATEST_MODEL)
	printf 'backend=local\nmodel=/models/%s\n' $(SIATEST_MODEL) > $(B)/sia-test.conf && $(B)/host/siefs $(B)/test.img put $(B)/sia-test.conf /etc/sia.conf
	python3 tools/siamemtest.py $(SMP) $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio
# demand-test: servers started on demand (the network, the assistant), stopped
# when unused and started again; calls racing a stop (tools/demandtest.py)
demand-test: all
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 1G -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	$(B)/host/siefs $(B)/test.img mkdir /models && $(B)/host/siefs $(B)/test.img put models/$(SIATEST_MODEL) /models/$(SIATEST_MODEL)
	printf 'backend=local\nmodel=/models/%s\n' $(SIATEST_MODEL) > $(B)/sia-test.conf && $(B)/host/siefs $(B)/test.img put $(B)/sia-test.conf /etc/sia.conf
	python3 tools/demandtest.py $(SMP) $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio
# clean keeps the disk image (your files in SIEOS); distclean removes it too.
clean:
	find $(B) -mindepth 1 -maxdepth 1 ! -name disk.img -exec rm -rf {} + 2>/dev/null || true
distclean:
	rm -rf $(B)

.PHONY: all run run-nox test crash-test gui-test gui-test-real kernel-test sia-test sia-mem-test demand-test model clean distclean newdisk FORCE
.SECONDARY:

# ---- The Raspberry Pis (ARCH=arm64, docs/raspberrypi.md). PI=4 or 5.
#   make ARCH=arm64 PI=4 sdcard [FIRMWARE=dir] [SDMODEL=1]   an SD card image, build-arm64/sieos-pi4.img
#                                                (SDMODEL=1: with the assistant's model, models/$(MODEL))
#   make ARCH=arm64 run-pi4                      QEMU's Raspberry Pi 4 (raspi4b) with that kind of card
#   make ARCH=arm64 pi4-test                     the boot and crash tests on QEMU's raspi4b
#   make ARCH=arm64 pi4-sdbench / sdhci-bench    SD card speed (raspi4b; virt with a PCI SD controller: DMA)
# The boards' device trees come from the Raspberry Pi firmware (not part of
# SIEOS): fetched once into .hosttools/rpi/ for QEMU; on a real card the
# firmware files are added from FIRMWARE (see docs/raspberrypi.md).
PI      ?= 4
SDDISK  ?= 2G
.hosttools/rpi/%.dtb:
	mkdir -p $(@D) && curl -sfL -o $@ https://raw.githubusercontent.com/raspberrypi/firmware/master/boot/$*.dtb
sdcard: all
	rm -f $(B)/sd-siefs.img && $(B)/host/mkfs.siefs -s $(SDDISK) -L SIEOS -d $(B)/rootfs $(B)/sd-siefs.img
	$(if $(SDMODEL),$(B)/host/siefs $(B)/sd-siefs.img mkdir /models && $(B)/host/siefs $(B)/sd-siefs.img put models/$(MODEL) /models/$(MODEL))
	python3 tools/mksdcard.py $(B)/sieos-pi$(PI).img --pi $(PI) --kernel $(B)/kernel.bin --initrd $(B)/modules.img \
	    --siefs $(B)/sd-siefs.img $(if $(FIRMWARE),--firmware $(FIRMWARE))
	rm -f $(B)/sd-siefs.img
# QEMU's raspi4b: the firmware's part (loading the kernel, the modules and the
# device tree) is done by QEMU itself; the card holds the two partitions.
# (QEMU wants a card size that is a power of two; it has no USB or Ethernet
# for this board, and attaches the card to the second SD controller.)
QEMU_PI4 := $(QEMU_A64SYS) -M raspi4b -kernel $(B)/kernel.bin -dtb .hosttools/rpi/bcm2711-rpi-4-b.dtb \
            -initrd $(B)/modules.img -no-reboot
run-pi4: all .hosttools/rpi/bcm2711-rpi-4-b.dtb
	@if [ ! -f $(B)/pi4-qemu.img ]; then \
	    $(B)/host/mkfs.siefs -s 192M -L SIEOS -d $(B)/rootfs $(B)/pi4-siefs.img && \
	    python3 tools/mksdcard.py $(B)/pi4-qemu.img --pi 4 --kernel $(B)/kernel.bin --initrd $(B)/modules.img \
	        --siefs $(B)/pi4-siefs.img --fat 64 --pow2 && rm -f $(B)/pi4-siefs.img; fi
	$(QEMU_PI4) -drive file=$(B)/pi4-qemu.img,if=sd,format=raw -display $(WINDOW) $(SERIAL)
# (the tests use a plain SieFS card, no partitions, so the host tools can check it after)
pi4-test: all $(B)/host/fsck.siefs .hosttools/rpi/bcm2711-rpi-4-b.dtb
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 64M -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	python3 tools/boottest.py 4 $(B)/host $(B)/test.img $(QEMU_PI4) -drive file=$(B)/test.img,if=sd,format=raw -display none -serial stdio
	python3 tools/crashtest.py 4 $(B)/host $(B)/test.img $(QEMU_PI4) -drive file=$(B)/test.img,if=sd,format=raw -display none -serial stdio
# pi4-sdbench: the SD card's speed on QEMU's raspi4b (bench on a fresh card; SDMIB MiB)
SDMIB ?= 16
SDBSIZE ?= 128M
pi4-sdbench: all $(B)/host/fsck.siefs .hosttools/rpi/bcm2711-rpi-4-b.dtb
	rm -f $(B)/sdb.img && $(B)/host/mkfs.siefs -s $(SDBSIZE) -L SIEOS -d $(B)/rootfs $(B)/sdb.img >/dev/null
	python3 tools/sdbench.py $(B)/host $(B)/sdb.img $(SDMIB) $(QEMU_PI4) -drive file=$(B)/sdb.img,if=sd,format=raw -display none -serial stdio
# sdhci-bench: the SD driver's DMA, interrupt and speed paths on QEMU's virt with a
# standard SD controller on PCI (QEMU's raspi4b controller reports no DMA).
# SDCAPS: its capabilities (0x057834b4: ADMA2 32-bit, High Speed; 0x157834b4: + 64-bit)
SDCAPS ?= 0x157834b4
QEMU_SDHCI = $(QEMU_A64SYS) -M virt,gic-version=2 -cpu $(CPU64) -smp $(SMP) -m 1G -accel tcg,thread=multi \
             -no-reboot -kernel $(B)/kernel.bin -initrd $(B)/modules.img \
             -device sdhci-pci,sd-spec-version=3,capareg=$(SDCAPS) -drive if=none,id=sd0,file=$(B)/sdb.img,format=raw \
             -device sd-card,drive=sd0
sdhci-bench: all $(B)/host/fsck.siefs
	rm -f $(B)/sdb.img && $(B)/host/mkfs.siefs -s $(SDBSIZE) -L SIEOS -d $(B)/rootfs $(B)/sdb.img >/dev/null
	python3 tools/sdbench.py $(B)/host $(B)/sdb.img $(SDMIB) $(QEMU_SDHCI) -display none -serial stdio
.PHONY: sdcard run-pi4 pi4-test pi4-sdbench sdhci-bench

# ---- PCs with UEFI firmware, from a USB key (x86-64; docs/usb.md):
#   make usb          build/sieos-usb.img: write it to a USB key; the PC boots it
#                     (SIEOS's own UEFI loader, boot/uefi) and SIEOS uses the
#                     key as its disk: a GPT with the EFI partition and SieFS
#   make run-usb      that key in QEMU, booting through UEFI like a real PC
#   make usb-test     a fresh key in QEMU: first start on the screen, files
#                     written, a second boot finds them; the key is checked
# USBDISK: the key's size (the image is sparse); USBMODEL=1: with the
# assistant's model (models/$(MODEL)); USBSCREEN: the preferred screen size.
# USBBUS: how QEMU attaches the key: usb (QEMU's xHCI and USB storage: the
# real case) or virtio (the image as a plain disk).
USBDISK   ?= 8G
USBSCREEN ?= 1920x1080
USBBUS    ?= usb
# UEFI firmware for the virtual PC (the key boots like on a real PC), one file
UEFI_FW   ?= /usr/share/ovmf/OVMF.fd
UEFI_CF   := -std=gnu11 -O2 -ffreestanding -fpic -mgeneral-regs-only -Iinclude -Iboot/uefi
# The loader, compiled and linked by sicc, twice (two addresses), then made a
# PE file by tools/elf2efi.py (its 64-bit addresses become relocations).
$(B)/uefi/BOOTX64.EFI: boot/uefi/start.S boot/uefi/loader.c boot/uefi/efi.h boot/uefi/efi.ld include/mk/boot.h \
                       lib/string.c tools/elf2efi.py $(SICC)
	@mkdir -p $(@D)
	$(SICC) $(UEFI_CF) -c boot/uefi/start.S -o $(@D)/start.o
	$(SICC) $(UEFI_CF) -c boot/uefi/loader.c -o $(@D)/loader.o
	$(SICC) $(UEFI_CF) -c lib/string.c -o $(@D)/string.o
	for a in 0x10000000 0x20000000; do sed "s/BASE/$$a/" boot/uefi/efi.ld > $(@D)/efi-$$a.ld && \
	    $(SICC) --ld -T $(@D)/efi-$$a.ld -o $(@D)/loader-$$a.elf $(@D)/start.o $(@D)/loader.o $(@D)/string.o || exit 1; done
	python3 tools/elf2efi.py $(@D)/loader-0x10000000.elf $(@D)/loader-0x20000000.elf $@
# (a new key each time: what you wrote on the previous image is lost; on the
# real key it stays, since you write the image once)
USBMAKE = python3 tools/mkusb.py $(1) --size $(2) --efi $(B)/uefi/BOOTX64.EFI --kernel $(B)/kernel.bin \
          --module $(B)/init.elf --module $(B)/con.elf --module $(B)/usb.elf --module $(B)/vblk.elf --module $(B)/fs.elf \
          --mkfs $(B)/host/mkfs.siefs --siefs-tool $(B)/host/siefs --rootfs $(B)/rootfs --screen $(USBSCREEN)
usb: all $(B)/uefi/BOOTX64.EFI $(B)/host/mkfs.siefs $(B)/host/siefs
	$(call USBMAKE,$(B)/sieos-usb.img,$(USBDISK)) $(if $(USBMODEL),--put models/$(MODEL) /models/$(MODEL))
QEMU_UEFI = qemu-system-x86_64 -smp $(SMP) -m $(MEM) $(ACCEL) -no-reboot \
           -bios $(UEFI_FW) \
           -drive if=none,id=k,file=$(1),format=raw \
           $(if $(filter usb,$(USBBUS)),-device qemu-xhci -device usb-storage$(,)drive=k$(,)bootindex=0 -device usb-tablet, \
                -device virtio-blk-pci$(,)drive=k$(,)disable-modern=on$(,)bootindex=0) \
           -netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-modern=on,addr=5
, := ,
run-usb: $(B)/sieos-usb.img
	$(call QEMU_UEFI,$(B)/sieos-usb.img) -display $(WINDOW) $(SERIAL)
$(B)/sieos-usb.img:
	$(MAKE) usb
usb-test: all $(B)/uefi/BOOTX64.EFI $(B)/host/mkfs.siefs $(B)/host/siefs $(B)/host/fsck.siefs
	$(call USBMAKE,$(B)/usb-test.img,512M)
	python3 tools/usbtest.py $(SMP) $(B)/host $(B)/usb-test.img $(B)/screens $(call QEMU_UEFI,$(B)/usb-test.img) -display none -serial stdio
.PHONY: usb run-usb usb-test

# usb-dev-test: the USB stack itself (tools/usbdevtest.py), on both architectures:
# SIEOS's only disk on a USB key (no virtio disk), the first start typed on a USB
# keyboard, bench, the USB server killed during fsloop, a second USB disk
# plugged / unplugged / plugged again, a second boot. Booted directly (no UEFI):
# x86-64 with the kernel and modules from QEMU; arm64 on virt, xHCI on PCIe (ECAM).
USBDEV_QEMU_x86_64 = qemu-system-x86_64 -smp $(SMP) -m $(MEM) $(ACCEL) -no-reboot -kernel $(B)/kernel.bin \
           -initrd "$(B)/init.elf,$(B)/con.elf,$(B)/usb.elf,$(B)/vblk.elf,$(B)/fs.elf" -vga none
USBDEV_QEMU_arm64 = $(QEMU_A64SYS) -M virt,gic-version=2 -cpu $(CPU64) -smp $(SMP) -m $(MEM) \
           -accel tcg,thread=multi -no-reboot -kernel $(B)/kernel.bin -initrd $(B)/modules.img
usb-dev-test: all $(B)/host/fsck.siefs $(if $(filter arm64,$(ARCH)),$(B)/modules.img)
	rm -f $(B)/usbdev.img && $(B)/host/mkfs.siefs -s $(TESTDISK) -L SIEOS -d $(B)/rootfs $(B)/usbdev.img >/dev/null
	python3 tools/usbdevtest.py $(SMP) $(B)/host $(B)/usbdev.img $(USBDEV_QEMU_$(ARCH)) -display none -serial stdio
.PHONY: usb-dev-test

# ---- SieFS, the file system (siefs/): host tools, tests, and a check that
# the library also builds the way SIEOS programs are built.
#   make siefs-tools         build/host/{mkfs.siefs,fsck.siefs,siefs-dump,siefs}
#   make siefs-test          unit, model, crash and corruption tests, with measurements
#   make siefs-freestanding  compile the library with the SIEOS flags (no C library)
HOSTCFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Isiefs -Itools/siefs -Itools -Iinclude
SIEFS_LIB   := siefs/crc.c siefs/tree.c siefs/space.c siefs/fs.c siefs/check.c lib/fmt.c
SIEFS_H     := siefs/siefs.h siefs/siefs_int.h tools/siefs/host.h
SIEFS_TOOLS := mkfs.siefs fsck.siefs siefs-dump siefs
SIEFS_FCFLAGS := -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic \
           -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns -mgeneral-regs-only -Iinclude -Isiefs

siefs-tools: $(SIEFS_TOOLS:%=$(B)/host/%)
$(B)/host/%: tools/siefs/%.c tools/siefs/host.c tools/hostdir.c $(SIEFS_LIB) $(SIEFS_H)
	@mkdir -p $(@D)
	gcc $(HOSTCFLAGS) -o $@ $< tools/siefs/host.c tools/hostdir.c $(SIEFS_LIB)
$(B)/host/siefs-test: tests/siefs/test.c $(SIEFS_LIB) $(SIEFS_H)
	@mkdir -p $(@D)
	gcc $(HOSTCFLAGS) -o $@ tests/siefs/test.c $(SIEFS_LIB)
siefs-test: $(B)/host/siefs-test siefs-freestanding
	$(B)/host/siefs-test

# Freestanding: compile as SIEOS does, then make sure nothing outside the
# library is needed but memcpy & co. (lib/string.c) and vformat (lib/fmt.c).
siefs-freestanding: $(SIEFS_LIB:%.c=$(B)/siefs-fs/%.o) $(B)/siefs-fs/lib/string.o
	@ld -r -o $(B)/siefs-fs/siefs.o $^
	@u=$$(nm -u $(B)/siefs-fs/siefs.o); if [ -n "$$u" ]; then echo "siefs: undefined symbols: $$u"; exit 1; fi
	@size $(B)/siefs-fs/siefs.o | tail -1 | awk '{print "siefs library, freestanding -O2: " $$1+$$2+$$3 " bytes (text " $$1 ")"}'
$(B)/siefs-fs/%.o: %.c $(SIEFS_H)
	@mkdir -p $(@D)
	gcc $(SIEFS_FCFLAGS) -c $< -o $@

.PHONY: siefs-tools siefs-test siefs-freestanding

# ---- the crypto library (lib/blake2b.c, chacha20.c, argon2.c): official test
# vectors, BLAKE2b against an independent implementation, Argon2id timing.
CRYPTO_SRC := lib/blake2b.c lib/chacha20.c lib/argon2.c lib/string.c
$(B)/host/crypto-test: tests/crypto/test.c $(CRYPTO_SRC) include/mk/crypto.h
	@mkdir -p $(@D)
	gcc $(HOSTCFLAGS) -o $@ tests/crypto/test.c $(CRYPTO_SRC)
crypto-test: $(B)/host/crypto-test
	$(B)/host/crypto-test
	python3 tests/crypto/check.py $(B)/host/crypto-test
.PHONY: crypto-test

# ---- sicc, the SIEOS C compiler (cc/, docs/cc.md): compiler, assembler and
# linker in one program, written for SIEOS.
#   make cc             build/host/sicc (built by the host compiler)
#   make cc-test        test programs (sicc and the host compiler must agree), bad programs
#                       (the right error at the right place), random expressions
#   make cc-bench       code speed and size against the host compiler
#   make cc-bootstrap   sicc compiles itself twice: the two results must be identical
#   make cc-sieos       all of SIEOS compiled, assembled and linked by sicc (build-sicc/), then its tests
#   make cc-a64-test    the tests for AArch64, under emulation (QEMU_A64=path of qemu-aarch64)
#   make cc-a64-bootstrap  sicc for AArch64 compiles itself (emulated): stages 2 and 3 identical
SICC_SRC  := util lex pp parse ir opt ra emit emit_a64 asm link main
cc: $(SICC)
$(SICC): $(SICC_SRC:%=cc/%.c) cc/*.inc cc/sicc.h cc/ir.h cc/sys.h cc/include/*.h
	@mkdir -p $(@D)
	gcc -std=gnu11 -O2 -Wall -Wextra -o $@ $(SICC_SRC:%=cc/%.c)
	rm -rf $(B)/host/sicc-include && cp -r cc/include $(B)/host/sicc-include
cc-test: $(SICC)
	OUT=$(B)/cc-tests tests/cc/run.sh $(SICC)
	tests/cc/errors.sh $(SICC)
	OUT=$(B)/cc-fuzz python3 tests/cc/fuzz.py $(SICC) 30
	OUT=$(B)/cc-cfuzz python3 tests/cc/cfuzz.py $(SICC) 30
	OUT=$(B)/cc-abi python3 tests/cc/abigen.py $(SICC) 60
# stage 2: sicc's sources compiled by sicc (linked by the host, with its C library); stage 3: by stage 2
cc-bootstrap: $(SICC)
	@for st in 2 3; do \
	    prev=$$([ $$st = 2 ] && echo $(SICC) || echo $(B)/cc-boot/sicc2); \
	    mkdir -p $(B)/cc-boot/s$$st; \
	    for f in $(SICC_SRC); do $$prev -O2 -c -o $(B)/cc-boot/s$$st/$$f.o cc/$$f.c || exit 1; done; \
	    gcc -no-pie -o $(B)/cc-boot/sicc$$st $(SICC_SRC:%=$(B)/cc-boot/s$$st/%.o) || exit 1; \
	    rm -rf $(B)/cc-boot/sicc-include && cp -r cc/include $(B)/cc-boot/sicc-include; \
	done
	cmp $(B)/cc-boot/sicc2 $(B)/cc-boot/sicc3 && echo "bootstrap: stage 2 and stage 3 are identical"
	OUT=$(B)/cc-boot/tests tests/cc/run.sh $(B)/cc-boot/sicc3
# SIEOS by sicc: wrappers named gcc/ld/ar/objcopy send SIEOS's work to sicc (the
# host tools, which run on the development machine, still use the host compiler)
SB := build-sicc
cc-sieos: $(SICC)
	@mkdir -p $(SB)/bin
	@printf '#!/bin/sh\ncase " $$* " in *" llm/"*) ;; *" -ffreestanding "*) exec %s "$$@";; esac\nPATH="%s" exec %s "$$@"\n' $(CURDIR)/$(SICC) "$$PATH" $$(command -v gcc) > $(SB)/bin/gcc
	@printf '#!/bin/sh\nexec %s --ld "$$@"\n' $(CURDIR)/$(SICC) > $(SB)/bin/ld
	@printf '#!/bin/sh\nexec %s --ar "$$@"\n' $(CURDIR)/$(SICC) > $(SB)/bin/ar
	@printf '#!/bin/sh\nexec %s --objcopy "$$@"\n' $(CURDIR)/$(SICC) > $(SB)/bin/objcopy
	@chmod +x $(SB)/bin/*
	$(MAKE) B=$(SB) PATH=$(CURDIR)/$(SB)/bin:$$PATH all
	$(MAKE) B=$(SB) PATH=$(CURDIR)/$(SB)/bin:$$PATH test
cc-bench: $(SICC)
	OUT=$(B)/cc-bench tests/cc/bench/run.sh $(SICC)
# AArch64, under QEMU's user-mode emulator (a host tool for tests only): the one
# on the PATH, else a private copy unpacked in .hosttools/ (not in git)
QEMU_A64 ?= $(shell command -v qemu-aarch64 || echo $(CURDIR)/.hosttools/usr/bin/qemu-aarch64)
cc-a64-test: $(SICC)
	OUT=$(B)/cc-a64/tests tests/cc/run_a64.sh $(SICC) $(QEMU_A64) -O0
	OUT=$(B)/cc-a64/tests tests/cc/run_a64.sh $(SICC) $(QEMU_A64) -O2
	tests/cc/errors.sh $(SICC) --target=aarch64
	OUT=$(B)/cc-a64/enc python3 tests/cc/a64/enc.py $(SICC)
	OUT=$(B)/cc-a64/abi python3 tests/cc/a64/abi_a64.py $(SICC) $(QEMU_A64) 60
	QEMU=$(QEMU_A64) OUT=$(B)/cc-a64/fuzz python3 tests/cc/fuzz.py $(SICC) 30
	QEMU=$(QEMU_A64) OUT=$(B)/cc-a64/cfuzz python3 tests/cc/cfuzz.py $(SICC) 30
# stage 2: sicc for AArch64, compiled by sicc; stage 3: by stage 2 (emulated); identical
cc-a64-bootstrap: $(SICC)
	OUT=$(B)/cc-a64/boot tests/cc/a64/bootstrap.sh $(SICC) $(QEMU_A64)
.PHONY: cc cc-test cc-bootstrap cc-sieos cc-bench cc-a64-test cc-a64-bootstrap

# ---- The language-model engine (llm/, docs/llm.md): host tools and tests.
# The engine is portable C (no C library); the tools give it memory, files
# and threads (tools/llm/host.c, ISO C). Models go in models/ (not in git).
LLM_SRC  := gguf quant model tok chat math
LLM_CF   := -std=gnu11 -O2 -Wall -Wextra -Iinclude -fno-math-errno
LLM_OBJ  := $(LLM_SRC:%=$(B)/llm/%.o) $(B)/llm/string.o
$(B)/llm/%.o: llm/%.c llm/llm.h llm/internal.h
	@mkdir -p $(@D)
	gcc $(LLM_CF) -c $< -o $@
$(B)/llm/string.o: lib/string.c
	@mkdir -p $(@D)
	gcc $(LLM_CF) -c $< -o $@
$(B)/llm/%: tools/llm/%.c tools/llm/host.c tools/llm/host.h $(LLM_OBJ)
	gcc -std=c11 -O2 -Wall -Wextra -o $@ $< tools/llm/host.c $(LLM_OBJ)
llm: $(B)/llm/llm-run $(B)/llm/llm-info
llm-test: llm
	tests/llm/run.sh $(B)/llm
llm-bench: llm
	tests/llm/bench.sh $(B)/llm
# the engine as SIEOS's servers will build it: freestanding, nothing but lib/string.c
llm-freestanding:
	@mkdir -p $(B)/llm-fs
	@for f in $(LLM_SRC); do gcc -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic \
	    -fno-asynchronous-unwind-tables -fno-math-errno -Iinclude -c llm/$$f.c -o $(B)/llm-fs/$$f.o || exit 1; done
	@ld -r -o $(B)/llm-fs/llm.o $(LLM_SRC:%=$(B)/llm-fs/%.o) && \
	 nm -u $(B)/llm-fs/llm.o | grep -v -E ' (memset|memcpy|memmove|memcmp|strlen|strcmp|strchr|strlcpy)$$' \
	 && { echo "llm: unexpected outside symbols above"; exit 1; } || echo "llm: freestanding build ok, size: $$(size $(B)/llm-fs/llm.o | tail -1 | cut -f1) bytes of code"
# The NEON kernels (llm/neon.c), built by sicc for AArch64, run under the emulator
llm-neon-test: $(SICC)
	OUT=$(B)/llm-neon tests/llm/neon.sh $(SICC) $(QEMU_A64)
.PHONY: llm llm-test llm-bench llm-freestanding llm-neon-test

# ---- the network (docs/net.md): the TLS crypto against official vectors and
# openssl-made signatures (host), then the whole stack in SIEOS: DHCP, ping,
# DNS, HTTP, a listening socket, HTTPS (real sites and a test CA), sia's
# remote backend against a mock server, a netd restart. NET_OFFLINE=1 skips
# the real sites.
NET_CRYPTO := lib/sha2.c lib/aead.c lib/x25519.c lib/pubkey.c lib/chacha20.c lib/string.c
$(B)/host/net-crypto-test: tests/net/crypto.c $(NET_CRYPTO) include/mk/crypto.h
	@mkdir -p $(@D)
	gcc $(HOSTCFLAGS) -o $@ tests/net/crypto.c $(NET_CRYPTO)
net-crypto-test: $(B)/host/net-crypto-test
	python3 tests/net/gen.py $(B)/host/sigvec.txt
	$(B)/host/net-crypto-test $(B)/host/sigvec.txt
net-test: all $(B)/host/fsck.siefs net-crypto-test
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 128M -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	python3 tools/nettest.py $(SMP) $(B)/host $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio
.PHONY: net-crypto-test net-test
