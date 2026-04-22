# xncd15r-mini — custom boot image for the NCD15

End-to-end recipe for serving a custom MIPS-I bare-metal image over TFTP
and telling the NCD15 boot monitor to load and run it. Use this to
confirm that your host, cable, IP config, and the NCD15's TFTP client
all agree before investing in anything larger.

## What this image does

The prebuilt `xncd15r.bin` (476 bytes total: 196-byte ECOFF header +
280-byte payload) polls the MC68681 DUART and writes a banner to
**both** serial channels (A and B), then spin-loops:

```
=== custom Xncd15r running at 0x0ED00000 ===
hello from MIPS-I bare metal
```

Entry at load address `0x0ED00000`: sets `$sp = 0x0ED3FF00`, calls the
main loop, halts on return. No interrupts, no cache init — relies on
the monitor's environment as-is. The first four bytes of the **payload**
(after the ECOFF header) are `3C 1D 0E D3` (`lui $sp, 0x0ED3`,
big-endian MIPS-I).

## Why the ECOFF wrapper?

The NCD15 boot monitor's `BT` command does **not** accept flat binary
images. Its TFTP-boot path is an ECOFF loader (magic `0x0160`, aout
magic `0x0107`, `f_opthdr==0x38`, `f_nscns ∈ [3..7]`, first section
must be STYP_TEXT). A raw `objcopy -O binary` blob is rejected by the
loader before any code runs. This directory therefore ships an
**ECOFF-wrapped** `xncd15r.bin`, and a tool (`ncd15-ecoff-wrap`, also
installed at `/opt/cross/mips-elf/bin/ncd15-ecoff-wrap` by the
toolchain recipe) that takes a raw MIPS blob and emits the minimum
ECOFF the monitor is happy with:

```bash
mips-elf-gcc $CFLAGS -T link.ld -o out.elf start.S main.c
mips-elf-objcopy -O binary out.elf out.raw
ncd15-ecoff-wrap out.raw xncd15r.bin
```

The tool defaults load/entry to `0x8ED00000` — the KSEG0 cached
view of physical `0x0ED00000` — which matches the real Xncd15r
ECOFF header (check with `mips-elf-objdump -h Xncd15r`). The
monitor loader masks the top 3 bits of `s_paddr` before writing,
so `0x8ED00000` (KSEG0) and `0x0ED00000` (KUSEG) produce the
same physical destination; using the KSEG0 form is just the
canonical convention and keeps kernel-mode cached fetch
explicit. Override with `--load` / `--entry` for anything else.

## Prerequisites

- Host on the same Ethernet segment as the NCD15, reachable from it.
- Serial console on the NCD15's DUART port (9600 8N1 typical) — the
  banner will print there. Without a console you can still verify by
  watching TFTP-server logs for the RRQ.
- Ability to bind UDP/69 on the host (needs root or capability).

## Serve over TFTP

`tftpd.py` is a plain RFC 1350 server (+ options support).

```bash
# bind 0.0.0.0:69 on the host, serve this directory
sudo python3 tftpd.py . --bind 0.0.0.0
```

Port 69 requires root. `--port 6969` works for local testing but the
NCD15's TFTP client only talks to 69.

### Windows

Same script, no Unix-only deps. From an **Administrator** PowerShell
or Command Prompt (admin is required to bind UDP/69):

```powershell
cd xncd15r-mini
python tftpd.py . --bind 0.0.0.0
```

Notes:

- Use Python 3.8+ from python.org or the Microsoft Store. `py
  tftpd.py …` works too.
- If Windows Firewall pops a prompt on first run, allow the script
  on **Private** networks (at minimum) so UDP/69 replies reach the
  NCD15.
- Stop any other TFTP services first — the built-in Windows TFTP
  client is fine to leave enabled, but a running TFTP **server**
  (e.g. SolarWinds, Tftpd64, the RIS/WDS role) will already own
  UDP/69 and the bind will fail with `WinError 10048`.
- Serial console to the NCD15's DUART: use PuTTY, Tera Term, or
  `plink -serial COM3 -sercfg 9600,8,n,1,N`. Settings are 9600 8N1,
  no flow control.
- For the monitor's `BT` command, use your Windows host's LAN IP
  (check with `ipconfig`), not `127.0.0.1`.

Everything after this section (monitor CLI, `BT` recipe, debugging
table) works identically on Windows.

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

Full command table in `../FINDINGS.md` § "Boot Monitor CLI command set".

### One-shot boot (no NVRAM change)

```
> BT xncd15r.bin 192.168.1.50 192.168.1.15
```

The monitor: resolves ARP → sends TFTP RRQ for `xncd15r.bin` to the
host → parses the ECOFF header → copies each loadable section to
its `s_paddr` (the one `.text` section lands at `0x0ED00000`) →
jumps to the aout header's `entry` (same address). Watch the
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
- **Transfer succeeds, "File corrupted CRC error" on serial console** →
  the downloaded file did not pass the ECOFF header check. If you
  built your own image, re-run `ncd15-ecoff-wrap` on the raw
  `objcopy -O binary` output; the monitor does not accept flat
  binaries.
- **Transfer succeeds, no banner** → image landed but crashed. `DM
  0ED00000 40` and compare against the first-16-words below.
- **Banner truncated** → wrong channel: monitor may have initialized
  only A or only B. The image writes to both, so this shouldn't
  happen, but if it does, check which channel your console cable is
  on.

First 16 bytes of `xncd15r.bin` (big-endian) for verification
(ECOFF file header — NOT what lands at `0x0ED00000`):
```
file[0x00]: 01 60 00 03   00 00 00 00   00 00 00 00   00 00 00 00
            ^---magic     ^---nscns=3   ^---timdat    ^---symptr,nsyms
file[0x10]: 00 38 00 07   01 07 02 0b  ...
            ^---opthdr    ^---aout magic + vstamp
file[0x20]: 00 00 00 00   8e d0 00 00   8e d0 00 00  ...
                          ^---entry     ^---text_start (KSEG0)
```

After the ECOFF loader runs, the first 4 words that land at
physical `0x0ED00000` (KSEG0-visible at `0x8ED00000`, the section
data starting at file offset `0xC4`) are:
```
0x8ED00000 (= phys 0x0ED00000):  3c1d0ed3 27bdff00 3c1c0000 0fb4001f
```

## Recovery

Zero persistent side-effects. Power-cycle = fully recovered. NVRAM
only changes if you used `SE`; that's undoable by another `SE`.

## Hardware register reference

DUART MC68681 @ `0xBE880000`, 4-byte register stride:

| Offset | Register | Used here |
|---|---|---|
| 0x06 | SRA (channel A status) | bit 2 = TxRDY |
| 0x0C | THRA (channel A TX hold) | write byte to send |
| 0x26 | SRB (channel B status) | bit 2 = TxRDY |
| 0x2C | THRB (channel B TX hold) | write byte to send |

Monitor has already initialized the DUART (baud, parity, fifo mode)
before jumping to `0x0ED00000`, so the image only has to poll+write.
No init required.
