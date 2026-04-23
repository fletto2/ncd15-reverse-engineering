# Emulator Errata — 2026-04-23

Building a working C emulator (`emulator/`) uncovered several
incorrect claims in the pre-emulator docs (FINDINGS.md, MONITOR.md,
NVRAM_SETUP.md, NEW_INSIGHTS.md, README.md, BT_LOADER.md). This
file collects the corrections in one place so the source of truth
isn't scattered. Each entry cites the pre-emulator claim, states
what's actually true, and points to the emulator code or disasm
address that confirms it.

## 1. Shadow RAM copy is from ROM+0x2000, not ROM+0x4000

**Old claim:** `dump_monitor.py` and `monitor.dis` treated the
main-monitor image as `ROM[0x4000..0x28000]` linked at VA
`0x0EC00000`. The region `ROM[0x2000..0x3FFF]` was labeled a
"RAM-copy trampoline".

**Correction:** The monitor's reset code at `sub_0xBFC00AB0` copies
the image linearly from `ROM[0x2000..0x28000]` into DRAM at phys
`0x0EC00000`. Confirmed bit-exact: `dram[0x2720..0x272C]` after
the copy matches `ROM[0x4720..0x472C]`, and `dram[0..0x2C]`
matches `ROM[0x2000..0x202C]`. Total copy size ≈ 0x26000 bytes
(152 KiB).

**Consequence:** Every VA label in the main-monitor section of
the original `monitor.dis` was shifted **−0x2000** from reality.
Example: what the original dis labels `0x0EC02724` (an `afb20018;
move; afb10014; …` move-style prologue) is actually VA `0x0EC00724`
with the real instruction `lbu $v0, 0($a0)` — the last insn of a
3-instruction bit-bang byte-reader. `dump_monitor.py` was fixed
with `MON_START = 0x02000`; the regenerated dis is the one the
emulator correlates against. `nvram_setup.dis` is unaffected
(setup tool is a separate image loaded at `0xBED40000`).

## 2. NVRAM is bit-banged via DUART, not a standalone MMIO

**Old claim:** NVRAM sits at `0xAEC80000` (CLAUDE.md / FINDINGS)
or `0xBFA00000` / `0xBFB00000` (README / NEW_INSIGHTS). Some docs
labeled 0xAEC80000 as the "keyboard MCU mailbox" or the "93C46
bit-bang port".

**Correction:** None of those addresses are the NVRAM chip. The
93C46 is wired to the DUART's output port pins, and the CPU
bit-bangs it through standard DUART register writes at `0xBE880000`:

| NVRAM pin | DUART connection |
|---|---|
| DI | OPR (reg 0x0F, offset 0x3C) bit 4 = OP4 |
| CS | OPR bit 5 = OP5 (held high through operation) |
| SK | OPR bit 6 = OP6 |
| DO | IPCR (reg 0x04, offset 0x12) bit 2 = IP2 |

Confirmed via:
- `sub_0ec04118`: arg-to-OPR-bit mapping (a0→OP6, a1→OP5, a2→OP4)
- `sub_0ec0418c` SK-pulse structure: toggles $a0 (OP6) across
  phases while holding $a1 (OP5) and $a2 (OP4) constant — SK is
  the thing that pulses, so OP6=SK.
- `sub_0ec04100`: `lbu $v0, 0x12($at); andi $v0, 4` — reads IPCR
  byte and masks bit 2, so DO=IP2.
- `sub_0ec041f4` argument flow: $a0 (SK)=0, $a1 (CS)=1, $a2
  (DI)=data — matches the 93C46 protocol (CS high, data on DI,
  clocked by SK pulse).

Additional protocol detail:

- **DO clocked LSB-first**, not MSB-first. NM93C46 datasheet
  shows MSB-first; on this board the monitor's receive-bit
  accumulator builds up each 16-bit data word from the LSB. If
  you emulate MSB-first, MAC bytes come back bit-reversed (0x02
  → 0x40).
- **Reg 0x0F on this board is a direct OPR write**, not the
  SCN2681 spec's "Set Output Port bits" semantics. The monitor
  never writes to reg 0x1F (Clear Output Port).
- **DO reads as logic 1 when the chip isn't driving it** (7407
  open-collector pull-up). The size-probe at `sub_0ec04474`
  relies on this — if DO reads as 0 when idle, the probe exits
  at iteration 1 with an out-of-range size and the monitor
  prints "PANIC -- Need to initalize NVRAM first".

## 3. BEV is cleared by shadow code; exceptions vector to 0x80000080

**Old claim:** "Status stays at `0x00400000` (BEV) for the
monitor's lifetime" (CLAUDE.md / MONITOR.md / NEW_INSIGHTS).

**Correction:** After shadow entry, Status settles at
`0x0000a001` (IEc + IM5 + IM7). BEV is clear, so the exception
vector is **`0x80000080`**, not `0xBFC00180`.

But: **no code anywhere in the 256 KB ROM ever installs a handler
at `0x80000080`** — verified by exhaustive scan for `sw`/`sh`/`sb`
instructions with the right target address. The monitor is fully
polling-driven and never actually takes an interrupt in normal
operation. If interrupts are somehow triggered (e.g. by an
emulator firing IP5 manually), the CPU vectors to `0x80000080`
and starts executing NOP words (dram[0x80..] is zero), wastes
millions of cycles running through DRAM, and eventually hits
real code by wrapping — definitely not the intended behavior.

## 4. Monitor console is DUART channel B, not A

**Old claim:** Some docs label channel A as the console.

**Correction:** The monitor polls DUART channel B throughout:
`sub_0ec07cb4` (blocking polling read) reads SRB at `0xBE880026`
(register 9) for RxRDY, then reads RHRB at `0xBE88002E` (register
11) for the byte. Channel A's registers are touched for config
but never for TX/RX.

## 5. NVRAM layout: MAC at bytes 2..7

**Old state:** NVRAM layout was unknown beyond "128 bytes".

**Correction:** The 128-byte 93C46 image, interpreted as 64
little-endian 16-bit words:
- bytes 0..1: unknown (likely version or magic)
- **bytes 2..7: Ethernet MAC address (6 bytes, in-order)**
- bytes 8+: other fields not yet mapped — IP, subnet mask,
  gateway, boot-server info all live somewhere here but the
  boot-time copy path for them isn't fully traced. The `SE`
  setup tool would presumably handle them interactively, but
  `SE` hangs in the emulator (it loads an ECOFF sub-tool from
  `ROM+0x38000` that we haven't fully wired up).

Determined empirically: a probe NVRAM image with bytes 0..127
produced `DA` output `MAC = 02:03:04:05:06:07`.

## 6. `data_0x0EC008D8` never initialized

**Correction:** The NVRAM-read function pointer at
`data_0x0EC008D8` is never written by any `sw`/`sh`/`sb`
instruction in the entire ROM. The monitor's checksum code at
`sub_0ec04818` does `jalr $v0` with $v0 loaded from this null
pointer. With a `jr $ra; nop` safety stub at phys 0 (one of the
emulator's hacks), the call returns harmlessly; on real hardware
this would crash, so the code path is evidently unreachable under
normal boot conditions. MAC is loaded into `data_0x0EC000F8+` via
a separate path; the in-memory NVRAM image at `data_0x0EC00C38+`
stays blank after boot (visible via `NV D` — it shows 128 zeros).

## 7. `0xBE200000` is a bus-ready handshake

**Correction:** This address was previously "unknown MMIO, heavy
polling". It's actually a bus-ready register: `sub_0ec173f8`
polls bit 0 before every MMIO transaction (called 8 sites,
including from the DUART + LANCE access helpers). Returns as
always-ready on real HW (7407 pull-up — same idiom as NVRAM DO
idle state).

## 8. Tick counter is at `data_0x0EC00730`, not `data_0x0EC006A8`

**Old claim:** `monitor.dis` labels `lw $v0, 0x6a8($s1)` (at
0x0EC0362C in the corrected dis) as reading `data_0x0EC006A8`.

**Correction:** The dis-annotator assumed $s1=0 to compute the
data reference. At runtime $s1=`0x0EC00088`, so the actual
effective address is `0x0EC00088 + 0x6a8 = 0x0EC00730`. This
address has 38 READERS and **zero WRITERS** in the entire
disassembly — its updater lives in the never-installed
`0x80000080` interrupt handler. The emulator auto-increments
this address on shadow-resident reads to satisfy the delay
loops.

## 9. Input read path has two modes; monitor takes the dead one by default

**Correction:** `sub_0ec083e0` selects between two input paths
based on `data_0x0EC01440` (halfword flag):
- flag != 0 → polling DUART read via `sub_0ec07cb4` (works)
- flag == 0 → ring-queue read (head at `data_0x0EC0056C`, tail
  at `data_0x0EC00570`)

`data_0x0EC01440` has 5 readers and zero writers in the
disassembly — another ISR-managed global. The ring queue's fill
code lives in the never-installed `0x80000080` handler, so on
our emulator the default code path is dead. Forcing this flag to
1 (emulator hack) makes the polling path active and stdin input
works.

## 10. CRTC sync loop polls byte 0 bit 1, at address `0x0EC03E18`

**Old claim:** "CRTC vsync polling on bit 8 of `0xBE380013`"
(multiple docs).

**Correction:** There are two separate poll patterns. The boot
sync loop at **`0x0EC03E18`** (in the corrected dis) does:

```
jal sub_0ec028fc      ; → LANCE-helper that reads CRTC byte 0
andi $v0, $v0, 2      ; mask bit 1
beqz $v0, 0xec03e18   ; spin until bit 1 is SET
...
andi $v0, $v0, 2
bnez $v0, 0xec02e2c   ; then spin until bit 1 is CLEAR
```

It's an edge-detect on bit 1 of CRTC byte 0, not bit 8 of
offset 0x13. The offset-0x13 poll is a different, later loop.
Emulator stub toggles bit 1 on a slow clock to unblock this.

## 11. DRAM / shadow / VRAM are SEPARATE memory banks

**Old claim:** "4 MiB DRAM... aliased every 0x03D00000". Often
loosely interpreted as a single aliased bank covering everything
below `0x10000000`.

**Correction:** On real hardware there are at least three
distinct banks:
- **Main DRAM** at phys 0 (4 MiB, aliased)
- **Shadow** at phys `0x0EC00000` (backs monitor code + stack)
- **VRAM** at phys `0x0F000000` (1 MiB framebuffer)

The emulator's original implementation aliased everything
through `pa & 0x3FFFFF`, which caused the monitor's VRAM memtest
(writes 0x5A5A5A5A through `0xAF000000+`) to trash shadow code.
Splitting into three separate backing stores fixed it. On real
HW these are genuinely distinct DRAM/VRAM chips with independent
address decoders.

## 12. Reset sequence: KUSEG trampoline, not direct shadow jump

**Small correction:** The reset code at `0xBFC00000` doesn't jump
directly to `0x0EC00000`. It:

1. Does cache + memctl init at `0xBFC00xxx` (uncached KSEG1).
2. `jr $t0` with `$t0 = 0x1FC0006C` (`0xBFC0006C & 0x1FFFFFFF`) —
   same ROM content, accessed via KUSEG cached alias.
3. Runs the rest of early init from KUSEG cached ROM through the
   memtest.
4. Calls the copy routine at `sub_0xBFC00AB0` which copies
   `ROM[0x2000..0x28000]` to shadow DRAM at phys `0x0EC00000`.
5. *Then* `jr $a0` with `$a0 = 0x0EC00000` lands execution in
   shadow.

The `ncd15-emu` trace pinpoints this: the shadow-enter event at
pc=`0x0EC00000` fires at cycle ~6,396,072 with prev_pc
=`0x1FC00A8C`.

---

## Applies to / supersedes

These corrections apply to the following pre-emulator docs:

- **FINDINGS.md** — shadow-RAM source (§1), NVRAM topology (§2),
  memory banking (§11), CRTC (§10), BEV (§3).
- **MONITOR.md** — shadow copy offset (§1), BEV (§3), channel B
  (§4).
- **NVRAM_SETUP.md** — bit-bang topology (§2), layout (§5).
- **NEW_INSIGHTS.md** — BEV (§3), reset sequence (§12).
- **README.md** — NVRAM MMIO addresses (§2), DRAM split (§11).

Where any of those conflicts with the above, this file wins.
