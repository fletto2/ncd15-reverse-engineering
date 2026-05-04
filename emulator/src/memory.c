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

bus *g_dbg_bus = NULL;
/* Trace every write into the config-slot region 0x0EC000C0..0x0EC00150 so
 * we can map which code populates which slot from NVRAM. Toggled via env
 * NCD15_TRACE_CFG=1. */
static int cfg_trace = -1;
static void cfg_trace_write(u32 pa, u32 value, unsigned size, u32 pc) {
    if (cfg_trace < 0) {
        const char *e = getenv("NCD15_TRACE_CFG");
        cfg_trace = (e && *e == '1') ? 1 : 0;
    }
    if (!cfg_trace) return;
    if (pa < 0x0EC000C0u || pa >= 0x0EC00150u) return;
    /* `pc` is the current instruction. When this fires inside the shared
     * memcpy at sub_0ec049d0, ra is the caller (post-jal+8). */
    extern bus *g_dbg_bus;
    fprintf(stderr, "[CFG W] pa=%08x sz=%u val=%08x pc=%08x ra=%08x\n",
            pa, size, value, pc, g_dbg_bus ? g_dbg_bus->last_ra : 0);
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
    g_dbg_bus = b;
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
            /* Cycle-derived tick. The original auto-increment-per-read
             * advanced too fast for real-network timing — the ARP-reply
             * round trip (~50 ms ≈ 600K cycles at 12 MHz) takes longer
             * than the boot's spin-loop deadline counted in per-read
             * ticks. Pace the tick to roughly match real-HW boot timing
             * (one tick per 50K CPU cycles → ~240 Hz, the ballpark of
             * the missing IRQ handler's update rate). */
            u32 v = (u32)(b->cur_cycles / 50000u);
            u8 *p = b->shadow + 0x730;
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
        if (pa == 0x0EC00104u && size == 4 && b->inject_server &&
            b->last_pc >= 0x0EC00000u && b->last_pc < 0x0F000000u) {
            return b->inject_server;
        }
        if (pa == 0x0EC000CCu && size == 4 && b->inject_gateway &&
            b->last_pc >= 0x0EC00000u && b->last_pc < 0x0F000000u) {
            return b->inject_gateway;
        }
        /* POST-error counter at data_0x0EC0148C. The boot POST in
         * sub_0ec02cac_passed increments this on every failed
         * subtest (memory, keyboard, etc.) via sub_0ec089f0. The
         * network-controller test is gated on this counter being
         * zero — if any earlier subtest "failed", we skip the network
         * section. Our emulator accumulates 5 phantom errors due to
         * various stub MMIO returns, hitting the gate. Force read to
         * zero so the network init runs. */
        if (pa == 0x0EC0148Cu && size == 1 &&
            b->last_pc >= 0x0EC00000u && b->last_pc < 0x0F000000u) {
            return 0;
        }
        /* LANCE register pointers — data_0x0EC008EC / 0x0EC008F0
         * hold the CPU addresses for RDP / RAP. Read by sub_0ec17e78
         * via lw at $s2+0x864/$s2+0x868 with $s2=0x0EC00088, so true
         * VAs are 0x0EC008EC / 0x0EC008F0. Never written by code in
         * the disassembly — should be set by an init path that
         * doesn't run on our emulator. Fake them. */
        if (pa == 0x0EC008ECu && size == 4 &&
            b->last_pc >= 0x0EC00000u && b->last_pc < 0x0F000000u) {
            return 0xBE482006u;   /* RDP */
        }
        if (pa == 0x0EC008F0u && size == 4 &&
            b->last_pc >= 0x0EC00000u && b->last_pc < 0x0F000000u) {
            return 0xBE482004u;   /* RAP */
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
        /* Card-type byte at shadow[0x168]: signals which kind of local
         * boot card is installed. Type 1 = linear flash at KSEG1
         * 0xBF800000. Set to 1 when --flash is given. The boot's
         * sub_0ec0f41c reads this byte to decide between local-flash
         * boot vs. network boot. */
        if (pa == 0x0EC00168u && size == 1 && b->flash &&
            b->last_pc >= 0x0EC00000u && b->last_pc < 0x0F000000u) {
            /* Card-type byte: linear flash = 1. Only respond to byte
             * reads from shadow-resident code, not the boot-time
             * memory test that lw-pattern-checks the same region. */
            return 1;
        }
        /* Boot-priority field at runtime addr 0x0EC00DD4 (dis annotates
         * as data_0x0EC00D4C because the annotator doesn't track $s0).
         * The auto-boot loop in sub_0ec0eb6c reads `lw 0xd4c($s0)` with
         * $s0=0x0EC00088 — actual addr is 0x0EC00DD4 — and per-iteration
         * matches a 4-bit nibble against the attempt counter. Bits 0-3
         * are the local-flash priority. Set to 1 to fire local-flash
         * on the first auto-boot iteration. Gated on shadow-resident
         * PC to skip the boot-time memory test. */
        if (pa == 0x0EC00DD4u && size == 4 && b->flash &&
            b->last_pc >= 0x0EC00000u && b->last_pc < 0x0F000000u) {
            return 0x00000001u;
        }
        return ld_be(b->shadow + (pa - 0x0EC00000u), size);
    }

    /* Linear-flash window at phys 0x1F800000 (KSEG1 0xBF800000). The
     * monitor reads the first ~0x40 bytes for the magic ("Xncd19r"
     * at offset 0x10) and then sectors after that. */
    if (b->flash && pa >= 0x1F800000u && pa < 0x1F800000u + b->flash_size) {
        /* On first read of the flash window, lazily re-blit ECOFF
         * sections into shadow. The monitor zeros shadow[0x100000+]
         * during its boot path, so we can't preload eagerly. */
        if (!b->ecoff_preloaded && b->ecoff_bytes && b->ecoff_size >= 0x140) {
            unsigned nscns = (b->ecoff_bytes[2] << 8) | b->ecoff_bytes[3];
            for (unsigned i = 0; i < nscns; i++) {
                u8 *sh = b->ecoff_bytes + 0x4C + i*40;
                u32 paddr  = ((u32)sh[8]<<24)|((u32)sh[9]<<16)|((u32)sh[10]<<8)|sh[11];
                u32 ssize  = ((u32)sh[16]<<24)|((u32)sh[17]<<16)|((u32)sh[18]<<8)|sh[19];
                u32 scnptr = ((u32)sh[20]<<24)|((u32)sh[21]<<16)|((u32)sh[22]<<8)|sh[23];
                if (scnptr == 0 || ssize == 0) continue;
                if ((size_t)scnptr + ssize > b->ecoff_size) continue;
                u32 phys = paddr & 0x1FFFFFFFu;
                if (phys < 0x0EC00000u || phys + ssize > 0x0F000000u) continue;
                memcpy(b->shadow + (phys - 0x0EC00000u),
                       b->ecoff_bytes + scnptr, ssize);
            }
            /* Install a synthetic exception handler at DRAM[0x80].
             * Real NCD15 boot software (which we don't have a source
             * for) leaves a handler here before transferring control;
             * neither the monitor ROM nor the X-server image installs
             * one. The handler increments the X-server's tick counter
             * at 0x8EEC2D9C (which its WaitForCounter routine spins
             * on), acks the HW IRQ in CP0 Cause, and RFEs back to the
             * interrupted PC. Paired with the periodic IP5 source in
             * mips_step. */
            /* Dispatch by interrupt source. If Cause.IP2 is set
             * (LANCE), jump straight to the X-server's own exception
             * handler at 0x8EE83860 — it has the full register-save
             * prologue, dispatcher table at 0x8EEC2650, and LANCE
             * driver wired in. For other sources (our IP5 timer,
             * primarily) keep the lightweight tick path that just
             * bumps 0x8EEC2D9C and RFEs. */
            static const u32 xncd_handler[] = {
                /* index 0..6: read Cause AND Status, AND together to
                 * get only ENABLED+PENDING interrupts, then check IP2.
                 * Monitor masks IM2 but its LANCE test does set
                 * Cause.IP2; checking Cause alone would wrongly dispatch
                 * us to the X-server handler during monitor boot. */
                0x401a6800u, /* mfc0  k0, c0_cause           */
                0x401b6000u, /* mfc0  k1, c0_sr              */
                0x00000000u,
                0x035bd024u, /* and   k0, k0, k1             */
                0x335b0400u, /* andi  k1, k0, 0x400          */
                0x13600005u, /* beqz  k1, +5 (timer_path)    */
                0x00000000u, /* nop (delay slot)             */
                /* index 7..9: LANCE → jump to X-server handler */
                0x3c1a8ee8u, /* lui   k0, 0x8EE8             */
                0x375a3860u, /* ori   k0, k0, 0x3860         */
                0x03400008u, /* jr    k0                     */
                0x00000000u, /* nop (delay slot)             */
                /* index 9+: timer_path */
                0x3c1b8eecu, /* lui   k1, 0x8EEC             */
                0x977a2d9cu, /* lhu   k0, 0x2D9C(k1)         */
                0x00000000u,
                0x275a0001u, /* addiu k0, k0, 1              */
                0xa77a2d9cu, /* sh    k0, 0x2D9C(k1)         */
                0x401a6800u, /* mfc0  k0, c0_cause           */
                0x00000000u,
                0x3c1bffffu, /* lui   k1, 0xFFFF             */
                0x377b03ffu, /* ori   k1, k1, 0x03FF         */
                0x035bd024u, /* and   k0, k0, k1             */
                0x409a6800u, /* mtc0  k0, c0_cause           */
                0x00000000u,
                0x401a7000u, /* mfc0  k0, c0_epc             */
                0x00000000u,
                0x03400008u, /* jr    k0                     */
                0x42000010u, /* rfe (delay slot)             */
            };
            for (size_t i = 0; i < sizeof xncd_handler / sizeof *xncd_handler; i++) {
                u32 w = xncd_handler[i];
                u8 *p = b->dram + 0x80 + i*4;
                p[0] = (u8)(w >> 24); p[1] = (u8)(w >> 16);
                p[2] = (u8)(w >>  8); p[3] = (u8)w;
            }
            /* Invalidate any stale icache lines covering phys [0x80, 0xD0)
             * so the next fetch picks up the fresh handler bytes. */
            extern struct mips_cpu *g_cpu;
            if (g_cpu)
                icache_flush_all(&g_cpu->icache);
            b->ecoff_preloaded = true;
            if (getenv("NCD15_TRACE_BOOT"))
                fprintf(stderr, "[boot] lazy-preloaded ECOFF sections + IRQ handler\n");
        }
        return ld_be(b->flash + (pa - 0x1F800000u), size);
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

    /* Cache isolation: tracked via CP0 reg 7 bit 13 (R3052-specific).
     * When isolated, stores hit the cache only; suppress them here so
     * the synthetic exception handler at DRAM[0x80] survives the
     * X-server's icache-flush sweep across phys 0..0x4000. */
    if (b->cache_isolated) return;

    /* Trace any write to the first 64 bytes of VRAM (phys 0x0F000000+).
     * The very first row of pixels has a stray-white-line glitch we're
     * tracking down. Gated on NCD15_TRACE_VRAM=1. */
    if (pa >= 0x0F000000u && pa < 0x0F000040u && getenv("NCD15_TRACE_VRAM"))
        fprintf(stderr, "[vram] write pa=0x%08x val=0x%x size=%u from pc=0x%08x cyc=%llu\n",
                pa, value, size, b->last_pc, (unsigned long long)b->cur_cycles);


    if (pa >= 0x0EC00000u && pa < 0x0F000000u) {
        cfg_trace_write(pa, value, size, b->last_pc);
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
