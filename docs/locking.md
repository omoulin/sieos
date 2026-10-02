# Locking in the SIEOS kernel

SIEOS's kernel has no big kernel lock. Like Solaris, it is built from
subsystems that each protect their own data with their own locks, taken in
a fixed order. This file describes the primitives, the locks, what each
one protects, and the order in which they are taken.

## The execution model

- **Kernel code runs with interrupts disabled.** Interrupts arrive in user
  mode and in the idle loop, never in the middle of kernel code. An LWP
  leaves its processor only when it blocks, or on its way back to user mode
  (preemption). What one LWP does between two blocking points is therefore
  atomic on its own processor, but not across processors. Shared data is
  protected by locks.
- **Interrupt handlers do little.** The local timer keeps the time, wakes the
  LWPs whose deadline has passed, and accounts processor time. Everything
  else runs in kernel threads (`disp.c`): LWPs of process 0 (`sched`), in
  the SYS class, above time-sharing.

  | Thread    | Priority | Work |
  |-----------|----------|------|
  | `intr`    | 165 | Device interrupts. The trap masks the line, acknowledges it and posts it; the thread runs the handlers and unmasks the line. |
  | `clock`   | 164 | The rest of each tick: interval timers, timerfds, the load average, the drivers' polls (USB, I2C HID, Wi-Fi), power management. It also kicks `netisr`. |
  | `netisr`  | 163 | The network: received frames (queued by `net_rx`), cards without interrupts, loopback, and the ARP, DHCP and TCP timers. |
  | `fsflush` | 160 | Once a second: commits old ext4 transactions, and reads disks that arrived after boot. |

  Until the boot is over (`kernel_running`), this work runs in the
  interrupts themselves.

## The primitives (`sync.h`, `sync.c`)

They follow Solaris's interfaces: `mutex(9F)`, `condvar(9F)` and `rwlock(9F)`.

- **`kmutex_t`.** `mutex_enter`, `mutex_exit`, `mutex_tryenter`, `mutex_owned`.
  - Adaptive (the default). A caller that finds the mutex held spins while
    the owner is running on another processor. It blocks when the owner is
    itself blocked.
  - May be held across blocking: disk I/O, or a `cv_wait` on another
    condition. Not recursive. A zeroed `kmutex_t` is an unlocked adaptive
    mutex.
  - `MUTEX_SPIN` (`MUTEX_SPIN_INITIALIZER`): spins only. For data that
    interrupt handlers touch; never held across blocking.
- **`kcondvar_t`.** `cv_wait`, `cv_wait_sig`, `cv_timedwait`,
  `cv_timedwait_sig`, `cv_timedwait_sig_hires`, `cv_signal`, `cv_broadcast`.
  - The mutex is released while the LWP sleeps and is held again on return.
  - Wake-ups may be spurious: callers loop on their condition.
  - The `_sig` forms end on a signal, or on the end of the LWP.
- **`krwlock_t`.** `rw_enter(RW_READER | RW_WRITER)`, `rw_exit`,
  `rw_tryenter`, `rw_downgrade`, `rw_tryupgrade`. Waiting writers hold back
  new readers.
- **`krmutex_t`.** A recursive adaptive mutex; SIEOS's own (Solaris has no
  equivalent). Used for file-system locks: ext4 re-enters itself through
  `iput`, and a copy to user memory may fault on a mapping of the same file
  system.
- **Sleep queues.** Hashed by the address waited on: Solaris's sleepq and
  turnstile tables, without priority inheritance.
  - Blocking takes two steps. First the LWP joins the queue; then, its
    interlock released, it sleeps only if no one has dequeued it meanwhile.
    No wake-up is lost.
  - Every wake-up dequeues: by a waker, a signal (`lwp_wake_sig`, for
    interruptible sleeps) or a deadline (the clock).
- **`struct spinlock`.** The low-level spin lock behind the dispatcher, the
  sleep queues and the allocators. Its spin (`cpu_relax`) answers TLB
  shootdowns, since a processor spinning with interrupts off would otherwise
  never acknowledge one.

## The dispatcher (`disp.c`)

- **The dispatcher lock** (`disp_enter`, a spin lock) protects every LWP's
  state and the choice of the next LWP to run.
- **Switching.** `swtch()` is called with the lock held and the caller's
  state already set; the LWP switched to releases it (`switch_finish`,
  `forkret`, `kthread_start`).
- **`oncpu`.** An LWP stays `oncpu` until the switch away from it is
  complete. Meanwhile no other processor runs it, even if a wake-up made it
  runnable (it then keeps its processor), and its stack is not freed
  (`lwp_wait_offcpu`).

## The locks, and what they protect

| Lock | Kind | Protects |
|---|---|---|
| `pidlock` | adaptive | The process table (allocation, states, pids), parent links, process groups and sessions, `waitid` (`p_cv`). Held to use another process (signal it, read it): it cannot be freed meanwhile. |
| `p->p_lock` | adaptive | A process's signals (dispositions, pending sets, its LWPs' masks), its LWPs (the list, `must_exit`, suspend), stop and exit state, interval timers, limits, credentials, working and root directories (`p_lwpcv`: LWPs exiting). |
| `p->as_lock` | rwlock | The address space's areas and `brk`. Writers: `mmap`, `munmap`, `mremap`, `mprotect`, `brk`, `shmat`/`shmdt`, `exec`, exit. Readers: `fork`, `msync`, `mincore`. |
| `p->vmlock` | spin | Areas and page tables, for page faults (which do not take `as_lock`). |
| `p->p_fdlock` | adaptive | The descriptor table. System calls use files through references (`getf`/`releasef`, or `fd_file`, held until the call returns). |
| `f->f_offlock` | adaptive | A file's offset, across the read or write that moves it. |
| `lwptab_lock`, `ftable_lock` | spin | Free slots of the LWP and file tables. |
| `fs->lockp` | recursive | A file system's inodes: attributes, data, directories, shared pages. Taken by the VFS dispatchers (`fs_enter`). All ext4 volumes share `ext4_lock`, because ext4 works on its current volume `V`. |
| `mount_lock` | rwlock | The mount table. |
| `bc_lock` | adaptive | The block cache's table, not held during I/O (readers of a block being read wait on `bc_cv`). |
| `blk_lock` | adaptive | Changes to the block-device table (registration, partitions, lofi, mount counts). |
| `flock_mx` | adaptive | Record locks. |
| pipe `lock` | adaptive | One pipe's state (`fifo_lock` comes before it, for the open FIFOs' list). |
| `tty_lock` | adaptive | Every terminal and pseudo-terminal (each terminal's `cv`, each pty master's `mcv`). |
| `input_lock` | adaptive | `/dev/events`. |
| `net_lock` | adaptive | The network stack: interfaces, ARP, IPv4/IPv6, TCP, UDP, the IP sockets. Socket calls sleep with it as interlock (`net_sleep`). |
| `rxq_lock` | spin | Frames the drivers received, queued for `netisr`. |
| `unix_lock` | adaptive | Every AF_UNIX socket. Descriptors in discarded messages are closed after it is released. |
| `fdext_lock` | adaptive | eventfds, timerfds. Each epoll set has its own lock. |
| `ipc_lock` | adaptive | System V message queues, semaphores, segments (attach counts are atomic). |
| `umtxq[].lock` | adaptive | User-mutex waits and wakes, hashed by key. |
| `rtq_lock` | spin | Queued real-time signals. |
| `display_lock` | adaptive | Displays' owners and modes. |
| `cons_lock`, `klog_lock` | spin | The console, the kernel log (kprintf from anywhere). |
| `kmem_lock`, `pmm_lock`, `kmap_lock` | spin | The kernel heap, frames, the kernel's MMIO window. |
| `cf8_lock`, `io_lock`, `pic_lock` | spin | PCI configuration ports, I/O APIC and PIC registers. |
| `clock_lock` | spin | The real-time clock's offset and slew. |
| drivers | own | `ata_lock`, NVMe's controller lock, virtio-blk's, xhci's busy flag, iwlwifi's device lock, NIC receive-ring locks, nvgpu's per-client lock. |

## The order

Outer first. A thread may take a lock only if it holds none that comes
after it.

1. `mod_lock`, `display_lock`, `pw_lock`, `flock_mx`
2. The subsystems' locks: `unix_lock`, `net_lock`, `tty_lock`, `input_lock`,
   `ipc_lock`, a pipe's lock (after `fifo_lock`), `fdext_lock`, an epoll
   set's lock, `umtxq`
3. `as_lock`
4. File-system locks (`fs->lockp`), `mount_lock`
5. `pidlock`
6. `p_lock` (one process's at a time)
7. `p_fdlock`, `f_offlock`
8. `bc_lock`, `blk_lock`, driver locks
9. Spin locks: `vmlock`, `rxq_lock`, sleep-queue buckets, `disp_lock`,
   `kmem_lock`, `pmm_lock`, `cons_lock` and the rest. A spin lock is never
   held while blocking, or while taking an adaptive lock.

Some rules follow from this order:

- **User memory.** Code may copy to and from user memory while holding
  adaptive locks: the system call checked the range first (`user_ok`), so
  its pages are present. A fault on a file mapping enters that file
  system's lock.
- **Closing files.** `file_close` may close a socket, a pipe or a
  terminal, so it is never called holding those subsystems' locks. AF_UNIX
  defers its closes until `unix_lock` is released.
- **procfs.** It snapshots the process table under `pidlock`, then builds
  its answers (`getcwd`'s lookups, the caller's buffer) without it.
- **Signals to a process group.** These come from a terminal (under
  `tty_lock`) and take `pidlock`. Finding a session's terminal (procfs,
  under `pidlock`) reads the terminal table without `tty_lock`.

## Debugging

- **Inspecting a hung system.** An NMI (QEMU's `inject-nmi`) makes each
  processor print where it is and the return addresses on its stack, raw on
  the serial line; the boot processor also lists every LWP with its state
  and wait channel. The addresses resolve with `build/kernel.nm`.
- **`panic`.** It names the processor and the LWP.

## Not done yet

- **The dispatcher.** It has one lock and scans the LWP table. Per-processor
  run queues, as Solaris has, would scale better on many processors.
- **Priority inheritance.** Solaris's turnstiles lend a blocked LWP's
  priority to the mutex owner. SIEOS's sleep queues do not.
- **Coarse locks.** The network stack, terminals, AF_UNIX sockets and ext4
  each have one lock (as Solaris 2.0's STREAMS and UFS had coarse ones at
  first). Finer locks, per socket, per inode or per terminal, come next
  where contention shows.
