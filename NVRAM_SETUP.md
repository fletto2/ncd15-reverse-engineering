# NCD15 NVRAM Setup tool — operational description

> **See [`EMULATOR_ERRATA.md`](EMULATOR_ERRATA.md) §2 and §5.** The
> 93C46 is bit-banged via DUART OPR/IPCR (OP4=DI, OP5=CS, OP6=SK,
> DO=IP2), not via a standalone MMIO at `0xAEC80000`. DO is clocked
> **LSB-first** within each 16-bit word. File format (for the
> emulator's `--nvram` flag): 128 bytes, 64 little-endian words.
> Known layout so far: bytes 2..7 = MAC address.

The NVRAM Setup tool is a small, self-contained MIPS program shipped
in the same ROM as the Boot Monitor but linked to a completely
different base address. It runs as the back-end of the monitor's
`SE` command and presents the interactive field editor users see on
the serial console when configuring IP addresses, the boot file, and
related persistent settings.

This document describes how the tool is laid out, how the monitor
reaches it, and how it edits the 93C46-family NVRAM. The
disassembly is `disasm/nvram_setup.dis` (~5.7 K lines, 60 fns, 97%
of headers topic-tagged after the classifier pass).

## Where it lives in ROM

```
file 0x38000 – 0x3DFFF  (24 KB)   NVRAM Setup tool — linked at 0xBED40000
```

The `NCD15-19rBM-V271-splice.u8` filename means "even+odd halves of
the two 128 KiB ROM chips combined into a single 256 KiB image" —
the dump is **complete**, not a partial / spliced capture. The
`0x28DE0 – 0x37FFF` region (~61 KB of `0xFF`) is genuinely erased
ROM inside a complete image, and the Setup tool is the last chunk
before end-of-image. Its entry at file offset `0x38000` is a
two-word trampoline that loads its link base and jumps to the real
entry point inside the slice.

## Why it is a separate image

The Boot Monitor proper is linked at `0x0EC00000`; the NVRAM Setup
tool is linked at `0xBED40000` (KSEG0 cached view of a different
physical page). Splitting it out means:

- The monitor can ship without the Setup UI if desired (the ROM is
  just two linked binaries concatenated with a padding gap).
- The Setup tool can be replaced or patched without rebuilding the
  monitor.
- Cross-image calls go through well-defined entry points rather
  than arbitrary symbols — see "External calls" below.

Of intra-slice `jal` targets, 45% self-resolve with base
`0xBED40000`; the other 55% target addresses inside the erased
`0x28000 – 0x37FFF` region of the ROM. Because the dump is
complete, those targets land on real (erased, `0xFF`) ROM — so
they are either unreachable at runtime, called only in a context
where that region is filled by something other than ROM, or
holdovers from an earlier ROM revision. See "External calls" below.

## How the monitor reaches it

The monitor's two-character CLI has an `SE` command. Its handler
loads the Setup tool's entry address and jumps there, passing
control to `0xBED40000 +` a small prologue that sets up the Setup
tool's stack, clears its BSS, and falls through into the interactive
main loop.

The Setup tool makes no assumption that the monitor's environment is
preserved across the hand-off beyond: DUART is initialized (it reuses
the monitor's console helpers via a function-pointer table — see
below), LANCE is optional (it doesn't need the network), and the
93C46-style NVRAM serial path at `0xAEC80000` (bit-banged from the
CPU through a 7407 buffer — there is no mainboard MCU) is available
for its own reads and writes.

## Configuration fields

The interactive UI walks the user through the persistent settings
stored in the 93C46 EEPROM. The fields and their print/parse
functions are visible in the `fn_nvram_*` and `fn_printf_*` clusters
of the disassembly:

- Local IP address (the NCD15's own IP)
- Host IP address (TFTP / boot server)
- Gateway IP address
- Subnet mask
- Boot file name (used by the monitor's TFTP path on autoboot)
- MAC address (read-only display — derived from the NVRAM
  manufacturing block)
- Keyboard layout / country code (driven by a ~310-entry dispatch
  table in the tool's `.rodata`; see "Country table" below)
- Display / video-mode preferences

Each field has a `print current value → prompt → parse → validate →
store` cycle. Values are serialized into the 93C46 via a bit-banged
serial protocol at `0xAEC80000`, driven directly from the CPU
through a 7407 open-collector buffer (no on-board MCU); the
driver lives in the `fn_nvram_*` cluster.

## External calls into the monitor (shared helpers)

The Setup tool is *almost* self-contained but relies on a handful of
helpers that live in the monitor at VAs inside the `0xBED4xxxx` link
range but reached via a shared function-pointer table at
`0x0EC008xx`. All 33 `jalr` call sites in the Setup tool are
preceded within 15 instructions by `lui $X, 0xec0`, i.e. the tool
reads pointers out of the monitor's init-populated vtable and
dispatches through them. Entries observed include:

- `g_screen_out_vec`   at `0x0EC008C8` (console emit)
- `g_screen_out_vec2`  at `0x0EC008CC` (console emit, alt channel)
- `g_ui_func_table`    at `0x0EC00890` (line-edit / prompt)

This means the Setup tool cannot run in isolation — its UI is
painted using the monitor's DUART driver. That's intentional: the
monitor already knows which channel the console is on, the baud
rate, flow-control mode, etc.

Separately, 94 distinct `jal` immediates in the Setup tool target
addresses inside the erased `0x28000 – 0x37FFF` region of the ROM.
The dump itself is complete (the `-splice.u8` suffix only means
"even+odd halves of the two 128 KiB ROM chips merged"), so those
`jal`s land on real bytes — which are all `0xFF`. They are either
never executed in practice, reachable only via code paths the
disassembly hasn't mapped yet, or leftover stubs from an earlier
ROM revision where that region was populated. Either way, the
block is not "missing" — it is just blank in this revision.

## Country / locale dispatch table

The trailing `.rodata` of the Setup slice (slice offsets
`0x5428 – 0x5FF8`) holds ~310 function pointers of the form
`0x0ED4_XXXX` into the tool's own body, interleaved with 169
string-pointer words and zero-padding. Adjacent string constants
("Finnish", "Siemens German", "dd.dd.dd.dd", …) make it plain that
this is the **country / keyboard-layout dispatch table** driving
the locale menu in the UI.

An earlier pass read those data words as disassembled `jal
0xBB5xxxxx` instructions and reported them as "310 external calls
to a non-existent address." They are not instructions at all — the
current `nvram_setup.dis` treats that region as data, and
`FINDINGS.md` § "Correction — the 0xBB5xxxxx 'phantom'" walks
through why the earlier reading was wrong.

## Persisted layout on the 93C46

The 93C46 is a 1024-bit serial EEPROM (64 × 16-bit words). The
Setup tool's word layout is inferred from the field/offset pairs in
its read/write helpers; fully validating those offsets would require
dumping a configured NVRAM and diffing it against freshly-saved
values. That work is pending.

## What is *not* here

- No TFTP / boot logic — that is all in the monitor; the Setup tool
  only persists the *parameters* the monitor later consumes.
- No Ethernet init — the Setup tool never touches the LANCE.
- No video init — CRTC / video-control register writes are
  exclusively in the monitor. (The NCD15 display is 1 bpp ECL at
  1024×800@70 Hz; there is no RAMDAC or palette to program.)

## Where to read which piece

| Topic | Start here |
|---|---|
| Entry trampoline | `nvram_setup.dis` first function |
| Field UI loop | grep `fn_printf_` in the main body |
| 93C46 bit-bang | grep `fn_nvram_` |
| Shared monitor vtable calls | grep `jalr` and look for `0xec0` in the prior window |
| Country table (.rodata) | slice offsets `0x5428 – 0x5FF8` (pointers) and `0x4B78+` (strings) |
