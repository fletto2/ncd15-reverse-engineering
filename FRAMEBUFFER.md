# NCD15 framebuffer

Working notes on where and how the NCD15 frame buffer is addressed,
based on disassembly of the monitor's video-detection probe and its
few VRAM access sites. This is an incremental writeup — some points
are confirmed, others are inferred from code structure.

## Base address and bus

- **VRAM base VA**: `0xAF000000` (KSEG1 uncached, physical
  `0x0F000000`).
- The frame buffer lives on a **dedicated VRAM chip on the video
  cartridge**, not on DRAM. Physical `0x0F000000` is far enough
  from the DRAM aliasing region (any-multiple-of `0x03D00000`
  starting at `0x00000000`) that it cannot be a DRAM view — it is
  a distinct decode target of the memory controller.
- Access is **uncached** (the monitor always uses KSEG1 addresses
  for VRAM, and the probe relies on every store being visible
  without a cache flush).

## VRAM sizing probe

`fn_ramdac /* sub_0ec009fc */` at `0x0EC00B98 – 0x0EC00BF0` runs a
classic aliasing probe to infer VRAM size:

```
  sw   $a1, 0(0xAF000000)       ; write pattern at base
  sw   $a0, 4(0xAF000000)       ; write different pattern at +4
  lw   $v0, 0(0xAF000000)       ; read back base
  bne  $v0, $a1, no_fb          ; if mismatch → no VRAM
  
  sw   $a0, 0(0xAF100000)       ; write to +1 MB line
  lw   $v0, 0(0xAF000000)
  beq  $v0, $a1, test_larger    ; if unchanged → no alias at 1 MB
  lui  $v0, 0x10                ; result = 0x00100000 = 1 MB
  j    done

  test_larger:
  sw   $a0, 0(0xAF400000)       ; write to +4 MB line
  lw   $v1, 0(0xAF000000)
  bne  $v1, $a1, larger_still
  lui  $v0, 0x40                ; result = 0x00400000 = 4 MB
  j    done

  larger_still:
  lui  $v0, 0x100               ; result = 0x01000000 = 16 MB
```

So the monitor recognises VRAM sizes of **1 MB / 4 MB / 16 MB** (or
"no frame buffer"). On the NCD15 the installed cartridge is 1 MB —
plenty for a 1 bpp 1024×800 display (which needs 100 KB).

## Framebuffer geometry (display layer)

- **Resolution**: 1024×800 at 70 Hz (user-confirmed).
- **Depth**: **1 bpp** (the display driver is ECL monochrome; there
  is no palette hardware — see `FINDINGS.md` § "Corrections").
- **Bytes per scanline**: 128 (1024 / 8).
- **Total bytes**: 102 400 (100 KB).
- **Packing**: the monitor's one bitmap-blit site writes 32-bit
  words with bit position matching pixel position, so pixels are
  MSB-left within each byte (pixel 0 = bit 7, pixel 7 = bit 0), and
  byte offsets increase left-to-right, scanline-by-scanline. This
  matches the classic X11 `XYBitmap` / mfb packing and matches the
  orientation seen when rendering the ROM's glyph bitmaps directly.
- Stride is fixed — no "virtual resolution" with scroll offset is
  set up by the monitor.

## Board-ID register block at 0xAF000000..3

The same probe also reads **four bytes** at
`0xAF000000`, `0xAF000001`, `0xAF000002`, `0xAF000003` (via
`sub_0ec02bfc`) and dispatches on each. These aren't frame buffer
pixels — on the first probe they look like a **video-cartridge
identification byte set** that tells the monitor which cart is
installed. Four non-zero IDs exist, each leading the monitor down
a different config path. This is consistent with the user's
observation that the same cartridge is also used in 19r boards (at
a higher resolution) — the ID bytes let the firmware pick the right
timing table.

## Who writes to VRAM from the monitor

Surprisingly few call sites. A per-function count of stores whose
base register was last-lui'd to any `0xAFxx` hi-half finds **three
writes total**, all inside the sizing probe above.

Nothing else in the monitor directly writes a pixel to
`0xAF000000`. The implication is that the **monitor itself renders
very little on-screen**: POST is driven through the DUART serial
console, and on-screen text (the boot banner, progress lines,
"press Setup to configure", etc.) is done through a callback into
an image loaded in DRAM — see next section.

## Deferred blit via a function pointer at 0x0EC008B0

The second-stage init writes `0x0ED40000` into the monitor's
function-pointer table at `0x0EC008B0`:

```
  0ec00d88:  lui   $v0, 0xed4
  0ec00d8c:  lui   $at, 0xec0
  0ec00d90:  sw    $v0, 0x8b0($at)        ; g_fb_draw_vec := 0x0ED40000
```

Later, at `0x0EC05BF0 – 0x0EC05BF8`:

```
  lw    $v0, 0x8b0($v0)         ; load the vec
  jalr  $v0                     ; dispatch
```

`0x0ED40000` is the Xncd15r `.text` base (`0x8ED00000`) **plus
0x40000** — an entry point inside the X server image at offset
256 KB. It's accessed via the DRAM alias (physical `0x0ED40000`
= DRAM offset `0x00040000`, the same 4 MB bank Xncd15r loads into).

The monitor has no content at `0x0ED40000` at cold-boot time; this
slot is meaningful **only after Xncd15r has been TFTP-loaded**. At
that point `jalr` through slot `0x8B0` invokes an Xncd15r-supplied
framebuffer-draw helper — the X server's own blit primitive,
called back by the monitor when it wants a screen update.

That explains the light VRAM footprint in `monitor.dis`: once the X
server is up, the monitor delegates screen output rather than
writing pixels directly.

## CRTC and video timing (`0xBE380000`)

`fn_video_2 / fn_video_3 / fn_video_4` clustered around
`0x0EC0A6C4 – 0x0EC0A830` poke the CRTC at `0xBE380000+offset`
with VGA-CRTC-style register indices:

```
0xBE380001        video-status / enable register
0xBE380004        horizontal retrace control
0xBE380006        vertical total (low)
0xBE380007        overflow / vertical total high
0xBE380013        vertical retrace status (bit 8 polled for vsync)
```

The CRTC-register convention is plain VGA: a pointer is put into
the address register, then the value into the data register. The
code polls bit 8 of `0xBE380013` in a 10-iteration loop as a
vsync-wait before reprogramming timing.

No explicit "mode table" was located — timing values are hard-coded
as immediates in the flow. The geometry choice (1024×800 vs 19r's
1280×1024) is presumably selected by the board-ID byte read from
`0xAF000000..3` earlier, routing into a different hard-coded
sequence.

## Open questions

- **Where is the 0x0EC008B0 slot's original value set to something
  other than `0x0ED40000`?** At cold boot Xncd15r isn't loaded yet,
  but the monitor may still draw a POST banner. Perhaps the
  monitor patches the slot after TFTP finishes, or perhaps it
  simply doesn't paint the FB before Xncd15r is up and the screen
  stays black during early boot.
- **Exact mapping from 4-byte board-ID → timing table**. The code
  path at `0x0EC01258 – 0x0EC01324` reads the four bytes and does
  something with them that looks like dispatch into code stubs at
  `0x0EC1D8B8`, `0x0EC1D8DC`, `0x0EC1D900`, `0x0EC1D924` (each ~36
  bytes apart — a tiny code block per ID value, not strings).
  Needs a dedicated pass.
- **The bitmap font in the same ROM (see `FRAMEBUFFER_FONT.md`)** —
  how does it reach VRAM? If the X server is the only thing
  drawing on screen, the font may never be used on the NCD15 at
  all (left over from a shared firmware codebase), or it may be
  used only via a POST-time FB-clear + test-pattern path that
  bypasses the `0x08B0` slot.

## Short summary

- VRAM: 1 MB at uncached VA `0xAF000000` (phys `0x0F000000`).
- Display: 1 bpp, 1024×800, 70 Hz, 128-byte scanline stride.
- CRTC: `0xBE380000`, VGA-style index/data registers.
- The monitor barely touches VRAM directly — most drawing goes
  through a slot-`0x8B0` callback into the X server once it is up.
