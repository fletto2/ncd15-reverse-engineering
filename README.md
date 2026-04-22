# NCD15 Reverse Engineering

Reverse-engineering work on the **NCD15 X-terminal** (Network Computing
Devices, ~1991): a MIPS R3052-based diskless X11 workstation that boots
its firmware from ROM and fetches the X server (`Xncd15r`) over TFTP.
This repo contains annotated disassemblies of the boot monitor, NVRAM
setup tool, and X server, plus a minimal replacement X-server image
you can serve to confirm the deployment path.

## Repository layout

```
├── README.md                   ← you are here
├── FINDINGS.md                 ← canonical structural writeup
├── BUILD.md                    ← MIPS-I cross-toolchain build recipe
├── disasm/
│   ├── monitor.dis             ← NCD15 boot monitor (52K lines, 626 fns, 76% topic-tagged)
│   ├── nvram_setup.dis         ← standalone NVRAM-setup tool (5.7K lines, 97% topic-tagged)
│   └── Xncd15r.dis             ← X server (242K lines, 4800 fns, 79% topic-tagged, 13% ground-truth named)
└── xncd15r-mini/
    ├── tftpd.py                ← host-side TFTP server (RFC 1350)
    ├── xncd15r.bin             ← prebuilt 280-byte test image
    └── README.md               ← deploy walkthrough + monitor CLI reference
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

### Recipe

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

## Conventions in the disassembly

- Every renamed function keeps its original symbol and VA in a
  trailing `/* sub_VA @ 0xVA */` comment — every rename is reversible
  by regex.
- Inline `; → "string"` / `; → NamedFn` / `; → g_global` annotations
  on lui/jal/load lines carry the per-line semantic signal.
- Function headers: `; ---- <name> /* sub_VA @ 0xVA */ (N insns, M call sites)`.
- Ground-truth names (matching X11R5 / MIT source) have no prefix.
  Topic-tagged functions are `fn_<topic>_VA` where the prefix narrows
  the function's domain but a precise identifier has not been lifted.

## License

Disassemblies, annotations, writeups, and tooling in this repository
are released under the MIT License (see `LICENSE` if/when added). The
NCD15 firmware itself is copyrighted by its original vendors and is
**not** included here — only analytical output derived from it.
