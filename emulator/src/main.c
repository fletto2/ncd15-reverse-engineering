/* NCD15 emulator entry point.
 *
 * Usage: ncd15-emu [--trace] [--max-cycles N] <rom-file>
 *
 * Loads the ROM into the emulated address space at physical
 * 0x0EC00000, resets the CPU, and runs until halt. DUART
 * channel A output streams to stdout, channel B to stderr. */

#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 stub_read(void *ctx, u32 off, unsigned sz) {
    (void)ctx; (void)off; (void)sz;
    /* Return "all ones" so polling loops looking for ready/idle bits
     * in undocumented devices advance past their init waits. When we
     * identify the specific device at an offset, register a dedicated
     * MMIO handler before this catch-all. */
    return 0xffffffffu;
}
static void stub_write(void *ctx, u32 off, u32 v, unsigned sz) {
    (void)ctx; (void)off; (void)v; (void)sz;
}

static void *xmalloc(size_t n) {
    void *p = calloc(1, n);
    if (!p) { perror("calloc"); exit(1); }
    return p;
}

static u8 *load_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    size_t n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = (u8*)xmalloc(n);
    if (fread(buf, 1, n, f) != n) { perror("fread"); exit(1); }
    fclose(f);
    *out_size = n;
    return buf;
}

int main(int argc, char **argv) {
    const char *rom_path = NULL;
    bool trace_bus = false;
    u64 max_cycles = 0;    /* 0 == unbounded */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) trace_bus = true;
        else if (!strcmp(argv[i], "--max-cycles") && i+1 < argc) {
            max_cycles = (u64)strtoull(argv[++i], NULL, 0);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 1;
        } else {
            rom_path = argv[i];
        }
    }
    if (!rom_path) {
        fprintf(stderr, "usage: %s [--trace] [--max-cycles N] <rom.u8>\n", argv[0]);
        return 1;
    }

    size_t rom_size = 0;
    u8 *rom_bytes = load_file(rom_path, &rom_size);
    if (rom_size != NCD15_ROM_SIZE) {
        fprintf(stderr, "warning: ROM is %zu bytes, expected %u — loading as-is\n",
                rom_size, NCD15_ROM_SIZE);
    }

    /* Build bus */
    bus b; bus_init(&b, rom_bytes);
    b.trace = trace_bus;

    /* Attach MMIO devices. Order matters if regions overlap; we want
     * the video-control/cart-ID register to shadow the first 4 bytes
     * of VRAM, so register it BEFORE the fallback VRAM mapping. */
    struct duart *d = duart_new();
    bus_add_mmio(&b, (mmio_region){
        .start  = NCD15_DUART_PHYS,  .length = 0x80,
        .read   = duart_read,  .write = duart_write,
        .ctx    = d, .name = "duart",
    });

    struct vidctl *v = vidctl_new();
    bus_add_mmio(&b, (mmio_region){
        .start  = NCD15_VIDCTL_PHYS, .length = 4,
        .read   = vidctl_read, .write = vidctl_write,
        .ctx    = v, .name = "vidctl",
    });

    struct memctl *mc = memctl_new();
    bus_add_mmio(&b, (mmio_region){
        .start  = NCD15_MEMCTL_PHYS, .length = 0x100,
        .read   = memctl_read, .write = memctl_write,
        .ctx    = mc, .name = "memctl",
    });

    /* Catch-all MMIO for devices we haven't emulated yet. Responds
     * with 0 on read, swallows writes. Covers everything from 0x10000000
     * up through 0x0FFFFFFF (KSEG1 = 0xB0000000..0xBFFFFFFF) — the
     * NCD15's entire I/O window except for the regions explicitly
     * claimed above. */
    bus_add_mmio(&b, (mmio_region){
        .start = 0x10000000u, .length = 0xE0000000u - 0x10000000u,
        .read = stub_read, .write = stub_write,
        .ctx = NULL, .name = "stub",
    });

    /* --- CPU --- */
    mips_cpu cpu;
    mips_reset(&cpu, &b);

    fprintf(stderr, "NCD15 emulator: ROM loaded (%zu bytes), PC=0x%08x\n",
            rom_size, cpu.pc);
    fprintf(stderr, "channel A -> stdout, channel B -> stderr\n");
    if (max_cycles) fprintf(stderr, "max_cycles = %llu\n", (unsigned long long)max_cycles);

    mips_run(&cpu, max_cycles);

    if (cpu.halted) {
        fprintf(stderr, "\n--- CPU halted ---\n");
        mips_dump(&cpu);
    } else {
        fprintf(stderr, "\n--- exhausted max_cycles=%llu ---\n",
                (unsigned long long)cpu.cycles);
        mips_dump(&cpu);
    }
    return cpu.halted ? 1 : 0;
}
