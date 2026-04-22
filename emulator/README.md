# NCD15 emulator — staging area

Goal: run the NCD15 boot monitor (and eventually Xncd15r) in software,
so we can debug our custom boot images without physical hardware.

This directory is **staging**. No emulator is built yet — only the
upstream sources we'll reuse are vendored here, plus notes on what
needs to be written.

## Upstream sources vendored

### `vendor/gxemul/` — the engine (6.7 MB, BSD-3-Clause)

Cloned from https://github.com/ryoon/gxemul (SVN-converted mirror of
Anders Gavare's GXemul 0.6.1, 2019). GXemul is a full-system emulator
for MIPS-based workstations and has native emulations of most NCD15
chips:

| What we need for NCD15 | GXemul file | Lines | Notes |
|---|---|---|---|
| MIPS R3052 CPU core | `src/cpus/cpu_mips*.cc` + `memory_mips*.cc` | ~10,200 | Supports R3000 family; MIPS-I BE first-class |
| Am7990 LANCE Ethernet | `src/devices/dev_le.cc` | 818 | MMIO base swappable |
| Framebuffer device | `src/devices/dev_fb.cc` | 967 | Needs 1bpp config (default is 8bpp) |
| DECstation machine model | `src/machines/machine_pmax.cc` | 1,012 | Template for `machine_ncd15.cc` |

The DECstation 5000/133 ("3MIN") machine model is the closest analog
to the NCD15 — same CPU family, same LANCE chip, similar bus layout.
Our new machine type will copy that template and change memory-map
addresses.

### `vendor/mame/` — selected device sources (332 KB, BSD-3-Clause)

Staged from the local MAME checkout at `~/src/claude/mame`. Kept
because GXemul doesn't emulate the MC68681/SCN2681 DUART the NCD15
uses on its console (DECstation used a DEC-specific DZ chip instead).

| File | Lines | Purpose |
|---|---|---|
| `src/devices/cpu/mips/mips1.{h,cpp}` + `mips1dsm.{h,cpp}` | ~3000 | Backup MIPS-I core if we prefer it over GXemul's |
| `src/devices/machine/mc68681.{h,cpp}` | ~2500 | DUART — **port to C for use with GXemul** |
| `src/devices/machine/am79c90.{h,cpp}` | ~1800 | LANCE — reference, GXemul's dev_le.cc is easier |
| `src/devices/machine/eepromser.{h,cpp}` + `eeprom.{h,cpp}` | ~1200 | 93C46 NVRAM — port or write from scratch |
| `src/mame/ncd/ncdmips.cpp` | ~250 | HMX PRO driver template (R4600, not NCD15) |
| `src/mame/ncd/ncd68k.cpp`, `ncd88k.cpp`, `bert_m.{cpp,h}` | ~2000 | NCD 68k and 88k X-terminal drivers — reference only |

## What still has to be written

Roughly 2–3k lines of new code. In priority order:

1. **`src/machine_ncd15.cc`** (~400 lines) — a new GXemul machine
   type. Port `machine_pmax.cc` and change:
   - Reset vector → `0xBFC00000` (already MIPS-I default)
   - DRAM aliasing → 4 MB aliased every `0x03D00000` (in the
     memory-map setup, add a custom read/write trap)
   - Remove TurboChannel + DZ + SII + VDAC + colorplanemask (DEC-specific)
   - Add MMIO trap → MC68681 at `0xBE880000` (4-byte stride — NCD
     uses the chip with one register per 32-bit word)
   - Add MMIO trap → LANCE `dev_le` at `0xBE482000`
   - Add MMIO trap → 1 bpp framebuffer at phys `0x0F000000`
     (accessed uncached via KSEG1 `0xAF000000`); 1024×800 = 102 400 B
   - Add MMIO trap → video-control regs at `0xAF000000..3`
     (4-byte cart ID — see `FINDINGS.md`)
   - Add MMIO trap → CRTC at `0xBE380000` (VGA-style index/data;
     vsync bit 8 of `0xBE380013`)
   - Add MMIO trap → bit-banged 93C46 / keyboard-serial line at
     `0xAEC80000`
   - Add MMIO trap → memory controller at `0xFFFE0000`
     (ignore writes, return last written values on reads —
     monitor only writes during init)
   - Boot vtable `0x0EC00880–0x0EC008CC` is just plain DRAM;
     no special handling needed

2. **MC68681 DUART port (C)** (~600 lines) — port `vendor/mame/.../mc68681.cpp`
   to a self-contained C module compatible with GXemul's device API
   (`dev_mc68681_init(machine, mem, base_addr, irq)` signature).
   Only the subset the NCD15 monitor touches needs to work:
   - Channel A TX/RX, interrupt status, baud-rate selection
   - Channel B TX/RX (the monitor writes banners to both)
   - Status bit 2 = TxRDY

3. **93C46 serial EEPROM (C)** (~150 lines) — a state machine with:
   - Chip select, clock, data-in, data-out lines bit-banged through
     `0xAEC80000`
   - 64 × 16-bit storage backed by a file (`nvram.bin`)
   - Read/Write/Erase commands per the 93C46 datasheet

4. **Video rendering** (~200 lines) — render the 1 bpp framebuffer
   to an X11 / SDL window (or save snapshots to PBM files). GXemul
   already has an X11 output path (`src/devices/dev_fb.cc`); we
   configure it with 1 bpp + 1024×800 and point it at our
   framebuffer memory.

5. **Xncd19r signature helper** — when BT-loads happen, the loader
   at `0x0EC10A2C` expects `"Xncd19r"` at .text+0x10 (see
   `BT_LOADER.md`). Make sure our emulator logs cleanly when the
   validator passes / fails each check, so future images can be
   debugged without a physical board.

## Build plan

Once the new sources are written, build with GXemul's existing
autoconf + GNU make flow (in `vendor/gxemul/`). No need for a separate
build system — we just register `machine_ncd15.cc` with GXemul's
`device_add()` table and add the new `dev_mc68681.cc` /
`dev_eeprom93c46.cc` alongside the other `dev_*.cc` files.

Alternatively: **extract** the needed GXemul pieces into a standalone
minimal emulator (drop the X11 dependency, 2D graphics, SCSI, PCI,
TurboChannel, etc.) — reducing the 6.7 MB of src to ~1 MB of focused
NCD15 code. Deferred decision.

## Why not MAME?

MAME's device abstractions (`device_t`, `machine_config`, DBW) are
deeply entangled with its C++ framework. Extracting a runnable subset
would require porting the entire device-registration and
machine-config system — far more work than adopting GXemul, which
already has a simpler C-style device API and targets the same
machine class.

We keep MAME's `mc68681.cpp` + `eepromser.cpp` as **reference
implementations** for the two chips we port by hand.

## License notes

- **GXemul**: BSD 3-Clause (per-file headers + `vendor/gxemul/LICENSE`)
- **MAME**: BSD 3-Clause on the vendored files (`license:BSD-3-Clause`
  in every file's comment header)

All upstream is permissively licensed; no GPL contamination.
