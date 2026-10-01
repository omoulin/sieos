# sia-brain, the package: the local model (built by the main Makefile:
# build/brain/sia-brain.gguf), its licence and notice, and the command that
# starts its server.  Built by tools/pkgbuild.py (recipe ports/pkgs/sia-brain).
TREE   ?= ../..
PREFIX ?= /usr/pkg

all:
	test -f $(TREE)/build/brain/sia-brain.gguf

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/sia-brain
	cp sia-brain $(DESTDIR)$(PREFIX)/bin/
	cp LICENSE NOTICE $(DESTDIR)$(PREFIX)/share/sia-brain/
	cp $(TREE)/build/brain/sia-brain.gguf $(DESTDIR)$(PREFIX)/share/sia-brain/

.PHONY: all install
