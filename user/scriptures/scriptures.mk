# scriptures.mk - /bin/scriptures (docs/scriptures.md): Scriptures, the app
# to read the scriptures offline, in a window on the desktop. Its portable
# core (util json html store layout draw ui library app panels notesview,
# font_tt + stb_truetype) is the upstream app's, as it is; the SIEOS parts:
# plat_sieos.c (an atlas window), net_sieos.c (user/lib's HTTPS), libc.c and
# include/ (the C library the core asks for). Built by the Makefile's
# compiler (gcc on x86-64, sicc on AArch64) with floating point (the font
# rasterizer, JSON numbers). Its fonts: DejaVu Sans Mono and its bold, in
# /lib/fonts, from the DejaVu release the web browser's build also uses.
#
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

BINS     += scriptures
SCR_CORE := util json html store layout draw ui library app panels notesview font_tt
SCR_OBJ  := $(SCR_CORE:%=$(B)/scriptures/%.o) $(addprefix $(B)/scriptures/,plat_sieos.o net_sieos.o debug_sieos.o libc.o) $(B)/user/cc/libc-fmt.o
SCR_CF   := $(filter-out -mgeneral-regs-only,$(UCFLAGS)) -fno-math-errno -Iuser/scriptures/include
$(B)/scriptures.elf: $(SCR_OBJ) $(UCORE:%=$(B)/user/lib/%.o) $(ULIB) user/user.ld | $(TOOLDEP)
	$(TLD) -T user/user.ld -z max-page-size=4096 -o $@ $(UCORE:%=$(B)/user/lib/%.o) $(SCR_OBJ) $(ULIB)
$(B)/scriptures/%.o: user/scriptures/%.c user/scriptures/*.h user/scriptures/include/*.h user/lib/mk.h user/lib/net.h include/mk/*.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(SCR_CF) -c $< -o $@
$(B)/scriptures/app.o: SCR_CF += -Dmain=scriptures_main
$(B)/scriptures/font_tt.o: SCR_CF += -Wno-unused-function -Os    # (stb_truetype: small; glyphs are cached)

# the fonts, into the disk's /lib/fonts (the Makefile's rootfs)
SCR_FONTS := $(addprefix $(B)/scriptures/fonts/,DejaVuSansMono.ttf DejaVuSansMono-Bold.ttf)
$(SCR_FONTS) &: user/web/deps.mk
	$(call fetch,https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_$(subst .,_,$(DJV_VER))/dejavu-fonts-ttf-$(DJV_VER).tar.bz2,dejavu-fonts-ttf-$(DJV_VER).tar.bz2,$(DJV_SHA))
	mkdir -p $(B)/scriptures/fonts && tar xjf .hosttools/dl/dejavu-fonts-ttf-$(DJV_VER).tar.bz2 -C $(B)/scriptures/fonts --strip-components=2 \
	    $(patsubst %,dejavu-fonts-ttf-$(DJV_VER)/ttf/%,$(notdir $(SCR_FONTS))) && touch $(SCR_FONTS)
ROOTFS_FONTS += $(SCR_FONTS)

# scriptures-test (tools/scripturestest.py): the app on the desktop in QEMU,
# its library from a stand-in for the content API on the host (HTTPS, a test CA)
scriptures-test: all $(B)/host/fsck.siefs
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s 128M -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	python3 tools/scripturestest.py $(SMP) $(B)/screens-scriptures $(B)/test.img $(B)/host $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) $(SCREEN) -display none -serial stdio
.PHONY: scriptures-test
