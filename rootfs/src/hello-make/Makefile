# A small C program in three files, built by make (docs/make.md).
# On SIEOS: copy its files to a folder of yours (cp FILE... DIR), then
# "make", "./hello", "make info", "make clean".
#
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

CC      = /bin/cc
CFLAGS  = -O2
SRCS   := $(wildcard *.c)
OBJS   := $(SRCS:.c=.o)
PROG    = hello

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(OBJS) -o $@

# every object from its C file; all of them read util.h
%.o: %.c util.h
	$(CC) $(CFLAGS) -c $< -o $@

# what make knows about the files (functions at work)
info:
	@echo sources: $(SRCS), objects: $(patsubst %.o,[%],$(OBJS))
	@echo in $(notdir $(CURDIR)): $(addprefix ./,$(PROG)) built with $(subst /bin/,,$(CC))

clean:
	rm -f $(OBJS) $(PROG)

.PHONY: all info clean
