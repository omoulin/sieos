# ui.mk - SIEOS's UI toolkit (docs/ui.md) and its tests.
#
#   make ui-test    QEMU: other programs' windows in their own colours in
#                   every look (tests/ui/uipatch.c), the toolkit's widgets
#
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

# uipatch (tests/ui/uipatch.c): exact colour patches in a window of the desktop
$(B)/uipatch.elf: $(B)/user/tests/uipatch.o $(UCORE:%=$(B)/user/lib/%.o) $(ULIB) user/user.ld | $(TOOLDEP)
	$(TLD) -T user/user.ld -z max-page-size=4096 -o $@ $(UCORE:%=$(B)/user/lib/%.o) $< $(ULIB)
$(B)/user/tests/%.o: tests/ui/%.c user/lib/mk.h include/mk/*.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(UCFLAGS) -c $< -o $@

UI_TESTBINS := uipatch
ui-test: all $(UI_TESTBINS:%=$(B)/%.elf)
	rm -f $(B)/test.img && $(B)/host/mkfs.siefs -s $(TESTDISK) -L SIEOS -d $(B)/rootfs $(B)/test.img >/dev/null
	for p in $(UI_TESTBINS); do $(B)/host/siefs $(B)/test.img put $(B)/$$p.elf /bin/$$p && $(B)/host/siefs $(B)/test.img chmod 755 /bin/$$p || exit 1; done
	python3 tools/uitest.py $(SMP) $(B)/screens-ui $(B)/test.img $(subst $(B)/disk.img,$(B)/test.img,$(QEMU)) $(SCREEN) -display none -serial stdio
.PHONY: ui-test
