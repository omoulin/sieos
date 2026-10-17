# jsc.mk - JavaScriptCore alone (docs/webkit-plan.md, phase 5): WebKit's
# JSCOnly port from the pinned WPE WebKit release, built against libposix's
# sysroot (ICU from deps.mk), static, with its `jsc` shell.
#
#   make jsc        $(B)/web/jsc: the shell, stripped
#   make jsc-test   tests/jsc/*.js run by it in QEMU, with the JIT and without
#
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

WK_VER    := 2.54.1
WK_SHA    := 0de8bdfba011f9cd63ba9a9171fa6038dd2c08237894f4082abbb821f29d3fe9
# (with SIEOS's few patches: user/web/patches, each saying why)
WK_PATCHES := $(sort $(wildcard user/web/patches/webkit-*.patch))
$(WB)/stamp/webkit-src: $(WB)/toolchain.stamp $(WK_PATCHES)
	$(call fetch,https://wpewebkit.org/releases/wpewebkit-$(WK_VER).tar.xz,wpewebkit-$(WK_VER).tar.xz,$(WK_SHA))
	$(call unpack,webkit,wpewebkit-$(WK_VER).tar.xz)
	for p in $(WK_PATCHES); do patch -s -p1 -d $(WB)/src/webkit < $$p || exit 1; done
	touch $@

# The JIT, all its tiers (LLInt, baseline, DFG, FTL on B3: upstream's Linux
# default; without the FTL this release does not build); WebKit's own malloc
# (bmalloc/libpas: 25% faster than musl's on bench.js); no sampling profiler
# (it stops threads with signals), inspector, API tests. -Os: as fast as -O2
# here (the JIT's code is the same), 4.8 MB smaller. WK_FLAGS (webkit.mk):
# libpas wants 16-byte atomics inline (x86-64: cmpxchg16b)
WK_FLAGS  := -Os -DNDEBUG $(if $(filter x86_64,$(ARCH)),-mcx16)
JSC_OPTS  := -DPORT=JSCOnly -DENABLE_STATIC_JSC=ON -DEVENT_LOOP_TYPE=Generic -DDEVELOPER_MODE=OFF \
             -DENABLE_API_TESTS=OFF -DUSE_SYSTEM_MALLOC=OFF -DENABLE_SAMPLING_PROFILER=OFF \
             -DENABLE_REMOTE_INSPECTOR=OFF -DUSE_LIBBACKTRACE=OFF -DENABLE_FUZZILLI=OFF -DUSE_HEADER_MAPS=OFF \
             -DCMAKE_C_FLAGS_RELEASE="$(WK_FLAGS)" -DCMAKE_CXX_FLAGS_RELEASE="$(WK_FLAGS)"
JSC_JOBS  ?= 16
$(WB)/jsc: $(WB)/stamp/webkit-src $(WB)/stamp/icu $(SYSROOT)/usr/lib/libc.a user/web/jsc.mk
	cd $(WB)/src/webkit && $(PKGENV) $(CMAKE) -G Ninja -DCMAKE_MAKE_PROGRAM=$(NINJA) -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/$(WB)/toolchain.cmake \
	    -DCMAKE_BUILD_TYPE=Release -S . -B $(CURDIR)/$(WB)/build/jsc $(JSC_OPTS) >$(CURDIR)/$(WB)/build/jsc.log
	rm -f $(WB)/build/jsc/bin/jsc && $(PKGENV) $(NINJA) -C $(WB)/build/jsc -j$(JSC_JOBS) jsc   # (relinked: libposix may have changed)
	$(LLVM)/bin/llvm-strip -o $@ $(WB)/build/jsc/bin/jsc
jsc: $(WB)/jsc

JSC_TESTS := $(wildcard tests/jsc/*.js)
jsc-test: all $(WB)/jsc $(JSC_TESTS)
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 256M -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	$(B)/host/siefs $(B)/test.img put $(WB)/jsc /bin/jsc && $(B)/host/siefs $(B)/test.img chmod 755 /bin/jsc
	$(B)/host/siefs $(B)/test.img mkdir /lib/jsc && for f in $(JSC_TESTS); do \
	    $(B)/host/siefs $(B)/test.img put $$f /lib/jsc/$$(basename $$f) || exit 1; done
	JSTESTS="$(notdir $(JSC_TESTS))" python3 tools/ktest.py jsc $(SMP) $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio
.PHONY: jsc jsc-test
