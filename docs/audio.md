# Sound

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

Playing and recording on Intel High Definition Audio: QEMU's `intel-hda`
and `ich9-intel-hda` with their codecs, and PC chipsets up to Raptor Lake
(the target laptop's controller, PCI 00:1f.3) with a Realtek-style codec.
One output and one input stream, 48 kHz, 16-bit, stereo: no mixing of
several programs, no other rates (convert files beforehand).

```
   play x.wav ──AUDIO_WRITE (the frames)──► audiod ──► the output ring (DMA) ──► codec ──► speaker / headphones
                 the kernel copies them straight into the ring: rbuf is its free room
   rec y.wav  ◄──AUDIO_READ (the frames)─── audiod ◄── the input ring (DMA) ◄── codec ◄── microphone / line in
                 the reply's sbuf is the ring itself
```

## Commands

| | |
|---|---|
| `play FILE.wav` | plays a WAV file (48 kHz, 16-bit, stereo PCM; others are refused) |
| `rec FILE.wav [SECONDS]` | records that long (default 5 s) into a WAV file |
| `vol [0-100\|mute]` | shows or sets the volume; a number also unmutes |

They are one program (`user/play/play.c`, 2 KB of code) under three names.
Ctrl-C stops `play` and `rec`. Settings has the volume too (Sound: Mute,
25-100%).

## The server: `/bin/audiod`

`user/audiod/audiod.c`, port `"audio"` (protocol in `include/mk/proto.h`:
`AUDIO_WRITE`, `AUDIO_READ`, `AUDIO_VOL`). init starts it when a program
first looks the port up, and keeps it (the volume lives there; it is not
saved across a reboot). With no controller it ends with status 2 and init
gives up on it: clients get "no sound card".

- **Controller:** found by PCI class (HDA 0403, or Intel's "audio device"
  0401: the DSP, SST/SOF, is on; the HDA registers still work, but a
  digital microphone wired to the DSP cannot be recorded; the log says
  which). Intel: TCSEL (traffic class 0), and on PCHs snooped DMA and no
  clock gating while the link resets, as Linux does (`hda_intel.c`); the
  link and codecs reset; commands through CORB/RIRB, polled.
- **Codec:** the first one with an analog output (the iGPU's HDMI codec
  has only digital pins). Its widgets are walked: from the speaker (else
  line-out) pin and the headphone pin down to a DAC, and from an ADC to a
  microphone or line-in pin, through mixers and selectors. On the way each
  connection is chosen, each amplifier opened at 0 dB (a mixer's other
  inputs muted), widgets powered, pins enabled (headphone drive, 80% bias
  for a microphone, EAPD on). The volume is the DACs' output amplifier
  (0 dB at 100). When a stream starts, a headphone plugged in silences
  the speaker (no interrupt for plugging in while playing).
- **Streams:** a 128 KiB ring each (683 ms), 8 periods of 16 KiB, an
  interrupt at each period's end. Playing zeroes what was played, so it
  plays silence if writes stop; it stops one period after the last data
  (what the codec still buffers). Recording runs while someone reads and
  stops when the ring is nearly full (nobody reads). A write that finds
  no room waits for the next period.

Sizes (x86-64, gcc -O2): audiod.o 6.4 KB of code (the program 20 KB with
the library), play.o 1.7 KB; Settings grew by 0.5 KB. RAM: 260 KiB of DMA
memory (two rings, one page for CORB, RIRB and the buffer lists).

## Tests

`make audio-test` (`tools/audiotest.py`; `Q35=1`: the q35 machine and its
ich9-intel-hda): QEMU's `hda-duplex` writing to a WAV file (`-audiodev
wav`): `vol`, a 1 s 1 kHz tone played and found unbroken, sample for
sample, in QEMU's file; `vol mute` and a silent second play; then, in a
second boot (QEMU's `wav` backend has no input), `rec` for 2 s from the
`none` backend: a WAV file of exactly 2 s. Also passed with `hda-micro`.

## Not done, or not checked on the real laptop

- Not run on Raptor Lake yet: the controller, the PCH bits and the
  codec walk on a real Realtek codec (QEMU's codecs have no mixers). If it
  fails, the log lines `audiod: HDA ...` and `audiod: codec ...` say what
  was found.
- Laptops whose speakers sit behind a "smart amplifier" on I2C or SPI
  (Cirrus CS35L41, TI TAS2781: common on gaming laptops) need that chip's
  driver and firmware: the headphone jack still works.
- Realtek codecs' vendor-specific fix-ups (`patch_realtek.c` coefficients,
  headset-mode switching) are not done: a headset's microphone may need
  them. No Link Control (24 MHz) fix-up for firmware that left 6 MHz.
- No jack interrupts, no microphone choice (the first one wired), no
  record level, no HDMI/DisplayPort sound (it needs the display driver's
  power well).
