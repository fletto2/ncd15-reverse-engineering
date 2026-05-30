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

## M5 polish

- [x] **M5a — RAM-backed FAT for A:.** `mkfs.fat -F 12 -n XCPM -C
      ramfat.img 128` at build time, `mcopy` injects `app/hello.bin` and
      `app/sysinfo.bin` as `HELLO.BIN` / `SYSINFO.BIN`. `ramfat_blob.S`
      embeds the 128 KB image as a `.rodata` blob; `src/disk_ramfat.c`
      serves `blk_read` out of it (blk_write returns -1 by design —
      apps load by name, they don't mutate the filesystem). `fat.c`
      mounts cleanly. `glue_ncd15.c` adds `run_app_from_fat(path)`:
      `fat_fopen` → `fat_fread` into TPA @ 0x0ED40000 → `jalr` with
      `$a0 = bdos_dispatch`. Both apps now load by filename:
        [loader] /HELLO.BIN: 278 bytes -> TPA @ 0x0ED40000
        [loader] /SYSINFO.BIN: 887 bytes -> TPA @ 0x0ED40000
      Known cosmetic: `list_fat_root()` reports `(0 files)` because
      `fat_dirnext` ends earlier than expected after the volume label.
      `fat_fopen` resolves names fine, so it's a dir-iter quirk, not a
      filesystem bug. Polish target.
- [ ] `syscall` ABI at 0x80000080 if we ever want position-independent
      app images (the current ABI requires the linker address to match
      the loader's TPA target — fine for one TPA, not for arbitrary
      mapping).
- [x] **M5c — `libxcpm.h`** for apps: `u8`/`u16`/`u32`/`bdos_fn_t`
      typedefs + `BDOS_*` constants + inline wrappers (`xputc`,
      `xputs`, `xputs_d`, `xputhex8`, `xputdec`, `xgetc`). Header-only
      (no libxcpm.a), so a minimal app is `#include "libxcpm.h"` +
      `int app_main(bdos_fn_t bdos) { ... }`. `hello.c` refactored
      against it as proof; emits `BDOS 100 ident: 0x58504D4B
      (1481657675 decimal)` via `xputhex8` + `xputdec`.
- [x] **dir-iter fix:** `list_fat_root` used `> 0` for the loop
      condition; `fat_dirnext` returns 0 on success, 1 at end-of-dir,
      -1 on error. Now `== 0`. `A: directory:` correctly shows
      `HELLO    MIP  278 bytes` / `SYSINFO  MIP  887 bytes` / `(2 files)`.
- [x] **M5d — syscall ABI at 0x80000080.** start.S installs a
      position-independent exception-handler stub at virtual
      0x80000080 (BEV=0 by the time we run, per CLAUDE.md). Stub saves
      caller-saved regs to `$sp`, calls `bdos_dispatch(v0, a0, a1)`,
      writes the result back to `$v0`, advances EPC by 4, and
      `rfe`/`jr $k0`s. libxcpm.h adds `bdos_sys(func, p1, p2)` — an
      inline `syscall` instruction with $v0/$a0/$a1 set. Apps using
      `bdos_sys` are fully position-independent (no kernel-address
      bake-in). New demo app `syscall.c` proves the path: 262 syscall
      traces from one boot, all correctly delivered:
        [syscall->exc] pc=0ed40014 v0=0c a0=0 a1=0    BDOS 12
        [syscall->exc] pc=0ed40020 v0=64 a0=0 a1=0    BDOS 100
        [syscall->exc] pc=0ed400e8 v0=02 a0=0d a1=0   BDOS 2 '\r'
        ...
      Emulator side (mips.c case 0x0C): if word @ 0x80000080 is
      non-zero, deliver via existing `take_exception(8)` instead of
      the X-server putchar fallback. Old X-server case still fires
      when no kernel handler is installed.
- [x] **M5d — I-cache flush in loaders.** `glue_ncd15.c` adds
      `icache_flush(base, size)` using CP0 reg 7 bit 13 (R3052's
      cache-isolate, NOT R3000's Status.IsC) + 16-byte-stride
      `sw $0` walk + clear. Called at every `jalr`-into-TPA site
      (embedded-hello, embedded-app2, FAT loader, pgmld_run_mips).
      Emulator no-ops the stores during isolation (CLAUDE.md note);
      real HW invalidates cache tags so the CPU re-fetches the new
      bytes instead of executing stale lines.
- [x] **M5e — useful apps.** `DIR.MIP` (BDOS 110), `CAT.MIP`
      (BDOS 111, takes filename via $a1 cmdtail), `WRITE.MIP`
      (BDOS 120, takes "name content" via cmdtail). App ABI
      extended: `int app_main(bdos_fn_t bdos, const char *tail)`
      with $a1 = the CCP-supplied command tail. `glue_ncd15.c`
      loaders updated to pass `cmdtail` (or `""`) as $a1; old
      one-arg apps still link since the extra register is ignored.
      libxcpm.h gains BDOS_LISTROOT / BDOS_CAT / BDOS_GETCWD
      constants. A short `readme.txt` is mcopy'd into the FAT
      image so `CAT README.TXT` shows something useful.
- [x] **M5f — writeable FAT.** `src/disk_ramfat.c` now backs
      `blk_read/blk_write` with a 128 KB BSS shadow at
      `0x0EF80000..0x0EFA0000` (`ncd15.ld` exports `_ramfat_rw`,
      shrinks `_tpa_end` from 0x0EFC0000 to 0x0EF80000 to make
      room; stack still has 256 KB above the shadow). `blk_init`
      copies the rodata "factory" image into the shadow on first
      mount; per-boot state, so a reset reverts to the embedded
      `mkfs.fat` image. Verified end-to-end via the CCP:
        A:\> WRITE T.X HI       -- WRITE.MIP -> BDOS 120
        Wrote 2 bytes to T.X
        A:\> DIR                -- T.X visible in listing
          ... T.X 2 ...
        A:\> CAT T.X            -- CAT.MIP reads it back
        HI

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
