# Python on SIEOS

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

`/bin/python` is [MicroPython](https://micropython.org) 1.29.0, the Python 3
language in a small interpreter, with a SIEOS port in `user/python`. It is
phase 1 of [python-plan.md](python-plan.md); CPython is for later.

```sh
python                        # the prompt: >>> and ... lines, Ctrl-D to leave
python primes.py 1000         # a script; sys.argv = ['primes.py', '1000']
python -c print(2**100)       # CODE: the words after -c, joined by spaces
python --version
```

The desktop has it in its menu (`/etc/apps/python.app`: a terminal).
Two examples are in `/src/python`: `primes.py N` and `words.py FILE...`.

The shell splits a command line at spaces and removes double quotes, so in
`python -c` use single quotes for strings: `python -c print('a b')`. A
program's command line (with its folder) has room for 256 bytes.

## What there is

- **The language:** what MicroPython has at its "extra features" level:
  classes, generators, `async`/`await`, f-strings, `:=`, big integers,
  floats (doubles) and complex numbers, sets, slices, exceptions with
  tracebacks, `help()`, `input()`.
- **Modules:** `sys` (`argv`, `path`, `exit`, `stdin`/`stdout`/`stderr`,
  `platform` = `'sieos'`), `os`, `time`, `math`, `json`, `re`, `struct`,
  `binascii`, `hashlib`, `random`, `collections` (`namedtuple`, `deque`,
  `OrderedDict`), `heapq`, `errno`, `io` (`StringIO`, `BytesIO`, `open`),
  `array`, `gc`, `micropython`, `socket`, `ssl`.
- **Files:** `open(name, mode)` with `r w a x + b`; read, readline,
  readlines, iteration, write, seek, tell, `with`. Text is UTF-8 and has no
  newline translation (as on Unix).
- **`os`:** `listdir getcwd chdir mkdir rmdir remove unlink rename stat
  urandom system sep`. `stat` gives the 10-tuple (mode, ino, dev, nlink, uid,
  gid, size, atime, mtime, ctime; times in seconds). `system(cmd)` runs
  `sh -c cmd` and returns its status.
- **`time`:** `time`, `time_ns`, `sleep` (seconds, a float), `sleep_ms`,
  `sleep_us`, `ticks_ms/us/cpu`, `ticks_diff`, `ticks_add`, `localtime`,
  `gmtime`, `mktime`. The date comes from the network server's clock (the
  clock chip; UTC), asked once.
- **`hashlib`:** `sha256`, `sha384`, `sha512` (`update`, `digest`,
  `hexdigest`), SIEOS's own SHA-2 (`lib/sha2.c`).
- **`socket`:** TCP and UDP over IPv4 through the network server
  (`user/lib/net.c`): `socket()`, `connect`, `bind`, `listen`, `accept`,
  `send`/`sendall`, `recv`, `sendto`, `recvfrom`, `settimeout` (None: wait
  as long as it takes), `close`, and `read`/`readline`/`write` as a stream;
  `getaddrinfo(host, port)` (DNS through netd).
- **`ssl`:** `ssl.wrap_socket(sock, server_hostname=host)`: TLS 1.3 with
  SIEOS's client (`user/lib/tls.c`), certificates checked against
  `/etc/ssl/roots` (`cert_reqs=ssl.CERT_NONE`: not checked).
- **`import`** reads `.py` files: `sys.path` is the script's folder (or the
  current one), then `/lib/python`.

## How the port works

| File | What it does |
|---|---|
| `main.c` | arguments, the prompt, running code, the console, clocks, reading files for `import`, the garbage collector's roots |
| `files.c` | `open()` and its file objects, the `os` module |
| `socket.c` | the `socket` and `ssl` modules |
| `hashlib.c` | the `hashlib` module |
| `formatfloat.c` | floats as text, with `/bin/cc`'s exact conversions (`cc/libc/fmt.c`) |
| `libc.c`, `include/` | the few C library functions and headers MicroPython wants that SIEOS's library lacks |
| `mpconfigport.h`, `mphalport.h`, `sieostime.h` | MicroPython's configuration and system hooks |
| `python.mk` | fetching MicroPython and building it (included by the Makefile) |

- **Sources:** MicroPython is not kept in SIEOS's repository. `python.mk`
  fetches the v1.29.0 release archive from GitHub once into `.hosttools/`,
  checks its SHA-256, and unpacks only `py/`, `extmod/`, `shared/`,
  `lib/libm_dbl` and `lib/re1.5`. MicroPython's scripts collect the
  interned strings, modules and roots (`genhdr/`) from the sources
  preprocessed by the host's `gcc -E`.
- **Compilers:** gcc on x86-64, sicc on AArch64, like every SIEOS program;
  built for size (`-Os`), the bytecode loop and the collector for speed
  (`-O2`). Python computes in floating point, so it is built without
  `-mgeneral-regs-only`.
- **Stacks and memory:** the interpreter runs in a thread with a 512 KiB
  stack (a program's first thread has 32 KiB); Python's frames go on a
  256 KiB "Python stack" of their own (MicroPython's `PYSTACK`: sicc has no
  `alloca`); a call chain too deep for either is a `RuntimeError`. The
  objects live in MicroPython's garbage-collected heap: 1 MiB at first,
  then more areas from `malloc` as it fills (each about doubling the
  heap), given back when empty. The collector scans the stack and the
  registers that keep values across calls (`rbx rbp r12-r15`, `x19-x29`).
- **Exceptions:** MicroPython's own non-local returns (`nlrx64.c`,
  `nlraarch64.c`).
- **The console:** output is buffered until a newline (or before reading
  and sleeping); input comes a typed line at a time from the terminal
  (`tty.c` does the editing), so the prompt has no history or completion.
- **Ctrl-C:** the terminal sends SIGINT to the job ([kernel.md](kernel.md#signals-ctrl-c));
  the interpreter's thread catches it and schedules a `KeyboardInterrupt`
  (`MICROPY_KBD_EXCEPTION`), which the interpreter raises at its next
  check: `while True: pass` stops at once. A wait it interrupts returns
  `-EINTR` and raises it there: `time.sleep`, `input()`, a socket. At the
  prompt it says `KeyboardInterrupt` and gives a new one; `try: ...
  except KeyboardInterrupt:` cleans up; uncaught, a script ends with its
  traceback (status 1).
- **Floats as text:** MicroPython's own conversion is exact only where
  `long double` is wider than `double`, which it is not on SIEOS's AArch64
  (sicc: 64 bits); `formatfloat.c` uses SIEOS's exact one on both, and
  `repr()` gives the shortest text that reads back as the same number.

Sizes of `/bin/python`: x86-64 379 KB (284 KB of code), AArch64 467 KB
(392 KB of code; sicc's code is less dense than gcc's), SIEOS's TLS and
certificate code included. A script starts with about 2 MiB of memory
(the heap and the two stacks).

## What is missing

- No `signal` module: Python code cannot catch other signals (SIGTERM
  ends python, as any program without a function for it).
- No `os.path`, `os.environ`, `subprocess`, `threading`/`_thread`,
  `select`, `zlib`/`deflate`, `datetime`, `cmath`, `binascii.crc32`; no
  `-m module`. No `.mpy` precompiled modules, no frozen modules.
- On AArch64, MicroPython's decimal-to-float parsing is its approximate one
  (64-bit `long double` again): a 17-digit literal can be one unit off in
  the last place (the shortest `repr` reads back right).
- Not yet: a `gpio` module (the plan's), MicroPython's own test suite
  under QEMU.

## Tests

`make py-test` (and `make ARCH=arm64 py-test`), `tools/pythontest.py`:
boots SIEOS on a fresh disk, then as alice on the serial console:
`python -c` with integers, floats, float formatting and `sys`; a script
(`tests/python/files.py`) that writes and reads files, lists folders,
imports a module beside it and uses json, re, struct, binascii, hashlib,
random, collections, time and math; deep recursion and a heap growing past
20 MB (`deep.py`); the two examples; an uncaught exception's traceback
and exit status; `sys.exit(3)`; the prompt with a function and a loop
typed on several lines, an exception, `input()`, Ctrl-D; HTTP from a
server on the host, TLS 1.3 to `openssl s_server`, and a listening socket
the host connects to (`net.py`). 19 checks, on both architectures.
Ctrl-C (a loop, `time.sleep`, `input()`, the prompt, `try`/`except`,
`python -c`) is in `make sig-test`.
