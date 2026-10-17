# Plan: a WebKit browser on SIEOS

What must be built so SIEOS has a real web browser, in what order, and
how we know each step works. The engine is **WPE WebKit** (WebKit's port
for embedded systems: no toolkit, a small platform API), in its smallest
configuration, cross-built on the development machine. Its dependencies
are built from pinned upstream releases (a URL and a SHA-256 in the
Makefile, fetched into `.hosttools/`, never kept in git), not forks.

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

## The facts this plan rests on

Measured or read in the sources, 2026-10-16:

| Fact | Value |
|---|---|
| Current WPE WebKit release | **2.54.1** (`wpewebkit-2.54.1.tar.xz`, 44.1 MiB, sha256 `0de8bdfb...29d3fe9`; 518 MiB unpacked) |
| Debian's full WPE (2.54.1, every feature on) | 31.3 MB download, 130.7 MB installed, 50 shared-library dependencies |
| Build system | CMake, Ninja, Perl, Python 3, **Ruby** (JavaScriptCore's offline assembler), gperf; `unifdef` is bundled |
| Hard dependencies of `OptionsWPE.cmake` (not optional) | GLib ≥ 2.70 (gio-unix, gthread, gmodule), HarfBuzz ≥ 2.7.4 with ICU, ICU ≥ 70.1, libjpeg, libepoxy ≥ 1.5.4, libgcrypt ≥ 1.7, libsoup 3, libtasn1, libxkbcommon, libxml2 ≥ 2.9.13, libpng, SQLite 3, threads, libwebp (demux), zlib, FreeType ≥ 2.9; Skia adds **Fontconfig ≥ 2.13** |
| Rendering | Skia (Cairo is gone: `USE_CAIRO FALSE`); with `acceleratedCompositingEnabled` and `hardwareAccelerationEnabled` off, the WebProcess draws with Skia on the CPU (`NonCompositedFrameRenderer`, no GL context) into shared-memory buffers (`RenderTargetSHMImage`) that the UI process receives as `WPEBufferSHM` |
| The platform layer | **WPEPlatform** (`wpe-platform-2.0`): three GObject classes (Display, Toplevel, View); an implementation may live *outside* WebKit, linked into the application (the docs' `tutorial-platform.md`; Igalia's GTK platform is built this way). `View.render_buffer` receives each frame |
| Compiler flags WebKit sets itself | `-fno-exceptions -fno-rtti` (C++23) |
| JavaScriptCore's JIT on x86-64 Linux | one 1 GiB executable pool reserved at start (`fixedExecutableMemoryPoolSize`; `FIXED_EXECUTABLE_MEMORY_POOL_SIZE_IN_MB` lowers it), mapped read+write+execute (`EXECUTABLE_POOL_WRITABLE`); ARM64: 128-512 MiB |
| Memory WebKit expects from the system | Linux's lazy anonymous memory: `mmap(MAP_NORESERVE)` reservations, `madvise(MADV_DONTNEED)` to give pages back (zero when touched again), `PROT_NONE` guard pages (`OSAllocatorPOSIX.cpp`, libpas) |
| IPC between WebKit's processes | `socketpair(AF_UNIX, SOCK_SEQPACKET)`, file descriptors passed with `SCM_RIGHTS`, shared memory from `memfd_create` (`ConnectionUnix.cpp`, `SharedMemoryUnix.cpp`) |
| Toolchain releases pinned | LLVM **23.1.3** (`LLVM-23.1.3-Linux-X64.tar.xz`, 2.0 GB, sha256 `ca9fe385...e71d013`; sources 179 MB), musl **1.2.6**, CMake **4.4.4** and Ninja **1.13.2** (PyPI wheels), gperf **3.3** |
| The host | Debian 13, g++ 14.2, Perl, Ruby, Python 3; no clang, cmake, ninja, gperf, meson or pkg-config; no root needed (below) |

## The decisions

**1. WPE WebKit, built the smallest way.** Every optional feature off:

```
-DPORT=WPE -DCMAKE_BUILD_TYPE=MinSizeRel -DENABLE_WPE_PLATFORM=ON
-DENABLE_WPE_LEGACY_API=OFF -DENABLE_WPE_PLATFORM_DRM=OFF -DENABLE_WPE_PLATFORM_WAYLAND=OFF
-DENABLE_WPE_PLATFORM_HEADLESS=ON (tests) -DENABLE_VIDEO=OFF -DENABLE_WEB_AUDIO=OFF
-DENABLE_MEDIA_STREAM=OFF -DENABLE_WEB_RTC=OFF -DENABLE_WEB_CODECS=OFF -DENABLE_MEDIA_RECORDER=OFF
-DENABLE_ENCRYPTED_MEDIA=OFF -DENABLE_WEBGL=OFF -DENABLE_WEBXR=OFF -DENABLE_GAMEPAD=OFF
-DENABLE_SPELLCHECK=OFF -DENABLE_SPEECH_SYNTHESIS=OFF -DENABLE_GEOLOCATION=OFF
-DENABLE_NOTIFICATIONS=OFF -DENABLE_WEBDRIVER=OFF -DENABLE_XSLT=OFF -DENABLE_PDFJS=OFF
-DENABLE_GPU_PROCESS=OFF -DENABLE_BUBBLEWRAP_SANDBOX=OFF -DENABLE_JOURNALD_LOG=OFF
-DENABLE_INTROSPECTION=OFF -DENABLE_DOCUMENTATION=OFF -DENABLE_MINIBROWSER=OFF
-DENABLE_API_TESTS=OFF -DENABLE_MHTML=OFF -DENABLE_CONTENT_EXTENSIONS=OFF
-DUSE_GBM=OFF -DUSE_LIBDRM=OFF -DUSE_ATK=OFF -DUSE_AVIF=OFF -DUSE_JPEGXL=OFF -DUSE_LCMS=OFF
-DUSE_WOFF2=OFF (FreeType's own WOFF2 if built with Brotli, else none) -DUSE_LIBHYPHEN=OFF
-DUSE_LIBBACKTRACE=OFF -DUSE_SYSPROF_CAPTURE=OFF -DUSE_SKIA_OPENTYPE_SVG=OFF
-DUSE_SYSTEM_MALLOC=ON (first; bmalloc/libpas later if measured faster) -DENABLE_JIT=ON
```

`USE_SYSTEM_MALLOC=ON` also turns off the Gigacage and libpas, which
reserve tens of GiB of address space. Video, audio and WebGL come back
as later phases, each measured.

**2. SIEOS speaks Linux to ported software.** Ported code is compiled
for the target `x86_64-linux-musl` (`aarch64-linux-musl`): it sees
`__linux__`, so WebKit, GLib, ICU, libsoup and the rest build *unchanged*,
on their Linux paths. Underneath, the C library is **musl** (MIT, ~1 MB
static, complete: stdio, wide characters, locales, iconv, regex,
pthreads, `getaddrinfo`), from its pinned release, with one change: its
system calls do not trap to a Linux kernel but call
`__sieos_syscall(n, ...)`, **the Linux personality**: a library that does
each Linux system call with SIEOS's own (the fd table, the file server,
netd, the kernel's new memory calls). Unsupported calls answer `-ENOSYS`,
as an old Linux would. This is the model of WSL 1 and Fuchsia's
starnix, at library level: the contract between the ported world and
SIEOS is one list of system calls, testable one by one, instead of
hundreds of libc functions written by hand. It is **the POSIX layer of
[python-plan.md](python-plan.md)**, shared: CPython builds on it too.
SIEOS's own programs keep `libsieos.a` and sicc's C library; nothing
changes for them.

**3. C++ is cross-compiled; sicc stays C.** clang and lld from the
pinned LLVM release, with **libc++, libc++abi and libunwind** built from
the same LLVM sources against the musl sysroot (statically). WebKit
needs no exceptions and no RTTI; libc++abi is still built with both
(libc++'s own code and a few dependencies, e.g. ICU's tools, use them),
which costs ~150 KB. One compiler targets both processors. sicc does not
need to compile C++, nor WebKit's C dependencies (all built by clang,
one ABI); it keeps compiling SIEOS itself.

**4. Static programs, one binary for WebKit's processes.** SIEOS has no
dynamic loader, and needs none: the UI process, WebProcess and
NetworkProcess are **one multi-call executable** (`/bin/web`; the role
from its arguments), so the disk holds WebKit once. Running it three
times would load its code three times (~3 × 60 MB) unless processes
started from the same image share their read-only pages: the kernel's
**shared program images** (phase 2) do that.

**5. GLib is accepted, honestly priced.** WPE has no non-GLib mode: its
run loop, IPC glue, API and networking (libsoup) are GLib. GLib + GObject
+ GIO cost ~3.5 MB of code (static, used part), plus libffi and PCRE2
(~0.3 MB). The alternative is WebKit's *generic* run loop with libcurl
networking (the shape of the Windows and PlayStation ports): no GLib, but
it is a new WebKit port to maintain (thousands of lines of platform
glue, outside upstream). Not chosen: it is a fork. If GLib proves too
costly in RAM or speed, that port is the fallback.

**6. TLS: SIEOS's own, not OpenSSL.** libsoup takes TLS from GIO's TLS
backend. Instead of glib-networking with GnuTLS (GnuTLS + Nettle + GMP +
p11-kit) or OpenSSL (~4 MB), a small GIO TLS backend (`GTlsBackend`,
client connections and a `GTlsDatabase` on `/etc/ssl/roots`) wraps
SIEOS's `tls.c` and `x509.c` (TLS 1.3 and 1.2, already used by
`fetch`, mail and git). tls.c's transport is `net_send`/`net_recv`
on the "socket" it is given: the GIO module (`user/web/giotls.c`) defines
them over the connection's base stream, so tls.c needed no callback, only
ALPN (`h2`, `http/1.1`) for HTTP/2 and its chain kept for GIO to judge.
Fallback, had that stalled: glib-networking with OpenSSL.

**7. Fonts through Fontconfig.** Skia's CMake and WebCore's font fallback
(`FontCacheSkia.cpp`) call Fontconfig directly; replacing it means
patching both. It costs ~250 KB plus Expat (~150 KB), with one small
`/etc/fonts/fonts.conf` and a few fonts in `/lib/fonts` (DejaVu or Noto
subsets, ~2-6 MB).

**8. No GL, no GStreamer at first.** Skia on the CPU, compositing off,
frames in shared memory. libepoxy is still a build dependency (headers,
and a dispatcher that finds no GL at run time); no Mesa.

## The process model on SIEOS

```
  atlas (desktop) ◄── window buffer (shared memory), damage, input ──► web (UI process: the browser)
                                                                      │  WPEPlatform "sieos" (in the app)
                                     IPC: unix socket pairs (fd pass)  │
                       ┌──────────────────────────────────────────────┤
                       ▼                                              ▼
               web --webprocess                             web --networkprocess
               (HTML, CSS, JS, Skia: draws                  (libsoup, HTTP/1.1 and 2, cookies in SQLite,
                frames into shared memory)                   cache, TLS via tls.c) ──► netd
```

What SIEOS must provide for it, and where it lives:

| WebKit needs | SIEOS provides | Where |
|---|---|---|
| lazy anonymous memory: reserve, commit on touch, decommit (`mmap`, `munmap`, `mprotect`, `madvise`) | memory regions with pages given on first touch (also when the kernel copies a message into them) | kernel, phase 1 |
| executable memory for the JIT (read+write+execute pool) | `PROT_EXEC` on anonymous memory, asked explicitly | kernel, phase 1 |
| futexes (musl's mutexes, condition variables, `pthread_join`) | wait/wake on an address (keyed by the physical page: works across shared memory too) | kernel, phase 1 |
| threads: TLS, their own ids, a cleared word at exit | `SYS_THREAD_SELF` (id, and the address cleared and woken when the thread ends) | kernel, phase 1 |
| big stacks (8 MiB main thread, 1-8 MiB per thread) | lazily mapped regions; the start-up code moves `main` onto one | personality, phase 3 |
| shared memory (`memfd_create`, `mmap(MAP_SHARED)`), passed between processes | shared memory objects: created by one process, granted to another, mapped by each | kernel, phase 2 |
| `socketpair(AF_UNIX)`, pipes, `SCM_RIGHTS` | a unix-socket server (`unixd`): socket pairs, pipes, descriptors passed (socket ends, pipe ends, shared memory) | server + personality, phase 3 |
| `poll` on sockets, pipes, the event fd GLib wakes itself with | servers note readiness on the waiting process's port (a non-blocking `SYS_NOTE`); `poll` waits there with a deadline | kernel (small) + personality, phase 2-3 |
| TCP, UDP, DNS (`getaddrinfo`) | netd through BSD sockets in the personality; `getaddrinfo` asks netd | personality, phase 3 |
| files, directories, `stat`, `mmap` of files | the file server; a read-only file mapping is a copy (a shared one later, if measured) | personality, phase 3 |
| one image for three processes | shared program images (the read-only segments shared) | kernel, phase 2 |
| a window and input | atlas client windows (a shared buffer, damage rectangles, input events) | atlas, phase 7 |

## Phases

Each phase is merged when its tests pass, on x86-64 first; arm64 (QEMU
`virt`, then the Pi 5) follows from phase 4 on, built by the same clang.

### Phase 0: the host toolchain, without root (done: ~15 agent minutes)

`make wk-tools` fetches into `.hosttools/` (not in git), each archive
checked against its SHA-256 in `user/posix/posix.mk`:

| Tool | From | Kept |
|---|---|---|
| clang, lld, llvm-ar/-ranlib/-nm/-objcopy/-strip/-readelf/-size, clang's own headers | `LLVM-23.1.3-Linux-X64.tar.xz` (release binaries, statically linked) | only those files: 630 MB of the 2 GB archive (clang 282 MB, lld 213 MB), the archive then deleted |
| libc++, libc++abi, libunwind, compiler-rt builtins sources | `llvm-project-23.1.3.src.tar.xz` | `runtimes cmake libcxx libcxxabi libunwind compiler-rt libc llvm/cmake llvm/utils third-party/siphash`: 226 MB |
| CMake 4.4.4, Ninja 1.13.2 | their PyPI wheels (plain zip files: unpacked, no pip) | `.hosttools/wk/bin/` |
| gperf 3.3 | GNU release, built by the host's g++ | `.hosttools/wk/bin/gperf` |
| Meson, pkgconf (phase 4) | a wheel; a release built by gcc | `.hosttools/wk/bin/` |

*Test:* `clang --version`, `cmake --version`, `ninja --version`,
`gperf --version` from `.hosttools`; a C file cross-compiled for
`x86_64-linux-musl` and linked by lld.

### Phase 1: kernel, memory and waiting (done: ~30 agent minutes)

New system calls (`include/mk/abi.h`), each with a `ktest` check:

| Call | What |
|---|---|
| `SYS_MMAP(addr, size, prot)` | an anonymous region (`PROT_READ/WRITE/EXEC`, `PROT_NONE` for guards, `MAP_FIXED` with an address); its pages are given (zeroed) on first touch |
| `SYS_MUNMAP(addr, size, flags)` | removes a region (or part); `UNMAP_KEEP`: gives back its pages but keeps the region (`MADV_DONTNEED`) |
| `SYS_MPROTECT(addr, size, prot)` | changes rights (pages present are remapped, other CPUs shot down) |
| `SYS_FUTEX(op, addr, value, ns)` | wait while `*addr == value` (with a time limit), wake n waiters |
| `SYS_THREAD_SELF(clear)` | the caller's thread id; when it ends, the kernel writes 0 at `clear` and wakes a waiter there |

Regions are a sorted list per process (32 bytes each), between 16 TiB
and the device-mapping area. A page fault in a region whose rights allow
the access maps a zeroed page and the instruction runs again; any other
fault still ends the process. The kernel's own copies (IPC, system
calls) give pages the same way, so a message may land in fresh memory.
W^X stays the rule for programs' files; a process may still ask for
`PROT_EXEC|PROT_WRITE` on its own anonymous memory (JavaScriptCore's JIT
pool).

*Tests:* `ktest vm` (reserve 1 GiB and touch 3 pages: 3 pages used;
decommit; `PROT_NONE` and rights faults end a child; mprotect read-only;
an executable page runs generated code; a message received into an
untouched page), `ktest futex` (8 threads, a futex mutex and a counter;
timeouts; the thread-exit wake), on both architectures. `test`,
`gui-test`, `kernel-test` unchanged. *Size:* kernel +2-3 KB (measured:
+4.7 KB of code on x86-64, +5.1 KB on arm64).

### Phase 2: kernel, sharing (done: ~30 agent minutes)

1. **Shared memory objects**: `SYS_SHM_CREATE(id, size)`, where id is a
   random 63-bit number the creator chose: knowing it is the right to
   use the object (`SYS_SHM_MAP`, `SYS_SHM_HOLD` for another process,
   `SYS_SHM_CLOSE`), so a program passes it like a file descriptor and no
   grant list is needed. Pages zeroed, counted once (on the creator's
   quota), freed when the last hold and mapping are gone. Also the
   `SYS_GRANT` of [gpu-plan.md](gpu-plan.md) phase 3, by another road: one
   mechanism, not two.
2. **Notes**: `SYS_NOTE_WORD(addr)`: a process names a 32-bit word of its
   own; `SYS_NOTE(pid)` adds one to that word and wakes its futex waiters
   (root, or the same user). The base of `poll`: the waiter sleeps with a
   futex time limit, so no receive deadline was needed (the earlier idea:
   port notes and `SYS_RECV` with a deadline).
3. **Shared program images**: `SYS_SPAWN` with `SPAWN_SHARE` from an ELF
   file the caller holds in a shared memory object: read-only segments are
   mapped from the object's pages (shared), writable ones copied.
   `/bin/web` is read once and its code is in RAM once, however many
   processes run it. Arguments and the environment travel through unixd
   (phase 3), so the kernel's 256-byte argument string stays as it is.
4. **The wall clock**: `SYS_TIME`: the real-time clock read once (x86-64:
   the CMOS clock; arm64: a PL031 if the device tree has one; the Pis: 0,
   and root sets it), plus the monotonic clock.

*Tests:* `ktest shm`: two processes see each other's writes and a futex
across them; the memory comes back when both are done; a note wakes its
word's waiter; the wall clock is after 2026; two processes started from
one image share its code (measured below).

### Phase 3: the Linux personality and musl: libposix (done: ~1 agent hour)

`user/posix/`: musl 1.2.6 unpacked from its release, plus our files:
`syscall_arch.h` for each processor (the one musl file replaced: calls
become `__sieos_syscall`), the start-up code (a Linux-style initial stack:
argc, argv, envp, auxv with `AT_PHDR`, `AT_PAGESZ`, `AT_RANDOM`, from
SIEOS's arguments; then `main` on a lazily mapped 8 MiB stack), and the
personality itself, by area:

| Area | Linux calls |
|---|---|
| memory | `mmap munmap mprotect madvise brk mremap(-ENOSYS) memfd_create` |
| threads | `clone`(threads only) `futex set_tid_address gettid exit exit_group arch_prctl/TPIDR sched_yield sched_getaffinity` |
| time | `clock_gettime clock_nanosleep nanosleep gettimeofday` (the wall clock from the RTC, below) |
| files | `openat read write pread pwrite readv writev lseek close fstat newfstatat statx getdents64 mkdirat unlinkat renameat ftruncate fsync getcwd chdir readlinkat faccessat fcntl dup3 ioctl(TIOCGWINSZ, FIONBIO)` |
| processes | `getpid getppid kill rt_sigaction rt_sigprocmask(no-op) wait4 uname getuid geteuid getgid prlimit64`; `posix_spawn` on `SYS_SPAWN` (musl's `fork`: `-ENOSYS`) |
| pipes, unix sockets | `pipe2 socketpair sendmsg recvmsg (SCM_RIGHTS) shutdown` through `unixd`; `eventfd2` local |
| network | `socket connect bind listen accept4 sendto recvfrom getsockopt setsockopt getsockname getpeername` on netd (TCP, UDP); `getaddrinfo` replaced by one that asks netd |
| waiting | `poll ppoll pselect6` (and `epoll` as a later, measured need) |
| other | `getrandom` (`SYS_RANDOM`), `/dev/null`, `/dev/urandom`, `/proc/self/exe` |

The kernel gains a **wall clock** (`SYS_TIME`: the RTC read once at boot
plus the monotonic clock), the python plan's item.

*Tests* (`make posix-test`, in QEMU, both architectures): C programs in
`tests/posix/` built against libposix, each printing `ok`/`FAIL` lines:
stdio and `printf` of doubles, `malloc` of 1 GiB untouched, files and
directories, time, pthreads (mutex, condvar, join, TLS, 64 threads),
pipes between a parent and a `posix_spawn`ed child, a socket pair with a
descriptor passed, `poll` on a pipe and a socket, TCP to the host
(the net-test's echo server), `getaddrinfo`. Then musl's own
`libc-test` (its functional and regression sets) as a list of expected
passes, grown over time.

### Phase 4: C++ and the dependencies (done: ~2 agent hours)

1. **libc++, libc++abi, libunwind** built by CMake against the musl
   sysroot (static, `LIBCXX_HAS_MUSL_LIBC=ON`, no shared libraries).
   *Test:* `make cxx-test`: a C++ program (strings, `std::vector`,
   `std::map`, `std::thread`, `std::mutex`, an exception thrown and
   caught, `std::chrono`) prints its results in QEMU.
2. **The dependencies**, each from its pinned release, by its own build
   system (CMake or Meson cross files in `user/web/cross/`), installed
   into the sysroot. In order, with the expected size of what WebKit
   uses (static, `-Os`, stripped):

| Library | Release | Build | ~Size | Why |
|---|---|---|---|---|
| zlib | 1.3.x | CMake | 80 KB | everything |
| libffi | 3.5.x | autotools | 40 KB | GObject |
| PCRE2 | 10.4x | CMake | 300 KB | GLib's regex |
| GLib (+GObject, GIO) | 2.86.x | Meson (`-Dlibmount=disabled -Dselinux=disabled -Dxattr=false -Dintrospection=disabled -Dtests=false`) | 3.5 MB | WPE itself |
| ICU | 78.x | autotools | 3 MB code + **data trimmed** to ~5 MB (ICU's data filter: break iterators, normalization, the collation and locales wanted, the legacy converters web pages need) | text, Intl |
| HarfBuzz | 12.x | Meson (`-Dicu=enabled -Dfreetype=enabled`, no cairo, glib) | 1 MB | text shaping |
| FreeType | 2.14.x | CMake (zlib, png; brotli for WOFF2 later) | 600 KB | glyphs |
| Expat, Fontconfig | 2.7.x, 2.17.x | CMake, Meson | 400 KB | fonts |
| libpng, libjpeg-turbo, libwebp | 1.6.x, 3.1.x, 1.6.x | CMake | 900 KB | images |
| SQLite | 3.5x (amalgamation) | one C file | 900 KB | cookies, storage |
| libxml2 | 2.15.x | CMake (no python, no iconv extras) | 900 KB | WebCore's XML |
| libgpg-error, libgcrypt | 1.5x, 1.11.x | autotools (`--disable-asm` first) | 1 MB | WebCrypto, hashes |
| libtasn1 | 4.20.x | autotools | 80 KB | WebCrypto keys |
| libepoxy | 1.5.10 | Meson (`-Degl=yes -Dglx=no -Dx11=false`) | 400 KB | GL dispatch (inert) |
| libxkbcommon | 1.12.x | Meson (no X11, no Wayland, no data: our keymap) | 300 KB | key symbols |
| libpsl, nghttp2 | 0.21.x, 1.6x | Meson, CMake | 250 KB | libsoup |
| libsoup | 3.6.x | Meson (`-Dtls_check=false -Dgssapi=disabled -Dntlm=disabled -Dbrotli=disabled -Dtests=false`) | 900 KB | HTTP |
| GIO TLS on tls.c | ours | Makefile | 60 KB | HTTPS |

   *Test:* each library's own test suite where it has one that runs
   without a host (GLib's `glib/tests` subset, SQLite's `speedtest1` as
   a smoke test, ICU's `cintltst` subset, libsoup's `uri-parsing`), as
   `make webdeps-test`.

### Phase 5: JavaScriptCore alone (estimate: 2-4 agent hours)

WebKit configured with `-DPORT=JSCOnly` first (no WebCore: WTF, bmalloc
off, JavaScriptCore and its `jsc` shell), against the same sysroot. The
LLInt (offline assembler, x86-64 and ARM64) and every JIT tier: baseline,
DFG and the FTL on B3 (no LLVM). The FTL was to be off, but this release
does not build without it (a missing include in that configuration), and
it is upstream's Linux default.

*Tests:* `make jsc-test`: `jsc` runs JavaScriptCore's `stress` tests (a
list of expected passes) and the SunSpider/JetStream 2 subsets in QEMU;
`--useJIT=0` against JIT timings (the JIT's worth, measured). *Size:*
`jsc` ~15-20 MB.

### Phase 6: WebKit headless (estimate: 4-8 agent hours)

`-DPORT=WPE` with the options above, `ENABLE_WPE_PLATFORM_HEADLESS=ON`.
The multi-call `/bin/web`; a test program loads a page from a file or
the network and writes the frame it receives (`WPEBufferSHM`) to a PNG.

*Tests:* `make web-test`: local pages (text, CSS layout, images, a
script that changes the page, a form) rendered and compared with
reference pictures made by the same WPE version on Linux (headless,
CPU rendering); `https://` pages through the test's proxy (no outside
network in tests); a WebProcess killed mid-page → the UI process sees it
and reloads. *Measures:* start-up time, time to the first frame,
memory per process.

### Phase 7: the browser (estimate: 4-8 agent hours)

1. **Atlas client windows** (`user/atlas`): a program asks for a window
   (`ATLAS_WIN_OPEN`: size, title) and gets a shared memory buffer;
   `ATLAS_WIN_DAMAGE` (rectangles) makes atlas copy those parts into its
   picture (a new display-list item: an image from shared memory);
   input for that window comes back as answers to the program's waiting
   `ATLAS_WIN_EVENT` call (keys, pointer, scroll, resize, close, focus).
   Useful beyond the browser (any program drawing its own window).
2. **The SIEOS WPE platform** (`user/web/platform/`, public API only):
   `WPEDisplaySieos` (connects to atlas), `WPEToplevelSieos` (an atlas
   window), `WPEViewSieos` (`render_buffer`: the SHM frame copied to the
   window buffer, damage only; input turned into `WPEEvent`s), a
   `WPEKeymap` from Atlas's keys (no xkb data files).
3. **The browser** (`user/web/browser.c`, C on WPE's GLib API): an
   address bar (a URL, or words searched), back, forward, reload/stop,
   a progress line, the page title on the window's tab; links opened in
   the same window, `Ctrl-L`, `Ctrl-R`, `Alt-←/→`; downloads to
   `~/Downloads` (`WebKitDownload`), cookies and cache in `~/.web/`;
   certificates checked against `/etc/ssl/roots`. Tabs later. A
   `Web.app` in `/etc/apps`, the Deskbar icon.

*Tests:* `gui-test` gains a browser step: open the app, load a local
page and a page from the test's HTTP server, check the screenshot's
known pixels; type in a form; back and forward.

### Phase 8: speed, arm64, and the rest (estimate: 2-4 agent hours, then as needed)

The JIT measured on and off; bmalloc/libpas instead of musl's malloc
(WebKit is malloc-heavy: musl's allocator is small but slow) measured;
`-O2` for JavaScriptCore and Skia's raster code, `-Os` elsewhere; the
Pi 5 (ARM64 JIT, NEON in Skia); then, each only if measured worth it:
compositing with the iGPU (GL through Mesa: large), video (GStreamer or
a small FFmpeg-based media player), WebAssembly (on by default with the
JIT: kept if it costs little).

## Size budget

To be measured at each phase; the targets:

| Part | Target | Note |
|---|---|---|
| libposix (musl + personality) in a program | 100-800 KB | only what it uses |
| libc++ + libc++abi + libunwind | 0.6-1 MB | |
| the dependencies (table above) | ~13 MB | ICU data the largest |
| JavaScriptCore | 12-18 MB | LLInt + baseline + DFG JITs |
| WebCore + WebKit (UI, Web, Network processes' code) + Skia | 35-50 MB | |
| **`/bin/web`** (one multi-call binary, stripped) | **60-80 MB** (25-30 MB compressed) | Debian's full WPE: 130.7 MB installed |
| fonts, `/etc/fonts`, ICU data if kept as a file | 3-8 MB | |
| kernel | +4-6 KB | phases 1-2 |
| atlas client windows | +3-4 KB | |

Memory (RAM), one tab, with shared program images:

| Process | Target | Without image sharing |
|---|---|---|
| code of `/bin/web`, once | 60-80 MB | ×3 |
| UI process (browser, platform) | 15-25 MB | |
| NetworkProcess | 15-25 MB | |
| WebProcess, a simple page | 40-80 MB | |
| WebProcess, a heavy site (news, mail) | 150-400 MB | as on Linux |
| **a first page, all in** | **~150-200 MB** | ~300-350 MB |

SIEOS boots in 3.1 MiB; the browser is by far its largest program. A
machine with 4 GiB (the AI plan's budget, the Pi 5's smallest) holds the
browser and sia's small model at once.

## Risks

| Risk | What we do |
|---|---|
| The Linux personality grows without end | only what ported code calls, found by building and running it (an unknown call logs its number once, answers `-ENOSYS`); each call tested |
| musl's malloc is slow under WebKit | measured in phase 6; bmalloc/libpas next (they need the lazy memory of phase 1, which they get) |
| Static linking of WebKit (it expects shared libraries: `libWPEWebKit-2.0.so`) | WebKit's CMake builds its parts as object libraries already (`WebCore_LIBRARY_TYPE OBJECT`); the main library is linked into `/bin/web` instead of a `.so` (a small CMake change, kept as a patch file applied to the release, upstreamable) |
| WebKit patches needed for SIEOS anyway | kept as small patch files in `user/web/patches/`, applied to the pinned release at build time, each with its reason; the goal is zero. Expected: the `.so` → executable link, and maybe process launching (`ProcessLauncherGLib` uses `g_subprocess`/`posix_spawn`: works through the personality) |
| GLib's main loop and `poll` on SIEOS: latency | `poll` is one call per server involved plus one wait; measured in phase 3 (a ping-pong through two unix sockets) |
| Build times (WebKit: ~1-2 h on this 28-thread host, 25 GB of build files) | out of the normal `make`; `make web` only when asked; ccache later |
| The JIT needs RWX memory | allowed for anonymous memory a process asks for; later, JavaScriptCore's separated write mapping (a second, non-executable mapping of the same shared memory) restores W^X |
| arm64 under QEMU is slow (emulated) | functional tests only there; speed on the Pi 5 |
| A WebKit release a year changes dependencies | pinned versions; moving to a new release is a deliberate step with the tests above |

## Timeline

In agent hours (Claude Code's working time, not a person's weeks): phases
0-4 were first planned at 9-16 weeks and took about 3.5 hours. The rest
is estimated the same way.

| Phase | Builds | Agent time |
|---|---|---|
| 0 | the host toolchain, fetched | ~15 min (measured) |
| 1 | kernel: lazy memory, `mprotect`, futexes, thread ids | ~30 min (measured) |
| 2 | kernel: shared memory, notes, shared program images | ~30 min (measured) |
| 3 | libposix: musl + the Linux personality, `unixd`, posix-test | ~1 h (measured) |
| 4 | C++ runtime; the ~25 dependencies, GIO TLS on `tls.c` | ~2 h (measured) |
| 5 | JavaScriptCore and `jsc` | 2-4 h |
| 6 | WPE WebKit headless, pages to PNG | 4-8 h |
| 7 | atlas client windows, the SIEOS platform, the browser | 4-8 h |
| 8 | speed, arm64, the Pi 5 | 2-4 h, then as needed |
| | **a usable browser (phases 0-7)** | **about 15-25 agent hours** |

The real bottlenecks are not the writing but the waiting, counted apart:

- **Compiling** on this 28-core host: JavaScriptCore from scratch
  ~10-20 minutes per architecture; all of WebKit ~1-1.5 hours per
  architecture (each from-scratch build of phases 5-8 pays it; incremental
  builds are minutes). The dependencies: ~10 minutes per architecture.
- **QEMU test runs**: the full set ~10 minutes on x86-64 (KVM); arm64 is
  emulated, several times slower, so JavaScriptCore's and WebKit's tests
  there are subsets.
- **The user's hardware test**: the laptop (from the USB key) and the Pi 5
  in phase 8, at the user's pace.

## What you will be asked to do

- Nothing as root: every tool is fetched into `.hosttools/` with its
  checksum (`make wk-tools`: 2.2 GB downloaded once, 943 MB kept). If you
  prefer Debian's packages, `sudo apt install clang lld cmake ninja-build
  gperf meson pkgconf` works too (the Makefile uses them when present
  and recent enough).
- Disk space: ~40 GB for WebKit's build directory and the sysroot
  (`build-web/`), outside `build/`.
- Let the long builds run (phases 5-6: about an hour each time WebKit is
  rebuilt from scratch).
- Try the browser on the laptop (from the USB key) and the Pi 5 in
  phase 8, and tell which sites matter to you: they become tests.

## Progress

| Phase | State |
|---|---|
| 0 | **done** (2026-10-16): `make wk-tools` (`user/posix/posix.mk`): clang/lld 23.1.3, CMake 4.4.4, Ninja 1.13.2, gperf 3.3 and LLVM's runtime sources, each checked by SHA-256, in `.hosttools/wk` (943 MB); about 3 minutes on this host |
| 1 | **done** (2026-10-16): `kernel/vm.c` (regions, page faults, the kernel's copies into untouched pages, `SYS_MMAP`, `SYS_MUNMAP`, `SYS_MPROTECT`, `SYS_FUTEX`, `SYS_THREAD_SELF`); `ktest vm` and `ktest futex` pass on x86-64 and arm64 (built by gcc and by sicc); kernel code 54,787 → 59,459 bytes (x86-64), 80,680 → 85,736 (arm64); `test`, `kernel-test`, `gui-test` (x86-64) and `test`, `kernel-test` (arm64) pass; IPC round trip unchanged (375-491 ns) |
| 3 (first part) | **done** (2026-10-16): **libposix** (`make posix`): musl 1.2.6 from its release with its system calls replaced (`user/posix/syscall_arch.h`, and `clone`, the thread pointer and cancellation in `user/posix/musl/`), the Linux personality (`user/posix/linux.c`, 770 lines: the fd table on the terminal, the file server and `/dev/null`, `/dev/zero`, `/dev/urandom`; files, directories, links, stat, `getdents64`; `mmap` (anonymous and private file copies), `mprotect`, `madvise`, `brk`, `mremap`'s stack probe; threads through `clone`, `futex`, `set_tid_address`; clocks and sleeps; identity; `uname`, `getrandom`, `sysinfo`, `sched_getaffinity`), the start-up code (`user/posix/start.c`: the Linux initial stack, an 8 MiB main stack given on first touch). Installed with musl's headers in `$(B)/posix/sysroot/usr`. `make posix-test` (stdio, files, threads, memory: 4 programs, 42 checks) passes on x86-64 and arm64. Sizes: the personality 14.4 KB of code; a C "hello" 46.9 KB (stripped), the tests ~100 KB. Not yet: pipes and unix sockets (`unixd`), sockets on netd, `poll`, `posix_spawn`, signals through `rt_sigaction`, a kernel wall clock (`CLOCK_REALTIME` is the build date plus the time since boot) |
| 4 (first part) | **done** (2026-10-16): the C++ runtime (`make cxx`): libc++, libc++abi (no demangler in `terminate`: -98 KB), libunwind and compiler-rt's builtins, built by CMake from LLVM 23.1.3's sources for each processor (2,300 files, ~2 minutes). `make cxx-test` (strings, containers, virtual calls, RTTI, exceptions through 20 frames, rethrow, 16 `std::thread`s with mutex and condition variable, atomics, chrono, iostreams, `std::filesystem`, a static destructor at exit) and `cxxtest-stress` (200 rounds of 16 `std::thread`s with a mutex and a condition variable) pass on x86-64 and arm64. The stress test found the personality's one real bug so far: musl's condition variables hand a woken waiter on with `FUTEX_REQUEUE` and have no fallback when it fails, so requeueing is done as a wake (a hang about one run in eight on arm64, none since in 12 runs). A C++ "hello" with `std::vector<std::string>`: 101 KB stripped; the test (iostreams, filesystem): 490 KB |

| 2 | **done** (2026-10-17): shared memory objects, notes, shared program images, the wall clock (`kernel/vm.c`, `kernel/arch/*/rtc.c`, `SYS_SHM_CREATE/MAP/HOLD/CLOSE`, `SYS_NOTE_WORD`, `SYS_NOTE`, `SYS_TIME`, `SPAWN_SHARE`). `ktest shm` passes on both architectures (gcc and sicc builds): ktest's own 46 KiB image, 35 pages for a process of its own, 26 for each of two sharing it (its 9 pages of code once). Kernel code: 62,787 → 67,427 bytes on x86-64 (+4.6 KB), 87,032 → 91,656 on arm64 (+4.6 KB): `vm.o` 4.0 → 7.3 KB, `rtc.o` 0.6 KB |
| 3 | **done** (2026-10-17): the rest of libposix. **unixd** (`user/unixd`, 455 lines, 33 KB, port "unix", on demand): pipes, unix socket pairs (stream and seqpacket), descriptors passed in messages (socket ends and memfds: SCM_RIGHTS), what posix_spawn hands a child (a stash claimed with a cookie), `UNIX_POLL`, holders that end without a word found every 2 s. **The personality** (`linux.c` 870 lines, `sock.c` 500, `spawn.c` 300; 31 KB of code): pipes, `socketpair`, `sendmsg`/`recvmsg` with `SCM_RIGHTS`, memfd (`memfd_create`, `ftruncate`, `mmap(MAP_SHARED)`), eventfd, TCP and UDP sockets on netd (non-blocking connect by a thread of its own, `getsockopt(SO_ERROR)`), `poll`, `ppoll`, `select`, `pselect6` (each server asked once, `NET_POLL` added to netd; nothing ready: sleep on the note word), `posix_spawn`/`posix_spawnp` (file actions, folder, environment; the image cached in shared memory per path), `wait4`, `rt_sigaction` (a trampoline over `SYS_SIGNAL`), `CLOCK_REALTIME` from `SYS_TIME`; musl's resolver asks the DNS server netd learnt. `make posix-test`: 6 programs (stdio, files, threads, memory, ipc: 18 checks, net: 6 checks against echo servers on the host and a DNS lookup) pass on x86-64 and arm64. A C "hello": 46.9 → 63.4 KB (the personality grew); the fd table is now mapped memory given on first touch, not 80 KB of every program's `.bss`. Not yet: named unix sockets (`bind` to a path), passing files or netd sockets between processes, `fork`/`exec` (never: `posix_spawn` instead), `epoll`, signal masks |

| 4 (libraries, first part) | **done** (2026-10-17): `user/web/deps.mk`. Host tools: Meson 1.12.1 (wheel), pkgconf 3.0.9 (built by gcc). The cross toolchain: `$(B)/web/bin/cc` and `c++` (clang for the processor, the sysroot, compiler-rt's builtins and its crtbegin/crtend, musl's crti/crtn, static, lld with SIEOS's segment layout and 4 KiB pages: arm64's default 64 KiB padding took a C "hello" from 72 to 199 KB), a CMake toolchain file and a Meson cross file, so each library builds with its own build system unchanged. Built, static, `-Os`: **zlib 1.3.2** (69 KB code+data), **libffi 3.8.0** (28 KB; no static trampolines), **PCRE2 10.49** (8-bit, no JIT: 340-370 KB, its Unicode tables most of it), **GLib 2.90.1** (GLib 1.07 MB, GObject 257 KB, GIO 1.47-1.49 MB, GModule 5 KB; no introspection, NLS, libmount, SELinux, xattr, tests). `make webdeps-test` passes on x86-64 and arm64: zlib's own `example`, and our wtest-ffi (3 checks), wtest-pcre2 (4), wtest-glib (14: main loop, threads, a thread pool, a GObject with a property and a signal through libffi, GVariant, GRegex, SHA-256, GDateTime, files, a key file, a socket pair passing a descriptor, GSubprocess with its output through a pipe and its end seen by a child watch). A program using all four: 2.38 MB stripped (x86-64), 2.41 MB (arm64). What GLib needed of SIEOS: pidfds and `waitid(P_PIDFD)` (and the kernel noting a parent when a child ends), a small epoll (an interest list over poll: GLib asks it whether a descriptor can be polled at all), `prctl`, `fallocate` (refused), two tiny Linux headers (`linux/types.h`, `linux/wait.h`). Found: posix_spawn applied close-on-exec before the file actions (GLib's dup2 of its pipes failed). Noted for later: GLib spawns that close every other descriptor need fork (GLib's own choice); WebKit's launcher inherits descriptors (`G_SUBPROCESS_FLAGS_INHERIT_FDS`) and so gets posix_spawn. WebKit's build will need GLib's tools on the host (`glib-compile-resources`, `glib-mkenums`, `gdbus-codegen`): a native GLib built by Meson, phase 6 |

| 4 (libraries, the text stack) | **done** (2026-10-17): **ICU 78.3**, its data built from source by a host build of the same ICU (its tools), filtered by `user/web/icu-filter.json` (English and French; no line-breaking dictionaries, legacy converters but UTF-8, Latin-1 and Windows-1252, transliteration, spellout, confusables, character names, StringPrep) and linked in: **3.5 MB of data** instead of ICU's 31 MB, code 1.46 MB (common) + 2.04 MB (i18n) before the linker drops what is not used; a program using it: 5.9 MB stripped. **FreeType 2.15.0** (593 KB; zlib, no PNG/Brotli/HarfBuzz yet). **HarfBuzz 14.6.0** with ICU and FreeType (1.35 MB; no GLib, Cairo, subsetter, tools, and no C++ runtime needed). **Expat 2.9.0** (131 KB). **Fontconfig 2.18.3** (385 KB; Expat, no iconv, tools, cache at build). **DejaVu 2.37** fonts (Sans, Serif) for the tests and, later, the browser. New checks: wtest-icu (8: case, NFC, French word breaking and collation, IDNA, a French date, Latin-1, Japanese left out), wtest-text (5: Fontconfig finds sans-serif and serif, FreeType renders an "A", HarfBuzz shapes the "ffi" ligature, kerning, Hebrew right to left). libposix gained `statfs`/`fstatfs` and `link`. `make webdeps-test` passes on x86-64 and arm64, also from fresh build directories. Building the text stack: ~6 minutes (ICU's host and cross builds), the rest seconds |

| 4 (libraries: images, SQLite, libxml2) | **done** (2026-10-17): **libpng 1.6.59** (152 KB, its SIMD filters), FreeType rebuilt with PNG (colour bitmaps: emoji), **libjpeg-turbo 3.2.0** (588 KB with its SIMD code: NASM 3.02 fetched and built on the host for x86-64's, intrinsics on arm64; no TurboJPEG API, tools), **libwebp 1.6.0** (385 KB + demux 7 KB + sharpyuv 13 KB; no tools), **SQLite 3.54.1** (the amalgamation; 820 KB; no loadable extensions, deprecated calls, FTS; exclusive locking by default, so WAL needs no memory shared between processes: one process owns a database, as in WebKit), **libxml2 2.15.4** (869 KB; no Python, ICU, zlib, LZMA, HTTP, catalogs, legacy calls, Schematron, programs). New checks: wtest-images (5: a picture written to PNG, JPEG, lossy and lossless WebP in memory and read back, the WebP demuxer), wtest-sqlite (8: 1000 rows in one transaction with an index, a rollback, WAL, reopened, an update; libxml2 parsing, XPath, writing, a broken document refused). libposix gained `fchown`, `chown`, `fchownat` (SQLite, run as root, keeps a journal's owner). Verified from fresh build directories on both architectures |

| 4 (libraries: WebCrypto, GL headers, keys) | **done** (2026-10-17): **libgpg-error 1.61** (115 KB), **libgcrypt 1.12.4** with only what WebCrypto uses (AES; RSA and ECC; SHA-1, SHA-2, and SHA-3, which its message digest code calls unconditionally; PBKDF2): 695 KB instead of 1.49 MB with every algorithm (arm64: 564 KB), a program using it 771 KB instead of 1.57 MB (arm64: 644 KB); **libtasn1 4.21.0** (51 KB). **libepoxy 1.5.10** (EGL only, no GLX, X11; 575 KB of dispatch tables, of which a program keeps what it calls) and **libxkbcommon 1.13.2** (309 KB; no X11, Wayland, keymap files, registry, tools; Bison 3.8.2 built on the host for its parser). Neither can be left out: WPE's build requires both (`find_package(Epoxy)`, `find_package(XkbCommon)` unconditionally), WebKit includes the EGL and OpenGL ES headers directly even with no GL, and its key events are xkb key symbols. The Khronos headers (EGL, GLES2, GLES3, KHR) are taken from Mesa 26.2.5's release, nothing else of it. New checks: wtest-crypto (8: SHA-256, HMAC, AES-GCM against NIST's vector, an ECDSA P-256 key made from SIEOS's random numbers, signed, verified, wrong data refused, DER through libtasn1), wtest-input (7: key names, their characters, a keymap given as text as SIEOS's platform will give it, Shift pressed and released, libepoxy linked). Verified from fresh build directories on both architectures |

| 4 (libraries: the network) | **done** (2026-10-17): **libpsl 0.23.3** (64 KB; the list built in, no IDNA library), **nghttp2 1.70.0** (127 KB; the library only), **libsoup 3.8.0** (436 KB; no Kerberos, NTLM, Brotli, Zstandard, sysprof), and **GIO's TLS on SIEOS's `tls.c`** (`user/web/giotls.c`, 740 lines: a `GTlsBackend`, client connections with pollable streams, certificates; 99 KB with tls.c, x509.c and their crypto, of which the module 14 KB) instead of glib-networking with GnuTLS or OpenSSL. tls.c gained ALPN (`h2` offered with `TLS_H2`), `TLS_CHAIN` (the chain kept and judged by the caller, so GIO's `accept-certificate` can overrule it; signatures still checked by tls.c), `tls_pending`, and lost its shared buffers (a record buffer per connection; x509.c's chain and pubkey.c's RSA buffers on the stack, its curves made once under a lock): handshakes now run in several threads at once. `tests/net/tls.py`: 31 checks against openssl s_server (4 new: ALPN). New check: wtest-soup (13: against servers on the host, `tests/webdeps/servers.py`: HTTP/1.1; HTTPS with a test CA, TLS 1.3, ALPN, 1 MiB down and 256 KiB up; HTTP/2 by ALPN, 256 KiB up; a self-signed certificate refused, then accepted through `accept-certificate`; public suffixes; cookies kept in SQLite). A program using libsoup over TLS: 3.4 MB stripped. Found: `poll` waiting to read a socket that could be written (or the reverse) never woke: netd and unixd answered "ready" (for the other direction) and so noted no one. `NET_POLL` and `UNIX_POLL` now take what the caller waits for (a new ptest-ipc check). Also found by wtest-soup run 30 times in a row: the GIO streams outliving their connection (libsoup keeps them: now a weak reference), and a pollable source's callback signature. Phase 4 is complete |

| 5 | **done** (2026-10-17): `user/web/jsc.mk`: WebKit's JSCOnly port from the WPE WebKit 2.54.1 release, unchanged, static, `-O2`, musl's malloc, no sampling profiler, inspector or API tests; `make jsc`: 1 m 45 s from scratch on this host (x86-64), 50 s (arm64). **`jsc`: 35.8 MB stripped (x86-64), 33.9 MB (arm64)**: JavaScriptCore 11.7 MB, its DFG 3.2 MB, WTF 2.4 MB, ICU's data 3.5 MB and code 1.0 MB, WebAssembly 1.5 MB, B3 1.5 MB, the FTL 0.8 MB, libc++ 1.0 MB. `make jsc-test` (`tests/jsc`: the language, the engine — every JIT tier giving the interpreter's answers, regular expressions, Intl in French, typed arrays, 200 MB through the garbage collector, WebAssembly — and a benchmark) passes on x86-64 and arm64, with the JIT and without: **the JIT's worth: x5.2 on x86-64** (177 ms against 918 ms; node on the host: 226 ms) and **x5.8 on arm64** (emulated). What it needed of SIEOS: **signals for one thread** (`SYS_TKILL`), with the registers they found (a copy on the thread's stack, given to the handler: a Linux `ucontext_t` from libposix), and `SYS_SIGWAIT` (sigsuspend inside a handler): how WebKit suspends a thread to read its registers and resumes it (its garbage collector, WebAssembly's code updates); new ptest-threads check. A dying process's other threads now end quietly when they touch memory no longer given (they were reported "killed"). libposix: `mprotect` of the program's own pages succeeds without changing them (JavaScriptCore freezes its configuration page read-only: that defense is lost for now), `sched_setattr` accepted, an `abort()` prints the code addresses on its stack (for the host's symbolizer), `asm/hwcap.h`. Not yet: faults as signals (WebAssembly finds out-of-bounds accesses by the fault they cause: until faults become signals, one ends the process instead of throwing) |

| 6 | **done** (2026-10-17): **WPE WebKit 2.54.1 renders pages on SIEOS**, headless. `user/web/webkit.mk`: the WPE port with every optional feature off (decision 1, and GStreamer), static, `-Os`, on WPEPlatform's headless display; `make webkit` builds from scratch in **~9 minutes on x86-64, ~6 on arm64** (24 jobs). Four small patches (`user/web/patches`, each saying why): the libraries static (`WEBKIT_STATIC_LIBRARY`: there is no dynamic linking), a missing `<vector>` (libc++), a libdrm-only constant guarded, and **WPE without GL**: with `WEBKIT_DISABLE_COMPOSITING_MODE=1` hardware acceleration and compositing are off and the WebProcess makes no EGL display, so pages are drawn by Skia on the CPU into shared memory (`NonCompositedFrameRenderer`). Every program links all the libraries' dependencies, GIO's TLS module with them; WebKit's copy of xdgmime renamed against GIO's; unifdef built for the host. Stripped: **the WebProcess 82.6 MB, the NetworkProcess 81.2 MB, a program using the API 85.8 MB** (x86-64; arm64 84.2, 82.9, 87.6): each holds all of WebKit (phase 8: one multi-call program, its code shared between processes). `make web-test` (`tests/web/render.c`: a web view on the headless display, a snapshot to PNG, pixels checked) passes on x86-64 and arm64: `basic.html` (flex, grid, borders, rounded corners, text in DejaVu Sans with accents, an SVG image, a script changing the page), `script.html` (a 2D canvas, 2000 elements made by script, a promise and a timer) and **a page over HTTPS from the host through `tls.c`** (`tests/webdeps/servers.py`). A page loads in ~1.2-1.5 s on x86-64 (KVM; the WebProcess and NetworkProcess started each time), ~6 s on arm64 (emulated). What it needed of SIEOS: `linux/memfd.h` (WebKit's shared memory through memfd), `inotify` and `timerfd` refused quietly (GLib and WebKit fall back), libc++abi's own thread-local destructors (musl has no `__cxa_thread_atexit_impl`), `/etc/fonts/fonts.conf` (`user/web/fonts.conf`) and `/usr/share/mime/globs2` (`user/web/mime-globs2`: without them local pages were plain text), DejaVu Sans Bold and Sans Mono. Not yet: a WebProcess killed and the page reloaded, memory per process measured, the injected bundle (web process extensions: they need dlopen) |

Next: phase 7, the browser.
