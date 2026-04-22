# NCD15 Boot Monitor — operational description

The Boot Monitor is the firmware that runs when an NCD15 powers on. It
lives in the mask ROM at physical `0x0EC00000` (KSEG1 alias
`0xBFC00000`), initializes the hardware, drops to an interactive
prompt on the serial console if autoboot is disabled, and otherwise
TFTPs the `Xncd15r` X server into DRAM and jumps to it. This document
is a narrative walkthrough of what it does and where to find each
piece in `disasm/monitor.dis`.

## What it is

- MIPS-I big-endian, R3052 core (no MMU, no TLB).
- Linked at `0x0EC00000` (physical), with a small reset stub at the
  ROM-alias `0xBFC00000` that trampolines into RAM-linked code once
  DRAM and caches are live.
- ~32 K disassembled lines, 626 functions. After the topic-tagging
  pass, ~76% of function headers carry a domain hint
  (`fn_duart_*`, `fn_lance_*`, `fn_video_*`,
  `fn_memctl_*`, `fn_nvram_*`, `fn_net_*`, `fn_printf_*`, etc.); the
  `fn_ramdac_*` prefix left over from early passes is a misnomer —
  see "Video hardware" below.
  remainder are named after matching X11R5 / MIT sources or left as
  raw `sub_VA` helpers where no signal was available.

## Reset and early init (`0xBFC00000` → RAM)

The CPU enters at `0xBFC00000`. The first ~64 instructions do the
minimum needed to make DRAM usable, then jump out of the uncached
reset alias into the cached RAM-linked image:

```
00: Status = 0x00400000 (BEV)                ; stay on ROM exception vectors
08-18: *(0xFFFE0020) = 0x002BFF2D            ; memory-controller config
1c-28: *(0xFFFE0010) = 500                   ; DRAM refresh interval
30-40: jal 0xBFC00188 / 01E4 / 023C          ; three cache-init subs
48:   $sp = 0x0EC28000                       ; stack at top of RAM-linked range
64:   jr  0x1FC0006C                         ; uncached → cached transition
```

From `0x0EC0006C` onward the CPU is executing out of DRAM at the
linker's expected addresses. The second-stage init block at
`0x0EC00D80 – 0x0EC00ED0` is a straight-line sequence of 24 `sw`
instructions that populates a **function-pointer table** at
`0x0EC00800 – 0x0EC008FF`. Every major monitor service — console
emit, UI dispatch, boot driver, error path — is reached through one
of those 24 slots. The table is written exactly once, at init, and
is otherwise read-only from within the monitor; the NVRAM-setup
tool (see `NVRAM_SETUP.md`) calls *through* this table to reach
monitor I/O helpers without needing its own copies.

## Hardware initialization order

| Stage | Driver | Where |
|---|---|---|
| Memory controller | write `0xFFFE0020` / `0xFFFE0010` | reset stub |
| Caches | three init subs | `0xBFC00188 / 01E4 / 023C` |
| DUART (SCN2681/MC68681 @ `0xBE880000`, 4-byte stride) | `fn_duart_*` | top of `monitor.dis` DUART cluster |
| LANCE Ethernet (Am7990 @ `0xBE482000`) | `fn_lance_*` | register pair at `+0x2000/2/4/6` |
| Video control (`0xAF000000`, 1 bpp ECL — no palette / RAMDAC) | `fn_video_*` (also stale `fn_ramdac_*`) | pair-writes at `+0/+4` are video-timing / cursor-position registers, not an LUT |
| CRTC / video timing (`0xBE380000`) | `fn_video_*` | mode tables |
| NVRAM serial line (`0xAEC80000`, bit-banged 93C46 through a 7407 buffer directly from the CPU) | `fn_nvram_*` | see "Keyboard / NVRAM" note below |

Each `fn_<topic>_*` function was identified by the hardware base
address it stores to (extracted from `lui` high-halves in the body);
the topic prefix is present on the function header and also tagged
inline (`; >>> [R*]`) on every MMIO site.

### Video hardware

The NCD15 is a 1 bpp monochrome ECL display at **1024×800 @ 70 Hz**
on a 15" panel. There is **no RAMDAC, no palette, no color LUT**.
The frame buffer is a single-bit-per-pixel memory region; video
output is clocked out through discrete ECL driver logic. The
`0xAF000000` register pair that earlier passes labeled "Bt47x
RAMDAC" and `fn_ramdac_*` headers in the disassembly are video
control / timing registers, not a color-lookup device. A future
rename pass should retag those as `fn_vidctl_*`.

### Keyboard / NVRAM path

The NCD15 mainboard does **not** have a dedicated keyboard MCU.
The Intel i8749-class microcontroller that NCD uses for keyboard
scanning lives inside the physical keyboard, not on the mainboard.
The only keyboard-adjacent part on the PCB is a 7407 open-collector
hex buffer, with traces running directly to the LSI-branded CPU
(custom-marked R3052 + glue). Keyboard serial input and the
93C46-style NVRAM bit-bang protocol are both implemented by the
CPU driving/reading these lines through the 7407 — there is no
intermediate mainboard microcontroller.

## Monitor CLI

Once init completes, the monitor either autoboots (if NVRAM has a
valid `boot file`) or prints a `>` prompt on the DUART. Commands are
**two-character case-insensitive prefixes** followed by space-
separated arguments. The dispatch table is a small `strcmp` ladder
near the top of main.

| Cmd | Arguments | Effect |
|---|---|---|
| `BT` | `file local-IP host-IP [gateway] [mask]` | TFTP-boot: ARP → RRQ → copy blocks to `0x0ED00000` → `jr _start` |
| `PI` | `local-IP host-IP` | Ping host from local IP (proves LANCE + IP + ARP) |
| `DA` | (none) | Display current IP addresses / NVRAM values |
| `SE` | (none) | Enter NVRAM-setup UI (runs the separate tool at `0xBED40000`) |
| `DS` | (none) | Display booting statistics (RRQs, retries, bytes) |
| `DM` | `addr len` | Dump memory at virtual address |
| `RS` | (none) | Reset the terminal |

The full alias table is in `FINDINGS.md` § "Boot Monitor CLI command set".

## TFTP boot path

`BT file local host` performs a classic embedded-network boot:

1. Parse dotted-quad args; stash in NVRAM-independent scratch.
2. Program the LANCE with the local MAC (read from NVRAM) and bring
   the chip out of reset (`fn_lance_*` init sequence).
3. ARP-resolve `host` on the local wire.
4. Build and send a TFTP RRQ (opcode 1) for `file` to UDP port 69.
5. For each 512-byte DATA block, copy the payload to
   `0x0ED00000 + (block_no − 1) × 512`, send ACK, repeat until a
   short block terminates the transfer.
6. `jr 0x0ED00000`. The loaded image is entered in the same
   environment the monitor was running in: DUART initialized, LANCE
   up, caches on, `$sp` wherever the caller left it.

The ~150 KB of "external" call targets from monitor code into the
`0x0B0xxxxx` range are hard-coded compile-time references into the
X-server image — they resolve at runtime via the DRAM-aliasing memory
controller (0x03D00000 stride) onto `0x0ED0xxxx`. In other words,
several monitor code paths require `Xncd15r` to already be loaded.

## One-shot vs. persistent boot

Both invocations hit the same TFTP path; they differ only in where
the arguments come from.

- `BT file local host` — one-shot, zero NVRAM side-effects.
  Power-cycle restores the stock state.
- `SE` → save → `BT` — persistent. The NVRAM-setup tool writes the
  93C46 via the CPU's bit-banged serial line (through the 7407);
  subsequent power-ons autoboot from those stored values.

## Relationship to the NVRAM-setup tool

The monitor is one half of the firmware; the other half is the
standalone NVRAM-setup tool shipped in the same ROM at file offset
`0x38000`, linked at `0xBED40000`. The `SE` command in the monitor
transfers control into that tool; the tool calls back into the
monitor's 24-slot function-pointer table for console I/O. See
`NVRAM_SETUP.md` for details.

## Where to read which piece

| Topic | Start here |
|---|---|
| Reset → RAM trampoline | `monitor.dis` first function (`_start`) |
| Vtable population | `0x0EC00D80 – 0x0EC00ED0` (straight-line `sw` block) |
| DUART driver | grep `fn_duart_` |
| LANCE driver | grep `fn_lance_` |
| TFTP + IP + ARP | grep `fn_net_` |
| CLI dispatch | grep `fn_printf_` for the emit primitives, then walk back |
| Video control / CRTC init | grep `fn_video_` (and legacy `fn_ramdac_`) |
