CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O2 -g
SRCS    := src/mips.c src/memory.c src/duart.c src/main.c
OBJS    := $(SRCS:.c=.o)
TARGET  := ncd15-emu

.PHONY: all clean run trace

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c src/emu.h
	$(CC) $(CFLAGS) -c -o $@ $<

ROM ?= ../NCD15-19rBM-V271-splice.u8

run: $(TARGET)
	./$(TARGET) --max-cycles 1000000 $(ROM)

trace: $(TARGET)
	MIPS_TRACE=1 ./$(TARGET) --trace --max-cycles 2000 $(ROM) 2>&1 | head -200

clean:
	rm -f $(OBJS) $(TARGET)
