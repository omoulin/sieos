#!/bin/sh
# tests/aml/check.sh aml-test: the AML interpreter on laptop.aml (made by
# iasl from laptop.asl), with the firmware's variables set three ways; then
# on power.aml (from power.asl): what user/acpid evaluates.
# Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
t=$1; a=$(dirname "$0")/laptop.aml
out=$( { "$t" -m 0x40001010=0x01 -m 0x40001008=0xE0000000 "$a"; "$t" -m 0x40001010=0xF002 "$a"; "$t" "$a"; } 2>/dev/null)
tpd1='\_SB_.PC00.I2C1.TPD1 SYNA0001 addr=0x2c speed=100000 bus=\_SB.PC00.I2C1 adr=0x150001 pin=69(low) gpio=\_SB.GPI0 irq=-1 desc=32'
exp="53 named objects
\\_SB_.PC00.I2C5.TPD0 ELAN06FA addr=0x15 speed=400000 bus=\\_SB.PC00.I2C5 adr=0x190001 pin=35(low) gpio=\\_SB.GPI0 irq=-1 desc=1
  GPIO group 1 pad 3 at 0xe06e08c0
$tpd1
53 named objects
\\_SB_.PC00.I2C5.TPD0 SYNA2BA6 addr=0x2c speed=400000 bus=\\_SB.PC00.I2C5 adr=0x190001 pin=35(low) gpio=\\_SB.GPI0 irq=-1 desc=32
  GPIO group 1 pad 3 at 0x6e08c0
$tpd1
\\_SB_.PC00.I2C1.TPD2 ACPI0C50 addr=0xffffffff speed=0 bus= adr=0xffffffffffffffff pin=-1 gpio= irq=-1 desc=-1
53 named objects
$tpd1"
if [ "$out" = "$exp" ]; then echo "aml-test: ok (3 touchpad cases)"; else printf 'aml-test: FAILED, got:\n%s\n' "$out"; exit 1; fi

# power.aml (user/acpid): sleep types, the controller, battery (80%, charging),
# AC, the lid opened by a query, a Notify, _PTS/_WAK's writes, PCI behind a bridge
p=$(dirname "$0")/power.aml
m="-m 0x40002000=0x100 -m 0xFE0B04BC=0x10000 -m 0xFE0B04C0=0x1F400200 -m 0xFE0B04C4=0x3E801234 -m 0xFE0B04C8=0x270F3E80"
m="$m -m 0xFE0B04CC=0x2710 -m 0xFE0B04D0=0x0E100000 -m 0xFE0B04A0=0x80000000 -m 0xFE0B04B8=2"
E='\_SB.PC00.LPCB.EC0'
out=$("$t" -w $m -d PNP0C09 -d PNP0C0A -d ACPI0003 -d PNP0C0D -v '\_S3' -v '\_S5' -v "$E._GPE" -v "$E._REG:3,1" -v "$E.BAT0._STA" \
      -v "$E.BAT0._BIX" -v "$E.BAT0._BST" -v '\_SB.ADP0._PSR' -v '\_SB.LID0._LID' -v "$E._Q15" -v '\_SB.LID0._LID' -v '\_PTS:3' \
      -v '\_WAK:3' -v '\_SB.PC00.RP01.PXSX.CMDR' -v '\_SB.MISC' "$p" 2>&1 | grep -v 'BAT0._BIX')
exp='71 named objects
PNP0C09: \_SB_.PC00.LPCB.EC0_
PNP0C0A: \_SB_.PC00.LPCB.EC0_.BAT0
ACPI0003: \_SB_.ADP0
PNP0C0D: \_SB_.LID0
\_S3 = 0x5 0x0 0x0 0x0
\_S5 = 0x7 0x0 0x0 0x0
\_SB.PC00.LPCB.EC0._GPE = 0x6e
\_SB.PC00.LPCB.EC0._REG:3,1 = 0x0
\_SB.PC00.LPCB.EC0.BAT0._STA = 0x1f
\_SB.PC00.LPCB.EC0.BAT0._BST = 0x2 0xe100 0x13880 0x3e80
\_SB.ADP0._PSR = 0x1
\_SB.LID0._LID = 0x0
write memory 0x40002000/1 = 0x1
notify \_SB_.LID0 0x80
\_SB.PC00.LPCB.EC0._Q15 = 0x0
\_SB.LID0._LID = 0x1
write io 0x80/2 = 0xf300
write ec 0x60/1 = 0x1
\_PTS:3 = 0x0
write io 0x80/2 = 0x300
write ec 0x60/1 = 0x0
\_WAK:3 = 0x0 0x0
read pci 00:1c.0 18/4
read pci 02:00.0 04/4
\_SB.PC00.RP01.PXSX.CMDR = 0x0
\_SB.MISC = 0x4d2 0x99 0x4 0x63 0x3'
bix=$("$t" $m -v "$E._REG:3,1" -v "$E.BAT0._BIX" "$p" 2>&1 | tail -1 | cut -d' ' -f3-7)
if [ "$out" = "$exp" ] && [ "$bix" = "0x1 0x0 0x18696 0x186a0 0x1" ]; then echo "aml-test: ok (power)"
else printf 'aml-test: power FAILED, got:\n%s\n_BIX %s\n' "$out" "$bix"; exit 1; fi
