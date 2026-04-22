# Xncd15r — operational description

`Xncd15r` is the X server that the NCD15 boot monitor TFTPs into DRAM
and jumps to. It is a full X11R5 server, statically linked with Xt
Intrinsics and the NCD-specific client binaries (window manager,
xterm, telnet, login, setup, stats), packaged as a single ~1.9 MB
MIPS-I ECOFF image. Once it has the display, everything the user
sees on the NCD15 is running inside this one binary.

Disassembly: `disasm/Xncd15r.dis` (~242 K lines, ~4800 functions,
79% of headers topic-tagged or ground-truth-named).

## File layout

```
file 0x00000-0x0013f  header (file hdr + 0x38-byte AOUT opthdr + 6 section hdrs)
file 0x00140-0x196b6f .text    va 0x8ED00000  size 0x196a30   (1.58 MB)
file 0x196b70-0x19b4ff .rdata  va 0x8EE96A30  size 0x04990
file 0x19b500-0x1d69cf .data   va 0x8EE9B3C0  size 0x3b4d0
file 0x1d69d0-0x1e0000 .sdata  va 0x8EED6890  size 0x09630
(no scnptr)             .sbss  va 0x8EEDFEC0  size 0x011b0
(no scnptr)             .bss   va 0x8EEE1070  size 0x158e0
entry = 0x8ED00000       (= .text start; first word is the boot vector)
magic 0x0160 (MIPS-I BE ECOFF), vstamp 523, opt magic 0x0107
```

Loadable sections are virtually contiguous starting at KSEG0 VA
`0x8ED00000` (= physical `0x0ED00000`). The monitor loads the
raw file minus the 0x140-byte header to DRAM and does `jr
0x0ED00000`.

## Entry and boot path

The first word of `.text` is the boot vector: set up `$sp`, clear
`.bss`, call the server's main. No relocation — the image is linked
for its exact load VA and relies on the DRAM-alias memory controller
to place it there.

The monitor itself has **209 hard-coded `jal`s** into the
`0x0B0xxxxx` DRAM alias that resolve at runtime to Xncd15r entry
points (`+0xF2B0`, `+0x33960`, `+0x350F0` account for ~73% of
them). Those calls only work once Xncd15r has been loaded — the
monitor and the server are tightly coupled through the
`0x03D00000`-stride DRAM alias even after the jump.

## What is inside

`Xncd15r` is not a minimal X: it is a complete X11R5 DIX/DDX server
with full Xt Intrinsics plus the NCD terminal-specific clients
linked in.

Subsystems visible in the disassembly (sampled via string pools,
MMIO hits, and call-graph topic tagging):

| Bucket | Evidence |
|---|---|
| X server DIX core | ProcVector / SwappedProcVector / ReplySwapVector dispatch tables; GCOps / GCFuncs vtables |
| MI layer | fb/mfb primitives; GC ops; region/pixmap helpers |
| Xt Intrinsics | 29 `Xt*` public names in error strings; 23 type converters; 24 resource class names |
| Xlib (client side) | 21 API / resource tokens; env-var names; protocol names |
| X protocol errors | all 17 `Bad*` names present |
| Keysyms | full internal integer tables (350 tight clusters); names deferred to external `XKeysymDB` |
| NCDwm | window manager |
| xterm / NCDterm | terminal emulator |
| xtelnet | telnet client |
| NCDlogin / NCDconsole / NCDsetup / NCDstats | NCD-specific UI apps |
| LAT / TSSnet | optional licensed protocols (copyright banners present) |
| TFTP / MOP / NFS client | boot-file fetch for fonts, configs, RGB |
| LANCE / IP / TCP | bottom of the network stack |

Specific artifacts from the renaming pipeline that are worth
grepping in `Xncd15r.dis`:

- `ProcVector`, `SwappedProcVector`, `ReplySwapVector` — the three
  top-level X protocol dispatch tables.
- `cfbGCOps`, `mfbGCOps`, `cfbGCFuncs`, `mfbGCFuncs` — the X11
  graphics-context vtables (21 + 7 slots per `gcstruct.h`), lifted
  at `0x8EEBAA30` / `0x8EEBAC08`.
- `fn_font_*`, `fn_dix_*`, `fn_xtrans_*`, `fn_xt_*`, `fn_wm_*`,
  `fn_term_*` — topic prefixes from caller-propagation and
  string-signal classifiers.

Roughly 13% of function headers are ground-truth names lifted from
public X11R5 / MIT source (where enough inline string or call-shape
evidence was present); another 66% carry a topic prefix narrowing
the domain; the remaining 21% are still raw `sub_VA` — mostly
orphan helpers with only indirect-call sites.

## External data dependencies

`Xncd15r` does not ship every resource it needs. It reaches out over
the network (or flash, if the optional PC Card is installed) for:

- `/usr/lib/X11/ncd/XKeysymDB` — human-readable keysym names
- `/usr/lib/X11/ncd/XErrorDB` — extended error strings
- Fonts (all of them — only the server's internal cursor font is
  built in)
- `rgb.txt` color database
- `NCD_STD.DAT` defaults blob
- Per-user / per-terminal config files

The string `Op_flash_screen` / `Compressed Flash server…` /
`NCD Ethernet Option Card` and the `LOCAL / MOP / NFS / TFTP`
boot-priority order all point at a 4 MB PC Card slot that holds an
uncompressed Xncd15r image plus the font/config payload, for sites
that want zero-network boot. The ROM contains no decompression
routines, so whatever is on the card is stored uncompressed.

## Hardware it drives directly

Once initialized, the server owns the display and most of the I/O
devices. The monitor's drivers are effectively dead after hand-off.

| Region | Role | Notes |
|---|---|---|
| `0xBE380000` | CRTC / video timing | mode setup at server init |
| `0xAF000000` / `0xAEC80000` | RAMDAC (Bt47x family) | pair-write LUT loads, cursor colors |
| Framebuffer VA | pixel store | authoritative address is in the ECOFF layout — not portable from the 68k siblings |
| `0xBE482000` | LANCE Am7990 | TCP/UDP/ARP, driven by the server's own stack |
| `0xBE880000` | DUART SCN2681/MC68681 | used for `NCDconsole` and as a diagnostic channel |
| `0xAEC80000` | keyboard MCU | reads keyboard/mouse events |

The server has its own LANCE driver — it does not call back through
the monitor's network code. The monitor's TFTP client is only used
to *load* the server; once the server is up, all subsequent TFTP,
NFS, and MOP traffic runs through the server's stack.

## Request dispatch

X11 protocol dispatch is table-driven. The three vectors
`ProcVector`, `SwappedProcVector`, and `ReplySwapVector` are
128-entry arrays of function pointers indexed by the X11 request
opcode. The core server loop reads a request from the client
socket, indexes the vector by `opcode`, and tail-calls into the
handler. Request *names* are not in the binary (dispatch is by
integer index, not by string), so when grepping the disassembly
for `CreateWindow` / `MapWindow` / etc. you will find their entries
by vtable offset, not by symbol.

GC (graphics context) operations dispatch through per-depth vtables
(`cfbGCOps` for color framebuffer, `mfbGCOps` for monochrome). Each
has the canonical X11R5 21-slot layout: `FillSpans`, `SetSpans`,
`PutImage`, `CopyArea`, `CopyPlane`, `PolyPoint`, `PolyLines`,
`PolySegment`, `PolyRectangle`, `PolyArc`, `FillPolygon`,
`PolyFillRect`, `PolyFillArc`, `PolyText8`, `PolyText16`,
`ImageText8`, `ImageText16`, `ImageGlyphBlt`, `PolyGlyphBlt`,
`PushPixels`, `LineHelper`.

## NCD clients bundled in

These "clients" are not separate processes — they are linked
directly into the server binary and invoked as C function entries.

- `NCDwm` — window manager; grep `fn_wm_*`
- `NCDterm` / xterm-like — grep `fn_term_*`
- `xtelnet` — grep `telnet` / `NVT`
- `NCDlogin`, `NCDconsole`, `NCDsetup`, `NCDstats` — UI apps;
  grep their string tokens

Because they all share the server's address space, the NCD15 never
needs to fork/exec or run external client binaries — which it
couldn't, because there is no filesystem to run them from.

## Relationship to the rest of the firmware

```
Boot Monitor (ROM @ 0x0EC00000)
    │
    ├── CLI: `BT Xncd15r local host`
    │
    ├── TFTP download → copy to 0x0ED00000
    │
    └── `jr 0x0ED00000`
            │
            ▼
       Xncd15r (DRAM @ 0x0ED00000, ECOFF linked @ 0x8ED00000 KSEG0)
            │
            ├── takes over LANCE, CRTC, RAMDAC, keyboard MCU
            ├── calls back into the monitor via 209 hard-coded jals
            │   (compile-time references through the DRAM alias)
            └── runs the X server + NCDwm + NCD apps forever
```

## Where to read which piece

| Topic | Start here |
|---|---|
| Boot vector / server main | first few hundred insns at `0x8ED00000` |
| Protocol dispatch | grep `ProcVector`, `SwappedProcVector`, `ReplySwapVector` |
| GC vtables | grep `cfbGCOps`, `mfbGCOps`, `cfbGCFuncs`, `mfbGCFuncs` |
| Xt Intrinsics | grep `fn_xt_` and `cvtStringTo…` |
| Window manager | grep `fn_wm_` |
| Terminal emulator | grep `fn_term_` |
| LANCE driver | grep `fn_lance_` |
| IP/TCP | grep `fn_net_` |
| Fonts / font server | grep `fn_font_` |
| Keyboard / keysyms | grep `fn_kbd_`, `XK_` |
