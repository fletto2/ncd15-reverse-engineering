# Monitor framebuffer font — extracted glyphs

Dump of the 60 unique bitmap glyphs referenced from the pointer
table at ROM `0x27B48` (VA `0x0EC27B48`). Full context in the
repo's `FRAMEBUFFER_FONT.md`.

## Format

- Each glyph is 16 pixels wide (2 bytes per row), MSB-left.
- Foreground pixel = 1 bit. No background byte, no per-glyph
  header — bitmaps start immediately at the pointer target.
- Heights are variable and inferred from the distance to the next
  glyph pointer in the sorted unique-pointer list, but many
  glyphs visibly fit in ~16–24 rows, and taller "sizes" include
  data that is actually the start of the *next* glyph. Treat
  heights past row 24 as unreliable.

## Files

- `glyph_NN_gXXXX_Rrows.pbm` — PBM (netpbm P4) bitmaps, one per
  unique glyph. `NN` = index (0–59), `XXXX` = offset from font
  base `0x0EC22B50`, `R` = raw-delta-derived row count.
- `glyphs.txt` — ASCII-art dump of every glyph at its raw delta
  height (useful but includes overshoot into next-glyph data).
- `glyphs_24row.txt` — same, capped at 24 rows; this is the
  easiest to read by eye.
- `slots.txt` — the full 108-pair pointer-table dump, showing
  which glyphs are referenced at which slot. Slots 92–107 point
  into `0x25364..0x25AFx` which is a **keymap / scancode table**,
  not glyph data — those are kept here for completeness but are
  out of scope for the font.

## Key observations

- **60 unique glyph bitmaps** for the on-screen font.
- **Pair structure** in the table: most slots hold `(A, B)` where
  `B` is either NULL or one of two common "companion" glyphs —
  `g+1554` (tiny vertical ticks, probably an underline / cursor
  marker) or `g+07fc` (chevron + underline, likely a digit
  decoration). The rendering semantics of the pair are still
  being worked out — best guess is `A` = the character shape,
  `B` = an optional overlay drawn at the same position.
- **Slots 50–65 are identical to slots 66–81** — the same 16
  glyph pointers in the same order. This is very likely
  "uppercase duplicates lowercase" or the same alphabet subset
  referenced from two separate menu screens.
- **Slots 82–90** share `B = g+07fc` (chevron/underline) and the
  `A` pointers form a coherent digit-set cluster — almost
  certainly the digits `0–9` with underline decoration for
  field display.
- **Pangram at ROM 0x27EB0** (`"The quick brown fox jumped over
  the starved lazy dog"`) immediately follows the table, confirming
  a font self-test path.

## What these glyphs map to (visual reading)

Approximate identifications from the 24-row rendering. Given the
variable height and composite-A/B structure, treat these as
hints, not assignments:

| # | offset | rough shape |
|---|--------|-------------|
| 00 | `0x07e8` | small filled diamond |
| 01 | `0x07fc` | chevron + underline (digit decoration) |
| 03 | `0x0964` | vertical bar ("I" / "1") |
| 04 | `0x09ec` | **C** |
| 06 | `0x0ae0` | **L** |
| 08 | `0x0ba4` | **D** |
| 10 | `0x0cfc` | **A** (with crossbar) |
| 11 | `0x0d98` | **7** |
| 12 | `0x0dac` | **T** |
| 14 | `0x0e98` | upper bar + descender arrow |
| 15 | `0x0f38` | **O** |
| 17 | `0x1028` | **b** |
| 18 | `0x10b8` | **k** |
| 19 | `0x113c` | **n** |
| 22 | `0x1298` | **u** |
| 23 | `0x132c` | **X** |
| 25 | `0x1554` | tiny vertical ticks (cursor/underline spacer) |
| 26 | `0x157c` | **T** (alt) |
| 28 | `0x159c` | **X** (compact) |
| 36 | `0x1a28` | **Y** |
| 39 | `0x1b98` | **E** |
| 40 | `0x1c24` | **E** (alt) |
| 43 | `0x1e0c` | **O** (oval) |
| 44 | `0x1ea4` | **V** |
| 46 | `0x1f88` | **H** |
| 47 | `0x200c` | **S** |
| 51 | `0x21b8` | **F** |
| 52 | `0x2268` | **E** (serif) |
| 57 | `0x24f0` | **U** |

Many of the entries not called out are composite, duplicated, or
tiny fragments used as punctuation/decoration overlays.

## Next steps to finish the font lift

1. Walk from `0x0EC27B48` in monitor code and find the function
   that reads the slot table. It must index by a transformed
   input (character code? menu-screen token?); once we have the
   index function, the slot → glyph mapping is exact.
2. Once the mapping is known, emit a proper BDF file.
