/*
 * mbp133_m2_hybrid.c - Milestone 2 Unified Hybrid Dual-GPU Loader (MacBookPro13,3)
 * Target: Skylake i7-6920HQ + Intel HD 530 (00:02.0) + AMD Radeon Pro 455 (01:00.0)
 *
 * Combines Milestone 1B (Intel HD 530 primary Retina panel via Indexed I/O GMUX,
 * DDI_BUF_CTL_A 4-lane strap, BDB[43]=NONE backlight, and SSDT_IGPU_MBP133.aml)
 * with Milestone 1A-2 (post-ExitBootServices 3-register Polaris 11 VRAM relocation:
 *   MC_VM_FB_LOCATION=0xF47FF400, HDP_NONSURFACE_BASE=0xF4000000,
 *   GRPH_PRIMARY_SURFACE_ADDRESS_HIGH=0xF4 latched via 0x1A04)
 * into a single ACPI "M2HY" + ExitBootServices() hook loader.
 *
 * Interactive modes (or via LoadOptions: mode=1/hybrid, mode=2/igpu, mode=3/amd-dual, mode=4/normal):
 *   [1] Hybrid Mode 5     (Intel Display + AMD On + 3-Reg FB Fix + SSDT)
 *   [2] Intel-Only Mode 4 (Intel Display + AMD Rail OFF + SSDT, ~9.3W)
 *   [3] Dual-GPU AMD Disp (AMD Display + Intel Headless + 3-Reg FB Fix)
 *   [4] Normal Boot Camp  (No SetOS, No GMUX, No SSDT + M2HY X-Ray)
 *   [0] Cancel / Direct Windows Boot (zero changes)
 *
 * SPDX-License-Identifier: MIT
 */

#include <efi.h>
#include <efiapi.h>
#include <eficon.h>
#include <efidef.h>
#include <efidevp.h>
#include <efierr.h>
#include <efipciio.h>
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

static EFI_GUID gConsoleCtrlGuid  = EFI_CONSOLE_CONTROL_PROTOCOL_GUID;
static EFI_GUID gAppleSetOsGuid   = EFI_APPLE_SET_OS_PROTOCOL_GUID;
static EFI_GUID gAcpi20Guid       = EFI_ACPI_20_TABLE_GUID;
static EFI_GUID gLoadedImageGuid  = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID gDevicePathGuid   = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID gSimpleFsGuid     = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID gPciIoGuid        = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID gAppleGpuPrefsGuid = {
    0xfa4ce28d, 0xb62f, 0x4c99, { 0x9c, 0xc3, 0x68, 0x15, 0x68, 0x6e, 0x30, 0xf9 }
};

#define GPU_PREFS_NAME L"gpu-power-prefs"
#define GPU_PREFS_ATTR (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

typedef struct {
    BOOLEAN    Known;
    BOOLEAN    Present;
    UINTN      Size;
    UINT8      Data[4];
    EFI_STATUS Status;
} GPU_PREFS;

/* ---- Pre-T2 Indexed I/O GMUX (Renesas R4F2113 @ 0x700..0x7FE) ---- */

#define GMUX_IO_VALUE               0x07C2u
#define GMUX_IO_PROBE0              0x07CCu
#define GMUX_IO_PROBE1              0x07CDu
#define GMUX_IO_PROBE2              0x07CEu
#define GMUX_IO_READ                0x07D0u
#define GMUX_IO_WRITE               0x07D4u

#define GMUX_PORT_VERSION           0x04u
#define GMUX_PORT_SWITCH_DISPLAY    0x10u
#define GMUX_PORT_IRQ_ENABLE        0x14u
#define GMUX_PORT_IRQ_STATUS        0x16u
#define GMUX_PORT_SWITCH_DDC        0x28u
#define GMUX_PORT_SWITCH_EXTERNAL   0x40u
#define GMUX_PORT_DISCRETE_POWER    0x50u
#define GMUX_PORT_MAX_BRIGHTNESS    0x70u
#define GMUX_PORT_BRIGHTNESS        0x74u

#define GMUX_IRQ_ENABLE_ALL         0xFFu
#define GMUX_IRQ_STATUS_POWER       0x04u

/* ---- Intel Skylake HD 530 (00:02.0) MMIO & OpRegion ---- */

#define IGPU_PCI_CMD                0x04u
#define IGPU_PCI_BAR0               0x10u
#define IGPU_PCI_ASLS               0xFCu

#define PWR_WELL_CTL2               0x45404u
#define PW_PW1_REQ                  (1u << 29)
#define PW_PW1_STATE                (1u << 28)
#define PW_DDI_AE_REQ               (1u << 3)
#define PW_DDI_AE_STATE             (1u << 2)

#define DDI_BUF_CTL_A               0x64000u
#define DDI_A_4_LANES               (1u << 4)
#define DP_AUX_CH_CTL_A             0x64010u
#define DP_AUX_CH_DATA_A(i)         (0x64014u + 4u * (UINT32)(i))

#define AUX_SEND_BUSY               (1u << 31)
#define AUX_DONE                    (1u << 30)
#define AUX_INTERRUPT               (1u << 29)
#define AUX_TIMEOUT_ERR             (1u << 28)
#define AUX_TIMEOUT_MAX             (3u << 26)
#define AUX_RECEIVE_ERR             (1u << 25)
#define AUX_MSG_SIZE(n)             ((UINT32)(n) << 20)
#define AUX_FW_SYNC_SKL             (23u << 5)
#define AUX_SYNC_SKL                31u

#define PP_STATUS                   0xC7200u
#define PP_CONTROL                  0xC7204u
#define PP_ON                       (1u << 31)
#define PP_FORCE_VDD                (1u << 3)
#define PP_UNLOCK                   0xABCD0000u

#define OPREGION_ASLE_OFF           0x100u
#define ASLE_RVDA_OFF               130u
#define ASLE_RVDS_OFF               138u
#define OPREGION_VBT_OFF            0x400u
#define OPREGION_VBT_MAX            0x1800u

/* ---- AMD Polaris 11 (01:00.0) BAR5 & SMU7 Register Offsets (in DWORDs) ---- */

#define mmSMC_IND_INDEX_11          0x01ACu
#define mmSMC_IND_DATA_11           0x01ADu
#define mmMC_VM_FB_LOCATION         0x0809u
#define mmHDP_NONSURFACE_BASE       0x0B01u
#define mmGRPH_PRIMARY_SURF_ADDR    0x1A04u
#define mmGRPH_PRIMARY_SURF_HIGH    0x1A07u
#define mmGRBM_STATUS               0x2004u

#define ixSMC_PC_C                  0x80000370u
#define ixSMU_FIRMWARE              0xE00030A4u

/* ---- Milestone 2 Modes, Flags & ACPI "M2HY" Table Layout ---- */

#define M2_MODE_HYBRID              1u
#define M2_MODE_INTEL_ONLY          2u
#define M2_MODE_AMD_DUAL            3u
#define M2_MODE_NORMAL              4u

#define M2_FLAG_BEFORE_EBS          (1u << 0)
#define M2_FLAG_AFTER_EBS           (1u << 1)
#define M2_FLAG_POST_ACTION         (1u << 2)
#define M2_FLAG_SETOS_OK            (1u << 3)
#define M2_FLAG_GMUX_IGD_OK         (1u << 4)
#define M2_FLAG_GMUX_RAIL_OFF       (1u << 5)
#define M2_FLAG_DDI_4LANES_OK       (1u << 6)
#define M2_FLAG_VBT_BDB43_OK        (1u << 7)
#define M2_FLAG_SSDT_INJECTED       (1u << 8)
#define M2_FLAG_FBLOC_3REG_OK       (1u << 9)
#define M2_FLAG_DDI_POST_EBS_FIX    (1u << 10)
#define M2_FLAG_VFCT_PATCHED        (1u << 11)
#define M2_FLAG_GFX0_DOD_PATCHED    (1u << 12)

#define ACPI_HDR_SIZE               36u
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
    UINT32  HbDeven;    /* 00:00.0 +0x54 */
    UINT32  Hb80;       /* 00:00.0 +0x80 */
    UINT32  PegCmd;     /* 00:01.0 +0x04 */
    UINT32  IgpuVidDid; /* 00:02.0 +0x00 */
    UINT32  IgpuCmd;    /* 00:02.0 +0x04 */
    UINT32  IgpuBar0;   /* 00:02.0 +0x10 */
    UINT32  IgpuAsls;   /* 00:02.0 +0xFC */
    UINT32  IgpuDdiA;   /* BAR0 + 0x64000 */
    UINT32  IgpuPwr2;   /* BAR0 + 0x45404 */
    UINT32  AmdVidDid;  /* 01:00.0 +0x00 */
    UINT32  AmdCmd;     /* 01:00.0 +0x04 */
    UINT32  AmdBar5;    /* 01:00.0 +0x24 */
    UINT32  AmdFbLoc;   /* BAR5 + 0x0809*4 */
    UINT32  AmdHdpBase; /* BAR5 + 0x0B01*4 */
    UINT32  AmdSurfLo;  /* BAR5 + 0x1A04*4 */
    UINT32  AmdSurfHi;  /* BAR5 + 0x1A07*4 */
    UINT32  AmdGrbm;    /* BAR5 + 0x2004*4 */
    UINT32  AmdSmuPc0;  /* ixSMC_PC_C */
    UINT32  AmdSmuFw;   /* ixSMU_FIRMWARE */
    UINT32  GmuxSig;    /* 0x55AA if present */
    UINT32  GmuxDisp;   /* Port 0x10 */
    UINT32  GmuxDdc;    /* Port 0x28 */
    UINT32  GmuxExt;    /* Port 0x40 */
    UINT32  GmuxPwr;    /* Port 0x50 */
    UINT32  GmuxBrt;    /* Port 0x74 */
} M2_HW_SNAP;

typedef struct {
    ACPI_SDT_HDR  Hdr;          /* 36 bytes */
    UINT32        Version;      /* 1 */
    UINT32        Mode;         /* M2_MODE_* */
    UINT32        Flags;        /* M2_FLAG_* */
    UINT32        EbsCallCount;
    UINT32        EbsRetryCount;
    UINT32        Reserved0;
    UINT64        EcamBase;     /* 36 + 32 = 68 bytes header+meta */
    M2_HW_SNAP    BeforeEbs;    /* 100 bytes */
    M2_HW_SNAP    AfterEbs;     /* 100 bytes */
    M2_HW_SNAP    PostAction;   /* 100 bytes -> 368 bytes total */
} M2HY_ACPI_TABLE;
#pragma pack()

_Static_assert(sizeof(ACPI_SDT_HDR) == 36, "ACPI_SDT_HDR must be 36 bytes");
_Static_assert(sizeof(M2_HW_SNAP) == 100, "M2_HW_SNAP must be 100 bytes");
_Static_assert(sizeof(M2HY_ACPI_TABLE) == 368, "M2HY_ACPI_TABLE must be 368 bytes");

/* ---- Global Static State (valid before & immediately after ExitBootServices) ---- */

static M2HY_ACPI_TABLE        *gM2Table = NULL;
static EFI_EXIT_BOOT_SERVICES  gOrigExitBootServices = NULL;
static UINT64                  gEcamBase = 0xE0000000ULL;

/* ---- Logging Buffer ---- */

#define LOG_CAP (128u * 1024u)

typedef struct {
    char   *Buf;
    UINTN   Len;
    UINTN   Cap;
    BOOLEAN Truncated;
} LOG_BUF;

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

static char to_lower_ascii(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static BOOLEAN contains_ci16(const CHAR16 *hay, UINTN nchars, const char *needle) {
    UINTN nlen = 0;
    while (needle[nlen]) nlen++;
    if (nlen == 0u || nchars < nlen) return FALSE;
    for (UINTN i = 0; i + nlen <= nchars; i++) {
        BOOLEAN match = TRUE;
        for (UINTN j = 0; j < nlen; j++) {
            CHAR16 hc = hay[i + j];
            if (hc == 0u || hc > 0x7Fu || to_lower_ascii((char)hc) != to_lower_ascii(needle[j])) {
                match = FALSE;
                break;
            }
        }
        if (match) return TRUE;
    }
    return FALSE;
}

static UINT16 rd16(const UINT8 *p) {
    return (UINT16)p[0] | ((UINT16)p[1] << 8);
}

static void wr16(UINT8 *p, UINT16 v) {
    p[0] = (UINT8)(v & 0xFFu);
    p[1] = (UINT8)((v >> 8) & 0xFFu);
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

static void log_putc(LOG_BUF *L, char c) {
    if (!L || !L->Buf) return;
    if (L->Len + 1u >= L->Cap) {
        L->Truncated = TRUE;
        return;
    }
    L->Buf[L->Len++] = c;
    L->Buf[L->Len] = 0;
}

static void log_str(LOG_BUF *L, const char *s) {
    if (!s) return;
    while (*s) log_putc(L, *s++);
}

static void log_strn(LOG_BUF *L, const char *s, UINTN n) {
    for (UINTN i = 0; i < n; i++) {
        char c = s[i];
        if (c < 0x20 || c > 0x7E) c = '.';
        log_putc(L, c);
    }
}

static void log_nl(LOG_BUF *L) {
    log_str(L, "\r\n");
}

static void log_hex(LOG_BUF *L, UINT64 v, UINTN digits) {
    static const char hex[] = "0123456789ABCDEF";
    for (UINTN i = digits; i > 0u; i--) {
        UINTN shift = (i - 1u) * 4u;
        log_putc(L, hex[(v >> shift) & 0xFu]);
    }
}

static void log_dec(LOG_BUF *L, UINT64 v) {
    char tmp[24];
    UINTN n = 0;
    if (v == 0u) {
        log_putc(L, '0');
        return;
    }
    while (v > 0u && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0u) log_putc(L, tmp[--n]);
}

/* ---- Console Output Helpers (Boot Services only) ---- */

static void ConPrintA(SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut, const char *s) {
    CHAR16 buf[128];
    UINTN idx = 0;
    while (*s) {
        if (*s == '\n') {
            buf[idx++] = L'\r';
            if (idx >= 120u) {
                buf[idx] = 0;
                ConOut->OutputString(ConOut, buf);
                idx = 0;
            }
        }
        buf[idx++] = (CHAR16)(UINT8)*s++;
        if (idx >= 120u) {
            buf[idx] = 0;
            ConOut->OutputString(ConOut, buf);
            idx = 0;
        }
    }
    if (idx > 0u) {
        buf[idx] = 0;
        ConOut->OutputString(ConOut, buf);
    }
}

static void ConPrintHex(SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut, UINT64 v, UINTN digits) {
    static const CHAR16 hex[] = L"0123456789ABCDEF";
    CHAR16 buf[20];
    if (digits > 16u) digits = 16u;
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

static UINT32 pci_rd32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
    UINT32 addr = 0x80000000u
                | ((UINT32)bus << 16)
                | ((UINT32)(dev & 0x1Fu) << 11)
                | ((UINT32)(func & 0x07u) << 8)
                | (UINT32)(off & 0xFCu);
    io_out32(0x0CF8u, addr);
    return io_in32(0x0CFCu);
}

static UINT16 pci_rd16(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
    UINT32 v = pci_rd32(bus, dev, func, off);
    return (UINT16)((v >> ((off & 2u) * 8u)) & 0xFFFFu);
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

/* ---- Pure Port-I/O GMUX Access (Zero BootServices Calls) ---- */

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

/* ---- Boot-Services GMUX Helpers ---- */

static BOOLEAN GmuxWaitReady(EFI_BOOT_SERVICES *BS) {
    for (UINTN i = 0; i < 200u; i++) {
        UINT8 st = io_in8(GMUX_IO_WRITE);
        if ((st & 0x01u) == 0u) return TRUE;
        (void)io_in8(GMUX_IO_READ);
        BS->Stall(100);
    }
    return FALSE;
}

static BOOLEAN GmuxWaitComplete(EFI_BOOT_SERVICES *BS) {
    for (UINTN i = 0; i < 200u; i++) {
        UINT8 st = io_in8(GMUX_IO_WRITE);
        if ((st & 0x01u) != 0u) {
            (void)io_in8(GMUX_IO_READ);
            return TRUE;
        }
        BS->Stall(100);
    }
    return FALSE;
}

static BOOLEAN GmuxDetect(void) {
    io_out8(GMUX_IO_PROBE0, 0xAAu);
    io_out8(GMUX_IO_PROBE1, 0x55u);
    io_out8(GMUX_IO_PROBE2, 0x00u);
    UINT16 sig = (UINT16)io_in8(GMUX_IO_PROBE0) | ((UINT16)io_in8(GMUX_IO_PROBE1) << 8);
    return (sig == 0x55AAu);
}

static BOOLEAN GmuxRead8(EFI_BOOT_SERVICES *BS, UINT8 port, UINT8 *out) {
    if (!GmuxWaitReady(BS)) return FALSE;
    io_out8(GMUX_IO_READ, port);
    if (!GmuxWaitComplete(BS)) return FALSE;
    *out = io_in8(GMUX_IO_VALUE);
    return TRUE;
}

static BOOLEAN GmuxWrite8(EFI_BOOT_SERVICES *BS, UINT8 port, UINT8 val) {
    io_out8(GMUX_IO_VALUE, val);
    if (!GmuxWaitReady(BS)) return FALSE;
    io_out8(GMUX_IO_WRITE, port);
    return GmuxWaitComplete(BS);
}

static BOOLEAN GmuxRead32(EFI_BOOT_SERVICES *BS, UINT8 port, UINT32 *out) {
    if (!GmuxWaitReady(BS)) return FALSE;
    io_out8(GMUX_IO_READ, port);
    if (!GmuxWaitComplete(BS)) return FALSE;
    *out = io_in32(GMUX_IO_VALUE);
    return TRUE;
}

static void LogGmuxState(EFI_BOOT_SERVICES *BS, LOG_BUF *L, const char *Label) {
    log_str(L, "  [GMUX "); log_str(L, Label); log_str(L, "]"); log_nl(L);
    if (!GmuxDetect()) {
        log_str(L, "    GmuxDetect FAILED (not 0x55AA)"); log_nl(L);
        return;
    }
    UINT32 ver = 0, bmax = 0, bcur = 0;
    if (GmuxRead32(BS, GMUX_PORT_VERSION, &ver)) {
        log_str(L, "    VERSION(0x04)=0x"); log_hex(L, ver, 8); log_nl(L);
    }
    static const UINT8 p8[] = { 0x10u, 0x14u, 0x16u, 0x28u, 0x40u, 0x50u };
    static const char n8[][9] = { "DISPLAY", "IRQ_EN", "IRQ_ST", "DDC", "EXT", "DGPU_PWR" };
    for (UINTN i = 0; i < 6u; i++) {
        UINT8 v = 0;
        log_str(L, "    Port 0x"); log_hex(L, p8[i], 2);
        log_str(L, "("); log_str(L, n8[i]); log_str(L, ")=");
        if (GmuxRead8(BS, p8[i], &v)) {
            log_str(L, "0x"); log_hex(L, v, 2);
        } else {
            log_str(L, "TMO");
        }
        log_nl(L);
    }
    if (GmuxRead32(BS, GMUX_PORT_MAX_BRIGHTNESS, &bmax)) {
        log_str(L, "    MAX_BRIGHTNESS=0x"); log_hex(L, bmax, 8); log_nl(L);
    }
    if (GmuxRead32(BS, GMUX_PORT_BRIGHTNESS, &bcur)) {
        log_str(L, "    BRIGHTNESS=0x"); log_hex(L, bcur, 8); log_nl(L);
    }
}

static BOOLEAN GmuxSwitchToIgd(EFI_BOOT_SERVICES *BS, BOOLEAN DoRailOff, BOOLEAN *RailOffDone, LOG_BUF *L) {
    *RailOffDone = FALSE;
    if (!GmuxDetect()) {
        log_str(L, "  GMUX switch ABORTED: probe != 0x55AA"); log_nl(L);
        return FALSE;
    }

    UINT8 orig_mask = 0;
    BOOLEAN mask_valid = GmuxRead8(BS, GMUX_PORT_IRQ_ENABLE, &orig_mask);
    GmuxWrite8(BS, GMUX_PORT_IRQ_ENABLE, GMUX_IRQ_ENABLE_ALL);
    UINT8 st = 0;
    if (GmuxRead8(BS, GMUX_PORT_IRQ_STATUS, &st) && st != 0u) {
        GmuxWrite8(BS, GMUX_PORT_IRQ_STATUS, st);
    }

    BOOLEAN mux_ok = FALSE;
    for (UINTN attempt = 0; attempt < 3u; attempt++) {
        BOOLEAN w1 = GmuxWrite8(BS, GMUX_PORT_SWITCH_DDC, 0x01u);      /* DDC -> IGD */
        BOOLEAN w2 = GmuxWrite8(BS, GMUX_PORT_SWITCH_DISPLAY, 0x02u);  /* Display -> IGD */
        BOOLEAN w3 = GmuxWrite8(BS, GMUX_PORT_SWITCH_EXTERNAL, 0x03u); /* External -> DIS (TB) */
        BS->Stall(10000);
        UINT8 disp = 0xFFu;
        BOOLEAN r1 = GmuxRead8(BS, GMUX_PORT_SWITCH_DISPLAY, &disp);
        log_str(L, "  GMUX switch attempt "); log_dec(L, attempt + 1u);
        log_str(L, ": w=("); log_dec(L, w1); log_putc(L, ','); log_dec(L, w2); log_putc(L, ','); log_dec(L, w3);
        log_str(L, ") rb=");
        if (r1) {
            log_str(L, "0x"); log_hex(L, disp, 2);
        } else {
            log_str(L, "TMO");
        }
        log_nl(L);
        if (w2 && r1 && ((disp & 0x01u) == 0u)) {
            mux_ok = TRUE;
            break;
        }
    }

    if (DoRailOff) {
        if (!mux_ok) {
            log_str(L, "  SAFETY GATE: dGPU rail OFF skipped because panel readback != IGD!"); log_nl(L);
        } else {
            BOOLEAN p1 = GmuxWrite8(BS, GMUX_PORT_DISCRETE_POWER, 0x01u);
            BOOLEAN p2 = GmuxWrite8(BS, GMUX_PORT_DISCRETE_POWER, 0x00u);
            BOOLEAN seen = FALSE;
            for (UINT32 ms = 0; ms < 200u; ms++) {
                UINT8 ist = 0;
                if (GmuxRead8(BS, GMUX_PORT_IRQ_STATUS, &ist) && (ist & GMUX_IRQ_STATUS_POWER)) {
                    GmuxWrite8(BS, GMUX_PORT_IRQ_STATUS, ist);
                    seen = TRUE;
                    break;
                }
                BS->Stall(1000);
            }
            BS->Stall(seen ? 20000 : 250000);
            UINT8 pwr_rb = 0xFFu;
            GmuxRead8(BS, GMUX_PORT_DISCRETE_POWER, &pwr_rb);
            if (pwr_rb == 0x00u) *RailOffDone = TRUE;
            log_str(L, "  GMUX rail OFF: w=("); log_dec(L, p1); log_putc(L, ','); log_dec(L, p2);
            log_str(L, ") pwr_irq="); log_dec(L, seen ? 1u : 0u);
            log_str(L, " 0x50_rb=0x"); log_hex(L, pwr_rb, 2); log_nl(L);
        }
    } else {
        log_str(L, "  GMUX dGPU rail LEFT ON (Hybrid Mode)"); log_nl(L);
    }

    if (GmuxRead8(BS, GMUX_PORT_IRQ_STATUS, &st) && st != 0u) {
        GmuxWrite8(BS, GMUX_PORT_IRQ_STATUS, st);
    }
    if (mask_valid) {
        GmuxWrite8(BS, GMUX_PORT_IRQ_ENABLE, orig_mask);
    }
    log_str(L, "  GMUX IRQ status cleared, mask restored=");
    log_dec(L, mask_valid ? 1u : 0u); log_nl(L);

    return mux_ok;
}

/* ---- USB FAT32 Read/Write Helpers (on LoadedImage->DeviceHandle) ---- */

static EFI_STATUS WriteUsbFile(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle,
                               CHAR16 *RelPath, const VOID *Data, UINTN Size) {
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_HANDLE root = NULL, dir = NULL, file = NULL;
    EFI_STATUS st;

    st = BS->HandleProtocol(ImageHandle, &gLoadedImageGuid, (VOID **)&li);
    if (EFI_ERROR(st) || !li) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;
    st = BS->HandleProtocol(li->DeviceHandle, &gSimpleFsGuid, (VOID **)&fs);
    if (EFI_ERROR(st) || !fs) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;
    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st) || !root) return EFI_ERROR(st) ? st : EFI_DEVICE_ERROR;

    st = root->Open(root, &dir, L"\\M2_LOGS",
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                    EFI_FILE_DIRECTORY);
    if (!EFI_ERROR(st) && dir != NULL) dir->Close(dir);

    if (!EFI_ERROR(root->Open(root, &file, RelPath, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && file != NULL) {
        file->Delete(file);
        file = NULL;
    }

    st = root->Open(root, &file, RelPath,
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(st) || !file) {
        root->Close(root);
        return EFI_ERROR(st) ? st : EFI_DEVICE_ERROR;
    }

    UINTN to_write = Size;
    st = file->Write(file, &to_write, (VOID *)Data);
    file->Flush(file);
    file->Close(file);
    root->Close(root);

    if (!EFI_ERROR(st) && to_write != Size) return EFI_DEVICE_ERROR;
    return st;
}

static EFI_STATUS ReadUsbFile(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle,
                              CHAR16 *RelPath, VOID **DataOut, UINTN *SizeOut) {
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_HANDLE root = NULL, file = NULL;
    EFI_STATUS st;
    UINT64 flen = 0;

    *DataOut = NULL;
    *SizeOut = 0;

    st = BS->HandleProtocol(ImageHandle, &gLoadedImageGuid, (VOID **)&li);
    if (EFI_ERROR(st) || !li) return EFI_NOT_FOUND;
    st = BS->HandleProtocol(li->DeviceHandle, &gSimpleFsGuid, (VOID **)&fs);
    if (EFI_ERROR(st) || !fs) return EFI_NOT_FOUND;
    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st) || !root) return EFI_ERROR(st) ? st : EFI_DEVICE_ERROR;

    st = root->Open(root, &file, RelPath, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !file) {
        root->Close(root);
        return EFI_ERROR(st) ? st : EFI_NOT_FOUND;
    }

    file->SetPosition(file, 0xFFFFFFFFFFFFFFFFULL);
    file->GetPosition(file, &flen);
    file->SetPosition(file, 0);

    if (flen == 0 || flen > 0x100000ULL) {
        file->Close(file);
        root->Close(root);
        return EFI_BAD_BUFFER_SIZE;
    }

    VOID *buf = NULL;
    st = BS->AllocatePool(EfiBootServicesData, (UINTN)flen, &buf);
    if (EFI_ERROR(st) || !buf) {
        file->Close(file);
        root->Close(root);
        return EFI_OUT_OF_RESOURCES;
    }

    UINTN rsz = (UINTN)flen;
    st = file->Read(file, &rsz, buf);
    file->Close(file);
    root->Close(root);

    if (EFI_ERROR(st) || rsz != (UINTN)flen) {
        BS->FreePool(buf);
        return EFI_ERROR(st) ? st : EFI_DEVICE_ERROR;
    }

    *DataOut = buf;
    *SizeOut = rsz;
    return EFI_SUCCESS;
}

/* ---- AppleSetOs Invocation ---- */

static EFI_STATUS InvokeAppleSetOs(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle, LOG_BUF *L) {
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    EFI_STATUS st = BS->LocateHandleBuffer(ByProtocol, &gAppleSetOsGuid, NULL, &count, &handles);
    log_str(L, "  [AppleSetOs] Locate=0x"); log_hex(L, st, 16);
    log_str(L, " count="); log_dec(L, EFI_ERROR(st) ? 0u : count); log_nl(L);
    if (EFI_ERROR(st) || !handles || count == 0u) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;

    EFI_STATUS final_st = EFI_SUCCESS;
    for (UINTN i = 0; i < count; i++) {
        EFI_APPLE_SET_OS_PROTOCOL *p = NULL;
        st = BS->OpenProtocol(handles[i], &gAppleSetOsGuid, (VOID **)&p,
                              ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(st) || !p) {
            final_st = st;
            continue;
        }
        log_str(L, "    handle["); log_dec(L, i); log_str(L, "] Ver="); log_dec(L, p->Version);
        if (p->Version != 0u) {
            EFI_STATUS sv = p->SetOsVendor((CHAR8 *)"Apple Inc.");
            EFI_STATUS vr = p->SetOsVersion((CHAR8 *)"Mac OS X 10.15");
            log_str(L, " Vendor=0x"); log_hex(L, sv, 16);
            log_str(L, " Version=0x"); log_hex(L, vr, 16);
            if (EFI_ERROR(sv)) final_st = sv;
            if (EFI_ERROR(vr)) final_st = vr;
        }
        log_nl(L);
    }
    BS->FreePool(handles);
    return final_st;
}

/* ---- Find Intel iGPU (00:02.0) via EFI_PCI_IO_PROTOCOL ---- */

static EFI_PCI_IO_PROTOCOL *FindIgpu(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle, LOG_BUF *L) {
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    EFI_STATUS st = BS->LocateHandleBuffer(ByProtocol, &gPciIoGuid, NULL, &count, &handles);
    if (EFI_ERROR(st) || !handles) {
        log_str(L, "  [FindIgpu] LocateHandleBuffer(PciIo) failed: 0x"); log_hex(L, st, 16); log_nl(L);
        return NULL;
    }

    EFI_PCI_IO_PROTOCOL *found = NULL;
    for (UINTN i = 0; i < count; i++) {
        EFI_PCI_IO_PROTOCOL *pci = NULL;
        UINT32 w0 = 0, w8 = 0;
        if (EFI_ERROR(BS->OpenProtocol(handles[i], &gPciIoGuid, (VOID **)&pci,
                                       ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL)) || !pci)
            continue;
        if (EFI_ERROR(pci->Pci.Read(pci, EfiPciIoWidthUint32, 0u, 1u, &w0))) continue;
        if ((w0 & 0xFFFFu) != 0x8086u) continue;
        if (EFI_ERROR(pci->Pci.Read(pci, EfiPciIoWidthUint32, 8u, 1u, &w8))) continue;
        if ((w8 >> 24) != 0x03u) continue;
        found = pci;
        break;
    }
    BS->FreePool(handles);
    return found;
}

/* ---- OpRegion VBT Inspector + BDB[43] -> NONE Backlight Patch ---- */

static BOOLEAN PatchVbtBacklightNone(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle,
                                      EFI_PCI_IO_PROTOCOL *Igpu, LOG_BUF *L) {
    if (!Igpu) {
        log_str(L, "  [VBT BDB43] Skipped (no iGPU)"); log_nl(L);
        return FALSE;
    }

    UINT32 asls = 0;
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_ASLS, 1u, &asls);
    log_str(L, "  [OpRegion/VBT] ASLS=0x"); log_hex(L, asls, 8); log_nl(L);
    if (asls == 0u || asls == 0xFFFFFFFFu) {
        log_str(L, "  [VBT BDB43] Skipped (ASLS=0)"); log_nl(L);
        return FALSE;
    }

    UINT8 *op = (UINT8 *)(UINTN)asls;
    if (!ascii_eq_n(op, "IntelGraphicsMem", 16u)) {
        log_str(L, "  [VBT BDB43] OpRegion sig mismatch"); log_nl(L);
        return FALSE;
    }

    UINTN opsz = (UINTN)rd32(op + 16u) * 1024u;
    UINT64 rvda = rd64(op + OPREGION_ASLE_OFF + ASLE_RVDA_OFF);
    UINT32 rvds = rd32(op + OPREGION_ASLE_OFF + ASLE_RVDS_OFF);
    log_str(L, "    size="); log_dec(L, opsz);
    log_str(L, " rvda=0x"); log_hex(L, rvda, 16);
    log_str(L, " rvds="); log_dec(L, rvds); log_nl(L);

    if (opsz < OPREGION_VBT_OFF + 64u) return FALSE;
    UINTN room = opsz - OPREGION_VBT_OFF;
    if (room > OPREGION_VBT_MAX) room = OPREGION_VBT_MAX;
    UINT8 *v = op + OPREGION_VBT_OFF;
    if (!ascii_eq_n(v, "$VBT", 4u)) {
        log_str(L, "  [VBT BDB43] No $VBT in MB4"); log_nl(L);
        return FALSE;
    }

    UINTN vsz = rd16(v + 24u);
    UINTN bdb = rd32(v + 28u);
    if (vsz < 64u || vsz > room || bdb + 22u > vsz) return FALSE;
    log_str(L, "    VBT sig=\""); log_strn(L, (const char *)v, 20u);
    log_str(L, "\" vbt_sz="); log_dec(L, vsz); log_nl(L);

    UINTN hdr  = rd16(v + bdb + 18u);
    UINTN bend = bdb + rd16(v + bdb + 20u);
    if (bend > vsz) bend = vsz;

    UINTN o43 = 0, s43 = 0;
    for (UINTN pos = bdb + hdr; pos + 3u <= bend; ) {
        UINT8 bid = v[pos];
        UINTN bsz = rd16(v + pos + 1u);
        if (pos + 3u + bsz > bend) break;
        if (bid == 43u) { o43 = pos + 3u; s43 = bsz; break; }
        pos += 3u + bsz;
    }
    if (!o43 || s43 < 1u) {
        log_str(L, "  [VBT BDB43] Block 43 not found"); log_nl(L);
        return FALSE;
    }

    UINTN esz = v[o43];
    if (esz < 1u || 1u + 16u * esz > s43) {
        log_str(L, "  [VBT BDB43] Unexpected entry_size="); log_dec(L, esz);
        log_str(L, " sz="); log_dec(L, s43); log_nl(L);
        return FALSE;
    }

    UINT8 before_p0 = v[o43 + 1u];
    UINT8 before_p2 = v[o43 + 1u + 2u * esz];
    for (UINTN i = 0; i < 16u * esz; i++) {
        v[o43 + 1u + i] = 0u;
    }
    if (s43 >= 1u + 16u * esz + 32u) {
        for (UINTN i = 0; i < 16u; i++) {
            v[o43 + 1u + 16u * esz + i] = 0u;
        }
    }

    v[26] = 0u;
    v[26] = (UINT8)(0u - sum8(v, vsz));

    BOOLEAN ok = (v[o43 + 1u] == 0u && sum8(v, vsz) == 0u);
    EFI_STATUS sst = WriteUsbFile(BS, ImageHandle, L"\\M2_LOGS\\vbt_patched.bin", v, vsz);
    log_str(L, "  [VBT BDB43] before(p0,p2)=0x"); log_hex(L, before_p0, 2);
    log_str(L, ",0x"); log_hex(L, before_p2, 2);
    log_str(L, " -> after=0x"); log_hex(L, v[o43 + 1u], 2);
    log_str(L, " esz="); log_dec(L, esz);
    log_str(L, " sum8="); log_dec(L, sum8(v, vsz));
    log_str(L, ok ? " OK" : " MISMATCH");
    log_str(L, " save=0x"); log_hex(L, sst, 16); log_nl(L);
    return ok;
}

/* ---- Intel HD 530 eDP AUX-A Probe & DDI_BUF_CTL_A 4-Lane Force ---- */

static UINT32 IgpuRd32(EFI_PCI_IO_PROTOCOL *Pci, UINT32 off) {
    UINT32 v = 0xFFFFFFFFu;
    Pci->Mem.Read(Pci, EfiPciIoWidthUint32, 0u, off, 1u, &v);
    return v;
}

static VOID IgpuWr32(EFI_PCI_IO_PROTOCOL *Pci, UINT32 off, UINT32 v) {
    Pci->Mem.Write(Pci, EfiPciIoWidthUint32, 0u, off, 1u, &v);
}

static EFI_STATUS AuxXfer(EFI_BOOT_SERVICES *BS, EFI_PCI_IO_PROTOCOL *Pci,
                          const UINT8 *tx, UINTN txn, UINT8 *rx, UINTN *rxn) {
    for (UINTN attempt = 0; attempt < 5u; attempt++) {
        UINT32 ctl = 0;
        BOOLEAN idle = FALSE;
        for (UINTN i = 0; i < 100u; i++) {
            ctl = IgpuRd32(Pci, DP_AUX_CH_CTL_A);
            if (ctl == 0xFFFFFFFFu) return EFI_NOT_READY;
            if (!(ctl & AUX_SEND_BUSY)) { idle = TRUE; break; }
            BS->Stall(100);
        }
        if (!idle) return EFI_TIMEOUT;

        for (UINTN i = 0; i < txn; i += 4u) {
            UINT32 w = 0;
            for (UINTN j = 0; j < 4u; j++) {
                w <<= 8;
                if (i + j < txn) w |= tx[i + j];
            }
            IgpuWr32(Pci, DP_AUX_CH_DATA_A(i / 4u), w);
        }

        UINT32 send = AUX_SEND_BUSY | AUX_DONE | AUX_INTERRUPT | AUX_TIMEOUT_ERR |
                      AUX_TIMEOUT_MAX | AUX_RECEIVE_ERR | AUX_MSG_SIZE(txn) |
                      AUX_FW_SYNC_SKL | AUX_SYNC_SKL;
        IgpuWr32(Pci, DP_AUX_CH_CTL_A, send);

        for (UINTN i = 0; i < 100u; i++) {
            ctl = IgpuRd32(Pci, DP_AUX_CH_CTL_A);
            if (!(ctl & AUX_SEND_BUSY)) break;
            BS->Stall(100);
        }
        IgpuWr32(Pci, DP_AUX_CH_CTL_A, ctl | AUX_DONE | AUX_TIMEOUT_ERR | AUX_RECEIVE_ERR);

        if ((ctl & AUX_SEND_BUSY) || (ctl & AUX_TIMEOUT_ERR) || (ctl & AUX_RECEIVE_ERR) || !(ctl & AUX_DONE)) {
            BS->Stall(1000);
            continue;
        }
        UINTN n = (ctl >> 20) & 0x1Fu;
        if (n == 0u || n > 20u) { BS->Stall(1000); continue; }
        if (n > *rxn) n = *rxn;
        for (UINTN i = 0; i < n; i += 4u) {
            UINT32 w = IgpuRd32(Pci, DP_AUX_CH_DATA_A(i / 4u));
            for (UINTN j = 0; j < 4u && i + j < n; j++) {
                rx[i + j] = (UINT8)(w >> (24u - 8u * j));
            }
        }
        *rxn = n;
        return EFI_SUCCESS;
    }
    return EFI_DEVICE_ERROR;
}

static EFI_STATUS DpcdRead(EFI_BOOT_SERVICES *BS, EFI_PCI_IO_PROTOCOL *Pci,
                           UINT32 addr, UINT8 *out, UINTN len) {
    for (UINTN defer = 0; defer < 7u; defer++) {
        UINT8 tx[4] = {
            (UINT8)(0x90u | ((addr >> 16) & 0xFu)),
            (UINT8)(addr >> 8),
            (UINT8)addr,
            (UINT8)(len - 1u)
        };
        UINT8 rx[20];
        UINTN rn = len + 1u;
        EFI_STATUS st = AuxXfer(BS, Pci, tx, 4u, rx, &rn);
        if (EFI_ERROR(st)) return st;
        UINT8 code = (UINT8)(rx[0] >> 4);
        if (code == 0x2u) { BS->Stall(400); continue; }
        if (code != 0u || rn < len + 1u) return EFI_DEVICE_ERROR;
        for (UINTN i = 0; i < len; i++) out[i] = rx[1u + i];
        return EFI_SUCCESS;
    }
    return EFI_TIMEOUT;
}

static BOOLEAN ProbeEdpAndForce4Lanes(EFI_BOOT_SERVICES *BS, EFI_PCI_IO_PROTOCOL *Igpu, LOG_BUF *L) {
    if (!Igpu) {
        log_str(L, "  [eDP/DDI_A] No Intel iGPU PciIo found"); log_nl(L);
        return FALSE;
    }

    UINT32 cmd = 0, bar0 = 0, bar1 = 0;
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_CMD,  1u, &cmd);
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_BAR0, 1u, &bar0);
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_BAR0 + 4u, 1u, &bar1);
    UINT64 base = (UINT64)(bar0 & ~0xFu) | ((UINT64)bar1 << 32);
    if ((bar0 & 1u) || base == 0u) {
        log_str(L, "  [eDP/DDI_A] BAR0 unassigned: 0x"); log_hex(L, bar0, 8); log_nl(L);
        return FALSE;
    }
    if (!(cmd & 0x2u)) {
        EFI_STATUS en = Igpu->Attributes(Igpu, EfiPciIoAttributeOperationEnable,
                                         EFI_PCI_IO_ATTRIBUTE_MEMORY, NULL);
        log_str(L, "  [eDP/DDI_A] Enabled MSE status=0x"); log_hex(L, en, 16); log_nl(L);
        if (EFI_ERROR(en)) return FALSE;
    }

    UINT32 pw  = IgpuRd32(Igpu, PWR_WELL_CTL2);
    UINT32 pps = IgpuRd32(Igpu, PP_STATUS);
    UINT32 ppc = IgpuRd32(Igpu, PP_CONTROL);
    log_str(L, "  [eDP Probe] PWR_WELL_CTL2=0x"); log_hex(L, pw, 8);
    log_str(L, " PP_STATUS=0x"); log_hex(L, pps, 8);
    log_str(L, " PP_CONTROL=0x"); log_hex(L, ppc, 8); log_nl(L);

    if (pw != 0xFFFFFFFFu && pps != 0xFFFFFFFFu) {
        UINT32 want = 0u;
        if (!(pw & PW_PW1_STATE))    want |= PW_PW1_REQ;
        if (!(pw & PW_DDI_AE_STATE)) want |= PW_DDI_AE_REQ;
        if (want) {
            IgpuWr32(Igpu, PWR_WELL_CTL2, pw | want);
            for (UINTN i = 0; i < 300u; i++) {
                UINT32 st = IgpuRd32(Igpu, PWR_WELL_CTL2);
                if ((st & PW_PW1_STATE) && (st & PW_DDI_AE_STATE)) break;
                BS->Stall(100);
            }
            log_str(L, "    Power wells now=0x"); log_hex(L, IgpuRd32(Igpu, PWR_WELL_CTL2), 8); log_nl(L);
        }

        BOOLEAN forced_vdd = FALSE;
        if (!(pps & PP_ON) && !(ppc & PP_FORCE_VDD)) {
            IgpuWr32(Igpu, PP_CONTROL, ((ppc & 0xFFFFu) | PP_FORCE_VDD) | PP_UNLOCK);
            forced_vdd = TRUE;
            BS->Stall(150000);
        }

        UINT8 dpcd[16];
        EFI_STATUS dst = DpcdRead(BS, Igpu, 0x000u, dpcd, 16u);
        if (EFI_ERROR(dst)) {
            log_str(L, "    DPCD 0x000 read FAILED: 0x"); log_hex(L, dst, 16); log_nl(L);
        } else {
            log_str(L, "    DPCD[0..15]: rev=0x"); log_hex(L, dpcd[0], 2);
            log_str(L, " max_rate=0x"); log_hex(L, dpcd[1], 2);
            log_str(L, " max_lanes="); log_dec(L, dpcd[2] & 0x1Fu); log_nl(L);
        }

        if (forced_vdd) {
            UINT32 now = IgpuRd32(Igpu, PP_CONTROL);
            IgpuWr32(Igpu, PP_CONTROL, ((now & 0xFFFFu) & ~PP_FORCE_VDD) | PP_UNLOCK);
        }
    }

    UINT32 ddi_before = IgpuRd32(Igpu, DDI_BUF_CTL_A);
    log_str(L, "  [DDI_BUF_CTL_A] before=0x"); log_hex(L, ddi_before, 8);
    if (ddi_before == 0xFFFFFFFFu) {
        log_str(L, " (all ones - skipped!)"); log_nl(L);
        return FALSE;
    }
    if ((ddi_before & DDI_A_4_LANES) == 0u) {
        IgpuWr32(Igpu, DDI_BUF_CTL_A, ddi_before | DDI_A_4_LANES);
    }
    UINT32 ddi_after = IgpuRd32(Igpu, DDI_BUF_CTL_A);
    BOOLEAN ok = ((ddi_after & DDI_A_4_LANES) != 0u);
    log_str(L, " after=0x"); log_hex(L, ddi_after, 8);
    log_str(L, ok ? " [4_LANES OK]" : " [4_LANES FAILED]");
    log_nl(L);
    return ok;
}

/* ---- Deterministic Hardware Snapshot (Safe Before AND After ExitBootServices) ---- */

static void CaptureM2Snap(M2_HW_SNAP *S) {
    mem_zero(S, sizeof(*S));

    S->HbDeven    = pci_rd32(0u, 0u, 0u, 0x54u);
    S->Hb80       = pci_rd32(0u, 0u, 0u, 0x80u);
    S->PegCmd     = (UINT32)pci_rd16(0u, 1u, 0u, 0x04u);

    S->IgpuVidDid = pci_rd32(0u, 2u, 0u, 0x00u);
    S->IgpuCmd    = pci_rd32(0u, 2u, 0u, 0x04u);
    S->IgpuBar0   = pci_rd32(0u, 2u, 0u, 0x10u);
    S->IgpuAsls   = pci_rd32(0u, 2u, 0u, 0xFCu);

    UINT32 ivid  = S->IgpuVidDid & 0xFFFFu;
    UINT32 icmd  = S->IgpuCmd & 0xFFFFu;
    UINT32 ib0   = S->IgpuBar0 & ~0xFu;
    UINT32 ibar1 = (ivid == 0x8086u) ? pci_rd32(0u, 2u, 0u, 0x14u) : 0xFFFFFFFFu;
    if (ivid == 0x8086u && (icmd & 0x02u) && (S->IgpuBar0 & 1u) == 0u &&
        ibar1 == 0u && ib0 != 0u && ib0 < 0xF0000000u) {
        S->IgpuDdiA = mmio_r32((UINT64)ib0 + DDI_BUF_CTL_A);
        S->IgpuPwr2 = mmio_r32((UINT64)ib0 + PWR_WELL_CTL2);
    } else {
        S->IgpuDdiA = 0xFFFFFFFFu;
        S->IgpuPwr2 = 0xFFFFFFFFu;
    }

    S->AmdVidDid = pci_rd32(1u, 0u, 0u, 0x00u);
    S->AmdCmd    = pci_rd32(1u, 0u, 0u, 0x04u);
    S->AmdBar5   = pci_rd32(1u, 0u, 0u, 0x24u);

    UINT32 avid = S->AmdVidDid & 0xFFFFu;
    UINT32 acmd = S->AmdCmd & 0xFFFFu;
    UINT32 b5   = S->AmdBar5 & ~0xFu;
    if (avid == 0x1002u && (acmd & 0x02u) && (S->PegCmd & 0x02u) &&
        (S->AmdBar5 & 1u) == 0u && b5 != 0u && b5 < 0xF0000000u) {
        S->AmdFbLoc   = mmio_r32((UINT64)b5 + (UINT64)mmMC_VM_FB_LOCATION * 4u);
        S->AmdHdpBase = mmio_r32((UINT64)b5 + (UINT64)mmHDP_NONSURFACE_BASE * 4u);
        S->AmdSurfLo  = mmio_r32((UINT64)b5 + (UINT64)mmGRPH_PRIMARY_SURF_ADDR * 4u);
        S->AmdSurfHi  = mmio_r32((UINT64)b5 + (UINT64)mmGRPH_PRIMARY_SURF_HIGH * 4u);
        S->AmdGrbm    = mmio_r32((UINT64)b5 + (UINT64)mmGRBM_STATUS * 4u);
        S->AmdSmuPc0  = smc_ind_read((UINT64)b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMC_PC_C);
        S->AmdSmuFw   = smc_ind_read((UINT64)b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMU_FIRMWARE);
    } else {
        S->AmdFbLoc   = 0xFFFFFFFFu;
        S->AmdHdpBase = 0xFFFFFFFFu;
        S->AmdSurfLo  = 0xFFFFFFFFu;
        S->AmdSurfHi  = 0xFFFFFFFFu;
        S->AmdGrbm    = 0xFFFFFFFFu;
        S->AmdSmuPc0  = 0xFFFFFFFFu;
        S->AmdSmuFw   = 0xFFFFFFFFu;
    }

    io_out8(GMUX_IO_PROBE0, 0xAAu);
    io_out8(GMUX_IO_PROBE1, 0x55u);
    io_out8(GMUX_IO_PROBE2, 0x00u);
    UINT16 sig = (UINT16)io_in8(GMUX_IO_PROBE0) | ((UINT16)io_in8(GMUX_IO_PROBE1) << 8);
    S->GmuxSig = (UINT32)sig;
    if (sig == 0x55AAu) {
        S->GmuxDisp = GmuxRead8Pure(GMUX_PORT_SWITCH_DISPLAY);
        S->GmuxDdc  = GmuxRead8Pure(GMUX_PORT_SWITCH_DDC);
        S->GmuxExt  = GmuxRead8Pure(GMUX_PORT_SWITCH_EXTERNAL);
        S->GmuxPwr  = GmuxRead8Pure(GMUX_PORT_DISCRETE_POWER);
        S->GmuxBrt  = GmuxRead32Pure(GMUX_PORT_BRIGHTNESS);
    } else {
        S->GmuxDisp = 0xFFFFFFFFu;
        S->GmuxDdc  = 0xFFFFFFFFu;
        S->GmuxExt  = 0xFFFFFFFFu;
        S->GmuxPwr  = 0xFFFFFFFFu;
        S->GmuxBrt  = 0xFFFFFFFFu;
    }
}

/* ---- Hooked BS->ExitBootServices (CRITICAL: MS ABI EFIAPI) ---- */

static void UpdateM2Checksum(M2HY_ACPI_TABLE *T) {
    T->Hdr.Checksum = 0u;
    T->Hdr.Checksum = (UINT8)(0u - sum8((const UINT8 *)T, T->Hdr.Length));
}

static EFI_STATUS EFIAPI HookedExitBootServices(EFI_HANDLE ImageHandle, UINTN MapKey) {
    if (gM2Table != NULL) {
        gM2Table->EbsCallCount++;
        if ((gM2Table->Flags & M2_FLAG_BEFORE_EBS) == 0u) {
            CaptureM2Snap(&gM2Table->BeforeEbs);
            gM2Table->Flags |= M2_FLAG_BEFORE_EBS;
            UpdateM2Checksum(gM2Table);
        }
    }

    EFI_STATUS st = gOrigExitBootServices(ImageHandle, MapKey);
    if (EFI_ERROR(st)) {
        if (gM2Table != NULL) {
            gM2Table->EbsRetryCount++;
            UpdateM2Checksum(gM2Table);
        }
        return st;
    }

    /* Boot Services are GONE now! Use only raw PCI / MMIO / I/O and gM2Table! */
    if (gM2Table != NULL) {
        CaptureM2Snap(&gM2Table->AfterEbs);
        gM2Table->Flags |= M2_FLAG_AFTER_EBS;

        if (gM2Table->Mode == M2_MODE_HYBRID || gM2Table->Mode == M2_MODE_AMD_DUAL) {
            UINT16 avid    = pci_rd16(1u, 0u, 0u, 0x00u);
            UINT16 acmd    = pci_rd16(1u, 0u, 0u, 0x04u);
            UINT16 peg_cmd = pci_rd16(0u, 1u, 0u, 0x04u);
            UINT32 bar5    = pci_rd32(1u, 0u, 0u, 0x24u);
            UINT32 b5      = bar5 & ~0xFu;
            if (avid == 0x1002u && (acmd & 0x02u) && (peg_cmd & 0x02u) &&
                (bar5 & 1u) == 0u && b5 != 0u && b5 < 0xF0000000u) {
                UINT32 cur_fb = mmio_r32((UINT64)b5 + (UINT64)mmMC_VM_FB_LOCATION * 4u);
                if (cur_fb == 0xF47FF400u) {
                    gM2Table->Flags |= M2_FLAG_FBLOC_3REG_OK;
                } else {
                    UINT32 surf_lo = mmio_r32((UINT64)b5 + (UINT64)mmGRPH_PRIMARY_SURF_ADDR * 4u);
                    mmio_w32((UINT64)b5 + (UINT64)mmMC_VM_FB_LOCATION * 4u, 0xF47FF400u);
                    mmio_w32((UINT64)b5 + (UINT64)mmHDP_NONSURFACE_BASE * 4u, 0xF4000000u);
                    mmio_w32((UINT64)b5 + (UINT64)mmGRPH_PRIMARY_SURF_HIGH * 4u, 0x000000F4u);
                    mmio_w32((UINT64)b5 + (UINT64)mmGRPH_PRIMARY_SURF_ADDR * 4u, surf_lo);
                    io_delay_us(500);
                    UINT32 rb_fb  = mmio_r32((UINT64)b5 + (UINT64)mmMC_VM_FB_LOCATION * 4u);
                    UINT32 rb_hdp = mmio_r32((UINT64)b5 + (UINT64)mmHDP_NONSURFACE_BASE * 4u);
                    UINT32 rb_dce = mmio_r32((UINT64)b5 + (UINT64)mmGRPH_PRIMARY_SURF_HIGH * 4u);
                    if (rb_fb == 0xF47FF400u && rb_hdp == 0xF4000000u &&
                        rb_dce == 0x000000F4u) {
                        gM2Table->Flags |= M2_FLAG_FBLOC_3REG_OK;
                    }
                }
            }
        }

        if (gM2Table->Mode == M2_MODE_HYBRID || gM2Table->Mode == M2_MODE_INTEL_ONLY) {
            UINT16 ivid  = pci_rd16(0u, 2u, 0u, 0x00u);
            UINT16 icmd  = pci_rd16(0u, 2u, 0u, 0x04u);
            UINT32 ibar0 = pci_rd32(0u, 2u, 0u, 0x10u);
            UINT32 ibar1 = (ivid == 0x8086u) ? pci_rd32(0u, 2u, 0u, 0x14u) : 0xFFFFFFFFu;
            UINT32 ib0   = ibar0 & ~0xFu;
            if (ivid == 0x8086u && (icmd & 0x02u) && (ibar0 & 1u) == 0u &&
                ibar1 == 0u && ib0 != 0u && ib0 < 0xF0000000u) {
                UINT32 ddi = mmio_r32((UINT64)ib0 + DDI_BUF_CTL_A);
                if (ddi != 0xFFFFFFFFu && (ddi & DDI_A_4_LANES) == 0u) {
                    mmio_w32((UINT64)ib0 + DDI_BUF_CTL_A, ddi | DDI_A_4_LANES);
                    UINT32 ddi_rb = mmio_r32((UINT64)ib0 + DDI_BUF_CTL_A);
                    if ((ddi_rb & DDI_A_4_LANES) != 0u) {
                        gM2Table->Flags |= M2_FLAG_DDI_POST_EBS_FIX;
                    }
                }
            }
        }

        CaptureM2Snap(&gM2Table->PostAction);
        gM2Table->Flags |= M2_FLAG_POST_ACTION;
        UpdateM2Checksum(gM2Table);
    }

    return EFI_SUCCESS;
}

/* ---- Single Clean XSDT Rebuild: DSDT Renames + SSDT_IGPU_MBP133 + M2HY ---- */

static UINTN FindMethodInAml(const UINT8 *t, UINTN len, const char *name4, UINTN *raw_count) {
    UINTN pos = 0u;
    *raw_count = 0u;
    for (UINTN i = ACPI_HDR_SIZE; i + 4u <= len; i++) {
        if (ascii_eq_n(t + i, name4, 4u)) {
            (*raw_count)++;
            if (i >= 3u && (t[i - 2] == 0x14u || t[i - 3] == 0x14u)) {
                pos = i;
            }
        }
    }
    return pos;
}

static UINT8 *FindDsdtViaFadt(const UINT8 *xsdt, UINTN entry_count) {
    for (UINTN i = 0; i < entry_count; i++) {
        const UINT8 *t = (const UINT8 *)(UINTN)rd64(xsdt + ACPI_HDR_SIZE + i * 8u);
        if (!t || !ascii_eq_n(t, "FACP", 4u)) continue;
        UINT32 flen = rd32(t + ACPI_HDR_LEN);
        UINT64 dsdt = 0;
        if (flen >= 148u) dsdt = rd64(t + 140u);
        if (dsdt == 0u && flen >= 44u) dsdt = rd32(t + 40u);
        return (UINT8 *)(UINTN)dsdt;
    }
    return NULL;
}

static UINT8 *PatchVfctRemoveEdp(EFI_BOOT_SERVICES *BS, const UINT8 *orig_vfct, LOG_BUF *L) {
    if (!orig_vfct || !ascii_eq_n(orig_vfct, "VFCT", 4u)) {
        log_str(L, "  [VFCT] Not a VFCT table"); log_nl(L);
        return NULL;
    }

    UINT32 vlen = rd32(orig_vfct + ACPI_HDR_LEN);
    if (vlen < 0x268u || vlen > 0x40000u || sum8(orig_vfct, vlen) != 0u) {
        log_str(L, "  [VFCT] Invalid len/checksum"); log_nl(L);
        return NULL;
    }

    UINT32 img_off = rd32(orig_vfct + 0x34u);
    if (img_off < ACPI_HDR_SIZE || (UINTN)img_off + 0x1Cu + 0x200u > vlen) {
        log_str(L, "  [VFCT] Invalid VBIOS image offset"); log_nl(L);
        return NULL;
    }

    UINT32 img_len = rd32(orig_vfct + img_off + 0x18u);
    UINTN vb_off = (UINTN)img_off + 0x1Cu;
    if (orig_vfct[vb_off] != 0x55u || orig_vfct[vb_off + 1u] != 0xAAu) {
        log_str(L, "  [VFCT] Missing 55 AA signature"); log_nl(L);
        return NULL;
    }

    UINTN rom_len = (UINTN)orig_vfct[vb_off + 2u] * 512u;
    if (rom_len < 0x1000u || rom_len > img_len || vb_off + rom_len > vlen ||
        sum8(orig_vfct + vb_off, rom_len) != 0u) {
        log_str(L, "  [VFCT] Invalid VBIOS ROM len/checksum"); log_nl(L);
        return NULL;
    }

    UINT8 *new_vfct = NULL;
    if (EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, vlen, (VOID **)&new_vfct)) || !new_vfct) {
        log_str(L, "  [VFCT] AllocatePool failed"); log_nl(L);
        return NULL;
    }
    mem_copy(new_vfct, orig_vfct, vlen);

    UINT8 *vb = new_vfct + vb_off;
    UINT16 rom_hdr = rd16(vb + 0x48u);
    if ((UINTN)rom_hdr + 36u > rom_len || !ascii_eq_n(vb + rom_hdr + 4u, "ATOM", 4u)) {
        log_str(L, "  [VFCT] Missing ATOM ROM header"); log_nl(L);
        BS->FreePool(new_vfct);
        return NULL;
    }

    UINT16 mdt = rd16(vb + rom_hdr + 32u);
    if ((UINTN)mdt + 4u + 2u * 23u > rom_len) {
        log_str(L, "  [VFCT] MasterDataTable out of range"); log_nl(L);
        BS->FreePool(new_vfct);
        return NULL;
    }

    UINT16 obj_off = rd16(vb + mdt + 4u + 2u * 22u);
    if (obj_off == 0u || (UINTN)obj_off + 18u > rom_len) {
        log_str(L, "  [VFCT] Object_Header offset invalid"); log_nl(L);
        BS->FreePool(new_vfct);
        return NULL;
    }

    UINT16 obj_sz = rd16(vb + obj_off);
    if (obj_sz < 18u || (UINTN)obj_off + obj_sz > rom_len) {
        log_str(L, "  [VFCT] Object_Header size invalid"); log_nl(L);
        BS->FreePool(new_vfct);
        return NULL;
    }

    UINT8 *obj = vb + obj_off;
    if (obj[2] != 1u || obj[3] != 3u) {
        log_str(L, "  [VFCT] Unexpected Object_Header rev"); log_nl(L);
        BS->FreePool(new_vfct);
        return NULL;
    }

    UINT16 dev_sup  = rd16(obj + 0x04u);
    UINT16 conn_off = rd16(obj + 0x06u);
    UINT16 enc_off  = rd16(obj + 0x0Au);
    UINT16 dp_off   = rd16(obj + 0x0Eu);

    if ((dev_sup & 0x0002u) == 0u || dp_off < 18u || (UINTN)dp_off + 4u > obj_sz ||
        conn_off < 18u || (UINTN)conn_off + 4u > obj_sz) {
        log_str(L, "  [VFCT] Invalid Object_Header offsets/dev_sup"); log_nl(L);
        BS->FreePool(new_vfct);
        return NULL;
    }

    /* A. Clear LCD1 bit (0x0002) in usDeviceSupport */
    UINT16 new_dev_sup = (UINT16)(dev_sup & ~0x0002u);
    wr16(obj + 0x04u, new_dev_sup);

    /* B. Remove eDP/LCD1 entry from DisplayPathTable (4-byte header + variable entries) */
    UINT8 ndp = obj[dp_off];
    UINTN cur = (UINTN)dp_off + 4u;
    UINTN rm_pos = 0u, rm_sz = 0u;
    BOOLEAN dp_valid = TRUE;
    for (UINTN i = 0; i < ndp; i++) {
        if (cur + 8u > obj_sz) { dp_valid = FALSE; break; }
        UINT16 dev_tag = rd16(obj + cur);
        UINT16 esz     = rd16(obj + cur + 2u);
        UINT16 conn_id = rd16(obj + cur + 4u);
        if (esz < 8u || cur + esz > obj_sz) { dp_valid = FALSE; break; }
        if (rm_sz == 0u && (dev_tag == 0x0002u || (conn_id & 0x00FFu) == 0x14u)) {
            rm_pos = cur;
            rm_sz  = esz;
        }
        cur += esz;
    }
    UINTN dp_end = cur;
    BOOLEAN dp_removed = FALSE;
    if (dp_valid && rm_sz > 0u && ndp > 0u && dp_end >= rm_pos + rm_sz) {
        for (UINTN p = rm_pos; p + rm_sz < dp_end; p++) {
            obj[p] = obj[p + rm_sz];
        }
        for (UINTN p = dp_end - rm_sz; p < dp_end; p++) {
            obj[p] = 0u;
        }
        obj[dp_off] = (UINT8)(ndp - 1u);
        dp_removed = TRUE;
    }

    /* C. Remove eDP (0x14) entry from ConnectorObjectTable */
    UINT8 nco = obj[conn_off];
    UINTN co_base = (UINTN)conn_off + 4u;
    UINTN co_end  = co_base + (UINTN)nco * 8u;
    BOOLEAN co_removed = FALSE;
    UINT16 edp_enc_id = 0u;
    if (co_end <= obj_sz && nco > 0u) {
        UINTN found_idx = nco;
        for (UINTN i = 0; i < nco; i++) {
            UINTN e = co_base + i * 8u;
            UINT16 oid = rd16(obj + e);
            if ((oid & 0x00FFu) == 0x14u) {
                found_idx = i;
                UINT16 src_off = rd16(obj + e + 2u);
                if ((UINTN)src_off + 3u <= obj_sz && obj[src_off] >= 1u) {
                    edp_enc_id = rd16(obj + src_off + 1u);
                }
                break;
            }
        }
        if (found_idx < nco) {
            UINTN rm_c = co_base + found_idx * 8u;
            for (UINTN p = rm_c; p + 8u < co_end; p++) {
                obj[p] = obj[p + 8u];
            }
            for (UINTN p = co_end - 8u; p < co_end; p++) {
                obj[p] = 0u;
            }
            obj[conn_off] = (UINT8)(nco - 1u);
            co_removed = TRUE;
        }
    }

    /* D. Remove orphaned eDP encoder from EncoderObjectTable if no remaining connector uses it */
    UINT8 new_nco = obj[conn_off];
    BOOLEAN edp_enc_shared = FALSE;
    if (co_removed && edp_enc_id != 0u) {
        for (UINTN i = 0; i < new_nco; i++) {
            UINTN e = co_base + i * 8u;
            UINT16 src_off = rd16(obj + e + 2u);
            if ((UINTN)src_off + 1u < obj_sz) {
                UINT8 nsrc = obj[src_off];
                if ((UINTN)src_off + 1u + (UINTN)nsrc * 2u <= obj_sz) {
                    for (UINTN s = 0; s < nsrc; s++) {
                        if (rd16(obj + src_off + 1u + s * 2u) == edp_enc_id) {
                            edp_enc_shared = TRUE;
                            break;
                        }
                    }
                }
            }
            if (edp_enc_shared) break;
        }
    }

    UINT8 nen = 0u, new_nen = 0u;
    BOOLEAN enc_removed = FALSE;
    if (co_removed && edp_enc_id != 0u && !edp_enc_shared &&
        enc_off >= 18u && (UINTN)enc_off + 4u <= obj_sz) {
        nen = obj[enc_off];
        new_nen = nen;
        UINTN en_base = (UINTN)enc_off + 4u;
        UINTN en_end  = en_base + (UINTN)nen * 8u;
        if (nen > 0u && en_end <= obj_sz) {
            UINTN found_enc = nen;
            for (UINTN i = 0; i < nen; i++) {
                UINTN e = en_base + i * 8u;
                if (rd16(obj + e) == edp_enc_id) {
                    found_enc = i;
                    break;
                }
            }
            if (found_enc < nen) {
                UINTN rm_e = en_base + found_enc * 8u;
                for (UINTN p = rm_e; p + 8u < en_end; p++) {
                    obj[p] = obj[p + 8u];
                }
                for (UINTN p = en_end - 8u; p < en_end; p++) {
                    obj[p] = 0u;
                }
                new_nen = (UINT8)(nen - 1u);
                obj[enc_off] = new_nen;
                enc_removed = TRUE;
            }
        }
    }

    /* E. Recompute VBIOS ROM checksum (vb[0x21]) and VFCT ACPI checksum */
    UINT8 rem = sum8(vb, rom_len);
    vb[0x21u] = (UINT8)(vb[0x21u] - rem);

    new_vfct[ACPI_HDR_CSUM] = 0u;
    new_vfct[ACPI_HDR_CSUM] = (UINT8)(0u - sum8(new_vfct, vlen));

    UINT8 vb_s8 = sum8(vb, rom_len);
    UINT8 vf_s8 = sum8(new_vfct, vlen);
    if (!dp_removed || !co_removed || vb_s8 != 0u || vf_s8 != 0u) {
        log_str(L, "  [VFCT] Patch verification failed (dp=");
        log_dec(L, dp_removed ? 1u : 0u);
        log_str(L, " co="); log_dec(L, co_removed ? 1u : 0u);
        log_str(L, " vb_s8="); log_dec(L, vb_s8);
        log_str(L, " vf_s8="); log_dec(L, vf_s8);
        log_str(L, ")"); log_nl(L);
        BS->FreePool(new_vfct);
        return NULL;
    }

    log_str(L, "  [VFCT] Patched dev_sup 0x"); log_hex(L, dev_sup, 4);
    log_str(L, "->0x"); log_hex(L, new_dev_sup, 4);
    log_str(L, " dp "); log_dec(L, ndp); log_str(L, "->"); log_dec(L, obj[dp_off]);
    log_str(L, " conn "); log_dec(L, nco); log_str(L, "->"); log_dec(L, new_nco);
    if (enc_removed) {
        log_str(L, " enc "); log_dec(L, nen); log_str(L, "->"); log_dec(L, new_nen);
    }
    log_str(L, " vb_csum=0x"); log_hex(L, vb[0x21u], 2);
    log_str(L, " vfct_csum=0x"); log_hex(L, new_vfct[ACPI_HDR_CSUM], 2);
    log_str(L, " sum8="); log_dec(L, vb_s8); log_putc(L, ','); log_dec(L, vf_s8);
    log_nl(L);
    return new_vfct;
}

static UINT8 *PatchPeg0Gfx0Dod(EFI_BOOT_SERVICES *BS, const UINT8 *orig_peg, LOG_BUF *L) {
    if (!orig_peg || !ascii_eq_n(orig_peg, "SSDT", 4u) ||
        !ascii_eq_n(orig_peg + ACPI_HDR_OEMTID, "PEG0GFX0", 8u)) {
        log_str(L, "  [PEG0GFX0] Not PEG0GFX0 SSDT"); log_nl(L);
        return NULL;
    }

    UINT32 plen = rd32(orig_peg + ACPI_HDR_LEN);
    if (plen < ACPI_HDR_SIZE || plen > 0x10000u || sum8(orig_peg, plen) != 0u) {
        log_str(L, "  [PEG0GFX0] Invalid len/checksum"); log_nl(L);
        return NULL;
    }

    UINTN c_dod = 0, c_xdod = 0;
    UINTN off_dod = FindMethodInAml(orig_peg, plen, "_DOD", &c_dod);
    (void)FindMethodInAml(orig_peg, plen, "XDOD", &c_xdod);
    if (c_dod != 1u || off_dod == 0u || c_xdod != 0u) {
        log_str(L, "  [PEG0GFX0] Unexpected _DOD count="); log_dec(L, c_dod); log_nl(L);
        return NULL;
    }

    UINT8 *new_peg = NULL;
    if (EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, plen, (VOID **)&new_peg)) || !new_peg) {
        log_str(L, "  [PEG0GFX0] AllocatePool failed"); log_nl(L);
        return NULL;
    }
    mem_copy(new_peg, orig_peg, plen);
    new_peg[off_dod] = 'X';
    new_peg[ACPI_HDR_CSUM] = 0u;
    new_peg[ACPI_HDR_CSUM] = (UINT8)(0u - sum8(new_peg, plen));

    if (sum8(new_peg, plen) != 0u) {
        log_str(L, "  [PEG0GFX0] Checksum verification failed"); log_nl(L);
        BS->FreePool(new_peg);
        return NULL;
    }

    log_str(L, "  [PEG0GFX0] Renamed _DOD->XDOD @0x"); log_hex(L, off_dod, 4);
    log_str(L, " csum=0x"); log_hex(L, new_peg[ACPI_HDR_CSUM], 2); log_nl(L);
    return new_peg;
}

static EFI_STATUS InstallM2AcpiTables(EFI_BOOT_SERVICES *BS, EFI_SYSTEM_TABLE *ST,
                                      EFI_HANDLE ImageHandle, UINT32 Mode,
                                      BOOLEAN IncludeSsdt, UINT32 InitialFlags, LOG_BUF *L) {
    ACPI_RSDP_20 *rsdp = NULL;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        if (guid_eq(&ST->ConfigurationTable[i].VendorGuid, &gAcpi20Guid)) {
            rsdp = (ACPI_RSDP_20 *)ST->ConfigurationTable[i].VendorTable;
            break;
        }
    }
    if (!rsdp || !ascii_eq_n(rsdp->Signature, "RSD PTR ", 8u) ||
        rsdp->Revision < 2u || rsdp->XsdtAddress == 0u) {
        log_str(L, "  [ACPI] ACPI 2.0 RSDP/XSDT missing"); log_nl(L);
        return EFI_UNSUPPORTED;
    }

    UINT8 *xsdt = (UINT8 *)(UINTN)rsdp->XsdtAddress;
    UINT32 xlen = rd32(xsdt + ACPI_HDR_LEN);
    if (!ascii_eq_n(xsdt, "XSDT", 4u) || xlen < ACPI_HDR_SIZE || sum8(xsdt, xlen) != 0u) {
        log_str(L, "  [ACPI] Invalid XSDT"); log_nl(L);
        return EFI_COMPROMISED_DATA;
    }
    UINTN n_entries = (xlen - ACPI_HDR_SIZE) / 8u;

    /* Check for duplicates, locate VFCT / PEG0GFX0 & extract MCFG ECAM base */
    UINTN vfct_idx = (UINTN)-1;
    UINTN peg_idx  = (UINTN)-1;
    const UINT8 *orig_vfct = NULL;
    const UINT8 *orig_peg  = NULL;
    for (UINTN i = 0; i < n_entries; i++) {
        const UINT8 *t = (const UINT8 *)(UINTN)rd64(xsdt + ACPI_HDR_SIZE + i * 8u);
        if (!t) continue;
        const ACPI_SDT_HDR *hdr = (const ACPI_SDT_HDR *)t;
        if (ascii_eq_n(hdr->Signature, "M2HY", 4u)) {
            log_str(L, "  [ACPI] M2HY table already present in XSDT"); log_nl(L);
            return EFI_ALREADY_STARTED;
        }
        if (ascii_eq_n(hdr->Signature, "MCFG", 4u) &&
            hdr->Length >= sizeof(ACPI_SDT_HDR) + 8u + sizeof(ACPI_MCFG_ENTRY)) {
            const ACPI_MCFG_ENTRY *me = (const ACPI_MCFG_ENTRY *)(t + sizeof(ACPI_SDT_HDR) + 8u);
            if (me->BaseAddress >= 0xC0000000ULL && me->BaseAddress < 0x100000000ULL) {
                gEcamBase = me->BaseAddress;
            }
        }
        if (orig_vfct == NULL && ascii_eq_n(hdr->Signature, "VFCT", 4u)) {
            vfct_idx = i;
            orig_vfct = t;
        }
        if (orig_peg == NULL && ascii_eq_n(hdr->Signature, "SSDT", 4u) &&
            ascii_eq_n(hdr->OemTableId, "PEG0GFX0", 8u)) {
            peg_idx = i;
            orig_peg = t;
        }
    }

    VOID *ssdt_file = NULL;
    UINTN ssdt_size = 0;
    UINT8 *dsdt = NULL;
    UINT32 dlen = 0;
    UINTN off_bcm = 0, off_pts = 0, off_wak = 0;

    if (IncludeSsdt) {
        static CHAR16 SsdtHybridPath[] = L"\\EFI\\tools\\SSDT_HYBRID_MBP133.aml";
        static CHAR16 SsdtIgpuPath[]   = L"\\EFI\\tools\\SSDT_IGPU_MBP133.aml";
        BOOLEAN is_hybrid_ssdt = (Mode == M2_MODE_HYBRID);
        CHAR16 *ssdt_path = is_hybrid_ssdt ? SsdtHybridPath : SsdtIgpuPath;
        const char *ssdt_name = is_hybrid_ssdt ? "SSDT_HYBRID_MBP133.aml" : "SSDT_IGPU_MBP133.aml";
        EFI_STATUS sst = ReadUsbFile(BS, ImageHandle, ssdt_path, &ssdt_file, &ssdt_size);
        if (EFI_ERROR(sst) || !ssdt_file) {
            log_str(L, "  [ACPI] Read "); log_str(L, ssdt_name);
            log_str(L, " failed: 0x"); log_hex(L, sst, 16); log_nl(L);
            return EFI_ERROR(sst) ? sst : EFI_NOT_FOUND;
        }
        const UINT8 *aml = (const UINT8 *)ssdt_file;
        if (ssdt_size < ACPI_HDR_SIZE || !ascii_eq_n(aml, "SSDT", 4u) ||
            rd32(aml + ACPI_HDR_LEN) != (UINT32)ssdt_size || sum8(aml, ssdt_size) != 0u) {
            log_str(L, "  [ACPI] Invalid "); log_str(L, ssdt_name);
            log_str(L, " (sig/len/csum)"); log_nl(L);
            BS->FreePool(ssdt_file);
            return EFI_COMPROMISED_DATA;
        }

        for (UINTN i = 0; i < n_entries; i++) {
            const UINT8 *t = (const UINT8 *)(UINTN)rd64(xsdt + ACPI_HDR_SIZE + i * 8u);
            if (t && ascii_eq_n(t, "SSDT", 4u) &&
                ascii_eq_n(t + ACPI_HDR_OEMTID, (const char *)(aml + ACPI_HDR_OEMTID), 8u) &&
                rd32(t + ACPI_HDR_LEN) == (UINT32)ssdt_size) {
                log_str(L, "  [ACPI] "); log_str(L, ssdt_name); log_str(L, " already in XSDT"); log_nl(L);
                BS->FreePool(ssdt_file);
                return EFI_ALREADY_STARTED;
            }
        }
        log_str(L, "  [ACPI] Loaded "); log_str(L, ssdt_name);
        log_str(L, " ("); log_dec(L, ssdt_size); log_str(L, " B)"); log_nl(L);

        dsdt = FindDsdtViaFadt(xsdt, n_entries);
        if (!dsdt || !ascii_eq_n(dsdt, "DSDT", 4u)) {
            log_str(L, "  [ACPI] DSDT not found via FACP"); log_nl(L);
            BS->FreePool(ssdt_file);
            return EFI_NOT_FOUND;
        }
        dlen = rd32(dsdt + ACPI_HDR_LEN);
        if (dlen < ACPI_HDR_SIZE || sum8(dsdt, dlen) != 0u) {
            log_str(L, "  [ACPI] DSDT checksum/len invalid"); log_nl(L);
            BS->FreePool(ssdt_file);
            return EFI_COMPROMISED_DATA;
        }

        UINTN c_bcm = 0, c_xbcm = 0, c_pts = 0, c_xpts = 0, c_wak = 0, c_xwak = 0;
        off_bcm = FindMethodInAml(dsdt, dlen, "_BCM", &c_bcm);
        (void)FindMethodInAml(dsdt, dlen, "XBCM", &c_xbcm);
        off_pts = FindMethodInAml(dsdt, dlen, "_PTS", &c_pts);
        (void)FindMethodInAml(dsdt, dlen, "XPTS", &c_xpts);
        off_wak = FindMethodInAml(dsdt, dlen, "_WAK", &c_wak);
        (void)FindMethodInAml(dsdt, dlen, "XWAK", &c_xwak);

        if (c_bcm != 1u || off_bcm == 0u || c_xbcm != 0u ||
            c_pts != 1u || off_pts == 0u || c_xpts != 0u ||
            c_wak != 1u || off_wak == 0u || c_xwak != 0u) {
            log_str(L, "  [ACPI] Unexpected DSDT layout, aborted"); log_nl(L);
            BS->FreePool(ssdt_file);
            return EFI_UNSUPPORTED;
        }
    }

    UINTN added_entries = IncludeSsdt ? 2u : 1u;
    UINT32 new_xlen = xlen + (UINT32)(added_entries * 8u);
    UINTN rlen = (rsdp->Length < sizeof(ACPI_RSDP_20)) ? sizeof(ACPI_RSDP_20) : rsdp->Length;

    M2HY_ACPI_TABLE *tbl = NULL;
    UINT8 *new_ssdt = NULL;
    UINT8 *new_xsdt = NULL;
    ACPI_RSDP_20 *new_rsdp = NULL;

    if (EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, sizeof(M2HY_ACPI_TABLE), (VOID **)&tbl)) ||
        (IncludeSsdt && EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, ssdt_size, (VOID **)&new_ssdt))) ||
        EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, new_xlen, (VOID **)&new_xsdt)) ||
        EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, rlen, (VOID **)&new_rsdp))) {
        log_str(L, "  [ACPI] AllocatePool(ACPIReclaim) failed"); log_nl(L);
        if (tbl) BS->FreePool(tbl);
        if (new_ssdt) BS->FreePool(new_ssdt);
        if (new_xsdt) BS->FreePool(new_xsdt);
        if (ssdt_file) BS->FreePool(ssdt_file);
        return EFI_OUT_OF_RESOURCES;
    }

    mem_zero(tbl, sizeof(*tbl));
    mem_copy(tbl->Hdr.Signature, "M2HY", 4u);
    tbl->Hdr.Length          = sizeof(M2HY_ACPI_TABLE);
    tbl->Hdr.Revision        = 1u;
    mem_copy(tbl->Hdr.OemId, "MBP133", 6u);
    mem_copy(tbl->Hdr.OemTableId, "M2HYBRID", 8u);
    tbl->Hdr.OemRevision     = 1u;
    mem_copy(&tbl->Hdr.CreatorId, "M2HY", 4u);
    tbl->Hdr.CreatorRevision = 1u;
    tbl->Version             = 1u;
    tbl->Mode                = Mode;
    tbl->Flags               = InitialFlags;
    tbl->EcamBase            = gEcamBase;

    UINT8 old_dcsum = 0;
    if (IncludeSsdt) {
        mem_copy(new_ssdt, ssdt_file, ssdt_size);
        old_dcsum = dsdt[ACPI_HDR_CSUM];
        volatile UINT8 *vdsdt = (volatile UINT8 *)dsdt;
        vdsdt[off_bcm] = 'X';
        vdsdt[off_pts] = 'X';
        vdsdt[off_wak] = 'X';
        vdsdt[ACPI_HDR_CSUM] = 0u;
        vdsdt[ACPI_HDR_CSUM] = (UINT8)(0u - sum8(dsdt, dlen));
        if (vdsdt[off_bcm] != 'X' || vdsdt[off_pts] != 'X' || vdsdt[off_wak] != 'X' || sum8(dsdt, dlen) != 0u) {
            vdsdt[off_bcm] = '_';
            vdsdt[off_pts] = '_';
            vdsdt[off_wak] = '_';
            vdsdt[ACPI_HDR_CSUM] = old_dcsum;
            log_str(L, "  [ACPI] DSDT memory not writable, reverted"); log_nl(L);
            BS->FreePool(tbl); BS->FreePool(new_ssdt); BS->FreePool(new_xsdt); BS->FreePool(new_rsdp);
            BS->FreePool(ssdt_file);
            return EFI_ACCESS_DENIED;
        }
        tbl->Flags |= M2_FLAG_SSDT_INJECTED;
    }

    mem_copy(new_xsdt, xsdt, xlen);
    UINT8 *new_vfct = NULL;
    UINT8 *new_peg  = NULL;
    if (Mode == M2_MODE_HYBRID) {
        if (orig_vfct != NULL) {
            new_vfct = PatchVfctRemoveEdp(BS, orig_vfct, L);
            if (new_vfct != NULL) {
                UINT64 vpa = (UINT64)(UINTN)new_vfct;
                mem_copy(new_xsdt + ACPI_HDR_SIZE + vfct_idx * 8u, &vpa, 8u);
                tbl->Flags |= M2_FLAG_VFCT_PATCHED;
            }
        }
        if (orig_peg != NULL) {
            new_peg = PatchPeg0Gfx0Dod(BS, orig_peg, L);
            if (new_peg != NULL) {
                UINT64 ppa = (UINT64)(UINTN)new_peg;
                mem_copy(new_xsdt + ACPI_HDR_SIZE + peg_idx * 8u, &ppa, 8u);
                tbl->Flags |= M2_FLAG_GFX0_DOD_PATCHED;
            }
        }
    }

    UpdateM2Checksum(tbl);

    UINTN cursor = xlen;
    if (IncludeSsdt) {
        UINT64 ssdt_pa = (UINT64)(UINTN)new_ssdt;
        mem_copy(new_xsdt + cursor, &ssdt_pa, 8u);
        cursor += 8u;
    }
    UINT64 tbl_pa = (UINT64)(UINTN)tbl;
    mem_copy(new_xsdt + cursor, &tbl_pa, 8u);

    mem_copy(new_xsdt + ACPI_HDR_LEN, &new_xlen, 4u);
    new_xsdt[ACPI_HDR_CSUM] = 0u;
    new_xsdt[ACPI_HDR_CSUM] = (UINT8)(0u - sum8(new_xsdt, new_xlen));

    mem_copy(new_rsdp, rsdp, rlen);
    new_rsdp->XsdtAddress = (UINT64)(UINTN)new_xsdt;
    new_rsdp->ExtChecksum = 0u;
    new_rsdp->ExtChecksum = (UINT8)(0u - sum8((const UINT8 *)new_rsdp, new_rsdp->Length));

    EFI_STATUS st = BS->InstallConfigurationTable(&gAcpi20Guid, new_rsdp);
    if (EFI_ERROR(st)) {
        if (IncludeSsdt) {
            volatile UINT8 *vdsdt = (volatile UINT8 *)dsdt;
            vdsdt[off_bcm] = '_';
            vdsdt[off_pts] = '_';
            vdsdt[off_wak] = '_';
            vdsdt[ACPI_HDR_CSUM] = old_dcsum;
            BS->FreePool(new_ssdt);
        }
        if (new_vfct) BS->FreePool(new_vfct);
        if (new_peg)  BS->FreePool(new_peg);
        log_str(L, "  [ACPI] InstallConfigurationTable failed: 0x"); log_hex(L, st, 16); log_nl(L);
        BS->FreePool(tbl); BS->FreePool(new_xsdt); BS->FreePool(new_rsdp);
        if (ssdt_file) BS->FreePool(ssdt_file);
        return st;
    }

    volatile ACPI_RSDP_20 *old_rsdp = (volatile ACPI_RSDP_20 *)rsdp;
    UINT64 saved_x = old_rsdp->XsdtAddress;
    UINT8  saved_e = old_rsdp->ExtChecksum;
    old_rsdp->XsdtAddress = new_rsdp->XsdtAddress;
    old_rsdp->ExtChecksum = new_rsdp->ExtChecksum;
    BOOLEAN old_synced = (old_rsdp->XsdtAddress == new_rsdp->XsdtAddress);
    if (!old_synced) {
        old_rsdp->XsdtAddress = saved_x;
        old_rsdp->ExtChecksum = saved_e;
    }

    if (ssdt_file) BS->FreePool(ssdt_file);
    gM2Table = tbl;

    log_str(L, "  [ACPI] Installed M2HY@0x"); log_hex(L, tbl_pa, 8);
    if (IncludeSsdt) {
        log_str(L, " + SSDT@0x"); log_hex(L, (UINT64)(UINTN)new_ssdt, 8);
    }
    log_str(L, " XSDT entries="); log_dec(L, n_entries + added_entries);
    log_str(L, old_synced ? " (old RSDP synced)" : " (via config table)"); log_nl(L);
    return EFI_SUCCESS;
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

/* ---- Apple NVRAM gpu-power-prefs Helpers ---- */

static void GpuPrefsRead(EFI_RUNTIME_SERVICES *RT, GPU_PREFS *P) {
    UINT8 buf[16];
    UINTN sz = sizeof(buf);
    UINT32 attr = 0u;

    mem_zero(P, sizeof(*P));
    P->Status = RT->GetVariable(GPU_PREFS_NAME, &gAppleGpuPrefsGuid, &attr, &sz, buf);
    if (P->Status == EFI_SUCCESS || P->Status == EFI_BUFFER_TOO_SMALL) {
        P->Known = TRUE;
        P->Present = TRUE;
        P->Size = sz;
        for (UINTN i = 0; i < sizeof(P->Data) && i < sz && i < sizeof(buf); i++) {
            P->Data[i] = buf[i];
        }
    } else if (P->Status == EFI_NOT_FOUND) {
        P->Known = TRUE;
    }
}

static BOOLEAN GpuPrefsIsIGpu(const GPU_PREFS *P) {
    return (P->Known && P->Present && P->Size >= 1u && P->Data[0] == 0x01u) ? TRUE : FALSE;
}

static EFI_STATUS GpuPrefsApply(EFI_RUNTIME_SERVICES *RT, BOOLEAN ToIGpu) {
    GPU_PREFS p;
    EFI_STATUS st;

    if (ToIGpu) {
        UINT8 val[4] = { 0x01u, 0x00u, 0x00u, 0x00u };
        st = RT->SetVariable(GPU_PREFS_NAME, &gAppleGpuPrefsGuid, GPU_PREFS_ATTR, sizeof(val), val);
    } else {
        st = RT->SetVariable(GPU_PREFS_NAME, &gAppleGpuPrefsGuid, GPU_PREFS_ATTR, 0u, NULL);
        if (st == EFI_NOT_FOUND) st = EFI_SUCCESS;
    }
    if (EFI_ERROR(st)) return st;

    GpuPrefsRead(RT, &p);
    if (ToIGpu) {
        return (p.Present && p.Size == 4u && p.Data[0] == 0x01u) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    }
    return (p.Known && !p.Present) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

static void LogGpuPrefs(EFI_RUNTIME_SERVICES *RT, LOG_BUF *L) {
    GPU_PREFS p;
    GpuPrefsRead(RT, &p);
    log_str(L, "  [NVRAM gpu-power-prefs] ");
    if (!p.Known) {
        log_str(L, "READ_ERR=0x"); log_hex(L, p.Status, 16);
    } else if (!p.Present) {
        log_str(L, "dGPU (not set)");
    } else {
        log_str(L, GpuPrefsIsIGpu(&p) ? "iGPU (" : "OTHER (");
        log_str(L, "sz="); log_dec(L, p.Size); log_str(L, " bytes=");
        for (UINTN i = 0; i < p.Size && i < 4u; i++) {
            if (i > 0u) log_putc(L, ' ');
            log_hex(L, p.Data[i], 2);
        }
        log_putc(L, ')');
    }
    log_nl(L);
}

/* ---- Mode Runner ---- */

static void ExecuteM2Mode(EFI_BOOT_SERVICES *BS, EFI_SYSTEM_TABLE *ST,
                          EFI_HANDLE ImageHandle, LOG_BUF *L, UINT32 Mode) {
    SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut = ST->ConOut;
    SIMPLE_INPUT_INTERFACE *ConIn = ST->ConIn;
    BOOLEAN want_intel_display = (Mode == M2_MODE_HYBRID || Mode == M2_MODE_INTEL_ONLY);
    BOOLEAN want_rail_off      = (Mode == M2_MODE_INTEL_ONLY);
    BOOLEAN want_setos         = (Mode != M2_MODE_NORMAL);
    UINT32  flags              = 0u;

    L->Len = 0;
    L->Truncated = FALSE;
    if (L->Buf) L->Buf[0] = 0;

    ConOut->ClearScreen(ConOut);
    ConPrintA(ConOut,
        "========================================================================\n"
        "  MacBookPro13,3 Milestone 2 — Executing Mode ");
    ConPrintHex(ConOut, Mode, 1);
    ConPrintA(ConOut, "\n");
    if (want_intel_display) {
        ConPrintA(ConOut,
            "  NOTE: After GMUX switches panel to Intel HD 530, screen goes DARK\n"
            "  for ~10-25s until Windows igdkmd64.sys initializes the eDP link.\n");
    }
    ConPrintA(ConOut,
        "  Logs flush to USB \\M2_LOGS\\ BEFORE Windows boots.\n"
        "========================================================================\n\n");

    log_str(L, "MBP133_M2_HYBRID_V1"); log_nl(L);
    log_str(L, "MODE="); log_dec(L, Mode);
    log_str(L, (Mode == M2_MODE_HYBRID)     ? " (Hybrid Mode 5: Intel Display + AMD On + 3-Reg FB)" :
               (Mode == M2_MODE_INTEL_ONLY) ? " (Intel-Only Mode 4: Intel Display + AMD Rail OFF)" :
               (Mode == M2_MODE_AMD_DUAL)   ? " (Dual-GPU AMD Display + Intel Headless + 3-Reg FB)" :
                                              " (Normal Boot Camp + M2HY X-Ray)");
    log_nl(L);
    LogGpuPrefs(ST->RuntimeServices, L);

    if (want_intel_display) {
        LogGmuxState(BS, L, "BEFORE");
    }

    if (want_setos) {
        ConPrintA(ConOut, "Invoking AppleSetOs(\"Mac OS X 10.15\")...\n");
        if (!EFI_ERROR(InvokeAppleSetOs(BS, ImageHandle, L))) {
            flags |= M2_FLAG_SETOS_OK;
        }
    } else {
        log_str(L, "  [AppleSetOs] Skipped (Normal Boot Camp mode)"); log_nl(L);
    }

    if (want_intel_display) {
        ConPrintA(ConOut, "Switching GMUX panel -> Intel HD 530...\n");
        BS->Stall(300000);
        BOOLEAN rail_off_done = FALSE;
        if (GmuxSwitchToIgd(BS, want_rail_off, &rail_off_done, L)) {
            flags |= M2_FLAG_GMUX_IGD_OK;
        }
        if (rail_off_done) {
            flags |= M2_FLAG_GMUX_RAIL_OFF;
        }
        BS->Stall(800000);
        LogGmuxState(BS, L, "AFTER");

        EFI_PCI_IO_PROTOCOL *igpu = FindIgpu(BS, ImageHandle, L);
        if (ProbeEdpAndForce4Lanes(BS, igpu, L)) {
            flags |= M2_FLAG_DDI_4LANES_OK;
        }
        if (PatchVbtBacklightNone(BS, ImageHandle, igpu, L)) {
            flags |= M2_FLAG_VBT_BDB43_OK;
        }
    }

    EFI_STATUS ast = InstallM2AcpiTables(BS, ST, ImageHandle, Mode, want_intel_display, flags, L);
    if (EFI_ERROR(ast)) {
        WriteUsbFile(BS, ImageHandle, L"\\M2_LOGS\\m2_last.txt", L->Buf, L->Len);
        ConPrintA(ConOut, "\nERROR: InstallM2AcpiTables failed. Press any key...\n");
        FlushKeys(ConIn);
        EFI_INPUT_KEY k;
        ReadKeyWait(BS, ConIn, &k);
        return;
    }

    gOrigExitBootServices = BS->ExitBootServices;
    BS->ExitBootServices  = HookedExitBootServices;
    BS->Hdr.CRC32         = 0u;
    BS->CalculateCrc32((VOID *)&BS->Hdr, BS->Hdr.HeaderSize, &BS->Hdr.CRC32);
    log_str(L, "  [EBS Hook] Installed + BS CRC32 updated"); log_nl(L);

    log_str(L, "PRE_EBS_FLAGS=0x"); log_hex(L, gM2Table->Flags, 8);
    log_str(L, " LOG_BYTES="); log_dec(L, L->Len);
    log_str(L, " TRUNCATED="); log_dec(L, L->Truncated ? 1u : 0u);
    log_nl(L);

    CHAR16 *mode_file = (Mode == M2_MODE_HYBRID)     ? L"\\M2_LOGS\\m2_mode1.txt" :
                        (Mode == M2_MODE_INTEL_ONLY) ? L"\\M2_LOGS\\m2_mode2.txt" :
                        (Mode == M2_MODE_AMD_DUAL)   ? L"\\M2_LOGS\\m2_mode3.txt" :
                                                       L"\\M2_LOGS\\m2_mode4.txt";
    WriteUsbFile(BS, ImageHandle, mode_file, L->Buf, L->Len);
    WriteUsbFile(BS, ImageHandle, L"\\M2_LOGS\\m2_last.txt", L->Buf, L->Len);
    BS->Stall(300000);

    EFI_STATUS bst = ChainloadWindows(BS, ImageHandle, ConOut);
    if (gOrigExitBootServices != NULL && BS->ExitBootServices == HookedExitBootServices) {
        BS->ExitBootServices = gOrigExitBootServices;
        BS->Hdr.CRC32        = 0u;
        BS->CalculateCrc32((VOID *)&BS->Hdr, BS->Hdr.HeaderSize, &BS->Hdr.CRC32);
    }
    if (EFI_ERROR(bst)) {
        ConPrintA(ConOut, "\nChainload failed. Press any key...\n");
        FlushKeys(ConIn);
        EFI_INPUT_KEY k;
        ReadKeyWait(BS, ConIn, &k);
    }
}

/* ---- Optional LoadOptions Parsing for One-Click rEFInd Entries ---- */

static UINT32 ParseAutoMode(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle) {
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    if (EFI_ERROR(BS->HandleProtocol(ImageHandle, &gLoadedImageGuid, (VOID **)&li)) ||
        !li || !li->LoadOptions || li->LoadOptionsSize < sizeof(CHAR16)) {
        return 0u;
    }
    const CHAR16 *opts = (const CHAR16 *)li->LoadOptions;
    UINTN nchars = li->LoadOptionsSize / sizeof(CHAR16);
    if (contains_ci16(opts, nchars, "mode=1") || contains_ci16(opts, nchars, "hybrid")) {
        return M2_MODE_HYBRID;
    }
    if (contains_ci16(opts, nchars, "mode=2") || contains_ci16(opts, nchars, "igpu")) {
        return M2_MODE_INTEL_ONLY;
    }
    if (contains_ci16(opts, nchars, "mode=3") || contains_ci16(opts, nchars, "amd-dual")) {
        return M2_MODE_AMD_DUAL;
    }
    if (contains_ci16(opts, nchars, "mode=4") || contains_ci16(opts, nchars, "normal")) {
        return M2_MODE_NORMAL;
    }
    return 0u;
}

/* ---- UEFI Entry Point ---- */

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_BOOT_SERVICES *BS = SystemTable->BootServices;
    EFI_RUNTIME_SERVICES *RT = SystemTable->RuntimeServices;
    SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut = SystemTable->ConOut;
    SIMPLE_INPUT_INTERFACE *ConIn = SystemTable->ConIn;

    ConSetModeText(BS, ConOut, TRUE);

    LOG_BUF L;
    mem_zero(&L, sizeof(L));
    L.Cap = LOG_CAP;
    if (EFI_ERROR(BS->AllocatePool(EfiBootServicesData, L.Cap, (VOID **)&L.Buf)) || !L.Buf) {
        ConPrintA(ConOut, "FATAL: AllocatePool(128KB) failed\n");
        return EFI_OUT_OF_RESOURCES;
    }

    UINT32 auto_mode = ParseAutoMode(BS, ImageHandle);
    if (auto_mode != 0u) {
        ExecuteM2Mode(BS, SystemTable, ImageHandle, &L, auto_mode);
    }

    while (1) {
        GPU_PREFS prefs;
        GpuPrefsRead(RT, &prefs);
        BOOLEAN is_igpu = GpuPrefsIsIGpu(&prefs);

        ConOut->ClearScreen(ConOut);
        ConPrintA(ConOut,
            "========================================================================\n"
            "  MacBookPro13,3 Milestone 2 — Unified Hybrid Dual-GPU Loader\n"
            "  Intel HD 530 + AMD Radeon Pro 455 | USB-only (NVMe ESP untouched)\n"
            "========================================================================\n\n"
            "  NVRAM gpu-power-prefs: ");
        if (!prefs.Known) {
            ConPrintA(ConOut, "READ ERROR 0x");
            ConPrintHex(ConOut, prefs.Status, 16);
            ConPrintA(ConOut, "\n\n");
        } else if (!prefs.Present) {
            ConPrintA(ConOut, "dGPU (not set)    [Press S -> iGPU for Mode 1]\n\n");
        } else if (is_igpu) {
            ConPrintA(ConOut, "iGPU (01 00 00 00) [Ready for Mode 1 Hybrid]\n\n");
        } else {
            ConPrintA(ConOut, "CUSTOM bytes=");
            for (UINTN i = 0; i < prefs.Size && i < 4u; i++) {
                if (i > 0u) ConPrintA(ConOut, " ");
                ConPrintHex(ConOut, prefs.Data[i], 2);
            }
            ConPrintA(ConOut, "\n\n");
        }

        ConPrintA(ConOut,
            "  [1] Hybrid Mode 5     (Intel Display + AMD On + VFCT + 3-Reg + SSDT)\n"
            "  [2] Intel-Only Mode 4 (Intel Display + AMD Rail OFF + SSDT, ~9.3W)\n"
            "  [3] Dual-GPU AMD Disp (AMD Display + Intel Headless + 3-Reg FB)\n"
            "  [4] Normal Boot Camp  (No SetOS, No GMUX, No SSDT + M2HY X-Ray)\n");
        if (is_igpu) {
            ConPrintA(ConOut, "  [S] Switch gpu-power-prefs -> dGPU (delete) + Cold Reboot\n");
        } else {
            ConPrintA(ConOut, "  [S] Switch gpu-power-prefs -> iGPU + Cold Reboot\n");
        }
        ConPrintA(ConOut,
            "  [D] Force Delete gpu-power-prefs (Reset to dGPU) + Cold Reboot\n"
            "  [0] Cancel / Direct Windows Boot (zero changes)\n"
            "  [R] Cold Reboot\n\n"
            "Select option: ");

        FlushKeys(ConIn);
        EFI_INPUT_KEY k;
        mem_zero(&k, sizeof(k));
        ReadKeyWait(BS, ConIn, &k);
        CHAR16 c = k.UnicodeChar;

        if (c == L'1') {
            ExecuteM2Mode(BS, SystemTable, ImageHandle, &L, M2_MODE_HYBRID);
        } else if (c == L'2') {
            ExecuteM2Mode(BS, SystemTable, ImageHandle, &L, M2_MODE_INTEL_ONLY);
        } else if (c == L'3') {
            ExecuteM2Mode(BS, SystemTable, ImageHandle, &L, M2_MODE_AMD_DUAL);
        } else if (c == L'4') {
            ExecuteM2Mode(BS, SystemTable, ImageHandle, &L, M2_MODE_NORMAL);
        } else if (c == L's' || c == L'S' || c == L'd' || c == L'D') {
            BOOLEAN to_igpu = ((c == L's' || c == L'S') && !is_igpu);
            ConPrintA(ConOut, to_igpu
                ? "\nWriting NVRAM gpu-power-prefs = iGPU (01 00 00 00)...\n"
                : "\nDeleting NVRAM gpu-power-prefs (reset to dGPU default)...\n");
            EFI_STATUS pst = GpuPrefsApply(RT, to_igpu);
            if (!EFI_ERROR(pst)) {
                ConPrintA(ConOut, "Verified OK! Cold rebooting in 1.5s so Apple EFI applies it...\n");
                BS->Stall(1500000);
                RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
            } else {
                ConPrintA(ConOut, "ERROR: SetVariable/verify failed: 0x");
                ConPrintHex(ConOut, pst, 16);
                ConPrintA(ConOut, " (NOT rebooting). Press any key...\n");
                FlushKeys(ConIn);
                ReadKeyWait(BS, ConIn, &k);
            }
        } else if (c == L'0') {
            ConPrintA(ConOut, "\nDirect Windows Boot (zero changes)...\n");
            EFI_STATUS bst = ChainloadWindows(BS, ImageHandle, ConOut);
            if (EFI_ERROR(bst)) {
                FlushKeys(ConIn);
                ReadKeyWait(BS, ConIn, &k);
            }
        } else if (c == L'r' || c == L'R') {
            ConPrintA(ConOut, "\nRebooting...\n");
            BS->Stall(300000);
            RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
        } else if (c == L'q' || c == L'Q' || k.ScanCode == 0x17u) {
            break;
        }
    }

    BS->FreePool(L.Buf);
    return EFI_SUCCESS;
}

/* gnu-efi 4.0.0 crt0-efi-x86_64.o invokes _entry(ImageHandle, SystemTable) */
EFI_STATUS _entry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    return efi_main(ImageHandle, SystemTable);
}
