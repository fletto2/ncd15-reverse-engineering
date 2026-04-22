CROSS   := /opt/cross/mips-elf/bin/mips-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

# Wrapper tool: in-tree first (for fresh clones), fall back to /opt/cross
WRAP    := $(shell test -x ./ncd15-ecoff-wrap && echo ./ncd15-ecoff-wrap \
                   || echo /opt/cross/mips-elf/bin/ncd15-ecoff-wrap)

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

# Stage 1: flat binary from ELF (just the code, no ELF header).
xncd15r.raw: xncd15r.elf
	$(OBJCOPY) -O binary $< $@

# Stage 2: wrap the flat binary in minimal ECOFF for the NCD15 monitor.
# The monitor's BT command rejects raw binaries — it parses ECOFF and
# enforces specific field values. See ../FRAMEBUFFER.md / NEW_INSIGHTS.md.
xncd15r.bin: xncd15r.raw
	$(WRAP) $< $@

xncd15r.dis: xncd15r.elf
	$(OBJDUMP) -d $< > $@

clean:
	rm -f $(OBJS) xncd15r.elf xncd15r.raw xncd15r.bin xncd15r.dis

.PHONY: all clean
