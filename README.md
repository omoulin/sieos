# SIEOS — Synthetic Intelligence Enhanced Operating System

<img src="docs/logo.svg" width="120" alt="SIEOS logo: a blue ring crossed by a light stratum, with an amber node">

A small Unix-style 64-bit kernel for x86_64. It boots from a hybrid BIOS/UEFI
ISO, runs on multiple CPUs (SMP) and supports multiple users with Unix
permissions. It has processes with signals, job control and environment
variables, pipes, and a read/write ext4 file system. It boots straight to a
graphical login screen and the Facet desktop, and a text console is one menu entry
away.

```
        +--------------------------------------------------------------+
        |          ____      ___     _____      ___      ____          |
        |         / ___|    |_ _|   | ____|    / _ \    / ___|         |
        |         \___ \     | |    |  _|     | | | |   \___ \         |
        |          ___) |    | |    | |___    | |_| |    ___) |        |
        |         |____/    |___|   |_____|    \___/    |____/         |
        |                                                              |
        |       Synthetic Intelligence Enhanced Operating System       |
        +--------------------------------------------------------------+
```

## Build and run

Requirements (Ubuntu/Debian): `gcc g++ binutils make python3 curl grub-pc-bin
grub-efi-amd64-bin xorriso dosfstools e2fsprogs qemu-system-x86 ovmf`.

The kernel is built by the host gcc in freestanding mode. The user programs are built
by an `x86_64-pc-sieos` cross compiler (GCC 15.2, binutils 2.45) against the C library
(musl 1.2.5 adapted to SIEOS). The first `make` downloads the pinned GCC, binutils,
GMP, MPFR and MPC releases (checked by SHA-256) and builds the cross toolchain into
`build/cross`, which takes a while; later builds reuse it.

```sh
make            # build/sieos.iso (BIOS + UEFI) and build/disk.img (the cross toolchain first)
make native     # GCC and binutils for SIEOS itself; then 'make newdisk' puts them on the disk
make run        # BIOS boot, 4 CPUs, e1000 network, persistent disk, QEMU window + serial here
make run SMP=8  # any CPU count up to 16
make run-uefi   # the same through UEFI firmware (OVMF)
make run-nox    # serial console only (no window)
make run-iso    # the ISO alone: root fs is a RAM disk shipped on the ISO
make fsck       # check build/disk.img with e2fsck
make newdisk    # reset build/disk.img to the pristine root file system
```

The guest gets 1 GiB of memory (`make run MEM=2G` for more). With `make native` done, the
disk carries `gcc`, `g++`, `as`, `ld` and the other binutils, the C and C++ headers and
libraries, and `sieos.h`/`libsieos.a`, so programs can be compiled on SIEOS itself:

```sh
gcc -O2 -o hello hello.c           # dynamically linked against /usr/lib/libc.so
g++ -O2 -o hello hello.cc          # libstdc++.so.6, libgcc_s.so.1
gcc -static -O2 -o hello hello.c   # static
```

Log in as **root / root** or **user / user**, then change these passwords with `passwd`.

The system boots into the graphical login screen (`sdm`). To get a text console, either:
- pick **SIEOS (text console)** in the GRUB menu (press Esc during the 2-second
  countdown to show it);
- click **Console** on the login screen, which runs one text login and then returns
  to the login screen;
- or put `CONSOLE=text` in `/etc/default/init`.

Networking uses QEMU's user-mode network. The guest gets 10.0.2.15 by DHCP, the host is
reachable as `10.0.2.2` (named `host` in `/etc/hosts`), and host port 8080 is forwarded
to the guest's port 80. Try running `httpd` as root in SIEOS, then `curl http://localhost:8080/`
on the host. Change the forwarding with `make run NET=user,model=e1000,hostfwd=...`.

The same ISO boots on legacy BIOS (El Torito, GRUB i386-pc) and on UEFI
(EFI system partition with GRUB x86_64-efi). It also carries a GPT/MBR hybrid,
so it can be written to a USB stick.

## Features

| Area        | Implementation |
|-------------|----------------|
| Boot        | GRUB multiboot2 → 32-bit stub builds page tables → long mode → higher-half kernel. Works on BIOS and UEFI. |
| Console     | VGA text mode, or a GOP/VBE linear framebuffer with an 8x16 font. Both handle ANSI colours and are mirrored to COM1. PS/2 keyboard. |
| SMP         | CPUs found through the ACPI MADT and started with INIT-SIPI-SIPI via a real-mode trampoline. Per-CPU GDT/TSS/idle process reached through `%gs` (`swapgs`). Local APIC timers preempt on every CPU, and idle CPUs are woken by reschedule IPIs. A big kernel lock serialises kernel code while user processes run in parallel. |
| Memory      | Bitmap frame allocator, 4-level paging with a per-process address space, direct map of physical memory, kernel heap. |
| System calls | ABI v2, Solaris-inspired (Solaris errno values, signal numbers, flags and structure layouts), entered with the `syscall` instruction; specified in [`docs/abi-v2.md`](docs/abi-v2.md) and `abi/include/sieos/`. |
| Processes   | Processes with LWPs (threads), preemptive round-robin scheduling across CPUs, copy-on-write `fork`, demand paging, `mmap` (private and shared, anonymous and file), `execve` of static, PIE and dynamically linked ELF64 programs (the kernel loads `PT_INTERP`), set-user-ID/set-group-ID, `waitid`, rlimits (with `RLIMIT_VMEM`) and rusage, ELF core files (`RLIMIT_CORE`), System V IPC, `/proc` (Solaris layout), `mount`/`umount2` of tmpfs and proc, `nanosleep` to the microsecond. |
| Signals     | Solaris numbering (1-41, real-time 42-73, queued). Default actions: terminate, core, stop, continue, ignore. `sigaction` handlers get a `ucontext`/`siginfo` frame, with `SA_RESTART`, `SA_RESETHAND`, `SA_NODEFER`, `SA_ONSTACK`; per-LWP masks, `sigtimedwait`, `sigqueue`, and system-call restart. |
| C library   | musl 1.2.5 adapted to ABI v2 (`libc/`), shared (`/usr/lib/libc.so`, which is also the dynamic linker `/lib/ld-musl-sieos64.so.1`) and static, with POSIX threads and the Solaris extensions (`thr_*`, `_lwp_*`, `gethrtime`, `processor_bind`, `sig2str`, ...). libstdc++ and libgcc_s are shared too. |
| Toolchain   | An `x86_64-pc-sieos` cross compiler (GCC 15.2 C/C++, binutils 2.45) built by `make`, and the same compiler hosted on SIEOS (`make native`): SIEOS can compile programs, including its own. |
| Job control | Process groups and sessions (`setpgid`, `setsid`), a controlling terminal (`TIOCSCTTY`), and the foreground group (`tcsetpgrp`). ^C/^Z/^\ send SIGINT/SIGTSTP/SIGQUIT; background reads get SIGTTIN, and so do writes when `TOSTOP` is set; a session leader's exit sends SIGHUP. |
| Terminal    | termios with canonical and raw modes, `ECHO` (used for password prompts), and editable control characters. |
| Users       | Real, effective and saved uid/gid plus supplementary groups; `setuid`, `seteuid`, `setgid`, `setgroups`; `umask`; `access`. |
| Permissions | Owner/group/other `rwx` checks on every open, exec and directory search. Creating or removing files needs write+search on the directory. Sticky directories (`/tmp`) restrict deletion. The superuser bypasses checks. `chown` is restricted to root, as in Solaris's `rstchown`, and a non-root `chown` clears the set-ID bits. |
| Network     | PCI enumeration and an Intel e1000 driver with polled descriptor rings. Ethernet, ARP (with a queue for packets awaiting resolution), IPv4 routing through a gateway, loopback (127.0.0.0/8), ICMP echo, UDP, **TCP** and a **DHCP** client at boot. TCP covers the three-way handshake, MSS, flow control, retransmission with backoff, zero-window probes, FIN/RST, TIME_WAIT and listen/accept backlogs. The **BSD sockets** API integrates with `read`/`write`/`poll`. Ports below 1024 and raw sockets require root. |
| Files       | ext4 read/write (see below), tmpfs (`/tmp`, `/dev/shm`), pipes and named FIFOs, `AF_UNIX` sockets (with descriptor passing), pseudo-terminals, `poll()`, record locks, and device nodes `/dev/console`, `/dev/tty`, `/dev/null`, `/dev/zero`, `/dev/random`, `/dev/urandom`, `/dev/fb0` and `/dev/events` stored as real ext4 character-special inodes. |
| Random      | `/dev/random` and `/dev/urandom` never block. They output a ChaCha20 keystream that is rekeyed after every read, from a pool fed by interrupt timing, the RTC and RDSEED/RDRAND when the CPU has them. |
| TLS         | A TLS 1.3 client in user space (`user/tls`): X25519 and P-256 key exchange, AES-128/256-GCM, RSA-PSS/PKCS #1 signatures, and X.509 chain and host-name validation against `/etc/ssl/certs.pem`, plus optional site roots in `/etc/ssl/local.pem`. |
| ext4        | Extent trees, 64bit, flex_bg, and metadata_csum/gdt_csum checksums on every metadata structure. Supports create, write, truncate, mkdir, unlink, rmdir and `rename` (replacing targets, moving directories between parents with `..` and link-count updates). `getcwd` is computed from the directory tree, so it stays right after renames. |

### User space

The programs are C programs on the C library, linked dynamically; `libsieos`
(`user/libsieos`, `user/include/sieos.h`) holds the SIEOS extensions they share:

- **Shell and login:** `init`, `login`, `sh` (pipelines, `&&`, `||`, `;`, `&`, redirections including `2>&1`, `~`, variables `NAME=value`, `NAME=value cmd`, `$NAME`/`${NAME}`/`$?`, built-ins `cd [-] pwd exit umask export unset set jobs fg bg wait kill`).
- **Environment:** `env`, `printenv`; login sets `HOME USER LOGNAME SHELL PATH TERM`, and programs are looked up through `$PATH`.
- **Users and permissions:** `su` and `passwd` (both set-user-ID root), `useradd`, `id`, `whoami`, `groups`, `chmod` (octal and symbolic), `chown`, `chgrp`.
- **Files and text:** `ls -lad`, `cat`, `cp`, `mv` (rename), `tail`, `rm -rf`, `mkdir -p`, `rmdir`, `touch`, `grep`, `head`, `wc`, `hexdump`, `stat`, `tty`, `yes`, `true`, `false`.
- **Network:** `ifconfig`, `ping` (setuid root), `host`, `nc` (TCP/UDP client and server),
  `wget`, `httpd` (web server with directory listings), `netstat`. The resolver uses
  `/etc/hosts` and then DNS.
- **System:** `ps` (with CPU column), `lscpu`, `nproc`, `kill`, `free`, `df`, `mount`, `umount`, `uname`, `date`, `uptime`, `sleep`, `sync`, `clear`, `sifetch`, `halt`, `reboot`.
- **Development** (with `make native`): `gcc`, `g++`, `cpp`, `as`, `ld`, `ar`, `nm`, `objdump`, `readelf`, `strip` and the other binutils.
- **Self-tests:** `fstest`, `forktest`, `sigtest`, `abi2test`.

Accounts live in `/etc/passwd`, `/etc/group` and `/etc/shadow`, the last with mode 0400.
Passwords are salted SHA-256, iterated 5000 times. The `$5a$` format is specific
to SIEOS and is implemented in `user/libsieos/crypt.c` and `tools/mkshadow.py`.
Boot sequence: the kernel passes its command line (from GRUB) to `/sbin/init`, which
runs `/etc/rc` and then keeps a login running on the console: the graphical `/sbin/sdm`
by default, or `/bin/login` when the command line contains `text` or `single`. If `sdm`
fails three times in a row, init falls back to the text login.

### Graphical login (sdm)

`sdm`, the SIEOS display manager, draws a Strata-style card. It shows the logo, one
clickable chip per login account, and Name and Password fields. The buttons are
**Log In**, **Console**, **Restart** and **Shut Down**.
- Keys: Tab or the arrow keys switch fields, Enter moves on or logs in, and Esc clears
  the field.
- Authentication uses the same `/etc/shadow` hashes as `login`, with a one-second delay
  after a wrong password.
- On success, `sdm` gives the devices to the user (`/etc/logindevperm`), drops to the
  user's credentials and becomes that user's Facet session.
- **Log Out** in Facet returns to the login screen.

### Facet Desktop (graphical interface)

The desktop starts when you log in on the graphical login screen. From a text console,
run `facet` to start it, and use **Log Out** to return. Its look is called **Strata**: a
dark, layered-stone desktop with graphite surfaces, warm light text and one amber accent.
It borrows the calm surfaces of a modern dock and the working habits of a Solaris-era
workstation. All artwork is original.

- **Spine** (the dock down the left edge):
  - the **SIEOS gem**, which opens the main menu: applications, every open window with
    its workspace, and Log Out;
  - one rounded tile per application: Terminal (sia), Shell, Files, System Monitor,
    Network Status and Clock. An amber mark shows which are running. Click a tile to open
    the application or bring its windows forward, right-click it for a new window, and
    hover over it for its name;
  - four **workspaces**. The current one is lit in its colour, and a dot marks those
    holding windows;
  - the clock.
- **Windows** have slightly rounded title bars.
  - The **window-menu button** on the left offers Minimize, Zoom, Send to Workspace N and
    Close, and the close box is on the right.
  - A band down the left edge takes the workspace colour (amber, blue, moss, plum) when
    the window has the focus.
  - Drag the title to move, double-click it to zoom, and drag the bottom-right corner to
    resize. Windows cast soft shadows.
- **sia strip** (along the bottom): the assistant's command line.
  - Press Ctrl+Space, click the strip, or simply start typing on the empty desktop, then
    press Enter. The line goes to the sia terminal, which opens if needed.
  - A light shows whether a model is connected and whether sia is working. Next to it are
    the model name and sia's last action, then CPU use and the IP address.
- **Keys:**
  - Ctrl+Space: the strip;
  - Ctrl+Alt+1..4 or Ctrl+Alt+Left/Right: workspaces;
  - Alt+Tab: next window;
  - Alt+F4: close.
- **Applications:** Terminal (sia, or the plain shell), Files and Viewer, System Monitor,
  **Network Status** (addresses, a live traffic graph and open sockets), Clock and About.
  All of them use the dark theme.

The drawing library (`gfx.c`) now includes alpha blending, antialiased rounded rectangles
and soft shadows, all in integer arithmetic.

Kernel support behind it:
- `/dev/fb0`, which is mapped into the desktop process with `fbmap()`. The text console
  pauses while the desktop runs and repaints when it exits.
- `/dev/events` for keyboard and mouse events. Under QEMU, VMware and VirtualBox the
  mouse uses the hypervisor's absolute pointer (vmmouse), so the cursor follows the host
  pointer and the emulator never grabs the mouse. Elsewhere it falls back to a PS/2
  mouse, which is fully reset at boot so it always sends standard 3-byte packets.
- Pseudo-terminals (`openpty()`) and `poll()`.

`login` hands `/dev/fb0` and `/dev/events` to the console user through `/etc/logindevperm`,
as on Solaris.

### sia: the assistant (libsia), the terminal and the desktop strip

The assistant lives in a library, **libsia** (`user/libsia`, built as `libsia.a` on top of
`libtls.a`). Any SIEOS program can link it: it provides the Azure AI Foundry client, the
conversation engine, tools, configuration and the desktop channel (see `libsia.h`). Three
programs use it:

- **`/bin/sia`, the terminal.** A Facet **Terminal** opens it; **Shell Terminal** in the
  menus always opens the plain shell.
  - **Plain lines are commands.** They run directly with `/bin/sh -c`, attached to the
    terminal, without involving the model. `cd`, `export` and `unset` are handled by the
    terminal itself.
  - **`/sia REQUEST` asks the model**, for example `/sia which process uses the most
    memory?` or `/sia open the clock`. The model can run commands, and inside Facet it can
    also open applications and manage windows.
  - **A failed command goes to the model automatically.** This covers "not found" and
    non-zero exits that print an error. If there is one clear fix, as with `lss -l /etc`, the
    model runs the corrected command and says what it changed. Otherwise it explains the
    problem and lists the alternatives.
  - **Terminal commands:** `/help`, `/shell` (full shell with job control), `/clear`,
    `/auto on|off`, `/model`, `/setup` and `/tools`. Ctrl-C interrupts.
- **`/bin/sia-agent`** runs libsia without a terminal and speaks JSON lines on
  stdin/stdout. Facet's strip uses it.
- **The Facet strip.**
  - Press Ctrl+Space, or start typing on the desktop, then ask for anything: "Open a new
    terminal for me", "launch the system monitor", "move the clock to workspace 2 and show
    me that workspace", "how much disk is free?".
  - Replies, the actions taken and short command output appear in a panel above the strip.
  - A change asks `Enter/y` (run), `n/Esc` (skip) or `a` (always); Esc interrupts.
- **Desktop tools.** Facet gives every program it starts a private pipe pair (fds 3/4,
  `SIEOS_DESKTOP=3,4`). libsia's desktop tools use it to open applications and to list,
  focus, close and move windows or switch workspaces. They are available in the strip and
  in every terminal.
- **Setup.**
  - The first use asks for the endpoint, the model or deployment name, and an API key
    (typed hidden), and stores them in `~/.sia/config` (mode 0600). Accepted endpoints are
    `https://NAME.openai.azure.com/`, `https://NAME.services.ai.azure.com/`, a serverless
    `https://X.REGION.models.ai.azure.com`, or a full `.../chat/completions` URL.
  - **Pre-registered model.** If an `ai.config` file sits at the top of the source tree
    when you build, `make` writes its settings into `~/.sia/config` for root and user. It
    can hold `key=value` lines, `key: value` lines or JSON, and the build never prints the
    values. Use `make AI_CONFIG=path` to use another file, and `make newdisk` to put the
    settings on an existing `disk.img`. `ai.config` is in `.gitignore`. The key ends up
    inside `rootfs.img`, `disk.img` and `sieos.iso`, so treat those images as secret too.
  - Without a model, or if it can't be reached, terminals start the standard shell.
    `sia --setup` reconfigures and `sia --off` unregisters.
- **Safety.**
  - A change asks for confirmation, unless it is exactly the command you typed or
    `/auto on` is set.
  - Read-only programs, and pipelines made only of them, run without asking.
  - Commands never inherit the desktop channel.
- **Network path.** Requests go out over SIEOS's own TCP/IP stack, DNS resolver and
  TLS 1.3 client (`libtls`).

For testing without Azure, `tools/mock_azure.py` serves a scripted chat-completions
endpoint over HTTPS, and `make tls-test` checks the cryptography against Python's
implementations.

### Try job control

```
user@sieos:~$ sleep 100 &
[1] 5
user@sieos:~$ cat
^Z
[2]+  Stopped                  cat
user@sieos:~$ jobs
[1]-  Running                  sleep 100
[2]+  Stopped                  cat
user@sieos:~$ bg %2 ; kill %1 ; fg %2
```

## Limitations

- No journaling: writes go straight to disk (write-through). A disk whose
  journal needs recovery is mounted read-only.
- Adding entries to hashed (htree) directories and allocating in
  `BLOCK_UNINIT` groups are not supported. The small images built here don't use them.
- No symlink following. The desktop renders in software, with an 8x16 bitmap font only.
- Network: no IPv6, no IP fragment reassembly, no TCP congestion control or
  out-of-order reassembly (out-of-order segments are dropped and retransmitted), and
  the NIC is polled every 10 ms rather than interrupt-driven.
- TLS: server certificates must be RSA (ECDSA chains are rejected); there is no
  session resumption. sia sends one request per connection and doesn't stream.
- SMP uses a big kernel lock: user code scales across CPUs, kernel code
  does not. Device interrupts go to the boot CPU through the 8259 PIC; there
  is no I/O APIC routing.

## Layout

```
kernel/          kernel sources and include/
user/libsieos/   SIEOS extensions and helpers over the C library (user/include/sieos.h)
user/bin/        user programs
user/facet/      the Facet desktop (window manager, drawing library, applications)
user/sdm/        the graphical login screen (/sbin/sdm), built on Facet's drawing code
user/tls/        libtls: TLS 1.3 client and cryptography (SHA-2, HKDF, AES-GCM, X25519, P-256, RSA, X.509)
user/libsia/     libsia: the assistant library (JSON, HTTP, Azure AI Foundry client, engine, tools)
user/sia/        sia (terminal) and sia-agent (headless, for the Facet strip) on libsia
rootfs/          files copied into the root file system (/etc, /home, /root)
tools/           ISO/FAT/shadow/permission build helpers, font converter
iso/boot/grub/   GRUB configuration
build/           output: kernel.elf, sieos.iso, rootfs.img, disk.img
```

## Credits

All kernel, libc, user-program and tool source code was written from scratch
for SIEOS. It follows public specifications: the ext4 disk layout, Multiboot2,
ELF64 and the Intel/AMD manuals. It borrows conventions, not code, from Unix,
Solaris and Linux: errno, signal and ioctl numbers, and the system-call
register convention.

Third-party components:

- **GRUB 2** (GPLv3) is the bootloader. It is placed on the ISO as a separate
  program and not linked into SIEOS.
- **Font:** `kernel/font8x16.c` is generated by `tools/psf2c.py` from
  `Lat15-VGA16.psf.gz` in the console-setup package. That package's copyright
  file states that the console fonts are in the public domain.
- The ASCII-art banner uses the letter shapes of figlet's "standard" font.
- The SIEOS logo ("Orbit Node", `docs/logo.svg`) is an original design: a blue ring crossed
  by a light stratum, with an amber node for the assistant. The kernel draws it beside the
  boot banner, and Facet draws it on the login screen, the spine and the About window.
- The Facet desktop's Strata design (colours, spine dock, frames, icons, cursor, layout)
  was created for SIEOS. It is inspired in general terms by modern docks and 1990s
  workstation desktops (workspaces, window menus), but uses no artwork, names, logos or
  code from macOS, CDE, BeOS, Haiku, IRIX or any other system.
