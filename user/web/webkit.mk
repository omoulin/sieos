# webkit.mk - WPE WebKit itself (docs/webkit-plan.md, phase 6): the WPE port
# from the release jsc.mk unpacks, every optional feature off, static
# (no dynamic linking on SIEOS), on WPEPlatform's headless backend first.
#
#   make webkit       libWPEWebKit, WPEPlatform, the WebProcess and NetworkProcess
#   make web-test     pages rendered headless in QEMU, compared with references
#
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

WK_OFF    := WPE_LEGACY_API WPE_PLATFORM_DRM WPE_PLATFORM_WAYLAND WPE_QT_API VIDEO WEB_AUDIO MEDIA_STREAM \
             WEB_RTC WEB_CODECS MEDIA_RECORDER MEDIA_SESSION ENCRYPTED_MEDIA WEBGL WEBXR GAMEPAD SPELLCHECK \
             SPEECH_SYNTHESIS GEOLOCATION NOTIFICATIONS WEBDRIVER XSLT PDFJS GPU_PROCESS BUBBLEWRAP_SANDBOX \
             JOURNALD_LOG INTROSPECTION DOCUMENTATION MINIBROWSER API_TESTS LAYOUT_TESTS MHTML \
             CONTENT_EXTENSIONS SAMPLING_PROFILER REMOTE_INSPECTOR WEB_PAGE_SPATIAL_BACKDROP COG
WK_USE_OFF := GBM LIBDRM ATK AVIF JPEGXL LCMS WOFF2 LIBHYPHEN LIBBACKTRACE SYSPROF_CAPTURE SKIA_OPENTYPE_SVG \
             FLITE VULKAN HEADER_MAPS GSTREAMER SYSTEM_SYSPROF_CAPTURE
WK_OPTS   := -DPORT=WPE -DWEBKIT_STATIC_LIBRARY=ON -DDEVELOPER_MODE=OFF -DENABLE_WPE_PLATFORM=ON \
             -DENABLE_WPE_PLATFORM_HEADLESS=ON -DUSE_SYSTEM_MALLOC=$(WK_SYSMALLOC) -DENABLE_JIT=ON \
             $(WK_OFF:%=-DENABLE_%=OFF) $(WK_USE_OFF:%=-DUSE_%=OFF) \
             -DCMAKE_C_FLAGS_MINSIZEREL="$(WK_FLAGS)" -DCMAKE_CXX_FLAGS_MINSIZEREL="$(WK_FLAGS)" -DJSC_CXX_FLAGS="$(JSC_CXX_FLAGS)" \
             -DCMAKE_C_FLAGS="-D_caches=_wk_xdg_caches -D_xdg_binary_or_text_fallback=_wk_xdg_binary_or_text_fallback"
# (WebKit's copy of xdgmime and GIO's share two global names its prefix misses:
#  linked statically together, WebKit's are renamed)
# Static linking: every program takes all the libraries' own dependencies too
# (lld finds what each needs, in any order); GIO's TLS module comes with them
WK_PKGS   := gio-2.0 gio-unix-2.0 gmodule-2.0 libsoup-3.0 gio-tls-sieos libgcrypt libtasn1 harfbuzz-icu fontconfig \
             freetype2 libpng libjpeg libwebp libwebpdemux sqlite3 libxml-2.0 epoxy xkbcommon icu-uc icu-i18n zlib
WK_JOBS   ?= 16
WK_DEPS   := $(WEBDEPS:%=$(WB)/stamp/%)
# unifdef, run on the host (WebKit's own copy would be built for SIEOS)
$(WKT)/bin/unifdef: $(WB)/stamp/webkit-src
	gcc -O2 -w -o $@ $(WB)/src/webkit/Source/ThirdParty/unifdef/unifdef.c
$(WB)/stamp/webkit: $(WB)/stamp/webkit-src $(WK_DEPS) $(SYSROOT)/usr/lib/libc.a user/web/webkit.mk user/web/jsc.mk | $(WKT)/bin/unifdef
	cd $(WB)/src/webkit && $(PKGENV) $(CMAKE) -G Ninja -DCMAKE_MAKE_PROGRAM=$(NINJA) -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/$(WB)/toolchain.cmake \
	    -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX=/usr -S . -B $(CURDIR)/$(WB)/build/webkit $(WK_OPTS) \
	    -DCMAKE_CXX_STANDARD_LIBRARIES="$$($(PKGENV) $(PKGCONF) --static --libs $(WK_PKGS))" >$(CURDIR)/$(WB)/build/webkit.log
	rm -f $(WB)/build/webkit/bin/WPEWebProcess $(WB)/build/webkit/bin/WPENetworkProcess   # (relinked: libposix may have changed)
	$(PKGENV) $(NINJA) -C $(WB)/build/webkit -j$(WK_JOBS) WPEWebProcess WPENetworkProcess   # (no injected bundle: no dlopen)
	touch $@
webkit: $(WB)/stamp/webkit
.PHONY: webkit

# ---- Programs using WebKit's API, linked as WebKit links its own WebProcess
# (its static libraries and objects). Each is also WebKit's WebProcess and
# NetworkProcess (user/web/multicall.cpp): one program, whose code every
# process started from it shares; their names in /usr/libexec/wpe-webkit-2.0
# are links to it.
WKB       := $(WB)/build/webkit
WK_INC    := $(addprefix -I$(WKB)/,DerivedSources/ForwardingHeaders/wpe DerivedSources/WebKit DerivedSources/ForwardingHeaders/wpe-jsc \
             JavaScriptCoreGLib/Headers JavaScriptCoreGLib/DerivedSources DerivedSources/WPEPlatform) \
             -I$(WB)/src/webkit/Source/WebKit/WPEPlatform    # (as wpe-webkit-2.0-uninstalled.pc says)
WK_LINK    = $$(awk '/^build bin\/WPEWebProcess:/{f=1} f&&/^  LINK_LIBRARIES =/{sub(/^  LINK_LIBRARIES = /,""); print; exit}' $(CURDIR)/$(WKB)/build.ninja)
WK_CC      = $(PKGENV) $(WB)/bin/cc -Os -c -Iuser/web -Iinclude $(WK_INC) $$($(PKGENV) $(PKGCONF) --cflags gio-2.0 libsoup-3.0 libpng)
define wk_program  # $(1): the program, $(2): its objects
	$(PKGENV) $(WB)/bin/c++ -Os -c -o $(WB)/multicall.o user/web/multicall.cpp
	cd $(WKB) && $(PKGENV) $(CURDIR)/$(WB)/bin/c++ -o $(CURDIR)/$(1) $(foreach o,$(2) $(WB)/multicall.o,$(CURDIR)/$(o)) $(WK_LINK) \
	    $$($(PKGENV) $(PKGCONF) --static --libs $(WK_PKGS))
	$(LLVM)/bin/llvm-strip -o $(1).strip $(1)
endef
$(WB)/web-render: tests/web/render.c user/web/multicall.cpp $(WB)/stamp/webkit
	$(WK_CC) -o $@.o $<
	$(call wk_program,$@,$@.o)
# /bin/web, the browser (user/web/browser.c) on SIEOS's WPE platform
# (user/web/platform: windows of the desktop, mk/atlas.h)
$(WB)/web: user/web/browser.c user/web/platform/sieos-platform.c user/web/platform/sieos-platform.h include/mk/atlas.h \
           user/web/multicall.cpp $(WB)/stamp/webkit
	$(WK_CC) -o $(WB)/browser.o user/web/browser.c
	$(WK_CC) -o $(WB)/sieos-platform.o user/web/platform/sieos-platform.c
	$(call wk_program,$@,$(WB)/browser.o $(WB)/sieos-platform.o)
# atlaswin (tests/web/atlaswin.c): a window of the desktop from a libposix program
$(WB)/atlaswin: tests/web/atlaswin.c include/mk/atlas.h $(SYSROOT)/usr/lib/libc.a | $(WB)/toolchain.stamp
	$(WB)/bin/cc -Os -Iinclude -o $@ $<

# ---- What a disk needs for WebKit: $(call web_base,IMAGE,PROGRAM): the
# program in /bin, WebKit's process names linked to it (symbolic links: the
# program's image is shared by file), the fonts, their
# configuration, the MIME types of local files
WK_EXEC   := /usr/libexec/wpe-webkit-2.0
define web_base
	for d in /usr /usr/libexec $(WK_EXEC) /usr/share /usr/share/mime /lib/web /lib/fonts /etc/fonts; do \
	    $(B)/host/siefs $(1) mkdir $$d 2>/dev/null; done; true
	$(B)/host/siefs $(1) put $(WB)/$(2).strip /bin/$(2) && $(B)/host/siefs $(1) chmod 755 /bin/$(2)
	for p in WPEWebProcess WPENetworkProcess; do $(B)/host/siefs $(1) rm $(WK_EXEC)/$$p 2>/dev/null; \
	    $(B)/host/siefs $(1) symlink /bin/$(2) $(WK_EXEC)/$$p || exit 1; done
	for f in $(WB)/fonts/*.ttf; do $(B)/host/siefs $(1) put $$f /lib/fonts/$$(basename $$f) || exit 1; done
	$(B)/host/siefs $(1) put user/web/fonts.conf /etc/fonts/fonts.conf && $(B)/host/siefs $(1) put user/web/mime-globs2 /usr/share/mime/globs2
endef
WEB_BASE  := $(WB)/fonts/DejaVuSans.ttf user/web/fonts.conf user/web/mime-globs2

# ---- web-test: pages from tests/web/pages rendered by web-render in QEMU,
# their pixels checked
WEB_RUNS  := web-render file:///lib/web/basic.html /tmp/basic.png 50,50=ff0000 150,50=00ff00 250,50=0000ff 25,125=ffff00 \
             75,175=00ffff 75,125=ffffff 350,50=ff00ff 350,5=000000 301,1=ffffff 32,332=ff8000 700,500=ffffff ;; \
             web-render file:///lib/web/script.html /tmp/script.png 75,75=008000 225,75=800080 150,160=~111111 \
             150,295=~f8f8f8 595,18=e0e0ff 595,6=ffffff 50,450=00ff00 ;; \
             web-render https://10.0.2.2:$$TLS/page /tmp/tls.png 500,500=123456
web-test: all $(WB)/web-render $(WEB_BASE) $(wildcard tests/web/pages/*)
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 1G -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	$(call web_base,$(B)/test.img,web-render)
	for f in tests/web/pages/*; do $(B)/host/siefs $(B)/test.img put $$f /lib/web/$$(basename $$f) || exit 1; done
	KRUN='$(WEB_RUNS)' python3 tools/ktest.py web $(SMP) $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio
.PHONY: web-test

# ---- browser-test (tools/browsertest.py): the desktop with the browser in
# QEMU: another program's window (atlaswin), then /bin/web: pages, typing,
# back, HTTPS
WEB_DATA  := user/web/toolbar.html user/web/start.html
browser-test: all $(WB)/web $(WB)/atlaswin $(WEB_BASE) $(WEB_DATA) $(wildcard tests/web/pages/*)
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 1G -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	$(call web_base,$(B)/test.img,web)
	for f in $(WEB_DATA) tests/web/pages/*; do $(B)/host/siefs $(B)/test.img put $$f /lib/web/$$(basename $$f) || exit 1; done
	$(B)/host/siefs $(B)/test.img put $(WB)/atlaswin /bin/atlaswin && $(B)/host/siefs $(B)/test.img chmod 755 /bin/atlaswin
	python3 tools/browsertest.py $(SMP) $(B)/screens-web $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) $(SCREEN) -display none -serial stdio
.PHONY: browser-test

# ---- web-disk: the browser onto $(B)/disk.img (make first; never while QEMU
# runs on it): /bin/web, WebKit's process names, fonts, the toolbar, Web.app
web-disk: $(B)/disk.img $(WB)/web $(WEB_BASE) $(WEB_DATA) user/web/web.app
	$(call web_base,$(B)/disk.img,web)
	for f in $(WEB_DATA); do $(B)/host/siefs $(B)/disk.img put $$f /lib/web/$$(basename $$f) || exit 1; done
	$(B)/host/siefs $(B)/disk.img put user/web/web.app /etc/apps/web.app
.PHONY: web-disk

# ---- WEB=1: the browser in the system's files ($(B)/rootfs), so that the
# disks made from them have it (make WEB=1 newdisk, usb, sdcard): /bin/web,
# WebKit's process names linked to it, the fonts, the toolbar, Web.app
# ($(B)/web.flag, rewritten when WEB changes, remakes the files then)
WEB_ROOTFS_DEPS = $(if $(WEB),$(WB)/web $(WEB_BASE) $(WEB_DATA) user/web/web.app) $(B)/web.flag
$(shell mkdir -p $(B) && [ "`cat $(B)/web.flag 2>/dev/null`" = "web=$(WEB)" ] || echo "web=$(WEB)" > $(B)/web.flag)
define WEB_ROOTFS
	mkdir -p $(B)/rootfs$(WK_EXEC) $(B)/rootfs/lib/web $(B)/rootfs/lib/fonts $(B)/rootfs/etc/fonts $(B)/rootfs/usr/share/mime $(B)/rootfs/etc/apps
	cp $(WB)/web.strip $(B)/rootfs/bin/web && for p in WPEWebProcess WPENetworkProcess; do ln -sf /bin/web $(B)/rootfs$(WK_EXEC)/$$p; done
	cp $(WB)/fonts/*.ttf $(B)/rootfs/lib/fonts/ && cp $(WEB_DATA) $(B)/rootfs/lib/web/ && cp user/web/fonts.conf $(B)/rootfs/etc/fonts/
	cp user/web/mime-globs2 $(B)/rootfs/usr/share/mime/globs2 && cp user/web/web.app $(B)/rootfs/etc/apps/
endef
