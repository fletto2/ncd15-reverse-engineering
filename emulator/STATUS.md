# NCD15 emulator — status

Standalone C emulator. Boots the V2.7.1 boot monitor to its
interactive `>` prompt and processes CLI commands.

## Session transcript (live)

```
$ ./ncd15-emu NCD15-19rBM-V271-splice.u8
NCD15 emulator: ROM loaded (262144 bytes), PC=0xbfc00000
channel B (monitor console) -> stdout; channel A -> stderr

PANIC -- Need to initalize NVRAM first
PANIC -- Need to initalize NVRAM first
Boot Monitor V2.7.1
Testing memory \|/-\|/-\|/-... 4.0 Mbytes
Boot Monitor
> ?
BL[file] boot locally
BN[file][local-IP host-IP][gateway-IP][subnet-mask] boot via nfs
BT[file][local-IP host-IP][gateway-IP][subnet-mask] boot via tftp
DA display addresses
DM[adr][len] display memory
DR display registers
DS display booting statistics
EX extended tests
KM keyboard mapper
KS keyboard statistics
NV NVRAM utility
PI[timeout][local-IP host-IP][gateway-IP][subnet-mask] ping host
RS reset system
SE NVRAM setup
SM show memory configuration
ST stack trace
TR[4 or 16] set token ring network speed
UP[file][local-IP host-IP][gateway-IP][subnet-mask] upload via tftp
ZK zero keyboard statistics
ZS zero boot statistics
> DR
R0 zero  R1 at    R2 v0    R3 v1    R4 a0    R5 a1    R6 a2    R7 a3
00000000 0EC00000 00000006 00000000 0000F001 00000004 00000FFF 0EC23348
...
```

## What's real

- **MIPS-I R3052 interpreter** (`src/mips.c`) — full two-PC delay-slot
  semantics, all integer opcodes used by the monitor, including
  unaligned LWL/LWR/SWL/SWR; CP0 (Status/Cause/EPC/BadVAddr/PRId),
  RFE, exception dispatch with KU/IE stack shift.
- **Direct-mapped I-cache**, 4 KB, 16-byte lines. Invalidated on
  cached stores; bypass for KSEG1. Essential to survive the
  monitor's uncached memtest which otherwise scribbles shadow code.
- **Memory map** (`src/memory.c`) — separate backing stores for
  4 MB DRAM (phys 0), 4 MB shadow bank (phys 0x0EC00000, holds the
  copied monitor image + stack), and 1 MB VRAM (phys 0x0F000000).
  ROM window at both 0xBFC00000 (KSEG1) and 0x1FC00000 (KUSEG
  cached alias used by the reset trampoline). No aliasing across
  banks — splitting was necessary because VRAM memtest aliases
  into shadow code otherwise.
- **SCN2681 DUART** (`src/duart.c`) — 4-byte register stride.
  Byte-granular MMIO read/write so big-endian byte reads return
  the correct byte of a 4-byte register. Channel B (monitor
  console) to stdout, channel A to stderr.
- **CRTC / video-sync stub** — responds to the boot sync loop at
  0x0EC03E18 by toggling bit 1 of byte 0 at a slow cadence.
- **Stdin pacing** — non-blocking read into an internal FIFO, feed
  one byte to the DUART when its RX queue is drained. Without
  pacing the monitor drops chars silently.

## Critical hacks

These keep the monitor running without a real interrupt handler:

| Location | Behavior |
|---|---|
| `data_0x0EC00730` | Auto-increments on shadow-resident reads; the monitor's delay loops wait on this tick counter (no ISR installed at 0x80000080 writes it) |
| `data_0x0EC01440` | Returns 1 on shadow-resident reads; forces the monitor onto its polling DUART-read path instead of the ISR-fed ring queue |
| CRTC byte 0 bit 1 | Toggles on a slow clock for the boot sync edge-detect |

## What's stubbed / missing

- **93C46 NVRAM at 0xAEC80000** — not modelled. The monitor's
  NVRAM-read function pointer (`data_0x0EC008D8`) is never
  initialized by any code in the disassembly, and the check at
  0x0EC04630 short-circuits the NVRAM path when
  `data_0x0EC00C34 == 0`. The two "PANIC -- Need to initalize
  NVRAM first" lines are authentic blank-NVRAM behavior.
- **LANCE Ethernet** at 0xBE482000 — skeleton glue only; enough
  to not crash. Needed for `BT` (TFTP boot) and `PI` (ping).
- **Interrupts** — dispatch is implemented but no device raises
  IP lines. The monitor uses polling throughout its main code
  path, so this is fine for the CLI.

## Usage

```bash
cd emulator
make                     # builds ncd15-emu
make run                 # default 1M cycles, no stdin

# Interactive CLI:
./ncd15-emu ../../NCD15-19rBM-V271-splice.u8

# Pipe a script:
( sleep 2; printf '?\r'; sleep 3 ) | ./ncd15-emu ../../NCD15-19rBM-V271-splice.u8

# Trace bus accesses:
./ncd15-emu --trace --max-cycles 2000 ../../rom.u8  2> trace.log
```

End commands with `\r` (CR), not `\n`. The monitor is line-terminated
on CR. When typing in a terminal, your Enter key is probably CR in
the default mode — fine.

## Files

```
emulator/
├── Makefile
├── README.md
├── STATUS.md           — this file
├── src/
│   ├── emu.h           — shared types + constants
│   ├── memory.c        — bus dispatch, 3 backing stores, MMIO routing
│   ├── mips.c          — R3052 interpreter + I-cache + exceptions
│   ├── duart.c         — SCN2681 DUART + CRTC/memctl/vidctl stubs
│   ├── lance.c         — Am79C90 core (adapted from 3com68k)
│   ├── lance_glue.c    — bus bridge for LANCE
│   └── main.c          — stdin pacing + device wiring
└── vendor/             — references (SOURCES.md)
```

## Design notes

- Endianness: big-endian throughout.
- No JIT — pure interpretation. ~20 M instructions/second; boots
  to the CLI in well under a second of wall time.
- The monitor.dis previously shipped had all VAs in the main
  section shifted -0x2000 from reality — the dis-maker assumed
  shadow source was ROM+0x4000, but the monitor's reset code
  actually copies from ROM+0x2000. Our emulator trace proved this;
  `dump_monitor.py` in the parent repo was fixed and the
  corrected disassembly is what's used by anyone correlating
  against emulator-observed PCs.
