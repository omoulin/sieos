# posix.mk - The toolchain for ported software and libposix
# (docs/webkit-plan.md, phases 0 and 3).
#
#   make wk-tools     the host tools, fetched into .hosttools/wk (no root):
#                     clang and lld (LLVM's release binaries), CMake and
#                     Ninja (their PyPI wheels), gperf (built from source)
#   make posix        libposix: musl's C library from its release, its system
#                     calls sent to the Linux personality (user/posix/linux.c),
#                     installed with its headers in $(B)/posix/sysroot
#   make posix-test   C programs (tests/posix) built against it, run in QEMU
#   make cxx          the C++ runtime (libc++, libc++abi, libunwind) and
#                     compiler-rt's builtins, from LLVM's sources, into the sysroot
#   make cxx-test     C++ programs (tests/cxx) built with it, run in QEMU
#
# Nothing downloaded is kept in git: each archive's SHA-256 is checked here.
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

WKT       := .hosttools/wk
LLVM_VER  := 23.1.3
LLVM_SHA  := ca9fe385a5d9e2d88132dc0e23a00e9f87f38fa75f67207b5e457d208e71d013
LLSRC_SHA := c44186a7762ed28954be72e5ff6df9808e0779d4f1bf014ecc4e7e211d31ee34
MUSL_VER  := 1.2.6
MUSL_SHA  := d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
CMAKE_VER := 4.4.4
CMAKE_SHA := f773a0544c66370408f451acd487c2c643b0dfebb48c5d64f3638bc1bb820238
CMAKE_URL := https://files.pythonhosted.org/packages/7e/d5/664ea06d01864f25d6067f09fa218404e4d588edd5e495135ac91429fecb/cmake-$(CMAKE_VER)-py3-none-manylinux2014_x86_64.manylinux_2_17_x86_64.whl
NINJA_VER := 1.13.2
NINJA_SHA := 65a24341b5ac09fcadcc37082660be40a94174e51a937fabf6e2cae26225fa2c
NINJA_URL := https://files.pythonhosted.org/packages/6e/53/ebfed7b689c338dd8ebeec9c0730c8d56821292f14e2536e5f3ef1a05744/ninja-$(NINJA_VER)-py3-none-manylinux2014_x86_64.manylinux_2_17_x86_64.whl
GPERF_VER := 3.3
GPERF_SHA := fd87e0aba7e43ae054837afd6cd4db03a3f2693deb3619085e6ed9d8d9604ad8

# $(call fetch,URL,FILE,SHA256): into .hosttools/dl, once, checked
fetch = mkdir -p .hosttools/dl && { [ -f .hosttools/dl/$(2) ] || { curl -sfL -o .hosttools/dl/$(2).part '$(1)' && \
        mv .hosttools/dl/$(2).part .hosttools/dl/$(2); }; } && { echo "$(3)  .hosttools/dl/$(2)" | sha256sum -c --status || \
        { echo "$(2): not the expected file"; rm -f .hosttools/dl/$(2); exit 1; }; }

# ---- the host tools. Of LLVM's 2 GB archive only the compiler, the linker,
# the archive and object tools and clang's own headers are kept.
LLVM      := $(WKT)/llvm
LLVM_TAR  := LLVM-$(LLVM_VER)-Linux-X64
LLVM_KEEP := bin/clang-23 bin/clang bin/clang++ bin/lld bin/ld.lld bin/llvm-ar bin/llvm-ranlib bin/llvm-nm \
             bin/llvm-objcopy bin/llvm-strip bin/llvm-readobj bin/llvm-readelf bin/llvm-size lib/clang/23/include \
             lib/clang/23/lib/x86_64-unknown-linux-gnu/libclang_rt.builtins.a
$(LLVM)/bin/clang:
	$(call fetch,https://github.com/llvm/llvm-project/releases/download/llvmorg-$(LLVM_VER)/$(LLVM_TAR).tar.xz,$(LLVM_TAR).tar.xz,$(LLVM_SHA))
	rm -rf $(LLVM) && mkdir -p $(LLVM)
	tar xJf .hosttools/dl/$(LLVM_TAR).tar.xz -C $(LLVM) --strip-components=1 $(LLVM_KEEP:%=$(LLVM_TAR)/%)
	rm .hosttools/dl/$(LLVM_TAR).tar.xz && touch $@
$(WKT)/bin/cmake:
	$(call fetch,$(CMAKE_URL),cmake-$(CMAKE_VER).whl,$(CMAKE_SHA))
	rm -rf $(WKT)/cmake && mkdir -p $(WKT)/bin && python3 -m zipfile -e .hosttools/dl/cmake-$(CMAKE_VER).whl $(WKT)/cmake
	chmod +x $(WKT)/cmake/cmake/data/bin/* && ln -sf ../cmake/cmake/data/bin/cmake $@
$(WKT)/bin/ninja:
	$(call fetch,$(NINJA_URL),ninja-$(NINJA_VER).whl,$(NINJA_SHA))
	rm -rf $(WKT)/ninja && mkdir -p $(WKT)/bin && python3 -m zipfile -e .hosttools/dl/ninja-$(NINJA_VER).whl $(WKT)/ninja
	chmod +x $(WKT)/ninja/ninja-$(NINJA_VER).data/scripts/ninja && ln -sf ../ninja/ninja-$(NINJA_VER).data/scripts/ninja $@
$(WKT)/bin/gperf:
	$(call fetch,https://ftp.gnu.org/gnu/gperf/gperf-$(GPERF_VER).tar.gz,gperf-$(GPERF_VER).tar.gz,$(GPERF_SHA))
	rm -rf $(WKT)/src/gperf-$(GPERF_VER) && mkdir -p $(WKT)/src && tar xzf .hosttools/dl/gperf-$(GPERF_VER).tar.gz -C $(WKT)/src
	cd $(WKT)/src/gperf-$(GPERF_VER) && ./configure -q --prefix=$(CURDIR)/$(WKT) && $(MAKE) -s && $(MAKE) -s install
	rm -rf $(WKT)/src/gperf-$(GPERF_VER)
# LLVM's sources: only the runtimes (C++ library, its ABI library, the
# unwinder, compiler-rt's builtins) and what their CMake files include
LLSRC     := $(WKT)/llvm-src
LLSRC_TAR := llvm-project-$(LLVM_VER).src
LLSRC_KEEP := runtimes cmake libcxx libcxxabi libunwind compiler-rt libc llvm/cmake llvm/utils third-party/siphash
$(LLSRC)/runtimes/CMakeLists.txt:
	$(call fetch,https://github.com/llvm/llvm-project/releases/download/llvmorg-$(LLVM_VER)/$(LLSRC_TAR).tar.xz,$(LLSRC_TAR).tar.xz,$(LLSRC_SHA))
	rm -rf $(LLSRC) && mkdir -p $(LLSRC)
	tar xJf .hosttools/dl/$(LLSRC_TAR).tar.xz -C $(LLSRC) --strip-components=1 $(LLSRC_KEEP:%=$(LLSRC_TAR)/%)
	rm .hosttools/dl/$(LLSRC_TAR).tar.xz && touch $@
wk-tools: $(LLVM)/bin/clang $(WKT)/bin/cmake $(WKT)/bin/ninja $(WKT)/bin/gperf $(LLSRC)/runtimes/CMakeLists.txt

# ---- libposix: musl + the Linux personality, for this processor
PXB       := $(B)/posix
SYSROOT   := $(PXB)/sysroot
MARCH     := $(if $(filter arm64,$(ARCH)),aarch64,x86_64)
TRIPLE    := $(MARCH)-linux-musl
PX_CC     := $(CURDIR)/$(LLVM)/bin/clang --target=$(TRIPLE)
PX_CFLAGS := --sysroot=$(CURDIR)/$(SYSROOT) -Os -fno-stack-protector -ffunction-sections -fdata-sections -Wall -Wextra
# compiler-rt's builtins (integer and soft-float helpers the compiler calls),
# built with the C++ runtime below, for each processor alike
PX_RT     := $(SYSROOT)/usr/lib/linux/libclang_rt.builtins-$(MARCH).a
# static, not position-independent, each segment on its own pages (SIEOS's
# loader maps whole pages), unused sections dropped
PX_LD     := -static -nostdlib -fuse-ld=lld -Wl,-z,separate-loadable-segments -Wl,--gc-sections -Wl,--eh-frame-hdr -L$(SYSROOT)/usr/lib
PX_LIBS   := $(SYSROOT)/usr/lib/crt1.o $(SYSROOT)/usr/lib/libc.a $(PX_RT)
# musl's files replaced: the system calls (syscall_arch.h), and its few
# assembly files that make them directly (threads, the thread pointer,
# cancellation; the generic C versions take over the rest)
MUSL_DROP := src/thread/$(MARCH)/clone.s src/thread/$(MARCH)/__set_thread_area.s src/thread/$(MARCH)/__set_thread_area.c \
             src/thread/$(MARCH)/syscall_cp.s src/thread/$(MARCH)/__unmapself.s src/signal/$(MARCH)/restore.s \
             src/process/$(MARCH)/vfork.s
$(PXB)/musl/lib/libc.a: user/posix/syscall_arch.h user/posix/sieos_spawn.h user/posix/musl/*/*.c | $(LLVM)/bin/clang
	$(call fetch,https://musl.libc.org/releases/musl-$(MUSL_VER).tar.gz,musl-$(MUSL_VER).tar.gz,$(MUSL_SHA))
	rm -rf $(PXB)/musl && mkdir -p $(PXB) && tar xzf .hosttools/dl/musl-$(MUSL_VER).tar.gz -C $(PXB) && mv $(PXB)/musl-$(MUSL_VER) $(PXB)/musl
	cd $(PXB)/musl && rm -f $(MUSL_DROP) && for d in thread process network; do mkdir -p src/$$d/$(MARCH) && \
	    cp $(CURDIR)/user/posix/musl/$$d/*.c src/$$d/$(MARCH)/ || exit 1; done && \
	    cp $(CURDIR)/user/posix/sieos_spawn.h src/process/$(MARCH)/ && cp $(CURDIR)/user/posix/syscall_arch.h arch/$(MARCH)/syscall_arch.h
	cd $(PXB)/musl && ./configure --target=$(TRIPLE) --prefix=/usr --disable-shared CC="$(PX_CC)" \
	    AR=$(CURDIR)/$(LLVM)/bin/llvm-ar RANLIB=$(CURDIR)/$(LLVM)/bin/llvm-ranlib CFLAGS="-Os -ffunction-sections -fdata-sections" >/dev/null
	$(MAKE) -s -C $(PXB)/musl ARCH=$(MARCH) lib/libc.a
	$(MAKE) -s -C $(PXB)/musl ARCH=$(MARCH) install-headers DESTDIR=$(CURDIR)/$(SYSROOT)
# (+ the few Linux headers ported code includes itself: user/posix/include)
$(SYSROOT)/usr/lib/libc.a: $(PXB)/musl/lib/libc.a $(PXB)/linux.o $(wildcard user/posix/include/*/*.h)
	mkdir -p $(@D) && cp $< $@ && $(LLVM)/bin/llvm-ar rs $@ $(PXB)/linux.o && cp -r user/posix/include/. $(SYSROOT)/usr/include/
	for l in m pthread rt dl util xnet resolv crypt; do rm -f $(@D)/lib$$l.a && $(LLVM)/bin/llvm-ar rc $(@D)/lib$$l.a; done   # (all in libc.a, as musl does)
$(PXB)/linux.o: user/posix/linux.c user/posix/sock.c user/posix/spawn.c user/posix/sieos_spawn.h include/mk/*.h siefs/siefs.h $(PXB)/musl/lib/libc.a
	$(PX_CC) $(PX_CFLAGS) -Wno-missing-field-initializers -Wno-sign-compare -Iinclude -Isiefs -c $< -o $@
$(SYSROOT)/usr/lib/crt1.o: user/posix/start.c $(PXB)/musl/lib/libc.a
	mkdir -p $(@D) && $(PX_CC) $(PX_CFLAGS) -c $< -o $@
posix: $(SYSROOT)/usr/lib/libc.a $(SYSROOT)/usr/lib/crt1.o

# ---- the C++ runtime (libc++, libc++abi, libunwind) and compiler-rt's
# builtins, built by CMake from LLVM's sources against the sysroot, static
RTB       := $(PXB)/runtimes
CMAKE     := $(CURDIR)/$(WKT)/bin/cmake
NINJA     := $(CURDIR)/$(WKT)/bin/ninja
RT_OFF    := SANITIZERS XRAY LIBFUZZER PROFILE MEMPROF ORC CTX_PROFILE GWP_ASAN CRT
$(SYSROOT)/usr/lib/libc++.a: $(LLSRC)/runtimes/CMakeLists.txt | $(SYSROOT)/usr/lib/libc.a $(SYSROOT)/usr/lib/crt1.o $(WKT)/bin/cmake $(WKT)/bin/ninja
	rm -rf $(RTB) && $(CMAKE) -G Ninja -S $(LLSRC)/runtimes -B $(RTB) -DCMAKE_MAKE_PROGRAM=$(NINJA) \
	    -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=$(MARCH) \
	    $(foreach l,C CXX ASM,-DCMAKE_$(l)_COMPILER_TARGET=$(TRIPLE)) -DCMAKE_ASM_COMPILER=$(CURDIR)/$(LLVM)/bin/clang \
	    -DCMAKE_C_COMPILER=$(CURDIR)/$(LLVM)/bin/clang -DCMAKE_CXX_COMPILER=$(CURDIR)/$(LLVM)/bin/clang++ \
	    -DCMAKE_AR=$(CURDIR)/$(LLVM)/bin/llvm-ar -DCMAKE_RANLIB=$(CURDIR)/$(LLVM)/bin/llvm-ranlib -DCMAKE_NM=$(CURDIR)/$(LLVM)/bin/llvm-nm \
	    -DCMAKE_SYSROOT=$(CURDIR)/$(SYSROOT) -DCMAKE_INSTALL_PREFIX=$(CURDIR)/$(SYSROOT)/usr -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
	    -DCMAKE_C_FLAGS="-ffunction-sections -fdata-sections" -DCMAKE_CXX_FLAGS="-ffunction-sections -fdata-sections" \
	    -DLLVM_ENABLE_RUNTIMES="compiler-rt;libunwind;libcxxabi;libcxx" -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
	    -DCOMPILER_RT_BUILD_BUILTINS=ON $(RT_OFF:%=-DCOMPILER_RT_BUILD_%=OFF) -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
	    -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBUNWIND_USE_COMPILER_RT=ON \
	    -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_USE_COMPILER_RT=ON -DLIBCXXABI_NON_DEMANGLING_TERMINATE=ON \
	    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF \
	    $(foreach p,LIBCXX LIBCXXABI LIBUNWIND COMPILER_RT,-D$(p)_INCLUDE_TESTS=OFF) -DLIBCXX_INCLUDE_BENCHMARKS=OFF > $(PXB)/runtimes.log
	$(NINJA) -C $(RTB) install >> $(PXB)/runtimes.log
cxx: $(SYSROOT)/usr/lib/libc++.a

# ---- posix-test: tests/posix/NAME.c -> /bin/ptest-NAME, run as root in QEMU
PTESTS    := $(patsubst tests/posix/%.c,%,$(wildcard tests/posix/*.c))
$(B)/ptest-%.elf: tests/posix/%.c $(SYSROOT)/usr/lib/libc.a $(SYSROOT)/usr/lib/crt1.o $(SYSROOT)/usr/lib/libc++.a
	$(PX_CC) $(PX_CFLAGS) -O2 -pthread $(PX_LD) -o $@ $< $(PX_LIBS)
posix-test: all $(PTESTS:%=$(B)/ptest-%.elf)
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 128M -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	for t in $(PTESTS); do $(B)/host/siefs $(B)/test.img put $(B)/ptest-$$t.elf /bin/ptest-$$t && \
	    $(B)/host/siefs $(B)/test.img chmod 755 /bin/ptest-$$t || exit 1; done
	PTESTS="$(PTESTS)" python3 tools/ktest.py posix $(SMP) $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio

# ---- cxx-test: tests/cxx/NAME.cpp -> /bin/cxxtest-NAME (libc++, exceptions, threads)
CXXTESTS  := $(patsubst tests/cxx/%.cpp,%,$(wildcard tests/cxx/*.cpp))
$(B)/cxxtest-%.elf: tests/cxx/%.cpp $(SYSROOT)/usr/lib/libc.a $(SYSROOT)/usr/lib/crt1.o $(SYSROOT)/usr/lib/libc++.a
	$(PX_CC) $(PX_CFLAGS) -O2 -std=c++20 -stdlib=libc++ -pthread $(PX_LD) -o $@ $< $(SYSROOT)/usr/lib/crt1.o \
	    $(SYSROOT)/usr/lib/libc++.a $(SYSROOT)/usr/lib/libc++abi.a $(SYSROOT)/usr/lib/libunwind.a $(SYSROOT)/usr/lib/libc.a $(PX_RT)
cxx-test: all $(CXXTESTS:%=$(B)/cxxtest-%.elf)
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 128M -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	for t in $(CXXTESTS); do $(B)/host/siefs $(B)/test.img put $(B)/cxxtest-$$t.elf /bin/cxxtest-$$t && \
	    $(B)/host/siefs $(B)/test.img chmod 755 /bin/cxxtest-$$t || exit 1; done
	PTESTS="$(CXXTESTS)" python3 tools/ktest.py cxx $(SMP) $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio
.PHONY: wk-tools posix posix-test cxx cxx-test
