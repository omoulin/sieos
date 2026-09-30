# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
CFLAGS = -O2 -ffreestanding -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
         -fno-stack-protector -fno-builtin -I/usr/include/ddk

hello.drv: hello.o
	ld -r -o $@ hello.o

hello.o: hello.c
	gcc $(CFLAGS) -c hello.c

clean:
	rm -f hello.o hello.drv
