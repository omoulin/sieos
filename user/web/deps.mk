# deps.mk - WebKit's dependencies (docs/webkit-plan.md, phase 4), each built
# from its pinned release (URL and SHA-256 here; the archive fetched once into
# .hosttools/dl, never into git) into libposix's sysroot, static, for this
# processor.
#
#   make webdeps        every library below, in order
#   make webdeps-test   their checks in QEMU (tests/webdeps: our programs and a
#                       few of each library's own tests)
#
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

WB        := $(B)/web
SR        := $(CURDIR)/$(SYSROOT)

# ---- host tools: Meson (its wheel) and pkgconf (built by the host's gcc)
MESON_VER := 1.12.1
MESON_SHA := 930bc7542cbd9f57009e182fd014eba48cf1a0180a7b9006d2d32d8f168d8b02
MESON_URL := https://files.pythonhosted.org/packages/57/99/36cd25f9598eef70db97577440a70a360af505f6f01c2338a18d1556079c/meson-$(MESON_VER)-py3-none-any.whl
PKGCF_VER := 3.0.9
PKGCF_SHA := 6ab2d3b8087dcf500e34003a6a8934dca55d0a3d6066aeafb03c352db3e68bfd
MESON     := $(CURDIR)/$(WKT)/bin/meson
PKGCONF   := $(CURDIR)/$(WKT)/bin/pkg-config
$(WKT)/bin/meson:
	$(call fetch,$(MESON_URL),meson-$(MESON_VER).whl,$(MESON_SHA))
	rm -rf $(WKT)/meson && mkdir -p $(WKT)/bin && python3 -m zipfile -e .hosttools/dl/meson-$(MESON_VER).whl $(WKT)/meson
	printf '#!/usr/bin/env python3\nimport sys\nsys.path.insert(0, "%s")\nfrom mesonbuild.mesonmain import main\nsys.exit(main())\n' \
	    $(CURDIR)/$(WKT)/meson > $@ && chmod +x $@
$(WKT)/bin/pkg-config:
	$(call fetch,https://distfiles.ariadne.space/pkgconf/pkgconf-$(PKGCF_VER).tar.xz,pkgconf-$(PKGCF_VER).tar.xz,$(PKGCF_SHA))
	rm -rf $(WKT)/src/pkgconf-$(PKGCF_VER) && mkdir -p $(WKT)/src && tar xJf .hosttools/dl/pkgconf-$(PKGCF_VER).tar.xz -C $(WKT)/src
	cd $(WKT)/src/pkgconf-$(PKGCF_VER) && ./configure -q --prefix=$(CURDIR)/$(WKT) --disable-shared && $(MAKE) -s && $(MAKE) -s install
	rm -rf $(WKT)/src/pkgconf-$(PKGCF_VER) && ln -sf pkgconf $@

# ---- the cross toolchain: compiler wrappers (clang for this processor, the
# sysroot, compiler-rt's builtins and crtbegin/crtend, static, lld with the
# segment layout SIEOS's loader wants), a CMake toolchain file, a Meson cross file
CLANG     := $(CURDIR)/$(LLVM)/bin/clang
WCFLAGS   := --target=$(TRIPLE) --sysroot=$(SR) -resource-dir=$(CURDIR)/$(WB)/resource -static -rtlib=compiler-rt \
             -unwindlib=libunwind -fuse-ld=lld -Qunused-arguments -ffunction-sections -fdata-sections \
             -Wl,-z,separate-loadable-segments -Wl,-z,max-page-size=4096 -Wl,--gc-sections -Wl,--eh-frame-hdr
$(WB)/toolchain.stamp: $(SYSROOT)/usr/lib/libc++.a $(SYSROOT)/usr/lib/crti.o | $(WKT)/bin/meson $(WKT)/bin/pkg-config $(WKT)/bin/cmake $(WKT)/bin/ninja
	mkdir -p $(WB)/bin $(WB)/resource/lib $(WB)/stamp $(WB)/src $(WB)/build
	ln -sfn $(CURDIR)/$(LLVM)/lib/clang/23/include $(WB)/resource/include
	ln -sfn $(SR)/usr/lib/linux $(WB)/resource/lib/linux
	printf '#!/bin/sh\nexec %s %s "$$@"\n' $(CLANG) "$(WCFLAGS)" > $(WB)/bin/cc
	printf '#!/bin/sh\nexec %s++ %s -stdlib=libc++ "$$@"\n' $(CLANG) "$(WCFLAGS)" > $(WB)/bin/c++
	chmod +x $(WB)/bin/cc $(WB)/bin/c++
	printf 'set(CMAKE_SYSTEM_NAME Linux)\nset(CMAKE_SYSTEM_PROCESSOR %s)\nset(CMAKE_SYSROOT %s)\n' $(MARCH) $(SR) > $(WB)/toolchain.cmake
	printf 'set(CMAKE_C_COMPILER %s/bin/cc)\nset(CMAKE_CXX_COMPILER %s/bin/c++)\n' $(CURDIR)/$(WB) $(CURDIR)/$(WB) >> $(WB)/toolchain.cmake
	printf 'set(CMAKE_AR %s/bin/llvm-ar)\nset(CMAKE_RANLIB %s/bin/llvm-ranlib)\nset(BUILD_SHARED_LIBS OFF)\n' $(CURDIR)/$(LLVM) $(CURDIR)/$(LLVM) >> $(WB)/toolchain.cmake
	printf 'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\nset(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\nset(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\nset(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n' >> $(WB)/toolchain.cmake
	printf "[binaries]\nc = '%s/bin/cc'\ncpp = '%s/bin/c++'\nar = '%s/bin/llvm-ar'\nstrip = '%s/bin/llvm-strip'\npkg-config = '%s'\n" \
	    $(CURDIR)/$(WB) $(CURDIR)/$(WB) $(CURDIR)/$(LLVM) $(CURDIR)/$(LLVM) $(PKGCONF) > $(WB)/cross.txt
	printf "[properties]\nsys_root = '%s'\npkg_config_libdir = '%s/usr/lib/pkgconfig:%s/usr/share/pkgconfig'\nneeds_exe_wrapper = true\n" $(SR) $(SR) $(SR) >> $(WB)/cross.txt
	printf "[host_machine]\nsystem = 'linux'\ncpu_family = '%s'\ncpu = '%s'\nendian = 'little'\n" $(MARCH) $(MARCH) >> $(WB)/cross.txt
	touch $@

# How each build system is run. Libraries are static, small (-Os), their
# programs and documentation left out where the build allows it.
PKGENV    := PATH=$(CURDIR)/$(WKT)/bin:$$PATH PKG_CONFIG=$(PKGCONF) PKG_CONFIG_LIBDIR=$(SR)/usr/lib/pkgconfig:$(SR)/usr/share/pkgconfig PKG_CONFIG_SYSROOT_DIR=$(SR)
CONFIGURE := $(PKGENV) CC=$(CURDIR)/$(WB)/bin/cc CXX=$(CURDIR)/$(WB)/bin/c++ AR=$(CURDIR)/$(LLVM)/bin/llvm-ar \
             RANLIB=$(CURDIR)/$(LLVM)/bin/llvm-ranlib CFLAGS=-Os CXXFLAGS=-Os ./configure --host=$(TRIPLE) --prefix=/usr \
             --disable-shared --enable-static
MESONSET  := $(PKGENV) $(MESON) setup --cross-file $(CURDIR)/$(WB)/cross.txt --prefix=/usr --libdir=lib \
             --buildtype=minsize -Ddefault_library=static -Db_staticpic=false -Db_ndebug=if-release --wrap-mode=nofallback
CMAKESET  := $(CMAKE) -G Ninja -DCMAKE_MAKE_PROGRAM=$(NINJA) -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/$(WB)/toolchain.cmake \
             -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
# $(call unpack,NAME,ARCHIVE): a fresh copy of its sources in $(WB)/src/NAME
unpack = rm -rf $(WB)/src/$(1) && mkdir -p $(WB)/src/$(1) && tar xf .hosttools/dl/$(2) -C $(WB)/src/$(1) --strip-components=1
# (libtool's .la files point at /usr/lib: not wanted in a sysroot)
nola   = find $(SR)/usr/lib -name '*.la' -delete

# ---- zlib
ZLIB_VER  := 1.3.2
ZLIB_SHA  := d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3
$(WB)/stamp/zlib: $(WB)/toolchain.stamp
	$(call fetch,https://zlib.net/zlib-$(ZLIB_VER).tar.xz,zlib-$(ZLIB_VER).tar.xz,$(ZLIB_SHA))
	$(call unpack,zlib,zlib-$(ZLIB_VER).tar.xz)
	cd $(WB)/src/zlib && CC=$(CURDIR)/$(WB)/bin/cc AR=$(CURDIR)/$(LLVM)/bin/llvm-ar CFLAGS=-Os ./configure --static --prefix=/usr >/dev/null
	$(MAKE) -s -C $(WB)/src/zlib libz.a example && $(MAKE) -s -C $(WB)/src/zlib install DESTDIR=$(SR) >/dev/null
	touch $@
$(WB)/wtest-zlib-example: $(WB)/stamp/zlib             # (zlib's own test)
	cp $(WB)/src/zlib/example $@

# ---- libffi (GObject's generic marshaller)
FFI_VER   := 3.8.0
FFI_SHA   := 7da3e2d9a171eb0a038f592ecad3ff2bb2550f3496d87b3b29ad0cf4430c0db4
$(WB)/stamp/libffi: $(WB)/toolchain.stamp
	$(call fetch,https://github.com/libffi/libffi/releases/download/v$(FFI_VER)/libffi-$(FFI_VER).tar.gz,libffi-$(FFI_VER).tar.gz,$(FFI_SHA))
	$(call unpack,libffi,libffi-$(FFI_VER).tar.gz)
	cd $(WB)/src/libffi && $(CONFIGURE) --disable-docs --disable-multi-os-directory --disable-exec-static-tramp >/dev/null
	$(MAKE) -s -C $(WB)/src/libffi && $(MAKE) -s -C $(WB)/src/libffi install DESTDIR=$(SR) >/dev/null && $(nola)
	touch $@

# ---- PCRE2 (GLib's regular expressions), 8-bit, no JIT
PCRE2_VER := 10.49
PCRE2_SHA := 53c156e1ba416a20da8e65395daa132da0d80e76910424caca3fcdae7831d384
$(WB)/stamp/pcre2: $(WB)/toolchain.stamp
	$(call fetch,https://github.com/PCRE2Project/pcre2/releases/download/pcre2-$(PCRE2_VER)/pcre2-$(PCRE2_VER).tar.bz2,pcre2-$(PCRE2_VER).tar.bz2,$(PCRE2_SHA))
	$(call unpack,pcre2,pcre2-$(PCRE2_VER).tar.bz2)
	cd $(WB)/src/pcre2 && $(CONFIGURE) --disable-pcre2grep-libz --disable-pcre2grep-libbz2 --disable-pcre2test-libreadline >/dev/null
	$(MAKE) -s -C $(WB)/src/pcre2 libpcre2-8.la && $(MAKE) -s -C $(WB)/src/pcre2 install-libLTLIBRARIES install-includeHEADERS \
	    install-nodist_includeHEADERS install-pkgconfigDATA DESTDIR=$(SR) >/dev/null && $(nola)
	touch $@

# ---- GLib (GLib, GObject, GIO): WPE's own base
GLIB_VER  := 2.90.1
GLIB_SHA  := 93c941aa17d5eb1d53fe838365f29a8b4e539c222a256d974ec8f30fc413e396
$(WB)/stamp/glib: $(WB)/stamp/zlib $(WB)/stamp/libffi $(WB)/stamp/pcre2
	$(call fetch,https://download.gnome.org/sources/glib/$(basename $(GLIB_VER))/glib-$(GLIB_VER).tar.xz,glib-$(GLIB_VER).tar.xz,$(GLIB_SHA))
	$(call unpack,glib,glib-$(GLIB_VER).tar.xz)
	rm -rf $(WB)/build/glib && cd $(WB)/src/glib && $(MESONSET) $(CURDIR)/$(WB)/build/glib -Dtests=false \
	    -Dintrospection=disabled -Dlibmount=disabled -Dselinux=disabled -Dxattr=false -Dman-pages=disabled \
	    -Dnls=disabled -Ddtrace=disabled -Dsystemtap=disabled -Dsysprof=disabled -Dlibelf=disabled \
	    -Ddocumentation=false -Dbsymbolic_functions=false -Dglib_debug=disabled >/dev/null
	$(PKGENV) $(NINJA) -C $(WB)/build/glib
	$(PKGENV) $(MESON) install -C $(WB)/build/glib --destdir $(SR) --no-rebuild | tail -3
	touch $@

WEBDEPS   := zlib libffi pcre2 glib
webdeps: $(WEBDEPS:%=$(WB)/stamp/%)

# ---- webdeps-test: our programs (tests/webdeps/NAME.c, linked with the
# libraries through pkg-config) and the libraries' own tests, run in QEMU
WTESTS_C  := $(patsubst tests/webdeps/%.c,%,$(wildcard tests/webdeps/*.c))
$(WB)/wtest-%: tests/webdeps/%.c $(WEBDEPS:%=$(WB)/stamp/%)
	$(PKGENV) $(WB)/bin/cc -Os -o $@ $< $$($(PKGENV) $(PKGCONF) --static --cflags --libs $$(sed -n 's,^/\* pkg-config: \(.*\) \*/,\1,p' $<))
webdeps-test: all webdeps $(WTESTS_C:%=$(WB)/wtest-%) $(WB)/wtest-zlib-example
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 512M -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	for f in $(WTESTS_C:%=$(WB)/wtest-%) $(WB)/wtest-zlib-example; do $(B)/host/siefs $(B)/test.img put $$f /bin/$$(basename $$f) && \
	    $(B)/host/siefs $(B)/test.img chmod 755 /bin/$$(basename $$f) || exit 1; done
	WTESTS="$(WTESTS_C:%=wtest-%) wtest-zlib-example" python3 tools/ktest.py webdeps $(SMP) $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio
.PHONY: webdeps webdeps-test
