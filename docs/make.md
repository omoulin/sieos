# make on SIEOS

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

`/bin/make` remakes files from the rules of a makefile, on SIEOS itself,
with `/bin/cc` ([cc.md](cc.md)) as its compiler. It is POSIX make with GNU
make's common additions, in one file: `user/make/make.c` (about 1,050
lines), built like every program in `/bin` (by gcc on x86-64, by sicc on
AArch64; it also builds itself on SIEOS).

```
make [-f FILE] [-C DIR] [-n] [-k] [-s] [-B] [-i] [-r] [-j1] [VAR=value...] [TARGET...]
```

## Try it

A small program in three files is on every disk, in `/src/hello-make`
(`rootfs/src/hello-make`): `main.c`, `util.c`, `util.h` and a Makefile with
variables, `$(wildcard)`, a pattern rule and `.PHONY`. As a user:

```
mkdir hm
cp /src/hello-make/Makefile /src/hello-make/main.c /src/hello-make/util.c /src/hello-make/util.h hm
cd hm
make                 /bin/cc -O2 -c main.c -o main.o, util.o, then the link
./hello              hello from make: dear SIEOS, sum 1..10 = 55
touch util.c
make                 only util.o and hello again
make -n              what would run, nothing run
make info            the functions at work
make clean
```

SIEOS's own `hello`, `sh` and `make` build on SIEOS the same way:
`/src/sieos` holds a Makefile and copies of `user/hello/hello.c`,
`user/sh/sh.c` and `user/make/make.c`, made with the disk. Copy them to a
folder of yours and `make`: the new `./make` builds hello-make again.

## How it works

1. **Options and `VAR=value`** are read first. `-C DIR` goes there (and
   says "Entering directory"); command-line variables win over the
   makefile's.
2. **The built-in part** is a short makefile text inside the program:
   `CC = cc`, `.SUFFIXES: .out .a .o .c .s .S .h`, and the rules
   `%.o: %.c`, `%.o: %.S`, `%.o: %.s`, `%: %.c`, `%: %.o` (with `$(CFLAGS)`,
   `$(CPPFLAGS)`, `$(LDFLAGS)`, `$(LDLIBS)` as GNU's). `-r` leaves the
   rules out.
3. **The makefile** (`-f`'s, else `makefile`, else `Makefile`) is read
   whole: lines joined at `\` + newline, `#` comments, variables, rules,
   includes. Targets and prerequisites are expanded when read, recipes
   when they run.
4. **Each goal** (the command line's, else `.DEFAULT_GOAL`, else the first
   target that does not start with `.`) is made depth first: its
   prerequisites, then its recipe if it is missing, older than one of
   them (the file server's modification times, in ns), a prerequisite was
   remade, it is phony, or `-B`. A target without a rule of its own gets
   the first pattern rule whose prerequisites exist or can be made (a
   chain of up to 3 rules; the makefile's rules before the built-in ones).
5. **Each recipe line** runs alone as `sh -c LINE` in make's folder, after
   it is shown (unless `@`, `-s` or `.SILENT`). The shell's status is the
   line's: anything but 0 stops make with `make: *** [target] Error N` and
   status 2 (with `-k`, the targets that do not depend on it are still
   made; with `-` or `-i`, `make: [target] Error N (ignored)` and on).

## What it supports

- **Rules:** `targets: prerequisites`, recipe lines after a tab, an inline
  recipe after `;`, several targets, the same target in several rules
  (prerequisites add up; a second recipe wins, with a warning),
  order-only prerequisites after `|`, `*` and `?` in prerequisites,
  pattern rules (`%.o: %.c`), static pattern rules
  (`$(OBJS): %.o: %.c`), suffix rules (`.c.o:`, `.c:`, from `.SUFFIXES`),
  `::` (read as `:`).
- **Variables:** `=`, `:=`, `::=`, `?=`, `+=`; `$(VAR)`, `${VAR}`, `$X`,
  `$$`; substitutions `$(VAR:.c=.o)` and `$(VAR:%.c=%.o)`; names made of
  references (`$($(X)_FLAGS)`); a recursive variable that uses itself is
  an error. `export VAR = value` is an assignment (there is no
  environment on SIEOS); `MAKE` (`make` and the options given, for
  `$(MAKE) -C dir`), `CURDIR`, `MAKECMDGOALS`, `.DEFAULT_GOAL`.
- **Automatic variables:** `$@ $< $^ $? $* $+ $|`, and `$(@D) $(@F)`
  (also for `<`, `^`...).
- **Recipe prefixes:** `@`, `-`, `+`; lines with `$(MAKE)` run even with
  `-n`.
- **Functions:** `subst`, `patsubst`, `strip`, `findstring`, `filter`,
  `filter-out`, `sort`, `word`, `words`, `firstword`, `lastword`, `dir`,
  `notdir`, `suffix`, `basename`, `addsuffix`, `addprefix`, `wildcard`,
  `foreach`, `if`, `call`, `info`, `warning`, `error`.
- **Directives:** `include`, `-include`, `sinclude` (wildcards allowed).
- **Special targets:** `.PHONY`, `.SUFFIXES`, `.SILENT`, `.IGNORE`; the
  others (`.PRECIOUS`, `.SECONDARY`, `.DELETE_ON_ERROR`...) are accepted
  and do nothing.
- **Options:** `-n` (show, do not run), `-C DIR`, `-f FILE` (several),
  `-k` (keep going), `-s` (silent), `-B` (remake everything), `-i`
  (ignore errors), `-r` (no built-in rules), `-j1` (`-jN` is accepted and
  runs one job at a time).
- **Messages** as GNU make's: `make: *** No rule to make target 'x',
  needed by 'y'.  Stop.`, `Makefile:3: *** missing separator.  Stop.`,
  `make: 'hello' is up to date.`, `make: Nothing to be done for 'all'.`,
  `make: Target 'all' not remade because of errors.`

## What it does not do (yet)

- Conditionals (`ifeq`, `ifdef`...), `define`, `override`, `vpath`,
  target-specific variables: an error names them.
- `$(shell ...)` and `!=`: a program's output cannot be read yet (no
  pipes); an error says so.
- Several jobs at once (`-j`), `-q`, `-t`, long options (`--dry-run`).
- Remaking an included makefile, deleting intermediate files, archive
  members (`lib.a(x.o)`), double-colon rules as GNU's (each with its own
  recipe).
- A recipe line is one shell command: SIEOS's shell has no `;`, `&&`,
  `|` or `cd` that lasts. A `\` + newline in a recipe joins the lines
  with a space. A line with its folder must fit SIEOS's program
  arguments: 256 bytes and 31 words (make says when it does not).

## What SIEOS gained for it

- **`sh -c LINE`** (`user/sh/sh.c`): runs one line, and ends with its
  status (a program's, or 1 when a built-in command fails, 127 when the
  command is unknown). A word that starts with `#` starts a comment.
- **New shell commands:** `cp FROM TO` and `cp FILE... DIR`, `touch FILE...`
  (a new empty file, or the time set to now), `rm -f`.
- **31 words** for a program's arguments (`__start` in `user/lib/lib.c`;
  15 before).
- **File times that never go back** (`user/fs/fs.c`): SIEOS has no clock
  chip driver yet, so its time is the build's date plus the time since
  boot, and a disk made after that build has files "from the future";
  the file server now moves its clock ahead to the newest file time a
  program asks about, so a file make writes is never older than its
  sources.

## Tests

`make make-test` (and `make ARCH=arm64 make-test`; tools/maketest.py)
boots SIEOS, logs in as a user, and checks: hello-make built and run;
nothing to do the second time; `touch util.c` remakes util.o and the
program only; `make -n` shows and runs nothing; the functions; `-B` and
`CFLAGS=-O0`; `make clean`; `-C` and `-s`; a failing recipe
(`tests/make/fail.mk`: `make: *** [bad] Error 1`, status 2), `-k`, `-`,
a target with no rule; then SIEOS's hello, sh and make built on SIEOS,
and that make building hello-make. PASS on both.
