# ui.mk - SIEOS's UI toolkit (docs/ui.md) and its tests.
#
#   make ui-test    QEMU: other programs' windows in their own colours
#                   (tests/ui/uipatch.c), the toolkit's widgets
#
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

UI_SRC   := ui ui_wid ui_menu ui_sys
UI_HDRS  := include/sieos/ui.h user/lib/ui/ui_int.h include/mk/atlas.h
UI_FONTS := -Dfont15=ui_font15 -Dfont=ui_font       # (atlas's fonts, under names of the toolkit's own)

# ---- libui.a: for programs built with user/lib (SIEOS's own compiler and library)
UI_LIB   := $(B)/user/libui.a
UI_OBJ   := $(UI_SRC:%=$(B)/ui/%.o) $(B)/ui/font15.o $(B)/ui/font5.o
$(UI_LIB): $(UI_OBJ) | $(TOOLDEP)
	rm -f $@ && $(TAR) rcs $@ $^
$(B)/ui/%.o: user/lib/ui/%.c $(UI_HDRS) user/lib/mk.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) -Os -c $< -o $@
$(B)/ui/font15.o: user/atlas/font15.c $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) $(UI_FONTS) -c $< -o $@
$(B)/ui/font5.o: user/atlas/font.c $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) $(UI_FONTS) -c $< -o $@

# a program on the toolkit: $(B)/NAME.elf from user/NAME/NAME.c
define ui_prog
$(B)/$(1).elf: $(B)/user/$(1).o $(UI_LIB) $(UCORE:%=$(B)/user/lib/%.o) $(ULIB) user/user.ld | $(TOOLDEP)
	$(TLD) -T user/user.ld -z max-page-size=4096 -o $$@ $(UCORE:%=$(B)/user/lib/%.o) $$< $(UI_LIB) $(ULIB)
$(B)/user/$(1).o: include/sieos/ui.h
endef
$(eval $(call ui_prog,uidemo))

# ---- libui-posix.a: the same for libposix programs (C or C++: the web browser)
UI_PX_OBJ := $(UI_SRC:%=$(WB)/ui/%.o) $(WB)/ui/font15.o $(WB)/ui/font5.o
$(WB)/libui-posix.a: $(UI_PX_OBJ)
	rm -f $@ && $(LLVM)/bin/llvm-ar rcs $@ $^
$(WB)/ui/%.o: user/lib/ui/%.c $(UI_HDRS) | $(WB)/toolchain.stamp
	@mkdir -p $(@D)
	$(WB)/bin/cc -Os -Wall -Wextra -DUI_POSIX -Iinclude -c $< -o $@
$(WB)/ui/font15.o: user/atlas/font15.c | $(WB)/toolchain.stamp
	@mkdir -p $(@D)
	$(WB)/bin/cc -Os $(UI_FONTS) -c $< -o $@
$(WB)/ui/font5.o: user/atlas/font.c | $(WB)/toolchain.stamp
	@mkdir -p $(@D)
	$(WB)/bin/cc -Os $(UI_FONTS) -c $< -o $@
# uicxx (tests/ui/uicxx.cpp): the toolkit from C++, a libposix program
$(WB)/uicxx: tests/ui/uicxx.cpp include/sieos/ui.h $(WB)/libui-posix.a | $(WB)/toolchain.stamp
	$(WB)/bin/c++ -Os -Iinclude -o $@ $< $(WB)/libui-posix.a

# uipatch (tests/ui/uipatch.c): exact colour patches in a window of the desktop
$(B)/uipatch.elf: $(B)/user/tests/uipatch.o $(UCORE:%=$(B)/user/lib/%.o) $(ULIB) user/user.ld | $(TOOLDEP)
	$(TLD) -T user/user.ld -z max-page-size=4096 -o $@ $(UCORE:%=$(B)/user/lib/%.o) $< $(ULIB)
$(B)/user/tests/%.o: tests/ui/%.c user/lib/mk.h include/mk/*.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) -c $< -o $@

UI_TESTBINS := uipatch uidemo
ui-test: all $(UI_TESTBINS:%=$(B)/%.elf)
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s $(TESTDISK) -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	for p in $(UI_TESTBINS); do $(B)/host/siefs $(B)/test.img put $(B)/$$p.elf /bin/$$p && $(B)/host/siefs $(B)/test.img chmod 755 /bin/$$p || exit 1; done
	python3 tools/uitest.py $(SMP) $(B)/screens-ui $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) $(SCREEN) -display none -serial stdio
.PHONY: ui-test
