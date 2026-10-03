# liblxcompat: Linux's interfaces for Chromium, made from SIEOS's (see
# ../../../docs/chromium.md).  Built with the SIEOS cross compiler:
#   make CC=x86_64-pc-sieos-gcc            liblxcompat.a
#   make CC=... test                       tests/lxtest (run on SIEOS)
# A program takes the layer whole, ahead of the C library:
#   -Wl,--whole-archive liblxcompat.a -Wl,--no-whole-archive
# and compiles with -include lxcompat.h -Ipath/to/include.
#
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.

CC      ?= x86_64-pc-sieos-gcc
AR      ?= x86_64-pc-sieos-ar
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -fPIC -Wall -Wextra -Wno-unused-parameter -Iinclude
SRCS    := src/procfiles.c src/files.c src/misc.c src/inotify.c
OBJS    := $(SRCS:.c=.o)

all: liblxcompat.a

src/%.o: src/%.c src/lx.h
	$(CC) $(CFLAGS) -c -o $@ $<

liblxcompat.a: $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

test: tests/lxtest

tests/lxtest: tests/lxtest.c liblxcompat.a
	$(CC) $(CFLAGS) -include include/lxcompat.h -o $@ $< \
		-Wl,--whole-archive liblxcompat.a -Wl,--no-whole-archive -lpthread

clean:
	rm -f $(OBJS) liblxcompat.a tests/lxtest

.PHONY: all test clean
