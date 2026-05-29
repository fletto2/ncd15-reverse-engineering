#==============================================================================
# app/Makefile -- build flat-binary XCP/M-MIPS user apps.
#
# Each app builds to <name>.bin (objcopy -O binary), linked at the TPA
# base via ../app.ld. The kernel image's hello_blob.S .incbin's hello.bin
# and the kernel-side loader memcpy's it into TPA at runtime.
#==============================================================================
CROSS    ?= /opt/cross/mips-elf/bin/mips-elf-
CC       := $(CROSS)gcc
OBJCOPY  := $(CROSS)objcopy
OBJDUMP  := $(CROSS)objdump

CFLAGS   := -march=mips1 -mabi=32 -EB -G0 -fno-pic -mno-abicalls \
            -nostdlib -nostartfiles -ffreestanding \
            -fno-builtin -fno-common -fno-stack-protector \
            -Os -Wall -Wextra -Wno-unused-parameter

LDFLAGS  := -nostdlib -Wl,-T,app.ld -Wl,--build-id=none \
            -Wl,--no-warn-rwx-segments

all: hello.bin sysinfo.bin syscall.bin

hello.bin: hello.elf
	$(OBJCOPY) -O binary $< $@
	@printf 'Built %s: %d bytes\n' $@ $$(stat -c%s $@)

hello.elf: start.o hello.o app.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.o hello.o

hello.dis: hello.elf
	$(OBJDUMP) -d $< > $@

sysinfo.bin: sysinfo.elf
	$(OBJCOPY) -O binary $< $@
	@printf 'Built %s: %d bytes\n' $@ $$(stat -c%s $@)

sysinfo.elf: start.o sysinfo.o app.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.o sysinfo.o

sysinfo.dis: sysinfo.elf
	$(OBJDUMP) -d $< > $@

syscall.bin: syscall.elf
	$(OBJCOPY) -O binary $< $@
	@printf 'Built %s: %d bytes\n' $@ $$(stat -c%s $@)

syscall.elf: start.o syscall.o app.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.o syscall.o

syscall.dis: syscall.elf
	$(OBJDUMP) -d $< > $@

syscall.o: syscall.c
	$(CC) $(CFLAGS) -c -o $@ $<

start.o: start.S
	$(CC) $(CFLAGS) -c -o $@ $<

hello.o: hello.c
	$(CC) $(CFLAGS) -c -o $@ $<

sysinfo.o: sysinfo.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o *.elf *.bin *.dis

.PHONY: all clean
