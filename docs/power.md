# Power: off, restart, sleep, battery, lid

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

On a PC, power is ACPI: fixed registers the FADT table describes, and byte
code (AML) in the DSDT and SSDTs that says the rest (the sleep types, the
battery, the lid, what the embedded controller's events mean). The kernel
writes the registers; the AML runs in user space, in `/bin/acpid`
(x86-64), with the interpreter the touchpad driver also uses.

| Piece | What it does |
|---|---|
| `kernel/arch/x86_64/platform.c` | power off and S3: sleep type + SLP_EN in the PM1 control registers; restart: the FADT's reset register, the keyboard controller, a triple fault |
| `kernel/task.c`, `smp.c`, `ap.S` | suspend: the other CPUs parked, CPU 0's state saved, the waking vector (the CPUs' trampoline), everything back, the other CPUs started again |
| `user/lib/aml.c` | the AML interpreter: memory, I/O, PCI and embedded-controller fields, packages, Notify ([touchpad.md](touchpad.md)) |
| `user/acpid/acpid.c` | sleep types to the kernel, the SCI (power button, GPEs, the embedded controller's queries), battery / AC / lid on port `acpi`, `_PTS`/`_WAK`, what wakes the machine (`_PRW`, the clock's alarm), PCI state across a sleep |
| `user/init/init.c` | `INIT_RESUME`: the device drivers started anew after a sleep |
| `user/atlas` | the battery in the Deskbar's tray, Sleep in its menu, the lid (Settings: screen off or sleep), the display set up again after a sleep |
| `user/sh/sh.c` | `poweroff`, `reboot`, `suspend [N]` (root), `battery` |

## How it works

- **Off and restart.** `poweroff` / `reboot` call `power()` (library): the
  files are committed, then acpid runs `\_PTS(5)` (off) and asks the kernel
  (`SYS_POWER`). The sleep types come from `\_S5` and `\_S3`, which acpid
  evaluates at start and gives the kernel (`POWER_SET`); QEMU's are
  0 and 1, the Legion's 7 and 5. Without acpid, S5's type is 0 (QEMU).
- **Events.** One interrupt, the SCI (FADT; level, active low unless the
  MADT says otherwise). acpid reads the PM1 status (the fixed power
  button) and the GPE0 block: each enabled GPE runs `\_GPE._Lxx` or
  `_Exx` (one that fails is disabled); the embedded controller's GPE (its
  `_GPE`) means it has news: query (0x84), run `_Qxx`. The AML says what
  changed with `Notify()`: battery, AC, lid, power button.
- **The embedded controller.** Its ports from `_CRS` (0x62, 0x66), its
  space read and written with commands 0x80 / 0x81; `_REG(3, 1)` tells the
  AML it is there. (The Legion's battery fields are in the controller's
  memory window, read as memory.)
- **Battery.** `_STA` (present), `_BIX` (else `_BIF`) for the capacities,
  `_BST` for state, rate, remaining and voltage: percent and time left or
  to full. Read at start, after each Notify and every 30 s. `ACPI_STATUS`
  gives it; `ACPI_WAIT` waits for the next change (the desktop's thread).
- **Power button**: a clean power off, as `poweroff`. **Lid**: acpid only
  reports; the desktop turns the panel's backlight off, or sleeps
  (Settings, `/etc/display.conf`: `lid=sleep`).
- **Sleep (S3).** `suspend` (root), the Deskbar's menu, or the lid:
  `power(POWER_SLEEP)`: files committed; acpid keeps every PCI function's
  header and PCI Express control registers and the interrupt links'
  settings, arms the wake devices, runs `\_PTS(3)`, leaves only the wake
  GPEs on (below), and calls the kernel. The kernel parks the other CPUs (their
  threads go to CPU 0; they write their caches back and stop), saves CPU
  0's registers, FPU state and I/O APIC entries, sets the FACS's waking
  vector to the trampoline at 0x8000 and writes S3's type. On waking, the
  firmware jumps there in real mode; CPU 0 climbs back to 64-bit mode,
  reloads its address space, GDT, IDT, TSS, MSRs, PAT, local APIC, COM1,
  sets its TSC back to where it was (sleep time does not count), returns
  into the system call, starts the other CPUs as at boot and restores the
  I/O APIC. acpid writes the PCI state back, runs `\_WAK(3)`, and init
  kills the device drivers (all but acpid and the desktop): they restart
  in about a millisecond and set their devices up again, their clients
  reconnect. The desktop sets the display up again itself (it keeps the
  windows) and reads the clock again.
- **Waking.** What can wake the machine from S3: the fixed power button
  (PWRBTN_EN, as when awake), and the lid, the power button device and
  the embedded controller if their `_PRW` says so: a GPE of GPE0 and a
  deepest sleep state of 3 or more (a GPE block device's GPE is not used).
  acpid finds those at start (`lid: wakes from S3: GPE 6e`); before
  `_PTS(3)` it arms each (`_DSW(1, 3, 0)`, else `_PSW(1)`), then turns
  every other GPE off and clears all their status bits (a pending one
  would wake the machine at once). `suspend N` also sets the CMOS clock's
  alarm N s ahead (N < 3600: minute and second, any hour) and PM1's
  RTC_EN. Awake, acpid logs why (`awake (PM1 status 8100)`: WAK_STS and
  the power button; 8400: the clock; `, GPE 6e`: a wake GPE), runs
  `_WAK(3)`, disarms the devices (`_DSW(0, ...)` / `_PSW(0)`), the alarm
  and RTC_EN, enables its runtime GPEs again, runs the controller's
  pending queries (the lid's among them), and evaluates `_LID`: the
  desktop gets the lid open with the wake count and turns the panel on.
  No loop: a sleep asked within 1 s of waking is not done (`suspend: just
  woke: not again`: a lid closed while the machine went to sleep, its
  request queued), and the desktop does not sleep on a lid it finds closed
  in the same status as the wake (woken by the power button, lid closed:
  the panel stays off).
- **The Legion's lid.** Its tables give the lid, the power button and the
  controller no `_PRW` (Linux agrees: `/proc/acpi/wakeup` lists only PCI
  ports, xHCI and the time-and-alarm device), so acpid enables no GPE for
  them: the controller wakes the machine itself when the lid opens, told
  of the sleep by `_PTS(3)` (it sets the controller's BS3F). Linux in S3
  (`deep`) on this machine also keeps the controller's GPE 6E off (only
  wake devices' GPEs stay on) and, by the lid's wake count (2 suspends, 2
  lid wake events), is woken by it. `_WAK` reads the lid's state from the controller.

## Tests

- `make power-test` (QEMU, x86-64): `battery`; `suspend`, woken by the
  monitor's `system_wakeup`, then files, a new file, the network; a second
  sleep while `fsloop` writes and checks files; `suspend 3`, woken by the
  clock's alarm alone (PM1 status 8401 in QEMU, in 2 to 3 s); `reboot`; booted again,
  the file written after waking is there, and the power button
  (`system_powerdown`) powers off. Passes with 1, 4 and 8 CPUs, on `pc`
  and on `q35` (`-M q35 -global ICH9-LPC.disable_s3=0`: the reset
  register), and with the kernel built by sicc.
- `make aml-test`: `tests/aml/power.asl` (the Legion's patterns: `\_S3`
  under `If`, `_PTS`/`_WAK`, the controller, the battery's packages, a
  query and its Notify, PCI behind a bridge), and waking (`aml-test -W`):
  the lid by the controller's GPE through the Legion's `GPRW` (S3 allowed,
  then S4 only, then neither by the firmware's variables) armed by `_DSW`,
  the power button's plain package and `_PSW`, a GPE block's GPE and an
  S1-only device not chosen. QEMU's tables have no `_PRW`.
- During development, the interpreter ran acpid's methods on the real
  tables of the target's platform (Lenovo Legion Pro 7 16IRX8H,
  linux-hardware.org): `\_S3` 5/0, `\_S5` 7/0, `_PTS`, `_WAK`, `_REG`,
  every `_Qxx` (four call methods of SSDTs not in the dump), the `_Lxx` /
  `_Exx` GPE methods, `_BIX`, `_BST`, `_PSR`, `_LID`, with a fake memory.
- `make test`, `usb-test` (OVMF) power off through the new path.

## On the laptop: what to look for

In the log (serial console or log viewer), `acpid:` lines:

1. `\_S3: sleep type 5/0`, `\_S5: sleep type 7/0`, `FADT: SCI 9, PM1 events
   ..., control ..., GPE0 ...` (Intel's usual 1800, 1804, 1860). Linux on the
   laptop agrees on what can be checked without root: the SCI on IRQ 9
   (level), the controller's GPE 0x6E, the fixed power button, GPEs 11, 12,
   14, 61, 62, 66, 69, 6D, 72 enabled (acpid enables those with methods;
   Linux leaves 6F off: acpid disables one that fails or keeps coming).
2. `embedded controller: \_SB_.PC00.LPCB.EC0_`, `ports 62/66, status ..,
   GPE 110` (0x6E); `battery: ...BAT0`, `AC adapter: \_SB_.ADP0`, `lid:
   \_SB_.LID0`, `power button: \_SB_.PWRB`; then `battery 80%, charging: ...`
   and `AC on, battery in, lid open`. `aml: ... at 0x...` lines are AML the
   interpreter could not run.
3. Unplug the charger, close the lid: `embedded controller: query ..`, and a
   new status line. `battery` in a shell shows the same.
4. The power button: `power button: power off`.
5. `suspend` (root): `mk: suspend: N other CPUs stopped`, `mk: ACPI S3: PM1
   control 1804/0, sleep type 5/0`; press the power button: `mk: awake; TSC
   carried on`, `acpid: awake (PM1 status 8100)`, `PCI: .. registers written back`, `init:
   ... started anew after the sleep`, `atlas: awake: the screen set up
   again` (and in `/etc/igfx.log`, the modeset after waking, then the
   monitor ports gone over: one unplugged during the sleep turned off, one
   plugged in lit).
6. The lid: Settings, Power, "Lid closed: Sleep". Close the lid: `atlas: lid
   closed: sleep`, `acpid: suspend`, `mk: ACPI S3`. Open it: `mk: awake`,
   `acpid: awake (PM1 status ...)` (which bits: 100, the controller pressed
   the power button for the lid; a `GPE` part: a wake GPE), `\_WAK(3)`,
   `AC ..., lid open`, `atlas: lid open`, `atlas: awake: the screen set up
   again`, and the machine stays awake. No `lid: wakes from S3` line is
   expected at start (no `_PRW`). If opening the lid does not wake it, the
   power button still does; the log then says what the controller did.
7. `suspend 10` (root): woken after 10 s by the clock, `PM1 status 8400`
   (if the firmware lets the CMOS alarm wake; the Legion's clock is its
   time-and-alarm device, AWAC).

Untested on the laptop: all of it. Expected weak points: the display after
waking (igfx sets it up again from the state kept at start, but the
firmware's own display set-up, combo PHYs and all, is not redone; on the
firmware's screen alone it stays dark), the drivers' restart on real
devices (NVMe, xHCI, Wi-Fi, audio), `mk: the firmware did not restore the
MTRRs`, the lid's wake (the controller's own: nothing for acpid to arm).
