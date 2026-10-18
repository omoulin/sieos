# python.mk - /bin/python (docs/python.md): MicroPython, an add-on package
# (make pkg-python, the Makefile's end; docs/packages.md), built for SIEOS by
# the Makefile's compiler (gcc on x86-64, sicc on AArch64) with the port in
# user/python. MicroPython's sources are not kept in SIEOS: the release's
# archive is fetched once into .hosttools (a pinned version, its checksum
# checked), and only its interpreter (py/), a few modules (extmod/), the
# double-precision maths (lib/libm_dbl) and two small helpers are built.
#
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

PY_VER  := 1.29.0
PY_SHA  := 8072f6ad42c0e3bbfca88ed09bf8f1106ce207a39129af920f70081b8f2fe286
MPY     := .hosttools/micropython-$(PY_VER)
PYB     := $(B)/python
$(MPY)/py/mpconfig.h:
	mkdir -p .hosttools && curl -sfL -o $(MPY).tar.gz https://github.com/micropython/micropython/archive/refs/tags/v$(PY_VER).tar.gz
	echo "$(PY_SHA)  $(MPY).tar.gz" | sha256sum -c --status || { echo "python: $(MPY).tar.gz: not the expected file"; rm -f $(MPY).tar.gz; exit 1; }
	tar xzf $(MPY).tar.gz -C .hosttools $(foreach d,py extmod shared lib/libm_dbl lib/re1.5,micropython-$(PY_VER)/$(d))
	rm $(MPY).tar.gz

# MicroPython's files (py/ as its py.mk lists them, without the other
# processors' nlr*.c and formatfloat.c) and the port's
PY_CORE := mpstate nlr nlrx64 nlraarch64 malloc gc pystack qstr vstr mpprint unicode mpz reader lexer parse \
           scope compile emitcommon emitbc parsenumbase parsenum emitglue runtime runtime_utils \
           scheduler cstack stackctrl argcheck warning map obj objarray objattrtuple objbool objboundmeth \
           objcell objclosure objcode objcomplex objdeque objdict objenumerate objexcept objfilter objfloat \
           objfun objgenerator objgetitemiter objint objint_mpz objlist objmap objmodule objobject \
           objpolyiter objproperty objnone objnamedtuple objrange objreversed objset objsingleton objslice \
           objstr objstrunicode objstringio objtemplate objtuple objtype objzip opmethods sequence stream \
           binary builtinimport builtinevex builtinhelp modarray modbuiltins modcollections modgc modio \
           modmath modmicropython modstring modstruct modsys moderrno vm bc repl smallint
PY_MODS := $(PY_CORE:%=py/%) $(addprefix extmod/,modjson modre modbinascii modrandom modtime modheapq) \
           shared/runtime/sys_stdio_mphal shared/timeutils/timeutils
PY_LIBM := __cos __expo2 __fpclassify __rem_pio2 __rem_pio2_large __signbit __sin __tan acos acosh asin \
           asinh atan atan2 atanh ceil cos cosh copysign erf exp expm1 floor fmod frexp ldexp lgamma log \
           log10 log1p modf nearbyint pow rint round scalbn sin sinh tan tanh tgamma trunc
PY_PORT := main files socket hashlib formatfloat libc
# (+ /bin/cc's formatting and number parsing, exact: formatfloat.c)
PY_OBJ  := $(PY_MODS:%=$(PYB)/%.o) $(PY_LIBM:%=$(PYB)/lib/libm_dbl/%.o) $(PY_PORT:%=$(PYB)/port/%.o) $(B)/user/cc/libc-fmt.o
# (compiled for size, as MicroPython's ports are, but the bytecode loop and
# the garbage collector for speed)
PY_CF   := $(patsubst -O2,-Os,$(filter-out -mgeneral-regs-only,$(UCFLAGS))) -fno-math-errno -Iuser/python/include -Iuser/python -I$(MPY) -I$(PYB)
$(PYB)/py/vm.o $(PYB)/py/gc.o: PY_CF += -O2
# The interned strings, modules and roots MicroPython's sources name are
# collected by its scripts from their preprocessed text (the host's gcc -E:
# it marks each file's lines; with sicc's headers, and on AArch64 told so).
PY_CPP  := gcc -E -nostdinc -isystem cc/include $(PY_CF) $(if $(filter arm64,$(ARCH)),-U__x86_64__ -U__amd64__ -D__aarch64__)
PY_GEN  := $(addprefix $(PYB)/genhdr/,mpversion.h qstrdefs.generated.h moduledefs.h root_pointers.h)

$(PY_MODS:%=$(MPY)/%.c) $(PY_LIBM:%=$(MPY)/lib/libm_dbl/%.c): | $(MPY)/py/mpconfig.h ;

$(B)/python.elf: $(PY_OBJ) $(UCORE:%=$(B)/user/lib/%.o) $(ULIB) user/user.ld | $(TOOLDEP)
	$(TLD) -T user/user.ld -z max-page-size=4096 -o $@ $(UCORE:%=$(B)/user/lib/%.o) $(PY_OBJ) $(ULIB)
$(PYB)/%.o: $(MPY)/%.c $(PY_GEN) user/python/*.h user/python/include/*.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(PY_CF) -w -c $< -o $@
$(PYB)/port/%.o: user/python/%.c $(PY_GEN) user/python/*.h user/python/include/*.h user/lib/mk.h $(TOOLDEP)
	@mkdir -p $(@D)
	$(TCC) $(PY_CF) -Wno-unused-parameter -c $< -o $@
$(PYB)/genhdr/mpversion.h: $(MPY)/py/mpconfig.h
	@mkdir -p $(@D)
	printf '#define MICROPY_GIT_TAG "v$(PY_VER)"\n#define MICROPY_GIT_HASH ""\n#define MICROPY_BUILD_DATE ""\n' > $@
$(PYB)/genhdr/qstr.i: $(PY_MODS:%=$(MPY)/%.c) $(PY_PORT:%=user/python/%.c) user/python/*.h $(PYB)/genhdr/mpversion.h
	for f in $(filter %.c,$^); do $(PY_CPP) -DNO_QSTR $(CURDIR)/$$f || exit 1; done > $@.tmp && mv $@.tmp $@   # (the scripts skip names starting with a dot)
$(filter-out %/mpversion.h,$(PY_GEN)) &: $(PYB)/genhdr/qstr.i
	cd $(PYB)/genhdr && for m in qstr module root_pointer; do rm -rf $$m && \
	    python3 $(CURDIR)/$(MPY)/py/makeqstrdefs.py split $$m qstr.i $$m _ && \
	    python3 $(CURDIR)/$(MPY)/py/makeqstrdefs.py cat $$m _ $$m $$m.collected >/dev/null || exit 1; done
	cat $(MPY)/py/qstrdefs.h $(PYB)/genhdr/qstr.collected | sed 's/^Q(.*)/"&"/' | $(PY_CPP) - | \
	    sed 's/^\"\(Q(.*)\)\"/\1/' > $(PYB)/genhdr/qstrdefs.preprocessed.h
	python3 $(MPY)/py/makeqstrdata.py $(PYB)/genhdr/qstrdefs.preprocessed.h > $(PYB)/genhdr/qstrdefs.generated.h.new
	python3 $(MPY)/py/makemoduledefs.py $(PYB)/genhdr/module.collected > $(PYB)/genhdr/moduledefs.h.new
	python3 $(MPY)/py/make_root_pointers.py $(PYB)/genhdr/root_pointer.collected > $(PYB)/genhdr/root_pointers.h.new
	cd $(PYB)/genhdr && for f in qstrdefs.generated.h moduledefs.h root_pointers.h; do \
	    cmp -s $$f.new $$f && rm $$f.new || mv $$f.new $$f; done   # (the same: kept, nothing to recompile)
