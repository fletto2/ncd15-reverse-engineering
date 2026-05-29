# xcpm-mips — XCP/M ported to the NCD15 (MIPS R3052)

This is a port of [XCP/M-68K](https://github.com/fletto2/xcpm68k) — a small
ROM-resident, FAT-native, CP/M-68K-compatible OS — to the **NCD15
X-terminal** (LSI/IDT R3052, MIPS-I, big-endian, no MMU).

The goal is **Route B**: recompile the portable C of XCP/M for MIPS so
that *new* apps can be built natively for the NCD15 hardware. Legacy
`.68K` binary compatibility is dropped (the CPU is MIPS, not 68K). The
OS core (BDOS / BIOS / FAT / lzss / ramdisk) is reused unchanged except
for five small `#ifdef __mips__` guards around 68K-specific inline asm.

## Status (snapshot)

| Milestone | What | State |
|---|---|---|
| **M1** | Boots, banner, serial console | ✅ |
| **M2** | OS core runs (BDOS/BIOS/FAT/ramdisk via direct dispatch) | ✅ |
| **M3** | CCP shell runs (`ver`, `help`, command echo, prompt) | ✅ |
| **M4** | App loading (syscall ABI + RAM-FAT + crt0/libxcpm) | in progress |

See `NOTES.md` for the per-milestone trail and verification output.

## Build

Needs the MIPS-I cross-toolchain at `/opt/cross/mips-elf/` and the
`ncd15-ecoff-wrap` helper from the parent repo (`xncd15r-mini/`).

```sh
cd xcpm-mips
make             # -> xcpm-ncd15.bin (ECOFF the NCD15 monitor accepts)
```

The output is an ECOFF image with the `Xncd19r` vendor signature + CRC
stash that the monitor's `BT`/`BL` loaders require.

## Boot in the emulator

The emulator (in `../emulator/`) accepts a `--flash <file>` argument and
a `--boot-flash` shortcut that jumps straight to the preloaded flash
entry once the monitor finishes POST + DUART + cache init. This lets
you iterate on guest images without satisfying the monitor's full
BL/BT validator.

```sh
EMU=../emulator/ncd15-emu
ROM=../../NCD15-19rBM-V271-splice.u8
$EMU --no-window --boot-flash --nvram nv.bin --flash xcpm-ncd15.bin $ROM
```

Expected output (M3):

```
XCP/M (NCD15/MIPS port) v0.1 -- milestone 2
Board: ncd15  CPU: LSI/IDT R3052 (MIPS-I)
RAM  0x0ED00000..0x0F000000  TPA 0x0ED40000
BIOS+BDOS dispatchers initialised.
Self-test (direct dispatch):
  BDOS 12 (version)   = 0x0022 (expect 0x0022)
  BDOS 100 (ident)    = 0x58504D4B (expect 0x58504D4B)
  BIOS 18 (getseg)    = 0x0ED0DE70
  BDOS 9 (printstr)  : hello via dispatch
  B: ramdisk (32 sec) head=A0 A1 A2 A3 A4 A5 A6 A7 (expect A0 A1 ..)

Launching CCP...
[CCP loaded into TPA from compressed ROM image]
A:\> ver
XCP/M-68K v0.1 (compressed CCP)
BDOS version (BDOS 12) = 0x0022
XCP/M magic   (BDOS 100) = 0x58504D4B
A:\>
```

## Layout

```
xcpm-mips/
├── Makefile                   board-specific build (paths into ./src, ./include, ./src_ccp)
├── ncd15.ld                   link at phys 0x0ED00000, 3 MB DRAM map
├── start.S                    MIPS-I entry + Xncd19r ECOFF signature preamble
├── main_ncd15.c               board main: init, self-test, launch CCP, warmboot
├── glue_ncd15.c               MIPS replacements for 68K traps.s / pgmld_jump.s
├── include/                   shared XCP/M headers (xcpm.h, bdos.h, bios.h, ccp.h, fat.h)
├── src/                       portable XCP/M core
│   ├── console_mc68681.c      DUART driver, parameterised by DUART_STRIDE / DUART_LANE
│   ├── bdos.c                 BDOS dispatcher (5 #ifdef __mips__ guards)
│   ├── bios.c                 BIOS dispatcher (1 guard)
│   ├── fat.c                  endian-explicit FAT reader (LE on-disk → host)
│   ├── lzss.c                 LZSS decompressor for CCP / help / stty blobs
│   ├── ramdisk.c              B: RAM block device
│   └── disk_null.c            placeholder A: until M4's FAT block device
└── src_ccp/
    └── ccpmain.c              CCP shell — bdos()/bios() wrappers MIPS-guarded
```

## Arch-specific changes (vs. the 68K tree)

These five guard sites are the entire MIPS-port delta in the shared C
(everything else is bit-for-bit the 68K source):

- `src/console_mc68681.c` — `DUART_STRIDE` / `DUART_LANE` macros (default
  1/0 for PVS-2; NCD15 passes 4/2). Register N at
  `BASE + N*STRIDE + LANE`.
- `src/bdos.c` — two warmboot inline-asm blocks + `bdos_init`'s TRAP #2
  install guarded `#if defined(__mips__)`. MIPS warmboot uses
  `la $sp,_stack_top; j warmboot`.
- `src/bios.c` — `b_setexc` body + `bios_init`'s TRAP #3 install guarded.
- `src_ccp/ccpmain.c` — `bdos()` / `bios()` wrappers guarded. The CCP is
  linked with the kernel in single-mode so it calls `bdos_dispatch` /
  `bios_dispatch` directly. The syscall ABI is for *external* apps (M4).

## Hardware-specific bits (the port)

- **Entry**: 7-instruction preamble at flash[0] = `b real_start; nop;
  …; .ascii "Xncd19r"; .hword 0xFFFF`. The monitor's BL/BT loaders
  verify these bytes before transferring control.
- **DUART**: SCN2681 at KSEG1 `0xBE880000`, 4-byte register stride,
  chip wired on data lines D15..D8 (register byte at word+2).
  **Channel B** is the monitor's console.
- **Link map** (`ncd15.ld`): code at phys `0x0ED00000`, TPA base at
  `0x0ED40000`, stack top at `0x0EFE0000`, 3 MB DRAM total. BSS is
  8-byte aligned so the `sw zero, 0(t0)` clear loop doesn't trap.

## Project links

- Parent OS: <https://github.com/fletto2/xcpm68k>
- NCD15 RE notes: see `../FINDINGS.md`, `../MONITOR.md`,
  `../NEW_INSIGHTS.md`
- ECOFF wrapper for the monitor's loader: `../xncd15r-mini/`
  (`ncd15-ecoff-wrap` source)
- Emulator: `../emulator/`
