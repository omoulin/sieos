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
| `user/acpid/acpid.c` | sleep types to the kernel, the SCI (power button, GPEs, the embedded controller's queries), battery / AC / lid on port `acpi`, `_PTS`/`_WAK`, PCI state across a sleep |
| `user/init/init.c` | `INIT_RESUME`: the device drivers started anew after a sleep |
| `user/atlas` | the battery in the Deskbar's tray, Sleep in its menu, the lid (Settings: screen off or sleep), the display set up again after a sleep |
| `user/sh/sh.c` | `poweroff`, `reboot`, `suspend` (root), `battery` |

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
  settings, runs `\_PTS(3)`, turns the GPEs off (only the power button
  wakes), and calls the kernel. The kernel parks the other CPUs (their
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

## Tests

- `make power-test` (QEMU, x86-64): `battery`; `suspend`, woken by the
  monitor's `system_wakeup`, then files, a new file, the network; a second
  sleep while `fsloop` writes and checks files; `reboot`; booted again,
  the file written after waking is there, and the power button
  (`system_powerdown`) powers off. Passes with 1, 4 and 8 CPUs, on `pc`
  and on `q35` (`-M q35 -global ICH9-LPC.disable_s3=0`: the reset
  register), and with the kernel built by sicc.
- `make aml-test`: `tests/aml/power.asl` (the Legion's patterns: `\_S3`
  under `If`, `_PTS`/`_WAK`, the controller, the battery's packages, a
  query and its Notify, PCI behind a bridge).
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
   carried on`, `acpid: awake`, `PCI: .. registers written back`, `init:
   ... started anew after the sleep`, `atlas: awake: the screen set up
   again` (and in `/etc/igfx.log`, the modeset after waking, then the
   monitor ports gone over: one unplugged during the sleep turned off, one
   plugged in lit).

Untested on the laptop: all of it. Expected weak points: the display after
waking (igfx sets it up again from the state kept at start, but the
firmware's own display set-up, combo PHYs and all, is not redone; on the
firmware's screen alone it stays dark), the drivers' restart on real
devices (NVMe, xHCI, Wi-Fi, audio), `mk: the firmware did not restore the
MTRRs`, opening the lid as a wake source (not armed: the power button wakes).
