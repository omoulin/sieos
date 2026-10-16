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
`fetch`, mail and git). tls.c needs a transport callback (today it owns
a netd socket) and ALPN (`h2`, `http/1.1`) for HTTP/2. Fallback, if that
stalls: glib-networking with OpenSSL, from its release.

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

### Phase 0: the host toolchain, without root (1-2 days)

`make wk-tools` fetches into `.hosttools/` (not in git), each archive
checked against its SHA-256 in `user/posix/posix.mk`:

| Tool | From | Kept |
|---|---|---|
| clang, lld, llvm-ar/-ranlib/-nm/-objcopy/-strip, clang's own headers | `LLVM-23.1.3-Linux-X64.tar.xz` (release binaries) | only those files (~250 MB of the 2 GB archive) |
| libc++, libc++abi, libunwind, compiler-rt builtins sources | `llvm-project-23.1.3.src.tar.xz` | `libcxx libcxxabi libunwind runtimes cmake llvm/cmake llvm/utils/llvm-lit compiler-rt` |
| CMake 4.4.4, Ninja 1.13.2, Meson (for GLib, HarfBuzz, libsoup...) | PyPI wheels, `pip install --target --require-hashes` | `.hosttools/py/` |
| gperf 3.3 | GNU release, built by the host's g++ | `.hosttools/bin/gperf` |
| pkgconf (phase 4) | its release, built by gcc | `.hosttools/bin/pkg-config` |

*Test:* `clang --version`, `cmake --version`, `ninja --version`,
`gperf --version` from `.hosttools`; a C file cross-compiled for
`x86_64-linux-musl` and linked by lld.

### Phase 1: kernel, memory and waiting (1 week)

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

### Phase 2: kernel, sharing (1-2 weeks)

1. **Shared memory objects**: `SYS_SHM_CREATE(size)` → an id;
   `SYS_SHM_GRANT(id, pid)`; `SYS_SHM_MAP(id, addr, prot)`. Pages
   counted once (on the creator's quota), freed with the last mapping.
   Also the `SYS_GRANT` of [gpu-plan.md](gpu-plan.md) phase 3, by another
   road: one mechanism, not two.
2. **Notes**: `SYS_NOTE(port, bits)`: any process may OR bits into a
   port's pending note (never blocks, coalesced, the kernel's `NOTE_*`
   path); `SYS_RECV` with a deadline. The base of `poll`.
3. **Shared program images**: `SYS_SPAWN` from a shared memory object
   holding an ELF file: read-only segments mapped from it (shared),
   writable ones copied. `/bin/web` is read once and its 50+ MB of code
   is in RAM once, however many processes run it. Spawn arguments grow
   from 256 bytes to a page (argv and the environment).

*Tests:* `ktest shm` (two processes see each other's writes; a futex
across processes; freed when both end), `ktest note`, `ktest spawnshm`
(two children of one image: memory used grows by the writable part only).

### Phase 3: the Linux personality and musl: libposix (3-5 weeks)

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

### Phase 4: C++ and the dependencies (3-5 weeks)

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

### Phase 5: JavaScriptCore alone (2-3 weeks)

WebKit configured with `-DPORT=JSCOnly` first (no WebCore: WTF, bmalloc
off, JavaScriptCore and its `jsc` shell), against the same sysroot. The
LLInt (offline assembler, x86-64 and ARM64) and the baseline and DFG
JITs; FTL off (it needs B3 only, no LLVM; measured later).

*Tests:* `make jsc-test`: `jsc` runs JavaScriptCore's `stress` tests (a
list of expected passes) and the SunSpider/JetStream 2 subsets in QEMU;
`--useJIT=0` against JIT timings (the JIT's worth, measured). *Size:*
`jsc` ~15-20 MB.

### Phase 6: WebKit headless (3-6 weeks)

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

### Phase 7: the browser (2-3 weeks)

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

### Phase 8: speed, arm64, and the rest (2-4 weeks, then as needed)

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

Honest, for focused work, phases in order (1 and 0 can overlap):

| Phase | Builds | Effort |
|---|---|---|
| 0 | the host toolchain, fetched | 1-2 days |
| 1 | kernel: lazy memory, `mprotect`, futexes, thread ids | 1 week |
| 2 | kernel: shared memory, notes, shared program images | 1-2 weeks |
| 3 | libposix: musl + the Linux personality, `unixd`, posix-test | 3-5 weeks |
| 4 | C++ runtime; the ~20 dependencies | 3-5 weeks |
| 5 | JavaScriptCore and `jsc` | 2-3 weeks |
| 6 | WPE WebKit headless, pages to PNG | 3-6 weeks |
| 7 | atlas client windows, the SIEOS platform, the browser | 2-3 weeks |
| 8 | speed, arm64, the Pi 5 | 2-4 weeks |
| | **a usable browser (phases 0-7)** | **about 4-6 months** |

## What you will be asked to do

- Nothing as root: every tool is fetched into `.hosttools/` with its
  checksum (`make wk-tools`: ~2.2 GB downloaded, ~1 GB kept). If you
  prefer Debian's packages, `sudo apt install clang lld cmake ninja-build
  gperf meson pkgconf` works too (the Makefile uses them when present
  and recent enough).
- Disk space: ~40 GB for WebKit's build directory and the sysroot
  (`build-web/`), outside `build/`.
- Let the long builds run (phases 5-6: an hour or two each time WebKit is
  rebuilt from scratch).
- Try the browser on the laptop (from the USB key) and the Pi 5 in
  phase 8, and tell which sites matter to you: they become tests.

## Progress

| Phase | State |
|---|---|
| 1 | **done** (2026-10-16): `kernel/vm.c` (regions, page faults, the kernel's copies into untouched pages, `SYS_MMAP`, `SYS_MUNMAP`, `SYS_MPROTECT`, `SYS_FUTEX`, `SYS_THREAD_SELF`); `ktest vm` and `ktest futex` pass on x86-64 and arm64 (built by gcc and by sicc); kernel code 54,787 → 59,459 bytes (x86-64), 80,680 → 85,736 (arm64); `test`, `kernel-test`, `gui-test` (x86-64) and `test`, `kernel-test` (arm64) pass; IPC round trip unchanged (375-491 ns) |
