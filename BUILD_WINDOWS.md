# Building an Xncd15r payload on Windows

Three viable paths, ranked by how much grief they save you. Pick
one and stick with it — don't mix. All three produce a raw
big-endian MIPS-I binary that the monitor can TFTP-boot onto the
NCD15 at load address `0x0ED00000`.

See `BUILD.md` for the canonical Linux recipe and the hard
requirements (flags, linker script, ABI). Everything in this doc is
Windows-specific packaging around that same recipe.

## Option A — WSL2 (recommended)

WSL2 runs a real Linux kernel under Windows and lets you follow
`BUILD.md` verbatim. Build artifacts land in a Linux filesystem
that is fully visible from Windows Explorer, and the resulting
`*.bin` can be served by `tftpd.py` from either side.

### One-time setup

In an **Administrator** PowerShell:

```powershell
wsl --install -d Ubuntu
```

Reboot if asked. On first launch of Ubuntu, pick a username and
password. Then inside the Ubuntu shell:

```bash
sudo apt update
sudo apt install -y build-essential wget xz-utils texinfo \
    libgmp-dev libmpfr-dev libmpc-dev libisl-dev zlib1g-dev flex bison
```

### Build the cross-toolchain

Follow `BUILD.md` § "Rebuilding the toolchain from scratch"
exactly. Paste the block as-is. Expect ~30–60 minutes depending on
your CPU. Install prefix `/opt/cross/mips-elf` works; you'll need
`sudo` on the `make install` lines. Put it on PATH:

```bash
echo 'export PATH=/opt/cross/mips-elf/bin:$PATH' >> ~/.bashrc
source ~/.bashrc
```

Verify:

```bash
mips-elf-gcc --version
mips-elf-gcc -dumpmachine      # → mips-elf
```

### Build the example payload

The repo ships a 280-byte reference payload as prebuilt
`xncd15r-mini/xncd15r.bin`. To rebuild it (or your own variant):

```bash
# clone this repo somewhere under your Linux home, e.g.
git clone https://github.com/fletto2/ncd15-reverse-engineering.git
cd ncd15-reverse-engineering/xncd15r-mini
```

There's no `Makefile` in the public repo, but the build is two
commands with the sources from `BUILD.md`. Save the `start.S` /
`main.c` / `link.ld` shown there, then:

```bash
CFLAGS="-march=mips1 -mabi=32 -EB -G0 -fno-pic -mno-abicalls \
  -nostdlib -nostartfiles -ffreestanding -O2 -Wall"
mips-elf-gcc $CFLAGS -T link.ld -o xncd15r.elf start.S main.c
mips-elf-objcopy -O binary xncd15r.elf xncd15r.raw
ncd15-ecoff-wrap xncd15r.raw xncd15r.bin
```

The `ncd15-ecoff-wrap` step is **mandatory**: the NCD15 boot
monitor is an ECOFF loader and rejects flat binaries — see
`BUILD.md` and `xncd15r-mini/README.md` for the full story. The
tool ships under `xncd15r-mini/ncd15-ecoff-wrap` in this repo and
is installed into `/opt/cross/mips-elf/bin/` by the toolchain
recipe (when building via WSL2 following `BUILD.md`).

First 2 bytes of `xncd15r.bin` should be `01 60` (ECOFF MIPS-BE
magic). The first 4 bytes of the *payload* — at file offset
`0xC4`, where the ECOFF loader copies from — should be
`3C 1D 0E D3` (big-endian `lui $sp, 0x0ED3`).

### Serve over TFTP from Windows

The WSL2 Linux VM has its own IP that the NCD15 can't reach
directly. Easiest path: copy the `.bin` out to Windows and serve
with `tftpd.py` from a native Python on the Windows side.

```bash
# from WSL:
cp xncd15r.bin /mnt/c/Users/<you>/ncd15/xncd15r.bin
```

Then in an **Administrator** PowerShell on Windows:

```powershell
cd C:\Users\<you>\ncd15
python tftpd.py . --bind 0.0.0.0
```

(See `xncd15r-mini/README.md` § "Windows" for the TFTP-server
specifics.)

Alternatively: install `tftpd-hpa` inside WSL and expose port 69 to
the LAN with `netsh interface portproxy`, but the copy-and-serve
approach is strictly simpler.

---

## Option B — MSYS2 (native Windows, no VM)

MSYS2 gives you a Unix-like shell environment on Windows and can
build the same binutils + gcc cross-toolchain with minor tweaks.

### Install

Download and run the MSYS2 installer from https://www.msys2.org/.
Open the **MSYS2 MINGW64** shell (not the plain MSYS shell) and:

```bash
pacman -Syu         # update once, may ask to close the shell
pacman -Syu         # again, finishes the update
pacman -S --needed base-devel mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-gmp mingw-w64-x86_64-mpfr mingw-w64-x86_64-mpc \
    mingw-w64-x86_64-isl wget texinfo
```

### Build the cross-toolchain

Same source tarballs as `BUILD.md`. Unpack under
`/c/ncd15-toolchain` (= `C:\ncd15-toolchain`) and build with prefix
`/mingw64/cross/mips-elf` so the resulting `mips-elf-gcc` ends up
on `PATH` in the MSYS2 shell:

```bash
mkdir -p /c/ncd15-toolchain && cd /c/ncd15-toolchain
wget https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.xz
wget https://ftp.gnu.org/gnu/gcc/gcc-13.2.0/gcc-13.2.0.tar.xz
tar -xf binutils-2.42.tar.xz
tar -xf gcc-13.2.0.tar.xz
( cd gcc-13.2.0 && ./contrib/download_prerequisites )

mkdir build-binutils && cd build-binutils
../binutils-2.42/configure --target=mips-elf \
  --prefix=/mingw64/cross/mips-elf \
  --with-sysroot --disable-nls --disable-werror --disable-multilib
make -j$(nproc) && make install
cd ..

mkdir build-gcc && cd build-gcc
../gcc-13.2.0/configure --target=mips-elf \
  --prefix=/mingw64/cross/mips-elf \
  --enable-languages=c --without-headers --disable-nls --disable-werror \
  --disable-multilib --disable-libssp --disable-libquadmath \
  --disable-threads --disable-shared --disable-libgomp --disable-libatomic \
  --with-newlib --with-gnu-as --with-gnu-ld
make -j$(nproc) all-gcc && make install-gcc
make -j$(nproc) all-target-libgcc && make install-target-libgcc

export PATH=/mingw64/cross/mips-elf/bin:$PATH
echo 'export PATH=/mingw64/cross/mips-elf/bin:$PATH' >> ~/.bashrc
```

### Build the payload

Identical to Option A from here on — the MSYS2 shell has bash,
gcc, make, and the cross tools available:

```bash
cd /c/Users/<you>/ncd15/xncd15r-mini
mips-elf-gcc -march=mips1 -mabi=32 -EB -G0 -fno-pic -mno-abicalls \
    -nostdlib -nostartfiles -ffreestanding -O2 \
    -T link.ld -o xncd15r.elf start.S main.c
mips-elf-objcopy -O binary xncd15r.elf xncd15r.bin
```

Serve with `python tftpd.py . --bind 0.0.0.0` from an admin
PowerShell (see `xncd15r-mini/README.md` § "Windows").

### MSYS2 gotchas

- **Do not** use the `MSYS` shell — its gcc targets Cygwin-like
  Windows, and line-ending handling is different. Always launch
  the **MINGW64** shell.
- `make -j$(nproc)` uses all CPUs; lower it (`-j4`) if the box
  thrashes.
- If the binutils configure fails to find a Windows-native
  `texinfo`, run `pacman -S texinfo` and try again.
- Paths with spaces (`C:\Program Files\...`) break GNU build
  systems. Keep everything under `C:\ncd15-toolchain` or similar.

---

## Option C — Prebuilt MIPS toolchain binaries

If you don't want to build gcc yourself, there are prebuilt
Windows MIPS cross-compilers. None are from this project; use at
your own risk and verify target/ABI/endianness match what the
NCD15 needs (MIPS-I, big-endian, o32).

Common sources (not endorsed, not tested against this repo):

- **MIPS Codescape / Mentor** — historical Imagination/MIPS
  release; check the download archive for a Windows build with
  target `mips-sde-elf` or `mips-elf`. Make sure you can set
  `-march=mips1 -EB`; many Codescape bundles default to
  little-endian MIPS32r2.
- **Sysprogs MIPS/OpenWrt toolchains** — sometimes ship as a
  single ZIP with `mips-elf-gcc.exe` inside. Same caveat —
  verify with `mips-elf-gcc -dumpmachine` and
  `mips-elf-gcc -v -march=mips1 -EB foo.c`.
- **Buildroot or crosstool-NG output** from someone else — only
  trust if source/config is available.

For any prebuilt, the sanity check is:

```cmd
mips-elf-gcc -dumpmachine
mips-elf-gcc -print-multi-lib
mips-elf-gcc -march=mips1 -mabi=32 -EB -nostdlib -nostartfiles ^
    -ffreestanding -c -o test.o -xc -
int f(void){return 0;}
^Z
mips-elf-objdump -d test.o
mips-elf-readelf -h test.o | findstr /I "class data machine"
```

The `readelf` output must show `ELF32`, `2's complement, big endian`,
`MIPS R3000`. Anything else (little-endian, MIPS32r2, N32/N64 ABI)
will not run on the NCD15.

If it checks out, proceed as with Options A/B: use the flags from
`BUILD.md`, the `link.ld` from `BUILD.md`, `objcopy -O binary`.

---

## Which should I pick?

- **Just want it working, have a recent Windows 10/11**: Option A
  (WSL2). The Linux recipe is the well-trodden path and anything
  you read about MIPS bare-metal assumes it.
- **Can't enable WSL2 (locked-down corp box, older Windows,
  virtualization disabled in BIOS)**: Option B (MSYS2). More
  fiddly but entirely native.
- **Already have a MIPS cross-compiler from another project and
  know how to check its flags**: Option C. Save the build time,
  but verify endianness and ISA level before trusting output.

## After the build

Regardless of path, once you have `xncd15r.bin`:

1. Copy it to a directory on the Windows host.
2. Serve with `python tftpd.py . --bind 0.0.0.0` in an Admin
   PowerShell (see `xncd15r-mini/README.md` § "Windows").
3. On the NCD15 monitor prompt:
   ```
   > BT xncd15r.bin <ncd15-ip> <windows-host-ip>
   ```
4. Watch the DUART console for your banner.

## Gotchas that bite harder on Windows

- **Line endings.** `start.S` and `link.ld` saved as CRLF still
  assemble fine with GNU tools, but some editors may sneak a UTF-8
  BOM in front — that breaks as/ld. Save as plain ASCII, no BOM.
- **Antivirus.** Real-time AV routinely quarantines freshly built
  `.elf`/`.bin` files or the cross-gcc itself. If a build fails
  with "file not found" on something you just wrote, check
  Defender/third-party AV quarantine first.
- **Path length.** GNU build systems can blow the 260-char path
  limit deep inside `gcc-13.2.0/build-gcc/mips-elf/libgcc/...`.
  Keep the toolchain build tree short: `C:\t\mips` is safer than
  `C:\Users\yourname\OneDrive\Projects\ncd15\toolchain`.
- **PowerShell vs cmd vs bash.** The `$(nproc)`, `export`, and
  heredoc syntax in `BUILD.md` assumes bash. Use WSL's Ubuntu or
  the MSYS2 MINGW64 shell — do not paste those lines into
  PowerShell.
- **TFTP and Windows Firewall.** Even after the server binds
  UDP/69, the firewall can silently drop return traffic. If the
  NCD15 logs show "TFTP TIMEOUT" but the server sees the RRQ, add
  an inbound rule allowing `python.exe` on UDP for Private
  networks.
