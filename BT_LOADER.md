# `BT` command boot-loader trace — every check the monitor applies

Derived from static analysis of `disasm/monitor.dis` (V2.7.1). Ordered
so each row is a check the monitor applies in sequence; if a row
fails, the loader rejects the image and the CLI prints the associated
message. This doc is the reference to beat — if a future image fails,
check every row before changing anything.

## Where the validator lives

Contrary to the disassembler's function boundaries, the whole ECOFF
validator is **one monolithic function** spanning roughly
`0x0EC10834 – 0x0EC10BC8`, with a single `jr $ra` at `0x0EC10BC8`.
The apparent inner subs (`sub_0ec10888`, `sub_0ec10910`,
`sub_0ec109c0`, `sub_0ec109f8`, `sub_0ec10a68`, `sub_0ec10ac8`) are
reachable both by fall-through from the validator **and** by `jal`
from other contexts (LANCE/DUART frame parsers) — which is why the
annotator labels them as separate fns (they have many call sites).
During `BT`, control falls through all of them sequentially.

## Check sequence

| # | VA | Field / offset read | Must equal | On failure |
|---|----|---------------------|------------|------------|
| 1 | `0x0EC10860` | `file[0x10..0x11]` (f_opthdr) | `0x0038` | → `0x0EC10A84` (err print) |
| 2 | `0x0EC10878` | `file[0x12..0x13]` (f_nscns) | `3..7` (unsigned: `(n-3) < 5`) | → `0x0EC10A84` |
| 3 | `0x0EC1087C` | `aouthdr.entry` (file `0x24..0x27`) | stored to `vtable[0x880]` for later `jalr` | — |
| 4 | `0x0EC108C0–108F8` | `s_paddr[0] & 0x1FFFFFFF` | `> 0x0ECFFFFF` **and** `≤ 0x0EC00130 + 0x0F000000` | → `0x0EC10B50` |
| 5 | loop `0x0EC10928` | `sh[0].s_flags & 0x20` | set (`STYP_TEXT`) | → `0x0EC10B50` |
| 6 | loop `0x0EC10944` | `sh[0].s_scnptr` | `≤ $s2` (file-size upper bound) | → `0x0EC10B50` |
| 7 | loop `0x0EC10968` | `sh[0].s_scnptr` | `≥ 0x4C + nscns*40` (past the headers) | → `0x0EC10B50` |
| 8 | loop `0x0EC1097C` | `sh[i].s_flags & 0x7E0` | any loadable bit set — else skip this section | (skip, not err) |
| 9 | loop `0x0EC10994` | `sh[i].s_paddr & 0x1FFFFFFF` | `> 0x0ECFFFFF` | → `0x0EC10B50` |
| 10 | `0x0EC109F8` | `aouthdr.magic` (file `0x14..0x15`) | `0x0107` | → `0x0EC10A84` |
| 11 | `0x0EC10A24` | `data_0x0EC00C2C & 0x200000` | state bit: selects 'M'-path vs 'X'-path | — |
| 12a | `0x0EC10A34` | *first section data* `[+0x10]` if state bit set | **`'M'` (0x4D)** — else `j 0x0EC12A84` (exits validator) | jumps to unrelated handler |
| 12b | `0x0EC10A4C` | *first section data* `[+0x10]` if state bit clear | **`'X'` (0x58)** | → `0x0EC10A84` |
| 13 | `0x0EC10A5C` | first section `[+0x11]` | **`'n'`** | → `0x0EC10A84` |
| 14 | `0x0EC10A6C` | first section `[+0x12]` | **`'c'`** | → `0x0EC10A84` |
| 15 | `0x0EC10A7C` | first section `[+0x13]` | **`'d'`** | falls into `0x0EC10A84` error if missing |
| 16 | `0x0EC10AA4` | first section `[+0x0F] & 0x01` | if clear → skip rows 17–19 and return success | (skip, not err) |
| 17 | `0x0EC10AB8` | first section `[+0x14]` | **`'1'`** | → `0x0EC10AE0` err |
| 18 | `0x0EC10AC8` | first section `[+0x15]` | **`'9'`** (NOT `'5'`) | → `0x0EC10AE0` err |
| 19 | `0x0EC10AD8` | first section `[+0x16]` | **`'r'`** | → `0x0EC10AE0` err |
| 20 | `0x0EC10B00` | `sh[0].s_nreloc<<16 | s_nlnno` — something stored to `g_var_ba0` | bounds-check — not rejection, just state | — |
| 21 | `0x0EC10B40` | OR `0x0808` into `data_0x0EC00918` (LANCE status) | "file accepted" flag set | — |

## What every signature value means in bytes

The mandatory first-section prologue (16 bytes header + 8 bytes signature):

```
offset 0x00..0x03:  instruction — monitor skips over this (doesn't read it)
offset 0x04..0x07:  instruction — monitor skips
offset 0x08..0x0B:  32-bit word — monitor skips  
offset 0x0C..0x0F:  32-bit word — byte [0x0F] low bit gates version check (row 16)
                    If bit 0 == 0: skip "19r" check; any 3 bytes at +0x14..+0x16 OK.
                    If bit 0 == 1: rows 17-19 enforced — bytes must be "19r".
offset 0x10..0x13:  "Xncd"  (or "Mncd" if state bit 0x200000 of data_0x0EC00C2C is set)
offset 0x14..0x16:  "19r"   (if byte[0x0F] bit 0 set; the real firmware does this)
offset 0x17:        NUL (conventional)
```

The real `Xncd15r` shipped firmware uses byte[0x0F]=`0x03` (bit 0 set)
and signature bytes `"Xncd19r"` despite the model being 15r. The
version embedded in the signature is the **firmware version**, not
the model. Our payload mirrors this exactly.

## The misleading `"File corrupted CRC error"` string

There is no CRC computation in the `BT` path on V2.7.1. The only
CRC-16 table builder in the ROM (at `0x0EC150B0`, poly `0x8408`) has
zero call sites and its stored-result slot `data_0x0EC00B9C` is only
read by an unrelated "LANCE receive CRC statistic" printer.

`g_state_404` (`data_0x0EC01404`) is a POST-stage progress counter,
incremented on successful self-tests (DUART, LANCE, memctl). The
string at `0x0EC013B8` prints when that counter is still zero after
the LANCE state machine runs — i.e. "no self-test or receive progress
happened". The wording is inherited firmware-vendor convention, not
an actual CRC failure.

If you see `"File corrupted CRC error"`, the actual cause is in
**rows 1–19 above** (structural header / signature rejection), not a
checksum mismatch.

## Final byte-level checklist for an acceptable image

In the order the loader touches them:

1. `file[0x00]` = `0x01` (high byte of `f_magic = 0x0160`)
2. `file[0x01]` = `0x60`
3. `file[0x02..0x03]` = `f_nscns`, must be `0x0003..0x0007`
4. `file[0x10..0x11]` = `0x0038` (f_opthdr)
5. `file[0x12..0x13]` = `f_flags`, must be `0x0003..0x0007`
6. `file[0x14..0x15]` = `0x0107` (aout magic)
7. `file[0x24..0x27]` = `aouthdr.entry`, any 32-bit VA — stored and later jumped to
8. First `scnhdr` at `file[0x4C..0x73]`:
   - `s_flags` (offset +0x24) must have bit `0x20` set
   - `s_paddr` (offset +0x08) masked to 0x1FFFFFFF must be `> 0x0ECFFFFF`
   - `s_scnptr` (offset +0x14) must be `≥ 0x4C + nscns*40` (past headers)
9. `file[s_scnptr + 0x10..+0x13]` = **`"Xncd"`** (with 'M' acceptable in place of 'X' under the state bit)
10. If `file[s_scnptr + 0x0F] & 0x01`: `file[s_scnptr + 0x14..+0x16]` = **`"19r"`**

Fail any and the monitor rejects the image.
