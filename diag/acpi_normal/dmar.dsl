/*
 * Intel ACPI Component Architecture
 * AML/ASL+ Disassembler version 20260408 (32-bit version)
 * Copyright (c) 2000 - 2026 Intel Corporation
 * 
 * Disassembly of dmar.dat
 *
 * ACPI Data Table [DMAR]
 *
 * Format: [HexOffset DecimalOffset ByteLength]  FieldName : FieldValue (in hex)
 */

[000h 0000 004h]                   Signature : "DMAR"    [DMA Remapping Table]
[004h 0004 004h]                Table Length : 00000160
[008h 0008 001h]                    Revision : 01
[009h 0009 001h]                    Checksum : E0
[00Ah 0010 006h]                      Oem ID : "APPLE "
[010h 0016 008h]                Oem Table ID : "SKL "
[018h 0024 004h]                Oem Revision : 00000001
[01Ch 0028 004h]             Asl Compiler ID : "INTL"
[020h 0032 004h]       Asl Compiler Revision : 00000001

[024h 0036 001h]          Host Address Width : 26
[025h 0037 001h]                       Flags : 01
[026h 0038 00Ah]                    Reserved : 00 00 00 00 00 00 00 00 00 00

[030h 0048 002h]               Subtable Type : 0000 [Hardware Unit Definition]
[032h 0050 002h]                      Length : 0018

[034h 0052 001h]                       Flags : 00
[035h 0053 001h]        Size (decoded below) : 00
                          Size (pages, log2) : 0
[036h 0054 002h]          PCI Segment Number : 0000
[038h 0056 008h]       Register Base Address : 00000000FED90000

[040h 0064 001h]           Device Scope Type : 01 [PCI Endpoint Device]
[041h 0065 001h]                Entry Length : 08
[042h 0066 001h]                       Flags : 00
[043h 0067 001h]                    Reserved : 00
[044h 0068 001h]              Enumeration ID : 00
[045h 0069 001h]              PCI Bus Number : 00

[046h 0070 002h]                    PCI Path : 02,00


[048h 0072 002h]               Subtable Type : 0000 [Hardware Unit Definition]
[04Ah 0074 002h]                      Length : 0050

[04Ch 0076 001h]                       Flags : 01
[04Dh 0077 001h]        Size (decoded below) : 00
                          Size (pages, log2) : 0
[04Eh 0078 002h]          PCI Segment Number : 0000
[050h 0080 008h]       Register Base Address : 00000000FED91000

[058h 0088 001h]           Device Scope Type : 03 [IOAPIC Device]
[059h 0089 001h]                Entry Length : 08
[05Ah 0090 001h]                       Flags : 00
[05Bh 0091 001h]                    Reserved : 00
[05Ch 0092 001h]              Enumeration ID : 02
[05Dh 0093 001h]              PCI Bus Number : 00

[05Eh 0094 002h]                    PCI Path : 1F,00


[060h 0096 001h]           Device Scope Type : 04 [Message-capable HPET Device]
[061h 0097 001h]                Entry Length : 08
[062h 0098 001h]                       Flags : 00
[063h 0099 001h]                    Reserved : 00
[064h 0100 001h]              Enumeration ID : 00
[065h 0101 001h]              PCI Bus Number : 00

[066h 0102 002h]                    PCI Path : 1F,00


[068h 0104 001h]           Device Scope Type : 05 [Namespace Device]
[069h 0105 001h]                Entry Length : 08
[06Ah 0106 001h]                       Flags : 00
[06Bh 0107 001h]                    Reserved : 00
[06Ch 0108 001h]              Enumeration ID : 01
[06Dh 0109 001h]              PCI Bus Number : 00

[06Eh 0110 002h]                    PCI Path : 15,00


[070h 0112 001h]           Device Scope Type : 05 [Namespace Device]
[071h 0113 001h]                Entry Length : 08
[072h 0114 001h]                       Flags : 00
[073h 0115 001h]                    Reserved : 00
[074h 0116 001h]              Enumeration ID : 07
[075h 0117 001h]              PCI Bus Number : 00

[076h 0118 002h]                    PCI Path : 1E,02


[078h 0120 001h]           Device Scope Type : 05 [Namespace Device]
[079h 0121 001h]                Entry Length : 08
[07Ah 0122 001h]                       Flags : 00
[07Bh 0123 001h]                    Reserved : 00
[07Ch 0124 001h]              Enumeration ID : 08
[07Dh 0125 001h]              PCI Bus Number : 00

[07Eh 0126 002h]                    PCI Path : 1E,03


[080h 0128 001h]           Device Scope Type : 05 [Namespace Device]
[081h 0129 001h]                Entry Length : 08
[082h 0130 001h]                       Flags : 00
[083h 0131 001h]                    Reserved : 00
[084h 0132 001h]              Enumeration ID : 09
[085h 0133 001h]              PCI Bus Number : 00

[086h 0134 002h]                    PCI Path : 1E,00


[088h 0136 001h]           Device Scope Type : 05 [Namespace Device]
[089h 0137 001h]                Entry Length : 08
[08Ah 0138 001h]                       Flags : 00
[08Bh 0139 001h]                    Reserved : 00
[08Ch 0140 001h]              Enumeration ID : 0A
[08Dh 0141 001h]              PCI Bus Number : 00

[08Eh 0142 002h]                    PCI Path : 1E,01


[090h 0144 001h]           Device Scope Type : 05 [Namespace Device]
[091h 0145 001h]                Entry Length : 08
[092h 0146 001h]                       Flags : 00
[093h 0147 001h]                    Reserved : 00
[094h 0148 001h]              Enumeration ID : 0B
[095h 0149 001h]              PCI Bus Number : 00

[096h 0150 002h]                    PCI Path : 19,00


[098h 0152 002h]               Subtable Type : 0001 [Reserved Memory Region]
[09Ah 0154 002h]                      Length : 0020

[09Ch 0156 002h]                    Reserved : 0000
[09Eh 0158 002h]          PCI Segment Number : 0000
[0A0h 0160 008h]                Base Address : 000000007B800000
[0A8h 0168 008h]         End Address (limit) : 000000007FFFFFFF

[0B0h 0176 001h]           Device Scope Type : 01 [PCI Endpoint Device]
[0B1h 0177 001h]                Entry Length : 08
[0B2h 0178 001h]                       Flags : 00
[0B3h 0179 001h]                    Reserved : 00
[0B4h 0180 001h]              Enumeration ID : 00
[0B5h 0181 001h]              PCI Bus Number : 00

[0B6h 0182 002h]                    PCI Path : 02,00


[0B8h 0184 002h]               Subtable Type : 0004 [ACPI Namespace Device Declaration]
[0BAh 0186 002h]                      Length : 001C

[0BCh 0188 003h]                    Reserved : 000000
[0BFh 0191 001h]               Device Number : 01
[0C0h 0192 00Fh]                 Device Name : "\_SB.PCI0.I2C0"

[0D4h 0212 002h]               Subtable Type : 0004 [ACPI Namespace Device Declaration]
[0D6h 0214 002h]                      Length : 001C

[0D8h 0216 003h]                    Reserved : 000000
[0DBh 0219 001h]               Device Number : 07
[0DCh 0220 00Fh]                 Device Name : "\_SB.PCI0.SPI0"

[0F0h 0240 002h]               Subtable Type : 0004 [ACPI Namespace Device Declaration]
[0F2h 0242 002h]                      Length : 001C

[0F4h 0244 003h]                    Reserved : 000000
[0F7h 0247 001h]               Device Number : 08
[0F8h 0248 00Fh]                 Device Name : "\_SB.PCI0.SPI1"

[10Ch 0268 002h]               Subtable Type : 0004 [ACPI Namespace Device Declaration]
[10Eh 0270 002h]                      Length : 001C

[110h 0272 003h]                    Reserved : 000000
[113h 0275 001h]               Device Number : 09
[114h 0276 00Fh]                 Device Name : "\_SB.PCI0.UA00"

[128h 0296 002h]               Subtable Type : 0004 [ACPI Namespace Device Declaration]
[12Ah 0298 002h]                      Length : 001C

[12Ch 0300 003h]                    Reserved : 000000
[12Fh 0303 001h]               Device Number : 0A
[130h 0304 00Fh]                 Device Name : "\_SB.PCI0.UA01"

[144h 0324 002h]               Subtable Type : 0004 [ACPI Namespace Device Declaration]
[146h 0326 002h]                      Length : 001C

[148h 0328 003h]                    Reserved : 000000
[14Bh 0331 001h]               Device Number : 0B
[14Ch 0332 00Fh]                 Device Name : "\_SB.PCI0.UA02"

Raw Table Data: Length 352 (0x160)

    0000: 44 4D 41 52 60 01 00 00 01 E0 41 50 50 4C 45 20  // DMAR`.....APPLE 
    0010: 53 4B 4C 20 00 00 00 00 01 00 00 00 49 4E 54 4C  // SKL ........INTL
    0020: 01 00 00 00 26 01 00 00 00 00 00 00 00 00 00 00  // ....&...........
    0030: 00 00 18 00 00 00 00 00 00 00 D9 FE 00 00 00 00  // ................
    0040: 01 08 00 00 00 00 02 00 00 00 50 00 01 00 00 00  // ..........P.....
    0050: 00 10 D9 FE 00 00 00 00 03 08 00 00 02 00 1F 00  // ................
    0060: 04 08 00 00 00 00 1F 00 05 08 00 00 01 00 15 00  // ................
    0070: 05 08 00 00 07 00 1E 02 05 08 00 00 08 00 1E 03  // ................
    0080: 05 08 00 00 09 00 1E 00 05 08 00 00 0A 00 1E 01  // ................
    0090: 05 08 00 00 0B 00 19 00 01 00 20 00 00 00 00 00  // .......... .....
    00A0: 00 00 80 7B 00 00 00 00 FF FF FF 7F 00 00 00 00  // ...{............
    00B0: 01 08 00 00 00 00 02 00 04 00 1C 00 00 00 00 01  // ................
    00C0: 5C 5F 53 42 2E 50 43 49 30 2E 49 32 43 30 00 00  // \_SB.PCI0.I2C0..
    00D0: 00 00 00 00 04 00 1C 00 00 00 00 07 5C 5F 53 42  // ............\_SB
    00E0: 2E 50 43 49 30 2E 53 50 49 30 00 00 00 00 00 00  // .PCI0.SPI0......
    00F0: 04 00 1C 00 00 00 00 08 5C 5F 53 42 2E 50 43 49  // ........\_SB.PCI
    0100: 30 2E 53 50 49 31 00 00 00 00 00 00 04 00 1C 00  // 0.SPI1..........
    0110: 00 00 00 09 5C 5F 53 42 2E 50 43 49 30 2E 55 41  // ....\_SB.PCI0.UA
    0120: 30 30 00 00 00 00 00 00 04 00 1C 00 00 00 00 0A  // 00..............
    0130: 5C 5F 53 42 2E 50 43 49 30 2E 55 41 30 31 00 00  // \_SB.PCI0.UA01..
    0140: 00 00 00 00 04 00 1C 00 00 00 00 0B 5C 5F 53 42  // ............\_SB
    0150: 2E 50 43 49 30 2E 55 41 30 32 00 00 00 00 00 00  // .PCI0.UA02......
