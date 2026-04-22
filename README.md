# NCD15 Reverse Engineering

Reverse-engineering work on the **NCD15 X-terminal** (Network Computing
Devices, ~1991): a MIPS R3052-based diskless X11 workstation that boots
its firmware from ROM and fetches the X server (`Xncd15r`) over TFTP.
This repo contains annotated disassemblies of the boot monitor, NVRAM
setup tool, and X server, plus a minimal replacement X-server image
you can build + deploy yourself to confirm the deployment path.

**Tooling story**: the disassembly annotations were built with a Monte
Carlo pipeline (see `MONTECARLO.md`) combining static analysis, rodata
table-structure scanning, X11R5 source-order matching, and a
two-member **Council of Clankers** (DeepSeek + GLM) for gated
name-lifting. The monitor is ~89% line-identifiable with 99.5%
independent-oracle precision; the X server is 63% function-named from
a starting point of 0%.

## Repository layout

```
├── README.md                   ← you are here
├── FINDINGS.md                 ← canonical structural writeup + 25-round MC trail
├── BUILD.md                    ← MIPS-I cross-toolchain build recipe
├── MONTECARLO.md               ← MC scoring procedure + Council of Clankers loop
├── disasm/
│   ├── monitor.dis             ← NCD15 boot monitor (52K lines, 622 fns, ~20K annotations)
│   ├── nvram_setup.dis         ← standalone NVRAM-setup tool (5.7K lines)
│   └── Xncd15r.dis             ← X server (242K lines, 4800 fns, 63% named)
├── xncd15r-mini/               ← 280-byte replacement X-server image
│   ├── start.S  main.c  link.ld  Makefile
│   ├── tftpd.py                ← host-side TFTP server (RFC 1350)
│   ├── xncd15r.bin             ← prebuilt test image
│   ├── xncd15r.elf / .dis      ← ELF with debug + its own disassembly
│   └── README.md               ← full build + deploy walkthrough
└── tools/
    └── mc_coc_loop.py          ← the Monte Carlo / Council-of-Clankers name-lifter
```

## Hardware context

| Component | Address |
|---|---|
| MIPS R3052 CPU | (no-MMU, no-TLB) |
| Boot ROM | 0xBFC00000 (phys 0x0EC00000) |
| DRAM (X-server load) | 0x0ED00000 (4 MB, aliased every 0x03D00000) |
| MC68681 DUART (console) | 0xBE880000, 4-byte register stride |
| Am7990 LANCE Ethernet | 0xBE482000 |
| Bt45x-family RAMDAC | 0xAF000000 / 0xAEC80000 |
| NVRAM (likely 93C46) | 0xBFA00000 / 0xBFB00000 |
| Memory controller | 0xFFFE0000 |

Details and derivation in `FINDINGS.md`.

---

## Quick test: boot a custom X-server image

This is the end-to-end smoke test that proves the TFTP download path,
entry-point convention, and DUART console init — without touching
anything in NVRAM.

### Prerequisites

- An NCD15 on the same Ethernet segment as your host.
- Serial console on the NCD15's DUART (9600 8N1 works).
- Ability to bind UDP/69 on the host (needs root or capability).
- MIPS-I cross-toolchain for building from source. Or just use the
  prebuilt `xncd15r-mini/xncd15r.bin` (280 bytes). Toolchain recipe
  is in `BUILD.md`.

### Recipe (uses prebuilt image)

1. **Serve the image**:
   ```bash
   cd xncd15r-mini
   sudo python3 tftpd.py . --bind 0.0.0.0
   ```
   Leave it running. Log goes to stdout — every RRQ and block transfer.

2. **Break into the monitor prompt** on the NCD15 (stop autoboot via
   the serial console, or pull the network cable during power-up).

3. **Tell the monitor to TFTP-boot it**:
   ```
   > BT xncd15r.bin <local-ip> <host-ip>
   ```
   Replace `<local-ip>` (NCD15's IP) and `<host-ip>` (your TFTP host).
   For a persistent configuration that autoboots on power-up, use
   `SE` to enter NVRAM-setup instead; see `xncd15r-mini/README.md`.

4. **Watch the console**:
   ```
   === custom Xncd15r running at 0x0ED00000 ===
   hello from MIPS-I bare metal
   ```

   Both DUART channels (A and B) get the banner.

If it fails: full debug table in `xncd15r-mini/README.md` § "Debugging
a failed boot".

### Recovery

Zero persistent side-effects from a one-shot `BT` boot. Power-cycle =
restored to stock firmware. NVRAM only changes if you ran `SE`.

---

## Running the Monte Carlo / Council of Clankers loop yourself

```bash
# One-time: provide API keys
export DEEPSEEK_API_KEY=sk-...                  # platform.deepseek.com
export GLM_API_KEY=...                          # z.ai

# 10 cycles on disasm/Xncd15r.dis (~20s per cycle per clanker, parallel)
cd tools
python3 mc_coc_loop.py 1 10
```

Each cycle:

1. Samples 20 unnamed `sub_*` functions (8–400 insns, ≥1 inline
   annotation) from the disassembly.
2. Sends first 60 instructions + inline annotations to both clankers.
3. Parses the `fn<N>: <identifier> | <purpose>` responses.
4. Accepts names where: (a) both clankers propose the same normalized
   name, OR (b) one clanker's rationale contains ≥2 domain keywords
   from the X11/cfb/mfb/os/wm vocabulary.
5. Applies accepted lifts to the `.dis`, preserving the `sub_VA @ 0xVA`
   trailer in the header comment. Backs up the pre-cycle state.
6. Propagates new names through inline `→` annotations.

Full protocol, calibration notes, consensus-rule tuning, and false-
positive audit procedure: `MONTECARLO.md`.

---

## Status at publication

| Artifact | Size | Named / total | Line-level honest score |
|---|---|---|---|
| `monitor.dis` | 52 219 lines, 622 fns | — | **89%** (frozen R21 scorer, median of 7 seeds) |
| `Xncd15r.dis` | 242 000 lines, 4800 fns | **627 gt + 2413 topic = 3040 / 4800 (63.3%)** | 76% (with topic credit), 13% (ground-truth-only) |

Independent oracle on the monitor: 99.5% precision, 93.9% recall.

The Xncd15r gap to close: remaining ~1760 `sub_*` fns. The Council of
Clankers loop averages ~5 accepted names per cycle with a tight
consensus gate — cheap to iterate but slows down as pool-remaining
shrinks. Structural lifts (rodata tables) consistently outperform:
`lift_procvector.py`, `lift_reply_swap.py`, `lift_gcops.py`, and
`lift_gcfuncs.py` together placed ~240 ground-truth names.

## Conventions in the disassembly

- Every renamed function keeps its original symbol and VA in a
  trailing `/* sub_VA @ 0xVA */` comment — every rename is reversible
  by regex.
- `; >>> [RN] note` lines are Monte Carlo round N annotations
  (monitor only). Cumulative total ~20,000.
- Inline `; → "string"` / `; → NamedFn` / `; → g_global` annotations
  on lui/jal/load lines carry the per-line semantic signal used by
  the scorer.
- Function headers: `; ---- <name> /* sub_VA @ 0xVA */ (N insns, M call sites)`.
- Topic-tagged functions are `fn_<topic>_VA`; ground-truth names have
  no `sub_`/`fn_` prefix.

## License

Disassemblies, annotations, writeups, and tooling in this repository
are released under the MIT License (see `LICENSE` if/when added). The
NCD15 firmware itself is copyrighted by its original vendors and is
**not** included here — only analytical output derived from it.
