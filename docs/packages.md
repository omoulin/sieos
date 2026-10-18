# Add-on packages

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

The base system is what every disk, USB key and SD card starts with: the
kernel, the servers, the desktop, the shell and the everyday programs
(`cc`, `make`, `git`, `ssh`, `mail`, `sia`...), 3.6 MiB of files. What is
big, or only some people want, is an **add-on package** instead: one file,
installed when wanted, removed exactly.

| Package | What it installs | Package file (x86-64) | Installed |
|---|---|---|---|
| `web` | the browser: `/bin/web` (WPE WebKit), WebKit's process names linked to it, the DejaVu fonts and their configuration, the MIME types, the toolbar and start page, Web.app | 33.2 MiB | 88 MiB |
| `python` | `/bin/python` (MicroPython), Python.app, the examples in `/src/python` | 184 KiB | 385 KiB |
| `scriptures` | `/bin/scriptures`, its fonts (DejaVu Sans Mono and bold), Scriptures.app | 542 KiB | 941 KiB |
| `model` | the assistant's model in `/models` (the PC's SmolLM2-1.7B; on arm64 the Pis' SmolLM2-360M) | 1 GB (386 MB) | the same |

A desktop entry (`/etc/apps/*.app`) comes with its package: Web.app is in
the menu only when `web` is installed (from the next login). A command of
a package that is not installed says which package it is in
(`/etc/pkg.cmds`):

    $ python
    python: not installed: it comes with the add-on package python (pkg install FILE|URL)

and `sia` without a model names the `model` package.

## Making and installing them

```sh
make pkg-python                    # build/pkg/python.pkg (also pkg-web, pkg-scriptures, pkg-model)
make usb PKGS="web python"         # a USB key with those packages installed (also sdcard, newdisk)
make PKGS=web pkg-disk             # onto the existing build/disk.img (never while QEMU runs on it)
```

`make all` builds the `python` and `scriptures` packages (they are small);
`web` needs WebKit built first (`make webkit`, [webkit-plan.md](webkit-plan.md)),
`model` the model file in `models/` ([llm.md](llm.md); `PKGMODEL=` another).
The old switches still work: `WEB=1` is `PKGS=web`, `USBMODEL=1` and
`SDMODEL=1` are `PKGS=model`.

On SIEOS, root uses `/bin/pkg`:

```sh
pkg install /tmp/python.pkg              # from a file
pkg install https://host/pkg/web.pkg     # or downloaded (http:// or https://, user/lib/http.c)
pkg list                                 # name, version, size of each installed package
pkg remove web                           # exactly what it installed
```

`pkg install` reads the package as it arrives (a download is not stored
first), checks each file's SHA-256 against the manifest, and if anything
is wrong (a damaged or cut-short package, a file it cannot write) removes
what it had written: nothing is left half-installed. It refuses to replace
a file of the base system. Installing a package already there replaces the
installed version.

## The format

A package (`tools/pkg.py` makes it, shows it and installs it into a SieFS
image on the host; `user/pkg/pkg.c` reads it on SIEOS) is a text manifest,
then the files' data:

    SIEOS-PKG 1
    name python
    version 20261018
    size 394106
    dir /src/python
    file 755 392560 d47f5454...a802 /bin/python
    file 644 182 366c4a72...a774 /etc/apps/python.app
    link /bin/web /usr/libexec/wpe-webkit-2.0/WPEWebProcess
    end

`size` is the bytes it installs; `dir` lines are made if missing; each
`file` gives its mode, size, SHA-256 and path; `link` a symbolic link.
Then each file's data in the manifest's order, in pieces of 1 MiB (the
last one shorter), each with a 4-byte little-endian header: its stored
length, bit 31 set when stored as is, else a zlib stream of the piece
(pieces keep the memory small on both sides; a file whose first pieces do
not shrink, a model, is stored as is).

What a package installed is recorded in `/etc/pkg/NAME`: its manifest,
with only the `dir` lines of the directories it made. `pkg remove` undoes
the record, last line first: its files and links, then those directories
if they are empty. A file two packages carry (the fonts `web` and
`scriptures` share) stays until the last of them goes.

## Sizes

`make budget` ([budgets.md](budgets.md)) checks the package files too.
Base images before and after the packages were split out (real data, the
images are sparse): the USB key 14.3 MiB -> 13.1 MiB, the Pi 4 card
10.5 MiB -> 9.5 MiB; the base system's files 4.8 MiB -> 3.6 MiB. A key
made with `WEB=1` used to carry the browser in its base; it is now the
`web` package's 33 MiB, only on the keys that ask for it.
