/* NCD15 framebuffer renderer — SDL2 window for the 1 bpp ECL display.
 *
 * Real hardware: 1024 × 800 @ 70 Hz, monochrome ECL. ~100 KB framebuffer
 * at phys 0x0F000000 (KSEG1 0xAF000000). 1 bpp packed, byte-addressed.
 * This module reads that buffer periodically and blits it to an SDL2
 * window with each bit expanded to a pixel (white = bit set, black = clear,
 * matching the white-on-black ECL phosphor convention).
 *
 * Build: link with `pkg-config --libs sdl2`. Define NCD15_NO_SDL to stub out.
 *
 * Runtime: --no-window suppresses creation. Otherwise window opens at
 * 1024×800. Press Esc or close button to quit. Other keys forwarded to
 * stdin (so the boot monitor's CLI keeps working).
 */
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NCD15_NO_SDL

void fb_init(int enabled) { (void)enabled; }
void fb_tick(bus *b)      { (void)b; }
int  fb_should_quit(void) { return 0; }
int  fb_poll_input(unsigned char *out) { (void)out; return 0; }

#else

#include <SDL2/SDL.h>

#define FB_W   1024
#define FB_H   800
#define FB_BYTES_PER_ROW (FB_W / 8)   /* 128 bytes per row */
#define FB_BYTES (FB_BYTES_PER_ROW * FB_H)

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static int  g_enabled;
static int  g_quit;

/* Pending scancodes/chars from window keyboard, fed to stdin via
 * fb_poll_input. Tiny FIFO. */
static unsigned char g_kbd[64];
static int g_kbd_head, g_kbd_tail;

static void kbd_push(unsigned char c)
{
    int nx = (g_kbd_tail + 1) % sizeof(g_kbd);
    if (nx == g_kbd_head) return;
    g_kbd[g_kbd_tail] = c;
    g_kbd_tail = nx;
}

void fb_init(int enabled)
{
    g_enabled = enabled;
    if (!g_enabled) return;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "fb: SDL_Init: %s — running without window\n", SDL_GetError());
        g_enabled = 0;
        return;
    }
    g_win = SDL_CreateWindow("NCD15 framebuffer (1024×800 1bpp)",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             FB_W, FB_H, SDL_WINDOW_SHOWN);
    if (!g_win) {
        fprintf(stderr, "fb: SDL_CreateWindow: %s\n", SDL_GetError());
        g_enabled = 0;
        return;
    }
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) {
        fprintf(stderr, "fb: SDL_CreateRenderer: %s\n", SDL_GetError());
        g_enabled = 0;
        return;
    }
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
    if (!g_tex) {
        fprintf(stderr, "fb: SDL_CreateTexture: %s\n", SDL_GetError());
        g_enabled = 0;
        return;
    }
    /* Black initial fill */
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderPresent(g_ren);
}

/* Walk the bus's vram buffer, expand 1bpp → ARGB8888 into the texture,
 * blit it. Called periodically from main; not every CPU step (way too
 * frequent). */
void fb_tick(bus *b)
{
    if (!g_enabled || !b->vram) return;

    /* Pump SDL events so the window stays responsive. */
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) g_quit = 1;
        else if (e.type == SDL_KEYDOWN) {
            SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_ESCAPE) {
                /* Esc → close window cleanly. */
                g_quit = 1;
            } else if (k == SDLK_RETURN) {
                kbd_push('\r');
            } else if (k == SDLK_BACKSPACE) {
                kbd_push(0x7F);
            } else if (k >= 0x20 && k < 0x7F) {
                /* Plain ASCII, including shifted chars for now. */
                int c = k;
                if (e.key.keysym.mod & (KMOD_LSHIFT | KMOD_RSHIFT)) {
                    if (c >= 'a' && c <= 'z') c -= 0x20;
                }
                kbd_push((unsigned char)c);
            }
        }
    }

    /* Lock the texture and write expanded pixels. */
    void *pixels;
    int pitch;
    if (SDL_LockTexture(g_tex, NULL, &pixels, &pitch) != 0) return;

    Uint32 *dst = (Uint32 *)pixels;
    const unsigned char *src = b->vram;
    /* Each input byte = 8 horizontal pixels, MSB = leftmost. */
    for (int y = 0; y < FB_H; y++) {
        const unsigned char *row = src + y * FB_BYTES_PER_ROW;
        Uint32 *out = dst + y * (pitch / 4);
        for (int x = 0; x < FB_BYTES_PER_ROW; x++) {
            unsigned char b8 = row[x];
            for (int bit = 7; bit >= 0; bit--) {
                *out++ = (b8 >> bit) & 1 ? 0xFFFFFFFFu : 0xFF000000u;
            }
        }
    }
    SDL_UnlockTexture(g_tex);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

int fb_should_quit(void) { return g_quit; }

int fb_poll_input(unsigned char *out)
{
    if (g_kbd_head == g_kbd_tail) return 0;
    *out = g_kbd[g_kbd_head];
    g_kbd_head = (g_kbd_head + 1) % sizeof(g_kbd);
    return 1;
}

#endif /* NCD15_NO_SDL */
