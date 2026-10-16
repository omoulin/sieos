/*
 * tests/aml/laptop.asl - What laptops' ACPI tables do with I2C touchpads,
 * in the forms seen in real firmware (Intel's reference code as Lenovo's
 * Legion 2023-2025 use it, and plain constants), for tests/aml/check.sh.
 * laptop.aml is made from it by iasl (acpica-tools): iasl -p laptop laptop.asl
 *
 * TPD0: the model is a firmware variable in memory (TPTY); _INI picks its
 * name, address and descriptor register; the pin comes from GNUM (a GPIO
 * group table); _CRS builds the I2C resource in I2CM (a Switch on the
 * controller's number, set by the controller's _INI); _DSM calls HIDD.
 * TPD1: everything constant. TPD2: not present (_STA from memory).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
DefinitionBlock ("", "DSDT", 2, "SIEOS", "LAPTOP", 1)
{
    OperationRegion (GNVS, SystemMemory, 0x40001000, 0x20)
    Field (GNVS, AnyAcc, Lock, Preserve)
    {
        OSYS, 16,
        Offset (0x04),
        TPDM, 8,
        SDS5, 8,
        Offset (0x08),
        SBRG, 32,
        Offset (0x10),
        TPTY, 8,
        , 4,
        TP2S, 4
    }

    Scope (_SB)
    {
        Name (GPCS, Package (0x02)
        {
            Package (0x09) { 0x006E0000, 0x19, 0x0700, 0x0150, 0xA0, 0x0240, 0x0110, 0x0114, Zero },
            Package (0x09) { 0x006E0000, 0x17, 0x0890, 0x0154, 0xB0, 0x0244, 0x0118, 0x011C, 0x20 }
        })
        Method (GINF, 2, NotSerialized) { Return (DerefOf (DerefOf (GPCS [Arg0]) [Arg1])) }
        Method (GADR, 2, NotSerialized) { Return (GINF (Arg0, Zero) + SBRG + GINF (Arg0, Arg1)) }
        Method (GGRP, 1, Serialized) { Return ((Arg0 & 0x00FF0000) >> 0x10) }
        Method (GNMB, 1, Serialized) { Return (Arg0 & 0xFFFF) }
        Method (GNUM, 1, NotSerialized)
        {
            Local0 = GNMB (Arg0)
            Local1 = GGRP (Arg0)
            Return (GINF (Local1, 0x08) + Local0)
        }

        Device (PC00)
        {
            Name (_HID, EisaId ("PNP0A08"))
            Name (HIDG, ToUUID ("3cdff6f7-4267-4555-ad05-b30a3d8938de"))
            Method (HIDD, 5, Serialized)
            {
                If (Arg0 == HIDG)
                {
                    If (Arg2 == Zero) { If (Arg1 == One) { Return (Buffer (One) { 0x03 }) } }
                    If (Arg2 == One) { Return (Arg4) }
                }
                Return (Buffer (One) { 0x00 })
            }
            Method (I2CM, 3, Serialized)
            {
                Switch (ToInteger (Arg0))
                {
                    Case (Zero)
                    {
                        Name (IIC0, ResourceTemplate ()
                        {
                            I2cSerialBusV2 (0x0000, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.PC00.I2C0", 0x00, ResourceConsumer, _Y00, Exclusive, )
                        })
                        CreateWordField (IIC0, \_SB.PC00.I2CM._Y00._ADR, DAD0)
                        CreateDWordField (IIC0, \_SB.PC00.I2CM._Y00._SPE, DSP0)
                        DAD0 = Arg1
                        DSP0 = Arg2
                        Return (IIC0)
                    }
                    Case (0x05)
                    {
                        Name (IIC5, ResourceTemplate ()
                        {
                            I2cSerialBusV2 (0x0000, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.PC00.I2C5", 0x00, ResourceConsumer, _Y05, Exclusive, )
                        })
                        CreateWordField (IIC5, \_SB.PC00.I2CM._Y05._ADR, DAD5)
                        CreateDWordField (IIC5, \_SB.PC00.I2CM._Y05._SPE, DSP5)
                        DAD5 = Arg1
                        DSP5 = Arg2
                        Return (IIC5)
                    }
                }
                Return (ResourceTemplate () {})
            }
            Device (I2C1) { Name (_ADR, 0x00150001) }
            Device (I2C5)
            {
                If (One) { Method (_ADR, 0, NotSerialized) { Return (0x00190001) } }
            }
        }
    }

    Scope (_SB.PC00.I2C5)
    {
        Name (I2CN, Zero)
        Name (I2CX, Zero)
        Method (_INI, 0, NotSerialized)
        {
            I2CN = SDS5
            I2CX = 0x05
        }
        Device (TPD0)
        {
            Name (HID2, Zero)
            Name (SBFB, ResourceTemplate ()
            {
                I2cSerialBusV2 (0x002C, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.PC00.I2C5", 0x00, ResourceConsumer, _Y10, Exclusive, )
            })
            Name (SBFG, ResourceTemplate ()
            {
                GpioInt (Level, ActiveLow, ExclusiveAndWake, PullDefault, 0x0000, "\\_SB.GPI0", 0x00, ResourceConsumer, , ) { 0x0000 }
            })
            CreateWordField (SBFB, \_SB.PC00.I2C5.TPD0._Y10._ADR, BADR)
            CreateDWordField (SBFB, \_SB.PC00.I2C5.TPD0._Y10._SPE, SPED)
            CreateWordField (SBFG, 0x17, INT1)
            Method (_INI, 0, NotSerialized)
            {
                If (OSYS < 0x07DC) { Return (Zero) }
                INT1 = GNUM (0x08010003)
                If (TPTY == One)
                {
                    _HID = "ELAN06FA"
                    BADR = 0x15
                    HID2 = One
                    Return (Zero)
                }
                If (TPTY == 0x02)
                {
                    _HID = "SYNA2BA6"
                    BADR = 0x2C
                    HID2 = 0x20
                    Return (Zero)
                }
            }
            Name (_HID, "XXXX0000")
            Name (_CID, "PNP0C50")
            Method (_DSM, 4, Serialized)
            {
                If (Arg0 == HIDG) { Return (HIDD (Arg0, Arg1, Arg2, Arg3, HID2)) }
                Return (Buffer (One) { 0x00 })
            }
            Method (_STA, 0, NotSerialized)
            {
                If (TPTY == Zero) { Return (Zero) }
                Return (0x0F)
            }
            Method (_CRS, 0, NotSerialized)
            {
                If (OSYS < 0x07DC) { Return (ResourceTemplate () {}) }
                If (TPDM == Zero) { Return (ConcatenateResTemplate (I2CM (I2CX, BADR, SPED), SBFG)) }
                Return (ResourceTemplate () {})
            }
        }
    }

    Scope (_SB.PC00.I2C1)
    {
        Device (TPD1)
        {
            Name (_HID, "SYNA0001")
            Name (_CID, EisaId ("PNP0C50"))
            Name (_CRS, ResourceTemplate ()
            {
                I2cSerialBusV2 (0x002C, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.PC00.I2C1", 0x00, ResourceConsumer, , Exclusive, )
                GpioInt (Edge, ActiveLow, Exclusive, PullUp, 0x0000, "\\_SB.GPI0", 0x00, ResourceConsumer, , ) { 0x0045 }
            })
            Method (_DSM, 4, NotSerialized)
            {
                If (Arg0 == ToUUID ("3cdff6f7-4267-4555-ad05-b30a3d8938de")) { Return (0x20) }
                Return (Buffer (One) { 0x00 })
            }
        }
        Device (TPD2)
        {
            Name (_HID, "ACPI0C50")
            Method (_STA, 0, NotSerialized) { Return (TP2S) }
        }
    }
}
