#!/bin/sh
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
# mkiso.sh OUT.iso ISODIR BUILDDIR [PARTITION.img]
#
# Build a hybrid ISO that boots with both legacy BIOS (El Torito, GRUB
# i386-pc) and UEFI (El Torito EFI system partition, GRUB x86_64-efi).
# The GRUB images embed every module they need plus an early config that
# locates the ISO file system and loads /boot/grub/grub.cfg.  PARTITION.img,
# if given, is appended as partition 3 of an MBR table (the brain image's packages),
# with the EFI system partition as partition 2 (UEFI firmware boots from it: with a
# partition table it no longer looks at the El Torito image).
set -e
OUT=$1
ISODIR=$2
BUILD=$3
APPEND=$4
GRUB_MODS="normal configfile search search_fs_file test echo multiboot2 iso9660 part_msdos part_gpt"

echo "SIEOS" > "$ISODIR/boot/sieos.tag"

cat > "$BUILD/grub-early.cfg" <<'EOF'
search --no-floppy --set=root --file /boot/sieos.tag
set prefix=($root)/boot/grub
configfile ($root)/boot/grub/grub.cfg
EOF

# Legacy BIOS boot image
grub-mkimage -O i386-pc-eltorito -d /usr/lib/grub/i386-pc -p /boot/grub \
    -c "$BUILD/grub-early.cfg" -o "$ISODIR/boot/grub/bios.img" \
    $GRUB_MODS biosdisk

# UEFI boot image: BOOTX64.EFI inside a FAT file system
grub-mkimage -O x86_64-efi -d /usr/lib/grub/x86_64-efi -p /boot/grub \
    -c "$BUILD/grub-early.cfg" -o "$BUILD/BOOTX64.EFI" \
    $GRUB_MODS fat efi_gop all_video video gfxterm
EFI_KB=$(( $(stat -c %s "$BUILD/BOOTX64.EFI") / 1024 + 512 ))
python3 "$(dirname "$0")/mkfat.py" "$ISODIR/boot/efi.img" "$EFI_KB" \
    "EFI/BOOT/BOOTX64.EFI=$BUILD/BOOTX64.EFI"

xorriso -as mkisofs -quiet -o "$OUT" -R -J -V SIEOS \
    -b boot/grub/bios.img -no-emul-boot -boot-load-size 4 -boot-info-table \
    --grub2-boot-info --grub2-mbr /usr/lib/grub/i386-pc/boot_hybrid.img \
    -eltorito-alt-boot -e boot/efi.img -no-emul-boot -isohybrid-gpt-basdat \
    ${APPEND:+-append_partition 2 0xef "$ISODIR/boot/efi.img" -append_partition 3 0x83 "$APPEND"} \
    "$ISODIR"
