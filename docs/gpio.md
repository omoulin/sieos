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
  input's level at each step (high: a hollow block; low: a low bar),
  marked `rec`; it stays after the sequence stops, until the next play.
- **The transport:** Play / Stop (or Space), Loop, the step's length (1
  ms to 1 s), the number of steps (8 to 64). A change while it plays is
  sent at once. The playhead shows the step being played.
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
  The Pi 5's pins are behind its RP1 chip: simulated there for now.
- **Started on demand** by init (the first program that uses `gpio`), and
  kept running (a pattern may be playing). Anyone logged in may use it.
- **Protocol:** `GPIO_*` in `include/mk/proto.h`.

## Tests

`make gpio-test` (x86-64) and `make ARCH=arm64 gpio-test` (QEMU's virt,
programs built by sicc), with simulated pins: `/bin/gpio` (list, out, get,
in with a pull, the console's pin refused, blink and stop), then
`tests/gpio/pins.c` built on SIEOS with `cc` and run: outputs read back, a
pull, a device's pin refused, no pin 40, a pattern that loops and stops,
the levels recorded at each step. `make gui-test` drives the Pins window:
a step set, a track made an input, played, the inputs recorded, stopped.
On a real Pi the same program drives the real pins.
