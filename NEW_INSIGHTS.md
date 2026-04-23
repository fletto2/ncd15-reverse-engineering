# NCD15 — additional insights

Short writeup of concrete new findings from this pass, on top of
what is already in `FINDINGS.md`, `MONITOR.md`, `FRAMEBUFFER.md`,
and the recent `Corrections` update.

> **[`EMULATOR_ERRATA.md`](EMULATOR_ERRATA.md) supersedes.** Two
> specific items in this doc are now known wrong: the claim that
> BEV stays set (actually cleared to Status=0x0000a001 after
> shadow entry, §3 of errata) and the direct `0xBFC00000 →
> 0x0EC00000` jump (actually via KUSEG cached trampoline at
> `0x1FC0006C`, then the copy, then the shadow jump, §12 of
> errata).

## 1. Boot sequence — the KSEG1→KUSEG cache-alias trampoline

Standard MIPS "run from uncached ROM while the memory controller
and caches come up, then jump to a cached alias of the same ROM"
dance. The NCD15 does it precisely:

```
0xBFC00000 lui   $t0, 0x0040              ; BEV bit only
0xBFC00004 mtc0  $t0, Status              ; CP0 Status = 0x00400000
                                          ;   => no interrupts, kernel mode,
                                          ;      BEV still on (pre-init vectors)
0xBFC00008 lui   $t0, 0x002b              ; memctl config value
0xBFC0000c ori   $t0, 0xff2d              ; $t0 = 0x002BFF2D
0xBFC00010 lui   $t1, 0xfffe
0xBFC00014 ori   $t1, 0x0020
0xBFC00018 sw    $t0, 0($t1)              ; *(0xFFFE0020) = 0x002BFF2D
0xBFC0001c addiu $t0, $zero, 0x01f4       ; 500
0xBFC00020-28: *(0xFFFE0010) = 500        ; DRAM refresh period
0xBFC0002c mtc0  $zero, Cause
0xBFC00030-44: jal 0xBFC00188 / 01E4 / 023C  ; three cache-init loops
0xBFC00048 lui   $sp, 0x0EC2
0xBFC0004c ori   $sp, 0x8000              ; $sp := 0x0EC28000
0xBFC00050-60 $t0 := 0xBFC0006C & 0x1FFFFFFF = 0x1FC0006C
0xBFC00064 jr    $t0                      ; jump into KUSEG alias of the
                                          ;   same ROM — now cached
0xBFC0006c j     0xBFC00844               ; encoded j, resolves to
                                          ;   0x1FC00844 in KUSEG
```

Key insights:

- **`and $t0, 0x1FFFFFFF`** is how KSEG1 `0xBFCxxxxx` gets converted
  to KUSEG `0x1FCxxxxx`: stripping the top 3 bits (`101`) of the
  KSEG1 selector leaves the KUSEG selector (`000`). R3052 has no
  MMU, so KUSEG virtual = physical — and KUSEG is cached. The
  CPU is fetching the *same* ROM bytes after the `jr $t0`, but
  through a cached alias.
- **Status stays at `0x00400000`** — only BEV set, everything else
  zero. The monitor runs with interrupts globally masked, in
  kernel mode, with the boot-time exception vectors. BEV never
  gets cleared later in the init path we've read.
- **Stack at `0x0EC28000`** is inside the RAM-linked code region
  (`0x0EC00000 – 0x0EC28000`), growing downward. Because DRAM is
  aliased by the memory controller, `0x0EC28000` physical maps
  into the same 4 MB bank the Xncd15r image will eventually fill.
  Stack can therefore sit on top of the code region without a
  separate allocation.

## 2. Memory controller config value `0x002BFF2D`

Written to `0xFFFE0020` before anything else. The refresh period
register `0xFFFE0010` then gets `500` (0x1F4). The chip is not
identified — the memory controller is a piece of NCD-custom glue
around DRAM plus the VRAM/ROM/MMIO decode — but the byte breakdown
is suggestive:

| Bits (MSB) | Value | Likely role |
|---|---|---|
| `0x00` | upper 8 | reserved / zero |
| `0x2B` | bits 16–23 | DRAM row / bank config |
| `0xFF` | bits 8–15 | enable-all mask for decode regions |
| `0x2D` | bits 0–7  | timing (CAS/RAS cycles) |

Without a datasheet this is inference, but the `0xFF` byte strongly
looks like "enable every decode region" (which aligns with the
ROM + DRAM + VRAM + DUART + LANCE + CRTC + NVRAM map all being
live immediately after reset).

## 3. Three distinct cache-init loops

`BFC00188`, `BFC001E4`, `BFC0023C` are three separate cache-sizing
routines, called sequentially before the cached-alias jump. Each
walks a different range with different byte-stride. This matches
the R3052 layout (split I$/D$) plus likely a scratchpad or tag-ram
probe: one loop writes a known pattern, reads it back, and notes
the largest power-of-two size for which writes don't alias. The
monitor stores the result at `data_0x0EC00C34` (`g_ramdac_c34` —
misnomer, the callers are cache-size consumers, not RAMDAC).

## 4. Full boot-vtable (0x0EC00880 – 0x0EC008CC, 17 slots)

The straight-line init block at `0x0EC00D80 – 0x0EC00ED0` populates
17 function-pointer slots. All but one point to monitor code; the
one exception is `0x8B0`, the Xncd15r callback.

| Slot | Value | Target region | Likely role |
|---|---|---|---|
| `0x880` | (set later) | monitor state | scratch / temporary |
| `0x884` | `0x0EC0851C` | monitor .text | fn |
| `0x890` | `0x0EC0838C` | monitor .text | `g_ui_func_table` (known) |
| `0x894` | `0x0EC06334` | monitor .text | fn |
| `0x898` | `0x0EC06440` | monitor .text | fn |
| `0x8A4` | `0x0EC048D8` | monitor .text | fn |
| `0x8A8` | `0x0EC04818` | monitor .text | fn |
| `0x8AC` | `0x0EC023C4` | monitor .text | fn (printf-adjacent) |
| **`0x8B0`** | **`0x0ED40000`** | **Xncd15r `.text`+0x40000** | **framebuffer-draw callback** |
| `0x8B4` | `0x0EC0D44C` | monitor .text | fn |
| `0x8B8` | `0x0EC08B2C` | monitor .text | fn |
| `0x8BC` | `0x0EC0F078` | monitor .text | fn |
| `0x8C0` | `0x0EC0F414` | monitor .text | fn |
| `0x8C4` | `0x0EC083E0` | monitor .text | fn |
| `0x8C8` | `0x0EC08098` | monitor .text | `g_screen_out_vec` (known) |
| `0x8CC` | `0x0EC08054` | monitor .text | `g_screen_out_vec2` (known) |

13 of those (`0x8A4 – 0x8CC`) form a contiguous fn-pointer array,
matching a classic C-struct layout of "console ops": print_char,
print_line, flush, clear, …, with `0x8B0` as the one framebuffer
op that gets shimmed to Xncd15r.

## 5. The only DRAM-pointing vtable entry = the framebuffer hook

This is a redundant framing of a point from `FRAMEBUFFER.md`, but
worth stating as a standalone rule: **exactly one of the 17 boot
vtable entries points into Xncd15r's address range**, and it is
the one named `0x8B0`. Every other entry is self-contained monitor
code that runs identically whether or not the X server has been
loaded. The monitor can boot to `BT`/`PI`/`DA` and service the
serial console with the X server absent; it only needs the X
server if something wants to actually draw on the framebuffer.

## 6. The video-card ID block at `0xAF000000..3`

`fn_Using_Subnet_Mask` (the big hw-probe fn at `0x0EC00CAC`) reads
byte 0, 1, 2, 3 of `0xAF000000` (via `sub_0ec02bfc`, a byte-read
helper) and dispatches on each byte individually. On failure of
any byte, the error string at `0x0EC1D8B8 + 0x24*i` is used —
except that those "strings" are actually `~36-byte code stubs`
that look like they're treated as printfable data but are in fact
executable callouts for a per-board-type init path.

This is consistent with the user-visible observation that the
same video cartridge is intended to work in both a 15r and a 19r
board at different resolutions: the cart exposes a 4-byte ID that
the monitor maps to a per-config code block.

## 7. `NCD15 Boot Monitor V2.7.1`

Not strictly new — the `-V271-` string is in the ROM filename —
but it is now confirmed as the version string inside `monitor.dis`
too (line 2). No conflicting version strings appear elsewhere in
the ROM, and the HMX PRO font match is to **V2.7.2** (one minor
revision ahead). These two firmwares are clearly siblings off the
same source tree.

## 8. Recap of dead code / unused regions

After this pass, three regions are high-confidence "dead":

- **Font + pointer table + pangram** at `0x22B50 – 0x27EDF` — no
  code references it; inherited from HMX PRO (see
  `FRAMEBUFFER_FONT.md` update).
- **Cache/memory-test subroutine at `0x0EC00A40`** — never `jal`'d
  from anywhere; falls through from the end of `fn_ramdac` but is
  unreachable at runtime.
- **Erased span `0x28DE0 – 0x37FFF`** (~61 KB of `0xFF`) — a
  genuine erased region of the ROM, not a splice gap. The NVRAM
  Setup tool has 94 `jal`s into this block; they are either
  unreachable paths or holdovers from a prior revision where
  code lived there. Either way, not code the monitor will run on
  this hardware.

## 9. `BT` is an ECOFF loader, not a raw-image loader

The monitor's TFTP-boot path (`BT file local host`) does **not**
accept a flat MIPS blob. It parses the downloaded file as ECOFF
and rejects anything that fails these checks, all visible in
`sub_0ec10834 / sub_0ec10910`:

1. `f_opthdr == 0x38` (aouthdr size, file hdr +0x10).
2. `f_nscns ∈ [3, 7]` (number of section headers).
3. `f_flags ∈ [3, 7]` (F_RELFLG | F_EXEC | F_LNNO mix).
4. `aouthdr.magic == 0x0107` (R3000 OMAGIC/ZMAGIC).
5. Section 0's `s_flags & 0x20 == STYP_TEXT` (first section must
   be `.text`).
6. `s_paddr > 0x0ECFFFFF` for every loadable section (anything in
   the monitor ROM region is rejected).
7. `s_scnptr` of the first section must be ≥ `0x4C + nscns*40`
   (inside the file, past the headers).

The loader then:
- Stores `aouthdr.entry` into monitor vtable slot `0x0EC00880`.
- Walks sections; for each whose `s_flags & 0x7E0` is non-zero
  (i.e. STYP_TEXT|STYP_DATA|STYP_BSS|STYP_LIT4|STYP_LIT8|STYP_SDATA),
  copies `s_size` bytes from file offset `s_scnptr` to physical
  address `s_paddr`.
- `jalr`s through slot `0x880` to enter the loaded image.

The first 2 bytes of any valid boot image must therefore be
`0x01 0x60` — the ECOFF MIPS-BE file magic — not a raw MIPS
instruction.

A tool `ncd15-ecoff-wrap` (in this repo under `xncd15r-mini/`
and installed to `/opt/cross/mips-elf/bin/`) wraps a flat
post-`objcopy -O binary` blob into the minimum ECOFF the
monitor accepts: 1 × `.text` section plus zero-length `.data`
and `.bss` to make `nscns = 3`. The shipped `xncd15r-mini/xncd15r.bin`
is now that wrapped form (196-byte ECOFF header + 280-byte
payload = 476 bytes total).

Default load/entry is **`0x8ED00000`** — the KSEG0 cached view of
physical `0x0ED00000`, matching the real Xncd15r's ECOFF header
(`mips-elf-objdump -h Xncd15r` confirms). The loader masks the
top 3 bits of `s_paddr` before writing, so KSEG0 vs KUSEG for the
load address only affects the execution view (both hit the same
physical RAM); using KSEG0 is just the canonical convention and
keeps cached fetch explicit in kernel mode.

## 10. ECOFF support in the toolchain

Binutils 2.42 declares `mips-*-ecoff` obsolete and refuses to
configure with that target triplet. The ECOFF BFD vectors
(`ecoff-bigmips`, `ecoff-littlemips`) are still present in the
tree, just not wired up by `--target=mips-elf`. Rebuilding with

```
../binutils-2.42/configure --target=mips-elf \
  --prefix=/opt/cross/mips-elf \
  --enable-targets=mips-netbsd \
  --with-sysroot --disable-nls --disable-werror --disable-multilib
```

adds `ecoff-bigmips` / `ecoff-littlemips` to `mips-elf-objcopy
--info`, enabling

```
mips-elf-objcopy -O ecoff-bigmips in.elf out.ecoff
mips-elf-ld --oformat=ecoff-bigmips ...
```

…but the output still fails the NCD15 monitor's loader checks
because binutils hard-codes `f_flags = 0x0060` and aout magic
`0x0207` (NMAGIC), while the monitor demands `f_flags ∈ [3..7]`
and magic `0x0107` (OMAGIC). Native BFD output therefore needs
the same post-processing our `ncd15-ecoff-wrap` already does, so
the Python wrapper stays the canonical path. A companion helper
`ncd15-ecoff-dump` (same directory) prints the structured header
for `diff` comparison against the real Xncd15r.

## 11. Stuff still worth chasing

- The four-byte board-ID semantics (per-byte → per-config-path).
  Reading `sub_0ec02bfc` carefully should reveal whether each
  byte is a discrete boolean ("has X?") or a small enum
  ("revision N").
- The memctl config byte field decode. Either requires a
  datasheet or a live `DM 0xFFFE0020 4` on hardware.
- Whether `0x0ED40000` is a single fixed entry into Xncd15r or
  whether Xncd15r patches the slot at its own init (and what to
  — likely the address of `fbCopyArea` or `mfbPutImage`).
- Confirming by chip print that the "video cartridge" is a real
  NuBus-style cart rather than a soldered-down video subsystem
  (the user's "markings on the NuBus card imply the same cart
  works in 19r" strongly suggests a removable cart).
