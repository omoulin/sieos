# make-test (tools/maketest.py): a recipe that fails between two that work.
# Without -k make stops at "bad"; with -k it makes ok2 too, then says so.
#
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

all: ok1 bad ok2

ok1 ok2:
	@echo $@ ran

bad:
	cc nosuch.c -o nosuch

ignored:
	-cc nosuch.c -o nosuch
	@echo after the ignored error

.PHONY: all ok1 ok2 bad ignored
