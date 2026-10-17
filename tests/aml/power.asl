/*
 * tests/aml/power.asl - What laptops' ACPI tables do for power, in the forms
 * of Lenovo's Legion (2023: Intel's reference code, an ITE embedded
 * controller), for tests/aml/check.sh and user/acpid. power.aml is made
 * from it by iasl (acpica-tools): iasl -p power power.asl
 *
 * Sleep types (\_S3 under If, as firmware does), _PTS/_WAK writing a debug
 * port and the controller; the controller (PNP0C09): _REG sets ECAV, a
 * byte of its own space (EmbeddedControl) and its memory window (the
 * battery's fields: SystemMemory, as on the Legion); the battery: _STA,
 * _BIX and _BST filling packages from those fields; the charger (_PSR);
 * the lid (_LID, from a variable a query method sets); queries _Q15 (lid)
 * and _Q25 (battery) that Notify; PCI configuration (a bridge and a device
 * behind it); a VarPackage, Divide, ToString, Mid, Concatenate, FromBCD.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
DefinitionBlock ("", "DSDT", 2, "SIEOS", "POWER", 1)
{
    OperationRegion (GNVS, SystemMemory, 0x40002000, 0x10)
    Field (GNVS, AnyAcc, Lock, Preserve) { LIDS, 8, SS3, 8, P80D, 16 }
    OperationRegion (DBG, SystemIO, 0x80, 2)
    Field (DBG, WordAcc, NoLock, Preserve) { P80H, 16 }

    If (SS3) { Name (_S3, Package (0x04) { 0x05, Zero, Zero, Zero }) }
    Name (_S5, Package (0x04) { 0x07, Zero, Zero, Zero })
    Method (_PTS, 1) { P80H = (Arg0 | 0xF0) << 8; If (Arg0 == 3) { \_SB.PC00.LPCB.EC0.BS3F = One } }
    Method (_WAK, 1) { P80H = Arg0 << 8; \_SB.PC00.LPCB.EC0.BS3F = Zero; Return (Package (0x02) { Zero, Zero }) }

    Scope (_SB)
    {
        Device (PC00)
        {
            Name (_HID, EisaId ("PNP0A08"))
            Name (_BBN, Zero)
            Device (RP01)                                   /* a root port: the bus behind it in its configuration */
            {
                Name (_ADR, 0x001C0000)
                Device (PXSX)
                {
                    Name (_ADR, Zero)
                    OperationRegion (PCFG, PCI_Config, Zero, 0x10)
                    Field (PCFG, DWordAcc, NoLock, Preserve) { VDID, 32, CMDR, 16 }
                }
            }
            Device (LPCB)
            {
                Name (_ADR, 0x001F0000)
                Device (EC0)
                {
                    Name (_HID, EisaId ("PNP0C09"))
                    Name (ECAV, Zero)
                    Mutex (LFCM, 0)
                    Method (_GPE, 0) { Return (0x6E) }
                    Name (_CRS, ResourceTemplate () { IO (Decode16, 0x62, 0x62, 0, 1) IO (Decode16, 0x66, 0x66, 0, 1) })
                    Method (_REG, 2) { If (Arg0 == 3) { ECAV = Arg1 } }
                    OperationRegion (ERAM, EmbeddedControl, Zero, 0xFF)
                    Field (ERAM, ByteAcc, Lock, Preserve) { Offset (0x60), BS3F, 1, , 7, BTMP, 8 }
                    OperationRegion (ERAX, SystemMemory, 0xFE0B0400, 0xFF)
                    Field (ERAX, ByteAcc, Lock, Preserve)
                    {
                        Offset (0xA3), , 7, ADPT, 1,
                        Offset (0xB8), , 1, LSTE, 1,
                        Offset (0xBE), BA1P, 1,
                        Offset (0xC1), B1ST, 8, B1RC, 16, B1SN, 16, B1FV, 16, B1DV, 16, B1DC, 16, B1FC, 16,
                        Offset (0xD2), B1AC, 16,
                        Offset (0xE0), BMN0, 72
                    }
                    Method (_Q15, 0) { If (ECAV) { If (Acquire (LFCM, 0xA000) == Zero) { LIDS = LSTE; Release (LFCM) } } Notify (\_SB.LID0, 0x80) }
                    Method (_Q25, 0) { Notify (BAT0, 0x80); Notify (\_SB.ADP0, 0x80) }

                    Device (BAT0)
                    {
                        Name (_HID, EisaId ("PNP0C0A"))
                        Name (XBIF, Package (0x15) { One, Zero, 0xFFFFFFFF, 0xFFFFFFFF, One, 0xFFFFFFFF, Zero, Zero, Zero,
                                                     0x00017318, 0xFFFFFFFF, 0xFFFFFFFF, 0x03E8, 0x03E8, 0x64, Zero, "", "", "", "", One })
                        Name (PBST, Package (0x04) { One, 0x0A90, 0x1000, 0x2A30 })
                        Method (_STA, 0) { If (ECAV) { If (BA1P) { Return (0x1F) } } Return (0x0F) }
                        Method (_BIX, 0)
                        {
                            If (ECAV == One)
                            {
                                XBIF [0x02] = B1DC * 0x0A
                                XBIF [0x03] = B1FC * 0x0A
                                XBIF [0x05] = B1DV
                                Name (SERN, Buffer (0x06) { "     " })
                                Local0 = B1SN
                                Local2 = 0x04
                                While (Local0) { Divide (Local0, 0x0A, Local1, Local0); SERN [Local2] = Local1 + 0x30; Local2-- }
                                XBIF [0x11] = SERN
                                Name (BMNT, Buffer (0x0A) { 0x00 })
                                BMNT = BMN0
                                XBIF [0x13] = ToString (BMNT, Ones)
                            }
                            Return (XBIF)
                        }
                        Method (_BST, 0, Serialized)
                        {
                            Switch (B1ST & 0x07)
                            {
                                Case (One) { PBST [Zero] = One }
                                Case (0x02) { PBST [Zero] = 0x02 }
                                Default { PBST [Zero] = Zero }
                            }
                            PBST [One] = B1AC * B1FV / 1000
                            PBST [0x02] = B1RC * 0x0A
                            PBST [0x03] = B1FV
                            Return (PBST)
                        }
                    }
                }
            }
        }
        Device (ADP0)
        {
            Name (_HID, "ACPI0003")
            Method (_PSR, 0) { If (\_SB.PC00.LPCB.EC0.ECAV) { Return (\_SB.PC00.LPCB.EC0.ADPT) } Return (One) }
        }
        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_LID, 0) { Return (LIDS) }
        }
        Device (PWRB) { Name (_HID, EisaId ("PNP0C0C")) }
        Name (MSCR, Package (0x05) { })
        Method (MISC, 0)                                    /* what else the interpreter now runs */
        {
            Local0 = Concatenate ("ab", "cd")
            Local1 = Mid (Local0, One, 0x02)
            Local3 = 0x03
            Local2 = Package (Local3) { 0x11, 0x22 }
            MSCR [Zero] = FromBCD (0x1234)
            MSCR [One] = ToBCD (0x63)
            MSCR [0x02] = SizeOf (Local0)
            MSCR [0x03] = DerefOf (Local1 [One])
            MSCR [0x04] = SizeOf (Local2)
            Return (MSCR)
        }
    }
}
