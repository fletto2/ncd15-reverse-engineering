/* NCD15 memory/bus dispatch. Big-endian throughout. */

#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* R3052 virtual → physical (no MMU, no TLB). Just segmentation. */
u32 mips_va_to_pa(u32 va) {
    if (va >= 0xC0000000u) {
        /* KSEG2 — TLB-mapped on a real MIPS, but the NCD15 monitor
         * doesn't use KSEG2. Treat as identity for now. */
        return va;
    }
    if (va >= 0xA0000000u) return va - 0xA0000000u;  /* KSEG1 */
    if (va >= 0x80000000u) return va - 0x80000000u;  /* KSEG0 */
    return va;                                       /* KUSEG */
}

/* Big-endian word read from a byte buffer. */
static u32 ld_be(const u8 *p, unsigned size) {
    switch (size) {
    case 1: return p[0];
    case 2: return ((u32)p[0] << 8) | p[1];
    case 4: return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                   ((u32)p[2] <<  8) |  p[3];
    }
    return 0;
}
static void st_be(u8 *p, u32 v, unsigned size) {
    switch (size) {
    case 1: p[0] = (u8)v; break;
    case 2: p[0] = (u8)(v >> 8); p[1] = (u8)v; break;
    case 4: p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
            p[2] = (u8)(v >>  8); p[3] = (u8)v;         break;
    }
}

void bus_init(bus *b, u8 *rom_bytes) {
    memset(b, 0, sizeof(*b));
    b->rom = rom_bytes;
    b->dram = (u8*)calloc(NCD15_DRAM_SIZE, 1);
    b->vram = (u8*)calloc(NCD15_VRAM_SIZE, 1);
    if (!b->dram || !b->vram) {
        fprintf(stderr, "bus_init: out of memory\n");
        exit(1);
    }
}

void bus_add_mmio(bus *b, mmio_region r) {
    if (b->nregions >= MAX_MMIO_REGIONS) {
        fprintf(stderr, "bus: too many MMIO regions\n");
        exit(1);
    }
    b->regions[b->nregions++] = r;
}

/* Given a physical address, find the MMIO region that covers it, if any. */
static mmio_region *find_mmio(bus *b, u32 pa) {
    for (size_t i = 0; i < b->nregions; i++) {
        mmio_region *r = &b->regions[i];
        if (pa >= r->start && pa < r->start + r->length) return r;
    }
    return NULL;
}

u32 bus_read(bus *b, u32 va, unsigned size) {
    u32 pa = mips_va_to_pa(va);

    /* ROM: the NCD15 hardware decodes the ROM chip at TWO physical
     * address ranges with DIFFERENT OFFSETS into the ROM image —
     * the reset-vector window and the main-monitor window.
     *
     *   phys 0x1FC00000..0x1FC00FFF  →  ROM[0x0000..0x0FFF]   (reset code)
     *   phys 0x0EC00000..0x0EC23FFF  →  ROM[0x4000..0x27FFF]  (main monitor, linked at 0x0EC00000)
     *
     * This is NOT a simple "ROM aliased at two addresses" setup — the
     * main-monitor window skips the first 0x4000 ROM bytes. Without
     * this offset, fetching at VA 0x0EC00000 would read the reset
     * code (wrong), not the main monitor. */
    if (pa >= NCD15_ROM_KUSEG && pa < NCD15_ROM_KUSEG + 0x1000u)
        return ld_be(b->rom + (pa - NCD15_ROM_KUSEG), size);
    if (pa >= 0x0EC00000u && pa < 0x0EC24000u)
        return ld_be(b->rom + (pa - 0x0EC00000u + 0x4000u), size);

    /* DRAM: 4 MiB, aliased. The stack (sp=0x0EC28000) and LANCE DMA
     * and most of the monitor's data live in DRAM. Anything below
     * the main-monitor window that isn't ROM is DRAM. Also the
     * 0x0ED00000..0x0ED3FFFF alias used by the X server. */
    if (pa < 0x10000000u)
        return ld_be(b->dram + (pa & (NCD15_DRAM_SIZE - 1)), size);

    /* VRAM window at physical 0x0F000000. First 4 bytes double as
     * the video-cartridge ID register — dispatched via MMIO below. */

    mmio_region *r = find_mmio(b, pa);
    if (r) {
        u32 off = pa - r->start;
        u32 v = r->read(r->ctx, off, size);
        r->read_count++;
        if (b->trace)
            fprintf(stderr, "  R [%s+0x%05x]%d = 0x%x\n", r->name, off, size*8, v);
        return v;
    }

    /* VRAM fallback. (MMIO dispatch may intercept first few bytes.) */
    if (pa >= NCD15_VRAM_PHYS && pa < NCD15_VRAM_PHYS + NCD15_VRAM_SIZE)
        return ld_be(b->vram + (pa - NCD15_VRAM_PHYS), size);

    fprintf(stderr, "bus: unmapped read from VA=0x%08x PA=0x%08x size=%d\n",
            va, pa, size);
    return 0xffffffffu;
}

void bus_write(bus *b, u32 va, u32 value, unsigned size) {
    u32 pa = mips_va_to_pa(va);

    /* ROM is read-only; silently drop writes to ROM windows. */
    if (pa >= NCD15_ROM_KUSEG && pa < NCD15_ROM_KUSEG + 0x1000u) return;
    if (pa >= 0x0EC00000u && pa < 0x0EC24000u) return;

    if (pa < 0x10000000u) {
        st_be(b->dram + (pa & (NCD15_DRAM_SIZE - 1)), value, size);
        return;
    }

    mmio_region *r = find_mmio(b, pa);
    if (r) {
        u32 off = pa - r->start;
        r->write_count++;
        if (b->trace)
            fprintf(stderr, "  W [%s+0x%05x]%d = 0x%x\n", r->name, off, size*8, value);
        r->write(r->ctx, off, value, size);
        return;
    }

    if (pa >= NCD15_VRAM_PHYS && pa < NCD15_VRAM_PHYS + NCD15_VRAM_SIZE) {
        st_be(b->vram + (pa - NCD15_VRAM_PHYS), value, size);
        return;
    }

    fprintf(stderr, "bus: unmapped write VA=0x%08x PA=0x%08x val=0x%x size=%d\n",
            va, pa, value, size);
}
