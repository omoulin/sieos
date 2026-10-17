# Installing SIEOS on a PC's drive

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

Started from the USB key ([usb.md](usb.md)), SIEOS can put itself on the
PC's own NVMe drive, **beside** the systems already there: into free
space you have made for it, or onto a whole drive you give it. The
firmware's boot menu (F12 on the Lenovo) then offers "SIEOS"; your usual
system stays the default.

**Read this first.** Your drives hold your only copy of your Linux system
and data. The installer never moves, resizes or touches an existing
partition, and it checks every write against the plan before making it;
but making free space (below) is done by you, from Linux, and a mistake
there loses data. **Back up first** (to a disk you then unplug).

```
  NVMe drive (GPT)                                   after the install
  ├── 1  EFI System Partition (FAT32)                \EFI\SIEOS\  added: SIEOSX64.EFI (the loader),
  │       \EFI\debian\ ...   (untouched)                KERNEL.BIN, *.ELF, BOOT.CFG (root=GUID)
  ├── 2  /boot              (untouched)
  ├── 3  LUKS (LVM: root, swap)  (untouched)
  └── free space         ──────────────────────────► 4  SIEOS (SieFS), found by its own GUID
```

## The two ways

- **Free space** (at least 4 GiB, in whole MiB): the installer adds one
  entry to the drive's GPT (both copies, CRCs recomputed) and fills the new
  partition; every other entry, and every sector outside the new
  partition, the GPT's own sectors and the EFI partition, stays as it was.
  The drive must have an EFI System Partition (FAT32): the boot files go
  into `\EFI\SIEOS\`, beside the other systems' files, which are not
  touched.
- **A whole drive**: everything on it is erased; a new GPT with an EFI
  partition (FAT32, 300 MiB) and SIEOS's partition (the rest). You must
  type the drive's model name, exactly, to confirm.

Refused: the drive SIEOS itself runs from (and every NVMe drive while
SIEOS runs from one: install from the USB key), a drive whose GPT is not
valid in every way (both headers and entry arrays, their CRCs, the backup
at the drive's end, a purely protective MBR), an MBR-only or hybrid-MBR
drive (for free space; erasing it whole is allowed), a GPT with no free
entry, free space under 4 GiB, an EFI partition that is FAT12/16 or whose
two FATs differ (run `fsck.vfat` from Linux first).

## Install

1. Start the PC from the USB key and log in. The desktop says so when it
   can install ("Started from the USB key: Install SIEOS...").
2. The menu (the cube) > **Install SIEOS**. Root's password first (unless
   root is logged in).
3. **Choose where**: each drive with its model, size, partitions and free
   extents; "Install here" on a free extent of 4 GiB or more, or "Erase
   whole drive" (then type its model).
4. **What will change**: the exact plan (the new partition's sectors and
   GUID, the GPT sectors written, the EFI partition's new folder, what is
   copied). Back, or Install.
5. **Installing**: the steps and a bar: the SieFS partition made and
   filled with a fresh system (the key's system as built, the firmware
   files included, no accounts), checked block by block; `\EFI\SIEOS\`
   written and read back; the GPT written (backup, then primary) and read
   back. About a second in QEMU for the 4 MiB system; on the laptop, the
   time to copy the system (with the assistant's model, 1 GB: the USB
   key's read speed).
6. **Start the PC from the USB key once more**: its loader adds "SIEOS" to
   the firmware's boot menu ("SIEOS loader: SIEOS added to the firmware's
   boot menu (Boot0003)"), at the end of the boot order. Then power off,
   remove the key, and choose SIEOS in the boot menu (F12). The first
   start creates the accounts, as on the key ([accounts.md](accounts.md)).

The same from a terminal, as root (what the window runs):

```
install list                  the drives, their partitions and free space
install -n free 0 2           the plan for free extent 2 of drive 0; nothing written
install free 0 2              the same, then "Type yes to install:"
install whole 1               erase drive 1: "Type the drive's model to erase it:"
```

## How it works

- **The engine**, `/bin/install` (`user/install/install.c`, `fat.c`), a
  program of its own: the drives through the disk server's raw access
  (`DISK_RAW_*` in `mk/proto.h`, root only; refused while SIEOS's own disk
  is an NVMe drive, since vblk would have to switch controllers under it),
  the GPT, FAT32, and the SieFS library itself. The **window**
  (`user/atlas/inst.c`) runs it as root on a console channel of the
  desktop and shows what it prints; it answers its question ("yes", or the
  model as typed in the window). So the disk work is tested without a
  screen (the first half of `make install-test`) and the window with one.
- **The write gate.** The plan declares its ranges (free space: the GPT's
  primary header and entries, the backup entries and header, the new
  partition, the EFI partition; a whole drive: the drive); every write is
  checked against them and anything else stops the program before it
  reaches the drive. In the EFI partition, `fat.c` checks finer: data only
  into clusters it found free (or SIEOS's own folder's), FAT entries only
  of those (or the end of a directory it grows), and in another
  directory's cluster exactly one 32-byte entry that was free.
- **Order** (free space): the SieFS partition first (free space: nothing
  refers to it), then the EFI partition's files, the GPT last. A power cut
  before the GPT leaves the drive's table as it was.
- **The system copied** is the key's snapshot "factory" (`/.snapshots/factory`,
  taken by `tools/mkusb.py` when the image is made): exactly the image,
  whatever was done on the key since (accounts, files). The boot files are
  in its `/boot`.
- **Which partition is SIEOS's.** `BOOT.CFG` has `root=GUID`, the new
  partition's unique GUID (random). The loader passes it to the kernel as
  a boot option (`sieos_boot_t` version 2), the kernel hands it to vblk
  (`SYS_INFO`), and vblk takes the SieFS partition with that GUID on
  whichever drive, never another one: the key keeps using its own
  partition even with SIEOS installed on the NVMe drive, and the drive's
  order does not matter. (Keys made before this have no `root=` line: vblk
  then takes the first SieFS partition, NVMe first. Make a new key.)
- **The boot entry: why at the next start, from the loader.** SIEOS's
  kernel runs after ExitBootServices and does not keep the firmware's
  runtime services mapped, so it cannot call SetVariable. Keeping them
  would mean mapping the firmware's runtime regions in the kernel and
  calling into it (the Microsoft calling convention) with interrupts and
  SMM variable writes in the way: kernel code for one call per install,
  and the place where firmware bugs show. Instead the installer leaves a
  request, `\EFI\SIEOS\ADDENTRY`, and the loader, which still has boot
  services, does what shim's `fallback.efi` does: at its next start
  (from the key, or from the EFI partition) it looks for the request on
  every FAT volume (connecting every device first when its own is the only
  one found: a firmware starting from USB may have connected nothing
  else), adds `Boot####` = "SIEOS",
  HD(partition, GPT, its GUID)/`\EFI\SIEOS\SIEOSX64.EFI` (the short form
  efibootmgr writes), appends it to `BootOrder` (never first), and deletes
  the request. An identical entry already there is not added twice.
- **Sizes** (x86-64, code): `/bin/install` 104 KB, of which the
  installer itself is 24 KB (install.o 17.6, fat.o 6.8) and the rest the
  SieFS library with its check, linked as the file server links it; the
  loader 10.5 -> 14 KB (the boot entry, `root=`, the second folder); vblk
  +1.7 KB (raw access, `root=`); the kernel +0.2 KB; atlas +8.6 KB (the
  window, inst.o 7.5 KB).

## Tests

`make install-test` (x86-64; OVMF with persistent variables, mtools):

1. The key (no screen) with four NVMe drives: **A**, a fake Debian drive
   (an EFI partition made by mtools with Debian's files under long names,
   `/boot`, a LUKS-like and a swap partition with random data, 3 MiB of
   unusable space, 6 GiB free); **B**, an MBR drive; **C**, 2 GiB free;
   **D**, full. `install list`; refused: too small (C, and A's 3 MiB), no
   free space (D), the MBR drive, the model typed wrong, an answer other
   than "yes"; a dry run; the install into A's free space. On the host:
   B, C, D unchanged (hashes); A's old GPT entries unchanged and one
   SieFS entry added where the free space was, both GPTs valid; every
   sector outside the new partition, the EFI partition and the GPT's own
   sectors unchanged (hashed: the three partitions with data, the gaps);
   the EFI partition's 9 other files byte-identical (read by mtools), the
   FAT consistent (both copies equal, no shared or lost clusters, FSInfo's
   free count right); `BOOT.CFG` names the new partition; `fsck.siefs` clean.
2. The key again: its loader adds Boot0002 "SIEOS"; the firmware's
   variable store read on the host: BootOrder has it **last**.
3. Drive A alone: the firmware starts "SIEOS"; vblk takes the partition by
   its GUID; the first start; a file written; the partition checked.
4. With a screen: the key, alice logged in on the desktop, the menu >
   Install SIEOS, root's password, "Erase whole drive" on B, its model
   typed, the summary, Install, done (screenshots in
   `build/install-test/`). B's new GPT, FAT and SieFS checked on the host.
5. The key once more (B's entry), then B alone: SIEOS starts from the EFI
   partition the installer formatted, with the first start on the screen.

Untested: the real firmwares (Lenovo's and the Ice Lake laptop's boot menus,
their handling of a boot entry added by a loader started from USB),
4 KiB-sector drives (the code handles them; QEMU's NVMe can do 4 KiB but
the test uses 512), an EFI partition with clusters over 64 KiB (refused).

## Making free space on the Lenovo's Debian drive

The Lenovo's first drive (nvme0n1) holds Debian: partition 1 the EFI
partition (976 MiB), 2 `/boot` (ext4), 3 LUKS, inside it LVM (a volume
group with `root`, often `swap_1`, sometimes `home`). The space at the end
of partition 3 is used; you make room by shrinking, from the inside out:
the file system, its logical volume, the physical volume, the LUKS
mapping, then the partition. Each layer keeps a margin above the one inside
it. The example frees about 64 GiB; change `FREE` for more.

**0. Back up** what you cannot lose, to a disk you then unplug, and the
LUKS header (from Debian, before anything):

```sh
sudo cryptsetup luksHeaderBackup /dev/nvme0n1p3 --header-backup-file /media/$USER/BACKUP/luks-header.img
```

**1. Start a Debian live USB** (same Debian release or newer), open a
terminal, `sudo -i`. The disk must not be mounted: nothing below works on
a running root file system.

**2. Look, and write the names down:**

```sh
lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT,MODEL
parted /dev/nvme0n1 unit s print            # partition 3: start and end sectors
cryptsetup open /dev/nvme0n1p3 cryptroot    # your LUKS passphrase
vgchange -ay
lvs; pvs; vgs                               # VG name, e.g. "debian-vg"; LVs root, swap_1
```

Below, `VG=debian-vg` and the LV to shrink is `root` (or `home` if you
have one and it holds the free room). `df` the file system after mounting
it read-only if you need to see how full it is: keep at least 10 GiB free
in it after shrinking.

```sh
VG=debian-vg; LV=root; FREE=64              # GiB to free
```

**3. The file system, offline, smaller than the target** (a 1 GiB margin),
then the LV, then the file system grown back to fill the LV exactly:

```sh
e2fsck -f /dev/$VG/$LV
SIZE=$(( $(blockdev --getsize64 /dev/$VG/$LV) / 1073741824 - FREE - 4 ))   # GiB: FREE + 3 for the layers' margins
resize2fs /dev/$VG/$LV $(( SIZE - 1 ))G     # the file system first, 1 GiB below ...
lvreduce -L ${SIZE}G $VG/$LV                # ... the LV (answer y)
resize2fs /dev/$VG/$LV                      # the file system fills the LV again
e2fsck -f /dev/$VG/$LV
```

(`lvreduce --resizefs -L ${SIZE}G $VG/$LV` does the first three in one
command, the same way.)

**4. The free extents must be at the end of the physical volume:**

```sh
pvs --segments -o pv_name,pvseg_start,pvseg_size,lv_name /dev/mapper/cryptroot
```

The last line must have no LV name (free) and a size (in extents of 4 MiB:
`vgs -o vg_extent_size`) of at least `FREE` GiB plus 2. If an LV comes
after the free extents (Debian puts `swap_1` after `root`), move it down
into the free ones (it takes a while; safe to run, but not to interrupt):

```sh
# example: swap_1 at extents 230000-232047, free from 213000: move those 2048 extents to 213000-215047
pvmove --alloc anywhere /dev/mapper/cryptroot:230000-232047 /dev/mapper/cryptroot:213000-215047
pvs --segments -o pv_name,pvseg_start,pvseg_size,lv_name /dev/mapper/cryptroot   # free now last
```

**5. The physical volume** (2 GiB of free extents or more stay in it):

```sh
PV=$(( $(pvs --noheadings --units b -o pv_size /dev/mapper/cryptroot | tr -dc 0-9) / 1073741824 - FREE - 2 ))
pvresize --setphysicalvolumesize ${PV}G /dev/mapper/cryptroot
pvs; vgs                                    # VFree: 2 GiB or a little more
```

**6. The LUKS mapping**, 1 GiB larger than the PV (sizes in 512-byte sectors):

```sh
CRYPT=$(( (PV + 1) * 2097152 ))
cryptsetup resize cryptroot --size $CRYPT
cryptsetup status cryptroot                 # "offset: N sectors" (the LUKS header, often 32768): note N
vgchange -an $VG
cryptsetup close cryptroot
```

**7. The partition**, 1 GiB larger than header + mapping:

```sh
OFFSET=32768                                # the "offset" from cryptsetup status
START=$(parted -m /dev/nvme0n1 unit s print | awk -F: '$1 == 3 { sub("s", "", $2); print $2 }')
END=$(( START + OFFSET + CRYPT + 2097152 - 1 ))
parted /dev/nvme0n1 unit s resizepart 3 ${END}s    # it warns that shrinking may lose data: Yes
parted /dev/nvme0n1 unit s print free       # the free space at the end: about FREE GiB
```

**8. Check that Debian is intact**, still from the live USB:

```sh
cryptsetup open /dev/nvme0n1p3 cryptroot && vgchange -ay
pvck /dev/mapper/cryptroot; vgck $VG
e2fsck -f /dev/$VG/$LV
mount -o ro /dev/$VG/$LV /mnt && ls /mnt && umount /mnt
vgchange -an $VG; cryptsetup close cryptroot
```

Then restart into Debian once (it must start as before), and only then
start from the SIEOS key and install into the free space ("free 2" or the
last one listed on the drive).

If a step fails, stop: everything before step 7 can be undone by growing
back (step 7 of "Removing SIEOS" below, in the reverse direction), and the
LUKS header backup restores a damaged header (`cryptsetup
luksHeaderRestore`).

## Removing SIEOS

From Debian (as root):

```sh
efibootmgr                                  # the line "Boot000N* SIEOS"
efibootmgr -b 000N -B                       # that entry removed (also possible in the firmware's setup)
rm -r /boot/efi/EFI/SIEOS                   # its folder in the EFI partition
parted /dev/nvme0n1 print                   # SIEOS's partition: name "SIEOS" (number 4 here)
parted /dev/nvme0n1 rm 4
```

To give the space back to Debian's root: grow each layer from the outside
in (all online, Debian running):

```sh
parted /dev/nvme0n1 resizepart 3 100%       # "being used": Yes (it only grows)
partprobe /dev/nvme0n1
cryptsetup resize nvme0n1p3_crypt           # the mapping's name: /etc/crypttab's first column
pvresize /dev/mapper/nvme0n1p3_crypt
lvextend -r -l +100%FREE debian-vg/root     # -r: the file system grows too
```

A whole drive given to SIEOS is reused like any drive: make a new
partition table on it from Linux, and remove the boot entry as above.
