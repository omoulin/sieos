# Chromium on SIEOS: the gap and the plan

Chromium 154 (154.0.8037.57, its "lite" source release, 11 GB unpacked) was
checked against SIEOS (October 2026) by `tools/chromium/gap.py`: the 145,923
files a Linux build compiles (Chromium's own and its bundled libraries; not
the other systems', not the tests) scanned for the system headers they
include, the system calls they make by number, the C library functions they
call, the `/proc`, `/sys` and `/dev` paths they read, and the kernel
interfaces' constants they use, each compared with SIEOS's sysroot, its C
library's symbols and the system calls SIEOS answers `ENOSYS` (130 of
Linux's). Its report: `build/chromium/gap/report.txt`. Most of what it lists
is in code a SIEOS build would not compile (the Linux sandbox, ChromeOS,
X11, Wayland, V4L2, VA-API, Breakpad, crashpad, tests); what follows is what
a SIEOS build does need.

## The rule: SIEOS stays Solaris; Linux is translated in the package

SIEOS is a Solaris-style system and does not change itself to resemble
Linux. What Chromium needs is sorted in two:

- **What Solaris itself has** (POSIX timers, peer credentials, event ports
  watching files, stack walking, `arc4random`...): added to SIEOS where it
  is missing, as Solaris has it (its names, its semantics), and useful to
  every program.
- **What only Linux has** (Linux's `/proc` text files, `prctl`, inotify,
  netlink, `SO_PEERCRED`, memfd seals, the futex operations, `statfs`'s
  magic numbers...): translated inside the Chromium package by its own
  library, `liblxcompat`, built on SIEOS's Solaris interfaces, and by
  Chromium patches where translating would be awkward. Nothing of it is in
  SIEOS's kernel or C library.

## The approach

- **Built as Linux with musl** (`target_os = "linux"`, as Alpine and Void
  build it): SIEOS's C library is a musl port, which already presents
  POSIX and the system calls' Linux names to programs built from portable
  sources; Chromium's `__GLIBC__` conditionals take musl's paths; Alpine's
  musl patches are the reference for the rest.
- **`liblxcompat`, the package's translation layer**, linked into every
  Chromium executable ahead of the C library (`-Wl,--wrap` for the calls
  it takes over; a header forced into every compilation for the missing
  declarations). It answers what Chromium asks Linux for from SIEOS's own
  interfaces (the table below), and is the only place SIEOS meets Linux's
  shapes.
- **Chromium's own toolchain**: its pinned clang (LLVM 24, prebuilt for the
  build host by `tools/clang/scripts/update.py`) cross-compiling with
  `--target=x86_64-unknown-linux-musl`, SIEOS's sysroot and its dynamic
  linker (`/lib/ld-musl-sieos64.so.1`), lld; its own libc++ (built in the
  tree); its Rust, whose standard library it builds in the tree, with the
  vendored `libc` crate given SIEOS's values by `tools/rust-sieos/gen-libc.py`.
  GN and Node.js (DevTools' front end) on the build host.
- **Off at first**: the Linux sandbox (seccomp-BPF, namespaces, the setuid
  helper; a sandbox for SIEOS would use its own privileges model later), the
  zygote, crash reporting, GLib, D-Bus, CUPS, ALSA and PulseAudio (a silent
  audio output), VA-API and V4L2, udev, X11 and Wayland, NaCl. On:
  PartitionAlloc, V8 with its JIT.
- **The display**: an Ozone platform of its own, `sieos`: Facet windows,
  Facet's input events, frames drawn by Skia in software and copied into
  the window; then GPU compositing through ANGLE on Vulkan (lavapipe, NVK).

## The gap, and where each part is filled

### In SIEOS, as Solaris has it

| What | Used by | Solaris's interface | SIEOS today |
|---|---|---|---|
| POSIX timers (`timer_create`, `timer_settime`, `timer_delete`...) | `base`'s watchdogs, Perfetto | POSIX timers in the kernel, per process | done (milestone 78) |
| Peer credentials of a Unix socket | `base`'s and Mojo's peer checks | `getpeerucred(3C)`, `ucred_get` | done (milestone 78) |
| Watching files | `base::FilePathWatcher` | event ports (`port_create`, `port_associate` with `PORT_SOURCE_FILE`: File Event Notification) | done (milestone 78) |
| Stack walking | `base::debug::StackTrace` | `walkcontext`, `printstack`, and `backtrace`/`backtrace_symbols` (Solaris 11's libc) | done (milestone 78) |
| `arc4random`, `arc4random_buf` | Expat, ffmpeg | Solaris 11.4's libc | done (milestone 78) |
| `pthread_cond_clockwait` | libc++, Abseil | POSIX (2024) | done, with the other clock waits (milestone 78) |
| The processor an LWP runs on | PartitionAlloc, Perfetto | `getcpuid(3C)` | there |
| File system information | `base` | `statvfs` (`f_basetype`) | there |

### In `liblxcompat` (the Chromium package)

`ports/chromium/lxcompat/` (stage 2, done): a static library linked whole
into every Chromium program ahead of the C library (its functions have
the C library's names and reach the C library's own through
`dlsym(RTLD_NEXT)`), a header forced into every compilation
(`lxcompat.h`), and the Linux kernel headers Chromium includes
(`linux/futex.h`, `linux/magic.h`, `linux/memfd.h`, `linux/limits.h`,
`linux/version.h`, `sys/cdefs.h`). Its tests (`tests/lxtest.c`) pass on
SIEOS.

| Linux's | Used by | Translated from |
|---|---|---|
| `/proc/cpuinfo`, `meminfo`, `stat`, `uptime`, `loadavg`, `version`; `/proc/PID/{stat,statm,status,comm,cmdline}`, `/proc/PID/task/` and `task/TID/*`; `/proc/self/{environ,auxv,maps,exe}`; `/proc/sys/kernel/random/{boot_id,uuid}`; `/sys/devices/system/cpu/{online,possible,present,cpuN/cpufreq/*}` (`open`, `openat`, `fopen`, `readlink`, `stat`, `access`, `opendir`) | `base`, PartitionAlloc, Abseil, cpuinfo, V8 | Solaris's `/proc` (`psinfo`, `status`, `cred`, `usage`, `lwp/`), the process's initial stack (`pr_argv`, `pr_envp`: the arguments, the environment, the aux vector), `cpuid`, `processor_info`, `sysinfo`, `getexecname`: the text made when the file is opened, in a memfd. `maps` lists the loaded objects (`dl_iterate_phdr`), the heap and the stack, not anonymous mappings |
| `prctl` (`PR_SET_NAME`, `PR_GET_NAME`, `PR_SET_PDEATHSIG`, `PR_SET/GET_DUMPABLE`, `PR_SET_NO_NEW_PRIVS`, `PR_SET_VMA`) | `base` | `pthread_setname_np`/`pthread_getname_np`; a thread waiting on the parent's pidfd sends the signal; the rest kept or accepted |
| `statfs`, `fstatfs` (`f_type`'s magic numbers) | `base` (drive information, `/dev/shm`) | `statvfs`'s `f_basetype` mapped to Linux's numbers |
| inotify (`inotify_init1`, `inotify_add_watch`, `inotify_rm_watch`) | `base::FilePathWatcher` | event ports' file events: a thread an instance writes `inotify_event` records into a socket pair; a directory's names compared after each change (`IN_CREATE`, `IN_DELETE`, `IN_MOVED_FROM`/`TO` with a cookie), its first 256 files watched for `IN_MODIFY`/`IN_ATTRIB` |
| futex's `FUTEX_WAIT_BITSET`, `FUTEX_WAKE_BITSET`, `FUTEX_CLOCK_REALTIME` (`syscall(SYS_futex)`) | PartitionAlloc, Abseil, V8 | `FUTEX_WAIT` (the absolute time made relative) and `FUTEX_WAKE`, which the C library makes SIEOS's user mutexes (`lwp_umtx`) |
| `SO_PEERCRED` (`SO_PASSCRED` accepted; no `SCM_CREDENTIALS` sent) | `base`, Mojo | `getpeerucred` |
| memfd seals (`F_ADD_SEALS`, `F_GET_SEALS`; `MFD_NOEXEC_SEAL`) | Mojo's read-only shared memory | recorded and answered by the layer (Chromium checks them; nothing enforces them in-process) |
| `sendmmsg`, `recvmmsg` | QUIC | loops over `sendmsg`/`recvmsg` |
| `sched_getcpu` | PartitionAlloc, Perfetto | `getcpuid(3C)` |
| `SOCK_SEQPACKET` Unix socket pairs | the browser's sandbox host, the zygote | not translated: both are off in the SIEOS build (stage 3) |
| Netlink route sockets | `net::AddressTrackerLinux` | not translated: a SIEOS network change notifier patched in (stage 3) |
| `fopen64`, `lseek64`, `pread64`... | minizip, libwebm, ffmpeg | the C library's macros (`_LARGEFILE64_SOURCE`) |
| xattrs, `pkey_*`, `rseq`, `membarrier`, `userfaultfd`, `perf_event_open` | V8's and PartitionAlloc's isolation, metrics | `ENOSYS` from the C library, or the features off |

SIEOS itself gained one thing on the way, as Solaris has it: `FIONREAD` on
pipes and sockets gives the bytes waiting (the next datagram's), not 1.

### Memory and threads (to be fast enough)

- **Huge reservations**: PartitionAlloc reserves 16 GiB pools, V8's
  sandbox 1 TiB (`PROT_NONE`, `MAP_NORESERVE`): SIEOS's 128 TiB of user
  address space holds them, and reservations are not filled; V8's sandbox
  can be turned off first.
- **Many mappings**: Chromium's processes have thousands; SIEOS's page
  faults find theirs in a sorted index, by bisection (milestone 78).
- **`madvise(MADV_DONTNEED)`, `MADV_FREE`**: SIEOS has them (through
  `memcntl`); PartitionAlloc's decommit uses them and `mmap(MAP_FIXED)`.
- **Threads**: tens a process, several processes: the kernel runs on
  every processor at once (no big kernel lock since milestone 77).

### The build host

- Chromium's clang and Rust (downloaded by its scripts), GN, Node.js, Python
  3; about 100 GB for a build; a full build, hours on this machine.

## The big kernel lock (removed: milestone 77)

SIEOS's kernel ran under one lock: user code, page faults and simple system
calls ran in parallel on every processor, but the rest of the kernel one
processor at a time. Chromium's tens of threads and several processes, each
making system calls, would have queued there. It is gone now, Solaris's way
([locking.md](locking.md)); what led there:

- **Solaris never had one.** SunOS 4 (BSD-based) had a single kernel lock;
  Solaris 2 (SunOS 5, 1992) was designed multithreaded from the start: the
  kernel itself is threads (interrupts too), every structure has its own
  lock — adaptive mutexes (spinning while the owner runs on another
  processor, sleeping otherwise), reader/writer locks, condition variables,
  turnstiles for priority inheritance — and the dispatcher has a run queue
  and a lock per processor.
- **The BSDs had one, and took years to leave it**: FreeBSD's "Giant"
  (SMPng, from 5.0 in 2003: subsystems marked MP-safe one by one, Giant
  gone from the hot paths by 7.0), NetBSD's `kernel_lock` (removed
  subsystem by subsystem from 5.0), OpenBSD's `KERNEL_LOCK` (still there,
  being pushed down). Linux's lasted from 2.0 to 2.6.39 (2011).
- **The options for SIEOS**: keep the lock and hold it less (more fast
  paths, as page faults and simple calls already are); or **Solaris's
  design, reached as FreeBSD reached its own**: adaptive mutexes, reader/
  writer locks and turnstiles as Solaris has them, then subsystem by
  subsystem made to need no big lock — the dispatcher (per-processor run
  queues), the address spaces (they have their own lock already), file
  descriptors, pipes and Unix sockets, the timers, the VFS and tmpfs, ext4
  and the block cache, the network stack, the drivers — each one tested
  under load, the big lock remaining for what is not done yet. The second
  is the way: SIEOS's own model's, and the one that lets Chromium run
  fast.

## The plan (in agent time: mostly builds)

| Stage | What | Time |
|---|---|---|
| 1 | **SIEOS, Solaris's way** (done: milestone 78): POSIX timers in the kernel, `getpeerucred`, event ports with file events, `backtrace`/`walkcontext`/`printstack`, `arc4random`, `pthread_cond_clockwait`; the page faults' area index; each tested on SIEOS | done |
| 2 | **`liblxcompat`** in the Chromium package (done): the table above, with its own tests (run on SIEOS) | done |
| 3 | **The build**: Chromium's clang with SIEOS's sysroot, its Rust's `libc` made SIEOS's, the GN arguments, the patch set (musl, the network change notifier, what is off), the first full build | about 1 day |
| 4 | **`headless_shell`**: pages drawn into images (`--headless --screenshot`), single process then several; its failures fixed | about 1–2 days |
| 5 | **`content_shell` in a Facet window**: the `sieos` Ozone platform | about 1 day |
| 6 | **Chrome**: the browser's UI, its profile, downloads; GPU compositing through ANGLE on Vulkan | a few days |
| 7 | **The big kernel lock, Solaris's way** (done first: milestone 77, [locking.md](locking.md)); next, where contention shows: per-processor run queues, priority inheritance, finer locks in the network stack, terminals and ext4 | continuing |

The uncertainty is in what Chromium finds that this scan cannot: the
kernel's behaviour under its load (as Python's tests found an `exec` that
ended the other threads).
