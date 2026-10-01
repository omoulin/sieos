# MiR (Make it Real), the package mir: built by tools/pkgbuild.py (recipe
# ports/pkgs/mir) with the cross toolchain, from this folder and the SDK.
# It needs libsia's own header (TREE/user/libsia: the agent's tools are
# libsia tools) and the desktop applications' helpers (TREE/user/facet-apps).
TREE    ?= ../..
CC      ?= x86_64-pc-sieos-gcc
PREFIX  ?= /usr/pkg
CFLAGS  ?= -O2
FLAGS    = -std=gnu11 -Wall -Wextra -Wno-format-truncation -I. -I$(TREE)/user/libsia -I$(TREE)/user/facet-apps
SIA_LIBS = -lsia -ltls -lsieos

all: facet-mir mir-agent

facet-mir: mir.c mir.h $(TREE)/user/facet-apps/common.c
	$(CC) $(CFLAGS) $(FLAGS) -o $@ mir.c $(TREE)/user/facet-apps/common.c -lfacet -Wl,-Bstatic $(SIA_LIBS) -Wl,-Bdynamic

mir-agent: mir-agent.c apptools.c mir.h
	$(CC) $(CFLAGS) $(FLAGS) -o $@ mir-agent.c apptools.c -Wl,-Bstatic $(SIA_LIBS) -Wl,-Bdynamic

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/libexec $(DESTDIR)$(PREFIX)/share/mir
	cp facet-mir $(DESTDIR)$(PREFIX)/bin/
	cp mir-agent $(DESTDIR)$(PREFIX)/libexec/
	cp -r share/. $(DESTDIR)$(PREFIX)/share/mir/

clean:
	rm -f facet-mir mir-agent

.PHONY: all install clean
