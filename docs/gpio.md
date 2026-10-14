# GPIO: the Raspberry Pi's pins

SIEOS gives every program the pins of a Raspberry Pi's 40-pin header
through one server, `/bin/gpiod` (port `gpio`), a small C library
(`gpio.h`, in `libsieos.a`), and a command, `/bin/gpio`. On a machine
without GPIO (x86-64, QEMU) the server **simulates** 28 pins, so programs
and tests run the same everywhere.

## From the shell

```
$ gpio                      # every pin: mode, level, pull
$ gpio out 17 1             # BCM 17 (header pin 11): an output, high
$ gpio get 17
1
$ gpio in 27 up             # an input with a pull-up
$ gpio blink 17 250         # on/off every 250 ms, played by the server
$ gpio stop
$ gpio selftest             # nothing wired: every free pin checked, then put back
```

## From C

```c
#include "mk.h"
#include "gpio.h"

int main(void)
{
    gpio_mode(17, GPIO_OUT, GPIO_PULL_NONE);        /* an LED on BCM 17 */
    gpio_mode(27, GPIO_IN, GPIO_PULL_UP);           /* a button to ground on BCM 27 */
    while (gpio_read(27)) {                         /* until the button is pressed */
        gpio_write(17, 1); sleep_ms(200);
        gpio_write(17, 0); sleep_ms(200);
    }
    uint32_t steps[] = { 1u << 17, 0, 1u << 17, 0, 0, 0 };   /* a pattern: two short blinks, a pause */
    gpio_play(1u << 17, steps, 6, 100000, 1);       /* 100 ms a step, looping, played by the server */
    return 0;
}
```

Build it on SIEOS with `cc blink.c -o blink` (or in the system's build,
like any program in `user/`). Every call returns `>= 0`, or `-EBUSY` (a
pin a device uses), `-EINVAL` (no such pin), `-EPERM` (writing an input).

| Call | |
|---|---|
| `gpio_info(board, cap, &real)` | the number of pins (28), the board's name; `real`: 0 when simulated |
| `gpio_mode(pin, GPIO_IN / GPIO_OUT, GPIO_PULL_NONE / _DOWN / _UP)` | a pin's direction, and an input's pull |
| `gpio_write(pin, 0 / 1)`, `gpio_read(pin)` | an output's level; any pin's level |
| `gpio_state(pins)` | every pin's mode, pull and level (and the levels as a bit mask) |
| `gpio_play(mask, steps, n, step_us, loop)` | a pattern: `n` steps, each the levels of the pins in `mask`, one every `step_us` (at least 100 us), once or looping |
| `gpio_playing(&rounds)`, `gpio_stop()` | the step being played (-1: none) and the rounds done; stop |
| `gpio_record(levels, max)` | every pin's level at each step of the pattern as it played (bit `p`: pin `p`): the inputs' recording |
| `gpio_selftest(res)` | the self-check (below): `res[p]`, the checks pin `p` passed (`GPIO_CHK_...`; 0: skipped); returns the pins that failed |

## The self-check (nothing wired)

`gpio selftest` checks the pins with **nothing wired to them** (no HAT,
no jumper wires: it drives every free pin high and low). For each free
pin:

- **out 1, out 0**: made an output, set to 1, then 0, and read back
  through the pin's own input (the pad's input buffer: on the Pi 5,
  `SYS_RIO0`'s input register, the pad's input enabled);
- **pull-up, pull-down**: made an input; with nothing on it, it must read
  1 with its pull-up and 0 with its pull-down;
- **kept**: then it is put back as it was (its function, direction,
  level and pull: the registers read before, written back, read again).

Left out ("skipped"): the pins a device uses (`busy`), 14 and 15 (the
serial console's), 2 and 3 (I2C1: the board pulls them up with 1.8 kOhm,
a pull-down cannot win), 0 and 1 (the HATs' ID EEPROM). It is refused
while a pattern plays (`gpio stop` first). On a real Pi:

```
$ gpio selftest
gpio selftest: Raspberry Pi 5 (RP1); nothing may be wired to the pins
  pin  out 1  out 0  pull-up  pull-down  kept
    0  skipped: the HAT ID EEPROM's
    ...
    4  ok     ok     ok       ok         ok    PASS
    ...
   14  skipped: the serial console's
   ...
   27  ok     ok     ok       ok         ok    PASS
selftest: PASS: 22 pins checked, 0 failed, 6 skipped
```

**Pass**: the last line says `PASS`, and every pin checked says `PASS`
(22 pins, or fewer if a device uses some: those say `used by a device`).
A `FAIL` names the check: `out 1` / `out 0` failing on every pin means the
levels written do not reach the pads or are not read back (on the Pi 5:
RP1's function, `SYS_RIO`, or its pads); `pull-up` or `pull-down` alone
on one pin: something is wired to it, or that pad's pull does not work;
`kept`: the pin was not put back as it was. On a machine without GPIO it
runs on the simulated pins, and passes the same way.

## The Pins window (the desktop)

*Pins* (the Deskbar, or the menu) shows the pins as the **tracks of a
sequencer**, the way music programs lay out theirs:

- **A track is a pin**, an **output** or an **input** (`OUT` / `IN` on
  its header; an input's pull: `up`, `dn` or `none`, a click changes
  it); `<` `>` choose another pin; `x` removes the track, `+ Track` adds
  one (up to 8).
- **An output's row is its pattern**: a click on a step turns it on or
  off; `clk`, `inv`, `clr` fill it with a clock, its opposite, nothing.
- **An input's row is its recording**: while the sequence plays, the
  server samples every pin at the end of each step, and the row shows the
  input's level at each step (high: a filled block; low: a low bar),
  marked `rec`; it stays after the sequence stops, until the next play.
- **The transport:** Play / Stop (or Space), Loop, the step's length (1
  ms to 1 s), the number of steps (8 to 64). A change while it plays is
  sent at once. The playhead lights the column of the step being played (and its number in the ruler).
- **The square** on each header is the pin's level now; a click on an
  output's sets it at once.
- The tracks are kept in `~/.pins`. On a machine without GPIO it runs on
  the simulated pins (an input then records its pull).

## The server

- **Pins:** BCM numbers 0 to 27. A pin a device already uses (an
  alternate function: the serial console's 14 and 15, I2C, SPI, the GFX
  HAT's) shows as `busy` and is refused.
- **Patterns** are played by the server's own thread, with deadlines
  counted from the start, so the timing does not drift whatever the
  client does; a new pattern replaces the one playing. The pins it drives
  become outputs. At the end of each step (50 us before the next one is
  set) it samples every pin: the recording (`GPIO_RECORD`).
- **The hardware:** the BCM2711 (Pi 4) or BCM283x (Pi 2, 3) GPIO block,
  found in the device tree; the pulls are set the way each chip wants.
- **The Pi 5:** its header's pins are bank 0 of the GPIO in its **RP1**
  chip, which the BCM2712 reaches through PCIe (the device tree's
  `raspberrypi,rp1-gpio`: RP1's `IO_BANK0` at `0x400d0000`, `SYS_RIO0` at
  `0x400e0000`, `PADS_BANK0` at `0x400f0000`, seen through the firmware's
  PCIe window at `0x1f_000d_0000`...; mapped as device memory). A pin is
  made software's with the function `SYS_RIO` (5) in its `CTRL`, its pad's
  input on and output allowed (and its pull); its direction and level are
  set through `SYS_RIO0`'s atomic `SET` / `CLR` aliases, and every level is
  read at once from its input register (offset `0x08`, the one Linux
  reads). A pin with no function (`0x1f`, as at reset, or its output
  disabled by `CTRL`'s override, as Linux leaves a pin it freed) shows as
  an input; any other function: `busy`. The firmware
  must leave the PCIe link up: `pciex4_reset=0` in `config.txt` (the card
  `make sdcard` writes has it); gpiod checks the link first, and with the
  link down says so on the console (`gpiod: RP1: the PCIe link is down`)
  and simulates the pins. Written from RP1's datasheet and Linux's
  `pinctrl-rp1.c`; QEMU has no RP1: check it with `gpio selftest`.
- **Started on demand** by init (the first program that uses `gpio`), and
  kept running (a pattern may be playing). Anyone logged in may use it.
- **Protocol:** `GPIO_*` in `include/mk/proto.h`.

## Tests

`make gpio-test` (x86-64) and `make ARCH=arm64 gpio-test` (QEMU's virt,
programs built by sicc), with simulated pins: `/bin/gpio` (list, out, get,
in with a pull, the console's pin refused, blink and stop, `gpio
selftest`: 22 pins pass, 6 skipped, every pin listed the same after), then
`tests/gpio/pins.c` built on SIEOS with `cc` and run: outputs read back, a
pull, a device's pin refused, no pin 40, a pattern that loops and stops,
the levels recorded at each step, no self-check while it plays, then the
self-check passing and every pin put back as it was. `make gui-test` drives the Pins window:
a step set, a track made an input, played, the inputs recorded, stopped.
On a real Pi the same program drives the real pins.
