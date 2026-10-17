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
             -DENABLE_WPE_PLATFORM_HEADLESS=ON -DUSE_SYSTEM_MALLOC=ON -DENABLE_JIT=ON \
             $(WK_OFF:%=-DENABLE_%=OFF) $(WK_USE_OFF:%=-DUSE_%=OFF) \
             -DCMAKE_C_FLAGS_MINSIZEREL="-Os -DNDEBUG" -DCMAKE_CXX_FLAGS_MINSIZEREL="-Os -DNDEBUG" \
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
$(WB)/stamp/webkit: $(WB)/stamp/webkit-src $(WK_DEPS) $(SYSROOT)/usr/lib/libc.a user/web/webkit.mk | $(WKT)/bin/unifdef
	cd $(WB)/src/webkit && $(PKGENV) $(CMAKE) -G Ninja -DCMAKE_MAKE_PROGRAM=$(NINJA) -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/$(WB)/toolchain.cmake \
	    -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX=/usr -S . -B $(CURDIR)/$(WB)/build/webkit $(WK_OPTS) \
	    -DCMAKE_CXX_STANDARD_LIBRARIES="$$($(PKGENV) $(PKGCONF) --static --libs $(WK_PKGS))" >$(CURDIR)/$(WB)/build/webkit.log
	rm -f $(WB)/build/webkit/bin/WPEWebProcess $(WB)/build/webkit/bin/WPENetworkProcess   # (relinked: libposix may have changed)
	$(PKGENV) $(NINJA) -C $(WB)/build/webkit -j$(WK_JOBS) WPEWebProcess WPENetworkProcess   # (no injected bundle: no dlopen)
	touch $@
webkit: $(WB)/stamp/webkit
.PHONY: webkit

# ---- web-render (tests/web/render.c): a program using WebKit's API, linked
# as WebKit links its own WebProcess (its static libraries and objects)
WKB       := $(WB)/build/webkit
WK_INC    := $(addprefix -I$(WKB)/,DerivedSources/ForwardingHeaders/wpe DerivedSources/WebKit DerivedSources/ForwardingHeaders/wpe-jsc \
             JavaScriptCoreGLib/Headers JavaScriptCoreGLib/DerivedSources DerivedSources/WPEPlatform) \
             -I$(WB)/src/webkit/Source/WebKit/WPEPlatform    # (as wpe-webkit-2.0-uninstalled.pc says)
WK_LINK    = $$(awk '/^build bin\/WPEWebProcess:/{f=1} f&&/^  LINK_LIBRARIES =/{sub(/^  LINK_LIBRARIES = /,""); print; exit}' $(CURDIR)/$(WKB)/build.ninja)
$(WB)/web-render: tests/web/render.c $(WB)/stamp/webkit
	$(PKGENV) $(WB)/bin/cc -Os -c -o $@.o $< $(WK_INC) $$($(PKGENV) $(PKGCONF) --cflags gio-2.0 libsoup-3.0 libpng)
	cd $(WKB) && $(PKGENV) $(CURDIR)/$(WB)/bin/c++ -o $(CURDIR)/$@ $(CURDIR)/$@.o $(WK_LINK) $$($(PKGENV) $(PKGCONF) --static --libs $(WK_PKGS))

# ---- web-test: pages from tests/web/pages rendered by web-render in QEMU,
# their pixels checked; the WebProcess and NetworkProcess where WebKit looks
# for them (/usr/libexec/wpe-webkit-2.0), the fonts in /lib/fonts
WK_EXEC   := /usr/libexec/wpe-webkit-2.0
WEB_RUNS  := web-render file:///lib/web/basic.html /tmp/basic.png 50,50=ff0000 150,50=00ff00 250,50=0000ff 25,125=ffff00 \
             75,175=00ffff 75,125=ffffff 350,50=ff00ff 350,5=000000 301,1=ffffff 32,332=ff8000 700,500=ffffff ;; \
             web-render file:///lib/web/script.html /tmp/script.png 75,75=008000 225,75=800080 150,160=~111111 \
             150,295=~f8f8f8 595,18=e0e0ff 595,6=ffffff 50,450=00ff00 ;; \
             web-render https://10.0.2.2:$$TLS/page /tmp/tls.png 500,500=123456
web-test: all $(WB)/stamp/webkit $(WB)/web-render $(WB)/fonts/DejaVuSans.ttf user/web/fonts.conf user/web/mime-globs2 $(wildcard tests/web/pages/*)
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 1G -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	for f in web-render build/webkit/bin/WPEWebProcess build/webkit/bin/WPENetworkProcess; do \
	    $(LLVM)/bin/llvm-strip -o $(WB)/$$(basename $$f).strip $(WB)/$$f || exit 1; done
	for d in /usr /usr/libexec $(WK_EXEC) /usr/share /usr/share/mime /lib/web /lib/fonts /etc/fonts; do \
	    $(B)/host/siefs $(B)/test.img mkdir $$d 2>/dev/null; done; true
	$(B)/host/siefs $(B)/test.img put user/web/fonts.conf /etc/fonts/fonts.conf
	$(B)/host/siefs $(B)/test.img put user/web/mime-globs2 /usr/share/mime/globs2
	$(B)/host/siefs $(B)/test.img put $(WB)/web-render.strip /bin/web-render && $(B)/host/siefs $(B)/test.img chmod 755 /bin/web-render
	for p in WPEWebProcess WPENetworkProcess; do $(B)/host/siefs $(B)/test.img put $(WB)/$$p.strip $(WK_EXEC)/$$p && \
	    $(B)/host/siefs $(B)/test.img chmod 755 $(WK_EXEC)/$$p || exit 1; done
	for f in $(WB)/fonts/*.ttf; do $(B)/host/siefs $(B)/test.img put $$f /lib/fonts/$$(basename $$f) || exit 1; done
	for f in tests/web/pages/*; do $(B)/host/siefs $(B)/test.img put $$f /lib/web/$$(basename $$f) || exit 1; done
	KRUN='$(WEB_RUNS)' python3 tools/ktest.py web $(SMP) $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) -display none -vga none -serial stdio
.PHONY: web-test
