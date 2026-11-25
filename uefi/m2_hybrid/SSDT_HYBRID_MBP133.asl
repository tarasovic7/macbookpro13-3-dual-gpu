/*
 * SSDT_HYBRID_MBP133.asl - Milestone 2 Hybrid Mode 5 ACPI patch for Apple MacBookPro13,3
 *
 * Bridges Windows Intel iGPU brightness (\_SB.PCI0.IGPU.DD01._BCM) to Apple's native
 * Indexed I/O GMUX backlight helper (\_SB.PCI0.PEG0.GFX0.ABCM) and preserves iGPU
 * display routing across S3 suspend/resume (_PTS / _WAK) WITHOUT touching GMUX port
 * 0x50 (dGPU power rail), so AMD Radeon RX 560X (01:00.0) stays powered across S3 wake.
 *
 * In-memory DSDT method renames performed by mbp133_m2_hybrid.efi:
 *   - \_SB.PCI0.IGPU.DD01._BCM -> XBCM
 *   - \_PTS -> XPTS
 *   - \_WAK -> XWAK
 *
 * SPDX-License-Identifier: MIT
 */
DefinitionBlock ("", "SSDT", 2, "MBP133", "M2HYBCM", 0x00001000)
{
    External (_SB_.PCI0.IGPU.DD01, DeviceObj)
    External (_SB_.PCI0.IGPU.DD01.XBCM, MethodObj)    // 1 Arg: renamed DSDT DD01._BCM
    External (_SB_.PCI0.PEG0.GFX0.ABCM, MethodObj)    // 1 Arg: native Apple GMUX brightness in PEG0GFX0
    External (_SB_.PCI0.PEG0.GFX0.SGMX, MethodObj)    // 3 Args: native Apple GMUX indexed write
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

    Method (_PTS, 1, NotSerialized)
    {
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
            // Hybrid Mode 5: NEVER touch GMUX port 0x50 (dGPU power rail stays ON)
            If (CondRefOf (\_SB.PCI0.PEG0.GFX0.ABCM))
            {
                \_SB.PCI0.PEG0.GFX0.ABCM (BRTL)
            }
        }

        Return (Local0)
    }
}
