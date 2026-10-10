# sicc: SIEOS's own C compiler

sicc is a C11 compiler, assembler and linker written from scratch for SIEOS.
It is one program with no third-party code, about 17,500 lines in `cc/` (plus
610 lines of headers), with two targets: x86-64 and AArch64 (for the
Raspberry Pi 4 and 5, see [AArch64](#aarch64)). It builds the whole of SIEOS (kernel, servers and
programs), and that build passes the boot and crash tests. It also compiles
itself, on both targets: the stage 2 and stage 3 binaries are byte-for-byte
identical.

```
make cc             # build/host/sicc (built by the development machine's compiler)
make cc-test        # test programs vs gcc, error messages, expression and control-flow fuzzing, the ABI matrix
make cc-bootstrap   # sicc builds sicc builds sicc; stages 2 and 3 must be identical
make cc-sieos       # all of SIEOS built by sicc into build-sicc/, then boot + crash tests
make cc-bench       # speed and size of ten small programs, sicc vs gcc -O2
make cc-a64-test    # the AArch64 tests, under QEMU's user-mode emulator (QEMU_A64=path of qemu-aarch64; default: the one on the PATH, else .hosttools/)
make cc-a64-bootstrap  # sicc for AArch64 compiles itself under emulation; stages 2 and 3 identical
```

## How it works

```
 file.c ──► lex ──► pp ──► parse ──► ir ──► opt ──► ra ──► emit ──► asm ──► .o ──► link ──► ELF / binary
            tokens  macros  typed AST  virtual   numbering  registers  AT&T text  machine    objects + script
                    #include           registers inlining                          code + DWARF lines
 file.S ──► lex ──► pp ─────────────────────────────────────────────────────────► asm
```

Every stage works in memory: the compiler emits assembly *text*, and the
assembler in the same process reads it straight back, with no temporary files.
`-S` stops after the text (`-o -`: standard output), and `-c` after the
object. Several sources can be given at once; each is compiled on its own
(macros and statics do not leak from one to the next).

| File | Lines | What it does |
|---|---|---|
| `sicc.h`, `sys.h` | 351 | Shared types (tokens, types, AST, objects). Also the few C library functions sicc itself uses. |
| `util.c` | 204 | Arena allocator, string interning (identifiers compare by pointer), buffers, vectors, maps. |
| `lex.c` | 265 | Tokens, line splices, universal character names, `file:line:col` errors with the line and a caret. |
| `pp.c` | 866 | Preprocessor: hide-sets, `__VA_OPT__`, GNU `, ## __VA_ARGS__`, include-guard and `#pragma once` detection, an assembler mode for `.S`. |
| `parse.c` | 3,533 | C11 parser and type checker. Every conversion becomes an explicit cast; constant expressions (also `long double`, `__int128`, `_Complex`); VLA sizes; vector types. |
| `ir.h`, `ir.c` | 2,848 | Lowers the AST to an IR on virtual registers (not SSA). System V calls, `va_arg`, bit-fields, switch, atomics, inline asm, x87 `long double`, `__int128`, complex, vectors, thread-locals, inlining, division by constants. |
| `opt.c` | 692 | Value numbering (CSE, copy and constant propagation, folding, address folding, rotations), copy forwarding, loop-invariant hoisting, dead code. |
| `ra.c` | 293 | Liveness, then linear scan with spill costs and coalescing. |
| `emit.c` | 1,588 | IR to x86-64 assembly: frames, parallel moves, SSE vector code, the DWARF debug information. |
| `asm.c` | 2,213 | Assembler: AT&T syntax, 16/32/64-bit code, x87, SSE to SSE4.1, macros, branch relaxation, TLS relocations, `.loc` line tables. ELF64 objects. |
| `link.c` | 1,352 | Static linker (linker-script subset, TLS, `.debug_*` kept), `-r`, `ar`, `objcopy -O binary`. |
| `main.c` | 148 | gcc-style driver. Also runs as `--ld`, `--ar` or `--objcopy`. |
| `include/` | 635 | Freestanding headers: `stddef.h`, `stdarg.h`, `stdint.h`, `float.h`, `stdatomic.h`, `complex.h`, and the SSE/AVX intrinsics (`xmmintrin.h` … `immintrin.h`). |

### The IR and the optimizer

A function is a list of blocks of instructions (`O_ADD`, `O_LOAD`, `O_BR`,
`O_CALL`…) on numbered virtual registers, integer or floating point. Values
that do not fit a register (`long double`, `__int128`, `_Complex`, structs,
vectors) are handled by address. Lowering is careful about order: operands are
always computed before the instruction itself.

- **While lowering:** constant folding, identities, power-of-two multiplies
  and divisions as shifts; **division by any other constant** as a
  multiplication by its reciprocal (Granlund and Montgomery; with the 65-bit
  "add back" form for unsigned 64-bit divisors; signed 64-bit divisors that
  need it fall back to `idiv`). `sqrt`/`sqrtf` become `sqrtsd`/`sqrtss` (errno
  is not set: `math_errhandling` is `MATH_ERREXCEPT`); small `memcpy`/`memset`
  of a constant size become moves.
- **Inlining** (`ir.c`): a call to a small function (at most 24 nodes; 6 at
  `-Os`), or to a static function whose only uses are calls (up to 600 nodes
  if called once), lowers the callee's body in place: its locals get storage
  in the caller, the arguments are assigned to its parameters, `return`
  assigns and jumps out. Such a static function is not emitted at all unless a
  call could not be inlined. `__attribute__((always_inline))` and `noinline`
  are obeyed. Never inlined: recursion (a function being inlined), more than
  8 levels, VLAs, `alloca`, computed goto, `setjmp`, variadic functions.
  **Stack safeguard:** a body whose locals need more than 256 bytes of frame is
  not inlined, and a function stops inlining once it has grown by 1 KB: an
  inlined body's frame lives as long as its caller's, also across the caller's
  own calls, so deep call chains (the kernel's 4 KB stacks) must not grow
  much. Measured with `STACKCHECK=1`: the deepest kernel thread stack is
  1,744 bytes with sicc (of 3,968), 1,608 with gcc.
- **Value numbering** (`opt.c`), over the dominator tree:
  common subexpressions (also loads, until memory may have changed), copy and
  constant propagation, folding and simplification, rotations (`x << c | x >>
  (32 - c)` → `rol`), extensions folded into the load, store-to-load
  forwarding, and address folding: `sym(%reg,%reg,4)`, constant indexes as
  displacements. The IR is not SSA, so a value is named by (register,
  version): each definition gets a new version; a register written several
  times gets a fresh version at each merge point; memory gets a version too.
  A block copy into a frame slot is forwarded to the loads from it, and the
  copy is deleted when the slot is never read otherwise.
- **Loop-invariant code** moves in front of its loop while registers remain
  (the loop's pressure is estimated per register class).
- **Register allocation** (`ra.c`): linear scan, one interval per virtual
  register. rax, rcx, rdx, r11 and xmm0–7 stay free as scratch. Spill choice:
  the fewest loop-weighted uses per position held. Coalescing: a value defined
  from a register that dies there takes that register (no move). Hints steer
  parameters and arguments to their registers; constant arguments are loaded
  straight into theirs.
- **Code** (`emit.c`): no frame for leaf functions without stack slots; only
  the callee-saved pushes (no `%rbp` frame) when nothing else needs it; loop
  heads aligned to 16 bytes (at most 10 bytes of padding; not at `-Os`);
  `movaps` for register copies of floats and a cleared destination before
  `cvtsi2sd` (no false dependencies).
- **`-Os`:** smaller inlining limits, no loop or function alignment, division
  by constants left to `div`, smaller inline copies.

### The ABI

sicc follows the x86-64 System V ABI and interoperates with gcc-built code in
both directions. `tests/cc/abigen.py` checks random signatures (scalars,
floats, `long double`, structs and unions of every shape, stack arguments,
struct results, variadic calls) with sicc as caller, as callee, and both.
- Structs of 16 bytes or less are split into eightbytes, INTEGER or SSE;
  larger ones and those that do not fit go in memory; struct results in memory
  use a hidden pointer in rdi.
- `long double` is the x87 80-bit type (16 bytes, class X87: in memory as an
  argument, in `st0` as a result); `_Complex long double` comes back in `st0`
  and `st1`; `__int128` is two INTEGER eightbytes.
- Variadic functions save the argument registers; `va_list` is the standard
  24-byte record; `al` carries the number of vector registers used.
- Vector types are passed like structs of their elements (see the
  limitations).

### The assembler and linker

- **Assembler:** what SIEOS's `.S` files use (`.code16/32/64`, `.macro`,
  `.irp`, `.rept`, `.if`, numeric local labels…), x87, SSE to SSE4.1. Branch
  relaxation. For `boot.S`, `entry.S`, `ap.S` and `crt0.S` the machine code is
  identical to the development machine's assembler. `sym@tpoff`,
  `sym@gottpoff`, `sym@plt`; `.tdata`/`.tbss`; `.file`/`.loc` build
  `.debug_line`.
- **Linker:** ELF64 static executables; script commands `ENTRY`, `SECTIONS`,
  location counter, `ALIGN`, `ADDR`/`SIZEOF`/`LOADADDR`, `AT()`, `KEEP`,
  wildcards, assignments, `/DISCARD/`, `PHDRS`; archives; COMMON; `-r`
  (relocatable output); thread-local storage (a `PT_TLS` header, `.tbss`
  taking no address space, `TPOFF32/64`, initial-exec code turned into
  local-exec); `.debug_*` sections kept. Its output does not get the
  execute permission (sicc uses only ISO C file functions).

## AArch64

`--target=aarch64` (or a program name ending in `aarch64-sicc`; a sicc built
for AArch64 defaults to it) generates AArch64 code for the same C. The
front end, the IR and the optimizer are shared; the target-specific parts
are `ir_a64.inc` (calls, parameters, returns, `va_arg`, inline asm operands:
530 lines), `emit_a64.c` (instructions, NEON included: 1,210 lines),
`asm_a64.inc` (the assembler, NEON included: 1,100 lines) and the AArch64 relocations in `link.c`. `cc/rt/rt.c`
holds the run-time helpers that AArch64 code calls (128-bit division,
128-bit integer/floating conversions, complex multiplication and division by
C11 Annex G). The second target leaves the x86-64 output unchanged (the same
bytes for the tests, sicc's sources and SIEOS's sources), except where the
AArch64 tests found an x86-64 bug: a `char` or `short` parameter whose
address is taken was stored into its slot with 4 bytes, overwriting its
neighbours; now with its own size.

### SIEOS's AArch64 ABI

AAPCS64, with these choices:
- **`long double` is `double`** (64 bits), as on Apple's platforms and
  Windows on ARM. The alternative, AAPCS64's 128-bit IEEE quad, has no
  hardware support: every operation would be a software call, for a type
  SIEOS does not need. `LDBL_*` in `<float.h>` follow.
- **`char` is unsigned**, as AAPCS64 says (`__CHAR_UNSIGNED__`).
- Arguments: x0–x7 and v0–v7; homogeneous floating-point aggregates (1–4
  floats or doubles: structs, arrays, complex numbers) in consecutive v
  registers, all or none; other values of 16 bytes or less in one or two x
  registers (16-byte-aligned ones from an even register); larger structs are
  copied by the caller and passed by address (the callee uses that copy in
  place); the stack takes the rest in 8-byte slots. Large results go to a
  buffer whose address the caller passes in x8. Values narrower than a
  register are extended by their user, not trusted.
- `va_list` is AAPCS64's 32-byte record (`__stack`, `__gr_top`, `__vr_top`,
  `__gr_offs`, `__vr_offs`); a variadic function saves x0–x7 and q0–q7 (192
  bytes; q registers not with `-mgeneral-regs-only`).
- Frames: `stp x29, x30, [sp, #-16]!`, a frame record chain for backtraces;
  leaf functions without locals get no frame.
- **Registers:** x16–x17 (the linker's veneer registers, unused as such: the
  linker makes no veneers) and x14–x15 are the code generator's scratch
  registers, and so is **x18**: SIEOS reserves no platform register. The
  allocator uses x0–x13 and x19–x28, v0–v29.
- Thread-local storage: local-exec, `tpidr_el0` plus the offset the linker
  computes (the block starts 16 bytes, aligned, after the thread pointer:
  AArch64's variant 1).
- Atomics: load-acquire/store-release exclusive loops (`ldaxr`/`stlxr`),
  `ldar`/`stlr` for atomic loads and stores, `dmb ish` for fences. `-mlse`
  (or `-march=armv8.1-a` and later, or `+lse`) uses the ARMv8.1 atomics
  (`ldaddal`, `swpal`, `casal`) instead: the Pi 5's Cortex-A76 has them, the
  Pi 4's Cortex-A72 does not, so the loops are the default.

Supported: all of the C that x86-64 supports (`__int128`, `_Complex`, VLAs,
`_Atomic`, `_Thread_local`, computed goto, bit-fields, inline asm with the
constraints `r`, `w`, `m`, `Q`, `i`, `n`, `I`–`N`, digits, named operands,
register variables, the modifiers `w x s d q b h c n` and `%=`), `-g` line
tables, `-mgeneral-regs-only` (no floating point: kernels). Not yet: vector
types (`vector_size`: an error on AArch64).

### The assembler and linker for AArch64

- **Assembler:** GNU syntax, `//` comments (and `#` at the start of a line),
  `:lo12:`, `:tprel_hi12:`, `:tprel_lo12_nc:`; the base instructions
  (arithmetic with immediates, shifts and extends; logical immediates;
  bit-fields; conditional select and compare; multiply and divide; loads and
  stores in every addressing mode, pairs, exclusive and acquire/release, LSE
  atomics; branches) and floating point (arithmetic, fused multiply-add,
  conversions with each rounding, `frint*`, `fcsel`, `fccmp`), and what a
  kernel needs: `mrs`/`msr` with about 90 system registers by name (EL0–EL3:
  `sctlr`, `ttbr0/1`, `tcr`, `mair`, `vbar`, `esr`, `far`, `elr`, `spsr`,
  timers, GIC CPU interface, `hcr_el2`, `scr_el3`…) and any by number
  (`s3_0_c15_c2_1`), `msr daifset/daifclr/spsel, #imm`, `tlbi`, `dc`, `ic`,
  `at`, `dsb`/`dmb`/`isb`, `eret`, `wfi`/`wfe`/`sev`, `svc`/`hvc`/`smc`/`brk`.
  Not supported: `ldr x0, =value` (literal pools: use `adrp`/`add` or
  `mov`/`movk`), SIMD vector instructions.
- **Linker:** the AArch64 relocations (absolute 16/32/64, PC-relative
  16/32/64, `adr`, `adrp`, `add`/load-store `:lo12:` at each access size,
  branches: 14-bit test, 19-bit conditional, 26-bit `b`/`bl`; TLS local-exec
  `tprel` high and low parts); `ALIGNOF()` in scripts; `SIZEOF`/`ALIGNOF` of
  a section absent from the output is 0. Binary output (`objcopy -O binary`)
  is target-independent. No range-extension veneers: a program's code must
  fit in ±128 MB of `bl`.

### Testing without the hardware

Programs run under QEMU's user-mode emulator on the development machine (a
host tool for tests only, like the host compiler; it is not part of SIEOS).
`tests/cc/a64/` has a small test C library (`libc.c`: start-up, memory,
files, strings, the emulator's system calls; `fmt.c`: an exact `printf` and
`strtod` with big integers; `crt0.S`, `prog.ld`) that is enough to run
every test and sicc itself.
- **References:** the tests are compiled by the host compiler with SIEOS's
  AArch64 types (`-funsigned-char -mlong-double-64`) and linked with the same
  `fmt.c`, so both sides print identically; the outputs must match. (`fmt.c`
  itself was compared with the host's `printf` and `strtod` on 3.4 million
  cases.)
- **The ABI** (`a64/abi_a64.py`): random signatures, checked against a model
  of AAPCS64 written from the standard (not against sicc). Hand-written
  assembly (`abi_tramp.S`) records every argument register and the stack of a
  sicc call, and calls sicc functions with registers and stack set by the
  model (with garbage in unused bits), checking results, x19–x28, d8–d15 and
  sp both ways.
- **Encodings** (`a64/enc.S`, `enc.py`): 504 instructions (228 NEON), each with the word
  a reference assembler produces. Also checked once: every function sicc
  generates for the tests and for its own sources assembles to the same
  bytes with sicc's assembler and with the reference (210 sections).
- **Bootstrap** (`a64/bootstrap.sh`): stage 2 is sicc for AArch64 built by
  sicc; stage 3 is the same built by stage 2 running emulated; they are
  identical (and stage 3 then passes the tests). Getting there found three
  places where sicc depended on its host: an unsigned `char` (a register
  number of -1), argument evaluation order (two operands that emit
  instructions, now made in a fixed order), and the linker writing symbols
  in hash order (now in input order).

### Vectors and NEON

`vector_size(8)` and `vector_size(16)` types compile to NEON (Advanced SIMD)
on AArch64, with every operator x86-64 has: `+ - * / % & | ^ ~ << >>`,
comparisons (masks), scalars mixed in, subscripts, casts,
`__builtin_shufflevector`. As on x86-64, a vector lives in memory like a
struct and each operation is one IR instruction, emitted as `ldr q/d`, one
NEON instruction on v0–v4 and `str` (v0–v7 are never allocated, so they are
free between two instructions). What NEON lacks is done element by element:
integer division and `%`, 64-bit multiplication. 32-byte vectors are split
in two 16-byte halves.

**`<arm_neon.h>`** is sicc's own (`cc/include/arm_neon.h`, 1,260 lines),
generated by `tools/cc/mkneon.py` (a host tool; rerun it after adding an
operation). Types `int8x8_t`…`float64x2_t`, `float16x4/8_t` (as raw 16-bit
storage), and the functions an inference engine and string routines need:
`vld1/vst1`, `vdup_n`/`vmov_n`, `vget_low/high`, `vcombine`,
`vget/vset_lane`; `vadd vsub vmul vneg vabs vmax vmin vqadd vqsub vrhadd`;
`vmla vmls vfma vfms`; widening `vmull vmull_high vmlal vmlal_high vmovl
vmovl_high`, narrowing `vmovn vqmovn vqmovun`, pairwise `vpadd vpaddl
vpadal`, across-vector `vaddv vaddvq`; shifts by constant and by vector;
`vand vorr veor vbic vmvn vbsl vcnt`; `vceq vcge vcgt vcle vclt`;
`vcvt` (int↔float, `vcvt_f32_f16`/`vcvt_high_f32_f16`/`vcvt_f16_f32`);
`vzip vuzp vtrn vext vrev vdup_lane vtbl/vqtbl1`; the `vreinterpret` family
(macros). With `-march=armv8.2-a+dotprod` (or `armv8.4-a` and later),
`__ARM_FEATURE_DOTPROD` is defined and `vdotq_s32`, `vdotq_u32`,
`vdotq_lane_s32` and `vdotq_laneq_s32` (`sdot`/`udot`) are there.
`__ARM_NEON` is always defined for AArch64.

The functions are `always_inline` wrappers around **`__builtin_neon(op,
(T *)0, a, b, acc, imm)`**: `op` one of the `VOP_*` names after `VOP_FMA` in
`cc/sicc.h`, the result type given by a null pointer, `acc` the accumulator
for `fmla`/`mla`/`sdot`/`smlal`/`sadalp`/`bsl` (copied to the result first),
`imm` a lane or a byte count. It is an error with `-mgeneral-regs-only`, and
`VOP_DOT` without the dot-product flag. The x86-64 side never sees it.

**Calls (AAPCS64):** an 8- or 16-byte vector, and a homogeneous aggregate of
up to four of them (HVA), go in v0–v7 and come back in v0–v3, as the
standard says, so NEON code calls and is called by other compilers'.
`va_arg` of a vector is an error.

Tests: `tests/cc/vector.c` now runs on AArch64 too (vs the host compiler);
`tests/cc/a64/t_neon.c` checks 14 groups of intrinsics against plain C, and
`t_neon_dot.c` the dot products (both at `-O0` and `-O2`); `enc.S` has 228
NEON instructions more, checked against a reference assembler (504/504).

### Building the AArch64 kernel with sicc: status

Done: SIEOS for AArch64 (QEMU `virt`, QEMU `raspi4b`, the Pi 4 and 5 card
images) is built by sicc alone ([arch.md](arch.md), [raspberrypi.md](raspberrypi.md)):
its start-up code, exception vectors, MMU code and `kernel8.img` all go
through sicc's assembler and linker; the kernel uses `-ffreestanding
-mgeneral-regs-only`. Still missing: literal pools (`ldr x0, =sym`: use
`adrp`/`add`), veneers for branches beyond ±128 MB, debug information
beyond line tables.

## What is supported

- **C11:** the whole language: `_Generic`, `_Static_assert`, `_Alignas`,
  `_Noreturn`, anonymous members, designated initializers, compound literals,
  flexible arrays, **variable-length arrays** (also as parameters, `[*]`,
  `sizeof` evaluated at run time, the stack given back by `break`, `continue`
  and `goto`; jumps into their scope are errors), **`_Complex`** (`float`,
  `double`, `long double`; multiplication and division call `__mulsc3`… as
  gcc does), **`_Atomic`** and `<stdatomic.h>`, **`_Thread_local`**/`__thread`,
  `long double` (x87), `u8"…"`/`u"…"`/`U"…"`, universal character names (also
  in identifiers, the same name as the UTF-8 characters).
- **GNU extensions:** statement expressions, `typeof`, attributes (`packed`,
  `aligned`, `section`, `noreturn`, `used`, `weak`, `noinline`,
  `always_inline`, `vector_size`…), case ranges, `a ?: b`, computed goto
  (`&&label`, `goto *p`, also in static tables), packed bit-fields crossing
  their storage unit, `__int128` (all operations; division and conversions
  call `__divti3`… as gcc does), inline asm (`r a b c d S D m i n g q`, `=`
  `+` `&`, matching operands, `%b %w %k %q %h %c`, clobbers), builtins
  (`__builtin_va_*`, `offsetof`, `expect`, `unreachable`, `trap`,
  `clz/ctz/popcount/bswap`, `frame_address`, `complex`, `shufflevector`,
  `memcpy` & co.), `__atomic_*` and `__sync_*`.
- **Vectors:** `__attribute__((vector_size(8|16|32)))` of any integer or
  floating type: `+ - * / % & | ^ ~ << >>`, comparisons (masks), scalars mixed
  in, subscripts, casts between vectors of one size, `__builtin_shufflevector`.
  SSE2 instructions where they exist (SSE4.1's with `-mavx`/`-mavx2`/
  `-march=native`), the rest element by element. The intrinsics headers
  (`xmmintrin.h`, `emmintrin.h`, `tmmintrin.h`, `smmintrin.h`, `immintrin.h`)
  cover the common SSE–SSE4.1 operations and a subset of AVX/AVX2 (loads and
  stores, arithmetic, logic, compares, shifts, `madd`/`maddubs`, packs,
  unpacks, conversions, `movemask`), checked against the host compiler's.
- **Thread-local storage:** local-exec code (`%fs:x@tpoff`) for variables of
  the program, initial-exec for others; `.tdata`/`.tbss`, `PT_TLS`. What a
  kernel must do to run it: give each thread an FS base (`wrfsbase` or
  `MSR_FS_BASE`, saved and restored on every switch), and set it up — copy the
  `PT_TLS` template (its initialized part, then zeros up to its size) just
  below the thread pointer, aligned as the header says, and store the thread
  pointer itself at `%fs:0` (local-exec code reads it there for `&x`).
  SIEOS does this now: the kernel loads each thread's FS base on every switch
  (`SYS_SET_FS`), `user/user.ld` lays the template out, and `user/lib/lib.c`
  copies it for each thread (`make kernel-test` checks sicc-built programs too).
  `tests/cc/static/tls.c` does it by hand on the development machine.
- **Debug information** (`-g`): DWARF 4 line tables (`.debug_line`, built by
  the assembler from `.loc`), a compile unit with one subprogram per function
  (name, file, line, address range) and `.debug_aranges`. Enough for
  backtraces, `addr2line`, breakpoints by line and source in disassembly.
  Variables and types are not described: their locations follow the register
  allocator (location lists), a larger piece of work with less use for SIEOS
  than lines and functions. `-g` does not change the code.
- **Output:** ELF64 objects, static executables, `-r`, `-mcmodel=kernel`,
  freestanding mode.
- **Driver flags:** `-c -S -E -o -I -isystem -D -U -O0/-O1/-O2/-Os -g`,
  `-mavx -mavx2 -march=native`, `-mgeneral-regs-only`, `-T -e -r -Wl,…`,
  `-nostdlib`. Other gcc flags are accepted and ignored.

## Tests

| Suite | What it checks | Result |
|---|---|---|
| `tests/cc/run.sh` | 30 programs built by sicc and by gcc, outputs compared: arithmetic, control flow, pointers, structs, floats, varargs, preprocessor, initializers, inline asm, scopes, C11 features, `long double`, `__int128`, complex, atomics, VLAs, packed bit-fields, computed goto, thread-locals (with threads, and a variable defined by gcc), division by constants, vectors, intrinsics, spills under pressure with a dirty stack, narrow parameters in memory; also several sources plus `-r`, TLS linked by sicc's own linker (with a gcc object), and `-g` (same code; `addr2line` reads the lines). | 30/30 |
| `tests/cc/errors.sh` | 16 wrong programs, each with its expected `file:line:col: error`. | 16/16 |
| `tests/cc/fuzz.py` | Random integer expressions over all integer types. | 300/300 (45,000 expressions); 30 in `make cc-test` |
| `tests/cc/cfuzz.py` | Random functions: loops, switch with fall-through, break/continue/goto, early returns, arrays, calls; at `-O0` and `-O2`. | 180/180 (3 seeds); 30 in `make cc-test` |
| `tests/cc/abigen.py` | Random signatures in a caller and a callee file; sicc callee, sicc caller, both, vs gcc. | 460 signatures (6 seeds), 3/3 each; 60 in `make cc-test` |
| `make cc-bootstrap` | gcc → sicc1 → sicc2 → sicc3, `cmp sicc2 sicc3`, then the tests on sicc3. | identical, 30/30 |
| `make cc-sieos` | SIEOS built only with sicc's tools; boot test (78 checks) and crash test. | both PASS |
| `tests/cc/run_a64.sh` | AArch64, emulated: the programs above (all but the x86 assembly and intrinsics ones) plus AArch64 inline asm and TLS, NEON intrinsics and dot products, vs the host compiler with AArch64 types; at `-O0`, the default, `-O2`, and with `-mlse`. | 29/29 each |
| `tests/cc/errors.sh --target=aarch64` | The same wrong programs, for AArch64. | 16/16 |
| `QEMU=$(QEMU_A64) fuzz.py`, `cfuzz.py` | The fuzzers, for AArch64. | 400/400 (60,000 expressions), 300/300 |
| `tests/cc/a64/abi_a64.py` | AAPCS64 from the standard vs sicc, both ways, `-O0/-O1/-O2`. | 1,510 signatures (11 seeds), 0 failures |
| `tests/cc/a64/enc.py` | Instruction encodings, NEON included, vs a reference assembler. | 504/504 |
| `make cc-a64-bootstrap` | sicc for AArch64 builds itself (emulated); `cmp` stage 2 and 3; tests on stage 3. | identical, 29/29 |

## Measurements

Development machine, wall time, best of 3. (The last runs shared the machine
with another heavy job: absolute times are a little high, the ratios hold.)

**Compiling:**

| | gcc -O2 | sicc -O2 |
|---|---|---|
| SIEOS's 38 C files | 5.6 s, 42 MB peak | 0.37 s, 14 MB peak |
| sicc's own 11 files | 9.5 s, 70 MB peak | 0.87 s, 36 MB peak |

The new optimizations cost sicc about 40% on its own sources (0.63 s before)
and nothing measurable on SIEOS.

**Code size**, `.text` bytes of SIEOS as built:

| | gcc -O2 | sicc before | sicc now | now / gcc |
|---|---|---|---|---|
| kernel | 35,401 | 47,833 | 37,125 | 1.05 |
| fs | 45,661 | 63,947 | 59,465 | 1.30 |
| auth | 27,116 | 42,635 | 29,315 | 1.08 |
| atlas | 40,149 | 46,461 | 42,219 | 1.05 |
| sh | 24,525 | 25,791 | 24,701 | 1.01 |
| init | 14,389 | 16,670 | 14,764 | 1.03 |
| con | 9,365 | 11,435 | 10,233 | 1.09 |
| vblk | 6,309 | 8,563 | 7,393 | 1.17 |
| all programs | 272,242 | | 294,580 | 1.08 |

`kernel.bin`: 40,944 bytes with sicc, 39,224 with gcc (51,656 before).

**Code speed**, `make cc-bench` (ms, lower is better; the benchmark's own
`.text` bytes):

| | gcc -O2 | sicc before | sicc now | now / gcc | gcc B | sicc B |
|---|---|---|---|---|---|---|
| crc | 447 | 445 | 447 | 1.00 | 852 | 288 |
| sieve | 356 | 373 | 371 | 1.04 | 232 | 156 |
| sort | 1,240 | 1,266 | 1,292 | 1.04 | 616 | 382 |
| list | 139 | 139 | 141 | 1.01 | 250 | 254 |
| interp | 906 | 509 | 622 | 0.69 | 754 | 703 |
| nbody | 171 | 647 | 199 | 1.16 | 515 | 753 |
| mix (SHA-like) | 344 | 1,570 | 399 | 1.16 | 807 | 1,199 |
| fib (recursion) | 146 | 287 | 206 | 1.41 | 1,076 | 383 |
| hash | 110 | 215 | 163 | 1.48 | 648 | 578 |
| matmul | 11 | 58 | 28 | 2.55 | 736 | 654 |

gcc vectorizes matmul; without vectorization it takes 21 ms (sicc 1.33×).
`interp` varies by ±10% with code layout. Login (Argon2 with 24 passes over
16 MB): 0.33–0.49 s with sicc (0.57 s before), 0.18–0.21 s with gcc.

**AArch64** (`tests/cc/a64/bench.sh`; emulated times are only indicative):

| bench | sicc AArch64 B | sicc x86-64 B | gcc x86-64 B | AArch64 ms (QEMU) | x86-64 ms |
|---|---|---|---|---|---|
| crc | 308 | 282 | 758 | 731 | 446 |
| fib | 456 | 370 | 919 | 1,240 | 243 |
| hash | 644 | 573 | 555 | 481 | 169 |
| interp | 644 | 535 | 541 | 6,036 | 624 |
| list | 280 | 249 | 189 | 189 | 140 |
| matmul | 648 | 644 | 590 | 519 | 28 |
| mix | 724 | 1,188 | 692 | 929 | 403 |
| nbody | 760 | 742 | 440 | 4,650 | 204 |
| sieve | 220 | 151 | 171 | 1,183 | 364 |
| sort | 532 | 377 | 467 | 2,877 | 1,173 |

sicc's own code: 483,204 bytes of `.text` for AArch64, 462,424 for x86-64
(1.04×). Compiling `ir.c` (sicc's largest file): 0.08 s and 47 MB peak for
AArch64, 0.17 s and 43 MB for x86-64; the AArch64 sicc itself, emulated,
0.71 s and 71 MB (with the emulator's own memory).

## Limitations

- **Code:** the targets were 1.15× gcc's size and 1.3× its time. Reached:
  SIEOS as a whole (1.08×), the kernel (1.05×), 8 of 10 benchmarks. Not
  reached: fs (1.30×: small structs passed by value are built in memory, not
  in registers — scalar replacement of aggregates is missing), fib (1.41×:
  gcc partly unrolls the recursion), hash (1.48×), matmul when gcc
  vectorizes, and Argon2 (login 2×). Linear scan has one interval per
  register (no splitting), and rax/rcx/rdx/r11 are never allocated, which
  limits register-hungry code. No auto-vectorization, no instruction
  scheduling, no `cmov`, no tail calls.
- **Vectors:** no VEX/AVX encoding yet: 256-bit operations run as two 128-bit
  SSE halves (same results, about half AVX2's throughput); `_mm256_fmadd_*`
  rounds twice. Vector arguments and results between functions follow the
  struct rules, not gcc's `__m128` in one xmm register: intrinsics (inline)
  are not affected, but such calls to gcc-built code are not compatible.
  `v op= w` evaluates `v` twice.
- **Atomics:** objects larger than 8 bytes are rejected (no lock-based
  library like libatomic).
- **Inline asm:** constraints `x`, `f`, `t`, `u` and the `asm goto` form are
  not supported.
- **Assembler:** AT&T syntax only (no `.intel_syntax`); 16-bit code only with
  the absolute addressing SIEOS's boot code uses; no segment-register
  operands other than as prefixes.
- **Debug information:** lines and functions only (no variables, types or
  call frame information; backtraces follow `%rbp` only where a frame exists).
- **AArch64:** vectors are memory values (a load and a store around each
  NEON instruction; inlined intrinsics copy their arguments), so NEON code
  is correct and 1.3–1.9× faster than scalar code under emulation, but
  slower than what keeping vectors in registers would give; no
  `va_arg` of vectors; no literal pools in the assembler;
  no branch veneers in the linker; code quality measured only under
  emulation so far (no instruction scheduling for the Cortex-A72/A76).
- **Host dependency:** sicc itself still runs on the development machine:
  `cc/sys.h` lists the C library functions it needs.

## Phase 3: sicc on SIEOS

The goal is for sicc to run inside SIEOS, so the system can rebuild itself.
The compiler needs nothing exotic, but SIEOS must provide the following. The
kernel is not changed by this work: these are requirements for it.

1. **A SIEOS C library.** sicc uses only the ISO C functions listed in
   `cc/sys.h`:
   - Memory: `malloc`, `calloc`, `realloc`, `free`.
   - Strings: `mem*` and `str*`, `strtol`/`strtoull`/`strtod`.
   - Formatting: `snprintf`/`vsnprintf`.
   - Files: `fopen`/`fread`/`fwrite`/`fclose`/`remove`, plus
     `stdout`/`stderr`.
   - Other: `qsort`, `getenv`, `exit`.

   Same on both architectures: sicc runs on AArch64 already (its emulated
   bootstrap), so a SIEOS C library is the main missing piece there too.

   A small libc on top of the fs server's protocol is enough. `malloc` can be
   a simple arena: sicc allocates mostly from its own arena and frees almost
   nothing.
2. **FPU/SSE state switching in the kernel.** Done (`kernel/arch/x86_64/fpu.c`): a thread
   gets a save area at its first floating-point instruction, then XSAVE /
   XRSTOR on every switch; programs that use floating point are built
   without `-mgeneral-regs-only`.
3. **Thread-local storage, and errno.** sicc is single-threaded, so it only
   needs `errno` as a plain global. Per-thread FS bases now exist, for a
   threaded C library.
4. **Memory and files.**
   - Peak memory is under 30 MB for the largest file, and under 10 MB for a
     typical one.
   - sicc reads whole source files and writes whole outputs, so it needs
     only open/read/write/close. No `mmap`, no `fork`/`exec`: the driver
     runs every stage in one process.
5. **A build tool.** A minimal `make` or a shell script running one sicc
   per file.
