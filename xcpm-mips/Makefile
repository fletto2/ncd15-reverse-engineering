#==============================================================================
# port/ncd15 -- build XCP/M as a bootable NCD15 (MIPS R3052) image.
#
# Reuses the portable C from ../../src; supplies MIPS startup, linker
# script, and a board main. Output xcpm-ncd15.bin is an ECOFF image the
# NCD15 monitor (BT/BL) accepts and the emulator loads via --flash.
#==============================================================================
CROSS    ?= /opt/cross/mips-elf/bin/mips-elf-
CC       := $(CROSS)gcc
OBJCOPY  := $(CROSS)objcopy
OBJDUMP  := $(CROSS)objdump
WRAP     ?= /opt/cross/mips-elf/bin/ncd15-ecoff-wrap

SRCDIR   := src
INCDIR   := include
CCPDIR   := src_ccp

# DUART on the NCD15: SCN2681 at KSEG1 0xBE880000, 4-byte register stride,
# chip on data lines D15..D8 so the register byte is at word+2.
DUART_BASE   := 0xBE880000
DUART_STRIDE := 4
DUART_LANE   := 2

# Memory map (must match ncd15.ld). XCP/M C code expects these as -D.
RAM_BASE_ADDR := 0x0ED00000
RAM_END_ADDR  := 0x0F000000
RAM_TPA_ADDR  := 0x0ED40000
RAM_SIZE_VAL  := 0x00300000
ROM_BASE_VAL  := 0x0ED00000
ROM_SIZE_VAL  := 0x00040000

CFLAGS  := -march=mips1 -mabi=32 -EB -G0 -fno-pic -mno-abicalls \
           -nostdlib -nostartfiles -ffreestanding -nostdinc \
           -fno-builtin -fno-common -fno-stack-protector \
           -Os -Wall -Wextra -Wno-unused-parameter \
           -I$(INCDIR) -Iinclude \
           -DBOARD_NAME=\"ncd15\" \
           -DDUART_BASE=$(DUART_BASE)UL \
           -DDUART_STRIDE=$(DUART_STRIDE) -DDUART_LANE=$(DUART_LANE) \
           -DRAM_BASE=$(RAM_BASE_ADDR) -DRAM_SIZE=$(RAM_SIZE_VAL) \
           -DRAM_TPA_ADDR=$(RAM_TPA_ADDR) -DRAM_END_ADDR=$(RAM_END_ADDR) \
           -DROM_BASE=$(ROM_BASE_VAL) -DROM_SIZE=$(ROM_SIZE_VAL) \
           -DRAMDISK_BYTES=4096

LDFLAGS := -nostdlib -Wl,-T,ncd15.ld -Wl,-Map,xcpm-ncd15.map \
           -Wl,--build-id=none -Wl,--no-warn-rwx-segments

# libgcc for any compiler-emitted helpers (64-bit shifts, etc.)
LIBGCC := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name 2>/dev/null)

# Object set: startup + board main + portable XCP/M core.
PORT_OBJS := start.o main_ncd15.o glue_ncd15.o hello_blob.o ramfat_blob.o
CORE_OBJS := console_mc68681.o bdos.o bios.o fat.o lzss.o ramdisk.o disk_ramfat.o

MKFS_FAT  ?= /sbin/mkfs.fat
MCOPY     ?= mcopy
CCP_OBJS  := ccpmain.o
OBJS := $(PORT_OBJS) $(CORE_OBJS) $(CCP_OBJS)

all: xcpm-ncd15.bin xcpm-ncd15.dis

# CCP: shell, linked with kernel in single-mode. Its `_start` is renamed
# to `ccp_main` so it doesn't collide with the kernel's `_start`.
ccpmain.o: $(CCPDIR)/ccpmain.c
	$(CC) $(CFLAGS) -D_start=ccp_main -c -o $@ $<

start.o: start.S
	$(CC) $(CFLAGS) -c -o $@ $<

glue_ncd15.o: glue_ncd15.c
	$(CC) $(CFLAGS) -c -o $@ $<

main_ncd15.o: main_ncd15.c
	$(CC) $(CFLAGS) -c -o $@ $<

# M4: embed app/hello.bin as a rodata blob. hello.bin is built by a
# sub-make in app/ which links it at TPA base via app/app.ld.
app/hello.bin:
	$(MAKE) -C app hello.bin

app/sysinfo.bin:
	$(MAKE) -C app sysinfo.bin

app/syscall.bin:
	$(MAKE) -C app syscall.bin

app/dir.bin:
	$(MAKE) -C app dir.bin

app/cat.bin:
	$(MAKE) -C app cat.bin

app/write.bin:
	$(MAKE) -C app write.bin

hello_blob.o: hello_blob.S app/hello.bin app/sysinfo.bin
	$(CC) $(CFLAGS) -c -o $@ hello_blob.S

# M5a: build a tiny FAT12 image (256 sectors = 128 KB) and populate it
# with the app binaries via mcopy. disk_ramfat.c serves blk_read out of
# this blob; the CCP / app loader can fat_fopen("FOO.BIN") and load by
# name instead of relying on .incbin'd blobs.
ramfat.img: app/hello.bin app/sysinfo.bin app/syscall.bin app/dir.bin app/cat.bin app/write.bin readme.txt
	rm -f $@
	$(MKFS_FAT) -F 12 -n XCPM -C $@ 128 >/dev/null
	$(MCOPY) -i $@ app/hello.bin   ::HELLO.MIP
	$(MCOPY) -i $@ app/sysinfo.bin ::SYSINFO.MIP
	$(MCOPY) -i $@ app/syscall.bin ::SYSCALL.MIP
	$(MCOPY) -i $@ app/dir.bin     ::DIR.MIP
	$(MCOPY) -i $@ app/cat.bin     ::CAT.MIP
	$(MCOPY) -i $@ app/write.bin   ::WRITE.MIP
	$(MCOPY) -i $@ readme.txt      ::README.TXT
	@printf 'Built ramfat.img: %d bytes\n' $$(stat -c%s $@)

ramfat_blob.o: ramfat_blob.S ramfat.img
	$(CC) $(CFLAGS) -c -o $@ ramfat_blob.S

# Portable core compiled from the shared tree.
%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

xcpm-ncd15.elf: $(OBJS) ncd15.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

xcpm-ncd15.raw: xcpm-ncd15.elf
	$(OBJCOPY) -O binary $< $@

xcpm-ncd15.bin: xcpm-ncd15.raw
	$(WRAP) $< $@
	@printf 'Built %s: %d bytes\n' $@ $$(stat -c%s $@)

xcpm-ncd15.dis: xcpm-ncd15.elf
	$(OBJDUMP) -d $< > $@

clean:
	rm -f *.o xcpm-ncd15.elf xcpm-ncd15.raw xcpm-ncd15.bin \
	      xcpm-ncd15.dis xcpm-ncd15.map ramfat.img
	$(MAKE) -C app clean

.PHONY: all clean
