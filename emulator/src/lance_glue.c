/* Glue between our bus and the Am79C90 LANCE engine (from
 * 3com68k/emulator, BSD-3-Clause C port of MAME).
 *
 * The NCD15 wires the LANCE at KSEG1 0xBE482000 = phys 0x0E482000.
 * The chip exposes 2 × 16-bit registers (RDP @ +0, RAP @ +2). The
 * NCD15's monitor addresses them with 16-bit word accesses, and the
 * bus uses a 16-bit stride so we translate `offset / 2` to recover
 * which of the two the CPU wants. */

#include "emu.h"
#include "lance.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct lance_glue {
    lance_t chip;
    bus *b;
} lance_glue;

/* DMA callbacks — 16-bit word in/out of host memory. The LANCE is a
 * bus master that addresses DRAM directly by physical address.
 * bus_read/bus_write expect virtual addresses but pass them through
 * a KSEG-aware mapper; raw physical addresses below 0xA0000000 are
 * treated as KUSEG (identity-mapped on the NCD15). */
static uint16_t lance_dma_in(uint32_t addr, void *ctx) {
    lance_glue *g = (lance_glue*)ctx;
    return (uint16_t)bus_read(g->b, addr, 2);
}
static void lance_dma_out(uint32_t addr, uint16_t data, void *ctx) {
    lance_glue *g = (lance_glue*)ctx;
    bus_write(g->b, addr, data, 2);
}
static void lance_intr(int state, void *ctx) {
    /* We don't emulate interrupts yet — the monitor works by polling
     * the LANCE CSR0 INTR bit rather than taking interrupts, so this
     * is fine. */
    (void)state; (void)ctx;
}

/* --- MMIO shim --- */

static u32 lance_mmio_read(void *ctx, u32 offset, unsigned size) {
    lance_glue *g = (lance_glue*)ctx;
    /* NCD15's LANCE: the 2 regs are at bytes 0 and 2 (16-bit-aligned).
     * 32-bit reads return the register zero-extended. */
    int reg = (offset & 2) ? 1 : 0;
    uint16_t v = lance_regs_r(&g->chip, reg);
    (void)size;
    return v;
}

static void lance_mmio_write(void *ctx, u32 offset, u32 value, unsigned size) {
    lance_glue *g = (lance_glue*)ctx;
    int reg = (offset & 2) ? 1 : 0;
    lance_regs_w(&g->chip, reg, (uint16_t)value);
    (void)size;
}

void *lance_glue_new(bus *b) {
    lance_glue *g = (lance_glue*)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->b = b;
    lance_init(&g->chip, LANCE_AM7990);
    lance_set_callbacks(&g->chip, lance_dma_in, lance_dma_out,
                        lance_intr, NULL /*send_cb*/, g);
    lance_reset(&g->chip);
    return g;
}

u32 lance_glue_read(void *ctx, u32 off, unsigned sz) {
    return lance_mmio_read(ctx, off, sz);
}
void lance_glue_write(void *ctx, u32 off, u32 v, unsigned sz) {
    lance_mmio_write(ctx, off, v, sz);
}
