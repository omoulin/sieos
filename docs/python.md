# Python: python

The package **python** is CPython 3.14: the interpreter, the standard library with
its C modules, and pip.

```sh
pkg install python        # with its libraries (below)
python3                   # the interactive prompt (line editing, history: readline)
python3 script.py
pip3 install requests     # pure-Python packages from PyPI
python3 -m venv ~/env     # virtual environments
```

`sys.platform` is `"sieos"`; `sysconfig.get_platform()` is `sieos-x86_64` (although
`uname -m` says `i86pc`, as on Solaris).

## What is there

Nearly the whole standard library, its C modules included:

| Module | Library (package) |
|--------|-------------------|
| `ssl`, `hashlib` (HTTPS) | OpenSSL (`openssl`) |
| `zlib`, `bz2`, `lzma` | `zlib`, `bzip2`, `xz` |
| `sqlite3` | SQLite (`sqlite`) |
| `ctypes` | libffi (`libffi`) |
| `curses`, `readline` | `ncurses`, `readline` |
| `decimal`, `pyexpat` | bundled with Python |

Threads, `subprocess`, `multiprocessing` (fork), `asyncio`, sockets (IPv4, IPv6,
Unix), `mmap`, `termios` and `venv` work.

Not there:
- `tkinter` (it needs X11);
- `dbm.gnu` and `dbm.ndbm` (`dbm` uses its SQLite backend);
- `compression.zstd` (no zstd library yet);
- the `_uuid` accelerator (`uuid` works without it);
- what SIEOS's kernel does not have: namespaces (`os.unshare`, `os.setns`),
  `os.preadv2`/`os.pwritev2`.

`select.epoll` (selectors and asyncio use it), `os.eventfd`, `os.timerfd_create`,
`os.memfd_create`, `os.pidfd_open` (asyncio waits for its children with it),
`os.splice`, `os.copy_file_range`, `os.preadv`/`os.pwritev`, `mmap.resize` and
`os.lchmod` work (milestone 74 in [abi-v2.md](abi-v2.md)).

## pip

pip comes installed (the wheel Python bundles):
- **Pure-Python packages** install from PyPI as anywhere.
- **Packages with C code** build from their source on SIEOS, with SIEOS's own gcc
  (`pip3 install` compiles them). PyPI's prebuilt wheels are for Linux, macOS and
  Windows, and do not install here.

## The libraries

The modules' libraries are packages of their own, which other software can use too:
- **libffi** 3.8: calls to C functions described at run time;
- **sqlite** 3.53: libsqlite3 and the `sqlite3` shell;
- **bzip2** 1.0.8 and **xz** 5.8: the `bzip2` and `xz` commands and their libraries;
- **ncurses** 6.6: libncursesw, `tic`, `tput`, `infocmp`, and a terminfo database
  with SIEOS's own terminal, `sieos` (what Facet's terminal understands: ANSI
  colours, cursor movement, erasing), and xterm, vt100, ansi, linux and screen;
- **readline** 8.3: line editing and history.

## Python's test suite

The package leaves Python's own tests out (`--disable-test-modules`). Run on SIEOS
(`python3 -m test -j4`, network and subprocess resources on), the whole suite runs:
484 test files in 13 minutes on 4 processors, 44,635 tests; 431 files pass, 20 have
failures (130 tests, 0.3%), 37 are skipped (other systems', or test modules the package
leaves out). Python 3.14.8, October 2026.

What still fails is mostly known and not SIEOS's to change:
- **musl, not glibc:** locales (`test__locale`, `test_locale`, `test_c_locale_coercion`,
  `test_re`'s and `test_strptime`'s locale cases), `%Z` in `time.strftime` (musl
  trusts only its own zone names);
- **the tests' view of the platform:** `os.sendfile` headers and trailers (BSD's) are
  expected on any system that is not Linux or Solaris;
- **SIEOS's limits:** paths of at most 1024 bytes (`test_tarfile`), no TCP urgent data
  (`MSG_OOB`, `test_ftplib`), a processor set of one or all processors
  (`sched_setaffinity`), no `posix_spawn` scheduling (musl's), mappings of 4 GiB
  (`test_mmap`);
- **not explained yet:** under the suite's parallel load, a test that waits for a child
  process sometimes times out (`test_events`' `test_subprocess_kill`,
  `test_multiprocessing_main_handling`); they pass in other runs.

Bugs it found in SIEOS were fixed for it: see milestone 72 in [abi-v2.md](abi-v2.md).

## How it is built

- **A Python of the same version for the build host** (`build/ports/python-host`,
  built by the Makefile) runs the cross-build's scripts and compiles the standard
  library.
- **`ports/pkgs/python/sieos.patch`** (`configure`): SIEOS is a system CPython can be
  cross-built for (`MACHDEP` `sieos`), and its shared libraries are Linux's kind.
- **SIEOS's C library** reports the main thread's stack properly
  (`pthread_getattr_np`, `libc/port/src/thread/sieos64`): Python 3.14 checks how deep
  its stack is against it.
- `configure` cannot find some answers by running programs when it cross-builds:
  the recipe gives them (`/dev/ptmx` exists; no `preadv2`, `pwritev2`,
  `process_vm_readv`, `setns` or `unshare`, which musl has but SIEOS's kernel does not).
