# xncd15r-mini — custom boot image for the NCD15

End-to-end recipe for building a MIPS-I bare-metal image, serving it
over TFTP, and telling the NCD15 boot monitor to load and run it.

## What this image does

`main.c` polls the MC68681 DUART and writes a banner to **both**
serial channels (A and B), then spin-loops:

```
=== custom Xncd15r running at 0x0ED00000 ===
hello from MIPS-I bare metal
```

`start.S` is the entry stub at load address `0x0ED00000`: sets
`$sp = 0x0ED3FF00`, calls `main`, halts on return. No interrupts,
no cache init — relies on the monitor's environment as-is.

Final image is 280 bytes (`xncd15r.bin`). ELF with debug is 5.3 KB
(`xncd15r.elf`). Full disassembly in `xncd15r.dis`.

## Prerequisites

- Cross-toolchain at `/opt/cross/mips-elf/` (binutils 2.42, gcc 13.2.0,
  bare-metal `mips-elf`). Rebuild recipe: `../../../BUILD.md`.
- Host on the same Ethernet segment as the NCD15, reachable from it.
- Serial console on the NCD15's DUART port (9600 8N1 typical) — the
  banner will print there. Without a console you can still verify by
  watching TFTP-server logs for the RRQ.

## Build

```bash
cd ~/src/claude/ncd15/ncd15-toolchain/xncd15r-mini
make                   # -> xncd15r.bin, xncd15r.elf, xncd15r.dis
```

First word of `xncd15r.bin` must be `3C 1D 0E D3` (`lui $sp, 0x0ED3`,
big-endian). If not, the toolchain isn't `-EB`.

## Serve over TFTP

`tftpd.py` is a plain RFC 1350 server (+ options, + an FS/1 helper port
the NCD15 doesn't use). Fetched from the Pi; same file that served the
3Com board.

```bash
# bind 0.0.0.0:69 on the host, serve this directory
sudo python3 tftpd.py . --bind 0.0.0.0
```

Port 69 needs root. `--port 6969` works for testing but the NCD15's
TFTP client talks to 69 only, so in practice: root.

## Drive the NCD15 monitor

If autoboot is running, hit the serial console's stop key (or remove
the network cable, power-cycle, reinsert) to break into the `>` prompt.

Two-char prefix commands, case-insensitive. Relevant ones:

| Cmd | What to type | Purpose |
|---|---|---|
| `PI` | `PI 192.168.1.50 192.168.1.15` | Ping host from `me ho` — proves LANCE + IP + ARP |
| `DA` | `DA` | Display current addresses (confirms NVRAM is sane) |
| `SE` | `SE` | NVRAM setup UI — set local IP, host IP, gateway, mask, boot file |
| `BT` | `BT xncd15r.bin 192.168.1.50 192.168.1.15` | Boot via TFTP: `file local-IP host-IP [gateway] [mask]` |
| `DS` | `DS` | Booting statistics — TFTP RRQs, retries, bytes |
| `DM` | `DM 0ED00000 40` | Dump memory — verify image landed at load address |
| `RS` | `RS` | Reset the terminal |

Full command table in `../../FINDINGS.md` § "Boot Monitor CLI command set".

### One-shot boot (no NVRAM change)

```
> BT xncd15r.bin 192.168.1.50 192.168.1.15
```

The monitor: resolves ARP → sends TFTP RRQ for `xncd15r.bin` to the
host → copies blocks to `0x0ED00000` → jumps to `_start`. Watch the
console for the banner.

### Persistent boot (stored in NVRAM)

```
> SE
  [enter IPs and "xncd15r.bin" as boot file; save; exit]
> BT
```

After that a power-cycle autoboots from NVRAM.

## Debugging a failed boot

Symptom → first check:

- **No RRQ reaches the host** → `PI` the host from NCD15; if ping
  fails it's IP/gateway/mask/cable. Check `DA`.
- **RRQ seen, TIMEOUT** → firewall on host blocking UDP/69 reply, or
  `tftpd.py` isn't bound to the right interface (`--bind 0.0.0.0`).
- **Transfer succeeds, no banner** → image landed but crashed. `DM
  0ED00000 40` and compare to `xncd15r.dis` first-16-words. If they
  don't match, the monitor's fixup step mangled something (shouldn't —
  we emit raw binary).
- **Banner truncated** → wrong channel: monitor may have initialized
  only A or only B. `main.c` writes to both, so this shouldn't happen,
  but if it does, check which channel the console cable is on.

## Recovery

Zero persistent side-effects. Power-cycle = fully recovered. NVRAM
only changes if you used `SE`; that's undoable by another `SE`.

## Files

```
start.S          entry + sp setup + call main + halt
main.c           DUART putc(A) + putc(B) + banner
link.ld          elf32-bigmips, load at 0x0ED00000, 4 MB region
Makefile         build rules (mips-elf-gcc flags + objcopy + objdump)
tftpd.py         RFC 1350 TFTP server (host-side)
xncd15r.bin      final image to serve (280 bytes)
xncd15r.elf      linked ELF with debug info
xncd15r.dis      disassembly for verification
```

## Hardware register reference

DUART MC68681 @ `0xBE880000`, 4-byte register stride:

| Offset | Register | Used here |
|---|---|---|
| 0x06 | SRA (channel A status) | bit 2 = TxRDY |
| 0x0C | THRA (channel A TX hold) | write byte to send |
| 0x26 | SRB (channel B status) | bit 2 = TxRDY |
| 0x2C | THRB (channel B TX hold) | write byte to send |

Monitor has already initialized the DUART (baud, parity, fifo mode)
before jumping to `0x0ED00000`, so `main.c` only has to poll+write.
No init required.

## See also

- `../BUILD.md` — toolchain build recipe + compile flags reference
- `../FINDINGS.md` — full RE writeup including monitor CLI table
