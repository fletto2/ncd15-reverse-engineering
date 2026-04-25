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
    void (*host_send)(const u8 *buf, int len, void *ctx);
    void *host_send_ctx;
} lance_glue;

/* DMA callbacks — 16-bit word in/out of LANCE shared memory. On the
 * NCD15 the LANCE is wired to a dedicated 128 KB shared-memory chip,
 * NOT to main DRAM. The CPU sees the shmem through a windowed MMIO
 * at 0xBE48_0000 (with 4-byte stride / byte-lane translation); the
 * LANCE chip itself sees a flat byte-addressed 128 KB. So our DMA
 * callbacks take a 24-bit address, mask to 17 bits (the LANCE-side
 * range), and access bus->lance_shmem directly — no KSEG translation
 * involved.
 *
 * Big-endian halfword on the wire. */
static uint16_t lance_dma_in(uint32_t addr, void *ctx) {
    lance_glue *g = (lance_glue*)ctx;
    addr &= NCD15_LANCE_SHMEM_SIZE - 1;
    return ((uint16_t)g->b->lance_shmem[addr] << 8) | g->b->lance_shmem[addr + 1];
}
static void lance_dma_out(uint32_t addr, uint16_t data, void *ctx) {
    lance_glue *g = (lance_glue*)ctx;
    addr &= NCD15_LANCE_SHMEM_SIZE - 1;
    g->b->lance_shmem[addr]     = (data >> 8) & 0xff;
    g->b->lance_shmem[addr + 1] = data & 0xff;
}
static void lance_intr(int state, void *ctx) {
    /* We don't emulate interrupts yet — the monitor works by polling
     * the LANCE CSR0 INTR bit rather than taking interrupts, so this
     * is fine. */
    (void)state; (void)ctx;
}

/* Callback from the LANCE engine when it wants to transmit a frame.
 * Forward to the host-side network backend if wired. */
static void lance_send_frame(const uint8_t *buf, int length, void *ctx) {
    lance_glue *g = (lance_glue*)ctx;
    if (g->host_send) g->host_send(buf, length, g->host_send_ctx);
}

/* --- MMIO shim ---
 *
 * NCD15 wires the LANCE register pair within a 16-byte slot at
 * 0xBE482000-0xBE48200F. The monitor's access pattern (sub_0ec08d20 +
 * sub_0ec08cb0) reads/writes:
 *   offset 4 = RAP (Register Address Port — selects which CSR is
 *              visible on RDP; lance.c index 1)
 *   offset 6 = RDP (Register Data Port; lance.c index 0)
 * INVERTED from what 3com68k uses (where RDP comes first). The
 * mapping below picks the right index based on bit 1 of the offset
 * being clear (RAP) vs set (RDP). Other offsets in the slot (e.g.
 * 0xE) are unmapped — return 0 / drop. */

static int lance_offset_to_reg(u32 offset) {
    /* offset & 6: 0,2 → unused; 4 → RAP (1); 6 → RDP (0) */
    switch (offset & 6) {
    case 4: return 1;   /* RAP */
    case 6: return 0;   /* RDP */
    default: return -1;
    }
}

static u32 lance_mmio_read(void *ctx, u32 offset, unsigned size) {
    lance_glue *g = (lance_glue*)ctx;
    int reg = lance_offset_to_reg(offset);
    (void)size;
    if (reg < 0) return 0;
    return lance_regs_r(&g->chip, reg);
}

static void lance_mmio_write(void *ctx, u32 offset, u32 value, unsigned size) {
    lance_glue *g = (lance_glue*)ctx;
    int reg = lance_offset_to_reg(offset);
    (void)size;
    if (reg < 0) return;
    lance_regs_w(&g->chip, reg, (uint16_t)value);
}

void *lance_glue_new(bus *b) {
    lance_glue *g = (lance_glue*)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->b = b;
    lance_init(&g->chip, LANCE_AM7990);
    lance_set_callbacks(&g->chip, lance_dma_in, lance_dma_out,
                        lance_intr, lance_send_frame, g);
    lance_reset(&g->chip);
    return g;
}

void lance_glue_set_send(void *glue,
                         void (*send)(const u8 *buf, int len, void *ctx),
                         void *ctx) {
    lance_glue *g = (lance_glue*)glue;
    g->host_send = send;
    g->host_send_ctx = ctx;
}

void lance_glue_recv(void *glue, const u8 *buf, int len) {
    lance_glue *g = (lance_glue*)glue;
    lance_recv(&g->chip, buf, len);
}

u32 lance_glue_read(void *ctx, u32 off, unsigned sz) {
    return lance_mmio_read(ctx, off, sz);
}
void lance_glue_write(void *ctx, u32 off, u32 v, unsigned sz) {
    lance_mmio_write(ctx, off, v, sz);
}
