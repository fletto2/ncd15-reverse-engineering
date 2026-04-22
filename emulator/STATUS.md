# NCD15 emulator — status

Minimal standalone C emulator, 4 source files, no external dependencies
beyond libc.

## What works

- **MIPS-I interpreter** (`src/mips.c`) — enough opcodes to run the
  boot monitor: all SPECIAL-class ALU / shift / multiply / divide,
  REGIMM branches, all the standard J/JAL/BEQ/BNE/B*Z branches, loads
  and stores (including unaligned LWL/LWR/SWL/SWR), LUI/ADDIU/etc.,
  CP0 (MFC0/MTC0/RFE for registers 8/12/13/14/15).
- **Memory map** (`src/memory.c`) — ROM at phys `0x0EC00000`
  (also accessible via the KUSEG `0x1FC00000` alias used by the
  cache trampoline). 4 MB DRAM aliased at all physical addresses
  below the ROM base (bits 21:0 decode; higher bits ignored), matching
  the "stride 0x03D00000, 4 MB bank" pattern from `FINDINGS.md`.
- **MC68681/SCN2681 DUART** (`src/duart.c`) with 4-byte register
  stride, channel A → stdout, channel B → stderr. Enough of the
  status + THR registers to get banner output.
- **Memory controller stub** (`src/duart.c`) at `0xFFFE0000`.
- **Video-control cart-ID stub** (`src/duart.c`) at `0xAF000000..3`.
- **Catch-all stub** for every other unmapped MMIO address — returns
  all-ones on reads (so monitor polling loops for ready/idle flags
  advance), silently drops writes.

## Current boot depth (as of last test)

5 million instructions run without halting. CPU reaches the DRAM
memory-test loop inside `fn_ramdac` / `sub_0ec00a40` — `r19` holds the
classic `0x5a5a5a5a` pattern, `r15` = 0x00400000 (4 MB), sp in the
right ballpark. No crashes, no unmapped accesses.

## What's left to reach the `>` prompt

1. **Identify the device at `0xBE200000`** (unlisted in `FINDINGS.md`;
   the reset code's 0xBFC005BC helper polls it via a multi-layer call
   chain). Currently served by the all-ones stub; real behaviour
   unknown. Candidates: early diagnostic serial, watchdog, initial
   bootstrap signal register.
2. **Real 93C46 NVRAM** at `0xAEC80000` — monitor reads its MAC and
   boot-file name from here. Stub returns ones, which may confuse the
   setup path. ~150 LOC to add.
3. **LANCE Ethernet** at `0xBE482000` — needed for `PI`, `BT`, `DA`
   commands to function. ~800 LOC if we adapt GXemul's `dev_le.cc`.
4. **CRTC** at `0xBE380000` — vsync polling loop at bit 8 of
   `0xBE380013` currently returns ones (matches "always in vsync"
   = immediate progress). Likely fine as-is for monitor boot.
5. **Video framebuffer** rendering — not needed for a serial-console
   `BT` boot.

## Usage

```bash
cd emulator
make              # builds ncd15-emu
make run          # runs the ROM at ../../NCD15-19rBM-V271-splice.u8, 1M cycles
make trace        # MIPS_TRACE=1 + --trace bus logging (first 200 lines)

# Or directly:
./ncd15-emu --max-cycles 10000000 /path/to/NCD15-19rBM-V271-splice.u8
./ncd15-emu --trace --max-cycles 2000 /path/to/rom.u8  2> trace.log
```

The ROM path defaults to `../../NCD15-19rBM-V271-splice.u8` (the one
next to the annotated disassembly).

## Files

```
emulator/
├── Makefile         — 30 lines
├── README.md        — goal, vendor sources, plan
├── STATUS.md        — this file
├── src/
│   ├── emu.h        — shared types and constants (~130 lines)
│   ├── memory.c     — bus dispatch, ROM/DRAM/MMIO routing (~130 lines)
│   ├── mips.c       — MIPS-I R3052 interpreter (~230 lines)
│   ├── duart.c      — MC68681 DUART + stubs (~180 lines)
│   └── main.c       — CLI + device wiring (~110 lines)
└── vendor/
    └── SOURCES.md   — where to re-clone GXemul + MAME references
```

Total new C: ~780 lines. No runtime dependencies beyond `libc`.

## Design notes

- Chose standalone C over adapting GXemul because the monitor is
  small (32 KB of code, a handful of peripherals) and the NCD15's
  DRAM aliasing + video-cart-ID quirks don't fit GXemul's machine
  model naturally. Writing the interpreter from scratch was faster
  than wiring a new GXemul machine type.
- Endianness: big-endian throughout. MIPS is big-endian on this
  board, and the ROM file is byte-ordered as the CPU sees it.
- No JIT — pure interpretation. 1–2 M instructions/second on a
  modern host, more than enough to boot-through in reasonable time.
- No interrupts implemented yet. Monitor runs with interrupts
  masked (`cp0_status & 1 == 0`) so we're OK to defer.
