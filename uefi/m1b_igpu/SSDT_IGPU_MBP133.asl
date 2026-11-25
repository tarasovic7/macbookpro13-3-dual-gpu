/*
 * SSDT_IGPU_MBP133.asl - Milestone 1B ACPI patch for Apple MacBookPro13,3 (T1, Intel HD 530 + Polaris 11)
 *
 * Bridges Windows Intel iGPU brightness (\_SB.PCI0.IGPU.DD01._BCM) to Apple's native
 * Indexed I/O GMUX backlight helper (\_SB.PCI0.PEG0.GFX0.ABCM) and preserves iGPU
 * display routing (and dGPU power-off state) across S3 suspend/resume (_PTS / _WAK).
 *
 * In-memory DSDT method renames performed by mbp133_m1b_igpu.efi:
 *   - \_SB.PCI0.IGPU.DD01._BCM -> XBCM
 *   - \_PTS -> XPTS
 *   - \_WAK -> XWAK
 *
 * SPDX-License-Identifier: MIT
 */
DefinitionBlock ("", "SSDT", 2, "MBP133", "IGPUBCM", 0x00001000)
{
    External (_SB_.PCI0.IGPU.DD01, DeviceObj)
    External (_SB_.PCI0.IGPU.DD01.XBCM, MethodObj)    // 1 Arg: renamed DSDT DD01._BCM
    External (_SB_.PCI0.PEG0.GFX0.ABCM, MethodObj)    // 1 Arg: native Apple GMUX brightness in PEG0GFX0
    External (_SB_.PCI0.PEG0.GFX0.SGMX, MethodObj)    // 3 Args: native Apple GMUX indexed write
    External (_SB_.PCI0.PEG0.GFX0.VID0, FieldUnitObj) // 16-bit AMD PCI VID (0xFFFF when powered off)
    External (BRTL, FieldUnitObj)                     // 8-bit brightness value in DSDT
    External (XPTS, MethodObj)                        // 1 Arg: renamed DSDT _PTS
    External (XWAK, MethodObj)                        // 1 Arg: renamed DSDT _WAK

    Scope (\_SB.PCI0.IGPU.DD01)
    {
        Method (_BCM, 1, NotSerialized)
        {
            XBCM (Arg0)
            If (CondRefOf (\_SB.PCI0.PEG0.GFX0.ABCM))
            {
                \_SB.PCI0.PEG0.GFX0.ABCM (Arg0)
            }
        }
    }

    // Read-only Indexed I/O GMUX port access for resume verification (Renesas R4F2113 @ 0x700..0x7FE)
    OperationRegion (M1IR, SystemIO, 0x07C2, One)
    Field (M1IR, ByteAcc, NoLock, Preserve)
    {
        M1DT,   8
    }

    OperationRegion (M1P1, SystemIO, 0x07D0, One)
    Field (M1P1, ByteAcc, NoLock, Preserve)
    {
        M1PR,   8
    }

    OperationRegion (M1P2, SystemIO, 0x07D4, One)
    Field (M1P2, ByteAcc, NoLock, Preserve)
    {
        M1PW,   8
    }

    Name (M1OF, Zero)   // 1 = Radeon rail was OFF before sleep

    Method (M1RD, 1, Serialized)
    {
        Local0 = 0xC8
        Local1 = M1PW
        While ((Local0 && ((Local1 & One) == One)))
        {
            If (M1PR) {}
            Local1 = M1PW
            Stall (0x64)
            Local0--
        }

        If ((Local0 == Zero))
        {
            Return (0xFF)
        }

        M1PR = (Arg0 & 0xFF)
        Local0 = 0xC8
        Local1 = M1PW
        While ((Local0 && ((Local1 & One) == Zero)))
        {
            Local1 = M1PW
            Stall (0x64)
            Local0--
        }

        If (((Local1 & One) == Zero))
        {
            Return (0xFF)
        }

        If (M1PR) {}
        Return (M1DT)
    }

    Method (_PTS, 1, NotSerialized)
    {
        If (CondRefOf (\_SB.PCI0.PEG0.GFX0.VID0))
        {
            If ((\_SB.PCI0.PEG0.GFX0.VID0 == 0xFFFF))
            {
                M1OF = One
            }
            Else
            {
                M1OF = Zero
            }
        }
        Else
        {
            M1OF = Zero
        }

        XPTS (Arg0)
    }

    Method (_WAK, 1, NotSerialized)
    {
        Local0 = XWAK (Arg0)
        If (CondRefOf (\_SB.PCI0.PEG0.GFX0.SGMX))
        {
            \_SB.PCI0.PEG0.GFX0.SGMX (0x0728, One, One)   // SWITCH_DDC -> IGD
            \_SB.PCI0.PEG0.GFX0.SGMX (0x0710, One, 0x02)  // SWITCH_DISPLAY -> IGD
            \_SB.PCI0.PEG0.GFX0.SGMX (0x0740, One, 0x03)  // SWITCH_EXTERNAL -> DIS
            Sleep (0x0A)
            If ((M1OF == One))
            {
                Local1 = M1RD (0x10)
                If (((Local1 != 0xFF) && ((Local1 & One) == Zero)))
                {
                    \_SB.PCI0.PEG0.GFX0.SGMX (0x0750, One, One)
                    \_SB.PCI0.PEG0.GFX0.SGMX (0x0750, One, Zero)
                    Sleep (0xFA)
                }
            }

            If (CondRefOf (\_SB.PCI0.PEG0.GFX0.ABCM))
            {
                \_SB.PCI0.PEG0.GFX0.ABCM (BRTL)
            }
        }

        Return (Local0)
    }
}
