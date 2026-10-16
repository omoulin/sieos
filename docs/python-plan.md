# Plan: Python on SIEOS

(Phase 1 is done, without its `gpio` module yet: [python.md](python.md).)

What must be built so Python runs on SIEOS, in what order, and how we
know each step works. Two interpreters, one after the other:
**MicroPython** first (small, few needs: a working Python in days, with a
`gpio` module for the Pi), then **CPython** (the real language and its
standard library, which needs a much fuller POSIX layer).

## Where SIEOS stands today

| Python needs | SIEOS has | Gap |
|---|---|---|
| A C11 compiler for the target | sicc (x86-64, AArch64), gcc for x86-64; `cc` on SIEOS | sicc must compile CPython's ~500 kLOC: computed gotos (`ceval`'s dispatch, can be turned off), `__attribute__`s, `_Thread_local`, atomics (`stdatomic.h` exists) |
| stdio, stdlib, string | a minimal C library for sicc on SIEOS (`cc/libc/sieos.c`, `fmt.c`), `lib/string.c` | `FILE` with buffering, `ungetc`, `fseek`/`ftell`, `setvbuf`, `strtol` family complete, `qsort`/`bsearch`, `getenv`/`setenv`, `atexit`, `abort`, `locale` (C only) |
| File descriptors: `open`, `read`, `write`, `lseek`, `close`, `dup`, `fstat` | file-server handles with positional reads and writes (`fs_*`) | an **fd table** in the library: fd → handle + offset + flags; 0/1/2 on the console; `O_APPEND`, `O_CLOEXEC` ignored |
| Directories, `stat`, `getcwd`, `chdir`, `mkdir`, `unlink`, `rename`, `symlink`, `readlink`, `access`, `utime` | most of these as `fs_*` calls | POSIX names and `struct stat`/`dirent`/`opendir`, `errno` values mapped |
| `math.h` (double, float) | `fmt.c` parses and prints doubles exactly; the engine has a few float functions | a **libm**: `sin cos tan exp log pow sqrt fmod floor ceil frexp ldexp modf atan2 hypot` and friends, `isnan`, rounding modes (port musl's, ~150 functions) |
| Time: `clock_gettime`, `gettimeofday`, `time`, `localtime`, `strftime`, `nanosleep` | `sys_clock` (monotonic ns), `sys_sleep`; the wall clock is read by atlas from the RTC | a wall-clock **syscall** (the kernel reads the RTC once and keeps it), `time.h` functions, UTC only at first (no tz files) |
| Processes: `fork`/`exec`, `posix_spawn`, `waitpid`, `getpid`, `kill`, pipes | `sys_spawn`, `spawn_file`, `sys_wait`, `sys_kill`; no fork | `posix_spawn` on top of `spawn_file` (Python's `subprocess` can use it), **pipes** (a pipe server or a kernel object) to connect a child's stdin/stdout |
| Threads: `pthread_create`, mutexes, condition variables, TLS | `thread_start`, `_Thread_local`, `sys_sleep`/`sys_wake` | **pthreads**: create/join (an exit status), mutex and condvar on a kernel wait primitive (a futex-like `SYS_WAIT_ADDR`/`SYS_WAKE_ADDR`), `pthread_key_*`, `once` |
| Memory: `malloc` (big), `mmap` (anonymous) | `malloc` over `sys_brk` | anonymous **`mmap`/`munmap`** (CPython's obmalloc arenas; can fall back to malloc) — a `SYS_MAP_ANON` would be cleaner |
| Signals | none | stubs: `signal`, `sigaction`, `raise` (SIGINT from Ctrl+C would be nice later) |
| Environment | none | `environ`, `getenv`, `setenv`, passed through `spawn` (`PATH`, `HOME`, `PYTHONPATH`) |
| Sockets (optional) | `netd` and `net.h` (TCP, UDP), TLS in `tls.c` | BSD socket names over `net.h`: `socket connect bind listen accept send recv getaddrinfo`; `select`/`poll` on sockets and the console |
| Dynamic loading | none (static programs) | not needed: extension modules are linked in statically (`Modules/Setup.local`, `*static*`) |
| A console that is a terminal | the console server and atlas's terminals (line editing in `tty.c`) | `isatty`, raw mode for the REPL's line editing (`termios` minimum) — or Python's REPL with plain line input |

## Phase 1: MicroPython (about 1 to 2 weeks)

MicroPython's "minimal" and "unix" ports need little: `malloc` or a
fixed heap, `printf`, `setjmp`/`longjmp`, file reads for `import`, a
character in and out.

1. **`setjmp`/`longjmp`** in the library (x86-64 and AArch64 assembly:
   MicroPython's exceptions use them; also needed by CPython later).
2. **A SIEOS port** (`ports/sieos/` in a MicroPython tree, or `third_party/`
   here): `mphalport.c` on `con_read`/`con_write`, `sys_clock`,
   `sys_sleep`; the GC scans the stack and registers (the port's
   `gc_helper` for both architectures); a 1 to 64 MiB heap from `malloc`.
3. **Files and `import`:** the VFS on `fs_*` (`open`, `read`, `listdir`,
   `stat`); `sys.path` = `/lib/python`, the current folder.
4. **Modules:** `time`, `os` (a subset), `math` (needs the libm below,
   or MicroPython's own `libm` subset: it ships one), and a **`gpio`
   module** over `gpio.h` (`Pin(17, Pin.OUT).value(1)`, patterns).
5. **Build** with sicc for both architectures (the rule from the start:
   x86-64 and AArch64 always together); `/bin/micropython` in the image;
   the REPL in a terminal, scripts with `micropython script.py`.
6. **Tests:** MicroPython's own test suite (`tests/basics`, `float`,
   `misc`) run under QEMU on both architectures by a `make mpy-test`;
   a GPIO script on the simulated pins.

Done when: the REPL works in a desktop terminal and on the serial
console, `import` reads `.py` files from the disk, the basics and float
tests pass on x86-64 and AArch64, and a script blinks a Pi's pin.

## Phase 2: the POSIX layer (about 4 to 6 weeks)

CPython's needs, built as a library every program may use
(`libposix.a`, beside `libsieos.a`), with its own tests in C (each
function against the same test on Linux, as `tests/cc` does for sicc):

1. **The fd table** and the POSIX file calls; `stdio` rebuilt on it with
   real buffering.
2. **libm** (musl's, which is MIT-licensed and portable): checked with
   musl's libc-test math cases.
3. **Kernel additions** (small, each with a `ktest` check):
   - a wall clock (`SYS_TIME`: the RTC at boot + the monotonic clock);
   - a wait/wake on an address (futex-like) for mutexes and condvars;
   - anonymous memory (`SYS_MAP_ANON` / unmap), or obmalloc on malloc;
   - a thread's exit status for `pthread_join`.
4. **pthreads** on those: create, join, detach, mutex, condvar, rwlock
   (CPython's GIL needs mutex + condvar), keys, once.
5. **Processes:** `posix_spawn` on `spawn_file`, `waitpid`, **pipes**
   (a pipe server: two handles, a 64 KiB buffer, blocking reads), stdin
   and stdout of a child redirected to them; `environ` passed at spawn.
6. **Time functions:** `time`, `gettimeofday`, `clock_gettime`,
   `gmtime`/`localtime` (UTC), `strftime`, `nanosleep`.
7. **Stubs** where SIEOS has no such thing: signals, users and groups
   lookups (`getpwuid` from `/etc/passwd`'s format), `termios` minimum.
8. **Sockets** (BSD names over `net.h`), `select`/`poll`.

## Phase 3: CPython (about 3 to 5 weeks)

1. **Cross-build** CPython 3.13 as for WASI (the closest model: static,
   no `fork`, no `dlopen`, few signals): a `Modules/Setup.local` with the
   modules wanted, linked statically; `pyconfig.h` written by hand for
   SIEOS (`HAVE_*` from Phase 2).
2. **First with gcc for x86-64** (to separate CPython's problems from
   sicc's), then **with sicc for both architectures**: CPython is a large,
   demanding test of the compiler (computed gotos can be turned off with
   `--without-computed-gotos` until sicc supports them; it already has
   most GNU extensions CPython uses).
3. **The frozen bootstrap:** CPython builds `_freeze_module` and
   `deepfreeze` with a host Python; the frozen modules are generated on
   the development machine and compiled in.
4. **The standard library** (`Lib/`, pure Python, ~30 MiB; ~10 MiB
   without tests and IDLE) installed in `/lib/python3.13`; `.pyc` files
   written on first import (the file server handles them like any file).
5. **Modules in C** to include first: `_io`, `_sre`, `_codecs`,
   `_struct`, `math`, `cmath`, `time`, `_datetime`, `itertools`,
   `_collections`, `_functools`, `_json`, `_random`, `_bisect`,
   `_heapq`, `binascii`, `zlib` (zlib's sources are small), `_sha2` and
   the other hashes, `select`, `_socket`, `_thread`, `posix` (Phase 2's
   functions), `_posixsubprocess` (on `posix_spawn`). Later: `_ssl` (on
   SIEOS's TLS, or BearSSL), `_sqlite3` (SQLite is one C file),
   `_ctypes` (no: needs dynamic code).
6. **A `gpio` module** in C (or pure Python over a `_sieos` module
   exposing IPC calls), and a `sieos` module for the system: services,
   sia.
7. **Tests:** CPython's own `python -m test` (a list of the modules
   expected to pass, kept in the repository and grown), run under QEMU
   on both architectures (`make python-test`); startup time and
   pystone-style speed measured on x86-64, AArch64 under QEMU, and the
   Pi 4.

Done when: `python3` runs scripts and its REPL on both architectures,
`pip`-free pure-Python packages work when copied into `site-packages`,
and the agreed list of the standard library's tests passes.

## Phase 4: in the system

- **Editor and desktop:** `.py` files colored (done: the editor's
  shell/Python mode), *Run* from the editor into a terminal, a *Python*
  app on the Deskbar (the REPL in a terminal).
- **sia:** let the assistant write and run small Python scripts (with
  the user's approval), as it opens files and apps now.
- **Packages:** a `pip` that installs pure-Python wheels from a local
  folder or over HTTPS (SIEOS has `http.c` and `tls.c`), no build step.
- **Size and memory:** the image grows by ~15 MiB (stdlib without tests,
  compiled `.pyc` not shipped); a CPython process starts at ~10 MiB.

## Risks and choices

- **sicc and CPython:** the largest unknown. Mitigation: build with gcc
  on x86-64 first; every sicc bug found becomes a test in `tests/cc`.
- **No fork:** CPython copes (WASI and iOS builds have none);
  `subprocess` needs `posix_spawn`, `multiprocessing` stays out.
- **Threads:** the GIL is simple on mutex + condvar; without the
  futex-like wait, a spin-and-sleep fallback works but costs CPU.
- **Order:** MicroPython first gives users Python (and GPIO scripting)
  early, and its needs (`setjmp`, libm, files) are a subset of CPython's,
  so nothing is thrown away.

## Size of the work, in short

| Phase | Builds | Rough effort |
|---|---|---|
| 1. MicroPython | `setjmp`/`longjmp`, a port, a `gpio` module, tests | 1 to 2 weeks |
| 2. POSIX layer | fd table, stdio, libm, pthreads, pipes, `posix_spawn`, time, sockets; 4 small kernel additions | 4 to 6 weeks |
| 3. CPython | cross-build, frozen modules, stdlib, C modules, test list | 3 to 5 weeks |
| 4. Integration | editor *Run*, Deskbar app, sia, a local `pip` | 1 to 2 weeks |
