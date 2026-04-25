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

/* Translate a CPU offset within the 0xBE48_0000 window into a LANCE
 * shared-memory byte offset.
 *
 * Each 4-byte CPU word maps to ONE 16-bit LANCE halfword. The CPU
 * accesses the same LANCE bytes through TWO byte-pair lanes:
 *   - offset 0,1 (high halfword in BE)  : write side
 *   - offset 2,3 (low halfword in BE)   : read side
 * Both halves alias to the same lance_shmem byte. Confirmed via
 * sub_0ec1b2b8 / sub_0ec1b30c which OR offset bit 1 (=2) onto the
 * SetupTrpAddr result before reading. */
static int lance_shmem_off(u32 cpu_off) {
    return ((cpu_off >> 2) << 1) | (cpu_off & 1);
}

u32 lance_shmem_mmio_read(void *ctx, u32 off, unsigned size);
void lance_shmem_mmio_write(void *ctx, u32 off, u32 v, unsigned size);

u32 lance_shmem_mmio_read(void *ctx, u32 off, unsigned size) {
    bus *b = (bus*)ctx;
    if (size == 1) {
        int s = lance_shmem_off(off);
        if (s < 0 || s >= (int)NCD15_LANCE_SHMEM_SIZE) return 0xff;
        return b->lance_shmem[s];
    }
    /* Halfword/word: read the LANCE 16-bit value at the aligned addr. */
    int s0 = lance_shmem_off(off & ~3u);
    if (s0 < 0 || s0 + 1 >= (int)NCD15_LANCE_SHMEM_SIZE) return ~0u;
    u32 hw = ((u32)b->lance_shmem[s0] << 8) | b->lance_shmem[s0 + 1];
    if (size == 2) return hw;
    return hw;  /* word access: same value, high half mirrored */
}

void lance_shmem_mmio_write(void *ctx, u32 off, u32 v, unsigned size) {
    bus *b = (bus*)ctx;
    if (size == 1) {
        int s = lance_shmem_off(off);
        if (s >= 0 && s < (int)NCD15_LANCE_SHMEM_SIZE)
            b->lance_shmem[s] = v & 0xff;
        return;
    }
    /* Halfword/word write: the LANCE side latches a 16-bit value. */
    int s0 = lance_shmem_off(off & ~3u);
    if (s0 < 0 || s0 + 1 >= (int)NCD15_LANCE_SHMEM_SIZE) return;
    b->lance_shmem[s0]     = (v >> 8) & 0xff;
    b->lance_shmem[s0 + 1] = v & 0xff;
}

void bus_init(bus *b, u8 *rom_bytes) {
    memset(b, 0, sizeof(*b));
    b->rom = rom_bytes;
    b->dram = (u8*)calloc(NCD15_DRAM_SIZE, 1);
    b->shadow = (u8*)calloc(NCD15_DRAM_SIZE, 1);  /* 4 MiB shadow bank */
    b->vram = (u8*)calloc(NCD15_VRAM_SIZE, 1);
    b->lance_shmem = (u8*)calloc(NCD15_LANCE_SHMEM_SIZE, 1);  /* 128 KB */
    if (!b->dram || !b->shadow || !b->vram || !b->lance_shmem) {
        fprintf(stderr, "bus_init: out of memory\n");
        exit(1);
    }
    /* Shadow RAM: the monitor's reset code at 0xBFC00A7C calls a copy
     * routine that pulls monitor image from a ROM window into DRAM at
     * phys 0x0EC00000 and then `jr`s there. We don't preload; let the
     * monitor do the copy. */
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

    /* ROM: decoded at phys 0x1FC00000 (standard MIPS boot alias).
     * The main-monitor code is also accessible via KUSEG VA
     * 0x0EC00000, but that's backed by SHADOW DRAM: at boot, the
     * hardware (or reset code) copies ROM[0x4000..0x27FFF] into
     * DRAM at phys 0x0EC00000, which lets the monitor execute main
     * code from DRAM (writable, cacheable) while reset vectors stay
     * read-only in the ROM window. We preload this shadow in
     * bus_init. */
    if (pa >= NCD15_ROM_KUSEG && pa < NCD15_ROM_KUSEG + NCD15_ROM_SIZE)
        return ld_be(b->rom + (pa - NCD15_ROM_KUSEG), size);


    /* Shadow bank: phys [0x0EC00000, 0x0F000000) backs the main monitor
     * code at 0x0EC00000 and the stack at 0x0EC28000+. Separate from
     * the main DRAM bank. */
    if (pa >= 0x0EC00000u && pa < 0x0F000000u) {
        /* Tick counter hack: data_0x0EC006A8 is read by 38 functions
         * but written by no code in the disassembly — its incrementer
         * is presumably in an interrupt handler we don't have. Auto-
         * increment on every read so the monitor's delay loops
         * advance. Guard: only active once the caller is shadow-
         * resident (post-boot), so we don't corrupt the early memtest
         * that also reads/writes AEC006A8. */
        /* Input-mode hack: data_0x0EC01440 is a halfword flag the
         * monitor reads (5 sites) to choose between blocking DUART
         * polling read (flag != 0) and ring-queue read (flag == 0).
         * No writers in the dis — also an ISR-maintained global.
         * Return 1 from shadow-resident reads so the monitor uses
         * the polling path, letting stdin work. */
        if (pa == 0x0EC01440u && size == 2 &&
            b->last_pc >= 0x0EC00000u && b->last_pc < 0x0F000000u) {
            return 1;
        }
        /* Tick counter hack: VA 0x0EC00730 is where the boot delay
         * loop at 0x0EC0362C reads its tick from ($s1+0x6a8 with
         * $s1=0x0EC00088 → 0x0EC00730). No code in the disassembly
         * writes this — its incrementer lives in an interrupt
         * handler we don't model. Auto-increment on read from shadow-
         * resident code so delay loops advance. */
        if (pa == 0x0EC00730u && size == 4 &&
            b->last_pc >= 0x0EC00000u && b->last_pc < 0x0F000000u) {
            u8 *p = b->shadow + 0x730;
            u32 v = ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];
            v++;
            p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v;
            return v;
        }
        /* Inject user-configured IP at data_0x0EC000C8 and mask at
         * data_0x0EC000EC. These are runtime slots the monitor reads
         * for DA/BT/PI; the NVRAM-to-runtime copy path for them isn't
         * mapped yet, so override on read if --ip / --mask were given. */
        if (pa == 0x0EC000C8u && size == 4 && b->inject_ip &&
            b->last_pc >= 0x0EC00000u && b->last_pc < 0x0F000000u) {
            return b->inject_ip;
        }
        if (pa == 0x0EC000ECu && size == 4 && b->inject_mask &&
            b->last_pc >= 0x0EC00000u && b->last_pc < 0x0F000000u) {
            return b->inject_mask;
        }
        /* Network-mode flag at data_0x0EC00DC0. Bit 26 (0x04000000)
         * means "Ethernet, not token-ring"; if clear, BT prints
         * "Warning: use 'tr 4' or 'tr 16'" and bails with "Check
         * network connection". The flag has no writer in monitor.dis
         * — it's set by HWINIT code at ROM+0x28000+ (presumably from
         * a board-revision strap). Force Ethernet mode. */
        if (pa == 0x0EC00DC0u && size == 4 &&
            b->last_pc >= 0x0EC00000u && b->last_pc < 0x0F000000u) {
            return 0x04000000u;
        }
        return ld_be(b->shadow + (pa - 0x0EC00000u), size);
    }

    /* Main DRAM: 4 MiB at phys 0, aliased every 4 MiB up to 0x0EC00000. */
    if (pa < 0x0EC00000u)
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

    /* ROM window is read-only. */
    if (pa >= NCD15_ROM_KUSEG && pa < NCD15_ROM_KUSEG + NCD15_ROM_SIZE) return;

    if (pa >= 0x0EC00000u && pa < 0x0F000000u) {
        st_be(b->shadow + (pa - 0x0EC00000u), value, size);
        return;
    }
    if (pa < 0x0EC00000u) {
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
