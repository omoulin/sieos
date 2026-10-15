# SIEOS's own programs, built on SIEOS by make and cc (docs/make.md): each
# is one C file and SIEOS's library (/lib/sicc). The sources here are
# copies of user/NAME/NAME.c, made when the disk is.
#
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

CC     = cc
CFLAGS = -O2
PROGS  = hello sh make

all: $(PROGS)

# a program from its C file (the built-in rule, written out)
%: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(PROGS)

.PHONY: all clean
