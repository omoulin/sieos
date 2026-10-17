# The kernel and the servers, file by file

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

This is the guided tour: how SIEOS starts, how its pieces talk, and what
each file does. The architecture interface (x86-64, AArch64) is in
[arch.md](arch.md); each subsystem has its own page ([index.md](index.md)).

## The idea: a microkernel

The kernel (about 2,900 lines of portable core, plus 1,700-2,000 per
processor architecture; 58 KB on x86-64, 86 KB on AArch64) does only what
nothing else can do:

| The kernel does | Programs ("servers") do |
|---|---|
| memory, address spaces, processors | the console (`con`): screen, keyboard, serial |
| processes, threads, scheduling, time, signals | the disk (`vblk`), files (`fs`), network (`vnet`, `netd`) |
| message passing (IPC) | users (`auth`, `login`), the desktop (`atlas`) |
| interrupts delivered as messages | the assistant (`siad`), the supervisor (`init`) |

Programs talk with **messages**. A server creates a named **port** and
waits on it. A client sends a message and sleeps until the reply. The
kernel copies the message **directly** from one program's memory into the
other's and stamps it with the sender's identity (pid, uid, gid, groups),
which servers can trust. A driver receives its device's interrupts as
messages on the same port, so one loop serves hardware and clients:

```
   sh                         kernel                        con
   printf("hi")
   ipc_call(console, WRITE) ->  copy "hi" into con's buffer -> ipc_recv returns
   (sleeps)                                                   prints on screen + serial
                            <-  copy the reply           <-   ipc_reply_recv(sh, 2)
   continues
   keyboard interrupt       ->  mask the line, wake con   ->  ipc_recv returns (token 0)
                                                              reads the key, irq_ack
```

A bug in a server kills only that server, and init starts it again.

## The boot, file by file

The kernel is a portable core (`kernel/*.c`) and one processor's part
(`kernel/arch/x86_64/` or `kernel/arch/arm64/`), joined by
`kernel/arch.h`. `make ARCH=x86_64|arm64` chooses (default `x86_64`).

1. **Entry** (`kernel/arch/*/boot.S`):
   - x86-64: QEMU (or GRUB) finds the *multiboot header*, loads the kernel
     at 1 MiB in 32-bit mode; `boot.S` builds the first page tables,
     switches to 64-bit mode, and `platform.c` turns the boot loader's
     information into a `boot_info_t`.
   - AArch64: QEMU or the Pi firmware loads the kernel `Image` and passes a
     *device tree*; `boot.S` drops from EL2 to EL1 if needed, turns the MMU
     on, and `platform.c` reads memory, CPUs and devices from the device
     tree (`fdt.c`).
2. **`kernel/main.c`**: `kernel_main` sets up memory and the first CPU,
   starts the first *boot module*, `init` (the supervisor), starts the
   other CPUs, and from then on only runs processes. The other boot
   modules (`con`, `vblk`, `fs`) stay in memory for init to start and
   restart. Also the boot options (`pcores=N`), and the list of the
   kernel's locks with the order in which they may be taken.
3. **`arch/*/cpu.c`, `entry.S`**: per CPU, the way into the kernel
   (x86: segments, TSS, `SYSCALL`; AArch64: the exception vector table,
   `svc #0`) and `trap()`, which handles every interrupt and exception.
4. **`arch/*/smp.c`** (and x86's `ap.S`): the other processors, the
   interrupt controllers (x86: I/O APIC and local APICs; AArch64: GIC-400),
   time and the tickless timer.
5. **Memory**: `kernel/mem.c` keeps a bitmap of free 4 KiB pages over all
   of RAM, a small allocator for kernel objects (it gives empty pages
   back), the DMA quarantine, quotas, and `vm_copy`, the checked copy
   between address spaces that IPC relies on. `arch/*/mmu.c`: the page
   tables (the *direct map* of all physical memory, each process's address
   space; AArch64 uses ASIDs so switching needs no TLB flush).
6. **`kernel/task.c`**: processes and threads (as many as memory allows),
   the scheduler: a round-robin run queue per CPU (a waiting thread costs
   no CPU time; an idle CPU takes waiting threads from busy ones, polls a
   moment, then sleeps), P-cores and E-cores (below), the tickless timer's
   decisions, sleeping, ending and waiting for processes, signals
   (below), and loading ELF programs (`SYS_SPAWN`).
7. **`kernel/ipc.c`**: ports, `call` / `recv` / `reply` (and `reply_recv`,
   the server's fast path), interrupts as messages (several drivers may
   share a line), and on-demand names. **The heart of the microkernel.**
8. **`kernel/syscall.c`**: the system call table. The contract with
   programs is `include/mk/abi.h`; the servers' message formats are
   `include/mk/proto.h`.

Then the servers, all ordinary programs:

9. **`user/con`**: the console: the serial port, and on x86 the VGA text
   screen and the PS/2 keyboard and mouse (QEMU's absolute pointer); on
   AArch64 the PL011 serial port and virtio keyboard and tablet. Line
   editing is `user/lib/tty.c`, shared with the desktop's terminals.
10. **`user/vblk`**: the disk driver: virtio (PCI on x86, MMIO on AArch64),
    on the Raspberry Pi the SD card (`sd.c`), an NVMe drive (`nvme.c`,
    [usb.md](usb.md#the-pcs-nvme-drives)) or a USB key (through `usb`). It serves the port
    "disk" (up to 128 KiB per message); data arrives by IPC straight into
    the buffer the device reads.
11. **`user/fs`**: the file server: the SieFS library ([siefs.md](siefs.md))
    on top of "disk", serving "fs". It checks permissions with the
    identity the kernel stamps on every message, keeps open-file handles,
    and commits changes 1 s after the first one (or on `sync`).
12. **`user/init`**: the supervisor, the only program the kernel starts.
    It starts every server from its table, starts each again when it ends,
    starts some only when needed and stops them when idle (below). Only
    init may give device rights. `svc` lists and controls the services.
13. **Users** ([accounts.md](accounts.md)): `user/auth` (accounts,
    Argon2id passwords), `user/login` (the text prompt), `user/acct`
    (`passwd`, `su`, `useradd`, `userdel`). The first accounts are created
    on the desktop's welcome screen (`user/atlas/setup.c`), or on the
    terminal when there is no screen.
14. **`user/sh`**: the shell (files, tags, users, processes; any other word
    runs `/bin/NAME`).
15. **`user/atlas`**: the desktop ([desktop.md](desktop.md)).
16. **The network** ([net.md](net.md)): `user/vnet` (the card's driver),
    `user/netd` (TCP/IP, DHCP, DNS), `user/fetch`.
17. **The assistant** ([sia.md](sia.md), [llm.md](llm.md)): `user/sia`.

`user/lib` is the library, an archive from which each program takes only
what it uses (system calls, `printf`, `malloc`, threads, files, network,
crypto).

## How a program reads a file

Every arrow is a message; the kernel copies the data once per arrow:

```
   sh: cat /etc/motd
     |  FS_OPEN "/etc/motd", FS_READ          (port "fs")
     v
   fs: walk the path, check the rights, SieFS finds the blocks
     |  DISK_READ block, count                (port "disk")
     v
   vblk: virtqueue request, waits for the device's interrupt (a message)
     v
   disk -> data -> vblk -> fs -> sh -> con (screen)
```

## When a server crashes

```
   kernel ──"your child ended" (a message)──► init ──spawn──► fs (new)
   client ──FS_READ──► (old fs dies) ── -EPIPE ──► library: port_lookup("fs")
          ◄── waits until the new fs registers ── reopens its files ── sends FS_READ again
```

- **Restart:** about 1 ms later (measured: fs 0.2-0.9 ms, vblk 0.2-0.9 ms,
  con 0.2-0.6 ms, auth 1-2 ms). A server failing within 1 s of starting
  waits 10, 20, 40, 80 ms ... and is given up after 5 such failures
  (`svc` shows "GIVEN UP"; `svc restart` tries again).
- **Clients carry on by themselves:** a call that fails because the server
  died (`-EPIPE`) or the port number is stale (`-ENOENT`) finds the port
  again by name and is resent, if repeating it is harmless (reads, writes
  at an offset, disk blocks, console text, most file operations; not a
  login, nor closing a file). Open files are reopened.
- **What survives:** `vblk`: everything. `con`: everything but the screen.
  `fs`: the disk's last commit, at most 1 s old (`sync` reports a loss
  once). `auth`: everything. `login`: the open session goes on.
- **Safety:** a dead driver's DMA buffers stay unused for 100 ms. init
  itself must not fail: the kernel stops with a clear message if it does.

## Servers started on demand, stopped when unused

```
   fetch ──lookup "net"──► kernel: no such port, but "net" is declared on demand
                             └──NOTE_WANT (a message)──► init ──spawn──► netd
   netd ──lookup "nic0"──► (the same) ─────────────────► init ──spawn──► vnet
   fetch wakes as soon as netd creates its port "net"

   unused for its idle time: init ──"may you stop?" (SVC_MAYSTOP)──► netd
   netd: nothing asked since, no connection open ──► yes, ends
   a call arriving meanwhile gets -ENOENT: the library resends it,
   which starts a new netd
```

- **Which ones:** the network (`netd`, `vnet`), the assistant (`siad`) and
  the accounts server (`auth`). `con`, `vblk`, `fs`, `login` and `atlas`
  start at boot.
- **Idle times:** network 10 min, assistant 15 min, accounts 5 min
  (`svc idle NAME SECONDS`; 0: never).
- **Measured:** after boot and a login, 3.1 MiB of RAM in use, instead of
  1 GiB when the assistant's 1.7B model was loaded at boot.

## Signals: Ctrl-C

```
   you press Ctrl-C ──► con / a desktop terminal / sshd (user/lib/tty.c)
                          the job the shell runs now: group 42 (CON_FG)
                          └──SYS_KILL(-42, SIGINT)──► kernel
   no function for SIGINT: each process of group 42 ends at once (status EXIT_SIGNAL(2))
   a function (python): the thread that set it is woken (its wait returns -EINTR)
                        and runs it on its way out of the kernel; SYS_SIGRETURN
                        puts it back where it was, every register as it was
   sh: wait() returns, prints nothing for ^C, shows a fresh prompt
```

What the kernel keeps is only what nothing else can: which signals wait
for a thread, what each one does to a process, and the registers of a
thread whose function runs. Who gets Ctrl-C, and when a terminal goes
away, is the terminals' business, in the one place they share (`tty.c`).

- **Calls:** `SYS_KILL(pid, sig)` sends (pid < 0: every process of group
  -pid; sig 0 only checks); the old `sys_kill(pid)` is `SIGKILL`. Root may
  signal any process but init, others only their own. `SYS_SIGNAL(sig,
  entry)`: `SIG_DFL` (the process ends), `SIG_IGN`, or a function.
  `SYS_SIGRETURN` ends that function. The library has `signal()`,
  `raise()` and `kill()` (`user/lib/sig.c`, `mk.h`), also for programs
  built by `/bin/cc` on SIEOS.
- **Signals:** any number 1-31; the named ones are `SIGHUP` (the terminal
  went away), `SIGINT` (Ctrl-C), `SIGKILL` (cannot be caught or ignored),
  `SIGUSR1`, `SIGUSR2`, `SIGTERM` (the shell's `kill` without an option).
  A process a signal ends has the status `EXIT_SIGNAL(n)` = -4096 - n:
  outside the -errno range, so the shell tells "ended by a signal" from
  "could not start" (`EXIT_SIGNO(status)` gives n back).
- **Not caught: the sender ends it.** As `SYS_KILL` always did (the
  process need not run, nor even be scheduled): a busy loop, a sleeper, a
  program stuck in a call all end the same way. Ignored: nothing happens.
- **Caught: pending on one thread**, the one that set the function (the
  last to: Python's interpreter thread, not its idle main thread). The
  sender marks the signal (`sigpend`), then does what ending a process
  does (below): a shootdown, so a thread going to sleep on another CPU
  sees the mark, and one running in user mode comes into the kernel; then
  it wakes the thread if it waits. Every wait is interruptible: `SYS_SLEEP`,
  `SYS_WAIT`, `SYS_RECV`, `SYS_PORT_LOOKUP`, and `SYS_CALL` both queued and
  waiting for the reply; the call returns `-EINTR`.
- **The function runs on the thread's own stack**, below where it was
  (x86-64: past the 128-byte red zone), entered on the thread's way out
  of the kernel (`kernel_exit`, every way out: a system call, a timer, an
  interrupt from another CPU). Its registers stay in the kernel
  meanwhile (a 184-byte, on AArch64 280-byte, block), and so do its
  floating-point and vector registers: their area is handed over, and the
  function starts with none (a clean set on its first use, only if it uses
  them). `SYS_SIGRETURN` puts both back (x86-64 leaves by `iretq` then, so
  rcx and r11 come back too). Nothing is written to the user's stack, so
  there is nothing to check on the way back and nothing a program could
  forge, and the size of the vector state (AVX-512: 2.7 KiB) is not part
  of the ABI. The price: one function at a time per thread (signals that
  come meanwhile wait for its end), and it must return, not jump out.
- **An interrupted call:** the server still holds the token and answers
  later. A token is the client thread's id and, in its high half, the
  count of calls the thread gave up; the count moves on when one is
  given up (a signal, or its process ending), so the late answer goes
  nowhere (`-ESRCH` for the server), even if the client has meanwhile
  called the same server again. An answered call changes nothing: the
  round trip writes no more than before. The work the server did stands
  (a read from the network has taken the data).
- **Process groups:** each process has one (`pgid`), its parent's unless
  started with `SPAWN_PGROUP` (it then leads a group of its own number).
  auth starts each session's shell so; the shell starts each job so.
- **Terminals** (`tty.c`, used by `con`, the desktop's terminals, `sshd`):
  the shell tells which group runs (`CON_FG`): the job while it runs,
  else its own. Ctrl-C (in the usual line mode; in raw mode, ssh's,
  the key goes to the program) sends that group `SIGINT`, drops the typed
  line and shows `^C`. The terminal accepts `CON_FG` only for a group whose
  leader runs as the sender's user (or from root): nobody aims a console's
  Ctrl-C at someone else's programs. A closed terminal window, an SSH
  connection gone, send `SIGHUP`. sshd also takes the SSH "signal" request
  (`INT`, `TERM`, `HUP`, `KILL`) for the terminal's group.
- **The shell** catches `SIGINT` (Ctrl-C at the prompt or in the built-in
  `sleep` interrupts only that); `kill [-INT|-TERM|-KILL|-HUP] PID`; it
  says "killed", "terminated", "hung up", nothing for Ctrl-C. `sh -c` (make's
  recipes) keeps its programs in its own group: Ctrl-C reaches all of them.
- **Cost:** the kernel's code grew by 1.6 KB on x86-64 (52.7 to 54.3 KB)
  and 2.3 KB on AArch64 (78.3 to 80.6 KB); programs that use no signal, not
  a byte (the functions are a library member of their own). On the IPC
  path: one test (`may_stop`: is the process ending, or a signal marked?)
  where a call waits and where a thread leaves the kernel, everything else
  out of line, and the token's check in the reply: a few instructions.
  Measured (`make ipc-bench`, x86-64 on KVM, several boots each): the round
  trip on 1 CPU 452-455 ns (448 before), 40 clients on 4 CPUs 742-798 ns
  per call (723-791 before). Where the code lands in memory moves these
  numbers as much: builds with the very same logic, only placed
  differently, measured from 443 to 457 ns.
- **Not done (on purpose, for size):** no signal masks (`sigprocmask`),
  no `sigaction` flags (an interrupted call is not made again: it returns
  `-EINTR`, the library passes it on), no information beyond the number,
  no queued repeats (a signal sent twice before it is taken counts once),
  no stop/continue (`SIGSTOP`, job control in the background). A program
  started by another begins with every signal at its default.

## Memory given on first touch, futexes

For big ported programs (the browser: [webkit-plan.md](webkit-plan.md)),
`kernel/vm.c` adds what Linux programs expect from `mmap`:

- **Regions:** `SYS_MMAP(addr, size, PROT_* | MAP_FIXED)` reserves address
  space from 16 TiB up (a sorted list per process, 32 bytes a region);
  nothing is allocated. The first touch of a page, by the program (a page
  fault) or by the kernel copying a message into it, maps a zeroed page
  with the region's rights, and the access runs again. Any other fault
  still ends the process. `SYS_MUNMAP` removes a range (or, `UNMAP_KEEP`,
  only gives its pages back: they come back zeroed), `SYS_MPROTECT`
  changes rights (pages already there are mapped again; every CPU forgets
  the old ones). `PROT_NONE` is a guard page; read+write+execute is allowed
  on anonymous memory only (a JIT's code pool). Pages count against the
  process's quota when touched.
- **Futexes:** `SYS_FUTEX(FUTEX_WAIT, addr, value, ns)` sleeps while the
  32-bit word holds `value` (a time limit, a signal or a wake ends it);
  `FUTEX_WAKE` wakes up to n. Waiters are keyed by the word's physical
  address (shared memory will work as is) and sleep like `SYS_SLEEP`, woken
  like `SYS_WAKE` (a wake before the sleep is not lost). Rarely a sleep
  that follows a timed-out wait may end early: callers loop anyway.
- **Thread ids:** `SYS_THREAD_SELF(clear)`: the caller's id; when that thread
  ends, 0 is written at `clear` and one waiter there woken (`pthread_join`).
- **Shared memory objects:** `SYS_SHM_CREATE(id, size)` makes zeroed pages
  named by a random 63-bit number the creator chose; knowing the number is
  the right to `SYS_SHM_MAP` them (a region whose pages are the object's),
  to `SYS_SHM_HOLD` the object (for another process too) and to
  `SYS_SHM_CLOSE` a hold. It lives while held or mapped. Futexes in it
  work across processes (they are keyed by physical address).
- **Shared program images:** `SYS_SPAWN` with `SPAWN_SHARE` and an ELF file
  in a shared memory object: read-only segments map the object's pages,
  so programs started from one image share its code.
- **Notes:** `SYS_NOTE_WORD(addr)` names a word of the caller's;
  `SYS_NOTE(pid)` adds one to it and wakes its futex waiters (root, or the
  same user): how unixd and netd tell libposix's `poll` to look again.
- **The wall clock:** `SYS_TIME(0)`: ns since 1970, the real-time clock
  (x86-64: CMOS; arm64: PL031) read at boot, before any program runs (a
  driver using the clock chip, acpid's alarm, is never interrupted
  half-way), plus the monotonic clock; `SYS_TIME(ns)`: root sets it (the
  Pis have no clock to read).
- **A child's end** bumps its parent's note word too (libposix's pidfds:
  GLib's child watches).
- **Cost:** 9.3 KB of kernel code in all (x86-64; 9.7 KB arm64); nothing on the paths
  that existed (`ktest vm`, `ktest futex`, `ktest shm`; IPC round trip
  unchanged).

## Several processors, time, floating point

- **CPUs:** x86 finds them in the ACPI MADT and starts them with
  INIT/SIPI (`ap.S` trampoline); AArch64 reads the device tree and uses
  PSCI (QEMU, Pi 5) or the spin table (Pi 4). Each CPU has one page of its
  own data.
- **Tickless:** a CPU's timer (x86: TSC deadline or LAPIC; AArch64: the
  generic timer) is set only when something must happen: a time slice
  ending while others wait, or a sleeper's wake-up. 8 idle CPUs went from
  5.6% of a host core with a 100 Hz tick to under 0.1%.
- **Run queues:** one per CPU. A thread that becomes ready goes to an
  idle CPU if there is one (its last CPU first), else to the shortest
  queue; an idle CPU takes threads waiting behind a busy CPU's running
  thread. A busy CPU is interrupted only if its timer would not end its
  running thread's slice anyway: a ready thread waits one slice at most.
- **Wake-ups:** a CPU that becomes idle polls its queue for 50 µs before
  it sleeps, letting interrupts in: a thread queued for it then needs no
  inter-processor interrupt (in a virtual machine one costs microseconds).
  A sleeping CPU is woken by one. In IPC the receiver usually gets the
  sender's CPU at once ("handoff"), so a round trip stays on one CPU:
  about 0.45 µs on x86 (KVM).
- **Locks:** every CPU may be in the kernel at once. Each piece of shared
  data has its own lock (a ticket spinlock): each bucket of the port table
  (a port and its queues), each bucket of the thread table, each process,
  each CPU's run queue, the process table, port names, interrupt lines,
  the sleepers, the page and small-object allocators, the DMA quarantine,
  the random pool. None is held while a message is copied. The order in
  which they may be taken (so that no two CPUs ever wait for each other)
  is written down in `kernel/main.c`:

  ```
  proc_lock -> a process -> port names -> interrupt lines -> a port bucket
    -> a thread bucket -> the sleepers -> a run queue -> DMA quarantine
    -> small objects -> pages -> random pool, log, serial port
  ```

  - A round trip takes three uncontended locks (the port's bucket twice,
    the client's thread bucket once) and no full barrier.
  - One waker, never two: whoever takes a thread off the queue it waits
    in (under that queue's lock), or, for a thread waiting for a reply,
    whoever clears its `server` (under its thread bucket's lock).
  - A thread just woken may still be on its old CPU's stack for a moment
    (`on_cpu`): its waker waits until that CPU has left it.
  - Ending a process: its threads may be going to sleep on other CPUs at
    that very moment. The killer sets `exiting`, waits until every other
    CPU has answered a shootdown (answered only from user mode, idle or a
    spin loop, never half-way into sleeping), then wakes each thread that
    waits. One of them always sees the other; the cost is on the kill,
    not on each message.
  - Freed memory: user pages only after a shootdown that every CPU
    answers (none can still be copying a message to or from them); a
    thread's stack once its CPU has switched away from it.
- **AArch64 ordering:** locks are acquire/release (`ldaxr`/`stlxr`,
  `ldar`, `stlr`); a value another CPU acts on is published with a
  release store and read with an acquire load; full barriers (`dmb ish`)
  where a store must be seen before a later load (claiming an idle CPU,
  the timer); new page tables are published after `dsb ishst`.
- **P-cores and E-cores** (Intel's hybrid processors since Alder Lake,
  ARM's big.LITTLE): each CPU's kind comes from the processor (x86-64:
  CPUID leaf 7 says hybrid, leaf 0x1A gives each core's type: 0x20 Core,
  0x40 Atom; AArch64: MIDR_EL1, Cortex-A35/A53/A55/A510/A520 being
  efficiency cores). Boot says `mk: 8 CPUs running: 2 P-cores, 6 E-cores`.
  Speed first: normal threads (interactive work, servers, the assistant's
  model) prefer P-cores, background ones (`SYS_PLACE`; `SPAWN_BACKGROUND`;
  init's `svc add -b`) E-cores. A thread goes to the other kind only to an
  idle CPU: normal work overflows to idle E-cores, background work may use
  an idle P-core. A normal thread waking up when every P-core runs
  background work takes one (that work moves away); an idle P-core takes
  over a normal thread running on an E-core, an idle E-core background
  work running on a P-core. siad compacts conversations (after the answer
  is delivered) as background work. Where all cores are alike (most PCs,
  the Pi 4's A72s, the Pi 5's A76s) all are treated as P-cores and nothing
  changes. QEMU's cores are alike: the boot option `pcores=N` pretends
  CPUs 0..N-1 are P-cores and the others E-cores; `make kernel-test`
  boots that way a second time and checks the placement (`ktest place`).
- **Interrupts:** each line can go to any CPU; it stays masked from the
  moment it fires until its drivers acknowledge it; up to 4 drivers may
  share a line.
- **TLB:** x86 flushes the other CPUs by IPI; AArch64 uses broadcast
  `tlbi`, then also waits for every other CPU's answer (see Locks).
- **Floating point and vectors** (`arch/*/fpu.c`): the kernel never uses
  them. A thread's first floating-point instruction traps once and gets a
  clean save area; from then on each switch saves and restores it
  (x86: XSAVEOPT/XRSTOR with AVX; AArch64: the FP/SIMD registers).
- **Processor features** (`SYS_INFO`'s `hwcap`, `arch_hwcap()`): the kernel
  tells programs what they may use: AVX2, FMA, AVX-VNNI on x86; the dot
  product (`sdot`, Pi 5) and LSE atomics on AArch64. The assistant picks
  its kernels from it.
- **Thread-local variables:** FS base (x86) or TPIDR_EL0 (AArch64), set
  per thread by the library.
- **Memory quotas:** a process may be limited in pages (heap, DMA,
  program); init gives the assistant 4.5 GiB.

## The console's log

Everything the kernel prints goes to the serial port (`arch_putc`) and
into a ring of the last 32 KiB (`kprintf.c`). The console server prints
through the kernel too (`SYS_DEBUG`), so the ring holds what a serial
wire would show. Any program may read it with `SYS_LOG` (buffer, size,
position): it gets the bytes from that position since boot (moved up to
the oldest still kept) and the position after them. `/bin/gfxhat` shows it
on a Raspberry Pi's GFX HAT (docs/raspberrypi.md); `ktest log` checks it.

## Choices made for speed and size

- **Fast system calls** (`SYSCALL`/`SYSRET`, `svc`), no message buffering
  (one copy, sender to receiver).
- **Small:** 4 KiB kernel stacks (deepest use measured: under 2 KiB,
  `make STACKCHECK=1`), one page per CPU, small hash tables.
- **Pages zeroed on allocation:** memory never leaks between programs.
- **W^X:** code is read-only, data never executable (but anonymous memory a
  program asks to be executable: a JIT's code pool).

## Limits

- **CPUs:** up to 256 (xAPIC), 8 (GICv2); tested with 1 to 28 on x86, 4
  and 8 on AArch64.
- **Memory:** all of it (tested with 6 GiB on x86).
- **Processes and threads:** as many as memory allows (sanity cap about a
  million threads).
- **Drivers** may map device memory only; DMA buffers come from the kernel.
- **Many clients, one server:** a server thread handles one message at
  a time, so its clients wait their turn; they no longer wait for each
  other in the kernel. 40 clients calling one server thread (`make
  ipc-bench`, x86-64 on KVM): 0.44 µs per call on 1 CPU, 0.76 on 4, 0.52
  on 8, 0.51 on 16 (with the one kernel lock it was 0.43, 2.4, 3.3 and
  3.2 µs). A round trip alone: 0.45 µs (0.43 before).
- **Crashes:** up to 1 s of file changes can be lost if `fs` dies; `sync`
  reports it. If init fails, the system stops.
- **P-cores and E-cores:** tested in QEMU only, with `pcores=N` (QEMU
  cannot show a hybrid processor); not yet on a real hybrid PC. A thread
  is placed when it wakes; a running one moves only when another CPU
  asks it to (see above).
