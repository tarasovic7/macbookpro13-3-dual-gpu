/*
 * mbp133_m1a2_ebs.c - Milestone 1A-2 ExitBootServices() X-Ray & Dual-GPU Isolator
 * Target: Apple MacBookPro13,3 (Skylake i7-6920HQ + Intel HD 530 + AMD Radeon Pro 455 Polaris 11)
 *
 * Hooks gBS->ExitBootServices() cleanly (EFIAPI MS ABI) to capture identical
 * hardware telemetry snapshots directly BEFORE and AFTER Apple EFI's
 * ExitBootServices() callback into an EfiACPIReclaimMemory ACPI table ("M1A2"),
 * readable from Windows without Admin privileges via GetSystemFirmwareTable('ACPI', '2A1M').
 *
 * Interactive modes:
 *   [1] Normal (NO AppleSetOs) + EBS X-Ray Capture
 *   [2] SetOS 10.15 + EBS X-Ray Capture
 *   [3] SetOS 10.15 + Hide 00:02.0 (DEVEN.D2EN=0) Post-EBS  (Cause A vs Cause B)
 *   [4] Normal + Unhide 00:02.0 (DEVEN.D2EN=1 + BARs) Post-EBS (Dual-GPU Fix Candidate)
 *   [0] Cancel / Normal Windows Boot (no hook, zero changes)
 *
 * SPDX-License-Identifier: MIT
 */

#include <efi.h>
#include <efiapi.h>
#include <eficon.h>
#include <efidef.h>
#include <efidevp.h>
#include <efierr.h>
#include <efiprot.h>

/* ---- Custom / Apple / Console Control Protocols ---- */

#define EFI_CONSOLE_CONTROL_PROTOCOL_GUID \
    { 0xf42f7782, 0x012e, 0x4c12, { 0x99, 0x56, 0x49, 0xf9, 0x43, 0x04, 0xf7, 0x21 } }

typedef enum {
    EfiConsoleControlScreenText = 0,
    EfiConsoleControlScreenGraphics = 1,
    EfiConsoleControlScreenMaxValue = 2
} EFI_CONSOLE_CONTROL_SCREEN_MODE;

typedef struct _EFI_CONSOLE_CONTROL_PROTOCOL EFI_CONSOLE_CONTROL_PROTOCOL;

struct _EFI_CONSOLE_CONTROL_PROTOCOL {
    EFI_STATUS (EFIAPI *GetMode)(
        IN  EFI_CONSOLE_CONTROL_PROTOCOL    *This,
        OUT EFI_CONSOLE_CONTROL_SCREEN_MODE *Mode,
        OUT BOOLEAN                         *GopUgaExists OPTIONAL,
        OUT BOOLEAN                         *StdInLocked OPTIONAL
    );
    EFI_STATUS (EFIAPI *SetMode)(
        IN  EFI_CONSOLE_CONTROL_PROTOCOL    *This,
        IN  EFI_CONSOLE_CONTROL_SCREEN_MODE Mode
    );
    EFI_STATUS (EFIAPI *LockStdIn)(
        IN  EFI_CONSOLE_CONTROL_PROTOCOL    *This,
        IN  CHAR16                          *Password
    );
};

#define EFI_APPLE_SET_OS_PROTOCOL_GUID \
    { 0xc5c5da95, 0x7d5c, 0x45e6, { 0xb2, 0xf1, 0x3f, 0xd5, 0x2b, 0xb1, 0x00, 0x77 } }

typedef struct {
    UINT64 Version;
    EFI_STATUS (EFIAPI *SetOsVersion)(IN CHAR8 *Version);
    EFI_STATUS (EFIAPI *SetOsVendor)(IN CHAR8 *Vendor);
} EFI_APPLE_SET_OS_PROTOCOL;

#ifndef EFI_ACPI_20_TABLE_GUID
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }
#endif

static EFI_GUID gConsoleCtrlGuid = EFI_CONSOLE_CONTROL_PROTOCOL_GUID;
static EFI_GUID gAppleSetOsGuid  = EFI_APPLE_SET_OS_PROTOCOL_GUID;
static EFI_GUID gAcpi20Guid      = EFI_ACPI_20_TABLE_GUID;
static EFI_GUID gDevicePathGuid  = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID gSimpleFsGuid    = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

/* ---- Polaris 11 (Baffin / SMU 7.1.3) Register Definitions ---- */

#define mmSMC_IND_ACCESS_CNTL       0x0092u
#define mmSMC_MESSAGE_0             0x0094u
#define mmSMC_RESP_0                0x0095u
#define mmSMC_MSG_ARG_0             0x00A4u
#define mmSMC_IND_INDEX_11          0x01ACu
#define mmSMC_IND_DATA_11           0x01ADu

#define mmBIOS_SCRATCH_0            0x05C9u
#define mmMC_VM_FB_LOCATION         0x0809u
#define mmCONFIG_MEMSIZE            0x150Au
#define mmCONFIG_APER_SIZE          0x150Cu
#define mmBIF_FB_EN                 0x1524u
#define mmGRBM_STATUS               0x2004u

#define ixSMC_SYSCON_RESET_CNTL     0x80000000u
#define ixSMC_SYSCON_CLOCK_CNTL_0   0x80000004u
#define ixSMC_SYSCON_MISC_CNTL      0x80000010u
#define ixSMC_PC_C                  0x80000370u
#define ixRCU_UC_EVENTS             0xC0000004u
#define ixSMU_EFUSE_49              (0xC0100000u + (49u * 4u))
#define ixSMU_STATUS                0xE0003088u
#define ixSMU_FIRMWARE              0xE00030A4u
#define ixFIRMWARE_FLAGS            0x0003F000u

/* ---- Apple Indexed I/O GMUX ---- */

#define GMUX_IO_VALUE               0x07C2u
#define GMUX_IO_PROBE0              0x07CCu
#define GMUX_IO_PROBE1              0x07CDu
#define GMUX_IO_PROBE2              0x07CEu
#define GMUX_IO_READ                0x07D0u
#define GMUX_IO_WRITE               0x07D4u

/* ---- M1A-2 Shared ACPI Table Layout (exact 4096 bytes) ---- */

#define M1A2_MAGIC                  0x3241314Du /* 'M1A2' LE */
#define M1A2_VERSION                2u

#define M1A2_MODE_NORMAL_XRAY       1u
#define M1A2_MODE_SETOS_XRAY        2u
#define M1A2_MODE_SETOS_HIDE_IGPU   3u
#define M1A2_MODE_NORMAL_UNHIDE_IGPU 4u
#define M1A2_MODE_SETOS_FIX_FBLOC   5u

#define M1A2_FLAG_BEFORE_EBS        (1u << 0)
#define M1A2_FLAG_AFTER_EBS         (1u << 1)
#define M1A2_FLAG_POST_ACTION       (1u << 2)
#define M1A2_FLAG_SETOS_CALLED      (1u << 3)
#define M1A2_FLAG_IGPU_RESTORED     (1u << 4)
#define M1A2_FLAG_D2EN_CHANGED      (1u << 5)
#define M1A2_FLAG_FBLOC_WRITTEN     (1u << 6)

#define ACPI_HDR_LEN                4u
#define ACPI_HDR_CSUM               9u
#define ACPI_HDR_OEMTID             16u

#pragma pack(1)
typedef struct {
    char    Signature[8];
    UINT8   Checksum;
    char    OemId[6];
    UINT8   Revision;
    UINT32  RsdtAddress;
    UINT32  Length;
    UINT64  XsdtAddress;
    UINT8   ExtChecksum;
    UINT8   Reserved[3];
} ACPI_RSDP_20;

typedef struct {
    char    Signature[4];
    UINT32  Length;
    UINT8   Revision;
    UINT8   Checksum;
    char    OemId[6];
    char    OemTableId[8];
    UINT32  OemRevision;
    UINT32  CreatorId;
    UINT32  CreatorRevision;
} ACPI_SDT_HDR;

typedef struct {
    UINT64  BaseAddress;
    UINT16  SegmentGroup;
    UINT8   StartBus;
    UINT8   EndBus;
    UINT32  Reserved;
} ACPI_MCFG_ENTRY;

typedef struct {
    UINT16  Type;
    UINT16  Length;
    UINT8   Flags;
    UINT8   Reserved;
    UINT16  Segment;
    UINT64  RegisterBaseAddress;
} ACPI_DMAR_DRHD;

typedef struct {
    UINT32  HostBridge[64]; /* 00:00.0 0x00..0xFC */
    UINT32  Peg0[64];       /* 00:01.0 0x00..0xFC */
    UINT32  Peg0Ecam[4];    /* 0x100, 0x248, 0x444, 0xC74 */
    UINT32  IgpuHdr[16];    /* 00:02.0 0x00..0x3C */
    UINT32  IgpuAsls;       /* 00:02.0 0xFC */
    UINT32  IgpuMmio[4];    /* PWR_WELL_CTL2, DDI_BUF_CTL_A, PP_STATUS, PP_CONTROL */
    UINT32  AmdPci[64];     /* 01:00.0 0x00..0xFC */
    UINT32  AmdAudio[4];    /* 01:00.1 0x00, 0x04, 0x10, 0x14 */
    UINT32  AmdBar5[17];    /* MEMSIZE, APER, BIF_FB, FB_LOC, GRBM, ACCESS, MSG0, RESP0, ARG0, SCRATCH[0..7] */
    UINT32  AmdSmu[15];     /* RST, CLK, MISC, PC1, PC2, RCU, SMU_STS, SMU_FW, FW_FLG, EF49, SRAM[0..3], ProbedFlag */
    UINT32  Gmux[8];        /* ProbeSig, 0x10, 0x14, 0x16, 0x28, 0x40, 0x50, 0x74 */
    UINT64  Iommu[6];       /* DRHD0(GSTS,RTADDR,FSTS), DRHD1(GSTS,RTADDR,FSTS) */
    UINT32  AmdExtra[60];   /* MC_VM[0x0800..0x081F], HDP[0x0B00..0x0B0B], 16x DCE0/SRBM/VGA/VM_CTX */
} M1A2_HW_SNAP;

typedef struct {
    ACPI_SDT_HDR  Hdr;          /* 36 bytes */
    UINT32        Magic;        /* 'M1A2' = 0x3241314D */
    UINT32        Version;      /* 2 */
    UINT32        Mode;         /* 1..5 */
    UINT32        Flags;        /* M1A2_FLAG_* */
    UINT32        EbsCallCount;
    UINT32        EbsRetryCount;
    UINT64        EcamBase;     /* 68 bytes total header+meta */
    M1A2_HW_SNAP  BeforeEbs;    /* 1332 bytes */
    M1A2_HW_SNAP  AfterEbs;     /* 1332 bytes */
    M1A2_HW_SNAP  PostAction;   /* 1332 bytes -> 4064 bytes total */
    UINT8         Pad[4096 - 68 - 3 * sizeof(M1A2_HW_SNAP)];
} M1A2_ACPI_TABLE;
#pragma pack()

_Static_assert(sizeof(M1A2_HW_SNAP) == 1332, "M1A2_HW_SNAP must be 1332 bytes");
_Static_assert(sizeof(M1A2_ACPI_TABLE) == 4096, "M1A2_ACPI_TABLE must be 4096 bytes");

/* ---- Global Static State (valid both before & immediately after ExitBootServices) ---- */

static M1A2_ACPI_TABLE *gTable = NULL;
static EFI_EXIT_BOOT_SERVICES gOrigExitBootServices = NULL;
static UINT64  gEcamBase     = 0;
static UINT8   gEcamStartBus = 0;
static UINT8   gEcamEndBus   = 0;
static BOOLEAN gHasEcam      = FALSE;
static UINT64  gDrhdBase[2]  = { 0xFED90000ULL, 0xFED91000ULL };

/* ---- Freestanding Helpers ---- */

static void mem_zero(void *p, UINTN n) {
    volatile UINT8 *b = (volatile UINT8 *)p;
    for (UINTN i = 0; i < n; i++) b[i] = 0;
}

static void mem_copy(void *dst, const void *src, UINTN n) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    for (UINTN i = 0; i < n; i++) d[i] = s[i];
}

static BOOLEAN guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    const UINT8 *pa = (const UINT8 *)a;
    const UINT8 *pb = (const UINT8 *)b;
    for (UINTN i = 0; i < sizeof(EFI_GUID); i++) {
        if (pa[i] != pb[i]) return FALSE;
    }
    return TRUE;
}

static BOOLEAN ascii_eq_n(const void *a, const char *b, UINTN n) {
    const UINT8 *pa = (const UINT8 *)a;
    for (UINTN i = 0; i < n; i++) {
        if (pa[i] != (UINT8)b[i]) return FALSE;
    }
    return TRUE;
}

static UINT16 rd16(const UINT8 *p) {
    return (UINT16)p[0] | ((UINT16)p[1] << 8);
}

static UINT32 rd32(const UINT8 *p) {
    return (UINT32)p[0]
         | ((UINT32)p[1] << 8)
         | ((UINT32)p[2] << 16)
         | ((UINT32)p[3] << 24);
}

static UINT64 rd64(const UINT8 *p) {
    return (UINT64)rd32(p) | ((UINT64)rd32(p + 4) << 32);
}

static UINT8 sum8(const UINT8 *p, UINTN n) {
    UINT8 s = 0;
    for (UINTN i = 0; i < n; i++) s = (UINT8)(s + p[i]);
    return s;
}

/* ---- Console Output Helpers (Boot Services only) ---- */

static void ConPrintA(SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut, const char *s) {
    CHAR16 buf[128];
    UINTN idx = 0;
    while (*s) {
        if (*s == '\n') {
            buf[idx++] = L'\r';
            if (idx >= 120) {
                buf[idx] = 0;
                ConOut->OutputString(ConOut, buf);
                idx = 0;
            }
        }
        buf[idx++] = (CHAR16)(UINT8)*s++;
        if (idx >= 120) {
            buf[idx] = 0;
            ConOut->OutputString(ConOut, buf);
            idx = 0;
        }
    }
    if (idx > 0) {
        buf[idx] = 0;
        ConOut->OutputString(ConOut, buf);
    }
}

static void ConPrintHex(SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut, UINT64 v, UINTN digits) {
    static const CHAR16 hex[] = L"0123456789ABCDEF";
    CHAR16 buf[20];
    if (digits > 16) digits = 16;
    for (UINTN i = 0; i < digits; i++) {
        UINTN shift = (digits - 1u - i) * 4u;
        buf[i] = hex[(v >> shift) & 0xFu];
    }
    buf[digits] = 0;
    ConOut->OutputString(ConOut, buf);
}

static void ConSetModeText(EFI_BOOT_SERVICES *BS, SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut, BOOLEAN TextMode) {
    EFI_CONSOLE_CONTROL_PROTOCOL *Cc = NULL;
    if (!EFI_ERROR(BS->LocateProtocol(&gConsoleCtrlGuid, NULL, (VOID **)&Cc)) && Cc != NULL) {
        Cc->SetMode(Cc, TextMode ? EfiConsoleControlScreenText : EfiConsoleControlScreenGraphics);
    }
    if (TextMode) {
        ConOut->EnableCursor(ConOut, TRUE);
    }
}

static EFI_STATUS ReadKeyWait(EFI_BOOT_SERVICES *BS, SIMPLE_INPUT_INTERFACE *ConIn, EFI_INPUT_KEY *Key) {
    while (1) {
        UINTN idx = 0;
        EFI_STATUS ws = BS->WaitForEvent(1, &ConIn->WaitForKey, &idx);
        if (EFI_ERROR(ws)) BS->Stall(20000);
        EFI_STATUS rs = ConIn->ReadKeyStroke(ConIn, Key);
        if (!EFI_ERROR(rs)) return rs;
        BS->Stall(20000);
    }
}

static void FlushKeys(SIMPLE_INPUT_INTERFACE *ConIn) {
    EFI_INPUT_KEY k;
    while (!EFI_ERROR(ConIn->ReadKeyStroke(ConIn, &k))) { }
}

/* ---- Pure Hardware I/O & MMIO Primitives (Safe Post-ExitBootServices!) ---- */

static inline UINT8 io_in8(UINT16 port) {
    UINT8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void io_out8(UINT16 port, UINT8 v) {
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline UINT32 io_in32(UINT16 port) {
    UINT32 v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void io_out32(UINT16 port, UINT32 v) {
    __asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(port));
}

static void io_delay_us(UINTN us) {
    for (UINTN i = 0; i < us; i++) {
        (void)io_in8(0x80u);
    }
}

static inline UINT32 mmio_r32(UINT64 addr) {
    return *(volatile UINT32 *)(UINTN)addr;
}

static inline void mmio_w32(UINT64 addr, UINT32 v) {
    *(volatile UINT32 *)(UINTN)addr = v;
}

static inline UINT64 mmio_r64(UINT64 addr) {
    UINT32 lo = mmio_r32(addr);
    UINT32 hi = mmio_r32(addr + 4u);
    return ((UINT64)hi << 32) | (UINT64)lo;
}

static UINT32 pci_rd32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
    UINT32 addr = 0x80000000u
                | ((UINT32)bus << 16)
                | ((UINT32)(dev & 0x1Fu) << 11)
                | ((UINT32)(func & 0x07u) << 8)
                | (UINT32)(off & 0xFCu);
    io_out32(0x0CF8u, addr);
    return io_in32(0x0CFCu);
}

static void pci_wr32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off, UINT32 val) {
    UINT32 addr = 0x80000000u
                | ((UINT32)bus << 16)
                | ((UINT32)(dev & 0x1Fu) << 11)
                | ((UINT32)(func & 0x07u) << 8)
                | (UINT32)(off & 0xFCu);
    io_out32(0x0CF8u, addr);
    io_out32(0x0CFCu, val);
}

static UINT16 pci_rd16(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
    UINT32 v = pci_rd32(bus, dev, func, off);
    return (UINT16)((v >> ((off & 2u) * 8u)) & 0xFFFFu);
}

static UINT8 pci_rd8(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
    UINT32 v = pci_rd32(bus, dev, func, off);
    return (UINT8)((v >> ((off & 3u) * 8u)) & 0xFFu);
}

static UINT32 ecam_rd32(UINT8 bus, UINT8 dev, UINT8 func, UINT16 off) {
    if (!gHasEcam || bus < gEcamStartBus || bus > gEcamEndBus || off >= 0x1000u) {
        return 0xFFFFFFFFu;
    }
    UINT64 addr = gEcamBase
                + ((UINT64)(bus - gEcamStartBus) << 20)
                + ((UINT64)(dev & 0x1Fu) << 15)
                + ((UINT64)(func & 0x07u) << 12)
                + (UINT64)(off & 0xFFCu);
    return mmio_r32(addr);
}

static UINT32 smc_ind_read(UINT64 bar5, UINT32 idx_dword, UINT32 data_dword, UINT32 smc_addr) {
    UINT64 idx_mmio  = bar5 + (UINT64)idx_dword * 4u;
    UINT64 data_mmio = bar5 + (UINT64)data_dword * 4u;
    UINT32 saved = mmio_r32(idx_mmio);
    mmio_w32(idx_mmio, smc_addr);
    UINT32 val = mmio_r32(data_mmio);
    mmio_w32(idx_mmio, saved);
    return val;
}

/* ---- Pure Port-I/O GMUX Read (Zero BS->Stall) ---- */

static BOOLEAN GmuxWaitReadyPure(void) {
    for (UINTN i = 0; i < 200u; i++) {
        UINT8 st = io_in8(GMUX_IO_WRITE);
        if ((st & 0x01u) == 0u) return TRUE;
        (void)io_in8(GMUX_IO_READ);
        io_delay_us(100);
    }
    return FALSE;
}

static BOOLEAN GmuxWaitCompletePure(void) {
    for (UINTN i = 0; i < 200u; i++) {
        UINT8 st = io_in8(GMUX_IO_WRITE);
        if ((st & 0x01u) != 0u) {
            (void)io_in8(GMUX_IO_READ);
            return TRUE;
        }
        io_delay_us(100);
    }
    return FALSE;
}

static UINT32 GmuxRead8Pure(UINT8 port) {
    if (!GmuxWaitReadyPure()) return 0xFFFFFFFFu;
    io_out8(GMUX_IO_READ, port);
    if (!GmuxWaitCompletePure()) return 0xFFFFFFFFu;
    return (UINT32)io_in8(GMUX_IO_VALUE);
}

static UINT32 GmuxRead32Pure(UINT8 port) {
    if (!GmuxWaitReadyPure()) return 0xFFFFFFFFu;
    io_out8(GMUX_IO_READ, port);
    if (!GmuxWaitCompletePure()) return 0xFFFFFFFFu;
    return io_in32(GMUX_IO_VALUE);
}

/* ---- Deterministic Hardware Snapshot (Safe Before AND After ExitBootServices) ---- */

static void CaptureHwSnap(M1A2_HW_SNAP *S) {
    for (UINTN i = 0; i < 64u; i++) {
        S->HostBridge[i] = pci_rd32(0u, 0u, 0u, (UINT8)(i * 4u));
        S->Peg0[i]       = pci_rd32(0u, 1u, 0u, (UINT8)(i * 4u));
    }

    S->Peg0Ecam[0] = ecam_rd32(0u, 1u, 0u, 0x100u);
    S->Peg0Ecam[1] = ecam_rd32(0u, 1u, 0u, 0x248u);
    S->Peg0Ecam[2] = ecam_rd32(0u, 1u, 0u, 0x444u);
    S->Peg0Ecam[3] = ecam_rd32(0u, 1u, 0u, 0xC74u);

    for (UINTN i = 0; i < 16u; i++) {
        S->IgpuHdr[i] = pci_rd32(0u, 2u, 0u, (UINT8)(i * 4u));
    }
    S->IgpuAsls = pci_rd32(0u, 2u, 0u, 0xFCu);

    for (UINTN i = 0; i < 4u; i++) S->IgpuMmio[i] = 0xFFFFFFFFu;
    UINT16 ivid = (UINT16)(S->IgpuHdr[0] & 0xFFFFu);
    UINT16 icmd = (UINT16)(S->IgpuHdr[1] & 0xFFFFu);
    UINT32 ib0  = S->IgpuHdr[4];
    UINT32 ib0h = S->IgpuHdr[5];
    UINT64 igpu_b0 = ((UINT64)ib0h << 32) | (UINT64)(ib0 & ~0xFu);
    if (ivid != 0xFFFFu && (icmd & 0x02u) && (ib0 & 1u) == 0u && igpu_b0 != 0u && igpu_b0 < 0x100000000ULL) {
        S->IgpuMmio[0] = mmio_r32(igpu_b0 + 0x45404u);
        S->IgpuMmio[1] = mmio_r32(igpu_b0 + 0x64000u);
        S->IgpuMmio[2] = mmio_r32(igpu_b0 + 0xC7200u);
        S->IgpuMmio[3] = mmio_r32(igpu_b0 + 0xC7204u);
    }

    for (UINTN i = 0; i < 64u; i++) {
        S->AmdPci[i] = pci_rd32(1u, 0u, 0u, (UINT8)(i * 4u));
    }
    S->AmdAudio[0] = pci_rd32(1u, 0u, 1u, 0x00u);
    S->AmdAudio[1] = pci_rd32(1u, 0u, 1u, 0x04u);
    S->AmdAudio[2] = pci_rd32(1u, 0u, 1u, 0x10u);
    S->AmdAudio[3] = pci_rd32(1u, 0u, 1u, 0x14u);

    for (UINTN i = 0; i < 17u; i++) S->AmdBar5[i] = 0xFFFFFFFFu;
    for (UINTN i = 0; i < 15u; i++) S->AmdSmu[i]  = 0xFFFFFFFFu;
    for (UINTN i = 0; i < 60u; i++) S->AmdExtra[i] = 0xFFFFFFFFu;
    S->AmdSmu[14] = 0u; /* ProbedFlag */

    UINT16 avid    = (UINT16)(S->AmdPci[0] & 0xFFFFu);
    UINT16 acmd    = (UINT16)(S->AmdPci[1] & 0xFFFFu);
    UINT16 asts    = (UINT16)((S->AmdPci[1] >> 16) & 0xFFFFu);
    UINT16 peg_cmd = (UINT16)(S->Peg0[1] & 0xFFFFu);
    UINT32 bar5    = S->AmdPci[9];
    UINT32 bar5_base = bar5 & ~0xFu;

    /* Find PMCSR safely */
    UINT32 pmcsr = 0xFFFFFFFFu;
    if (avid == 0x1002u && (asts & 0x10u)) {
        UINT8 cap = pci_rd8(1u, 0u, 0u, 0x34u) & 0xFCu;
        for (UINTN g = 0; g < 32u && cap >= 0x40u; g++) {
            UINT8 id  = pci_rd8(1u, 0u, 0u, cap);
            UINT8 nxt = pci_rd8(1u, 0u, 0u, (UINT8)(cap + 1u)) & 0xFCu;
            if (id == 0x01u) {
                pmcsr = pci_rd32(1u, 0u, 0u, (UINT8)(cap + 4u));
                break;
            }
            if (nxt == 0u || nxt == cap) break;
            cap = nxt;
        }
    }

    if (avid == 0x1002u && (acmd & 0x02u) && (peg_cmd & 0x02u) &&
        (pmcsr & 0x03u) == 0u && (bar5 & 1u) == 0u &&
        bar5_base != 0u && bar5_base < 0xF0000000u) {
        UINT64 b5 = (UINT64)bar5_base;
        UINT32 cfg_mem = mmio_r32(b5 + mmCONFIG_MEMSIZE * 4u);
        UINT32 grbm    = mmio_r32(b5 + mmGRBM_STATUS * 4u);

        S->AmdBar5[0] = cfg_mem;
        S->AmdBar5[1] = mmio_r32(b5 + mmCONFIG_APER_SIZE * 4u);
        S->AmdBar5[2] = mmio_r32(b5 + mmBIF_FB_EN * 4u);
        S->AmdBar5[3] = mmio_r32(b5 + mmMC_VM_FB_LOCATION * 4u);
        S->AmdBar5[4] = grbm;
        S->AmdBar5[5] = mmio_r32(b5 + mmSMC_IND_ACCESS_CNTL * 4u);
        S->AmdBar5[6] = mmio_r32(b5 + mmSMC_MESSAGE_0 * 4u);
        S->AmdBar5[7] = mmio_r32(b5 + mmSMC_RESP_0 * 4u);
        S->AmdBar5[8] = mmio_r32(b5 + mmSMC_MSG_ARG_0 * 4u);
        for (UINTN i = 0; i < 8u; i++) {
            S->AmdBar5[9u + i] = mmio_r32(b5 + (mmBIOS_SCRATCH_0 + (UINT32)i) * 4u);
        }

        for (UINTN i = 0; i < 32u; i++) {
            S->AmdExtra[i] = mmio_r32(b5 + (0x0800u + (UINT32)i) * 4u);
        }
        for (UINTN i = 0; i < 12u; i++) {
            S->AmdExtra[32u + i] = mmio_r32(b5 + (0x0B00u + (UINT32)i) * 4u);
        }
        static const UINT32 extra_offs[16] = {
            0x00CCu, 0x00CEu, 0x0393u, 0x0394u,
            0x0B8Eu, 0x0CB2u, 0x1A00u, 0x1A01u,
            0x1A04u, 0x1A05u, 0x1A06u, 0x1A07u,
            0x1A08u, 0x1B9Cu, 0x0528u, 0x0529u
        };
        for (UINTN i = 0; i < 16u; i++) {
            S->AmdExtra[44u + i] = mmio_r32(b5 + extra_offs[i] * 4u);
        }

        if (cfg_mem != 0xFFFFFFFFu && grbm != 0xFFFFFFFFu) {
            S->AmdSmu[0]  = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMC_SYSCON_RESET_CNTL);
            S->AmdSmu[1]  = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMC_SYSCON_CLOCK_CNTL_0);
            S->AmdSmu[2]  = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMC_SYSCON_MISC_CNTL);
            S->AmdSmu[3]  = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMC_PC_C);
            io_delay_us(500);
            S->AmdSmu[4]  = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMC_PC_C);
            S->AmdSmu[5]  = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixRCU_UC_EVENTS);
            S->AmdSmu[6]  = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMU_STATUS);
            S->AmdSmu[7]  = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMU_FIRMWARE);
            S->AmdSmu[8]  = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixFIRMWARE_FLAGS);
            S->AmdSmu[9]  = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMU_EFUSE_49);
            for (UINTN i = 0; i < 4u; i++) {
                S->AmdSmu[10u + i] = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, 0x20000u + (UINT32)i * 4u);
            }
            S->AmdSmu[14] = 1u;
        }
    }

    io_out8(GMUX_IO_PROBE0, 0xAAu);
    io_out8(GMUX_IO_PROBE1, 0x55u);
    io_out8(GMUX_IO_PROBE2, 0x00u);
    UINT16 sig = (UINT16)io_in8(GMUX_IO_PROBE0) | ((UINT16)io_in8(GMUX_IO_PROBE1) << 8);
    S->Gmux[0] = (UINT32)sig;
    if (sig == 0x55AAu) {
        S->Gmux[1] = GmuxRead8Pure(0x10u);
        S->Gmux[2] = GmuxRead8Pure(0x14u);
        S->Gmux[3] = GmuxRead8Pure(0x16u);
        S->Gmux[4] = GmuxRead8Pure(0x28u);
        S->Gmux[5] = GmuxRead8Pure(0x40u);
        S->Gmux[6] = GmuxRead8Pure(0x50u);
        S->Gmux[7] = GmuxRead32Pure(0x74u);
    } else {
        for (UINTN i = 1; i < 8u; i++) S->Gmux[i] = 0xFFFFFFFFu;
    }

    for (UINTN i = 0; i < 2u; i++) {
        UINT64 b = gDrhdBase[i];
        if (b != 0ULL && b < 0x100000000ULL) {
            S->Iommu[i * 3u + 0u] = (UINT64)mmio_r32(b + 0x1Cu);
            S->Iommu[i * 3u + 1u] = mmio_r64(b + 0x20u);
            S->Iommu[i * 3u + 2u] = (UINT64)mmio_r32(b + 0x34u);
        } else {
            S->Iommu[i * 3u + 0u] = 0xFFFFFFFFFFFFFFFFULL;
            S->Iommu[i * 3u + 1u] = 0xFFFFFFFFFFFFFFFFULL;
            S->Iommu[i * 3u + 2u] = 0xFFFFFFFFFFFFFFFFULL;
        }
    }
}

/* ---- Hooked BS->ExitBootServices (CRITICAL: MS ABI EFIAPI) ---- */

static void UpdateTableChecksum(M1A2_ACPI_TABLE *T) {
    T->Hdr.Checksum = 0u;
    T->Hdr.Checksum = (UINT8)(0u - sum8((const UINT8 *)T, T->Hdr.Length));
}

static EFI_STATUS EFIAPI HookedExitBootServices(EFI_HANDLE ImageHandle, UINTN MapKey) {
    if (gTable != NULL) {
        gTable->EbsCallCount++;
        if ((gTable->Flags & M1A2_FLAG_BEFORE_EBS) == 0u) {
            CaptureHwSnap(&gTable->BeforeEbs);
            gTable->Flags |= M1A2_FLAG_BEFORE_EBS;
            UpdateTableChecksum(gTable);
        }
    }

    EFI_STATUS st = gOrigExitBootServices(ImageHandle, MapKey);
    if (EFI_ERROR(st)) {
        if (gTable != NULL) {
            gTable->EbsRetryCount++;
            UpdateTableChecksum(gTable);
        }
        return st;
    }

    /* NOTE: Boot Services are GONE now! Only raw PCI / MMIO / I/O and gTable! */
    if (gTable != NULL) {
        CaptureHwSnap(&gTable->AfterEbs);
        gTable->Flags |= M1A2_FLAG_AFTER_EBS;

        if (gTable->Mode == M1A2_MODE_SETOS_HIDE_IGPU) {
            UINT32 deven = pci_rd32(0u, 0u, 0u, 0x54u);
            if ((deven & (1u << 4)) != 0u) {
                pci_wr32(0u, 0u, 0u, 0x54u, deven & ~(1u << 4));
                UINT32 deven_rb = pci_rd32(0u, 0u, 0u, 0x54u);
                if ((deven_rb & (1u << 4)) == 0u) {
                    gTable->Flags |= M1A2_FLAG_D2EN_CHANGED;
                }
            }
            CaptureHwSnap(&gTable->PostAction);
            gTable->Flags |= M1A2_FLAG_POST_ACTION;
        } else if (gTable->Mode == M1A2_MODE_NORMAL_UNHIDE_IGPU) {
            UINT32 deven = pci_rd32(0u, 0u, 0u, 0x54u);
            if ((deven & (1u << 4)) == 0u) {
                pci_wr32(0u, 0u, 0u, 0x54u, deven | (1u << 4));
                UINT32 deven_rb = pci_rd32(0u, 0u, 0u, 0x54u);
                if ((deven_rb & (1u << 4)) != 0u) {
                    gTable->Flags |= M1A2_FLAG_D2EN_CHANGED;
                }
            }

            if (pci_rd16(0u, 2u, 0u, 0x00u) == 0x8086u) {
                static const UINT8 restore_offs[] = { 0x10u, 0x14u, 0x18u, 0x1Cu, 0x20u, 0xFCu, 0x04u };
                BOOLEAN restored = FALSE;
                for (UINTN i = 0; i < sizeof(restore_offs); i++) {
                    UINT8 off = restore_offs[i];
                    UINT32 want = (off == 0xFCu) ? gTable->BeforeEbs.IgpuAsls : gTable->BeforeEbs.IgpuHdr[off / 4u];
                    UINT32 cur  = pci_rd32(0u, 2u, 0u, off);
                    if (cur != want && want != 0xFFFFFFFFu && want != 0u) {
                        pci_wr32(0u, 2u, 0u, off, want);
                        restored = TRUE;
                    }
                }
                if (restored) {
                    gTable->Flags |= M1A2_FLAG_IGPU_RESTORED;
                }
            }
            CaptureHwSnap(&gTable->PostAction);
            gTable->Flags |= M1A2_FLAG_POST_ACTION;
        } else if (gTable->Mode == M1A2_MODE_SETOS_FIX_FBLOC) {
            UINT16 avid    = pci_rd16(1u, 0u, 0u, 0x00u);
            UINT16 acmd    = pci_rd16(1u, 0u, 0u, 0x04u);
            UINT16 peg_cmd = pci_rd16(0u, 1u, 0u, 0x04u);
            UINT32 bar5    = pci_rd32(1u, 0u, 0u, 0x24u);
            UINT32 b5      = bar5 & ~0xFu;
            if (avid == 0x1002u && (acmd & 0x02u) && (peg_cmd & 0x02u) &&
                (bar5 & 1u) == 0u && b5 != 0u && b5 < 0xF0000000u) {
                UINT32 surf_lo = mmio_r32((UINT64)b5 + 0x1A04u * 4u);
                mmio_w32((UINT64)b5 + mmMC_VM_FB_LOCATION * 4u, 0xF47FF400u);
                mmio_w32((UINT64)b5 + 0x0B01u * 4u, 0xF4000000u);
                mmio_w32((UINT64)b5 + 0x1A07u * 4u, 0x000000F4u);
                mmio_w32((UINT64)b5 + 0x1A04u * 4u, surf_lo);
                io_delay_us(500);
                UINT32 rb_fb  = mmio_r32((UINT64)b5 + mmMC_VM_FB_LOCATION * 4u);
                UINT32 rb_hdp = mmio_r32((UINT64)b5 + 0x0B01u * 4u);
                UINT32 rb_dce = mmio_r32((UINT64)b5 + 0x1A07u * 4u);
                if (rb_fb == 0xF47FF400u && rb_hdp == 0xF4000000u &&
                    rb_dce == 0x000000F4u) {
                    gTable->Flags |= M1A2_FLAG_FBLOC_WRITTEN;
                }
            }
            CaptureHwSnap(&gTable->PostAction);
            gTable->Flags |= M1A2_FLAG_POST_ACTION;
        }

        UpdateTableChecksum(gTable);
    }

    return EFI_SUCCESS;
}

/* ---- Install "M1A2" ACPI Table & Discover ECAM/DMAR ---- */

static EFI_STATUS InstallM1A2AcpiTable(EFI_BOOT_SERVICES *BS, EFI_SYSTEM_TABLE *ST,
                                        SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut, UINT32 Mode) {
    ACPI_RSDP_20 *rsdp = NULL;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        if (guid_eq(&ST->ConfigurationTable[i].VendorGuid, &gAcpi20Guid)) {
            rsdp = (ACPI_RSDP_20 *)ST->ConfigurationTable[i].VendorTable;
            break;
        }
    }
    if (!rsdp || !ascii_eq_n(rsdp->Signature, "RSD PTR ", 8u) ||
        rsdp->Revision < 2u || rsdp->XsdtAddress == 0u) {
        ConPrintA(ConOut, "ERROR: ACPI 2.0 RSDP/XSDT not found\n");
        return EFI_UNSUPPORTED;
    }

    UINT8 *xsdt = (UINT8 *)(UINTN)rsdp->XsdtAddress;
    UINT32 xlen = rd32(xsdt + ACPI_HDR_LEN);
    if (!ascii_eq_n(xsdt, "XSDT", 4u) || xlen < sizeof(ACPI_SDT_HDR) || sum8(xsdt, xlen) != 0u) {
        ConPrintA(ConOut, "ERROR: Invalid XSDT\n");
        return EFI_COMPROMISED_DATA;
    }

    UINTN n_entries = (xlen - sizeof(ACPI_SDT_HDR)) / 8u;
    UINTN drhd_idx = 0;
    for (UINTN i = 0; i < n_entries; i++) {
        const UINT8 *t = (const UINT8 *)(UINTN)rd64(xsdt + sizeof(ACPI_SDT_HDR) + i * 8u);
        if (!t) continue;
        const ACPI_SDT_HDR *hdr = (const ACPI_SDT_HDR *)t;
        if (ascii_eq_n(hdr->Signature, "M1A2", 4u)) {
            ConPrintA(ConOut, "ERROR: M1A2 table already in XSDT\n");
            return EFI_ALREADY_STARTED;
        }
        if (ascii_eq_n(hdr->Signature, "MCFG", 4u) && hdr->Length >= sizeof(ACPI_SDT_HDR) + 8u + sizeof(ACPI_MCFG_ENTRY)) {
            const ACPI_MCFG_ENTRY *me = (const ACPI_MCFG_ENTRY *)(t + sizeof(ACPI_SDT_HDR) + 8u);
            if (me->BaseAddress >= 0xC0000000ULL && me->BaseAddress < 0x100000000ULL) {
                gEcamBase     = me->BaseAddress;
                gEcamStartBus = me->StartBus;
                gEcamEndBus   = me->EndBus;
                gHasEcam      = TRUE;
            }
        } else if (ascii_eq_n(hdr->Signature, "DMAR", 4u) && hdr->Length > sizeof(ACPI_SDT_HDR) + 12u) {
            const UINT8 *p   = t + sizeof(ACPI_SDT_HDR) + 12u;
            const UINT8 *end = t + hdr->Length;
            while (p + 4u <= end) {
                UINT16 stype = rd16(p);
                UINT16 slen  = rd16(p + 2u);
                if (slen < 4u || p + slen > end) break;
                if (stype == 0u && slen >= sizeof(ACPI_DMAR_DRHD) && drhd_idx < 2u) {
                    const ACPI_DMAR_DRHD *d = (const ACPI_DMAR_DRHD *)p;
                    gDrhdBase[drhd_idx++] = d->RegisterBaseAddress;
                }
                p += slen;
            }
        }
    }

    M1A2_ACPI_TABLE *tbl = NULL;
    UINT8 *new_xsdt = NULL;
    ACPI_RSDP_20 *new_rsdp = NULL;
    UINTN rlen = (rsdp->Length < sizeof(ACPI_RSDP_20)) ? sizeof(ACPI_RSDP_20) : rsdp->Length;

    if (EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, sizeof(M1A2_ACPI_TABLE), (VOID **)&tbl)) ||
        EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, (UINTN)xlen + 8u, (VOID **)&new_xsdt)) ||
        EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, rlen, (VOID **)&new_rsdp))) {
        ConPrintA(ConOut, "ERROR: AllocatePool(ACPIReclaim) failed\n");
        if (tbl) BS->FreePool(tbl);
        if (new_xsdt) BS->FreePool(new_xsdt);
        return EFI_OUT_OF_RESOURCES;
    }

    mem_zero(tbl, sizeof(*tbl));
    mem_copy(tbl->Hdr.Signature, "M1A2", 4u);
    tbl->Hdr.Length          = sizeof(M1A2_ACPI_TABLE);
    tbl->Hdr.Revision        = 1u;
    mem_copy(tbl->Hdr.OemId, "MBP133", 6u);
    mem_copy(tbl->Hdr.OemTableId, "EBSPROBE", 8u);
    tbl->Hdr.OemRevision     = 1u;
    mem_copy(&tbl->Hdr.CreatorId, "1A2E", 4u);
    tbl->Hdr.CreatorRevision = 1u;
    tbl->Magic               = M1A2_MAGIC;
    tbl->Version             = M1A2_VERSION;
    tbl->Mode                = Mode;
    tbl->EcamBase            = gEcamBase;
    UpdateTableChecksum(tbl);

    mem_copy(new_xsdt, xsdt, xlen);
    UINT64 tbl_pa = (UINT64)(UINTN)tbl;
    mem_copy(new_xsdt + xlen, &tbl_pa, 8u);
    UINT32 new_xlen = xlen + 8u;
    mem_copy(new_xsdt + ACPI_HDR_LEN, &new_xlen, 4u);
    new_xsdt[ACPI_HDR_CSUM] = 0u;
    new_xsdt[ACPI_HDR_CSUM] = (UINT8)(0u - sum8(new_xsdt, new_xlen));

    mem_copy(new_rsdp, rsdp, rlen);
    new_rsdp->XsdtAddress = (UINT64)(UINTN)new_xsdt;
    new_rsdp->ExtChecksum = 0u;
    new_rsdp->ExtChecksum = (UINT8)(0u - sum8((const UINT8 *)new_rsdp, new_rsdp->Length));

    EFI_STATUS st = BS->InstallConfigurationTable(&gAcpi20Guid, new_rsdp);
    if (EFI_ERROR(st)) {
        ConPrintA(ConOut, "ERROR: InstallConfigurationTable failed: 0x");
        ConPrintHex(ConOut, st, 16);
        ConPrintA(ConOut, "\n");
        BS->FreePool(tbl);
        BS->FreePool(new_xsdt);
        BS->FreePool(new_rsdp);
        return st;
    }

    volatile ACPI_RSDP_20 *old_rsdp = (volatile ACPI_RSDP_20 *)rsdp;
    UINT64 saved_x = old_rsdp->XsdtAddress;
    UINT8  saved_e = old_rsdp->ExtChecksum;
    old_rsdp->XsdtAddress = new_rsdp->XsdtAddress;
    old_rsdp->ExtChecksum = new_rsdp->ExtChecksum;
    if (old_rsdp->XsdtAddress != new_rsdp->XsdtAddress) {
        old_rsdp->XsdtAddress = saved_x;
        old_rsdp->ExtChecksum = saved_e;
    }

    gTable = tbl;
    return EFI_SUCCESS;
}

/* ---- AppleSetOs Invocation ---- */

static EFI_STATUS InvokeAppleSetOs(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle,
                                   SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut) {
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    EFI_STATUS st = BS->LocateHandleBuffer(ByProtocol, &gAppleSetOsGuid, NULL, &count, &handles);
    if (EFI_ERROR(st) || !handles || count == 0u) {
        ConPrintA(ConOut, "ERROR: AppleSetOs protocol not found\n");
        return EFI_ERROR(st) ? st : EFI_NOT_FOUND;
    }

    EFI_STATUS final_st = EFI_SUCCESS;
    for (UINTN i = 0; i < count; i++) {
        EFI_APPLE_SET_OS_PROTOCOL *p = NULL;
        st = BS->OpenProtocol(handles[i], &gAppleSetOsGuid, (VOID **)&p,
                              ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(st) || !p) {
            final_st = st;
            continue;
        }
        if (p->Version != 0u) {
            EFI_STATUS sv = p->SetOsVendor((CHAR8 *)"Apple Inc.");
            EFI_STATUS vr = p->SetOsVersion((CHAR8 *)"Mac OS X 10.15");
            if (EFI_ERROR(sv)) final_st = sv;
            if (EFI_ERROR(vr)) final_st = vr;
        }
    }
    BS->FreePool(handles);
    return final_st;
}

/* ---- Build Clean File Device Path & Chainload Windows bootmgfw.efi ---- */

static UINTN DevicePathLen(const EFI_DEVICE_PATH_PROTOCOL *Dp) {
    UINTN total = 0;
    const UINT8 *p = (const UINT8 *)Dp;
    while (p != NULL) {
        UINT8 type = p[0];
        UINT8 sub  = p[1];
        UINT16 len = (UINT16)p[2] | ((UINT16)p[3] << 8);
        if (len < 4u) break;
        total += len;
        if (type == 0x7Fu && sub == 0xFFu) break;
        p += len;
    }
    return total;
}

static EFI_DEVICE_PATH_PROTOCOL *BuildFileDevicePath(EFI_BOOT_SERVICES *BS,
                                                     const EFI_DEVICE_PATH_PROTOCOL *DevDp,
                                                     const CHAR16 *FilePathStr) {
    UINTN dev_len = DevicePathLen(DevDp);
    if (dev_len < 4u) return NULL;
    UINTN prefix_len = dev_len - 4u;

    UINTN chars = 0;
    while (FilePathStr[chars]) chars++;
    UINTN fp_node_len = 4u + (chars + 1u) * sizeof(CHAR16);
    UINTN total = prefix_len + fp_node_len + 4u;

    UINT8 *buf = NULL;
    if (EFI_ERROR(BS->AllocatePool(EfiBootServicesData, total, (VOID **)&buf)) || !buf) return NULL;
    mem_zero(buf, total);
    mem_copy(buf, DevDp, prefix_len);

    UINT8 *fn = buf + prefix_len;
    fn[0] = MEDIA_DEVICE_PATH;
    fn[1] = MEDIA_FILEPATH_DP;
    fn[2] = (UINT8)(fp_node_len & 0xFFu);
    fn[3] = (UINT8)((fp_node_len >> 8) & 0xFFu);
    mem_copy(fn + 4u, FilePathStr, (chars + 1u) * sizeof(CHAR16));

    UINT8 *end = fn + fp_node_len;
    end[0] = END_DEVICE_PATH_TYPE;
    end[1] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    end[2] = 4u;
    end[3] = 0u;

    return (EFI_DEVICE_PATH_PROTOCOL *)buf;
}

static EFI_STATUS ChainloadWindows(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle,
                                   SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut) {
    static CHAR16 WinPath[] = L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    EFI_STATUS st = BS->LocateHandleBuffer(ByProtocol, &gSimpleFsGuid, NULL, &count, &handles);
    if (EFI_ERROR(st) || !handles) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;

    EFI_HANDLE win_dev = NULL;
    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        EFI_FILE_HANDLE root = NULL, f = NULL;
        if (EFI_ERROR(BS->HandleProtocol(handles[i], &gSimpleFsGuid, (VOID **)&fs)) || !fs) continue;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;
        if (!EFI_ERROR(root->Open(root, &f, WinPath, EFI_FILE_MODE_READ, 0)) && f != NULL) {
            f->Close(f);
            root->Close(root);
            win_dev = handles[i];
            break;
        }
        root->Close(root);
    }
    BS->FreePool(handles);

    if (!win_dev) {
        ConPrintA(ConOut, "\nERROR: \\EFI\\Microsoft\\Boot\\bootmgfw.efi not found!\n");
        return EFI_NOT_FOUND;
    }

    EFI_DEVICE_PATH_PROTOCOL *dev_dp = NULL;
    st = BS->HandleProtocol(win_dev, &gDevicePathGuid, (VOID **)&dev_dp);
    if (EFI_ERROR(st) || !dev_dp) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;

    EFI_DEVICE_PATH_PROTOCOL *full_dp = BuildFileDevicePath(BS, dev_dp, WinPath);
    if (!full_dp) return EFI_OUT_OF_RESOURCES;

    EFI_HANDLE win_img = NULL;
    st = BS->LoadImage(FALSE, ImageHandle, full_dp, NULL, 0, &win_img);
    BS->FreePool(full_dp);
    if (EFI_ERROR(st)) {
        ConPrintA(ConOut, "\nERROR: LoadImage(bootmgfw.efi) failed: 0x");
        ConPrintHex(ConOut, st, 16);
        ConPrintA(ConOut, "\n");
        return st;
    }

    ConOut->ClearScreen(ConOut);
    ConSetModeText(BS, ConOut, FALSE);
    st = BS->StartImage(win_img, NULL, NULL);
    ConSetModeText(BS, ConOut, TRUE);
    ConPrintA(ConOut, "\nERROR: StartImage(bootmgfw.efi) returned: 0x");
    ConPrintHex(ConOut, st, 16);
    ConPrintA(ConOut, "\n");
    return st;
}

/* ---- Mode Execution ---- */

static void RunM1A2Mode(EFI_BOOT_SERVICES *BS, EFI_SYSTEM_TABLE *ST,
                        EFI_HANDLE ImageHandle, UINT32 Mode) {
    SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut = ST->ConOut;
    SIMPLE_INPUT_INTERFACE *ConIn = ST->ConIn;
    BOOLEAN call_setos = (Mode == M1A2_MODE_SETOS_XRAY ||
                          Mode == M1A2_MODE_SETOS_HIDE_IGPU ||
                          Mode == M1A2_MODE_SETOS_FIX_FBLOC);

    ConOut->ClearScreen(ConOut);
    ConPrintA(ConOut, "========================================================================\n");
    ConPrintA(ConOut, "  Milestone 1A-2 Mode ");
    ConPrintHex(ConOut, Mode, 1);
    ConPrintA(ConOut, " — Installing M1A2 ACPI table & EBS hook...\n");
    ConPrintA(ConOut, "========================================================================\n\n");

    EFI_STATUS st = InstallM1A2AcpiTable(BS, ST, ConOut, Mode);
    if (EFI_ERROR(st)) {
        ConPrintA(ConOut, "\nAborted. Press any key...\n");
        FlushKeys(ConIn);
        EFI_INPUT_KEY k;
        ReadKeyWait(BS, ConIn, &k);
        return;
    }

    if (call_setos) {
        ConPrintA(ConOut, "Calling AppleSetOs(\"Mac OS X 10.15\")...\n");
        EFI_STATUS sst = InvokeAppleSetOs(BS, ImageHandle, ConOut);
        if (!EFI_ERROR(sst)) {
            gTable->Flags |= M1A2_FLAG_SETOS_CALLED;
            UpdateTableChecksum(gTable);
        }
    } else {
        ConPrintA(ConOut, "AppleSetOs NOT called (Normal Boot Camp path).\n");
    }

    /* Install ExitBootServices hook and update BootServices CRC32 */
    gOrigExitBootServices = BS->ExitBootServices;
    BS->ExitBootServices  = HookedExitBootServices;
    BS->Hdr.CRC32         = 0u;
    BS->CalculateCrc32((VOID *)&BS->Hdr, BS->Hdr.HeaderSize, &BS->Hdr.CRC32);

    ConPrintA(ConOut, "M1A2 ACPI table @ 0x");
    ConPrintHex(ConOut, (UINT64)(UINTN)gTable, 8);
    ConPrintA(ConOut, " | Hook installed -> Booting Windows...\n");
    BS->Stall(300000);

    st = ChainloadWindows(BS, ImageHandle, ConOut);
    /* Restore ExitBootServices if chainload failed before EBS */
    if (gOrigExitBootServices != NULL && BS->ExitBootServices == HookedExitBootServices) {
        BS->ExitBootServices = gOrigExitBootServices;
        BS->Hdr.CRC32        = 0u;
        BS->CalculateCrc32((VOID *)&BS->Hdr, BS->Hdr.HeaderSize, &BS->Hdr.CRC32);
    }
    if (EFI_ERROR(st)) {
        ConPrintA(ConOut, "Press any key to return to menu...\n");
        FlushKeys(ConIn);
        EFI_INPUT_KEY k;
        ReadKeyWait(BS, ConIn, &k);
    }
}

/* ---- UEFI Entry Point ---- */

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_BOOT_SERVICES *BS = SystemTable->BootServices;
    EFI_RUNTIME_SERVICES *RT = SystemTable->RuntimeServices;
    SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut = SystemTable->ConOut;
    SIMPLE_INPUT_INTERFACE *ConIn = SystemTable->ConIn;

    ConSetModeText(BS, ConOut, TRUE);

    while (1) {
        ConOut->ClearScreen(ConOut);
        ConPrintA(ConOut,
            "========================================================================\n"
            "  MacBookPro13,3 Milestone 1A-2 — ExitBootServices X-Ray & Dual-GPU\n"
            "  Captures HW before+after Apple EFI ExitBootServices into ACPI 'M1A2'\n"
            "========================================================================\n\n"
            "  [1] Normal (NO AppleSetOs) + EBS X-Ray Capture\n"
            "  [2] SetOS 10.15 + EBS X-Ray Capture\n"
            "  [3] SetOS 10.15 + Hide 00:02.0 (D2EN=0) Post-EBS  [Cause A vs B]\n"
            "  [4] Normal + Unhide 00:02.0 (D2EN=1 + BARs) Post-EBS [Locked by HB80]\n"
            "  [5] SetOS 10.15 + 3-Reg FB Fix (0x809+0xB01+0x1A07) [Dual-GPU Fix]\n"
            "  [0] Cancel / Normal Windows Boot (no hook, zero changes)\n"
            "  [R] Cold Reboot\n\n"
            "Select option: ");

        FlushKeys(ConIn);
        EFI_INPUT_KEY k;
        mem_zero(&k, sizeof(k));
        ReadKeyWait(BS, ConIn, &k);
        CHAR16 c = k.UnicodeChar;

        if (c == L'1') {
            RunM1A2Mode(BS, SystemTable, ImageHandle, M1A2_MODE_NORMAL_XRAY);
        } else if (c == L'2') {
            RunM1A2Mode(BS, SystemTable, ImageHandle, M1A2_MODE_SETOS_XRAY);
        } else if (c == L'3') {
            RunM1A2Mode(BS, SystemTable, ImageHandle, M1A2_MODE_SETOS_HIDE_IGPU);
        } else if (c == L'4') {
            RunM1A2Mode(BS, SystemTable, ImageHandle, M1A2_MODE_NORMAL_UNHIDE_IGPU);
        } else if (c == L'5') {
            RunM1A2Mode(BS, SystemTable, ImageHandle, M1A2_MODE_SETOS_FIX_FBLOC);
        } else if (c == L'0') {
            ConPrintA(ConOut, "\nNormal Windows Boot (no hook, zero changes)...\n");
            EFI_STATUS bst = ChainloadWindows(BS, ImageHandle, ConOut);
            if (EFI_ERROR(bst)) {
                FlushKeys(ConIn);
                ReadKeyWait(BS, ConIn, &k);
            }
        } else if (c == L'r' || c == L'R') {
            ConPrintA(ConOut, "\nRebooting...\n");
            BS->Stall(300000);
            RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
        } else if (k.ScanCode == 0x17u) {
            break;
        }
    }

    return EFI_SUCCESS;
}

/* gnu-efi 4.0.0 crt0-efi-x86_64.o invokes _entry(ImageHandle, SystemTable) */
EFI_STATUS _entry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    return efi_main(ImageHandle, SystemTable);
}
