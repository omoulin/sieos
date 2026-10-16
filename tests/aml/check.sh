#!/bin/sh
# tests/aml/check.sh aml-test: the AML reader on laptop.aml (made by iasl
# from laptop.asl), with the firmware's variables set three ways.
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
if [ "$out" = "$exp" ]; then echo "aml-test: ok (3 cases)"; else printf 'aml-test: FAILED, got:\n%s\n' "$out"; exit 1; fi
