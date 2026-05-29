# XCP/M → NCD15 (MIPS R3052) port — progress notes

Route B: recompile the portable C for MIPS R3052 (goal: build *new* apps,
not run legacy `.68K` binaries). The NCD15 is MIPS, not 68K, so the CCP +
apps are rebuilt MIPS-native; the OS core (BDOS/BIOS/FAT/lzss/ramdisk) is
reused unchanged except for 5 small `#ifdef __mips__` guards.

## Build & boot

```sh
cd port/ncd15 && make            # -> xcpm-ncd15.bin (ECOFF the monitor accepts)

# Boot in the NCD15 emulator (jumps straight to the flash image after the
# monitor finishes POST/DUART/cache init — bypasses the monitor's stricter
# boot-method validation):
EMU=~/ext/src/claude/ncd15/ncd15-reverse-engineering/emulator/ncd15-emu
ROM=~/ext/src/claude/ncd15/NCD15-19rBM-V271-splice.u8
$EMU --no-window --boot-flash --nvram nv.bin --flash xcpm-ncd15.bin $ROM
```

## Milestones

- [x] **M1 — boots + console.** start.S (MIPS entry + the monitor's
      `Xncd19r` ECOFF signature/CRC preamble), ncd15.ld (link phys
      0x0ED00000), console retarget (SCN2681 @ 0xBE880000, stride 4,
      lane +2, channel B). Banner + memory map + echo over serial.
- [x] **M2 — OS core runs.** bdos.c/bios.c/fat.c/lzss.c/ramdisk.c compiled
      for MIPS and exercised via direct dispatch:
        BDOS 12 version=0x0022, BDOS 100 ident=0x58504D4B,
        BIOS 18 getseg, BDOS 9 printstr, B: ramdisk r/w round-trip — all OK.
      glue_ncd15.c provides MIPS warmboot + a stub pgmld_finish_and_run.
- [x] **M3 — CCP shell.** `ccpmain.c` (2766 LoC, designed for TRAP #2)
      rebuilt MIPS-native by `#ifdef __mips__`-guarding its `bdos()`/`bios()`
      wrappers to call the dispatchers directly. Linked with the kernel in
      single-mode; `_start` renamed to `ccp_main` via `-D_start=ccp_main`.
      Zero-size embedded `ccp_help_lz` / `ccp_stty_lz` blobs in
      glue_ncd15.c (real compressed blobs land with M4). Verified:
        A:\> ver
        XCP/M-68K v0.1 (compressed CCP)
        BDOS version (BDOS 12) = 0x0022
        XCP/M magic   (BDOS 100) = 0x58504D4B
        A:\> help
        XCP/M-68K commands:
      Char-by-char echo + ESC[K line-clear render correctly. NUL-flood from
      the earlier echo-loop placeholder did not recur (CCP uses BDOS
      console I/O properly).
- [x] **M4 — dynamic app loading (function-pointer ABI).** Apps build
      to flat MIPS binaries linked at TPA base (0x0ED40000) via
      `app/app.ld`. The kernel embeds `app/hello.bin` as a rodata blob
      via `hello_blob.S` (`.incbin`), and `run_embedded_hello()` in
      `glue_ncd15.c` memcpy's the bytes to TPA, casts the base to a
      function pointer, and `jalr`s with `$a0 = bdos_dispatch`. The
      app's `_app_start` (offset 0) trampolines to `app_main(bdos)`
      which calls BDOS 9 through the passed-in pointer and `return 0`s
      back to the loader. Verified:
        M4: dynamic app load demo
        [loader] hello.bin: 275 bytes -> TPA @ 0x0ED40000
        --- hello.bin: dynamically-loaded MIPS app speaking ---
        I was just memcpy'd into TPA at 0x0ED40000 and jumped to.
        I'm calling BDOS via the function pointer you passed in via a0.
        Returning to the kernel loader now.
        [loader] app returned, rc=0
      Workflow for new apps: drop `app/foo.c`, run `make`, kernel
      re-links with the new blob, run. The function-pointer ABI was
      picked over a full MIPS `syscall` handler at 0x80000080 because
      it needs no exception vector setup, no register save/restore
      stub, and no I-cache flush wrapper — the loader is six lines of
      C, and the app sees BDOS as a normal function call.

## M5 polish (deferred — port goal is met)

- RAM-backed FAT block device for A: so apps load by filename rather
  than baked-in blobs (carve DRAM, `mkfs.fat -C -F 16`, `incbin`).
- `syscall` ABI at 0x80000080 if we ever want position-independent
  app images (the current ABI requires the linker address to match
  the loader's TPA target — fine for one TPA, not for arbitrary
  mapping).
- `libxcpm` header for apps (typedefs + BDOS wrapper macros).
- I-cache flush in the loader for real HW (CP0 reg 7 bit 13 +
  16-byte-stride `sw zero` walk).

## Arch-specific changes (shared tree)

- `src/console_mc68681.c`: `DUART_STRIDE`/`DUART_LANE` macros (default 1/0
  for PVS-2; NCD15 passes 4/2). Register N at `BASE + N*STRIDE + LANE`.
- `src/bdos.c`: two warmboot inline-asm blocks + `bdos_init` trap install
  guarded `#if defined(__mips__)` (MIPS uses `la $sp,_stack_top; j warmboot`
  and the syscall ABI instead of TRAP #2).
- `src/bios.c`: `b_setexc` + `bios_init` trap install guarded for MIPS.

## Emulator support

`--boot-flash` (committed to ncd15-reverse-engineering): after the monitor
reaches its boot driver (`sub_0ec0eb6c`, post-POST), redirect PC to the
preloaded flash ECOFF entry and force the lazy section re-blit. Lets us
iterate on guest images without satisfying the monitor's full BL/BT
validator.

## Known issue

NUL-flood after the M2 echo-loop prompt: `uart_getc` blocks correctly
(verified: SRB RXRDY=0, rx queue empty), so the NULs are not from getc
returning. Source TBD — instrument THRB-B (channel-B TX) in M3. Cosmetic;
does not affect the M2 self-test.
