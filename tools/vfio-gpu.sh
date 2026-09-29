#!/bin/sh
# vfio-gpu.sh - hand a host PCI GPU to QEMU (VFIO) for 'make run-uefi GPU=intel', and back.
#
#   tools/vfio-gpu.sh status  [PCI]        which driver has the GPU, its IOMMU group
#   sudo tools/vfio-gpu.sh bind  [PCI]     detach it from the host (i915/xe) and bind vfio-pci
#   sudo tools/vfio-gpu.sh unbind [PCI]    give it back to the host's driver
#   sudo tools/vfio-gpu.sh run PCI -- QEMU ARGS...
#        run QEMU as the calling user with the memory-lock limit VFIO needs
#   tools/vfio-gpu.sh check [PCI]          fail unless the GPU is bound to vfio-pci
#   sudo tools/vfio-gpu.sh rom [PCI] [OUT]  save the GPU's option ROM (default build/igd.rom)
#        and show its images; a UEFI one (the GOP driver) is what GPU_ROM needs
#   tools/vfio-gpu.sh romcheck FILE        show the images of a ROM file
#
# PCI defaults to 0000:00:02.0, the Intel integrated GPU.
#
# WARNING: while the GPU is bound to vfio-pci the host cannot use it.  When it
# drives the host's screens (on a laptop, the integrated GPU usually drives the
# built-in panel), they go dark: bind from a text console (Ctrl+Alt+F3) or over
# ssh, with the graphical session stopped (e.g. 'sudo systemctl stop gdm'), and
# unbind afterwards.  The guest's picture then appears on the GPU's own outputs.
set -e
cmd=${1:-status}
[ $# -gt 0 ] && shift
PCI=0000:00:02.0
case "$1" in
    ????:??:??.?) PCI=$1; shift ;;
esac
DEV=/sys/bus/pci/devices/$PCI

die() { echo "vfio-gpu: $*" >&2; exit 1; }
[ -d "$DEV" ] || die "no PCI device $PCI"
driver() { [ -e "$DEV/driver" ] && basename "$(readlink "$DEV/driver")" || echo none; }
group() { [ -e "$DEV/iommu_group" ] && basename "$(readlink "$DEV/iommu_group")" || echo ""; }
need_root() { [ "$(id -u)" = 0 ] || die "run this as root (sudo $0 $cmd $PCI)"; }

case $cmd in
status)
    g=$(group)
    echo "$PCI: $(lspci -s "$PCI" 2>/dev/null | cut -d' ' -f2- || echo '?')"
    echo "  driver:      $(driver)"
    if [ -n "$g" ]; then
        echo "  IOMMU group: $g ($(ls /sys/kernel/iommu_groups/$g/devices | tr '\n' ' '))"
    else
        echo "  IOMMU group: none - enable VT-d in the firmware and boot with intel_iommu=on"
    fi
    echo "  memlock:     $(ulimit -l) KiB (VFIO pins all guest memory; 'run' raises it)"
    ;;
check)
    [ "$(driver)" = vfio-pci ] || die "$PCI is bound to $(driver), not vfio-pci: first 'sudo $0 bind $PCI' (read the warning in $0)"
    g=$(group)
    [ -r /dev/vfio/$g ] && [ -w /dev/vfio/$g ] || die "/dev/vfio/$g is not accessible: 'sudo $0 bind $PCI' again"
    ;;
bind)
    need_root
    g=$(group)
    [ -n "$g" ] || die "$PCI has no IOMMU group: enable VT-d and boot the host with intel_iommu=on"
    others=$(ls /sys/kernel/iommu_groups/$g/devices | grep -v "^$PCI\$" || true)
    [ -z "$others" ] || die "IOMMU group $g also holds $others: they would all have to be passed"
    modprobe vfio-pci
    echo vfio-pci > "$DEV/driver_override"
    [ "$(driver)" = none ] || echo "$PCI" > "$DEV/driver/unbind"
    echo "$PCI" > /sys/bus/pci/drivers_probe
    [ "$(driver)" = vfio-pci ] || die "vfio-pci did not take $PCI (driver: $(driver))"
    [ -n "$SUDO_UID" ] && chown "$SUDO_UID" /dev/vfio/$g
    echo "$PCI is bound to vfio-pci (group $g); 'sudo $0 unbind' gives it back"
    ;;
unbind)
    need_root
    echo > "$DEV/driver_override"
    [ "$(driver)" = none ] || echo "$PCI" > "$DEV/driver/unbind"
    echo "$PCI" > /sys/bus/pci/drivers_probe
    echo "$PCI is bound to $(driver)"
    ;;
rom|romcheck)
    if [ "$cmd" = rom ]; then
        need_root
        out=${1:-build/igd.rom}
        [ -e "$DEV/rom" ] || die "$PCI has no ROM in sysfs"
        echo 1 > "$DEV/rom"
        if ! cat "$DEV/rom" > "$out" 2>/dev/null; then
            echo 0 > "$DEV/rom"
            die "the ROM of $PCI cannot be read (the firmware keeps the GOP driver elsewhere)"
        fi
        echo 0 > "$DEV/rom"
        [ -n "$SUDO_UID" ] && chown "$SUDO_UID:$SUDO_GID" "$out"
        echo "saved $out ($(stat -c %s "$out") bytes)"
    else
        out=$1
        [ -f "$out" ] || die "romcheck: no file $out"
    fi
    python3 - "$out" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
off, found, efi = 0, 0, False
kinds = {0: 'legacy x86 (VBIOS)', 1: 'Open Firmware', 2: 'HP PA-RISC', 3: 'UEFI (GOP driver)'}
while off + 0x1A <= len(data) and data[off:off + 2] == b'\x55\xaa':
    pcir = off + struct.unpack_from('<H', data, off + 0x18)[0]
    if data[pcir:pcir + 4] != b'PCIR':
        break
    vendor, device = struct.unpack_from('<HH', data, pcir + 4)
    length = struct.unpack_from('<H', data, pcir + 0x10)[0] * 512
    ctype, last = data[pcir + 0x14], data[pcir + 0x15] & 0x80
    print(f'  image at {off:#x}: {kinds.get(ctype, ctype)}, {vendor:04x}:{device:04x}, {length} bytes')
    found += 1
    efi |= ctype == 3
    if last or not length:
        break
    off += length
if not found:
    print('  no option ROM image in it (no 55 AA / PCIR header): not usable as GPU_ROM')
elif efi:
    print('  it has a UEFI image: try it with  make run-uefi GPU=intel GPU_ROM=' + sys.argv[1])
else:
    print('  no UEFI image: OVMF cannot use it; extract the GOP driver from the firmware instead')
PY
    ;;
run)
    need_root
    [ "$1" = -- ] && shift
    [ $# -gt 0 ] || die "run: no command"
    ulimit -l unlimited
    if [ -n "$SUDO_UID" ]; then
        exec setpriv --reuid="$SUDO_UID" --regid="$SUDO_GID" --init-groups --inh-caps=-all "$@"
    fi
    exec "$@"
    ;;
*)
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
    ;;
esac
