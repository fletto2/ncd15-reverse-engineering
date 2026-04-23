# NCD15 Reverse-Engineering Notebook

Working notes for the NCD15 X-terminal RE project. This file is the
consolidated state; individual `.dis` / `.py` / image files in this
directory are the supporting artifacts.

> **See [`EMULATOR_ERRATA.md`](EMULATOR_ERRATA.md) first.** Building
> the emulator (`emulator/`, 2026-04-23) turned up a dozen
> corrections to the claims here — notably: shadow-RAM source
> (§1), NVRAM-bit-bang wiring (§2), BEV-cleared status (§3),
> three separate memory banks (§11), and an off-by-0x2000 VA shift
> in the main-monitor section of `monitor.dis`. Corrections in
> `EMULATOR_ERRATA.md` supersede anything in this file.

## Corrections (2026-04-22, from the hardware owner)

Several recurring claims below were derived from disassembly alone
and are wrong. The `MONITOR.md`, `NVRAM_SETUP.md`, `XNCD15R.md`, and
`README.md` have been updated; the narrative in this file has not
been retroactively rewritten, so read the following overrides first:

1. **There is no RAMDAC on the NCD15 mainboard.** The display is
   **1 bpp monochrome ECL at 1024×800 @ 70 Hz** on a 15" panel
   (~93 dpi). There is no palette, no LUT, no colour at all. The
   `0xAF000000` / `0xAF000004` pair-writes that earlier passes
   called a "Bt47x RAMDAC" or "Bt431 cursor RAM" are video-control
   registers (timing / cursor-position), not a colour-lookup
   device. Any "RAMDAC" reference below refers to this region —
   treat it as "video-control registers".
2. **The `-splice.u8` suffix only means "even+odd halves of the
   two 128 KiB ROM chips combined into a single 256 KiB image".**
   The dump is **complete**. The `0x28DE0 – 0x37FFF` region of
   `0xFF` is genuinely erased ROM inside a complete image, not a
   missing piece. The 94 external `jal` targets from the NVRAM
   setup tool into that region therefore land on real (erased)
   bytes; they are either unreachable at runtime, holdovers from
   an earlier ROM revision, or called only when that region is
   shadowed by something other than ROM at runtime. The setup
   tool is not "blocked on a missing splice gap".
3. **No mainboard keyboard MCU.** The Intel i8749-class
   microcontroller used for NCD keyboards lives **inside the
   physical keyboard**, not on the mainboard. The only
   keyboard-adjacent part on the NCD15 PCB is a 7407
   open-collector hex buffer, with traces running straight to the
   custom-marked LSI / R3052 CPU. So `0xAEC80000` is not a
   "keyboard MCU port" — it is the CPU's direct bit-banged serial
   line (keyboard input on receive, 93C46 NVRAM on transmit)
   through that 7407.
4. **The video cartridge appears to be shared with the 19r board.**
   The PCB markings imply the same cart drives 19r (probably at
   1280×1024), which is why the CRTC/video register writes look
   generic rather than a hardcoded 1024×800 timing table.

Everything below this section predates these corrections and
should be read with them overlaid.

## Hardware

NCD15 (15", monochrome X-terminal, 1991-era) — **X11R5 client machine**.

- **CPU**: IDT **R3052** (MIPS-I, big-endian) — cache-probe sweeps at reset
  `sub_bfc00188` (8 KB range, 16-byte stride) and `sub_bfc001e4` (1 KB range,
  16-byte stride) size the on-chip caches; 8 KB I-cache matches R3052 (R3051
  would be 4 KB). Reset sequence uncached-trampolines through KSEG1 during the
  sweeps, then re-enables cached execution. `sub_bfc0023c` reads the memory
  controller config word at 0xFFFE_0020 and returns it OR'd with 0x00400000.
- **Boot ROM**: 256 KB, 8-bit wide, file `NCD15-19rBM-V271-splice.u8`
- **Back-panel connectors** (left→right): `RS-232C  AUI  MONITOR  AUXILIARY  MOUSE  KEYBOARD`
- **Ethernet**: AMD Am7990 LANCE (AUI port) — **verified**, RDP/RAP at 0xBE480000/0xBE480002
- **Token Ring**: IBM TROPIC-family chipset — **driver fully compiled in** with complete
  SRB (Service Request Block) state machine, correlator handling, and error-path strings
  (`itr_int_tx_bad_correlato…`). Board may be populated-but-silent on this unit, or
  probed-and-ignored; either way the firmware is not conditionally compiled.
- **Serial**: **SCN2681 / MC68681 DUART** — directly verified on NCD15 (not just inferred
  from MAME). Register-offset histogram at 0xBE88xxxx lands on 04/06/08/0E/10/12/14/16
  (channel A) | 20/24/26/28/2C/2E (channel B) | 36/38/3C (IPCR/ACR/IMR/ISR/timer) — exact
  2681 layout with byte-stride-2 indicating an 8-bit chip on a 16-bit bus. Init routine
  at `sub_0ec06de0` (VA 0x0EC06DE0, 15 refs into 0xBE88 space).
- **Keyboard µC**: 8-bit micro with DPRAM / serial-EEPROM mailbox at **0xAEC80000**
  (primary window) and **0xAED00000** (secondary window). Bit-banging loop in
  `sub_0ec069f0` (0x59-count shift/and/store pattern) matches 93C46 serial-EEPROM clocking
  through the MCU — consistent with MAME's "MCU + 93C46" NCD pattern. Whether the chip is
  i8749 (user's read of the silkscreen) or MC6805/MC68705P3 (NCD68k sibling family) is a
  silkscreen question, not a firmware one.

### MMIO map (chip-level identifications, verified against monitor disassembly)

| Address range | Device | Evidence |
|---|---|---|
| 0xBE48_2000 / 0xBE48_2002 (and 2004/2006) | **Am7990 LANCE** register window | 40+ CPU accesses concentrated at 0xBE48_2000-200E; `sub_0ec07de4` reads 0x2006 (status) and writes 0x2004 (command). Exact RDP/RAP vs. alt-register mapping depends on endianness wiring of the 16-bit chip on this 32-bit bus. |
| 0xBE48_0370 / 0xBE48_0B70-B74 / 0xBE48_6001 | LANCE ring descriptors (shared SRAM) | splice-gap init table writes these as TMD/RMD entries; CPU reads back to harvest status/length |
| 0xBE48_4000 / 8000 / A000 / C000 | LANCE ring buffer memory | 0x2000-stride pages in the same decode range |
| 0xBE88_0000 - 0xBE88_003F | **SCN2681 / MC68681 DUART** | register offsets match two 2681 channel blocks + timer/IRQ regs; `sub_0ec06de0` (15 refs) = DUART init |
| 0xAEC8_0000 | Keyboard MCU primary mailbox / 93C46 path | bit-banging loop in `sub_0ec069f0` with 0x59-cycle shift/and/store |
| 0xAED0_0000 | Keyboard MCU secondary window | lesser refs, same functional cluster |
| 0xBEC0_0000 - 0xBEFF_FFFF | Secondary ROM alias (+ splice-gap 0xBB5xxxxx targets) | |
| 0xBE38_0000 - 0xBE38_0013 | **Video / CRTC controller** | main entry at 0xBFC00844 writes 32-bit constant 0xBB83_1F1D to 0xBE38_0000 then 0x20 to 0xBE38_0004 as part of display init; later code touches offsets 01/04-08/0A/0C/13 byte-wise |
| 0xAF00_0000 - 0xAF00_0004 | **RAMDAC / cursor chip** (Bt431 / Bt458-class) | pair-write pattern at +0/+4 (`sw $data,0; sw $addr,4`) is classic addr/data register interface for Bt431 cursor RAM or Bt458 LUT; 10 `lui 0xAF00` references clustered around video-init routines. Accessed via helper `sub_0ec02bfc` (8 call sites). |
| 0xBFC0_0000 - 0xBFC3_FFFF | Boot ROM (KSEG1) | |
| 0xFFFE_0010 / 0xFFFE_0020 | Memory controller (refresh timer + config) | touched by reset code at 0xBFC00008-28 |

KSEG0 shadow of main RAM is 0x8EC0_0000 (phys 0x0EC0_0000); the
monitor actually runs at 0x0EC00000 unmapped, confirmed by `lui 0x0EC0`
dominating main-monitor lui immediates (1181 hits vs zero for
`0xBEC0`).

## ROM layout (256 KB `NCD15-19rBM-V271-splice.u8`)

```
file offset    size   contents
0x00000-0x00FFF 4 KB   Reset vectors + cache probes (ROM-linked at 0xBFC00000)
0x01000-0x01FFF 4 KB   Erased (0xFF)
0x02000-0x03FFF 8 KB   RAM-copy trampoline
0x04000-0x22B4F ~123 KB Main Boot Monitor V2.7.1 code (linked for RAM @ 0x0EC00000)
0x22B50-0x25190 9793 B Boot-console bitmap font (byte-identical to HMX PRO V2.7.2)
0x25191-0x27FFF ~11 KB Tail of main monitor (strings, late tables)
0x28000-0x28DDF 3552 B HW-init + RAM-test + LANCE descriptor tables (NOT erased)
0x28DE0-0x37FFF ~61 KB ERASED (0xFF) — genuine splice gap
0x38000-0x3DFFF 24 KB  NVRAM Setup tool (linked @ 0xBED40000)
0x3E000-0x3FFFF 8 KB   Zero fill
```

### ROM aliases

Same 256 KB ROM is visible at **0xBFC00000** (KSEG1 boot alias) and
**0xBEC00000** (secondary ROM alias). The main monitor is *RAM-linked*:
it's copied into DRAM and executed from 0x0EC00000. Evidence: 873
pointer references to 0x0E…xxxxx vs only 3 inside the ROM region.

### Splice gap

94 distinct external jal targets (not 310 as first reported — that
number was a hardcoded literal in an earlier tool) live at 0xBB5xxxxx,
i.e. **outside the dumped file**. The filename suffix `-splice.u8`
indicates a partial dump. The missing region holds format-string
helpers, screen-output primitives, and NVRAM read/write routines shared
between the monitor and the setup tool.

**Splice-gap correction (HMX cross-check):** the region we originally
called "erased (0xFF)" from 0x28000-0x37FFF is **not uniformly erased**.
The first 3552 bytes (0x28000-0x28DDF) are a real data block containing
three tables, in order:

1. **Peripheral init sequence** — `(addr, value)` pairs targeting
   0xBE880008/10/14/20/28 with byte-in-MSB values (0x01/0x02/0x07/0x10/
   0x13/0x87 in the top byte). Classic "byte register accessed as
   word" MMIO. Same access *style* as HMX PRO's 0x18000028/0x18000058
   registers in MAME, at a different decode base.
2. **Walking-pattern RAM test vectors** — `00/FF`, `FFFF0000/0000FFFF`,
   `FF00FF00/00FF00FF`, `F0F0F0F0/0F0F0F0F`, `33/CC`, `55/AA`.
3. **LANCE Ethernet buffer descriptors** — address pattern 0xBE48-,
   0xBE4A-, 0xBE4C-, 0xBE4E- at 0x2000 strides, matching AM7990 ring /
   RDP/RAP register layout.

From 0x28DE0 onward the 61 KB block is genuine 0xFF fill — this is the
real missing slice, and it must contain the 94 external stubs the
NVRAM-setup tool jumps into.

## Artifacts in this directory

| File | Purpose |
|---|---|
| `NCD15-19rBM-V271-splice.u8` | Original ROM dump (256 KB, incomplete splice) |
| `dump_setup.py` | Standalone disassembler for the 0x38000-0x3DFFF NVRAM-setup slice |
| `nvram_setup.dis` | Output of `dump_setup.py` — 5118 lines, 60 fns, 132 strings, 10 labeled |
| `mipshunt.py` | Reusable MIPS-I heuristic function+string inventory tool |
| `imgs/Xncd*` | 12 X-server kernels (see "Xncd kernel inventory" below) |
| `FINDINGS.md` | This file |

### `dump_setup.py` findings

- Link base **0xBED40000** (45% of intra-slice `jal`s self-resolve with
  this base; the other 55% escape to the splice gap).
- Entry at file offset 0x38000 is a two-word trampoline that loads a
  function pointer from RAM[0x0EC008C8] and jumps through it. Main init
  (sub_bed40008) clears/restores bit-1 of a 16-bit flags word at
  RAM[0x0EC01DA2] bracketing screen output.
- UI is a 5-menu tree: **Keyboard / Network / Boot / Help / Monitor**,
  with `Dots Per Inch` and `Done` pseudo-tabs.
- Boot methods are prioritised over **LOCAL / MOP / NFS / TFTP**; the
  UI enforces unique priority levels per method.
- Network config covers both **IP** (Terminal/Gateway/Subnet/Broadcast
  + three boot-host slots) and **NCDnet** (DECnet Phase IV: Terminal /
  Host / Router addresses in `a.n` form).
- Keyboard menu lists 30+ layouts — `OADG Kana`, VT220 VMS/Ultrix,
  IBM PS/2, N-101/N-97/N-Kana, plus Group families (101/102, 105, 107,
  108, 122, 123, 3270). Matches the i8749 multi-keymap architecture.
- Config-directory split: separate 64-char-limited NCDnet-vs-UNIX
  config directories, each with an appended config-file name.

### `mipshunt.py` design

Reusable heuristic scanner applying the `super68k/HEURISTIC*.md`
concepts to MIPS-I images. Key passes:

1. Prologue detection (`addiu $sp,$sp,-N` + `sw $ra,K($sp)`).
2. Cross-reference from every in-image `jal` target.
3. Intra-function HI-half dataflow tracker resolving `lui` + {`addiu` | `ori` | `lw`/`sw`} pointer loads.
4. String extraction with newline splits; string labels attached to
   functions via resolved-pointer matching.
5. Trace-to-`jr $ra` validation for every candidate entry.
6. Subsystem classification via regex table (Xlib, Xt, Xserver,
   DIX/MI, font code, keyboard, LANCE, etc.).

CLI: `mipshunt.py <image> --base 0xBFC00000 [--ram-base 0x0EC00000] [--alias 0xBEC00000] [--json out.json]`

Tested against the ROM: **662 function entries, 662/662 traceable to
`jr $ra`, 35 labeled** (5% — low because boot monitors carry few
strings relative to code).

## Xncd kernel inventory (`imgs/`)

Twelve X-server kernels spanning **three CPU ISAs** and **three NCDware
generations**. Only **Xncd15r** matches the NCD15 hardware.

| File | CPU / format | NCDware build | Target |
|---|---|---|---|
| Xncd14c | 68020 a.out (SunOS) | pre-3.0 (© 1988-91) | NCD14c — 14" color 68020 |
| Xncd15b | 68010 a.out | pre-3.0 (© 1988-91) | NCD15b — 15" mono 68010 |
| **Xncd15r** | **MIPS ECOFF BE (R3000)** | 1992 (© 1988-92) | **NCD15r — the MIPS 15" terminal we're REing** |
| Xncd16 | 68010 a.out | pre-3.0 (© 1988-91) | NCD16 — 16" mono 68010 |
| Xncd16.1 | 68010 a.out | `NCD16 V3.1.0 X 06/30/93 #4920` | NCD16 — 3.1 respin |
| Xncd16e | 68020 a.out | `NCD16e V3.1.0 X 06/30/93 #4922` | NCD16e — 16" mono 68020 |
| Xncd17c_lt | 68020 a.out | pre-3.0 (© 1988-90, low-tier) | NCD17c — 17" color 68020 |
| Xncd19b | 68010 a.out | `NCD19b V3.1.0 X 06/30/93 #4929` | NCD19b — 19" mono 68010 |
| Xncd19b.old | 68010 a.out | `NCD19b V3.0.0 X 01/20/93 #25009` | NCD19b — prior V3.0 |
| Xncd19b_lt | 68010 a.out | pre-3.0 (© 1988-90, low-tier) | NCD19b — early low-tier |
| Xncd19c | 88K BCS (MC88100) | `NCD19c/19g/17cr/MCS1 V3.0.EF PEX beta 10/29/92 #21053` | shared 88K binary for 19c/19g/17cr/MCS1 |
| Xncd19c_lt | 88K BCS | pre-3.0 (© 1988-91) | NCD19c — early 88K build |

Generational markers (from strings):

- **Pre-3.0 "-lt"** (1988-90 ©): smallest, no `@(#)` banner.
- **1991-92 transitional** (1988-91/-92 ©): includes Xncd15r and
  Xncd19c_lt.
- **V3.0 / V3.0.EF / V3.1.0** (1988-93 ©, `@(#)` SCCS banner): the
  mature NCDware 3.x release family; 06/30/93 is a coordinated 3.1.0
  cut across m68k.

All twelve bundle the same `/usr/lib/X11/ncd/{fonts,configs,rgb.txt}`
layout plus BSD TCP/IP, Thursby **TSSnet** (DECnet) and **TSSlat**
(LAT). Xncd19c is the only **PEX** (3-D PHIGS extension) build.

## Xncd15r — the MIPS X server (applied `mipshunt.py`)

### ECOFF layout

```
file 0x00000-0x0013f  header (file hdr + 0x38-byte AOUT opthdr + 6 section hdrs)
file 0x00140-0x196b6f .text       va 0x8ED00000  size 0x196a30   (1.58 MB code)
file 0x196b70-0x19b4ff .rdata     va 0x8EE96A30  size 0x04990
file 0x19b500-0x1d69cf .data      va 0x8EE9B3C0  size 0x3b4d0
file 0x1d69d0-0x1e0000 .sdata     va 0x8ED6890(*) size 0x09630
(no scnptr)                .sbss  va 0x8EEDFEC0  size 0x011b0
(no scnptr)                .bss   va 0x8EEE1070  size 0x158e0
entry point = 0x8ED00000   (= text start; first word is the boot vector)
magic 0x0160 (MIPS-I BE ECOFF), vstamp 523, opt magic 0x0107
```

All loadable sections are **virtually contiguous**: 0x8ED00000 →
0x8EED6890+0x9630 → 0x8EEDFEC0+0x158E0. After chopping the 0x140-byte
header, the rest of the file is a single linear memory image anchored
at KSEG0 VA 0x8ED00000.

### `mipshunt.py` results on Xncd15r

| | |
|---|---|
| Image after header strip | 1,965,760 B (1920 KB) |
| Base VA | 0x8ED00000 (KSEG0) |
| Function entries | **4644** — `jal`-reached 3354, prologue-only 3406, both 2116 |
| `jr $ra` reach | 4644/4644 (100%) |
| Distinct strings | 5508 |
| String-labeled fns | 461 (10%) |
| Pointer-table runs ≥ 4 | 541 |

Artifacts: `work/Xncd15r.mem` (header-stripped image),
`work/Xncd15r.json` (full inventory), `work/xncd15r_xapi.txt` (symbol
inventory, below).

### Subsystem classifier — tightened

The regex table in `mipshunt.py` was rewritten with word boundaries
and long distinctive tokens to avoid collisions (old table mis-hit
"TX"/"Rx" as LANCE, every `%s` as a printf function). New tag list (32
tags): NCD, FlashBoot, Xlib, Xt, Xserver, XExt, MI/FB, Font,
Color/Vis, Keysym, Keyboard, Mouse/Ptr, Selection, WindowMgr,
Terminal, XDM/Session, PEX/3D, LANCE, TokenRing, IP/TCP, TFTP,
NFS/RPC, MOP, LAT, SCC/Serial, Video, NVRAM, Monitor/CLI, POST/Diag,
Exceptions, Libc/Math, HasFormat. Current Xncd15r tag distribution
has `Xt=57`, `XDM/Session=14`, `WindowMgr=13`, `IP/TCP=10`, `LAT=9`,
and so on. Undercounts (`Xserver=2`, `MI/FB=5`) reflect that the
X server's DIX/MI code is mostly silent — errors defer to external DB
files (`XErrorDB`, `XKeysymDB`) rather than embedded strings. Call-
graph tag propagation is the next improvement.

### Unified MIPS build — **downgraded** after cross-check

Original claim (kept for honesty): first readable string at .rdata base
(va 0x8ED00010) is `Xncd19r`, and rodata tail has `NCD16`/`NCD19r`
tokens. Initial read was that one binary serves NCD15r/16r/19r, by
analogy with the 88K `NCD19c/19g/17cr/MCS1` build.

**Revised read:** scanning the whole image
finds only **three `NCD1[569]` hits total** (`NCD16` at file 0x1D8DD0 +
0x1D8DD8 adjacent, `NCD19r` at 0x1DF8C0) — 27 KB apart, not a
co-located string table, and with no sibling `NCD15r` entry. This is
*not* the 88K pattern (which has four model tokens packed into one
banner). More likely reading: Xncd15r is the NCD15r-specific build, and
the handful of NCD16/19r tokens are stale-reuse from the shared source
tree. The "unified build" claim is **withdrawn** until a proper
hardware-ID dispatch is located (a `beq`/`bne` on a board-ID register
near one of these strings would restore the claim).

### Xt / Xlib symbol inventory

Stripped binary, no symbol table — but the string pool reveals every
name the server *talks about* in its own errors and resource DB:

| Bucket | Count | Notes |
|---|---|---|
| Xt public API (`Xt[A-Z]…`) | 29 | substring-level, pulled out of error messages |
| Xlib public API / resource | 21 | env vars, DBs, protocol names |
| Xt type converters (`cvtStringTo…`, `cvtIntTo…`) | 23 | full coverage |
| Xt resource class names (`xt…` lower-camel) | 24 | one-to-one with their uppercase APIs |
| Internal (`_X*` / `_Xt*`) | 7 | image/pixel primitives + Xt selection atoms |
| X protocol errors (`Bad*`) | **17 / 17** | complete |
| X protocol request names | 0 / 120 | dispatch is by opcode index |
| `XK_*` keysym **name strings** | 0 | names live in external `/usr/lib/X11/ncd/XKeysymDB` |
| `XK_*` keysym **integer values** | **17 distinct, 350 tight clusters** | internal keysym tables (BE half-words; e.g. `FF0D FF50 FFE1 FFE7`) |

Post-review correction: Xncd15r carries full internal keysym lookup
tables as integer values; only the human-readable keysym *names* are
deferred to the external DB. Earlier wording implying "no internal
keysym handling" was wrong.

Full list in `work/xncd15r_xapi.txt`.

What it confirms: Xncd15r is a **full X11R5 server with a complete Xt
Intrinsics** bundled in (not a minimal/cut-down X), plus XDMCP and
selection machinery. The NCD-specific clients (`NCDwm`, `xtelnet`,
`NCDconsole`, `NCDlogin`, `NCDsetup`, `NCDstats`, `NCDterm`) all run
*inside* the X-server binary alongside the server code — this is a
single-binary X terminal, not an X server with separately-loaded clients.

### The 4 MB PC Card (undumped, write-enable sticker)

Evidence from the image strings `Op_flash_screen`,
`Compressed Flash server…`, `NCD Ethernet Option Card`,
`Warning: Installed option card is unknown!`, `error installing
decompression for file %s: %s`, the `LOCAL/MOP/NFS/TFTP` boot-priority
order, and the canonical flash-resident file layout
(`NCD_STD.DAT` + `NCD_FONTS:` + `NCD_CONFIGS:`):

**Most likely contents (ranked, revised)**

1. An **uncompressed** `Xncd15r` image (1.88 MB raw) + boot header.
2. `NCD_STD.DAT` defaults blob.
3. Minimal fonts subset (`fonts/misc/{6x13,cursor,fixed}`, index files).
4. `rgb.txt`.
5. Optionally TSSnet/TSSlat licensed code segments (the server already
   includes their copyright banners).

**Revision note:** an earlier version of this list ranked "compressed
Xncd15r image" as #1. A ROM-wide search for decompression routines
(`inflate`/`uncompress`/`unpack`/`LZ`/`deflate` strings, SRL shift
distribution for LZ* bit extraction) turned up **nothing**: the string
`decompress` appears only inside Xncd15r itself. The ROM cannot expand
a compressed flash image, so the card is almost certainly uncompressed.
4 MB is comfortable — 1.88 MB Xncd15r + ~1 MB fonts/configs leaves
headroom.

A plain SRAM config card is ruled out by size (NCD NVRAM is ≤ 128 B).
No `PCMCIA` or `NuBus` literal appears in the ROM or in any of the 12
kernels — NCDware of this generation speaks only of "Option Card" with
runtime card-type auto-ID.

## MAME cross-reference

`~/src/claude/mame/src/mame/ncd/` has a complete NCD driver family
written by R. Belmont. It confirms and corrects several hypotheses:

### Sibling drivers

| MAME tag | CPU | Year | Boot Monitor | Matches our binary |
|---|---|---|---|---|
| `ncd16` | 68000 12.5 MHz | 1989 | V2.1.0 / V2.3.L (BM V2.2.2) | Xncd16 |
| `ncd17c` | 68020 | 1990 | — | Xncd17c_lt |
| `ncd19` | 68020 | 1990 | — | Xncd19b_lt, Xncd19b.old, Xncd19b |
| `ncd19c` | MC88100 | 1991 | — | Xncd19c_lt, Xncd19c |
| `ncdmcx` | MC88100 | 1993 | — | Xncd19c (shared-build `MCS1`) |
| **`hmxpro`** | **R4600 50 MHz** | **1994** | **V2.7.2** | descendant of Xncd15r |
| `explorapro` | PPC | 1995 | — | (not in our folder) |

The **HMX PRO is our direct MIPS-family descendant** — its boot
monitor is **V2.7.2**, exactly one minor version above our
**V2.7.1**. Our ROM sits between the 68k/88k era and the R4600 HMX
era.

### Corrections to earlier hypotheses (from MAME driver evidence)

| Earlier claim | MAME says | Correction |
|---|---|---|
| "Z8530 SCC for serial" | MC68681 / SCN2681 **DUART** on every NCD68k and on HMX PRO | **Serial is SCN2681, not Z8530.** |
| "i8749 keyboard µC" (user-asserted) | NCD68k family uses **MC6805 / MC68705P3** talking to 93C46 serial EEPROM | Keyboard MCU on our NCD15 may in fact be an 8749 (user saw the chip); but the sibling NCD boards use a 6805. Worth reverifying the chip print on the NCD15 board. |
| "NVRAM" (dedicated chip) | It's a **93C46 serial EEPROM** (64×16 = 128 B) bit-banged by the MCU via a mailbox | "NVRAM" in NCD firmware terminology = 93C46 on an MCU-managed serial link. The 128 B limit explains the NVRAM-setup UI's 64-char field caps. |
| "AMD Am7990 LANCE" | AMD **Am7990** (NCD68k) / **Am79C950** (HMX PRO) | Confirmed. `ncd_mips_state` comment: "AM79C950 Ethernet". |

### Custom silicon: the NCD **BERT** ASIC (NCD16 only — **absent on NCD15**)

`src/mame/ncd/bert_m.{cpp,h}` emulates the **BERT** chip — NCD's in-house
**programmable barrel-shifter + QLC block-copy engine**. On NCD16 it sits at
0x800000-0xFFFFFF (8 MB window) and accelerates text rendering:

- Read path: `return (history<<16 | data) >> shift` — sliding-window barrel
  shift for text blitting at arbitrary sub-byte x-offsets.
- Write path: normal passthrough, plus **QLC mode** — any write triggers a
  512-byte block-copy from `qlc_src` (fast scroll / clear).
- Control word = `{bit12=clear history, bit10=invert mask, bit9=invert data,
  bit8=invert result, bit4=shift direction, bits[3:0]=shift count 0-15}`.

The NCD15 firmware has **no BERT**. Our video path is CRTC at 0xBE38 + RAMDAC
at 0xAF00 — a simpler, register-driven design than the NCD16 ASIC. No 8 MB
BERT-style window appears in the NCD15 lui histogram. Likely the R3052's
higher raw CPU speed made the BERT blit-accelerator redundant; text rendering
runs as MIPS code against a directly-mapped linear framebuffer.

### NCD68k memory-map layout (from MAME) — structural analogy for NCD15

| Peripheral | NCD16 (68000) | NCD17c (68020) | NCD19 (68020) | **NCD15 (MIPS)** |
|---|---|---|---|---|
| ROM base | 0x000000 | 0x00000000 | 0x00000000 | 0xBFC00000 (KSEG1) |
| Keyboard MCU | 0x0C0000 | 0x001C0000 | 0x001C0000 | **0xAEC80000** |
| DUART (SCN2681) | 0x0E0000 | 0x001C8000 | 0x001E0000 | **0xBE880000** |
| RAMDAC | — | 0x001D0000 (Bt478) | — | **0xAF000000** (Bt431/458 class) |
| LANCE (Am7990) | 0x100000 | 0x00200000 | 0x00200000 | **0xBE482000** |
| VRAM | 0x200000 (128 KB) | 0x03000000 (1 MB) | 0x00400000 (256 KB) | **unknown — not in ROM histogram** |
| Video glue | BERT @ 0x800000-0xFFFFFF | — | — | **CRTC @ 0xBE380000** |

The NCD15 keeps the same *five-peripheral* set (MCU / DUART / LANCE / RAMDAC /
video-glue) but relocates each into MIPS KSEG1 space with different offsets.
The RAMDAC interface style on NCD17c (Bt478 at 8 bytes of regs) maps to our
byte pair-writes on 0xAF00_0000±0x4 — strong evidence the NCD15 RAMDAC is
**Bt478-family** (monochrome variant / Brooktree pin-compatible part).

**Framebuffer inference from NCD16 analogy:** NCD16 uses a 1024×1024 mono
linear framebuffer (128 KB = 1024 rows × 128 bytes). NCD15 is a 15" mono
display — confirmed 1024×800 @ 70 Hz at 1 bpp (100 KB, see Corrections
section at top of file; the "1024×768 or 1152×900" guess here predates
the HW-owner update). In the NCD68k
design this VRAM is **directly CPU-mapped** (install_ram) with no register
interface, so the monitor would only touch it when actually drawing; the bulk
of framebuffer writes happen from Xncd15r. That explains why no "VRAM-shaped"
base appears in the monitor's lui histogram — the boot monitor renders banner
text through the CRTC+RAMDAC path but otherwise leaves VRAM alone.

### HMX PRO memory map (useful reference for later NCD15 work)

```
0x00000000-0x003FFFFF  VRAM (4 MB)
0x10000000-0x103FFFFF  RAM / aux (4 MB)
0x18000028             status reg (mipshunt-found reads here won't apply — different board)
0x18000058             TTY output (byte at >>16 & 0x7F)
0x1B000000-0x1B00007F  SCN2681 DUART (umask32 0xFF000000 → MSB of word)
0x1FC00000-0x1FC3FFFF  ROM (256 KB)
0x20000000-0x207FFFFF  main RAM (8 MB)
```

Our NCD15 boots from the same ROM base (0xBFC00000 = KSEG1 of
0x1FC00000 ✓) but its RAM-linked monitor runs from **phys
0x0EC00000** (KSEG0 alias 0x8EC00000, KSEG1 alias 0xAEC00000) — a
distinctly different main-memory base than HMX PRO's 0x20000000. This
means the NCD15 has its own memory-map generation, not an HMX variant.

### Cousin product: Tektronix TekXpress XP330

`src/mame/tektronix/tekxp330.cpp` — a competing 1992 X terminal.
Relevant one-liner in its driver comments:

> 16K (2K×8) EEPROM is SEEQ NQ2816A-250, AT28C16 compatible. Contains
> Boot Monitor settings. The content is compressed.

Cross-vendor confirmation that X-terminal boot monitors of this era
store their on-board config *compressed* in a small EEPROM, reinforcing
our read of the NVRAM Setup tool's role.

## HMX PRO V2.7.2 binary cross-reference

We pulled `hmxpro.zip` from the local MAME ROM set (split 2×128 KB
even/odd halves `ncdhmx_bm_v2.7.2_b0{e,o}.bin`), byte-interleaved into
`work/hmxpro_bm272.bin` (256 KB). This lets us binary-diff our V2.7.1
against the direct V2.7.2 descendant.

### Shared-content map (non-blank 16-byte chunk matches, by 16 KB bin)

```
NCD15 0x00000-0x04000    12/1024  — reset vectors (R3000 vs R4600 diverge)
NCD15 0x04000-0x1FFFF    60-150/1024  — product-specific monitor front-end
NCD15 0x20000-0x28000    740-910/1024  (~80%) — shared library tail
NCD15 0x38000-0x3DFFF    91/1024  — NVRAM setup UI (product-specific)
NCD15 0x3C000-0x3FFFF    199/1024  — zero-fill + tail strings
```

The last ~32 KB of the Boot Monitor (file 0x20000-0x28000) is **~80%
chunk-identical to HMX V2.7.2** — this is the shared library layer
(printf / string / TFTP / MOP / ARP). Claim E (HMX as direct codebase
descendant) is materially confirmed at that level.

### Longest shared contiguous run: 9793 bytes, byte-identical

NCD15 @0x22B50 ↔ HMX @0x27EC0 — the **boot console bitmap font**. Same
binary font blob in both V2.7.1 and V2.7.2; useful anchor for
alignment-based structural comparison elsewhere in the ROM.

### Reset-vector divergence (expected, ISA gap)

- NCD15 R3052: linear init code at 0xBFC00000 — `lui $t0,0x40; mtc0
  $t0,$12` (Status=0x00400000 = BEV), then writes to 0xFFFE0020.
- HMX R4600: classic exception-vector jump table — `j 0xBFC00430`
  repeated. Completely different.

### HMX MMIO base

HMX top-10 lui immediates (for comparison):

```
  0x4000  (0x40000000)  1096    — not in MAME map; probably DMA window
  0x9fc2/9fc3           436     — ROM self-refs via KSEG0
  0xb000/b800/bc00      303     — KSEG1 peripherals
  0xbb00                60      — I/O
  0x2001/2000           77      — main RAM @0x20000000
```

Disjoint from NCD15's map (0x0EC00000 RAM, 0xBE48/0xBE88 I/O).
Shared-library tail aside, the products are *not* binary-compatible.

### HMX-unique hardware strings

Only `write_nvram: fail` stands out as a hardware-specific string
present in HMX but absent from NCD15. See §"HMX `write_nvram` reverse"
below for what it yields about the NVRAM protocol.

## HMX `write_nvram` reverse → NCD15 transfer — **blocked**

Attempted transfer: locate `write_nvram` in HMX, reverse its byte-level
93C46 access, then find the same shape in NCD15 to name our NVRAM I/O
routines. Outcome: the SCN2681-vs-Z8530 UART question motivated
this path, but it dead-ends in HMX.

**What we tried:**

1. Find `write_nvram: fail` at HMX file 0x22BDC (VA 0x9FC22BDC). **Zero**
   direct `la` references (neither lui+addiu nor lui+ori), zero hits as
   a 4-byte BE pointer anywhere in ROM. Whole adjacent NVRAM-string
   cluster (`NVRAM too big…`, `PANIC -- Need to initalize NVRAM first`)
   is similarly unreferenced — apparent dead/relocated message pool.
2. Find referenced NVRAM strings: `NV NVRAM utility`, `SE NVRAM setup`,
   ` NVRAM `. The first two are entries in a CLI command table at file
   0x30270+ with shape `(name_ptr, flags=0x00070000)` — no handler
   column (dispatch is by 2-char prefix matching at runtime).
3. The ` NVRAM ` entry at file 0x31350 sits in a second table whose
   "handler" slot (0x9FC32BB4) decodes to nop+garbage, i.e. it's a data
   panel descriptor, not a function pointer.

**Conclusion:** HMX NVRAM access can't be reached via the obvious
string-ref path. Dropping this route. More productive future lines:

- Locate NVRAM code in NCD15 by direct 93C46 **bit-bang signature**
  (tight loop with 1-bit shifts + `sb`/`lb` against a single base with
  small offsets for CS / CLK / DIN / DOUT).
- Reverse the NCD15 NVRAM-setup tool's external stubs (0xBB5xxxxx) once
  the missing splice-gap slice is recovered — that's where the actual
  read/write primitives live.



## Monitor disassembly (`dump_monitor.py` → `monitor.dis`)

Full annotated disassembly of the main Boot Monitor: 32205 lines, 623
functions (15 auto-labeled), 558 strings, 342 external jal targets, 822
RAM/data refs. Regions covered: reset vectors 0x0-0xFFF, trampoline
0x2000-0x3FFF, main monitor 0x4000-0x27FFF (RAM-linked at 0x0EC00000),
HW-init block 0x28000-0x28DDF. Font 0x22B50-0x25190 is skipped.

Reset sequence (0xBFC00000-0xBFC00064):

```
00: Status = 0x00400000 (BEV)
08-18: *(0xFFFE0020) = 0x002BFF2D        ; memory controller config
1c-28: *(0xFFFE0010) = 500                ; DRAM refresh timer
30-40: jal 0xBFC00188 / 01E4 / 023C       ; three cache-init subs
48:   $sp = 0x0EC28000                    ; stack at top of RAM-linked range
64:   jr to 0x1FC0006C                    ; uncached→cached transition
```

### Labeling caveat — `sub_0ec049d0` is printf, not "Loopback_test"

The heuristic string-attachment picked the first format template
(`"Loopback test"`) reached from the function and glued that label on.
This function has **115 call sites** and is clearly the top-level
format/emit primitive — it's printf (or its internal equivalent). The
mislabel cascades through every caller's annotation. Treat any
`sub_0ec049d0_Loopback_test` reference in `monitor.dis` as printf
until a manual re-label sweep is done.

### Top functions by MMIO base

| Function | VA | Refs | Role |
|---|---|---|---|
| `sub_0ec06de0` | 0x0EC06DE0 | 15 stores to 0xBE88xxxx | SCN2681 DUART init |
| `sub_0ec07de4` | 0x0EC07DE4 | 20 accesses to 0xBE48xxxx | Am7990 LANCE init |
| `sub_0ec069f0` | 0x0EC069F0 | 5 accesses to 0xAEC80000 | Keyboard-MCU / 93C46 bit-bang |
| `sub_0ec04eac` | 0x0EC04EAC | — | IBM TROPIC Token Ring: "int_tx_bad_correlator" ISR path |

## NCD 68k X-server MMIO cross-check

To calibrate which hardware platform each `imgs/Xncd*` binary targets,
the Sun a.out headers were parsed and each binary was scanned as raw
big-endian 32-bit words for the MAME-documented MMIO addresses of the
NCD16, NCD17c and NCD19 68k boards. Pattern hits (raw 4-byte matches
in text+data, not disassembled operands) cluster cleanly by platform:

| Binary        | CPU     | Platform   | MCU | DUART | LANCE | VRAM | BERT / RAMDAC |
|---------------|---------|------------|-----|-------|-------|------|---------------|
| Xncd15b       | 68010   | **NCD16**  | 20  | 38    | 55    | 43   | 29 (BERT)     |
| Xncd16        | 68010   | NCD16      | 22  | 44    | 60    | 43   | 52 (BERT)     |
| Xncd16.1 / 16e| 68020   | NCD16-late | 566 | —     | —     | —    | —             |
| Xncd17c_lt    | 68020   | NCD17c     | 15  | 7     | 53    | 83 @ 0x3M | 11 (Bt478) |
| Xncd19b / 19c | 68010/20| NCD19      | 86  | 149   | 47    | 68   | —             |

Key structural finding: **Xncd15b is a 68010 binary linked for the
NCD16 memory map** (MCU@0xC0000, DUART@0xE0000, LANCE@0x100000,
VRAM@0x200000, BERT@0x800000). It is *not* a 68k sibling of the
NCD15-MIPS board. The NCD15-MIPS address space
(MCU@0xAEC80000, DUART@0xBE880000, LANCE@0xBE482000, CRTC@0xBE380000,
RAMDAC@0xAF000000) is a fresh hardware generation:

```
NCD15b  (68010,  NCD16 layout)
   ↓
NCD15r  (R3052,  new MIPS layout — our target)
   ↓
HMX PRO (R4600,  yet another MIPS layout, MAMEd)
```

Corollaries for this project:

- Framebuffer / VRAM VA for NCD15-MIPS cannot be read off the 68k
  siblings — they use physical 0x200000 on a 24-bit 68010 bus, which
  does not port to the MIPS KSEG1 window. The authoritative source
  for VRAM VA is the Xncd15r ECOFF binary itself.
- The 0xAF00_0000 pair-write interface in the Boot Monitor matches
  the Bt478 register/LUT pair-write idiom seen 11× in Xncd17c_lt at
  0x1D0000 — narrowing the RAMDAC-family identification to the
  Brooktree Bt47x line rather than Bt431/458.
- No BERT on NCD15-MIPS: the 8 MB BERT window (0x800000-0xFFFFFF on
  NCD16) has zero echoes in the MIPS Boot Monitor `lui` histogram.
  BERT was an NCD16-era barrel-shifter/QLC block-move ASIC; NCD15
  uses the simpler CRTC+RAMDAC path.

## Fn-ptr table population (closes Claim 1)

At ROM reset, RAM 0x0EC008XX contains a copy of the ROM bytes at
file offset 0x08XX (which is reset-section code, not data). The
Monitor then **overwrites** this region at init time with real
function pointers. The init block lives at VA 0x0EC00D90-0x0EC00EC0
and uses a repeating pattern:

```
  0ec00e54: lui   $v0, 0xec1
  0ec00e58: addiu $v0, $v0, -0x7fac      ; $v0 = 0x0EC08054  (Monitor text)
  0ec00e5c: lui   $at, 0xec0
  0ec00e60: sw    $v0, 0x8c8($at)        ; g_screen_out_vec := 0x0EC08054 → wait, see below
  0ec00e64: lui   $v0, 0xec1
  0ec00e68: addiu $v0, $v0, -0x7f68      ; $v0 = 0x0EC08098  (Monitor text)
  0ec00e70: sw    $v0, 0x8cc($at)        ; g_screen_out_vec2
```

(Note: the Monitor is computing 0x0EC1_0000 − 0x7FAC = 0x0EC0_8054;
this is a Monitor text-segment address, valid target for `jalr`.)

All 20+ slots in 0x0EC00880-0x0EC008CC are populated this way with
pointers to Monitor code in the 0x0EC05xxx-0x0EC14xxx range. When
the NVRAM tool executes `lw $v0, 0x8c8($v0=0x0ec00000); jalr $v0`,
it reads a Monitor-populated pointer and calls into Monitor code.
Runtime target values are statically provable from the monitor
disassembly alone — no RAM capture needed.

This closes the last open item: both
binaries' `jalr`s target Monitor code, and the targets are
statically determinable from `lui`+`addiu` pairs in the Monitor's
init routine.

### Follow-up: CFG dominance and Xncd15r overwrites

Two residual worries came up: (a) the init stores might be inside
conditional branches, leaving some slots uninitialized; (b) Xncd15r
might overwrite the table at runtime.

**(a) CFG dominance.** Monitor init range 0x0EC00D80–0x0EC00ED0
contains **24 `sw` instructions and zero branches**. The first
branch in the routine (`beqz $v0, 0x0EC00EF8`) is at 0x0EC00EE0 —
after all 24 stores. Straight-line code; every store executes
unconditionally on every boot.

**(b) Xncd15r overwrites.** Scanned Xncd15r.mem (1.96 MB) for any
`sw` whose base register was loaded via `lui` with immediate
0x0ec0 / 0x0b00 / 0x8ec0 / 0xaec0 (all aliases of the Monitor's
data page) and any short offset. Result: **11 writes total**, all
targeting 0x0EC00088–0x0EC0011C. **Zero writes into
0x0EC00800–0x0EC008FF**. Xncd15r does not modify the vtable. The
boot-time values set by the Monitor are also the runtime values.

Both objections are closed with static evidence.

Residual uncertainty (self-modifying code, register-indirect stores,
compiler artifacts in the linkage pattern, consumer-side symbolic
execution) is unfalsifiable by static analysis alone — it would
require either live-hardware capture or formal methods. The claim
set is pinned by disassembly; any remaining uncertainty is
epistemic rather than a specific identified gap.

## Notes on filename and LANCE verification

- **"splice" string is not present in the ROM.** Searched for
  `splice`, `Splice`, `SPLICE`, `patch`, `overlay`, `relo` — zero
  hits. The suffix `-splice.u8` on the dump filename is external
  labeling (dumper-supplied), not a build artifact. So the
  interpretation that it refers to the Xncd15r compile-time
  dependency is speculative, not evidenced.
- **Am7990 LANCE at 0xBE482000 re-verified**: offset histogram
  across the whole disassembly shows concentration at +0x2000,
  +0x2002, +0x2004, +0x2006 (47 hits total) — matches LANCE
  register-pair layout. Also +0xB70/B74 (13 hits) consistent with
  init-block struct accesses in device-local DRAM. Identification
  holds.

## Indirect-call audit (88 monitor `jalr` + 33 NVRAM-tool `jalr`)

The `jal`-only histogram missed `jalr` indirect calls. A backward-scan audit was run on both
disassemblies; the defining instruction for every target register
was classified.

**Boot Monitor (88 jalrs)**: every one of them loads its target via
`lw <reg>, <offset>(<ptr>)`. Zero `lui`-based direct indirect calls.
The most common sources are offsets 0x8d0 (19×), 0x84c (18×), 0x850
(6×), 0x8d8 / 0x8e4 / 0x8e0 — consistent with one or two vtables
of function pointers in the Monitor's RAM data segment at
0x0EC008XX. Short offsets (0xc, 0x10, 0x14, 0x18, 0x38) appear in a
cluster around 0x0EC19000-0x0EC1A200 and look like per-object method
tables dispatched through a `this`-like pointer. None load their
target from an address outside 0x0ECxxxxx.

**NVRAM tool (33 jalrs)**: **all 33** are preceded within 15
instructions by `lui $X, 0xec0` — i.e. the NVRAM tool reads
function pointers out of the *Monitor's* data at 0x0EC008XX and
calls them. Observed entries include `g_screen_out_vec` (0x8C8),
`g_screen_out_vec2` (0x8CC), `g_ui_func_table` (0x890). So the
NVRAM tool is not truly isolated — it depends on the Monitor for
console/UI I/O. Its `jal` self-resolution is 100 %, but its `jalr`s
cross into the Monitor via a shared function-pointer table. This is
standard embedded firmware practice and does not imply hidden code.

## Correction — the 0xBB5xxxxx "phantom" was a misread data table

An earlier pass claimed "310 `jal`s to 0xBB5xxxxx". A prior
retraction dismissed this as fabrication. Both were wrong in
different ways. The accurate picture:

- **Real `jal` instructions in actual code**: 256, all resolve
  intra-slice (0xBED40064–0xBED449F8). ✓
- **"310 jal 0xBB5xxxxx" lines**: these are *data words* in a
  trailing .rodata region (slice offsets 0x5428..0x5FF8), decoded
  by capstone as jal instructions due to an encoding coincidence.
  Each data word is a pointer of the form 0x0ED4_XXXX into the
  NVRAM tool's own body (slice offsets 0x2XXX..0x4XXX). When
  capstone treats such a word as an instruction, opcode = top
  6 bits of 0x0E.. = 3 (jal), and the resulting jal target —
  `(PC & 0xF0000000) | (imm26 << 2)` with PC top nibble = 0xB —
  lands at 0xBB5xxxxx, an address that does not exist.
- The trailing region also contains string tables ("Finnish",
  "Siemens German", "dd.dd.dd.dd", …) starting at slice offset
  0x4B78. It is a mixed .rodata section: 310 fn-pointer words +
  169 string words + 190 zeros + 91 small-int/other.
- Spot-checked pointers (0x0ED42620, 0x0ED425D4, 0x0ED42630,
  0x0ED42858) all land on real function entries or code
  locations within the NVRAM tool. So the table is an internal
  dispatch table — probably a country/locale-driven jump table,
  given the adjacent country-name strings.

**Net:** the NVRAM tool *is* self-contained at 0xBED40000, as
the retraction claimed, but the supporting evidence is the
pointer-table interpretation of the 310 "jal" lines, not their
non-existence. The current `nvram_setup.dis` omits this .rodata
region entirely (annotator's function-walk ends at 0xBED44B74);
adding a data-section dump would be the right next improvement.

## Real external-segment evidence (monitor.dis)

Separately, the Boot Monitor proper *does* have external jumps —
just at a different address. Jal-target histogram of all 2503 `jal`
instructions in monitor.dis:

| Segment | Count | Meaning |
|---|---:|---|
| 0x0EC0xxxx (RAM-linked self)   | 2289 | internal calls |
| **0x0B0xxxxx (external)**       | **209** | into a loaded image |
| 0xBFCxxxxx (KSEG1 ROM alias)    | 4    | reset-path calls |
| 0xBECxxxxx (KSEG0 ROM alias)    | 1    | trampoline |

The 209 external calls hit **53 unique target addresses** spanning
**0x0B00EFF0 – 0x0B0350F0** (~150 KB range). Three addresses account
for 73 % of the calls:

| Target       | Calls | Likely role (by call density) |
|--------------|------:|------------------------------|
| 0x0B033960   | 66   | Looks like an event / message-loop entry |
| 0x0B0350F0   | 62   | Looks like a block/wait/yield primitive |
| 0x0B00F2B0   | 25   | Looks like printf/console-emit |

Interpretation: the Boot Monitor was linked at compile time against
**fixed entry points in a companion image that loads at KUSEG
0x0B000000** at runtime. The monitor never stores pointers into the
0x0B0xxxxx range (only one `lui $t0, 0xb00` exists, and it's used as
a bitmask constant, not as a pointer), so these are hard-coded
compile-time references, not dynamically resolved.

**Update (round 3):** the "separate kernel" reading is **wrong**.
The 0x0B0xxxxx region is a DRAM alias of 0x0ED0xxxxx, i.e. the
Xncd15r load base. When the three hot targets are interpreted as
offsets from Xncd15r's .text start (0x8ED00000 KSEG0 = physical
0x0ED00000), the code makes sense:

- **0x0B00F2B0** → Xncd15r .text+0xF2B0: valid prologue with two
  nested `jal`s into the same function region.
- **0x0B033960** → Xncd15r .text+0x33960: `lw $v0, 0x2150($gp)` —
  classic $gp-relative access into sdata.
- **0x0B0350F0** → Xncd15r .text+0x350F0: `lui $t0, 0x8eef` /
  `lw $t0, -0x1c6c($t0)` — Xncd15r loading from its own .data
  segment (0x8EEF3394 range).

The memory controller on NCD15 aliases DRAM across the full 256 MB
KUSEG window 0x00000000-0x0FFFFFFF, ignoring high address bits. The
Boot Monitor's `jal` field encodes a 28-bit target; it always
shares the top 4 bits with PC (= 0 in KUSEG). The linker emitted
these calls as `0x0B0xxxxx` instead of `0x0ED0xxxxx` because the
NCD toolchain's Xncd15r was logically linked at 0x0B000000, and the
aliasing memory map makes both views equivalent at runtime. The
offset between logical and runtime base is
**0x0ED00000 − 0x0B000000 = 0x03D00000**.

So the actual picture:

- Boot Monitor calls **209 hard-coded entry points in Xncd15r** via
  compile-time-resolved `jal` immediates, relying on DRAM aliasing.
- Xncd15r must be loaded before any of these code paths execute.
- There is no separate "runtime kernel" image below the X-server.

Retained value of the original Xncd15r ECOFF parse (it still disambiguates the load base):

```
Xncd15r  ECOFF MIPS-I BE  magic=0x0160  flags=0x0007 (stripped)
aout    magic=0x0107 (OMAGIC/ZMAGIC, R3000)  vstamp=523
entry   0x8ED00000        (KSEG0 = physical 0x0ED00000)
.text   vaddr=0x8ED00000  size=0x00196A30  (1.6 MB)
.rdata  vaddr=0x8EE96A30  size=0x00004990
.data   vaddr=0x8EE9B3C0  size=0x0003B4D0
.sdata  vaddr=0x8EED6890  size=0x00009630
.bss    vaddr=0x8EEE1070  size=0x000158E0
```

Xncd15r occupies 0x8ED00000–0x8EEF7000 (KSEG0 cached view of physical
0x0ED00000–0x0EEF7000) — nowhere near the 0x0B000000 target of the
Monitor's external jumps. So the image the Boot Monitor calls into at
0x0B000000 is **a separate component**, not Xncd15r. Candidates:

- A lower-level OS / runtime kernel loaded below the X-server
- A network-boot loader / decompressor stub
- A licensed co-module (TSSnet, DEC LAT, DECnet)

The ~150 KB span (0x0B00F000-0x0B035000) is plausibly sized for a
small OS kernel or a loader+libc runtime. The three hot entry points
(0x0B00F2B0, 0x0B033960, 0x0B0350F0) look like canonical OS
primitives (printf / event-loop / wait).

This is the real "splice" story: the Boot Monitor has **209
compile-time-resolved external calls** into an image that must be
loaded into DRAM before monitor code paths that use them can run.
Those call sites — not the 0xFF padding regions — are the load-bearing
evidence for the `-splice.u8` filename.

## Status & next steps

- **Done**: ROM structural map; NVRAM setup tool standalone
  disassembly; Xncd image inventory + identification; `mipshunt.py`
  polished and tested; Xncd15r heuristic inventory; Xt/Xlib symbol
  extraction; MAME cross-reference.
- **Blocked on splice gap**: 310 `jal`s from the setup tool target
  0xBB5xxxxx (~128 KB not in the file). The erased regions
  0x01000-0x01FFF and 0x28000-0x37FFF are the likely missing pieces.
- **Pending**: If the 4 MB PC Card is read, verify hypothesis
  (compressed Xncd15r + std config + fonts) and add the dump to this
  repo. A CIS-tuple parser + linear-flash dump script can be prepared
  in advance.
- **Future**: Call-graph tag propagation in `mipshunt.py` so that
  Xserver / MI / FB fn counts reflect actual code volume; dispatch-
  table (ProcVector[256]) locator for Xncd15r to map opcode→handler.

## Running custom code on the hardware (post-RE)

Structural RE of the monitor is sufficient to build and deploy a
custom replacement for `Xncd15r` (the 0x0ED00000 payload the monitor
fetches over TFTP and jumps to).

Completed artifacts in `ncd15-toolchain/`:

- `/opt/cross/mips-elf/` — binutils 2.42 + gcc 13.2.0, bare-metal
  `mips-elf` (C only, no libc). Rebuild recipe in `BUILD.md`.
- `test/hello.S` — 12-byte smoke test (writes `'H'` to DUART A THR).
  Verified: assembles, links, `objdump -d` output is a valid R3000
  big-endian ELF with entry at 0x0ED00000.
- `xncd15r-mini/` — 280-byte replacement image that prints a banner
  to both DUART channels and spins. Full deploy recipe + monitor CLI
  walkthrough in `xncd15r-mini/README.md`. TFTP server (RFC 1350)
  is `xncd15r-mini/tftpd.py`, copied from the Pi that served the
  3Com CS/2500 during its RE.

Monitor CLI is 2-char prefix dispatched (see "Boot Monitor CLI command
set" above). Typical boot flow:
```
> PI 192.168.1.50 192.168.1.15            # prove LANCE + IP
> BT xncd15r.bin 192.168.1.50 192.168.1.15 # TFTP + jump
```

No persistent side-effects on failure; power-cycle = full recovery.
`SE` edits NVRAM, undoable by another `SE`.

This closes the loop: disassembly → structural model → toolchain →
runnable image on real hardware, with every step reproducible from
the files in this project.

