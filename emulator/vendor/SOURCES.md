# Upstream source checkout instructions

The actual files under `gxemul/` and `mame/` are intentionally
gitignored — they're pristine upstream and would balloon this repo.
Re-create them with:

## GXemul 0.6.1 (MIPS + DECstation engine)

```bash
cd emulator/vendor
git clone --depth 1 https://github.com/ryoon/gxemul.git
```

Upstream commit pinned: `61000e3` ("Apply pkgsrc patch"). Size: 6.7 MB.
Files we build against: `src/cpus/cpu_mips*.cc`,
`src/cpus/memory_mips*.cc`, `src/devices/dev_le.cc`,
`src/devices/dev_fb.cc`, `src/machines/machine_pmax.cc`.

Alternate mirrors if ryoon disappears:
- https://github.com/threader/gxemul.git
- https://github.com/5HT/gxemul.git

## MAME device sources (DUART + EEPROM reference)

The user maintains a full MAME checkout at `~/src/claude/mame/`.
For a fresh clone (~5 GB):

```bash
cd emulator/vendor
git clone --depth 1 https://github.com/mamedev/mame.git
```

Files we reference: `src/devices/cpu/mips/mips1.{h,cpp}`,
`src/devices/cpu/mips/mips1dsm.{h,cpp}`,
`src/devices/machine/mc68681.{h,cpp}`,
`src/devices/machine/am79c90.{h,cpp}`,
`src/devices/machine/eepromser.{h,cpp}`,
`src/devices/machine/eeprom.{h,cpp}`,
`src/mame/ncd/ncdmips.cpp`, `src/mame/ncd/ncd68k.cpp`,
`src/mame/ncd/ncd88k.cpp`, `src/mame/ncd/bert_m.{cpp,h}`.
