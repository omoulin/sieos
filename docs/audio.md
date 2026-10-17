# Sound

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

Playing and recording on Intel High Definition Audio: QEMU's `intel-hda`
and `ich9-intel-hda` with their codecs, and PC chipsets up to Raptor Lake
(the target laptop's controller, PCI 00:1f.3; the Surface Pro 7's,
8086:34c8, in HDA mode: [surface-pro-7.md](surface-pro-7.md#sound)) with a
Realtek-style codec.
One output and one input stream, 48 kHz, 16-bit, stereo: no mixing of
several programs, no other rates (convert files beforehand). The output
goes to the speakers (analog) by default, or to an HDMI or DisplayPort
monitor with sound: one lit by the Intel display (igfx.c), or one the
firmware lit on the NVIDIA GPU (see [HDMI and DisplayPort](#hdmi-and-displayport)),
or to Bluetooth headphones (see [Bluetooth](#bluetooth)).

```
   play x.wav ──AUDIO_WRITE (the frames)──► audiod ──► the output ring (DMA) ──► codec ──► speaker / headphones
                 the kernel copies them straight into the ring: rbuf is its free room
   rec y.wav  ◄──AUDIO_READ (the frames)─── audiod ◄── the input ring (DMA) ◄── codec ◄── microphone / line in
                 the reply's sbuf is the ring itself
```

## Commands

| | |
|---|---|
| `play FILE.wav...` | plays WAV files (48 kHz, 16-bit, stereo PCM; others are refused), one after the other; media keys pause it, skip, stop it |
| `rec FILE.wav [SECONDS]` | records that long (default 5 s) into a WAV file |
| `vol [0-100\|mute]` | shows or sets the volume; a number also unmutes |
| `vol out [N]` | lists the outputs (0 the speakers, then each monitor with sound, then connected Bluetooth headphones, by name; `*` the one playing), or plays to output N |

They are one program (`user/play/play.c`, 2.1 KB of code) under three names.
Ctrl-C stops `play` and `rec`. Settings has the volume too (Sound: Mute,
25-100%), and, when a monitor with sound is there, the output (Sound:
Output: Speakers / the monitor's name). The choice lasts until audiod
ends (a reboot); a monitor that goes away sends the sound back to the
speakers the next time the outputs are asked.

## The server: `/bin/audiod`

`user/audiod/audiod.c` (controllers, streams, the port) and `codec.c`
(the codecs, through verbs only), port `"audio"` (protocol in
`include/mk/proto.h`: `AUDIO_WRITE`, `AUDIO_READ`, `AUDIO_VOL`, `AUDIO_OUT`, `AUDIO_KEY`). init starts it when a program
first looks the port up, and keeps it (the volume lives there; it is not
saved across a reboot). With no controller it ends with status 2 and init
gives up on it: clients get "no sound card".

- **Controller:** found by PCI class (HDA 0403, or Intel's "audio device"
  0401: the DSP, SST/SOF, is on; the HDA registers still work, but a
  digital microphone wired to the DSP cannot be recorded; the log says
  which). Intel: TCSEL (traffic class 0), and on PCHs snooped DMA and no
  clock gating while the link resets, as Linux does (`hda_intel.c`); the
  link and codecs reset; the DSP's processing pipe off if the firmware left
  it on, a multi-link controller's link moved from 6 to 24 MHz (Ice Lake
  on, `intel_init_lctl`); commands through CORB/RIRB, polled.
- **Controllers:** up to two: the chipset's, and an NVIDIA GPU's HDMI
  function (class 0403 too; snooped DMA set as Linux's `AZX_SNOOP_TYPE_NVIDIA`).
  Each has its own CORB/RIRB page and buffer lists (the rings are shared);
  every codec on each is walked. The input stream is on the analog codec's
  controller, the output one moves to the chosen output's.
- **Codec:** the first one with an analog output (the iGPU's HDMI codec
  has only digital pins). Its widgets are walked: from the speaker (else
  line-out) pin and the headphone pin down to a DAC, and from an ADC to a
  microphone or line-in pin, through mixers and selectors. On the way each
  connection is chosen, each amplifier opened at 0 dB (a mixer's other
  inputs muted), widgets powered, pins enabled (headphone drive, 80% bias
  for a microphone, EAPD on). The volume is the DACs' output amplifier
  (0 dB at 100). When a stream starts, a headphone plugged in silences
  the speaker (no interrupt for plugging in while playing). When
  recording starts with headphones in, a jack's microphone (a headset's)
  is preferred, on whichever ADC reaches it; else the first microphone.
  Realtek's ALC225 family (ALC215/225/285/289/295/299, Dell's ALC3271):
  its combo jack's mode set through coefficients (unplugged, or a CTIA or
  OMTP headset, measured), and on a Dell the headset microphone's pin made
  one. Realtek's ALC294 family (the Surface Pro 7's ALC274): Linux's
  `alc294_init` (EAPD coefficients, the headphone depop), then the
  generic walk ([surface-pro-7.md](surface-pro-7.md#sound)). Volume keys
  (a USB keyboard's consumer keys, a Surface's buttons) come as
  `AUDIO_KEY`.
- **Streams:** a 128 KiB ring each (683 ms), 8 periods of 16 KiB, an
  interrupt at each period's end. Playing zeroes what was played, so it
  plays silence if writes stop; it stops one period after the last data
  (what the codec still buffers). Recording runs while someone reads and
  stops when the ring is nearly full (nobody reads). A write that finds
  no room waits for the next period.
- **Media keys** (`AUDIO_KEY`, `MK_...`: the AV/C operation ids, so btd
  passes a headset's buttons on unchanged; [bluetooth.md](bluetooth.md#remote-control-avrcp)).
  Pause: the writer's next `AUDIO_WRITE` is held (nothing taken; the
  stream drains and stops, btd suspends its own), play lets it go. Stop,
  next, previous: the writer's next write is answered with nothing taken
  and the key in `w[1]`; `play` then ends, goes to the next file, or the
  one before (or the same again). Volume up/down: 5 points. The keys are
  for the process writing then: a write from another one forgets them (a
  new `play` is not paused by an old key).

Sizes (x86-64, gcc -O2): audiod.o 10.5 KB of code with HDMI/DP and two
controllers (6.4 KB before them; the program 24 KB with the library),
play.o 2.1 KB; Settings 0.5 KB for the volume, the output row added
nothing measurable. RAM: 260 KiB of DMA memory (two rings, one page per
controller for CORB, RIRB and the buffer lists).

## HDMI and DisplayPort

```
  igfx (Atlas), at its start if the VBT has monitor ports:
     display audio on (AUD_FREQ_CNTRL, AUD_PIN_BUF_CTL, the codec wake signal)
  then when it lights a monitor whose EDID says "basic audio" (at the start or plugged in later):
     HDMI: HDMI mode (not DVI) and an AVI infoframe
     the transcoder's audio on, N/CTS by the hardware, the ELD (built from the EDID) ──┐
                                                                                      │ (inside the chip)
  audiod: the HDMI codec's pin device for that pipe says "ELD valid" ◄────────────────┘
     AUDIO_OUT N: that pin's converter gets the output stream (2 channels), the pin's output, an audio infoframe
```

No message between Atlas and audiod: each does its half, and the ELD in
the codec is how audiod learns that a monitor with sound is there.

- **The display side** (`user/atlas/igfx.c`, Linux's `intel_audio.c`
  `hsw_audio_codec_enable`, `i915_audio_component_get_power`,
  `codec_wake_override`): when a monitor (not the panel) has a CEA block
  with basic audio, `user/lib/eld.h` builds its ELD (CEA revision, name,
  short audio descriptors, speaker allocation, manufacturer and product;
  DP or HDMI). Before the outputs, whenever the VBT has a monitor port (so
  that the codec is there for a monitor plugged in later): `AUD_FREQ_CNTRL` 0x8010 (8T, 96 MHz
  BCLK, i915's value for display 12), `AUD_PIN_BUF_CTL` on, and the codec
  wake signal (`AUD_CHICKENBIT` bit 15) pulsed and left on, so that
  audiod's link reset finds the HDMI codec; the power wells (PW2-PW4) are
  on already. An HDMI monitor with sound runs in HDMI mode instead of DVI,
  with an AVI infoframe (RGB, no VIC: full range). After its pipe starts:
  `AUD_PIN_ELD_CP_VLD` audio enable for the transcoder, `AUD_CFG` (DP: N
  by the hardware; HDMI: the pixel clock's class, 25.2 MHz if not one of
  i915's table), the ELD into `AUD_EDID_DATA`, ELD valid; unplugged: audio
  off, ELD not valid (with its pipe). Logged in
  `/etc/igfx.log` (`sound: ELD ...`, `display audio on`, `pipe B: sound on`).
- **The codec side** (`user/audiod/codec.c`, Linux's `patch_hdmi.c`): the
  digital pins with HDMI or DP capability and a connection (the pin
  default), on every codec of every controller. Intel display codecs first
  get the vendor verb 0x781 (on widget 2, Tiger Lake on): all pins and
  converters, and DP 1.2 mode, where each pin has a device per display
  pipe (the device number is the pipe: igfx's monitors are on B and C).
  The sinks: each pin's devices 0-3 (as many as it accepts: the device
  select read back), pin sense "ELD valid", the ELD's name read through
  the ELD verb (0xF2F). Playing to one: the device selected, its first
  converter, digital on, 2 channels, the stream's format and tag, the
  pin's output on and unmuted, channels 0 and 1 to slots 0 and 1, and an
  audio infoframe (HDMI's, or DP's SDP form: 2 channels, front left and
  right) sent at every frame; the analog DACs get no stream. The volume
  of a monitor is its own (no amplifier there); mute stops its converter.
  Logged: `audiod: codec 2 (8086:2818): 9 HDMI/DP pins, ...` and
  `audiod: HDMI/DP sink 1: codec 2 pin 6 device 1 converter 3: NAME (HDMI)`.
- **NVIDIA** (01:00.1 on the laptop, codec 10de:00ad: pins 5 and 6 wired,
  4 devices each, 4 converters): the same code. audiod first sets bit 25
  of register 0x488 of the GPU's function 0 if clear (Linux's
  `quirk_nvidia_hda`: the firmware may hide the HDA function; the register
  is in the extended configuration space, reached through the chipset's
  PCIEXBAR). Limitation: the NVIDIA codec has a sink only when the NVIDIA
  GPU drives a monitor, and SIEOS does not drive that GPU: only if the
  firmware lit a screen on it (the BIOS in discrete-GPU mode, or a monitor
  on a port wired to the dGPU at boot) and wrote the ELD. A function the
  firmware hid has no address (no BAR): then it stays unused.

## Bluetooth

Headphones or speakers connected by Bluetooth ([bluetooth.md](bluetooth.md#headphones-and-speakers-a2dp))
are the last output, by their name, while connected. btd asks audiod to
play to them when they connect (`AUDIO_OUT` with `AUDIO_BT`); audiod also
picks them at start if they are there. Then the output stream of the
controller stops and each `AUDIO_WRITE` goes on to btd's port `btaudio`
(root only), with the volume in `w[1]`: the kernel copies the frames
from where they landed in audiod's ring into btd's, the one copy more
than for the speakers. btd encodes them (SBC), paces them by the media
clock and holds the answer while its 170 ms ring is full, which paces
the client. The volume is applied in the encoder, or, when the headset
has absolute volume (AVRCP), by the headset: audiod sends each change on
(`AUDIO_VOL` to `btaudio`), and the headset's own changes come back as
`AUDIO_VOL` from btd. If btd says the
headphones are gone, audiod goes back to the speakers and the client's
write is answered 0 (sent again, to the speakers). With no HDA codec but
a Bluetooth adapter (the Pis), audiod now stays up for headphones only
(writes get -ENOENT until some connect). Sizes: audiod +1.0 KB; media
keys and the volume to btd +0.6 KB, play +0.3 KB.

## Tests

`make audio-test` (`tools/audiotest.py`; `Q35=1`: the q35 machine and its
ich9-intel-hda): QEMU's `hda-duplex` writing to a WAV file (`-audiodev
wav`): `vol`, a 1 s 1 kHz tone played and found unbroken, sample for
sample, in QEMU's file; `vol mute` and a silent second play; then, in a
second boot (QEMU's `wav` backend has no input), `rec` for 2 s from the
`none` backend: a WAV file of exactly 2 s. Also passed with `hda-micro`.

`make hdmi-test` (part of `audio-test`; `tests/hdmi/test.c`, on the
host): QEMU has no HDMI codec, so `codec.c` runs against the laptop's
own codecs as Linux printed them (`/proc/asound/card0/codec#0`, the
ALC257; `card0/codec#2`, the Raptor Lake HDMI codec; `card1/codec#0`,
the RTX 5060's), answered verb by verb by a fake that also hides the
Intel codec's converters and devices until the vendor verb. Checked: the
analog walk on the real ALC257 (speaker pin 0x14 and headphone pin 0x21
to DAC 2, the internal microphone 0x12 to ADC 7), the vendor verb, the 9
Intel pins and the 2 wired NVIDIA ones, the ELDs built from two real
monitors' EDIDs (Samsung C24F390 on HDMI, Dell U2718Q on DP, from the
linuxhw EDID collection: every byte for the first) and their names read
back through the pins, the verbs that send the stream to each (device,
converter, format and tag, pin, infoframe and its checksum), mute, a
monitor unplugged.

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
  record level.
- HDMI/DP sound is not tried on real hardware: it needs a monitor with
  speakers on the laptop's HDMI or a DP port driven by the Intel GPU
  (`vol out` lists it by name, `vol out 1` plays to it). If it does not
  show: `/etc/igfx.log` should have `sound: ELD` for the monitor and
  `display audio on`, the console `audiod: codec 2 (8086:2818): 9 HDMI/DP
  pins ... 4 converters each` (0 converters: the vendor verb did not take)
  and an `HDMI/DP sink` line. Silence while listed: DP M/N or HDMI N/CTS
  (the hardware's), the infoframe, or the codec wake left on are the
  suspects. Only 48 kHz stereo PCM (no surround, no passthrough). A
  monitor plugged in later is lit by igfx with its ELD (hot-plug:
  [desktop.md](desktop.md#how-it-works)): `vol out` and Settings list the
  sinks again each time they are asked; when a stream starts on a monitor
  that is gone, the sound goes back to the speakers. No software
  volume for monitors. Intel display versions other than 12 are not
  driven by igfx, so get no display-side set-up.
