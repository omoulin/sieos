# Git, the package facet-git: built by tools/pkgbuild.py (recipe
# ports/pkgs/facet-git) with the cross toolchain, from this folder and the
# SDK (and the desktop applications' helpers, TREE/user/facet-apps).
TREE   ?= ../..
CC     ?= x86_64-pc-sieos-gcc
PREFIX ?= /usr/pkg
CFLAGS ?= -O2
FLAGS   = -std=gnu11 -Wall -Wextra -Wno-format-truncation -I$(TREE)/user/facet-apps

all: facet-git

facet-git: git.c $(TREE)/user/facet-apps/common.c
	$(CC) $(CFLAGS) $(FLAGS) -o $@ git.c $(TREE)/user/facet-apps/common.c -lfacet

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/facet/apps
	cp facet-git $(DESTDIR)$(PREFIX)/bin/
	cp share/facet/apps/git.app $(DESTDIR)$(PREFIX)/share/facet/apps/

clean:
	rm -f facet-git

.PHONY: all install clean
