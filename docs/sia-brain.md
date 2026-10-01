# sia-brain: the local model

**sia-brain** is a language model that runs on the computer itself, on its processor,
without a network or an account. sia uses it like any other model: in the strip, the
terminals and MiR.

It is built on Mistral AI's open model **Ministral 3 3B Instruct (2512)**, released under
the Apache License 2.0, and runs on [llama.cpp](https://github.com/ggml-org/llama.cpp)
(MIT licence). `/usr/pkg/share/sia-brain` holds the licence (`LICENSE`) and the notice
(`NOTICE`) that say so. Mistral AI does not endorse SIEOS, and "sia-brain" is SIEOS's own
name for this build.

## Getting it

- **The USB image with the model:** `sieos-usb-brain.img` (2.8 GB) has it installed, with
  every other package (see below). `sieos-usb.img` (710 MB) has every package but
  sia-brain and llama-cpp.
- **On an installed system:** `pkg install sia-brain` (with SiPM or in a terminal). It
  pulls in `llama-cpp`. The download is 2 GB.

Once it is installed:
- **Settings > Assistant** lists it as **sia-brain (local)**, beside the models
  registered there; *Use this one* makes sia use it.
- **With no model registered**, sia uses it by default.

## Using it

- **Starting:** sia starts the engine the first time it needs it (`sia-brain start`). The
  model loads in a few seconds from a disk, longer from a slow USB drive. It then
  answers on `127.0.0.1:8095`, for this user only.
- **Commands:** `sia-brain start`, `sia-brain stop`, `sia-brain status`. The log is
  `~/.sia/brain.log`.
- **Memory:** the model takes 2 GB and its context (16,384 tokens) about 1 GB more. A
  computer needs about 4 GB of free memory; 8 GB of RAM in all is comfortable.
- **Speed:** it depends on the processor. llama.cpp picks the fastest code the processor
  has: SSE4.2, AVX, AVX2 or AVX-512, which SIEOS saves for programs. In QEMU with 4
  virtual CPUs (AVX2) it reads about 95 tokens a second and writes about 20.
  - The first answer of a conversation takes the longest: sia's instructions and tools,
    about 700 tokens, are read first (7 s there).
  - After that, the engine keeps what it has read. Each turn only reads what is new.
- **Compact tools:** a local model gets fewer tools than a cloud model, to keep its prompt
  short:
  - `sh` runs any command line, and the instructions name the programs available;
  - `cd` and `write_file`;
  - the desktop's or MiR's tools.

  Commands that change something still need the user's OK, as with any model.
- **No images:** the vision part of Ministral 3 is not included, so MiR tells the user to
  check windows themselves (see [mir.md](mir.md)).

## The USB image with the model

Both USB images end with a partition of packages: ext4, labelled `sieos-pkg`, holding
`/usr/pkg` with the packages installed and their database in `/usr/pkg/.pkgdb`. In
`sieos-usb-brain.img` it has every package; in `sieos-usb.img`, every package but
`sia-brain` and `llama-cpp`.

- **On a live system** (started from the drive, its root in memory), `/etc/rc` mounts that
  partition on `/usr/pkg` (`mount -L sieos-pkg /usr/pkg`) and links `/var/lib/pkg` to its
  database. It reads it through the USB storage driver (xHCI, bulk-only mass storage).
  The images' root has `/etc/sieos-usb-pkg`, so `rc` waits up to 10 seconds for a drive
  that connects late. If the partition never shows, the boot messages say "the USB
  drive's package partition (sieos-pkg) was not found", sia asks for a model as on
  `sieos-usb.img`, and `dmesg | grep -i 'usb\|disk'` tells what the driver saw.
  Packages installed later go to the drive too and stay there; the rest of the live
  system is lost at power-off, as with `sieos-usb.img`. The partition has 256 MB free for
  them (`PKG_FREE_MB`).
- **The installer** (`sieinstall`, *Install SIEOS*) copies these packages to the disk,
  with their database in the installed system's `/var/lib/pkg`.

In QEMU it runs as a USB drive, an IDE disk or a virtio disk, for example:

```sh
qemu-system-x86_64 -accel kvm -cpu host -m 8G -smp 4 -nic user,model=e1000 \
    -drive file=build/sieos-usb-brain.img,format=raw,if=virtio,readonly=on
```

With `readonly=on` the package partition is mounted read-only: sia-brain works, but
packages cannot be added or removed.

Write it to a drive of 4 GB or more:

```sh
sudo dd if=build/sieos-usb-brain.img of=/dev/sdX bs=4M conv=fsync status=progress
```

## How it is made

`make brain` makes `build/brain/sia-brain.gguf`:
1. It downloads Mistral's own BF16 GGUF of the model (6.9 GB). Its SHA-256 is in
   `ports/SHA256SUMS`.
2. It builds llama.cpp for the build host (`cmake` is needed).
3. It quantizes the model to **IQ4_XS** (1.96 GB) with an importance matrix:
   - the matrix is `ports/sia-brain/imatrix.gguf`, computed from SIEOS's own
     documentation (`make brain-imatrix` computes it again);
   - perplexity on that text is 13.44, against 13.05 for the BF16 original.

The packages are built from recipes, like the others:
- **`ports/pkgs/llama-cpp`** cross-builds llama.cpp 0.5.0 (`sieos.patch`):
  - `llama-server` and `llama-cli`;
  - a CPU back end for each instruction set, loaded at run time.
- **`ports/pkgs/sia-brain`** packs `user/sia-brain` (the `sia-brain` script, `LICENSE`,
  `NOTICE`) with the model.

`make usb` and `make usb-brain` install the packages into the partition on the build host
(`tools/pkgstage.py`, as `pkg` would) and appends it to the USB image
(`tools/mkiso.sh`).
