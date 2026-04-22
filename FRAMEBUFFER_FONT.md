# Framebuffer bitmap font in the NCD15 boot monitor

Yes — there is a real bitmap font in the monitor ROM, intended to be
drawn on the framebuffer during POST / boot. This is a note on what
was found, where it lives, and how it is structured.

## Where

```
ROM file offset  0x22B50 – 0x25190   (9 792 bytes)   glyph bitmaps
ROM file offset  0x27B40 – 0x27EA8   (   360 bytes)  glyph pointer table
ROM file offset  0x27EB0 – 0x27EE4   (    52 bytes)  pangram test string
ROM file offset  0x27EE8 – 0x28000   (  ~280 bytes)  DUART init (reg/value pairs)
```

In virtual addresses, the font data region is
`0x0EC22B50 – 0x0EC25190` and the pointer table is
`0x0EC27B48 – 0x0EC27EA8`.

The glyph region was already flagged in `monitor.dis` line 50535 as
"Console bitmap font ... byte-identical to HMX PRO V2.7.2 font at
its file 0x27EC0". The pointer table and the purpose of the whole
block (i.e. "drawn to the framebuffer") was not previously nailed
down.

## Pangram at 0x27EB0

Immediately after the glyph pointer table, the ROM contains, as
plain ASCII:

```
"The quick brown fox jumped over the starved lazy dog"
```

(52 bytes, unterminated here, followed by a NULL word and then the
DUART init tables.) The presence of a classic typography-test
pangram inside the boot ROM is the cleanest possible evidence that
this block is a **font that gets rendered to the framebuffer** —
almost certainly as a visual self-test during POST, and probably
also as the font used for the "press key to enter setup" / boot
status lines shown on the screen while the X server is being
fetched.

## Format — variable-width, variable-height glyphs via pointer table

The font is **not** a fixed-stride char-indexed array. Instead:

- The pointer table at `0x27B48` is an array of 32-bit big-endian
  pointers. About 151 entries are valid pointers into the glyph
  region, 33 entries are NULL, and ~40 entries hold other values
  (small integers, pointers into a separate fixup region around
  `0x0EC2524C – 0x0EC25AFC`).
- 60 unique glyph pointers across those entries. Duplicates are
  common — e.g. `font+0x1554` (a narrow vertical-bar glyph used as
  a spacer) appears dozens of times interleaved between letter
  glyphs, suggesting the table is structured as
  `(glyph_ptr, spacer_ptr, glyph_ptr, spacer_ptr, ...)` — i.e.
  alternating "what to draw" and "inter-letter kerning glyph".
- Each glyph starts immediately at its pointer; there is **no
  per-glyph header byte** for width or height. Size is implicit
  and varies from 20 bytes (≈ 16×10 bit) to 264 bytes
  (≈ 16×130 bit block — possibly a composite graphic, not a
  single letter). The average is ~160 bytes / glyph.

Rendered as 16-pixel-wide rows (2 bytes / row, MSB-left,
`1 = foreground`, `0 = background`), the glyphs are recognisable
letters:

```
font+0x0958  (passed directly by fn at 0x0EC0B4B4)
    ##########......
    ############....
    ##........##....
    ##.........##...
    ##.........##...
    ...     (identical rows)
    ##.........##...
    ##........##....
    ############....
    ##########......
```

A clean capital **O** (or zero) at ~16×20 pixels. Similar renders
for other entries produce identifiable curves, arms, and stems of
Latin letters.

## Call site

The two direct code references into the glyph region are at
monitor VA `0x0EC0B490` and `0x0EC0B4B8`, both passing explicit
glyph pointers to `sub_0EC0CC18` — a blit-to-framebuffer helper.
Those two sprites appear to be special status icons (e.g. a
"network activity" or "boot progress" indicator) drawn by the
caller directly, bypassing the pointer-table path used by the
general string render.

The general string-rendering path loads the table at
`0x0EC27B48` via `$gp`-relative addressing (no direct
`lui/addiu` pair for its base was found), then indexes by a
processed character code to fetch a glyph pointer, then calls the
blit helper. Identifying that function precisely is future work.

## Why this matters

- **Concrete confirmation** that the NCD15 boot monitor is wired
  to draw text on its own framebuffer — not just DUART-only
  output. Any "press Ctrl-Setup to enter NVRAM setup" / "Booting
  from 192.168.x.y..." text the user sees on the 1024×800 ECL
  monitor at power-up is rendered using this font.
- **A reusable asset.** The 60 glyph bitmaps + pointer table are
  straightforward to extract. A future pass can lift them into a
  BDF or PSF file and render the full glyph set to verify coverage
  (likely upper+lower alpha + digits + a few punctuation, matching
  the ~60 distinct shapes).
- **Shared heritage.** The block is byte-identical to the HMX PRO
  V2.7.2 ROM at its own file offset `0x27EC0`. The same font (and
  probably the same draw routine) was carried forward into the
  later R4600-based MIPS X-terminal generation.

## Update — the font appears to be dead ROM

Follow-up search for the *rendering* function that walks the
pointer table shows it does not exist in any of the three
disassemblies (monitor, NVRAM setup, Xncd15r). Specifically:

- No `lui` + `addiu`/`ori` pair in the monitor computes the
  base `0x0EC27B48` (or any address in `0x0EC27B00-0x0EC27EFF`).
- No `lw` with an offset that would reach the table from any
  register known to hold an `0x0EC2xxxx` upper half.
- No 32-bit word equal to `0x0EC27B48` appears anywhere in the
  ROM as data (i.e. no one loads the address indirectly from a
  pointer constant).
- The only code that looks like a "table indexed by input value
  ×8, base = address near the font" is `0x0EC0B4E8` — but it
  indexes **`0x0EC25B48`**, not `0x0EC27B48`. Inspecting that
  data shows `(char, scancode)` 16-bit pairs: it is a **keymap**
  dispatch, not a font one.
- `Xncd15r.dis` has zero references to the monitor's font
  region, pangram address, or pointer table.
- The monitor performs only **three** VRAM stores in the entire
  32 K lines of disassembly, all within the VRAM-sizing probe
  (see `FRAMEBUFFER.md`).

Combined with the earlier finding that the monitor defers all
screen drawing to Xncd15r via the slot-`0x0EC008B0` callback,
the most plausible reading is that **this font is inherited
firmware, not active code**. The comment in `monitor.dis`
line 50536 — "byte-identical to HMX PRO V2.7.2 font at its file
0x27EC0" — is itself evidence: the shared codebase put the font
in both ROMs, but the NCD15 boot path does not draw characters
on its own framebuffer.

Practical upshot:

- The 60 extracted glyphs in `monitor-font/` are still a valid
  historical artifact — they are real bitmaps of a font that
  the HMX-family firmware can render. They just are not
  rendered by *this* firmware.
- Recovering the exact ASCII mapping therefore requires looking
  at a live HMX PRO or its ROM, not this one. The monitor-font
  dump here is useful for cross-referencing against an HMX
  rendering dump.

## File references

- `disasm/monitor.dis` line 50535 — original "Console bitmap
  font" annotation.
- ROM file `NCD15-19rBM-V271-splice.u8`, offsets listed above.
- `FRAMEBUFFER.md` — the live on-screen drawing path (callback
  into Xncd15r, no direct font use).
