CROSS   := /opt/cross/mips-elf/bin/mips-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

CFLAGS  := -march=mips1 -mabi=32 -EB -G0 -fno-pic -mno-abicalls \
           -nostdlib -nostartfiles -ffreestanding \
           -fno-builtin -fno-stack-protector \
           -Wall -Wextra -O2

LDFLAGS := -T link.ld -Wl,--build-id=none

OBJS    := start.o main.o

all: xncd15r.bin xncd15r.dis

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

xncd15r.elf: $(OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

xncd15r.bin: xncd15r.elf
	$(OBJCOPY) -O binary $< $@

xncd15r.dis: xncd15r.elf
	$(OBJDUMP) -d $< > $@

clean:
	rm -f $(OBJS) xncd15r.elf xncd15r.bin xncd15r.dis

.PHONY: all clean
