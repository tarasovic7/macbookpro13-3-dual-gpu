/*
 * mbp133_m1a_probe.c - Milestone 1A UEFI Telemetry & SetOS Isolation Probe
 * Target: Apple MacBookPro13,3 (Skylake i7-6920HQ + Intel HD 530 + AMD Radeon Pro 455 Polaris 11)
 *
 * Read-only hardware telemetry before and after EFI_APPLE_SET_OS_PROTOCOL:
 *   - PCI Config Space (00:00.0 DEVEN, 00:01.0 PEG0, 00:02.0 IGPU, 01:00.0/1 AMD) + ECAM
 *   - AMD Polaris 11 BAR5 MMIO + SMC_IND (0x1AC/0x1AD) SMU7 registers (non-destructive)
 *   - Intel HD 530 OpRegion/VBT/ASLE + DDI_BUF_CTL_A + PWR_WELL_CTL2 + PP_STATUS
 *   - Apple Pre-T2 Indexed I/O GMUX (Renesas R4F2113 @ 0x700..0x7FE, read-only)
 *   - ACPI RSDP/XSDT/MCFG/DMAR/VFCT + VT-d IOMMU DRHD MMIO status
 *   - Saves log to \M1A_LOGS\probe_*.txt on USB ESP and optionally chainloads Windows
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

#define APPLE_NVRAM_GPU_PREFS_GUID \
    { 0xfa4ce28d, 0xb62f, 0x4c99, { 0x9c, 0xc3, 0x68, 0x15, 0x68, 0x6e, 0x30, 0xf9 } }

#ifndef EFI_ACPI_20_TABLE_GUID
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }
#endif

static EFI_GUID gConsoleCtrlGuid    = EFI_CONSOLE_CONTROL_PROTOCOL_GUID;
static EFI_GUID gAppleSetOsGuid     = EFI_APPLE_SET_OS_PROTOCOL_GUID;
static EFI_GUID gGpuPrefsGuid       = APPLE_NVRAM_GPU_PREFS_GUID;
static EFI_GUID gAcpi20Guid         = EFI_ACPI_20_TABLE_GUID;
static EFI_GUID gLoadedImageGuid    = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID gDevicePathGuid     = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID gSimpleFsGuid       = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID gPciIoGuid          = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID gGopGuid            = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID gEdidActiveGuid     = EFI_EDID_ACTIVE_PROTOCOL_GUID;
static EFI_GUID gEdidDiscoveredGuid = EFI_EDID_DISCOVERED_PROTOCOL_GUID;

/* ---- Polaris 11 (Baffin / SMU 7.1.3) Register Definitions ---- */

#define mmSMC_IND_INDEX_0           0x0080u
#define mmSMC_IND_DATA_0            0x0081u
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

/* ---- Apple Indexed I/O GMUX (Renesas R4F2113) ---- */

#define GMUX_IO_VALUE               0x07C2u
#define GMUX_IO_PROBE0              0x07CCu
#define GMUX_IO_PROBE1              0x07CDu
#define GMUX_IO_PROBE2              0x07CEu
#define GMUX_IO_READ                0x07D0u
#define GMUX_IO_WRITE               0x07D4u

/* ---- ACPI Packed Structures ---- */

#pragma pack(1)
typedef struct {
    char    Signature[8];
    UINT8   Checksum;
    char    OemId[6];
    UINT8   Revision;
    UINT32  RsdtAddress;
    UINT32  Length;
    UINT64  XsdtAddress;
    UINT8   ExtendedChecksum;
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
#pragma pack()

/* ---- Summary & Context ---- */

typedef struct {
    UINT32  Deven;
    UINT16  IgpuVid;
    UINT16  IgpuDid;
    UINT32  IgpuBar0Lo;
    UINT32  IgpuBar2Lo;
    UINT32  IgpuAsls;
    BOOLEAN OpRegionValid;
    BOOLEAN VbtPresent;
    UINT32  DdiBufCtlA;
    UINT32  AmdCmd;
    UINT32  AmdPmcsr;
    UINT32  AmdBar5;
    UINT32  CfgMemSize;
    UINT32  GrbmStatus;
    UINT32  SmcRst;
    UINT32  SmcClk;
    UINT32  SmcPc1;
    UINT32  SmcPc2;
    UINT32  RcuEvents;
    UINT32  SmuStatus;
    UINT32  SmuFirmware;
    UINT32  FwFlags;
    BOOLEAN Bar5Probed;
    BOOLEAN SmcIndProbed;
    BOOLEAN GmuxIndexed;
    UINT8   GmuxDisp;
    UINT8   GmuxDgpuPwr;
} PROBE_SUMMARY;

typedef struct {
    UINT64  EcamBase;
    UINT8   EcamStartBus;
    UINT8   EcamEndBus;
    BOOLEAN HasEcam;
    UINT64  DrhdBases[4];
    UINTN   DrhdCount;
    BOOLEAN EdidDumped;
} PROBE_ENV;

/* ---- Minimal Freestanding Helpers ---- */

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

static BOOLEAN ascii_eq_n(const char *a, const char *b, UINTN n) {
    for (UINTN i = 0; i < n; i++) {
        if (a[i] != b[i]) return FALSE;
    }
    return TRUE;
}

/* ---- Dynamic ASCII Log Buffer ---- */

#define LOG_CAP (256u * 1024u)

typedef struct {
    char   *Buf;
    UINTN   Len;
    UINTN   Cap;
    BOOLEAN Truncated;
} LOG_BUF;

static void log_putc(LOG_BUF *L, char c) {
    if (!L || !L->Buf) return;
    if (L->Len + 1 >= L->Cap) {
        L->Truncated = TRUE;
        return;
    }
    L->Buf[L->Len++] = c;
    L->Buf[L->Len] = 0;
}

static void log_str(LOG_BUF *L, const char *s) {
    if (!s) return;
    while (*s) {
        log_putc(L, *s++);
    }
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
    for (UINTN i = digits; i > 0; i--) {
        UINTN shift = (i - 1u) * 4u;
        log_putc(L, hex[(v >> shift) & 0xFu]);
    }
}

static void log_dec(LOG_BUF *L, UINT64 v) {
    char tmp[24];
    UINTN n = 0;
    if (v == 0) {
        log_putc(L, '0');
        return;
    }
    while (v > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) log_putc(L, tmp[--n]);
}

/* ---- Console Output Helpers ---- */

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
        if (EFI_ERROR(ws)) {
            BS->Stall(20000);
        }
        EFI_STATUS rs = ConIn->ReadKeyStroke(ConIn, Key);
        if (!EFI_ERROR(rs)) {
            return rs;
        }
        BS->Stall(20000);
    }
}

static void FlushKeys(SIMPLE_INPUT_INTERFACE *ConIn) {
    EFI_INPUT_KEY k;
    while (!EFI_ERROR(ConIn->ReadKeyStroke(ConIn, &k))) { }
}

/* ---- Low-Level x86_64 Port I/O & MMIO ---- */

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

static inline UINT32 mmio_r32(UINT64 addr) {
    return *(volatile UINT32 *)(UINTN)addr;
}

static inline void mmio_w32(UINT64 addr, UINT32 v) {
    *(volatile UINT32 *)(UINTN)addr = v;
}

static inline UINT8 mmio_r8(UINT64 addr) {
    return *(volatile UINT8 *)(UINTN)addr;
}

static inline UINT64 mmio_r64(UINT64 addr) {
    UINT32 lo = mmio_r32(addr);
    UINT32 hi = mmio_r32(addr + 4u);
    return ((UINT64)hi << 32) | (UINT64)lo;
}

/* Legacy PCI Config Mechanism #1 (0xCF8 / 0xCFC) */

static UINT32 pci_rd32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
    UINT32 addr = 0x80000000u
                | ((UINT32)bus << 16)
                | ((UINT32)(dev & 0x1Fu) << 11)
                | ((UINT32)(func & 0x07u) << 8)
                | (UINT32)(off & 0xFCu);
    __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((UINT16)0x0CF8));
    return io_in32(0x0CFCu);
}

static UINT16 pci_rd16(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
    UINT32 v = pci_rd32(bus, dev, func, off);
    return (UINT16)((v >> ((off & 2u) * 8u)) & 0xFFFFu);
}

static UINT8 pci_rd8(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off) {
    UINT32 v = pci_rd32(bus, dev, func, off);
    return (UINT8)((v >> ((off & 3u) * 8u)) & 0xFFu);
}

static UINT32 ecam_rd32(const PROBE_ENV *Env, UINT8 bus, UINT8 dev, UINT8 func, UINT16 off) {
    if (!Env->HasEcam || bus < Env->EcamStartBus || bus > Env->EcamEndBus || off >= 0x1000u) {
        return 0xFFFFFFFFu;
    }
    UINT64 addr = Env->EcamBase
                + ((UINT64)(bus - Env->EcamStartBus) << 20)
                + ((UINT64)(dev & 0x1Fu) << 15)
                + ((UINT64)(func & 0x07u) << 12)
                + (UINT64)(off & 0xFFCu);
    return mmio_r32(addr);
}

static void DumpPci256(LOG_BUF *L, const char *Name, UINT8 bus, UINT8 dev, UINT8 func) {
    log_str(L, "  [PCI ");
    log_hex(L, bus, 2); log_putc(L, ':');
    log_hex(L, dev, 2); log_putc(L, '.');
    log_hex(L, func, 1); log_str(L, " ");
    log_str(L, Name);
    log_str(L, " 0x00..0xFC]");
    log_nl(L);

    UINT16 vid = pci_rd16(bus, dev, func, 0x00);
    if (vid == 0xFFFFu) {
        log_str(L, "    NOT PRESENT (VID=0xFFFF)");
        log_nl(L);
        return;
    }

    for (UINTN off = 0; off < 256u; off += 16u) {
        log_str(L, "    ");
        log_hex(L, off, 2);
        log_str(L, ": ");
        for (UINTN c = 0; c < 16u; c += 4u) {
            UINT32 d = pci_rd32(bus, dev, func, (UINT8)(off + c));
            log_hex(L, d, 8);
            log_putc(L, ' ');
        }
        log_nl(L);
    }
}

static void DumpEcamRange(LOG_BUF *L, const PROBE_ENV *Env, const char *Name,
                          UINT8 bus, UINT8 dev, UINT8 func, UINT16 start, UINT16 end) {
    if (!Env->HasEcam) return;
    log_str(L, "  [ECAM ");
    log_hex(L, bus, 2); log_putc(L, ':');
    log_hex(L, dev, 2); log_putc(L, '.');
    log_hex(L, func, 1); log_str(L, " ");
    log_str(L, Name);
    log_str(L, " 0x"); log_hex(L, start, 3);
    log_str(L, "..0x"); log_hex(L, end - 4u, 3);
    log_str(L, "]"); log_nl(L);

    for (UINT16 off = start; off < end; off += 16u) {
        log_str(L, "    ");
        log_hex(L, off, 3);
        log_str(L, ": ");
        for (UINT16 c = 0; c < 16u; c += 4u) {
            UINT32 d = ecam_rd32(Env, bus, dev, func, (UINT16)(off + c));
            log_hex(L, d, 8);
            log_putc(L, ' ');
        }
        log_nl(L);
    }
}

/* ---- ACPI Inspection ---- */

static void InspectAcpi(EFI_SYSTEM_TABLE *ST, LOG_BUF *L, PROBE_ENV *Env) {
    log_str(L, "  [ACPI RSDP / XSDT / MCFG / DMAR / VFCT]");
    log_nl(L);

    ACPI_RSDP_20 *Rsdp = NULL;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        if (guid_eq(&ST->ConfigurationTable[i].VendorGuid, &gAcpi20Guid)) {
            Rsdp = (ACPI_RSDP_20 *)ST->ConfigurationTable[i].VendorTable;
            break;
        }
    }
    if (!Rsdp || !ascii_eq_n(Rsdp->Signature, "RSD PTR ", 8)) {
        log_str(L, "    ACPI 2.0 RSDP not found");
        log_nl(L);
        return;
    }

    log_str(L, "    RSDP=0x"); log_hex(L, (UINT64)(UINTN)Rsdp, 16);
    log_str(L, " Rev="); log_dec(L, Rsdp->Revision);
    log_str(L, " XSDT=0x"); log_hex(L, Rsdp->XsdtAddress, 16);
    log_nl(L);

    if (Rsdp->XsdtAddress == 0 || Rsdp->XsdtAddress >= 0x100000000ULL) return;
    ACPI_SDT_HDR *Xsdt = (ACPI_SDT_HDR *)(UINTN)Rsdp->XsdtAddress;
    if (!ascii_eq_n(Xsdt->Signature, "XSDT", 4) || Xsdt->Length < sizeof(ACPI_SDT_HDR)) return;

    UINTN count = (Xsdt->Length - sizeof(ACPI_SDT_HDR)) / sizeof(UINT64);
    UINT64 *entries = (UINT64 *)((UINT8 *)Xsdt + sizeof(ACPI_SDT_HDR));
    Env->DrhdCount = 0;
    Env->HasEcam = FALSE;

    for (UINTN i = 0; i < count; i++) {
        UINT64 pa = entries[i];
        if (pa == 0 || pa >= 0x100000000ULL) continue;
        ACPI_SDT_HDR *T = (ACPI_SDT_HDR *)(UINTN)pa;
        log_str(L, "    Table["); log_dec(L, i); log_str(L, "]: ");
        log_strn(L, T->Signature, 4);
        log_str(L, " OemTbl=");
        log_strn(L, T->OemTableId, 8);
        log_str(L, " Addr=0x"); log_hex(L, pa, 8);
        log_str(L, " Len="); log_dec(L, T->Length);
        log_nl(L);

        if (ascii_eq_n(T->Signature, "MCFG", 4) && T->Length >= sizeof(ACPI_SDT_HDR) + 8u + sizeof(ACPI_MCFG_ENTRY)) {
            ACPI_MCFG_ENTRY *Me = (ACPI_MCFG_ENTRY *)((UINT8 *)T + sizeof(ACPI_SDT_HDR) + 8u);
            log_str(L, "      MCFG[0] Base=0x"); log_hex(L, Me->BaseAddress, 16);
            log_str(L, " Seg="); log_dec(L, Me->SegmentGroup);
            log_str(L, " Bus="); log_dec(L, Me->StartBus);
            log_str(L, ".."); log_dec(L, Me->EndBus);
            log_nl(L);
            if (Me->BaseAddress >= 0xC0000000ULL && Me->BaseAddress < 0x100000000ULL) {
                Env->EcamBase = Me->BaseAddress;
                Env->EcamStartBus = Me->StartBus;
                Env->EcamEndBus = Me->EndBus;
                Env->HasEcam = TRUE;
            }
        } else if (ascii_eq_n(T->Signature, "DMAR", 4) && T->Length > sizeof(ACPI_SDT_HDR) + 12u) {
            UINT8 *p = (UINT8 *)T + sizeof(ACPI_SDT_HDR) + 12u;
            UINT8 *end = (UINT8 *)T + T->Length;
            while (p + 4 <= end) {
                UINT16 stype = *(UINT16 *)p;
                UINT16 slen  = *(UINT16 *)(p + 2);
                if (slen < 4 || p + slen > end) break;
                if (stype == 0 && slen >= sizeof(ACPI_DMAR_DRHD)) {
                    ACPI_DMAR_DRHD *D = (ACPI_DMAR_DRHD *)p;
                    log_str(L, "      DMAR DRHD Flags=0x"); log_hex(L, D->Flags, 2);
                    log_str(L, " RegBase=0x"); log_hex(L, D->RegisterBaseAddress, 16);
                    log_nl(L);
                    if (Env->DrhdCount < 4) {
                        Env->DrhdBases[Env->DrhdCount++] = D->RegisterBaseAddress;
                    }
                }
                p += slen;
            }
        } else if (ascii_eq_n(T->Signature, "VFCT", 4) && T->Length > 0x100u) {
            const UINT8 *v = (const UINT8 *)T;
            UINT32 imgSz = 0;
            UINTN vbOff = 0;
            for (UINTN k = 0x30; k + 4 < T->Length && k < 0x120; k++) {
                if (v[k] == 0x55u && v[k + 1] == 0xAAu) {
                    vbOff = k;
                    if (k >= 12u) {
                        imgSz = *(const UINT32 *)(v + k - 12u);
                    }
                    break;
                }
            }
            log_str(L, "      VFCT VBIOS sig=0x55AA at +0x"); log_hex(L, vbOff, 4);
            log_str(L, " ImageLen="); log_dec(L, imgSz);
            log_nl(L);
        }
    }
}

/* ---- Apple Indexed I/O GMUX Read-Only Probe ---- */

static BOOLEAN GmuxWaitReady(EFI_BOOT_SERVICES *BS) {
    for (UINTN i = 0; i < 200u; i++) {
        UINT8 st = io_in8(GMUX_IO_WRITE);
        if ((st & 0x01u) == 0) return TRUE;
        (void)io_in8(GMUX_IO_READ);
        BS->Stall(100);
    }
    return FALSE;
}

static BOOLEAN GmuxWaitComplete(EFI_BOOT_SERVICES *BS) {
    for (UINTN i = 0; i < 200u; i++) {
        UINT8 st = io_in8(GMUX_IO_WRITE);
        if ((st & 0x01u) != 0) {
            (void)io_in8(GMUX_IO_READ);
            return TRUE;
        }
        BS->Stall(100);
    }
    return FALSE;
}

static BOOLEAN GmuxRead8(EFI_BOOT_SERVICES *BS, UINT8 port, UINT8 *out) {
    if (!GmuxWaitReady(BS)) return FALSE;
    io_out8(GMUX_IO_READ, port);
    if (!GmuxWaitComplete(BS)) return FALSE;
    *out = io_in8(GMUX_IO_VALUE);
    return TRUE;
}

static BOOLEAN GmuxRead32(EFI_BOOT_SERVICES *BS, UINT8 port, UINT32 *out, UINT8 bytes[4]) {
    if (!GmuxWaitReady(BS)) return FALSE;
    io_out8(GMUX_IO_READ, port);
    if (!GmuxWaitComplete(BS)) return FALSE;
    *out = io_in32(GMUX_IO_VALUE);
    for (UINTN i = 0; i < 4u; i++) bytes[i] = io_in8((UINT16)(GMUX_IO_VALUE + i));
    return TRUE;
}

static void ProbeGmux(EFI_BOOT_SERVICES *BS, LOG_BUF *L, PROBE_SUMMARY *Sum) {
    log_str(L, "  [GMUX Indexed I/O 0x700..0x7FE (read-only)]");
    log_nl(L);

    io_out8(GMUX_IO_PROBE0, 0xAAu);
    io_out8(GMUX_IO_PROBE1, 0x55u);
    io_out8(GMUX_IO_PROBE2, 0x00u);
    UINT16 sig = (UINT16)io_in8(GMUX_IO_PROBE0) | ((UINT16)io_in8(GMUX_IO_PROBE1) << 8);
    log_str(L, "    Probe sig=0x"); log_hex(L, sig, 4);
    if (sig != 0x55AAu) {
        log_str(L, " (not indexed GMUX)");
        log_nl(L);
        Sum->GmuxIndexed = FALSE;
        return;
    }
    Sum->GmuxIndexed = TRUE;
    log_str(L, " (Indexed Renesas R4F2113 OK)");
    log_nl(L);

    UINT32 v32 = 0;
    UINT8 raw4[4] = {0, 0, 0, 0};
    if (GmuxRead32(BS, 0x04u, &v32, raw4)) {
        log_str(L, "    VERSION(0x04)=0x"); log_hex(L, v32, 8);
        log_str(L, " bytes=[");
        for (UINTN i = 0; i < 4u; i++) {
            if (i) log_putc(L, ' ');
            log_hex(L, raw4[i], 2);
        }
        log_putc(L, ']');
        log_nl(L);
    }

    static const UINT8 ports8[] = { 0x10u, 0x14u, 0x16u, 0x28u, 0x40u, 0x50u };
    static const char *names8[] = {
        "SWITCH_DISPLAY", "IRQ_ENABLE", "IRQ_STATUS",
        "SWITCH_DDC", "SWITCH_EXTERNAL", "DISCRETE_POWER"
    };
    for (UINTN i = 0; i < 6u; i++) {
        UINT8 val = 0;
        BOOLEAN ok = GmuxRead8(BS, ports8[i], &val);
        log_str(L, "    Port 0x"); log_hex(L, ports8[i], 2);
        log_str(L, " ("); log_str(L, names8[i]); log_str(L, "): ");
        if (ok) {
            log_str(L, "0x"); log_hex(L, val, 2);
            if (ports8[i] == 0x10u) {
                Sum->GmuxDisp = val;
                log_str(L, (val & 1u) ? " [DIS/AMD]" : " [IGD/Intel]");
            } else if (ports8[i] == 0x50u) {
                Sum->GmuxDgpuPwr = val;
            }
        } else {
            log_str(L, "TIMEOUT");
        }
        log_nl(L);
    }

    UINT32 bmax = 0, bcur = 0;
    if (GmuxRead32(BS, 0x70u, &bmax, raw4)) {
        log_str(L, "    MAX_BRIGHTNESS(0x70)=0x"); log_hex(L, bmax, 8); log_nl(L);
    }
    if (GmuxRead32(BS, 0x74u, &bcur, raw4)) {
        log_str(L, "    BRIGHTNESS(0x74)=0x"); log_hex(L, bcur, 8); log_nl(L);
    }
}

/* ---- AMD Polaris 11 BAR5 & SMC_IND Probe ---- */

static UINT32 smc_ind_read(UINT64 bar5, UINT32 idx_reg_dword, UINT32 data_reg_dword, UINT32 smc_addr) {
    UINT64 idx_mmio  = bar5 + (UINT64)idx_reg_dword * 4u;
    UINT64 data_mmio = bar5 + (UINT64)data_reg_dword * 4u;
    UINT32 saved = mmio_r32(idx_mmio);
    mmio_w32(idx_mmio, smc_addr);
    UINT32 val = mmio_r32(data_mmio);
    mmio_w32(idx_mmio, saved);
    return val;
}

static void ProbePolaris11(EFI_BOOT_SERVICES *BS, LOG_BUF *L, const PROBE_ENV *Env, PROBE_SUMMARY *Sum) {
    log_str(L, "  [AMD Polaris 11 (01:00.0) PCI Capabilities & BAR5/SMU7]");
    log_nl(L);

    UINT16 vid = pci_rd16(1, 0, 0, 0x00);
    UINT16 did = pci_rd16(1, 0, 0, 0x02);
    UINT16 cmd = pci_rd16(1, 0, 0, 0x04);
    UINT16 sts = pci_rd16(1, 0, 0, 0x06);
    UINT32 bar5 = pci_rd32(1, 0, 0, 0x24);
    UINT32 rom  = pci_rd32(1, 0, 0, 0x30);
    UINT16 peg_cmd = pci_rd16(0, 1, 0, 0x04);

    Sum->AmdCmd = cmd;
    Sum->AmdBar5 = bar5;

    log_str(L, "    VID:DID=0x"); log_hex(L, vid, 4); log_putc(L, ':'); log_hex(L, did, 4);
    log_str(L, " CMD=0x"); log_hex(L, cmd, 4);
    log_str(L, " STS=0x"); log_hex(L, sts, 4);
    log_str(L, " BAR5=0x"); log_hex(L, bar5, 8);
    log_str(L, " ROM=0x"); log_hex(L, rom, 8);
    log_str(L, " PEG0_CMD=0x"); log_hex(L, peg_cmd, 4);
    log_nl(L);

    if (vid != 0x1002u) {
        log_str(L, "    SKIP BAR5: 01:00.0 is not AMD (VID != 0x1002)");
        log_nl(L);
        return;
    }

    /* Walk PCI capabilities */
    UINT32 pmcsr = 0xFFFFFFFFu;
    UINT8 cap = (sts & 0x10u) ? (pci_rd8(1, 0, 0, 0x34) & 0xFCu) : 0u;
    UINTN guard = 0;
    while (cap >= 0x40u && guard++ < 32u) {
        UINT8 id = pci_rd8(1, 0, 0, cap);
        UINT8 nxt = pci_rd8(1, 0, 0, (UINT8)(cap + 1u)) & 0xFCu;
        log_str(L, "    Cap @0x"); log_hex(L, cap, 2);
        log_str(L, " ID=0x"); log_hex(L, id, 2);
        if (id == 0x01u) {
            pmcsr = pci_rd32(1, 0, 0, (UINT8)(cap + 4u));
            Sum->AmdPmcsr = pmcsr;
            log_str(L, " PMCSR=0x"); log_hex(L, pmcsr, 8);
            log_str(L, " (D"); log_dec(L, pmcsr & 3u); log_putc(L, ')');
        } else if (id == 0x05u) {
            UINT16 msiCtl = pci_rd16(1, 0, 0, (UINT8)(cap + 2u));
            log_str(L, " MSI_CTL=0x"); log_hex(L, msiCtl, 4);
        } else if (id == 0x10u) {
            UINT16 devCtl = pci_rd16(1, 0, 0, (UINT8)(cap + 0x08u));
            UINT16 devSta = pci_rd16(1, 0, 0, (UINT8)(cap + 0x0Au));
            UINT16 lnkCtl = pci_rd16(1, 0, 0, (UINT8)(cap + 0x10u));
            UINT16 lnkSta = pci_rd16(1, 0, 0, (UINT8)(cap + 0x12u));
            log_str(L, " DevCtl=0x"); log_hex(L, devCtl, 4);
            log_str(L, " DevSta=0x"); log_hex(L, devSta, 4);
            log_str(L, " LnkCtl=0x"); log_hex(L, lnkCtl, 4);
            log_str(L, " LnkSta=0x"); log_hex(L, lnkSta, 4);
            log_str(L, " (Gen"); log_dec(L, lnkSta & 0xFu);
            log_str(L, " x"); log_dec(L, (lnkSta >> 4) & 0x3Fu);
            log_putc(L, ')');
        }
        log_nl(L);
        if (nxt == 0u || nxt == cap) break;
        cap = nxt;
    }

    DumpEcamRange(L, Env, "AMD_EXT", 1, 0, 0, 0x100u, 0x200u);

    UINT32 bar5_base = bar5 & ~0xFu;
    if ((bar5 & 1u) != 0u || bar5_base == 0u || bar5_base >= 0xF0000000u) {
        log_str(L, "    SKIP BAR5: invalid BAR5 address"); log_nl(L);
        return;
    }
    if ((cmd & 0x02u) == 0u || (peg_cmd & 0x02u) == 0u) {
        log_str(L, "    SKIP BAR5: Memory Space Enable off (CMD or PEG0_CMD)"); log_nl(L);
        return;
    }
    if ((pmcsr & 0x03u) != 0u) {
        log_str(L, "    SKIP BAR5: AMD GPU is not in D0 (PMCSR != D0)"); log_nl(L);
        return;
    }

    Sum->Bar5Probed = TRUE;
    UINT64 b5 = (UINT64)bar5_base;

    UINT32 cfg_mem  = mmio_r32(b5 + mmCONFIG_MEMSIZE * 4u);
    UINT32 cfg_aper = mmio_r32(b5 + mmCONFIG_APER_SIZE * 4u);
    UINT32 bif_fb   = mmio_r32(b5 + mmBIF_FB_EN * 4u);
    UINT32 fb_loc   = mmio_r32(b5 + mmMC_VM_FB_LOCATION * 4u);
    UINT32 grbm_st  = mmio_r32(b5 + mmGRBM_STATUS * 4u);
    UINT32 smc_acc  = mmio_r32(b5 + mmSMC_IND_ACCESS_CNTL * 4u);
    UINT32 smc_msg0 = mmio_r32(b5 + mmSMC_MESSAGE_0 * 4u);
    UINT32 smc_rsp0 = mmio_r32(b5 + mmSMC_RESP_0 * 4u);
    UINT32 smc_arg0 = mmio_r32(b5 + mmSMC_MSG_ARG_0 * 4u);

    Sum->CfgMemSize = cfg_mem;
    Sum->GrbmStatus = grbm_st;

    log_str(L, "    BAR5 Direct: CONFIG_MEMSIZE=0x"); log_hex(L, cfg_mem, 8);
    log_str(L, " APER_SIZE=0x"); log_hex(L, cfg_aper, 8);
    log_str(L, " BIF_FB_EN=0x"); log_hex(L, bif_fb, 8);
    log_str(L, " FB_LOC=0x"); log_hex(L, fb_loc, 8);
    log_nl(L);
    log_str(L, "    BAR5 Direct: GRBM_STATUS=0x"); log_hex(L, grbm_st, 8);
    log_str(L, " SMC_ACCESS_CNTL=0x"); log_hex(L, smc_acc, 8);
    log_str(L, " MSG0=0x"); log_hex(L, smc_msg0, 8);
    log_str(L, " RESP0=0x"); log_hex(L, smc_rsp0, 8);
    log_str(L, " ARG0=0x"); log_hex(L, smc_arg0, 8);
    log_nl(L);

    log_str(L, "    BIOS_SCRATCH[0..7]: ");
    for (UINTN i = 0; i < 8u; i++) {
        UINT32 s = mmio_r32(b5 + (mmBIOS_SCRATCH_0 + i) * 4u);
        log_hex(L, s, 8); log_putc(L, ' ');
    }
    log_nl(L);

    if (cfg_mem == 0xFFFFFFFFu || grbm_st == 0xFFFFFFFFu) {
        log_str(L, "    SKIP SMC_IND: BAR5 direct read returned 0xFFFFFFFF");
        log_nl(L);
        return;
    }

    Sum->SmcIndProbed = TRUE;

    UINT32 rst   = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMC_SYSCON_RESET_CNTL);
    UINT32 clk   = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMC_SYSCON_CLOCK_CNTL_0);
    UINT32 misc  = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMC_SYSCON_MISC_CNTL);
    UINT32 pc1   = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMC_PC_C);
    BS->Stall(5000); /* 5 ms sample interval */
    UINT32 pc2   = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMC_PC_C);
    UINT32 pc_0  = smc_ind_read(b5, mmSMC_IND_INDEX_0,  mmSMC_IND_DATA_0,  ixSMC_PC_C);
    UINT32 rcu   = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixRCU_UC_EVENTS);
    UINT32 sst   = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMU_STATUS);
    UINT32 sfw   = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMU_FIRMWARE);
    UINT32 fwf   = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixFIRMWARE_FLAGS);
    UINT32 ef49  = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, ixSMU_EFUSE_49);

    Sum->SmcRst      = rst;
    Sum->SmcClk      = clk;
    Sum->SmcPc1      = pc1;
    Sum->SmcPc2      = pc2;
    Sum->RcuEvents   = rcu;
    Sum->SmuStatus   = sst;
    Sum->SmuFirmware = sfw;
    Sum->FwFlags     = fwf;

    BOOLEAN smc_running = ((clk & 1u) == 0u) && (pc1 >= 0x20100u) && (pc1 != 0xFFFFFFFFu);

    log_str(L, "    SMC_IND(11) ixSMC_SYSCON_RESET_CNTL   = 0x"); log_hex(L, rst, 8);
    log_str(L, " (rst_reg="); log_dec(L, rst & 1u); log_putc(L, ')'); log_nl(L);

    log_str(L, "    SMC_IND(11) ixSMC_SYSCON_CLOCK_CNTL_0 = 0x"); log_hex(L, clk, 8);
    log_str(L, " (ck_disable="); log_dec(L, clk & 1u); log_putc(L, ')'); log_nl(L);

    log_str(L, "    SMC_IND(11) ixSMC_SYSCON_MISC_CNTL    = 0x"); log_hex(L, misc, 8); log_nl(L);

    log_str(L, "    SMC_IND(11) ixSMC_PC_C (t0 / +5ms)    = 0x"); log_hex(L, pc1, 8);
    log_str(L, " / 0x"); log_hex(L, pc2, 8);
    log_str(L, " [idx0=0x"); log_hex(L, pc_0, 8);
    log_str(L, "] ram_running="); log_dec(L, smc_running ? 1u : 0u);
    log_str(L, " advancing="); log_dec(L, (pc1 != pc2) ? 1u : 0u);
    log_nl(L);

    log_str(L, "    SMC_IND(11) ixRCU_UC_EVENTS           = 0x"); log_hex(L, rcu, 8);
    log_str(L, " (boot_seq_done="); log_dec(L, (rcu >> 7) & 1u);
    log_str(L, " irq_en="); log_dec(L, (rcu >> 16) & 1u); log_putc(L, ')'); log_nl(L);

    log_str(L, "    SMC_IND(11) ixSMU_STATUS              = 0x"); log_hex(L, sst, 8);
    log_str(L, " (SMU_DONE="); log_dec(L, sst & 1u);
    log_str(L, " SMU_PASS="); log_dec(L, (sst >> 1) & 1u); log_putc(L, ')'); log_nl(L);

    log_str(L, "    SMC_IND(11) ixSMU_FIRMWARE            = 0x"); log_hex(L, sfw, 8);
    log_str(L, " (SMU_MODE="); log_dec(L, (sfw >> 16) & 1u);
    log_str(L, " SMU_SEL="); log_dec(L, (sfw >> 17) & 1u); log_putc(L, ')'); log_nl(L);

    log_str(L, "    SMC_IND(11) ixFIRMWARE_FLAGS          = 0x"); log_hex(L, fwf, 8);
    log_str(L, " (IRQ_EN="); log_dec(L, fwf & 1u); log_putc(L, ')'); log_nl(L);

    log_str(L, "    SMC_IND(11) ixSMU_EFUSE_49            = 0x"); log_hex(L, ef49, 8);
    log_str(L, " (AVFS="); log_dec(L, ef49 & 1u); log_putc(L, ')'); log_nl(L);

    log_str(L, "    SMC SRAM 0x20000..0x2003C:");
    for (UINT32 off = 0; off < 0x40u; off += 4u) {
        if ((off % 16u) == 0u) {
            log_nl(L);
            log_str(L, "      0x"); log_hex(L, 0x20000u + off, 5); log_str(L, ": ");
        }
        UINT32 w = smc_ind_read(b5, mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11, 0x20000u + off);
        log_hex(L, w, 8); log_putc(L, ' ');
    }
    log_nl(L);
}

/* ---- Intel Host Bridge, PEG0, IGPU & IOMMU Probe ---- */

static void ProbeIntelAndIommu(LOG_BUF *L, const PROBE_ENV *Env, PROBE_SUMMARY *Sum) {
    UINT32 ggc   = pci_rd32(0, 0, 0, 0x50u);
    UINT32 deven = pci_rd32(0, 0, 0, 0x54u);
    UINT32 bdsm  = pci_rd32(0, 0, 0, 0xB0u);
    UINT32 bgsm  = pci_rd32(0, 0, 0, 0xB4u);
    UINT32 tseg  = pci_rd32(0, 0, 0, 0xB8u);
    UINT32 tolud = pci_rd32(0, 0, 0, 0xBCu);
    Sum->Deven = deven;

    log_str(L, "  [00:00.0 Host Bridge Decoded]"); log_nl(L);
    log_str(L, "    GGC=0x"); log_hex(L, ggc, 8);
    log_str(L, " DEVEN=0x"); log_hex(L, deven, 8);
    log_str(L, " (D2EN="); log_dec(L, (deven >> 4) & 1u); log_putc(L, ')');
    log_str(L, " BDSM=0x"); log_hex(L, bdsm, 8);
    log_str(L, " BGSM=0x"); log_hex(L, bgsm, 8);
    log_str(L, " TSEGMB=0x"); log_hex(L, tseg, 8);
    log_str(L, " TOLUD=0x"); log_hex(L, tolud, 8);
    log_nl(L);

    /* PEG0 decoded */
    UINT16 p0_cmd = pci_rd16(0, 1, 0, 0x04u);
    UINT32 p0_bus = pci_rd32(0, 1, 0, 0x18u);
    UINT32 p0_mem = pci_rd32(0, 1, 0, 0x20u);
    UINT32 p0_prf = pci_rd32(0, 1, 0, 0x24u);
    UINT32 p0_pbl = pci_rd32(0, 1, 0, 0x28u);
    UINT32 p0_pul = pci_rd32(0, 1, 0, 0x2Cu);
    UINT16 p0_bcl = pci_rd16(0, 1, 0, 0x3Eu);
    log_str(L, "  [00:01.0 PEG0 Decoded]"); log_nl(L);
    log_str(L, "    CMD=0x"); log_hex(L, p0_cmd, 4);
    log_str(L, " BUS=0x"); log_hex(L, p0_bus, 8);
    log_str(L, " MEM=0x"); log_hex(L, p0_mem, 8);
    log_str(L, " PREF=0x"); log_hex(L, p0_prf, 8);
    log_str(L, " PREF_U32=0x"); log_hex(L, p0_pbl, 8);
    log_str(L, "/0x"); log_hex(L, p0_pul, 8);
    log_str(L, " BCTL=0x"); log_hex(L, p0_bcl, 4);
    log_nl(L);
    if (Env->HasEcam) {
        log_str(L, "    ECAM 0x248=0x"); log_hex(L, ecam_rd32(Env, 0, 1, 0, 0x248u), 8);
        log_str(L, " 0x444=0x"); log_hex(L, ecam_rd32(Env, 0, 1, 0, 0x444u), 8);
        log_str(L, " 0xC74=0x"); log_hex(L, ecam_rd32(Env, 0, 1, 0, 0xC74u), 8);
        log_nl(L);
    }

    /* IGPU 00:02.0 */
    UINT16 ivid = pci_rd16(0, 2, 0, 0x00u);
    UINT16 idid = pci_rd16(0, 2, 0, 0x02u);
    Sum->IgpuVid = ivid;
    Sum->IgpuDid = idid;
    log_str(L, "  [00:02.0 Intel HD 530 Decoded]"); log_nl(L);
    if (ivid == 0xFFFFu) {
        log_str(L, "    HIDDEN (VID=0xFFFF)"); log_nl(L);
    } else {
        UINT16 icmd  = pci_rd16(0, 2, 0, 0x04u);
        UINT32 bar0  = pci_rd32(0, 2, 0, 0x10u);
        UINT32 bar0h = pci_rd32(0, 2, 0, 0x14u);
        UINT32 bar2  = pci_rd32(0, 2, 0, 0x18u);
        UINT32 bar2h = pci_rd32(0, 2, 0, 0x1Cu);
        UINT32 bar4  = pci_rd32(0, 2, 0, 0x20u);
        UINT32 asls  = pci_rd32(0, 2, 0, 0xFCu);
        Sum->IgpuBar0Lo = bar0;
        Sum->IgpuBar2Lo = bar2;
        Sum->IgpuAsls   = asls;

        log_str(L, "    VID:DID=0x"); log_hex(L, ivid, 4); log_putc(L, ':'); log_hex(L, idid, 4);
        log_str(L, " CMD=0x"); log_hex(L, icmd, 4);
        log_str(L, " BAR0=0x"); log_hex(L, bar0h, 8); log_hex(L, bar0, 8);
        log_str(L, " BAR2=0x"); log_hex(L, bar2h, 8); log_hex(L, bar2, 8);
        log_str(L, " BAR4=0x"); log_hex(L, bar4, 8);
        log_str(L, " ASLS=0x"); log_hex(L, asls, 8);
        log_nl(L);

        if (asls != 0u && asls < 0xF0000000u) {
            const char *sig = (const char *)(UINTN)asls;
            BOOLEAN sig_ok = ascii_eq_n(sig, "IntelGraphicsMem", 16);
            Sum->OpRegionValid = sig_ok;
            log_str(L, "    OpRegion sig=\""); log_strn(L, sig, 16); log_str(L, "\"");
            if (sig_ok) {
                UINT32 opsz = *(const UINT32 *)(UINTN)(asls + 0x10u);
                UINT64 rvda = mmio_r64((UINT64)asls + 0x100u + 130u);
                UINT32 rvds = mmio_r32((UINT64)asls + 0x100u + 138u);
                const UINT8 *mb4 = (const UINT8 *)(UINTN)(asls + 0x400u);
                Sum->VbtPresent = (mb4[0] == '$' && mb4[1] == 'V' && mb4[2] == 'B' && mb4[3] == 'T');
                log_str(L, " sizeKB="); log_dec(L, opsz);
                log_str(L, " rvda=0x"); log_hex(L, rvda, 16);
                log_str(L, " rvds="); log_dec(L, rvds);
                log_str(L, " MB4[0..15]=");
                for (UINTN i = 0; i < 16u; i++) log_hex(L, mb4[i], 2);
            }
            log_nl(L);
        }

        UINT64 igpu_b0 = ((UINT64)bar0h << 32) | (UINT64)(bar0 & ~0xFu);
        if ((bar0 & 1u) == 0u && igpu_b0 != 0u && igpu_b0 < 0x100000000ULL && (icmd & 0x02u)) {
            UINT32 pwr2 = mmio_r32(igpu_b0 + 0x45404u);
            UINT32 ddiA = mmio_r32(igpu_b0 + 0x64000u);
            UINT32 auxA = mmio_r32(igpu_b0 + 0x64010u);
            UINT32 ppst = mmio_r32(igpu_b0 + 0xC7200u);
            UINT32 ppct = mmio_r32(igpu_b0 + 0xC7204u);
            Sum->DdiBufCtlA = ddiA;
            log_str(L, "    IGPU MMIO: PWR_WELL_CTL2=0x"); log_hex(L, pwr2, 8);
            log_str(L, " DDI_BUF_CTL_A=0x"); log_hex(L, ddiA, 8);
            log_str(L, " DP_AUX_CTL_A=0x"); log_hex(L, auxA, 8);
            log_str(L, " PP_STATUS=0x"); log_hex(L, ppst, 8);
            log_str(L, " PP_CTRL=0x"); log_hex(L, ppct, 8);
            log_nl(L);
        }
    }

    /* 01:00.1 AMD Audio */
    UINT16 avid = pci_rd16(1, 0, 1, 0x00u);
    UINT16 adid = pci_rd16(1, 0, 1, 0x02u);
    UINT16 acmd = pci_rd16(1, 0, 1, 0x04u);
    UINT32 abar0 = pci_rd32(1, 0, 1, 0x10u);
    log_str(L, "  [01:00.1 AMD Audio] VID:DID=0x"); log_hex(L, avid, 4);
    log_putc(L, ':'); log_hex(L, adid, 4);
    log_str(L, " CMD=0x"); log_hex(L, acmd, 4);
    log_str(L, " BAR0=0x"); log_hex(L, abar0, 8);
    log_nl(L);

    /* VT-d IOMMU DRHD registers */
    log_str(L, "  [VT-d IOMMU DRHD MMIO]"); log_nl(L);
    for (UINTN i = 0; i < Env->DrhdCount; i++) {
        UINT64 b = Env->DrhdBases[i];
        if (b == 0 || b >= 0x100000000ULL) continue;
        UINT32 ver  = mmio_r32(b + 0x00u);
        UINT64 cap  = mmio_r64(b + 0x08u);
        UINT64 ecap = mmio_r64(b + 0x10u);
        UINT32 gsts = mmio_r32(b + 0x1Cu);
        UINT64 rt   = mmio_r64(b + 0x20u);
        UINT32 fsts = mmio_r32(b + 0x34u);
        log_str(L, "    DRHD@0x"); log_hex(L, b, 8);
        log_str(L, " VER=0x"); log_hex(L, ver, 8);
        log_str(L, " CAP=0x"); log_hex(L, cap, 16);
        log_str(L, " ECAP=0x"); log_hex(L, ecap, 16);
        log_str(L, " GSTS=0x"); log_hex(L, gsts, 8);
        log_str(L, " RTADDR=0x"); log_hex(L, rt, 16);
        log_str(L, " FSTS=0x"); log_hex(L, fsts, 8);
        log_nl(L);
    }
}

/* ---- UEFI Protocols, NVRAM & EDID Snapshot ---- */

static void ProbeProtocolsAndEdid(EFI_BOOT_SERVICES *BS, EFI_RUNTIME_SERVICES *RT,
                                  EFI_HANDLE ImageHandle, LOG_BUF *L, PROBE_ENV *Env) {
    log_str(L, "  [UEFI Protocols & NVRAM]"); log_nl(L);

    /* AppleSetOs */
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    EFI_STATUS st = BS->LocateHandleBuffer(ByProtocol, &gAppleSetOsGuid, NULL, &count, &handles);
    log_str(L, "    AppleSetOs: status=0x"); log_hex(L, st, 16);
    log_str(L, " handles="); log_dec(L, EFI_ERROR(st) ? 0u : count);
    if (!EFI_ERROR(st) && handles != NULL) {
        for (UINTN i = 0; i < count; i++) {
            EFI_APPLE_SET_OS_PROTOCOL *p = NULL;
            if (!EFI_ERROR(BS->OpenProtocol(handles[i], &gAppleSetOsGuid, (VOID **)&p,
                                            ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL)) && p != NULL) {
                log_str(L, " ["); log_dec(L, i); log_str(L, "].Ver="); log_dec(L, p->Version);
            }
        }
        BS->FreePool(handles);
    }
    log_nl(L);

    /* Read-only NVRAM gpu-power-prefs */
    UINT8 buf[16];
    UINTN sz = sizeof(buf);
    UINT32 attr = 0;
    st = RT->GetVariable(L"gpu-power-prefs", &gGpuPrefsGuid, &attr, &sz, buf);
    log_str(L, "    gpu-power-prefs: status=0x"); log_hex(L, st, 16);
    if (!EFI_ERROR(st)) {
        log_str(L, " attr=0x"); log_hex(L, attr, 8);
        log_str(L, " size="); log_dec(L, sz);
        log_str(L, " data=");
        for (UINTN i = 0; i < sz && i < sizeof(buf); i++) {
            log_hex(L, buf[i], 2); log_putc(L, ' ');
        }
    }
    log_nl(L);

    /* EFI_PCI_IO_PROTOCOL Display Devices */
    handles = NULL;
    count = 0;
    if (!EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &gPciIoGuid, NULL, &count, &handles)) && handles != NULL) {
        for (UINTN i = 0; i < count; i++) {
            EFI_PCI_IO_PROTOCOL *pci = NULL;
            if (EFI_ERROR(BS->OpenProtocol(handles[i], &gPciIoGuid, (VOID **)&pci,
                                           ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL)) || !pci) {
                continue;
            }
            UINT32 hdr[16];
            mem_zero(hdr, sizeof(hdr));
            if (EFI_ERROR(pci->Pci.Read(pci, EfiPciIoWidthUint32, 0, 16, hdr))) continue;
            UINT8 cc = (UINT8)((hdr[2] >> 24) & 0xFFu);
            if (cc != 0x03u) continue;
            UINTN seg = 0, bus = 0, dev = 0, fn = 0;
            pci->GetLocation(pci, &seg, &bus, &dev, &fn);
            log_str(L, "    PciIo Display ");
            log_hex(L, bus, 2); log_putc(L, ':'); log_hex(L, dev, 2); log_putc(L, '.'); log_hex(L, fn, 1);
            log_str(L, " VID:DID=0x"); log_hex(L, hdr[0] & 0xFFFFu, 4);
            log_putc(L, ':'); log_hex(L, (hdr[0] >> 16) & 0xFFFFu, 4);
            log_str(L, " CMD=0x"); log_hex(L, hdr[1] & 0xFFFFu, 4);
            log_str(L, " BAR0..5=");
            for (UINTN b = 0; b < 6u; b++) {
                log_hex(L, hdr[4u + b], 8); log_putc(L, ' ');
            }
            log_nl(L);
        }
        BS->FreePool(handles);
    }

    /* GOP Handles */
    handles = NULL;
    count = 0;
    if (!EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &gGopGuid, NULL, &count, &handles)) && handles != NULL) {
        for (UINTN i = 0; i < count; i++) {
            EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
            if (!EFI_ERROR(BS->HandleProtocol(handles[i], &gGopGuid, (VOID **)&gop)) && gop && gop->Mode) {
                log_str(L, "    GOP["); log_dec(L, i); log_str(L, "] FB=0x");
                log_hex(L, gop->Mode->FrameBufferBase, 16);
                log_str(L, " Sz=0x"); log_hex(L, gop->Mode->FrameBufferSize, 8);
                if (gop->Mode->Info) {
                    log_str(L, " ");
                    log_dec(L, gop->Mode->Info->HorizontalResolution);
                    log_putc(L, 'x');
                    log_dec(L, gop->Mode->Info->VerticalResolution);
                    log_str(L, " PPSL=");
                    log_dec(L, gop->Mode->Info->PixelsPerScanLine);
                }
                log_nl(L);
            }
        }
        BS->FreePool(handles);
    }

    /* EDID Active / Discovered (dump once per run) */
    if (!Env->EdidDumped) {
        EFI_GUID *eg[2] = { &gEdidActiveGuid, &gEdidDiscoveredGuid };
        const char *en[2] = { "EDID_ACTIVE", "EDID_DISCOVERED" };
        for (UINTN g = 0; g < 2u; g++) {
            handles = NULL;
            count = 0;
            if (!EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, eg[g], NULL, &count, &handles)) && handles != NULL) {
                for (UINTN i = 0; i < count; i++) {
                    EFI_EDID_ACTIVE_PROTOCOL *ep = NULL;
                    if (!EFI_ERROR(BS->HandleProtocol(handles[i], eg[g], (VOID **)&ep)) && ep && ep->Edid) {
                        log_str(L, "    "); log_str(L, en[g]); log_str(L, "["); log_dec(L, i);
                        log_str(L, "] size="); log_dec(L, ep->SizeOfEdid);
                        for (UINTN k = 0; k < ep->SizeOfEdid && k < 256u; k++) {
                            if ((k % 16u) == 0u) {
                                log_nl(L);
                                log_str(L, "      ");
                            }
                            log_hex(L, ep->Edid[k], 2); log_putc(L, ' ');
                        }
                        log_nl(L);
                    }
                }
                BS->FreePool(handles);
            }
        }
        Env->EdidDumped = TRUE;
    }
}

/* ---- Snapshot Orchestrator ---- */

static void CaptureSnapshot(EFI_BOOT_SERVICES *BS, EFI_RUNTIME_SERVICES *RT,
                            EFI_SYSTEM_TABLE *ST, EFI_HANDLE ImageHandle,
                            LOG_BUF *L, PROBE_ENV *Env, const char *Tag, PROBE_SUMMARY *Sum) {
    mem_zero(Sum, sizeof(*Sum));

    log_str(L, "================================================================"); log_nl(L);
    log_str(L, "SNAPSHOT: "); log_str(L, Tag); log_nl(L);
    log_str(L, "================================================================"); log_nl(L);

    InspectAcpi(ST, L, Env);
    ProbeProtocolsAndEdid(BS, RT, ImageHandle, L, Env);

    DumpPci256(L, "HostBridge", 0, 0, 0);
    DumpPci256(L, "PEG0",       0, 1, 0);
    DumpPci256(L, "IGPU",       0, 2, 0);
    DumpPci256(L, "AMD_GFX0",   1, 0, 0);

    ProbeIntelAndIommu(L, Env, Sum);
    ProbePolaris11(BS, L, Env, Sum);
    ProbeGmux(BS, L, Sum);
    log_nl(L);
}

/* ---- AppleSetOs Call ---- */

static EFI_STATUS InvokeAppleSetOs(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle,
                                   const char *VersionStr, LOG_BUF *L) {
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    EFI_STATUS st = BS->LocateHandleBuffer(ByProtocol, &gAppleSetOsGuid, NULL, &count, &handles);
    log_str(L, ">>> InvokeAppleSetOs(\""); log_str(L, VersionStr); log_str(L, "\"): Locate=");
    log_hex(L, st, 16); log_str(L, " count="); log_dec(L, EFI_ERROR(st) ? 0u : count); log_nl(L);
    if (EFI_ERROR(st) || handles == NULL || count == 0) {
        return EFI_ERROR(st) ? st : EFI_NOT_FOUND;
    }

    EFI_STATUS final_st = EFI_SUCCESS;
    for (UINTN i = 0; i < count; i++) {
        EFI_APPLE_SET_OS_PROTOCOL *p = NULL;
        st = BS->OpenProtocol(handles[i], &gAppleSetOsGuid, (VOID **)&p,
                              ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(st) || p == NULL) {
            final_st = st;
            continue;
        }
        log_str(L, "    handle["); log_dec(L, i); log_str(L, "] Version="); log_dec(L, p->Version);
        if (p->Version != 0) {
            EFI_STATUS sv = p->SetOsVersion((CHAR8 *)VersionStr);
            EFI_STATUS sd = p->SetOsVendor((CHAR8 *)"Apple Inc.");
            log_str(L, " SetOsVersion=0x"); log_hex(L, sv, 16);
            log_str(L, " SetOsVendor=0x"); log_hex(L, sd, 16);
            if (EFI_ERROR(sv)) final_st = sv;
            if (EFI_ERROR(sd)) final_st = sd;
        } else {
            log_str(L, " (skipped: Version == 0)");
        }
        log_nl(L);
    }
    BS->FreePool(handles);
    return final_st;
}

/* ---- File Save to Boot USB ESP (\M1A_LOGS\...) ---- */

static EFI_STATUS SaveLogToUsb(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle,
                               CHAR16 *RelPath, const LOG_BUF *L) {
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

    /* Ensure \M1A_LOGS directory exists */
    st = root->Open(root, &dir, L"\\M1A_LOGS",
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                    EFI_FILE_DIRECTORY);
    if (!EFI_ERROR(st) && dir != NULL) {
        dir->Close(dir);
    }

    /* Delete previous file if it exists */
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

    UINTN to_write = L->Len;
    st = file->Write(file, &to_write, (VOID *)L->Buf);
    file->Flush(file);
    file->Close(file);
    root->Close(root);

    if (!EFI_ERROR(st) && to_write != L->Len) return EFI_DEVICE_ERROR;
    return st;
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
    UINTN prefix_len = dev_len - 4u; /* exclude End node */

    UINTN chars = 0;
    while (FilePathStr[chars]) chars++;
    UINTN fp_node_len = 4u + (chars + 1u) * sizeof(CHAR16);
    UINTN total = prefix_len + fp_node_len + 4u;

    UINT8 *buf = NULL;
    if (EFI_ERROR(BS->AllocatePool(EfiBootServicesData, total, (VOID **)&buf)) || !buf) {
        return NULL;
    }
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
    EFI_STATUS st;

    st = BS->LocateHandleBuffer(ByProtocol, &gSimpleFsGuid, NULL, &count, &handles);
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
        ConPrintA(ConOut, "\nERROR: \\EFI\\Microsoft\\Boot\\bootmgfw.efi not found on any volume!\n");
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

/* ---- Compact On-Screen Summary ---- */

static void PrintSummaryLine(SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut, const char *Label, const PROBE_SUMMARY *S) {
    ConPrintA(ConOut, "["); ConPrintA(ConOut, Label); ConPrintA(ConOut, "] ");
    ConPrintA(ConOut, "DEVEN="); ConPrintHex(ConOut, S->Deven, 8);
    ConPrintA(ConOut, " IGPU="); ConPrintHex(ConOut, S->IgpuVid, 4);
    ConPrintA(ConOut, ":"); ConPrintHex(ConOut, S->IgpuDid, 4);
    ConPrintA(ConOut, " ASLS="); ConPrintHex(ConOut, S->IgpuAsls, 8);
    ConPrintA(ConOut, " DDI_A="); ConPrintHex(ConOut, S->DdiBufCtlA, 8);
    ConPrintA(ConOut, "\n  AMD CMD="); ConPrintHex(ConOut, S->AmdCmd, 4);
    ConPrintA(ConOut, " PM="); ConPrintHex(ConOut, S->AmdPmcsr, 4);
    ConPrintA(ConOut, " B5="); ConPrintHex(ConOut, S->AmdBar5, 8);
    if (S->SmcIndProbed) {
        ConPrintA(ConOut, " | RST="); ConPrintHex(ConOut, S->SmcRst, 1);
        ConPrintA(ConOut, " CLK="); ConPrintHex(ConOut, S->SmcClk, 1);
        ConPrintA(ConOut, " PC="); ConPrintHex(ConOut, S->SmcPc1, 8);
        ConPrintA(ConOut, "->"); ConPrintHex(ConOut, S->SmcPc2, 8);
        ConPrintA(ConOut, "\n  RCU="); ConPrintHex(ConOut, S->RcuEvents, 8);
        ConPrintA(ConOut, " SMU_STS="); ConPrintHex(ConOut, S->SmuStatus, 8);
        ConPrintA(ConOut, " SMU_FW="); ConPrintHex(ConOut, S->SmuFirmware, 8);
        ConPrintA(ConOut, " FW_FLG="); ConPrintHex(ConOut, S->FwFlags, 8);
    } else {
        ConPrintA(ConOut, " | SMC_IND=SKIPPED");
    }
    if (S->GmuxIndexed) {
        ConPrintA(ConOut, " | GMUX disp="); ConPrintHex(ConOut, S->GmuxDisp, 2);
        ConPrintA(ConOut, " pwr="); ConPrintHex(ConOut, S->GmuxDgpuPwr, 2);
    }
    ConPrintA(ConOut, "\n");
}

/* ---- Interactive Workflow Runner ---- */

static void RunProbeSequence(EFI_BOOT_SERVICES *BS, EFI_RUNTIME_SERVICES *RT,
                             EFI_SYSTEM_TABLE *ST, EFI_HANDLE ImageHandle,
                             LOG_BUF *L, const char *SetOsVer, CHAR16 *OutFile,
                             BOOLEAN BootWin) {
    SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut = ST->ConOut;
    SIMPLE_INPUT_INTERFACE *ConIn = ST->ConIn;
    PROBE_ENV Env;
    PROBE_SUMMARY S0, S1, S2;

    mem_zero(&Env, sizeof(Env));
    L->Len = 0;
    L->Truncated = FALSE;
    if (L->Buf) L->Buf[0] = 0;

    ConOut->ClearScreen(ConOut);
    ConPrintA(ConOut, "=== MacBookPro13,3 Milestone 1A Probe ===\n");
    ConPrintA(ConOut, "Capturing BEFORE snapshot...\n");

    log_str(L, "MBP133_M1A_PROBE_V1"); log_nl(L);
    log_str(L, "MODE_SETOS="); log_str(L, SetOsVer ? SetOsVer : "NONE"); log_nl(L);

    CaptureSnapshot(BS, RT, ST, ImageHandle, L, &Env, SetOsVer ? "BEFORE_SETOS" : "NORMAL", &S0);
    PrintSummaryLine(ConOut, SetOsVer ? "BEFORE" : "NORMAL", &S0);

    if (SetOsVer != NULL) {
        ConPrintA(ConOut, "\nCalling AppleSetOs(\"");
        ConPrintA(ConOut, SetOsVer);
        ConPrintA(ConOut, "\")...\n");
        InvokeAppleSetOs(BS, ImageHandle, SetOsVer, L);

        CaptureSnapshot(BS, RT, ST, ImageHandle, L, &Env, "AFTER_SETOS_IMMEDIATE", &S1);
        PrintSummaryLine(ConOut, "AFTER_0MS", &S1);

        BS->Stall(500000); /* 500 ms */
        CaptureSnapshot(BS, RT, ST, ImageHandle, L, &Env, "AFTER_SETOS_500MS", &S2);
        PrintSummaryLine(ConOut, "AFTER_500MS", &S2);
    }

    log_str(L, "LOG_BYTES="); log_dec(L, L->Len);
    log_str(L, " TRUNCATED="); log_dec(L, L->Truncated ? 1u : 0u);
    log_nl(L);

    EFI_STATUS saveSt = SaveLogToUsb(BS, ImageHandle, OutFile, L);
    ConPrintA(ConOut, "\nSave to USB: ");
    if (!EFI_ERROR(saveSt)) {
        ConOut->OutputString(ConOut, OutFile);
        ConPrintA(ConOut, " -> OK (");
        ConPrintHex(ConOut, L->Len, 6);
        ConPrintA(ConOut, " bytes)\n");
    } else {
        ConPrintA(ConOut, "FAILED 0x");
        ConPrintHex(ConOut, saveSt, 16);
        ConPrintA(ConOut, "\n");
    }

    if (!BootWin) {
        ConPrintA(ConOut, "\nProbe only complete. Press any key to return to menu...\n");
        FlushKeys(ConIn);
        EFI_INPUT_KEY k;
        ReadKeyWait(BS, ConIn, &k);
        return;
    }

    ConPrintA(ConOut, "\nBooting Windows in 3s (Enter = now, Esc = cancel to menu)...\n");
    FlushKeys(ConIn);
    BOOLEAN abort_boot = FALSE;
    for (UINTN t = 0; t < 60u; t++) {
        EFI_INPUT_KEY k;
        if (!EFI_ERROR(ConIn->ReadKeyStroke(ConIn, &k))) {
            if (k.ScanCode == 0x17u) {
                abort_boot = TRUE;
                break;
            }
            if (k.UnicodeChar == 0x0Du || k.UnicodeChar == 0x0Au || k.UnicodeChar == L' ') {
                break;
            }
        }
        BS->Stall(50000);
    }

    if (!abort_boot) {
        EFI_STATUS bst = ChainloadWindows(BS, ImageHandle, ConOut);
        if (EFI_ERROR(bst)) {
            ConPrintA(ConOut, "Press any key to return to menu...\n");
            FlushKeys(ConIn);
            EFI_INPUT_KEY k;
            ReadKeyWait(BS, ConIn, &k);
        }
    }
}

/* ---- UEFI Entry Point (gnu-efi crt0 passes System V args: rdi, rsi) ---- */

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
        ConPrintA(ConOut, "FATAL: AllocatePool(256KB) failed\n");
        return EFI_OUT_OF_RESOURCES;
    }

    while (1) {
        ConOut->ClearScreen(ConOut);
        ConPrintA(ConOut,
            "========================================================================\n"
            "  MacBookPro13,3 Milestone 1A Probe (Polaris 11 SMU7 + AppleSetOs)\n"
            "  100% Read-Only Hardware Probe | USB-only (NVMe ESP untouched)\n"
            "========================================================================\n\n"
            "  [1] Normal Probe (NO AppleSetOs)     -> save probe_normal.txt  -> Win\n"
            "  [2] SetOS 10.15 Probe (Before+After) -> save probe_setos15.txt -> Win\n"
            "  [3] SetOS 10.9  Probe (Before+After) -> save probe_setos09.txt -> Win\n"
            "  [4] SetOS 10.15 Probe ONLY (no Windows boot)\n"
            "  [5] Normal Probe ONLY      (no Windows boot)\n"
            "  [R] Cold Reboot\n"
            "  [0] Exit to rEFInd\n\n"
            "Select option: ");

        FlushKeys(ConIn);
        EFI_INPUT_KEY k;
        mem_zero(&k, sizeof(k));
        ReadKeyWait(BS, ConIn, &k);
        CHAR16 c = k.UnicodeChar;

        if (c == L'1') {
            RunProbeSequence(BS, RT, SystemTable, ImageHandle, &L, NULL, L"\\M1A_LOGS\\probe_normal.txt", TRUE);
        } else if (c == L'2') {
            RunProbeSequence(BS, RT, SystemTable, ImageHandle, &L, "Mac OS X 10.15", L"\\M1A_LOGS\\probe_setos15.txt", TRUE);
        } else if (c == L'3') {
            RunProbeSequence(BS, RT, SystemTable, ImageHandle, &L, "Mac OS X 10.9", L"\\M1A_LOGS\\probe_setos09.txt", TRUE);
        } else if (c == L'4') {
            RunProbeSequence(BS, RT, SystemTable, ImageHandle, &L, "Mac OS X 10.15", L"\\M1A_LOGS\\probe_only15.txt", FALSE);
        } else if (c == L'5') {
            RunProbeSequence(BS, RT, SystemTable, ImageHandle, &L, NULL, L"\\M1A_LOGS\\probe_only_nrm.txt", FALSE);
        } else if (c == L'r' || c == L'R') {
            ConPrintA(ConOut, "\nRebooting...\n");
            BS->Stall(300000);
            RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
        } else if (c == L'0' || k.ScanCode == 0x17u) {
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
