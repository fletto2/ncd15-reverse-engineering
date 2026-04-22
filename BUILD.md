# Building MIPS-I code for the NCD15

## Toolchain

Prebuilt at `/opt/cross/mips-elf/` (binutils 2.42, gcc 13.2.0,
`--target=mips-elf` bare-metal). Source + build tree preserved in
`~/src/claude/ncd15/ncd15-toolchain/` if rebuild needed.

```bash
export PATH=/opt/cross/mips-elf/bin:$PATH
```

All tools have the `mips-elf-` prefix: `mips-elf-gcc`,
`mips-elf-as`, `mips-elf-ld`, `mips-elf-objcopy`,
`mips-elf-objdump`, `mips-elf-readelf`, etc.

## Rebuilding the toolchain from scratch

```bash
mkdir -p ~/src/claude/ncd15/ncd15-toolchain && cd ~/src/claude/ncd15/ncd15-toolchain
wget https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.xz
wget https://ftp.gnu.org/gnu/gcc/gcc-13.2.0/gcc-13.2.0.tar.xz
tar -xf binutils-2.42.tar.xz && tar -xf gcc-13.2.0.tar.xz
( cd gcc-13.2.0 && ./contrib/download_prerequisites )

# binutils
mkdir build-binutils && cd build-binutils
../binutils-2.42/configure --target=mips-elf --prefix=/opt/cross/mips-elf \
  --with-sysroot --disable-nls --disable-werror --disable-multilib
make -j$(nproc) && make install
cd ..

# gcc (stage 1, bare-metal, no libc)
mkdir build-gcc && cd build-gcc
../gcc-13.2.0/configure --target=mips-elf --prefix=/opt/cross/mips-elf \
  --enable-languages=c --without-headers --disable-nls --disable-werror \
  --disable-multilib --disable-libssp --disable-libquadmath \
  --disable-threads --disable-shared --disable-libgomp --disable-libatomic \
  --with-newlib --with-gnu-as --with-gnu-ld
make -j$(nproc) all-gcc && make install-gcc
make -j$(nproc) all-target-libgcc && make install-target-libgcc
```

## Compile flags (required for NCD15 R3052)

```
-march=mips1       # R3000-class ISA; no MIPS-II instructions
-mabi=32           # classic 32-bit o32 ABI
-EB                # big-endian (R3052 boots big-endian)
-G0                # no small-data area ($gp addressing)
-fno-pic           # no position-independent code
-mno-abicalls      # no PIC/GOT call sequences
-nostdlib          # no startup, no crt, no libc
-nostartfiles
-ffreestanding     # no hosted-env assumptions
```

Also recommended:
```
-Wall -Wextra -O2
-fno-builtin       # disable memcpy/memset autorecognition
-fno-stack-protector
```

## Linker script

Target is Xncd15r's load address `0x0ED00000` (physical = DRAM
alias of 0x0B000000). Minimal `link.ld`:

```ld
OUTPUT_FORMAT("elf32-bigmips")
OUTPUT_ARCH(mips)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x0ED00000, LENGTH = 0x400000 }
SECTIONS {
    .text 0x0ED00000 : { *(.text.start) *(.text*) } > RAM
    .rodata : { *(.rodata*) } > RAM
    .data   : { *(.data*)   } > RAM
    .bss    : { *(.bss*)    } > RAM
}
```

The `.text.start` section gets `_start` placed first so the
entry point lands exactly at `0x0ED00000`. Mark it in asm with:

```asm
.section .text.start, "ax"
.global _start
_start:
    ...
```

## Build + image

```bash
mips-elf-gcc $CFLAGS -T link.ld -o out.elf src.S src.c ...
mips-elf-objcopy -O binary out.elf out.raw     # raw code only, no ELF header
ncd15-ecoff-wrap out.raw out.bin               # ECOFF wrapper (required)
mips-elf-objdump -d out.elf | less              # verify
mips-elf-readelf -h out.elf                     # sanity: ELF32, MIPS R3000, big-endian
```

`out.bin` is what the monitor loads over the wire.

**Why the wrap step is mandatory**: the NCD15 boot monitor's `BT`
command parses an ECOFF header on the downloaded file. A flat
binary fails magic-word validation and the loader refuses to run
it. See `xncd15r-mini/README.md` § "Why the ECOFF wrapper?" for the
specific constraints the monitor enforces. The `ncd15-ecoff-wrap`
tool (installed into `/opt/cross/mips-elf/bin/` by the toolchain
recipe, and also copied into `xncd15r-mini/`) produces the minimum
header that passes those checks. Default load/entry is
`0x0ED00000`; override with `--load` / `--entry` if needed.

Sanity check the wrapped image: the first two bytes must be
`01 60` (ECOFF file magic). The first four bytes of the payload
(at file offset `0xC4`) should be the opcode at `_start`
(e.g. `3C08BE88` for `lui $t0, 0xBE88`), big-endian, no ELF
header.

## Minimal test binary

Verified working example at
`~/src/claude/ncd15/ncd15-toolchain/test/` — writes `'H'` to DUART
channel A THR then spins. Build:

```bash
cd ~/src/claude/ncd15/ncd15-toolchain/test
mips-elf-gcc -march=mips1 -mabi=32 -EB -nostdlib -nostartfiles \
  -ffreestanding -G0 -fno-pic -mno-abicalls \
  -T link.ld -o hello.elf hello.S
mips-elf-objcopy -O binary hello.elf hello.bin
```

## Using the Monitor's vtable as an ABI

The Monitor populates 24 function-pointer slots at
`0x0EC008XX` during init and never overwrites them. Xncd15r and
our code can call them as syscalls. To reference a slot from C:

```c
/* Example: slot 0x0EC008D0 holds the printf-like routine */
typedef int (*monitor_printf_fn)(const char *fmt, ...);
#define mon_printf (*(monitor_printf_fn *)0x0EC008D0)

void main(void) {
    mon_printf("hello from 0x%08x\n", 0xED00000);
}
```

See `FINDINGS.md` for the slot-by-slot mapping (derived from
`fn_vtable_init` at 0x0EC00D80).

## Deployment

1. `mips-elf-objcopy -O binary out.elf out.raw` → flat blob
2. `ncd15-ecoff-wrap out.raw out.bin` → ECOFF image (required)
3. Serve via TFTP/NFS at the bootpath stored in NVRAM
4. Power-cycle the NCD15 — monitor parses the ECOFF, copies the
   `.text` section to `s_paddr` (default `0x0ED00000`), jumps to
   the aout-header entry point
5. Recovery = power cycle (no persistent state altered)

## Known gotchas

- Do **not** use `-mips2` or higher — R3052 lacks branch-likely
  and some R4000+ instructions even though gas will accept them
- Do **not** rely on `$gp` — we compile `-G0` and don't set it up
- Delay slots: write code assuming `-mips1` semantics; gas
  auto-schedules unless you use `.set noreorder`
- Initial `$sp` must be set before any stack use — the monitor
  leaves it wherever it was; set it manually in `_start`
- Cache coherency: R3052 has split I/D caches. After writing
  code into DRAM you may need to flush I-cache before jumping
  (the monitor already handles this for the loaded image entry)
