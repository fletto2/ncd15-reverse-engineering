CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O2 -g
SRCS    := src/mips.c src/memory.c src/duart.c src/lance.c src/lance_glue.c src/nvram.c src/fb.c src/main.c
OBJS    := $(SRCS:.c=.o)
TARGET  := ncd15-emu

# SDL2 — optional. If unavailable, build with NCD15_NO_SDL=1.
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
ifeq ($(NCD15_NO_SDL),1)
  CFLAGS += -DNCD15_NO_SDL
  SDL_CFLAGS :=
  SDL_LIBS :=
endif
CFLAGS += $(SDL_CFLAGS)

.PHONY: all clean run trace

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(SDL_LIBS)

%.o: %.c src/emu.h
	$(CC) $(CFLAGS) -c -o $@ $<

ROM ?= ../NCD15-19rBM-V271-splice.u8

run: $(TARGET)
	./$(TARGET) --max-cycles 1000000 $(ROM)

trace: $(TARGET)
	MIPS_TRACE=1 ./$(TARGET) --trace --max-cycles 2000 $(ROM) 2>&1 | head -200

clean:
	rm -f $(OBJS) $(TARGET)
