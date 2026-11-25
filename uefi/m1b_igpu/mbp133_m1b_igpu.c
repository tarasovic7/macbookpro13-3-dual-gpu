/*
 * mbp133_m1b_igpu.c - Milestone 1B Intel-Only Boot Loader (Apple MacBookPro13,3)
 * Target: Skylake i7-6920HQ + Intel HD 530 (00:02.0) + AMD Radeon Pro 455 (01:00.0)
 *
 * Implements Mode 4 (Intel-Only) using:
 *   - Pre-switch telemetry & native Apple VBT dump/inspection (\M1B_LOGS\vbt_orig.bin)
 *   - EFI_APPLE_SET_OS_PROTOCOL ("Apple Inc.", "Mac OS X 10.15")
 *   - Pre-T2 Indexed I/O GMUX switch (Renesas R4F2113 @ 0x700..0x7FE -> IGD) + verified rail-off (0x50)
 *   - Intel HD 530 eDP AUX-A DPCD/EDID probe & DDI_BUF_CTL_A 4-lane strap (bit 4)
 *   - Optional VBT APPA030 EDID timing patch (Mode 2)
 *   - In-memory DSDT method renames (_BCM/_PTS/_WAK) + SSDT_IGPU_MBP133.aml injection
 *   - Logs flushed to USB \M1B_LOGS\ before chainloading \EFI\Microsoft\Boot\bootmgfw.efi
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

static EFI_GUID gConsoleCtrlGuid    = EFI_CONSOLE_CONTROL_PROTOCOL_GUID;
static EFI_GUID gAppleSetOsGuid     = EFI_APPLE_SET_OS_PROTOCOL_GUID;
static EFI_GUID gAcpi20Guid         = EFI_ACPI_20_TABLE_GUID;
static EFI_GUID gLoadedImageGuid    = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID gDevicePathGuid     = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID gSimpleFsGuid       = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID gPciIoGuid          = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID gEdidActiveGuid     = EFI_EDID_ACTIVE_PROTOCOL_GUID;
static EFI_GUID gEdidDiscoveredGuid = EFI_EDID_DISCOVERED_PROTOCOL_GUID;

/* ---- Pre-T2 Indexed I/O GMUX (Renesas R4F2113) ---- */

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

#define BDB_LFP_OPTIONS_ID          40u
#define BDB_LFP_DATA_PTRS_ID        41u

/* ---- ACPI Packed Structures ---- */

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
#pragma pack()

/* ---- Logging Buffer ---- */

#define LOG_CAP (128u * 1024u)

typedef struct {
    char   *Buf;
    UINTN   Len;
    UINTN   Cap;
    BOOLEAN Truncated;
} LOG_BUF;

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

/* ---- Low-Level x86_64 Port I/O ---- */

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

    st = root->Open(root, &dir, L"\\M1B_LOGS",
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

/* ---- Apple Indexed I/O GMUX (Renesas R4F2113) ---- */

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
    static const char *n8[] = { "DISPLAY", "IRQ_EN", "IRQ_ST", "DDC", "EXT", "DGPU_PWR" };
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

static BOOLEAN GmuxSwitchToIgdAndCutRail(EFI_BOOT_SERVICES *BS, BOOLEAN DoRailOff, LOG_BUF *L) {
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
            log_str(L, "  GMUX rail OFF: w=("); log_dec(L, p1); log_putc(L, ','); log_dec(L, p2);
            log_str(L, ") pwr_irq="); log_dec(L, seen ? 1u : 0u);
            log_str(L, " 0x50_rb=0x"); log_hex(L, pwr_rb, 2); log_nl(L);
        }
    } else {
        log_str(L, "  GMUX dGPU rail LEFT ON (dry-run mode)"); log_nl(L);
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

/* ---- Firmware Internal Panel EDID Capture ---- */

static BOOLEAN EdidLooksInternal(const UINT8 *e, UINTN n) {
    static const UINT8 hdr[8] = { 0x00u, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x00u };
    if (!e || n < 128u) return FALSE;
    for (UINTN i = 0; i < 8u; i++) if (e[i] != hdr[i]) return FALSE;
    if (sum8(e, 128u) != 0u) return FALSE;
    if (e[8] != 0x06u || e[9] != 0x10u) return FALSE; /* "APP" */
    if (e[54] == 0u && e[55] == 0u) return FALSE;     /* non-zero first DTD */
    return TRUE;
}

static BOOLEAN CaptureFirmwareEdid(EFI_BOOT_SERVICES *BS, UINT8 OutEdid[128], LOG_BUF *L) {
    EFI_GUID guids[2] = { gEdidActiveGuid, gEdidDiscoveredGuid };
    const char *names[2] = { "EDID_ACTIVE", "EDID_DISCOVERED" };
    for (UINTN g = 0; g < 2u; g++) {
        EFI_HANDLE *handles = NULL;
        UINTN count = 0;
        if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &guids[g], NULL, &count, &handles)) || !handles)
            continue;
        for (UINTN i = 0; i < count; i++) {
            EFI_EDID_ACTIVE_PROTOCOL *ep = NULL;
            if (!EFI_ERROR(BS->HandleProtocol(handles[i], &guids[g], (VOID **)&ep)) &&
                ep && EdidLooksInternal(ep->Edid, ep->SizeOfEdid)) {
                mem_copy(OutEdid, ep->Edid, 128u);
                BS->FreePool(handles);
                log_str(L, "  [EDID Capture] "); log_str(L, names[g]);
                log_str(L, " size="); log_dec(L, ep->SizeOfEdid);
                const UINT8 *d = OutEdid + 54u;
                UINT16 hact = (UINT16)(d[2] | ((d[4] >> 4) << 8));
                UINT16 vact = (UINT16)(d[5] | ((d[7] >> 4) << 8));
                UINT32 pclk = (UINT32)rd16(d) * 10u;
                log_str(L, " timing="); log_dec(L, hact); log_putc(L, 'x'); log_dec(L, vact);
                log_str(L, " pclk="); log_dec(L, pclk); log_str(L, "kHz"); log_nl(L);
                return TRUE;
            }
        }
        BS->FreePool(handles);
    }
    log_str(L, "  [EDID Capture] No APP internal EDID found in GOP handles"); log_nl(L);
    return FALSE;
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

/* ---- OpRegion VBT Inspector, Dumper & Optional Patcher ---- */

static void InspectAndDumpNativeVbt(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle,
                                    EFI_PCI_IO_PROTOCOL *Igpu, LOG_BUF *L) {
    if (!Igpu) return;
    UINT32 asls = 0;
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_ASLS, 1u, &asls);
    log_str(L, "  [OpRegion/VBT] ASLS=0x"); log_hex(L, asls, 8); log_nl(L);
    if (asls == 0u || asls == 0xFFFFFFFFu) return;

    const UINT8 *op = (const UINT8 *)(UINTN)asls;
    if (!ascii_eq_n(op, "IntelGraphicsMem", 16u)) {
        log_str(L, "    OpRegion signature mismatch"); log_nl(L);
        return;
    }

    UINTN opsz = (UINTN)rd32(op + 16u) * 1024u;
    UINT64 rvda = rd64(op + OPREGION_ASLE_OFF + ASLE_RVDA_OFF);
    UINT32 rvds = rd32(op + OPREGION_ASLE_OFF + ASLE_RVDS_OFF);
    log_str(L, "    size="); log_dec(L, opsz);
    log_str(L, " rvda=0x"); log_hex(L, rvda, 16);
    log_str(L, " rvds="); log_dec(L, rvds); log_nl(L);

    if (opsz < OPREGION_VBT_OFF + 64u) return;
    UINTN room = opsz - OPREGION_VBT_OFF;
    if (room > OPREGION_VBT_MAX) room = OPREGION_VBT_MAX;
    const UINT8 *v = op + OPREGION_VBT_OFF;

    if (!ascii_eq_n(v, "$VBT", 4u)) {
        log_str(L, "    MB4 does not start with $VBT"); log_nl(L);
        return;
    }

    UINTN vsz = rd16(v + 24u);
    if (vsz == 0u || vsz > room) vsz = room;
    UINT32 bdb = rd32(v + 28u);
    log_str(L, "    VBT hdr: sig=\""); log_strn(L, (const char *)v, 20u);
    log_str(L, "\" ver=0x"); log_hex(L, rd16(v + 20u), 4);
    log_str(L, " hdr_sz="); log_dec(L, rd16(v + 22u));
    log_str(L, " vbt_sz="); log_dec(L, vsz);
    log_str(L, " csum=0x"); log_hex(L, v[26], 2);
    log_str(L, " sum8="); log_dec(L, sum8(v, vsz));
    log_str(L, " bdb_off=0x"); log_hex(L, bdb, 4); log_nl(L);

    EFI_STATUS dst = WriteUsbFile(BS, ImageHandle, L"\\M1B_LOGS\\vbt_orig.bin", v, vsz);
    log_str(L, "    Save \\M1B_LOGS\\vbt_orig.bin: 0x"); log_hex(L, dst, 16); log_nl(L);

    if (bdb + 22u <= vsz && ascii_eq_n(v + bdb, "BIOS_DATA_BLOCK", 15u)) {
        UINT16 bver = rd16(v + bdb + 16u);
        UINT16 bhdr = rd16(v + bdb + 18u);
        UINT16 btot = rd16(v + bdb + 20u);
        UINTN bend = (UINTN)bdb + (UINTN)btot;
        if (bend > vsz) bend = vsz;
        log_str(L, "    BDB ver="); log_dec(L, bver);
        log_str(L, " hdr_sz="); log_dec(L, bhdr);
        log_str(L, " total_sz="); log_dec(L, btot); log_nl(L);

        UINTN o40 = 0, s40 = 0, o41 = 0, s41 = 0;
        for (UINTN pos = (UINTN)bdb + bhdr; pos + 3u <= bend; ) {
            UINT8 bid = v[pos];
            UINTN bsz = rd16(v + pos + 1u);
            if (pos + 3u + bsz > bend) break;
            if (bid == 9u || bid == 27u || bid == 40u || bid == 41u || bid == 43u) {
                log_str(L, "      BDB["); log_dec(L, bid);
                log_str(L, "] off=+0x"); log_hex(L, pos + 3u, 4);
                log_str(L, " sz="); log_dec(L, bsz);
                if (bid == 9u || bid == 27u || bid == 40u) {
                    log_str(L, " head=");
                    for (UINTN k = 0; k < bsz && k < 16u; k++) {
                        log_hex(L, v[pos + 3u + k], 2); log_putc(L, ' ');
                    }
                }
                log_nl(L);
            }
            if (bid == BDB_LFP_OPTIONS_ID && !o40) { o40 = pos + 3u; s40 = bsz; }
            if (bid == BDB_LFP_DATA_PTRS_ID && !o41) { o41 = pos + 3u; s41 = bsz; }
            pos += 3u + bsz;
        }
        if (o40 && s40 >= 1u && o41) {
            UINTN panel = v[o40];
            if (panel >= 16u) panel = 0u;
            UINTN e = o41 + 1u + panel * 9u;
            if (e + 6u <= o41 + s41) {
                UINTN fp = (UINTN)bdb + rd16(v + e);
                UINTN dv = (UINTN)bdb + rd16(v + e + 3u);
                if (fp + 4u <= vsz && dv + 18u <= vsz) {
                    log_str(L, "      LFP panel="); log_dec(L, panel);
                    log_str(L, " fp="); log_dec(L, rd16(v + fp)); log_putc(L, 'x'); log_dec(L, rd16(v + fp + 2u));
                    log_str(L, " pclk="); log_dec(L, (UINT32)rd16(v + dv) * 10u); log_str(L, "kHz");
                    if (s40 >= 8u) {
                        log_str(L, " BDB40.psr_dword=0x"); log_hex(L, rd32(v + o40 + 4u), 8);
                    }
                    log_nl(L);
                }
            }
        }
    }
}

static void PatchNativeVbt(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle,
                           EFI_PCI_IO_PROTOCOL *Igpu, const UINT8 *Edid,
                           BOOLEAN HaveEdid, LOG_BUF *L) {
    if (!Igpu || !HaveEdid) {
        log_str(L, "  [VBT Patch] Skipped (missing iGPU or EDID)"); log_nl(L);
        return;
    }

    UINT32 asls = 0;
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_ASLS, 1u, &asls);
    if (asls == 0u || asls == 0xFFFFFFFFu) return;
    UINT8 *op = (UINT8 *)(UINTN)asls;
    UINTN opsz = (UINTN)rd32(op + 16u) * 1024u;
    if (opsz < OPREGION_VBT_OFF + 64u) return;
    UINTN room = opsz - OPREGION_VBT_OFF;
    if (room > OPREGION_VBT_MAX) room = OPREGION_VBT_MAX;
    UINT8 *v = op + OPREGION_VBT_OFF;
    if (!ascii_eq_n(v, "$VBT", 4u)) return;

    UINTN vsz = rd16(v + 24u);
    UINTN bdb = rd32(v + 28u);
    if (vsz < 64u || vsz > room || bdb + 22u > vsz) return;
    UINTN hdr  = rd16(v + bdb + 18u);
    UINTN bend = bdb + rd16(v + bdb + 20u);
    if (bend > vsz) bend = vsz;

    UINTN o9 = 0, s9 = 0, o27 = 0, s27 = 0, o40 = 0, s40 = 0, o41 = 0, s41 = 0;
    for (UINTN pos = bdb + hdr; pos + 3u <= bend; ) {
        UINT8 bid = v[pos];
        UINTN bsz = rd16(v + pos + 1u);
        if (pos + 3u + bsz > bend) break;
        if (bid == 9u  && !o9)  { o9  = pos + 3u; s9  = bsz; }
        if (bid == 27u && !o27) { o27 = pos + 3u; s27 = bsz; }
        if (bid == BDB_LFP_OPTIONS_ID   && !o40) { o40 = pos + 3u; s40 = bsz; }
        if (bid == BDB_LFP_DATA_PTRS_ID && !o41) { o41 = pos + 3u; s41 = bsz; }
        pos += 3u + bsz;
    }
    if (!o40 || !o41) {
        log_str(L, "  [VBT Patch] Missing BDB 40/41, aborted"); log_nl(L);
        return;
    }

    UINTN panel = v[o40];
    if (panel >= 16u) panel = 0u;
    UINTN e = o41 + 1u + panel * 9u;
    if (e + 6u > o41 + s41) return;
    UINTN fp = bdb + rd16(v + e);
    UINTN dv = bdb + rd16(v + e + 3u);
    if (fp + 4u > vsz || dv + 18u > vsz) return;

    const UINT8 *d = Edid + 54u;
    UINT16 hact = (UINT16)(d[2] | ((d[4] >> 4) << 8));
    UINT16 vact = (UINT16)(d[5] | ((d[7] >> 4) << 8));
    v[fp + 0] = (UINT8)(hact & 0xFFu); v[fp + 1] = (UINT8)(hact >> 8);
    v[fp + 2] = (UINT8)(vact & 0xFFu); v[fp + 3] = (UINT8)(vact >> 8);
    for (UINTN i = 0; i < 18u; i++) v[dv + i] = d[i];

    /* Disable PSR in BDB 40 psr_bits (2 bits per panel at o40+4) and BDB 9 byte 0 (4B/panel) if sized */
    if (s40 >= 8u) {
        UINT32 psr_mask = rd32(v + o40 + 4u);
        psr_mask &= ~(3u << (panel * 2u));
        v[o40 + 4u] = (UINT8)(psr_mask & 0xFFu);
        v[o40 + 5u] = (UINT8)((psr_mask >> 8) & 0xFFu);
        v[o40 + 6u] = (UINT8)((psr_mask >> 16) & 0xFFu);
        v[o40 + 7u] = (UINT8)((psr_mask >> 24) & 0xFFu);
    }
    if (o9 && s9 == 64u) {
        v[o9 + panel * 4u] &= (UINT8)~0x03u;
    }
    (void)o27;
    (void)s27;

    /* Recompute VBT byte 26 checksum so sum8(v, vsz) == 0 */
    v[26] = 0u;
    v[26] = (UINT8)(0u - sum8(v, vsz));

    WriteUsbFile(BS, ImageHandle, L"\\M1B_LOGS\\vbt_patched.bin", v, vsz);
    log_str(L, "  [VBT Patch] Applied APPA030 "); log_dec(L, hact); log_putc(L, 'x'); log_dec(L, vact);
    log_str(L, " + PSR off, sum8="); log_dec(L, sum8(v, vsz)); log_nl(L);
}

/* Set BDB[43] (LFP Backlight) type=0 (NONE) so igfx leaves brightness to ACPI DD01._BCM -> GMUX 0x774 */
static void PatchVbtBacklightNone(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle,
                                  EFI_PCI_IO_PROTOCOL *Igpu, LOG_BUF *L) {
    if (!Igpu) {
        log_str(L, "  [VBT BDB43] Skipped (no iGPU)"); log_nl(L);
        return;
    }

    UINT32 asls = 0;
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_ASLS, 1u, &asls);
    if (asls == 0u || asls == 0xFFFFFFFFu) {
        log_str(L, "  [VBT BDB43] Skipped (ASLS=0)"); log_nl(L);
        return;
    }
    UINT8 *op = (UINT8 *)(UINTN)asls;
    if (!ascii_eq_n(op, "IntelGraphicsMem", 16u)) {
        log_str(L, "  [VBT BDB43] OpRegion sig mismatch"); log_nl(L);
        return;
    }
    UINTN opsz = (UINTN)rd32(op + 16u) * 1024u;
    if (opsz < OPREGION_VBT_OFF + 64u) return;
    UINTN room = opsz - OPREGION_VBT_OFF;
    if (room > OPREGION_VBT_MAX) room = OPREGION_VBT_MAX;
    UINT8 *v = op + OPREGION_VBT_OFF;
    if (!ascii_eq_n(v, "$VBT", 4u)) {
        log_str(L, "  [VBT BDB43] No $VBT in MB4"); log_nl(L);
        return;
    }

    UINTN vsz = rd16(v + 24u);
    UINTN bdb = rd32(v + 28u);
    if (vsz < 64u || vsz > room || bdb + 22u > vsz) return;
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
        return;
    }

    UINTN esz = v[o43];
    if (esz < 1u || 1u + 16u * esz > s43) {
        log_str(L, "  [VBT BDB43] Unexpected entry_size="); log_dec(L, esz);
        log_str(L, " sz="); log_dec(L, s43); log_nl(L);
        return;
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
    EFI_STATUS sst = WriteUsbFile(BS, ImageHandle, L"\\M1B_LOGS\\vbt_patched.bin", v, vsz);
    log_str(L, "  [VBT BDB43] before(p0,p2)=0x"); log_hex(L, before_p0, 2);
    log_str(L, ",0x"); log_hex(L, before_p2, 2);
    log_str(L, " -> after=0x"); log_hex(L, v[o43 + 1u], 2);
    log_str(L, " esz="); log_dec(L, esz);
    log_str(L, " sum8="); log_dec(L, sum8(v, vsz));
    log_str(L, ok ? " OK" : " MISMATCH");
    log_str(L, " save=0x"); log_hex(L, sst, 16); log_nl(L);
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

static EFI_STATUS I2cOverAux(EFI_BOOT_SERVICES *BS, EFI_PCI_IO_PROTOCOL *Pci,
                             BOOLEAN read, BOOLEAN mot, UINT8 *data, UINTN len) {
    for (UINTN defer = 0; defer < 7u; defer++) {
        UINT8 tx[5];
        UINT8 rx[20];
        UINTN txn = 4u, rn = read ? (len + 1u) : 1u;
        tx[0] = (UINT8)(((read ? 0x1u : 0x0u) | (mot ? 0x4u : 0x0u)) << 4);
        tx[1] = 0x00u;
        tx[2] = 0x50u;
        tx[3] = (UINT8)(len - 1u);
        if (!read) { tx[4] = data[0]; txn = 5u; }
        EFI_STATUS st = AuxXfer(BS, Pci, tx, txn, rx, &rn);
        if (EFI_ERROR(st)) return st;
        UINT8 code = (UINT8)(rx[0] >> 4);
        if ((code & 0xCu) == 0x8u || (code & 0x3u) == 0x2u) { BS->Stall(400); continue; }
        if (code != 0u) return EFI_DEVICE_ERROR;
        if (read) {
            if (rn < len + 1u) return EFI_DEVICE_ERROR;
            for (UINTN i = 0; i < len; i++) data[i] = rx[1u + i];
        }
        return EFI_SUCCESS;
    }
    return EFI_TIMEOUT;
}

static void ProbeEdpAndForce4Lanes(EFI_BOOT_SERVICES *BS, EFI_PCI_IO_PROTOCOL *Igpu,
                                   UINT8 AuxEdidOut[128], BOOLEAN *HasAuxEdid, LOG_BUF *L) {
    *HasAuxEdid = FALSE;
    if (!Igpu) {
        log_str(L, "  [eDP/DDI_A] No Intel iGPU PciIo found"); log_nl(L);
        return;
    }

    UINT32 cmd = 0, bar0 = 0, bar1 = 0;
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_CMD,  1u, &cmd);
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_BAR0, 1u, &bar0);
    Igpu->Pci.Read(Igpu, EfiPciIoWidthUint32, IGPU_PCI_BAR0 + 4u, 1u, &bar1);
    UINT64 base = (UINT64)(bar0 & ~0xFu) | ((UINT64)bar1 << 32);
    if ((bar0 & 1u) || base == 0u) {
        log_str(L, "  [eDP/DDI_A] BAR0 unassigned: 0x"); log_hex(L, bar0, 8); log_nl(L);
        return;
    }
    if (!(cmd & 0x2u)) {
        EFI_STATUS en = Igpu->Attributes(Igpu, EfiPciIoAttributeOperationEnable,
                                         EFI_PCI_IO_ATTRIBUTE_MEMORY, NULL);
        log_str(L, "  [eDP/DDI_A] Enabled MSE status=0x"); log_hex(L, en, 16); log_nl(L);
        if (EFI_ERROR(en)) return;
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
            log_str(L, "    Forced panel VDD for AUX-A probe"); log_nl(L);
        }

        UINT8 dpcd[16];
        EFI_STATUS dst = DpcdRead(BS, Igpu, 0x000u, dpcd, 16u);
        if (EFI_ERROR(dst)) {
            log_str(L, "    DPCD 0x000 read FAILED: 0x"); log_hex(L, dst, 16);
            log_str(L, " AUX_CTL=0x"); log_hex(L, IgpuRd32(Igpu, DP_AUX_CH_CTL_A), 8); log_nl(L);
        } else {
            UINT8 edp_rev = 0, psr_cap = 0;
            DpcdRead(BS, Igpu, 0x700u, &edp_rev, 1u);
            DpcdRead(BS, Igpu, 0x070u, &psr_cap, 1u);
            log_str(L, "    DPCD[0..15]: rev=0x"); log_hex(L, dpcd[0], 2);
            log_str(L, " max_rate=0x"); log_hex(L, dpcd[1], 2);
            log_str(L, " max_lanes="); log_dec(L, dpcd[2] & 0x1Fu);
            log_str(L, " eDP_cfg=0x"); log_hex(L, dpcd[0x0Du], 2);
            log_str(L, " eDP_rev="); log_dec(L, edp_rev);
            log_str(L, " PSR_cap=0x"); log_hex(L, psr_cap, 2); log_nl(L);

            UINT8 off = 0u;
            if (!EFI_ERROR(I2cOverAux(BS, Igpu, FALSE, TRUE, &off, 1u))) {
                BOOLEAN ed_ok = TRUE;
                for (UINTN i = 0; i < 128u; i += 16u) {
                    if (EFI_ERROR(I2cOverAux(BS, Igpu, TRUE, (i + 16u) < 128u, AuxEdidOut + i, 16u))) {
                        ed_ok = FALSE;
                        break;
                    }
                }
                if (ed_ok && EdidLooksInternal(AuxEdidOut, 128u)) {
                    *HasAuxEdid = TRUE;
                    log_str(L, "    I2C-over-AUX EDID OK (valid APPA030 128B)"); log_nl(L);
                } else {
                    log_str(L, "    I2C-over-AUX EDID read incomplete/invalid"); log_nl(L);
                }
            } else {
                log_str(L, "    I2C-over-AUX EDID offset NACK/fail"); log_nl(L);
            }
        }

        if (forced_vdd) {
            UINT32 now = IgpuRd32(Igpu, PP_CONTROL);
            IgpuWr32(Igpu, PP_CONTROL, ((now & 0xFFFFu) & ~PP_FORCE_VDD) | PP_UNLOCK);
            log_str(L, "    Released panel VDD override"); log_nl(L);
        }
    }

    UINT32 ddi_before = IgpuRd32(Igpu, DDI_BUF_CTL_A);
    log_str(L, "  [DDI_BUF_CTL_A] before=0x"); log_hex(L, ddi_before, 8);
    if (ddi_before == 0xFFFFFFFFu) {
        log_str(L, " (all ones - skipped!)"); log_nl(L);
        return;
    }
    if ((ddi_before & DDI_A_4_LANES) == 0u) {
        IgpuWr32(Igpu, DDI_BUF_CTL_A, ddi_before | DDI_A_4_LANES);
    }
    UINT32 ddi_after = IgpuRd32(Igpu, DDI_BUF_CTL_A);
    log_str(L, " after=0x"); log_hex(L, ddi_after, 8);
    log_str(L, (ddi_after & DDI_A_4_LANES) ? " [4_LANES OK]" : " [4_LANES FAILED]");
    log_nl(L);
}

/* ---- In-Memory DSDT Patcher + SSDT_IGPU_MBP133.aml Injector ---- */

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

static EFI_STATUS ApplyAcpiPatch(EFI_BOOT_SERVICES *BS, EFI_SYSTEM_TABLE *ST,
                                 EFI_HANDLE ImageHandle, LOG_BUF *L) {
    static CHAR16 SsdtPath[] = L"\\EFI\\tools\\SSDT_IGPU_MBP133.aml";
    VOID *file_data = NULL;
    UINTN file_size = 0;

    log_str(L, "  [ACPI Patcher] Loading "); log_str(L, "\\EFI\\tools\\SSDT_IGPU_MBP133.aml"); log_nl(L);
    EFI_STATUS st = ReadUsbFile(BS, ImageHandle, SsdtPath, &file_data, &file_size);
    if (EFI_ERROR(st) || !file_data) {
        log_str(L, "    ReadUsbFile failed: 0x"); log_hex(L, st, 16); log_nl(L);
        return EFI_ERROR(st) ? st : EFI_NOT_FOUND;
    }

    const UINT8 *aml = (const UINT8 *)file_data;
    if (file_size < ACPI_HDR_SIZE || !ascii_eq_n(aml, "SSDT", 4u) ||
        rd32(aml + ACPI_HDR_LEN) != (UINT32)file_size || sum8(aml, file_size) != 0u) {
        log_str(L, "    Invalid SSDT file (sig/len/csum)"); log_nl(L);
        BS->FreePool(file_data);
        return EFI_COMPROMISED_DATA;
    }

    ACPI_RSDP_20 *rsdp = NULL;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        if (guid_eq(&ST->ConfigurationTable[i].VendorGuid, &gAcpi20Guid)) {
            rsdp = (ACPI_RSDP_20 *)ST->ConfigurationTable[i].VendorTable;
            break;
        }
    }
    if (!rsdp || !ascii_eq_n(rsdp->Signature, "RSD PTR ", 8u) ||
        rsdp->Revision < 2u || rsdp->XsdtAddress == 0u) {
        log_str(L, "    ACPI 2.0 RSDP/XSDT missing"); log_nl(L);
        BS->FreePool(file_data);
        return EFI_UNSUPPORTED;
    }

    UINT8 *xsdt = (UINT8 *)(UINTN)rsdp->XsdtAddress;
    UINT32 xlen = rd32(xsdt + ACPI_HDR_LEN);
    if (!ascii_eq_n(xsdt, "XSDT", 4u) || xlen < ACPI_HDR_SIZE || sum8(xsdt, xlen) != 0u) {
        log_str(L, "    Invalid XSDT"); log_nl(L);
        BS->FreePool(file_data);
        return EFI_COMPROMISED_DATA;
    }
    UINTN n_entries = (xlen - ACPI_HDR_SIZE) / 8u;

    for (UINTN i = 0; i < n_entries; i++) {
        const UINT8 *t = (const UINT8 *)(UINTN)rd64(xsdt + ACPI_HDR_SIZE + i * 8u);
        if (t && ascii_eq_n(t, "SSDT", 4u) &&
            ascii_eq_n(t + ACPI_HDR_OEMTID, (const char *)(aml + ACPI_HDR_OEMTID), 8u) &&
            rd32(t + ACPI_HDR_LEN) == (UINT32)file_size) {
            log_str(L, "    SSDT already present in XSDT"); log_nl(L);
            BS->FreePool(file_data);
            return EFI_ALREADY_STARTED;
        }
    }

    UINT8 *dsdt = FindDsdtViaFadt(xsdt, n_entries);
    if (!dsdt || !ascii_eq_n(dsdt, "DSDT", 4u)) {
        log_str(L, "    DSDT not found via FACP"); log_nl(L);
        BS->FreePool(file_data);
        return EFI_NOT_FOUND;
    }
    UINT32 dlen = rd32(dsdt + ACPI_HDR_LEN);
    if (dlen < ACPI_HDR_SIZE || sum8(dsdt, dlen) != 0u) {
        log_str(L, "    DSDT checksum/len invalid"); log_nl(L);
        BS->FreePool(file_data);
        return EFI_COMPROMISED_DATA;
    }

    UINTN c_bcm = 0, c_xbcm = 0, c_pts = 0, c_xpts = 0, c_wak = 0, c_xwak = 0;
    UINTN off_bcm = FindMethodInAml(dsdt, dlen, "_BCM", &c_bcm);
    (void)FindMethodInAml(dsdt, dlen, "XBCM", &c_xbcm);
    UINTN off_pts = FindMethodInAml(dsdt, dlen, "_PTS", &c_pts);
    (void)FindMethodInAml(dsdt, dlen, "XPTS", &c_xpts);
    UINTN off_wak = FindMethodInAml(dsdt, dlen, "_WAK", &c_wak);
    (void)FindMethodInAml(dsdt, dlen, "XWAK", &c_xwak);

    log_str(L, "    DSDT len="); log_dec(L, dlen);
    log_str(L, " _BCM("); log_dec(L, c_bcm); log_str(L, ")@0x"); log_hex(L, off_bcm, 4);
    log_str(L, " _PTS("); log_dec(L, c_pts); log_str(L, ")@0x"); log_hex(L, off_pts, 4);
    log_str(L, " _WAK("); log_dec(L, c_wak); log_str(L, ")@0x"); log_hex(L, off_wak, 4); log_nl(L);

    if (c_bcm != 1u || off_bcm == 0u || c_xbcm != 0u ||
        c_pts != 1u || off_pts == 0u || c_xpts != 0u ||
        c_wak != 1u || off_wak == 0u || c_xwak != 0u) {
        log_str(L, "    Unexpected DSDT layout, aborted (nothing modified)"); log_nl(L);
        BS->FreePool(file_data);
        return EFI_UNSUPPORTED;
    }

    UINT8 *new_ssdt = NULL, *new_xsdt = NULL;
    ACPI_RSDP_20 *new_rsdp = NULL;
    UINTN rlen = (rsdp->Length < sizeof(ACPI_RSDP_20)) ? sizeof(ACPI_RSDP_20) : rsdp->Length;

    if (EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, file_size, (VOID **)&new_ssdt)) ||
        EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, (UINTN)xlen + 8u, (VOID **)&new_xsdt)) ||
        EFI_ERROR(BS->AllocatePool(EfiACPIReclaimMemory, rlen, (VOID **)&new_rsdp))) {
        log_str(L, "    AllocatePool(ACPIReclaim) failed"); log_nl(L);
        if (new_ssdt) BS->FreePool(new_ssdt);
        if (new_xsdt) BS->FreePool(new_xsdt);
        BS->FreePool(file_data);
        return EFI_OUT_OF_RESOURCES;
    }

    mem_copy(new_ssdt, aml, file_size);
    mem_copy(new_xsdt, xsdt, xlen);
    UINT64 ssdt_pa = (UINT64)(UINTN)new_ssdt;
    mem_copy(new_xsdt + xlen, &ssdt_pa, 8u);
    UINT32 new_xlen = xlen + 8u;
    mem_copy(new_xsdt + ACPI_HDR_LEN, &new_xlen, 4u);
    new_xsdt[ACPI_HDR_CSUM] = 0u;
    new_xsdt[ACPI_HDR_CSUM] = (UINT8)(0u - sum8(new_xsdt, new_xlen));

    mem_copy(new_rsdp, rsdp, rlen);
    new_rsdp->XsdtAddress = (UINT64)(UINTN)new_xsdt;
    new_rsdp->ExtChecksum = 0u;
    new_rsdp->ExtChecksum = (UINT8)(0u - sum8((const UINT8 *)new_rsdp, new_rsdp->Length));

    /* Rename _BCM/_PTS/_WAK in DSDT and recompute DSDT checksum */
    UINT8 old_dcsum = dsdt[ACPI_HDR_CSUM];
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
        log_str(L, "    DSDT memory not writable, reverted"); log_nl(L);
        BS->FreePool(new_ssdt); BS->FreePool(new_xsdt); BS->FreePool(new_rsdp);
        BS->FreePool(file_data);
        return EFI_ACCESS_DENIED;
    }

    st = BS->InstallConfigurationTable(&gAcpi20Guid, new_rsdp);
    if (EFI_ERROR(st)) {
        vdsdt[off_bcm] = '_';
        vdsdt[off_pts] = '_';
        vdsdt[off_wak] = '_';
        vdsdt[ACPI_HDR_CSUM] = old_dcsum;
        log_str(L, "    InstallConfigurationTable failed: 0x"); log_hex(L, st, 16); log_nl(L);
        BS->FreePool(new_ssdt); BS->FreePool(new_xsdt); BS->FreePool(new_rsdp);
        BS->FreePool(file_data);
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

    log_str(L, "    ACPI patch OK: DSDT _BCM/_PTS/_WAK->X*, SSDT@0x");
    log_hex(L, ssdt_pa, 8);
    log_str(L, " XSDT entries="); log_dec(L, n_entries + 1u);
    log_str(L, old_synced ? " (old RSDP synced)" : " (via config table)"); log_nl(L);

    BS->FreePool(file_data);
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

/* ---- Mode Runner ---- */

static void ExecuteM1BMode(EFI_BOOT_SERVICES *BS, EFI_SYSTEM_TABLE *ST,
                           EFI_HANDLE ImageHandle, LOG_BUF *L, UINTN Mode) {
    SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut = ST->ConOut;
    SIMPLE_INPUT_INTERFACE *ConIn = ST->ConIn;
    BOOLEAN patch_vbt   = (Mode == 2u);
    BOOLEAN do_rail_off = (Mode == 1u || Mode == 2u);

    L->Len = 0;
    L->Truncated = FALSE;
    if (L->Buf) L->Buf[0] = 0;

    ConOut->ClearScreen(ConOut);
    ConPrintA(ConOut,
        "========================================================================\n"
        "  Executing Milestone 1B Mode ");
    ConPrintHex(ConOut, Mode, 1);
    ConPrintA(ConOut, "\n"
        "  NOTE: After GMUX switches panel to Intel HD 530, screen goes DARK\n"
        "  for ~10-25s until Windows igdkmd64.sys initializes the eDP link.\n"
        "  All logs will be flushed to USB \\M1B_LOGS\\ BEFORE Windows boots.\n"
        "========================================================================\n\n"
        "Step 1/7: Pre-switch EDID, GMUX & native VBT capture...\n");

    log_str(L, "MBP133_M1B_IGPU_V1"); log_nl(L);
    log_str(L, "MODE="); log_dec(L, Mode);
    log_str(L, (Mode == 1u) ? " (Intel-Only Native VBT)" :
               (Mode == 2u) ? " (Intel-Only Patched VBT)" :
                              " (Dry-Run Rail ON)");
    log_nl(L);

    UINT8 fw_edid[128], aux_edid[128];
    mem_zero(fw_edid, sizeof(fw_edid));
    mem_zero(aux_edid, sizeof(aux_edid));
    BOOLEAN has_fw_edid = CaptureFirmwareEdid(BS, fw_edid, L);
    BOOLEAN has_aux_edid = FALSE;

    LogGmuxState(BS, L, "BEFORE");
    EFI_PCI_IO_PROTOCOL *igpu = FindIgpu(BS, ImageHandle, L);
    InspectAndDumpNativeVbt(BS, ImageHandle, igpu, L);

    ConPrintA(ConOut, "Step 2/7: Invoking AppleSetOs(\"Mac OS X 10.15\")...\n");
    InvokeAppleSetOs(BS, ImageHandle, L);

    ConPrintA(ConOut, "Step 3/7: Switching GMUX panel -> IGD (screen will go dark!)...\n");
    /* Small pause so the user sees Step 3 before the panel switches away from AMD GOP */
    BS->Stall(300000);
    GmuxSwitchToIgdAndCutRail(BS, do_rail_off, L);
    BS->Stall(800000);

    LogGmuxState(BS, L, "AFTER");

    /* Step 4/7: eDP AUX-A probe & DDI_BUF_CTL_A 4-lanes */
    ProbeEdpAndForce4Lanes(BS, igpu, aux_edid, &has_aux_edid, L);

    /* Step 5/7: Optional VBT patch (Mode 2) + BDB[43] backlight NONE for all modes */
    if (patch_vbt) {
        const UINT8 *chosen = has_fw_edid ? fw_edid : (has_aux_edid ? aux_edid : NULL);
        PatchNativeVbt(BS, ImageHandle, igpu, chosen, chosen != NULL, L);
    } else {
        log_str(L, "  [VBT Patch] Keeping Apple native $VBT SKYLAKE timings"); log_nl(L);
    }
    PatchVbtBacklightNone(BS, ImageHandle, igpu, L);

    /* Step 6/7: In-memory DSDT renames + SSDT_IGPU_MBP133.aml */
    ApplyAcpiPatch(BS, ST, ImageHandle, L);

    /* Step 7/7: Flush logs to USB BEFORE chainloading Windows */
    log_str(L, "LOG_BYTES="); log_dec(L, L->Len);
    log_str(L, " TRUNCATED="); log_dec(L, L->Truncated ? 1u : 0u);
    log_nl(L);

    CHAR16 *mode_file = (Mode == 1u) ? L"\\M1B_LOGS\\m1b_mode1.txt" :
                        (Mode == 2u) ? L"\\M1B_LOGS\\m1b_mode2.txt" :
                                       L"\\M1B_LOGS\\m1b_mode3.txt";
    WriteUsbFile(BS, ImageHandle, mode_file, L->Buf, L->Len);
    WriteUsbFile(BS, ImageHandle, L"\\M1B_LOGS\\m1b_last.txt", L->Buf, L->Len);
    BS->Stall(500000);

    EFI_STATUS bst = ChainloadWindows(BS, ImageHandle, ConOut);
    if (EFI_ERROR(bst)) {
        ConPrintA(ConOut, "\nChainload failed. Press any key...\n");
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

    LOG_BUF L;
    mem_zero(&L, sizeof(L));
    L.Cap = LOG_CAP;
    if (EFI_ERROR(BS->AllocatePool(EfiBootServicesData, L.Cap, (VOID **)&L.Buf)) || !L.Buf) {
        ConPrintA(ConOut, "FATAL: AllocatePool(128KB) failed\n");
        return EFI_OUT_OF_RESOURCES;
    }

    while (1) {
        ConOut->ClearScreen(ConOut);
        ConPrintA(ConOut,
            "========================================================================\n"
            "  MacBookPro13,3 Milestone 1B — Intel-Only Mode (GMUX + HD 530)\n"
            "  Indexed I/O GMUX 0x700..0x7FE | USB-only (NVMe ESP & NVRAM untouched)\n"
            "========================================================================\n\n"
            "  [1] Intel-Only (Apple VBT + BDB43 NONE + DDI 4-Lanes + GMUX IGD + Rail OFF + SSDT)\n"
            "  [2] Intel-Only + Patch VBT (Apply APPA030 EDID + Force PSR OFF)\n"
            "  [3] Panel-Only Dry-Run (GMUX IGD + DDI 4-Lanes + SSDT, AMD Rail ON)\n"
            "  [0] Cancel / Normal Windows Boot (zero hardware writes)\n"
            "  [R] Cold Reboot\n\n"
            "Select option: ");

        FlushKeys(ConIn);
        EFI_INPUT_KEY k;
        mem_zero(&k, sizeof(k));
        ReadKeyWait(BS, ConIn, &k);
        CHAR16 c = k.UnicodeChar;

        if (c == L'1') {
            ExecuteM1BMode(BS, SystemTable, ImageHandle, &L, 1u);
        } else if (c == L'2') {
            ExecuteM1BMode(BS, SystemTable, ImageHandle, &L, 2u);
        } else if (c == L'3') {
            ExecuteM1BMode(BS, SystemTable, ImageHandle, &L, 3u);
        } else if (c == L'0') {
            ConPrintA(ConOut, "\nNormal Windows Boot (zero hardware writes)...\n");
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

    BS->FreePool(L.Buf);
    return EFI_SUCCESS;
}

/* gnu-efi 4.0.0 crt0-efi-x86_64.o invokes _entry(ImageHandle, SystemTable) */
EFI_STATUS _entry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    return efi_main(ImageHandle, SystemTable);
}
